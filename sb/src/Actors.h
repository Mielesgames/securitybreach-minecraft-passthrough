#pragma once

#include "Link.h"

#include <cstdint>

namespace RC::Unreal
{
    class UObject;
}

// Makes the game's characters (Freddy, the animatronics, the bots) and
// Minecraft's blocks exist for each other:
//  - every character near the player is listed for Minecraft, which keeps an
//    invisible stand-in entity on its position. That is what steps on
//    pressure plates and trips tripwires.
//  - a character that walks into a solid Minecraft block is pushed back out
//    of it, so an iron door or a wall holds it.
namespace actors
{
    // Call every game tick while Minecraft mode is on. `player` is the
    // player's own pawn, which Minecraft already has.
    void update(SecurityBreachLink& link, RC::Unreal::UObject* player);

    // The character Minecraft knows under this number, or null.
    RC::Unreal::UObject* find(std::uint32_t id);

    // Calls `visit` for every character near the player.
    void for_each(void (*visit)(RC::Unreal::UObject* character, void* context), void* context);

    // Minecraft mode is off: no stand-ins.
    void clear(SecurityBreachLink& link);
}
