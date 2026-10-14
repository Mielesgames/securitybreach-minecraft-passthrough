#pragma once

#include "Link.h"

#include <atomic>

// Draws Minecraft's picture (hand, HUD, screens on a transparent background)
// over Security Breach's frame, right before it is presented.
namespace overlay
{
    // Written by the game thread, read by the thread that presents (and the
    // other way around for the viewport).
    struct State
    {
        // Draw the overlay at all (Minecraft mode is on and Minecraft is in its world).
        std::atomic<bool> show{ false };
        // Minecraft is showing its crosshair, which needs its own blend pass.
        std::atomic<bool> crosshair{ false };
        std::atomic<int> gui_scale{ 0 };
        // Size of the game's back buffer, 0 until the first frame.
        std::atomic<int> viewport_w{ 0 };
        std::atomic<int> viewport_h{ 0 };
        // The game's window, null until the first frame.
        std::atomic<HWND> window{ nullptr };
        // Minecraft's mouse cursor while it has a screen open, in back buffer pixels.
        std::atomic<bool> cursor_on{ false };
        std::atomic<int> cursor_x{ 0 };
        std::atomic<int> cursor_y{ 0 };
        // A box in Minecraft's GUI pixels where a picture of the game is shown
        // (Freddy in his mule screen): world::capture_view.
        std::atomic<bool> preview_on{ false };
        std::atomic<int> preview_x{ 0 };
        std::atomic<int> preview_y{ 0 };
        std::atomic<int> preview_w{ 0 };
        std::atomic<int> preview_h{ 0 };
    };

    State& state();

    // Hooks IDXGISwapChain::Present. The link must outlive the hook.
    bool install(SecurityBreachLink& link);
    void uninstall();
}
