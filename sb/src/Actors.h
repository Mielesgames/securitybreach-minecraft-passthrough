#pragma once

#include "Link.h"

#include <cstdint>
#include <set>
#include <string>

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

    // The number Minecraft knows this character under, or 0.
    std::uint32_t id_of(RC::Unreal::UObject* character);

    // Calls `visit` for every character near the player.
    void for_each(void (*visit)(RC::Unreal::UObject* character, void* context), void* context);

    // Calls `visit` once for every character that stood in fire or lava
    // since the last call (at most once a second each). `hazard` is a
    // proto::BlockHazard.
    void take_burning(void (*visit)(RC::Unreal::UObject* character, int hazard, void* context), void* context);

    // The blueprint classes of every character seen so far, as asset paths
    // ("/Game/.../Roxy.Roxy_C"). Grows as the player meets them.
    const std::set<std::wstring>& known_classes();

    // A character that isn't there for now (in a bucket): no stand-in for it.
    void ignore(RC::Unreal::UObject* character, bool ignored);

    // Minecraft mode is off: no stand-ins.
    void clear(SecurityBreachLink& link);
}
