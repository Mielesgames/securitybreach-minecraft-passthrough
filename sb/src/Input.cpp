#include "Input.h"
#include "Overlay.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <windowsx.h>

#include <algorithm>

namespace input
{
    namespace
    {
        SecurityBreachLink* g_link = nullptr;
        HWND g_window = nullptr;
        WNDPROC g_original_proc = nullptr;

        bool g_minecraft_mode = false;
        bool g_screen_open = false;
        int g_viewport_w = 0;
        int g_viewport_h = 0;
        int g_cursor_x = 0;
        int g_cursor_y = 0;

        // The right click that is waiting for a decision (see Input.h).
        bool g_use_pending = false;
        bool g_use_released = false;   // its button went up again while waiting
        bool g_use_forwarded = false;  // Minecraft has the button down
        bool g_attack_pending = false;
        bool g_attack_held = false;
        bool g_grab_held = false;

        // What reached us since the last report, to see how the game reads its mouse.
        int g_raw_moves = 0;
        int g_plain_moves = 0;
        ULONGLONG g_last_raw_tick = 0;
        ULONGLONG g_last_report_tick = 0;

        // Virtual key -> SDL scancode (USB HID usage), which is what Minecraft
        // takes. 0: not sent to Minecraft.
        std::uint16_t to_scancode(WPARAM vk, LPARAM lparam)
        {
            const bool extended = (lparam & (1 << 24)) != 0;
            const UINT hardware_scancode = (lparam >> 16) & 0xFF;

            if (vk >= 'A' && vk <= 'Z')
            {
                return static_cast<std::uint16_t>(4 + (vk - 'A'));
            }

            if (vk >= '1' && vk <= '9')
            {
                return static_cast<std::uint16_t>(30 + (vk - '1'));
            }

            // Not F6/F7 (ours) and F11 (would make Minecraft's own window fullscreen).
            if (vk >= VK_F1 && vk <= VK_F12)
            {
                if (vk == VK_F6 || vk == VK_F7 || vk == VK_F11)
                {
                    return 0;
                }

                return static_cast<std::uint16_t>(58 + (vk - VK_F1));
            }

            switch (vk)
            {
            case '0': return 39;
            case VK_RETURN: return 40;
            case VK_ESCAPE: return 41;
            case VK_BACK: return 42;
            case VK_TAB: return 43;
            case VK_SPACE: return 44;
            case VK_OEM_MINUS: return 45;
            case VK_OEM_PLUS: return 46;
            case VK_OEM_4: return 47;
            case VK_OEM_6: return 48;
            case VK_OEM_5: return 49;
            case VK_OEM_1: return 51;
            case VK_OEM_7: return 52;
            case VK_OEM_3: return 53;
            case VK_OEM_COMMA: return 54;
            case VK_OEM_PERIOD: return 55;
            case VK_OEM_2: return 56;
            case VK_INSERT: return 73;
            case VK_HOME: return 74;
            case VK_PRIOR: return 75;
            case VK_DELETE: return 76;
            case VK_END: return 77;
            case VK_NEXT: return 78;
            case VK_RIGHT: return 79;
            case VK_LEFT: return 80;
            case VK_DOWN: return 81;
            case VK_UP: return 82;
            case VK_CONTROL: return extended ? 228 : 224;
            case VK_SHIFT: return hardware_scancode == 0x36 ? 229 : 225;
            default: return 0;
            }
        }

        void release_all()
        {
            if (g_link != nullptr)
            {
                g_link->push_input(skycraft::proto::kInReleaseAll, 0);
            }
        }

        // Sends the button to Minecraft. Returns whether the game must not
        // see it: a press while the game hasn't captured the mouse (after
        // alt-tabbing) is how it captures it again, and without that capture
        // the camera doesn't turn.
        bool mouse_button(std::uint16_t button, bool down)
        {
            g_link->push_input(skycraft::proto::kInMouseButton, button, down ? 1 : 0);

            return !down || ::GetCapture() == g_window;
        }

