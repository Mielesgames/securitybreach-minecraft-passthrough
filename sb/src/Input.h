#pragma once

#include "Link.h"

// Sends keyboard and mouse to Minecraft while Minecraft mode is on, and keeps
// them away from Security Breach. Works by sitting in front of the game
// window's message handler, so everything here runs on the game thread.
namespace input
{
    // Call every game tick. `window` is the game's window (null: not known
    // yet), `screen_open` whether Minecraft shows a screen (inventory, chat):
    // then the mouse moves Minecraft's cursor instead of the camera.
    void update(
        SecurityBreachLink& link,
        HWND window,
        bool minecraft_mode,
        bool screen_open,
        int viewport_w,
        int viewport_h
    );

    // A right click outside a Minecraft screen is held back until the mod
    // has decided whose it is: the game's (a door, a button) or Minecraft's.
    // True while one is waiting.
    bool take_use_click();
    // The waiting right click is Minecraft's after all.
    void forward_use_click();
    // The waiting right click was used on something in the game.
    void consume_use_click();

    // True once for every left click outside a Minecraft screen. Minecraft
    // gets those clicks regardless.
    bool take_attack_click();

    // The left button is down outside a Minecraft screen: Steve keeps hacking.
    bool attack_held();

    // The middle button is down outside a Minecraft screen (creative: grab).
    bool grab_held();

    void uninstall();
}
