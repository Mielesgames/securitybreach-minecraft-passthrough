#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/AActor.hpp>

#include <UE4SS_SDK/Script/CoreUObject/Rotator.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Link.h"

#include <array>
#include <exception>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

static SecurityBreachLink g_link;

// Sign of Unreal Y when converting to Minecraft Z.
//  +1.0: geometrically correct (Unreal is left-handed, Minecraft right-handed),
//        no mirror image. This is what you want when Minecraft blocks get
//        drawn inside Security Breach later.
//  -1.0: the original SkyCraft (Skyrim) convention. Gives a mirror image here.
// Yaw below is derived from this sign so position and direction always agree.
static constexpr double kZSign = 1.0;

// Sweep Gregory against Unreal geometry (walls, props) when Minecraft moves him.
// Off until walking itself is proven: with sweeping on, the engine writes a full
// FHitResult, and a sweep that starts inside the floor is refused (Gregory then
// barely moves).
static constexpr bool kSweepWalls = false;

static double sb_to_mc_x(double x)
{
    return x / skycraft::proto::kUnitsPerBlock;
}

static double sb_to_mc_y(double z)
{
    return z / skycraft::proto::kUnitsPerBlock;
}

static double sb_to_mc_z(double y)
{
    return kZSign * y / skycraft::proto::kUnitsPerBlock;
}

static float sb_to_mc_yaw(float yaw)
{
    // UE yaw 0 = +X = Minecraft east (-90). Turning right in Unreal (+yaw)
    // goes towards +Y, which maps to Minecraft Z with sign kZSign.
    float result = static_cast<float>(kZSign) * yaw - 90.0f;

    while (result > 180.0f)
    {
        result -= 360.0f;
    }

    while (result < -180.0f)
    {
        result += 360.0f;
    }

    return result;
}

static float sb_to_mc_pitch(float pitch)
{
    float result = -pitch;

    while (result > 180.0f)
    {
        result -= 360.0f;
    }

    while (result < -180.0f)
    {
        result += 360.0f;
    }

    return result;
}

static double mc_to_sb_x(double x)
{
    return x * skycraft::proto::kUnitsPerBlock;
}

static double mc_to_sb_y(double y)
{
    return y * skycraft::proto::kUnitsPerBlock;
}

static double mc_to_sb_z(double z)
{
    return kZSign * z * skycraft::proto::kUnitsPerBlock;
}