        void move_cursor(int dx, int dy)
        {
            g_cursor_x = std::clamp(g_cursor_x + dx, 0, (std::max)(g_viewport_w - 1, 0));
            g_cursor_y = std::clamp(g_cursor_y + dy, 0, (std::max)(g_viewport_h - 1, 0));

            g_link->push_input(skycraft::proto::kInCursor, 0, g_cursor_x, g_cursor_y);

            auto& shown = overlay::state();

            shown.cursor_x.store(g_cursor_x, std::memory_order_relaxed);
            shown.cursor_y.store(g_cursor_y, std::memory_order_relaxed);
        }

        // Returns true when the message is Minecraft's and the game must not see it.
        bool handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
        {
            switch (message)
            {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP:
            {
                const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                const bool repeat = down && (lparam & (1 << 30)) != 0;
                const auto scancode = to_scancode(wparam, lparam);

                if (scancode == 0)
                {
                    return false;
                }

                // Outside a Minecraft screen Escape is the game's pause menu.
                if (!g_screen_open && wparam == VK_ESCAPE)
                {
                    return false;
                }

                // Held keys only repeat where it matters: typing in a screen.
                if (!repeat || g_screen_open)
                {
                    g_link->push_input(skycraft::proto::kInKey, scancode, down ? 1 : 0);
                }

                return true;
            }

            case WM_CHAR:
                if (!g_screen_open)
                {
                    return false;
                }

                g_link->push_input(skycraft::proto::kInText, 0, static_cast<std::int32_t>(wparam));
                return true;

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                g_attack_pending = !g_screen_open;
                g_attack_held = !g_screen_open;
                return mouse_button(1, true);

            case WM_LBUTTONUP:
                g_attack_held = false;
                return mouse_button(1, false);

            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
                g_grab_held = !g_screen_open;
                return mouse_button(2, true);

            case WM_MBUTTONUP:
                g_grab_held = false;
                return mouse_button(2, false);

            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
                if (g_screen_open)
                {
                    return mouse_button(3, true);
                }

                g_use_pending = true;
                g_use_released = false;
                return ::GetCapture() == g_window;

            case WM_RBUTTONUP:
                if (g_use_pending)
                {
                    g_use_released = true;
                }
                else if (g_use_forwarded || g_screen_open)
                {
                    g_use_forwarded = false;
                    mouse_button(3, false);
                }

                return true;

            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
                return mouse_button(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 4 : 5, true);

            case WM_XBUTTONUP:
                return mouse_button(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 4 : 5, false);

            case WM_MOUSEWHEEL:
                g_link->push_input(skycraft::proto::kInScroll, 0, GET_WHEEL_DELTA_WPARAM(wparam));
                return true;

            case WM_MOUSEMOVE:
            {
                ++g_plain_moves;

                if (!g_screen_open)
                {
                    return false;
                }

                // No raw movement coming in: the game isn't capturing the
                // mouse, so the pointer's place in the window is the cursor.
                if (::GetTickCount64() - g_last_raw_tick > 250)
                {
                    RECT client{};

                    if (::GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
                    {
                        const int x = GET_X_LPARAM(lparam) * g_viewport_w / client.right;
                        const int y = GET_Y_LPARAM(lparam) * g_viewport_h / client.bottom;

                        move_cursor(x - g_cursor_x, y - g_cursor_y);
                    }
                }

                // The camera must not turn while Minecraft has a screen open.
                return true;
            }

            case WM_INPUT:
            {
                ++g_raw_moves;
                g_last_raw_tick = ::GetTickCount64();

                if (!g_screen_open)
                {
                    return false;
                }

                // The game reads the mouse as raw movement; a Minecraft screen
                // gets that movement as its cursor instead.
                RAWINPUT raw{};
                UINT size = sizeof(raw);

                if (
                    ::GetRawInputData(
                        reinterpret_cast<HRAWINPUT>(lparam),
                        RID_INPUT,
                        &raw,
                        &size,
                        sizeof(RAWINPUTHEADER)
                    ) != static_cast<UINT>(-1) &&
                    raw.header.dwType == RIM_TYPEMOUSE &&
                    (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                    (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0)
                )
                {
                    move_cursor(raw.data.mouse.lLastX, raw.data.mouse.lLastY);

                    // The game normally keeps the real (hidden) pointer in
                    // the middle of its window. It doesn't see the mouse now,
                    // so do that here, or the pointer wanders off to another
                    // monitor and the next click lands there.
                    RECT client{};

                    if (::GetClientRect(window, &client))
                    {
                        POINT centre{ client.right / 2, client.bottom / 2 };

                        ::ClientToScreen(window, &centre);
                        ::SetCursorPos(centre.x, centre.y);
                    }
                }

                return true;
            }

            default:
                return false;
            }
        }

        LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (g_minecraft_mode && g_link != nullptr)
            {
                // Alt-tabbing away: Minecraft would keep walking on a key
                // whose release it never sees.
                if (message == WM_KILLFOCUS)
                {
                    release_all();
                    g_attack_held = false;
                    g_grab_held = false;
                }
                else if (handle(window, message, wparam, lparam))
                {
                    // Windows needs to clean up after every raw input message.
                    return ::DefWindowProcW(window, message, wparam, lparam);
                }
            }

            return ::CallWindowProcW(g_original_proc, window, message, wparam, lparam);
        }
    }

