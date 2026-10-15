#pragma once

#include "Link.h"

namespace RC::Unreal
{
    class UObject;
}

// Tells Minecraft what Security Breach is shaped like around the player, so
// Minecraft's own physics can walk, jump and slide on it.
//
// Unreal doesn't hand out its collision geometry, so this feels it out: it
// asks the engine "would a pawn be blocked in this box?" for ever smaller
// boxes, down to a quarter of a block, and sends the result as SkyCraft's
// collision regions (triangles for the player, voxels for everything else).
namespace collision
{
    // Call every game tick while Minecraft mode is on. `character` is the
    // player's pawn (left out of the queries), (x, y, z) Minecraft's player
    // position in blocks. Works for at most a few milliseconds per call.
    void update(SecurityBreachLink& link, RC::Unreal::UObject* character, double x, double y, double z);

    // What a pawn-blocking ray from `from` to `to` (Unreal units) hits first:
    // the actor, or null for nothing. `thickness` > 0 sweeps a box of that
    // half size instead, which is easier to hit things with. Other
    // characters count, the player's own doesn't. `component` gets the part
    // of the actor that was hit.
    RC::Unreal::UObject* trace(
        RC::Unreal::UObject* character,
        const double from[3],
        const double to[3],
        double thickness,
        RC::Unreal::UObject** component = nullptr,
        double* impact = nullptr
    );

    // True for a door of the game that stands open (its invisible player
    // blockers linger after it opened; walking through them is fine).
    bool is_open_door(RC::Unreal::UObject* actor);

    // True for any door of the game (left out of Minecraft's collision; the
    // wall check asks the game about it instead).
    bool is_door(RC::Unreal::UObject* actor);

    // What a ray you can see along hits first (the game's visibility
    // channel): finds buttons and other things a pawn can walk through.
    // `thickness` > 0 sweeps a box of that half size, for small things that
    // are hard to point at exactly.
    RC::Unreal::UObject* trace_visible(
        RC::Unreal::UObject* character,
        const double from[3],
        const double to[3],
        double thickness
    );

    // Something changed around this point (Minecraft coordinates, blocks): a
    // door opened or closed. The regions within `radius` are scanned again
    // right away instead of when their turn comes.
    void invalidate(double x, double y, double z, double radius);

    // Regions sent to Minecraft so far, for the log.
    int regions_sent();
}