class SecurityBreachMinecraftPassthrough : public RC::CppUserModBase
{
public:
    SecurityBreachMinecraftPassthrough() : CppUserModBase()
    {
        ModName = STR("SecurityBreachMinecraftPassthrough");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Minecraft passthrough for FNAF: Security Breach");
        ModAuthors = STR("Mielesgames");

        // register_keydown_event(
        //     RC::Input::Key::F6,
        //     [this]()
        //     {
        //         minecraft_mode = !minecraft_mode;

        //         if (minecraft_mode)
        //         {
        //             ++teleport_seq;
        //         }

        //         RC::Output::send<RC::LogLevel::Verbose>(
        //             STR(
        //                 "SBMP Minecraft mode: {}\n"
        //             ),
        //             minecraft_mode ? STR("ON") : STR("OFF")
        //         );
        //     }
        // );

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("SecurityBreachMinecraftPassthrough loaded!\n")
        );
    }

    struct MinecraftKey
    {
        int virtual_key;
        std::uint16_t scancode;
        bool was_down;
    };

    bool gregory_found = false;
    bool minecraft_mode = false;
    bool minecraft_launch_started = false;
    bool f6_was_down = false;
    HANDLE minecraft_job = nullptr;
    std::uint32_t teleport_seq = 1;
    std::uint64_t last_mc_frame = 0;
    RC::Unreal::UObject* move_input_controller = nullptr;
    bool move_input_ignored = false;
    int resync_cooldown = 0;

    // Stops the game's own WASD from moving Gregory while Minecraft drives
    // him. Otherwise Gregory gets moved twice (game input + Minecraft).
    auto set_ignore_move_input(RC::Unreal::UObject* controller, bool ignore) -> void
    {
        auto* function =
            RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr,
                nullptr,
                STR("/Script/Engine.Controller:SetIgnoreMoveInput")
            );

        if (function == nullptr)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("SBMP could not find Controller:SetIgnoreMoveInput\n")
            );
            return;
        }

        struct
        {
            bool bNewMoveInput;
        } params{ ignore };

        controller->ProcessEvent(function, &params);

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP game move input ignored: {}\n"),
            ignore ? STR("YES") : STR("NO")
        );
    }

    std::array<MinecraftKey, 8> minecraft_keys{
        MinecraftKey{ 'W', 26, false },
        MinecraftKey{ 'A', 4, false },
        MinecraftKey{ 'S', 22, false },
        MinecraftKey{ 'D', 7, false },
        MinecraftKey{ VK_SPACE, 44, false },
        MinecraftKey{ VK_LSHIFT, 225, false },
        MinecraftKey{ VK_LCONTROL, 224, false },
        MinecraftKey{ 'E', 8, false }
    };

    auto sync_minecraft_keyboard() -> void
    {
        if (!minecraft_mode)
        {
            bool any_released = false;

            for (auto& key : minecraft_keys)
            {
                if (key.was_down)
                {
                    any_released = true;
                    g_link.push_input(
                        skycraft::proto::kInKey,
                        key.scancode,
                        0
                    );

                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR(
                            "SBMP input: scancode={}, state=UP\n"
                        ),
                        key.scancode
                    );

                    key.was_down = false;
                }
            }

            // Only once, when keys were actually down. Pushing this every
            // frame fills the input ring and later key events get dropped.
            if (any_released)
            {
                g_link.push_input(
                    skycraft::proto::kInReleaseAll,
                    0
                );
            }

            return;
        }

        for (auto& key : minecraft_keys)
        {
            const bool down =
                (::GetAsyncKeyState(key.virtual_key) & 0x8000) != 0;

            if (down == key.was_down)
            {
                continue;
            }

            g_link.push_input(
                skycraft::proto::kInKey,
                key.scancode,
                down ? 1 : 0
            );

            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP input: VK={}, scancode={}, state={}\n"
                ),
                key.virtual_key,
                key.scancode,
                down ? STR("DOWN") : STR("UP")
            );

            key.was_down = down;
        }
    }

    ~SecurityBreachMinecraftPassthrough() override
    {
        if (minecraft_job != nullptr)
        {
            ::CloseHandle(minecraft_job);
            minecraft_job = nullptr;
        }
    }

    auto start_minecraft() -> void
    {
        if (minecraft_launch_started)
        {
            return;
        }

        if (minecraft_job == nullptr)
        {
            minecraft_job = ::CreateJobObjectW(
                nullptr,
                nullptr
            );

            if (minecraft_job == nullptr)
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR(
                        "SBMP failed to create Minecraft job. Windows error: {}\n"
                    ),
                    ::GetLastError()
                );

                return;
            }

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};

            job_info.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

            if (!::SetInformationJobObject(
                minecraft_job,
                JobObjectExtendedLimitInformation,
                &job_info,
                sizeof(job_info)
            ))
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR(
                        "SBMP failed to configure Minecraft job. Windows error: {}\n"
                    ),
                    ::GetLastError()
                );

                ::CloseHandle(minecraft_job);
                minecraft_job = nullptr;

                return;
            }
        }

        // Set the environment variable SBMP_FABRIC_DIR to the fabric folder,
        // so renaming the repo folder doesn't break this.
        std::wstring fabric_dir =
            L"D:\\Github\\miside-minecraft-passthrough\\fabric";

        // Tells the Minecraft side there is no Skyrim collision feed.
        ::SetEnvironmentVariableW(L"SKYCRAFT_PASSTHROUGH", L"1");

        wchar_t env_buffer[MAX_PATH * 2]{};

        const DWORD env_len = ::GetEnvironmentVariableW(
            L"SBMP_FABRIC_DIR",
            env_buffer,
            static_cast<DWORD>(std::size(env_buffer))
        );

        if (env_len > 0 && env_len < std::size(env_buffer))
        {
            fabric_dir.assign(env_buffer, env_len);
        }

        std::wstring command_line =
            L"\"C:\\Windows\\System32\\cmd.exe\" /C call \"" +
            fabric_dir +
            L"\\gradlew.bat\" runClient";

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);

        PROCESS_INFORMATION process_info{};

        const BOOL started = ::CreateProcessW(
            L"C:\\Windows\\System32\\cmd.exe",
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            fabric_dir.c_str(),
            &startup_info,
            &process_info
        );

        if (!started)
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR(
                    "SBMP failed to start Minecraft. Windows error: {}\n"
                ),
                ::GetLastError()
            );

            return;
        }

        if (!::AssignProcessToJobObject(
            minecraft_job,
            process_info.hProcess
        ))
        {
            const DWORD error = ::GetLastError();

            RC::Output::send<RC::LogLevel::Error>(
                STR(
                    "SBMP failed to assign Minecraft to job. Windows error: {}\n"
                ),
                error
            );

            ::TerminateProcess(
                process_info.hProcess,
                1
            );

            ::CloseHandle(process_info.hThread);
            ::CloseHandle(process_info.hProcess);

            return;
        }

        minecraft_launch_started = true;

        ::ResumeThread(process_info.hThread);

        ::CloseHandle(process_info.hThread);
        ::CloseHandle(process_info.hProcess);

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("SBMP started Minecraft automatically.\n")
        );
    }

    auto on_update() -> void override
    {
        try
        {
            update_impl();
        }
        catch (const std::exception& e)
        {
            static int errors = 0;

            if (errors++ < 10)
            {
                const std::string what = e.what();
                const std::wstring what_w(what.begin(), what.end());

                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP exception in on_update: {}\n"),
                    what_w
                );
            }
        }
        catch (...)
        {
            static int errors = 0;

            if (errors++ < 10)
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP unknown exception in on_update\n")
                );
            }
        }
    }

    // Failsafe state: if Minecraft stops rendering frames, give control back.
    std::uint64_t watch_frame = 0;
    ULONGLONG watch_tick = 0;

    auto update_impl() -> void
    {
        static bool update_logged = false;

        if (!update_logged)
        {
            update_logged = true;

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP on_update is running!\n")
            );
        }

        if (!g_link.valid() && !g_link.create())
        {
            return;
        }

        if (!g_link.valid())
        {
            return;
        }

        start_minecraft();

        const bool f6_down =
            (::GetAsyncKeyState(VK_F6) & 0x8000) != 0;

        if (f6_down && !f6_was_down)
        {
            minecraft_mode = !minecraft_mode;

            if (minecraft_mode)
            {
                ++teleport_seq;
            }

            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP Minecraft mode: {}\n"
                ),
                minecraft_mode ? STR("ON") : STR("OFF")
            );
        }

        f6_was_down = f6_down;

        // Emergency off.
        if (minecraft_mode && (::GetAsyncKeyState(VK_F7) & 0x8000) != 0)
        {
            minecraft_mode = false;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP Minecraft mode: OFF (F7)\n")
            );
        }

        // Watchdog: Minecraft that doesn't produce frames can't drive Gregory.
        {
            skycraft::proto::McState watch{};

            if (g_link.read_mc_state(watch))
            {
                const ULONGLONG now = ::GetTickCount64();

                if (watch.frameCounter != watch_frame)
                {
                    watch_frame = watch.frameCounter;
                    watch_tick = now;
                }
                else if (minecraft_mode && now - watch_tick > 3000)
                {
                    minecraft_mode = false;

                    RC::Output::send<RC::LogLevel::Default>(
                        STR("SBMP Minecraft stopped responding, mode OFF\n")
                    );
                }
            }
        }

        sync_minecraft_keyboard();

        std::vector<RC::Unreal::UObject*> controllers;

        RC::Unreal::UObjectGlobals::FindAllOf(
            STR("MainGamePC_C"),
            controllers
        );

        if (controllers.empty())
        {
            return;
        }

        static int controller_log_counter = 0;

        if (controller_log_counter++ % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP MainGamePC_C count: {}\n"),
                controllers.size()
            );
        }

        auto* controller = controllers[0];

        if (controller == nullptr)
        {
            return;
        }

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP MainGamePC_C: {}\n"),
                controller->GetFullName()
            );
        }

        if (controller != move_input_controller || move_input_ignored != minecraft_mode)
        {
            move_input_controller = controller;
            move_input_ignored = minecraft_mode;
            set_ignore_move_input(controller, minecraft_mode);
        }

        const std::uintptr_t controller_address =
            reinterpret_cast<std::uintptr_t>(controller);

        auto* first_person_character =
            *reinterpret_cast<RC::Unreal::UObject**>(
                controller_address + static_cast<std::uintptr_t>(0x6F0)
            );

        if (first_person_character == nullptr)
        {
            return;
        }

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP FirstPersonCharacter: {}\n"),
                first_person_character->GetFullName()
            );
        }

        auto* actor =
            reinterpret_cast<RC::Unreal::AActor*>(
                first_person_character
            );

        auto location = actor->K2_GetActorLocation();

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP FirstPersonCharacter location: X={}, Y={}, Z={}\n"
                ),
                location.X(),
                location.Y(),
                location.Z()
            );
        }

        auto* control_rotation =
            reinterpret_cast<WSDK::FRotator*>(
                controller_address + static_cast<std::uintptr_t>(0x290)
            );

        float pitch = control_rotation->Pitch;

        if (pitch > 180.0f)
        {
            pitch -= 360.0f;
        }

        if (controller_log_counter % 60 == 0)
        {
            RC::Output::send<RC::LogLevel::Verbose>(
                STR(
                    "SBMP view: Pitch={}, Yaw={}, Roll={}\n"
                ),
                pitch,
                control_rotation->Yaw,
                control_rotation->Roll
            );
        }

        if (!gregory_found)
        {
            auto gregory = RC::Unreal::UObjectGlobals::FindFirstOf(
                STR("Gregory_C")
            );

            if (gregory != nullptr)
            {
                gregory_found = true;

                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP Gregory found: {}\n"),
                    gregory->GetFullName()
                );

                if (gregory == first_person_character)
                {
                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR("SBMP FirstPersonCharacter IS Gregory!\n")
                    );
                }
            }
        }

        const double mc_x = sb_to_mc_x(location.X());
        const double mc_y = sb_to_mc_y(location.Z());
        const double mc_z = sb_to_mc_z(location.Y());

        const float mc_yaw =
            sb_to_mc_yaw(control_rotation->Yaw);

        const float mc_pitch =
            sb_to_mc_pitch(pitch);

        g_link.write_sky_state(
            skycraft::proto::kSkyInGame,
            1,
            1,
            mc_x,
            mc_y,
            mc_z,
            mc_yaw,
            mc_pitch,
            teleport_seq,
            1920,
            1080,
            12.0f
        );

        static int mc_log_counter = 0;

        skycraft::proto::McState mc_state{};

        if (g_link.read_mc_state(mc_state))
        {
            if (
                minecraft_mode &&
                (mc_state.flags & skycraft::proto::kMcInWorld) != 0 &&
                mc_state.frameCounter != last_mc_frame
            )
            {
                last_mc_frame = mc_state.frameCounter;

                const double sb_x = mc_to_sb_x(mc_state.x);
                const double sb_y = mc_to_sb_y(mc_state.y);
                const double sb_z = mc_to_sb_z(mc_state.z);

                // Don't yank Gregory to a Minecraft position that is far away.
                // That happens right after F6 (Minecraft hasn't processed the
                // teleport yet) or when Minecraft's player is frozen/stuck.
                const double gap_x = sb_x - location.X();
                const double gap_y = sb_z - location.Y();

                if (gap_x * gap_x + gap_y * gap_y > 350.0 * 350.0)
                {
                    if (resync_cooldown > 0)
                    {
                        --resync_cooldown;
                    }
                    else
                    {
                        ++teleport_seq;
                        resync_cooldown = 60;

                        RC::Output::send<RC::LogLevel::Default>(
                            STR("SBMP Minecraft is {} units away from Gregory, asking for teleport (seq {})\n"),
                            std::sqrt(gap_x * gap_x + gap_y * gap_y),
                            teleport_seq
                        );
                    }
                }
                else
                {
                // Minecraft only decides horizontal movement (X/Y in Unreal).
                // Height (Z) stays Unreal's, so Gregory keeps standing on the
                // real floor. sb_y (Minecraft height) is deliberately unused.
                (void)sb_y;

                RC::Unreal::FVector new_location{
                    static_cast<float>(sb_x),
                    static_cast<float>(sb_z),
                    location.Z()
                };

                // The engine writes a whole FHitResult when sweeping. Give it a big
                // zeroed buffer so it can never write past a smaller local struct.
                alignas(16) std::uint8_t hit_storage[1024]{};
                auto& sweep_hit =
                    *reinterpret_cast<RC::Unreal::FHitResult*>(hit_storage);

                actor->K2_SetActorLocation(
                    new_location,
                    kSweepWalls,
                    sweep_hit,
                    !kSweepWalls
                );

                // If Gregory didn't reach the target he hit something.
                // Snap Minecraft back to him, otherwise Minecraft keeps
                // walking through the wall and the two drift apart.
                const auto after = actor->K2_GetActorLocation();
                const double dx = after.X() - new_location.X();
                const double dy = after.Y() - new_location.Y();

                if (resync_cooldown > 0)
                {
                    --resync_cooldown;
                }

                if (kSweepWalls && dx * dx + dy * dy > 25.0 && resync_cooldown == 0)
                {
                    ++teleport_seq;
                    resync_cooldown = 10;
                }
                }

                // float sb_yaw = 90.0f - mc_state.yaw;
                // float sb_pitch = -mc_state.pitch;

                // while (sb_yaw > 180.0f)
                // {
                //     sb_yaw -= 360.0f;
                // }

                // while (sb_yaw < -180.0f)
                // {
                //     sb_yaw += 360.0f;
                // }

                // while (sb_pitch > 180.0f)
                // {
                //     sb_pitch -= 360.0f;
                // }

                // while (sb_pitch < -180.0f)
                // {
                //     sb_pitch += 360.0f;
                // }

                // control_rotation->Yaw = sb_yaw;
                // control_rotation->Pitch = sb_pitch;
            }

            if (mc_log_counter++ % 60 == 0)
            {
                RC::Output::send<RC::LogLevel::Verbose>(
                    STR(
                        "SBMP MC state: X={}, Y={}, Z={}, Yaw={}, Pitch={}, Flags={}, Frame={}\n"
                    ),
                    mc_state.x,
                    mc_state.y,
                    mc_state.z,
                    mc_state.yaw,
                    mc_state.pitch,
                    mc_state.flags,
                    mc_state.frameCounter
                );
            }
        }
    }

    auto on_unreal_init() -> void override
    {
    }
};

#define SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API __declspec(dllexport)

extern "C"
{
    SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API RC::CppUserModBase* start_mod()
    {
        return new SecurityBreachMinecraftPassthrough();
    }

    SECURITY_BREACH_MINECRAFT_PASSTHROUGH_API void uninstall_mod(
        RC::CppUserModBase* mod
    )
    {
        delete mod;
    }
}