    void update(
        SecurityBreachLink& link,
        HWND window,
        bool minecraft_mode,
        bool screen_open,
        int viewport_w,
        int viewport_h
    )
    {
        g_link = &link;
        g_viewport_w = viewport_w;
        g_viewport_h = viewport_h;

        if (g_window == nullptr && window != nullptr)
        {
            g_window = window;
            g_original_proc = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc))
            );

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP input: listening to the game window\n")
            );
        }

        if (g_minecraft_mode && !minecraft_mode)
        {
            release_all();

            g_use_pending = false;
            g_use_forwarded = false;
            g_attack_pending = false;
            g_attack_held = false;
            g_grab_held = false;
        }

        // A screen just opened: its cursor starts in the middle.
        if (screen_open && !g_screen_open)
        {
            g_cursor_x = viewport_w / 2;
            g_cursor_y = viewport_h / 2;

            if (minecraft_mode)
            {
                move_cursor(0, 0);
            }
        }

        // Back in Minecraft's hands (after a pause, a loading hitch, alt-tab):
        // the keys that are physically down count again. Without this Steve
        // stops until the key is pressed anew.
        if (!g_minecraft_mode && minecraft_mode && ::GetForegroundWindow() == g_window)
        {
            static constexpr int kHeld[] = { 'W', 'A', 'S', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL };

            for (const int vk : kHeld)
            {
                if ((::GetAsyncKeyState(vk) & 0x8000) != 0)
                {
                    if (const auto scancode = to_scancode(static_cast<WPARAM>(vk), 0))
                    {
                        g_link->push_input(skycraft::proto::kInKey, scancode, 1);
                    }
                }
            }
        }

        g_minecraft_mode = minecraft_mode;
        g_screen_open = minecraft_mode && screen_open;

        const ULONGLONG now = ::GetTickCount64();

        if (minecraft_mode && now - g_last_report_tick > 5000)
        {
            g_last_report_tick = now;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP input: raw moves {}, plain moves {}, game has the mouse: {}, screen: {}\n"),
                g_raw_moves,
                g_plain_moves,
                ::GetCapture() == g_window ? STR("YES") : STR("NO"),
                g_screen_open ? STR("YES") : STR("NO")
            );

            g_raw_moves = 0;
            g_plain_moves = 0;
        }

        overlay::state().cursor_on.store(g_screen_open, std::memory_order_relaxed);
    }

    bool take_use_click()
    {
        return g_use_pending;
    }

    void forward_use_click()
    {
        if (!g_use_pending || g_link == nullptr)
        {
            return;
        }

        g_use_pending = false;
        g_link->push_input(skycraft::proto::kInMouseButton, 3, 1);

        if (g_use_released)
        {
            g_link->push_input(skycraft::proto::kInMouseButton, 3, 0);
        }
        else
        {
            g_use_forwarded = true;
        }
    }

    void consume_use_click()
    {
        g_use_pending = false;
    }

    bool take_attack_click()
    {
        const bool pending = g_attack_pending;

        g_attack_pending = false;

        return pending;
    }

    bool grab_held()
    {
        return g_grab_held && !g_screen_open;
    }

    bool attack_held()
    {
        return g_attack_held && !g_screen_open;
    }

    void uninstall()
    {
        if (g_window == nullptr)
        {
            return;
        }

        // Only put the old handler back if nobody hooked in after us.
        if (::GetWindowLongPtrW(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&window_proc))
        {
            ::SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_proc));
        }

        g_window = nullptr;
    }
}
