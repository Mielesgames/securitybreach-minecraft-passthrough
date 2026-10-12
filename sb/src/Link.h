#pragma once

#include <protocol/skycraft_protocol.h>

#include <Windows.h>

#include <cstddef>
#include <cstdint>

class SecurityBreachLink
{
public:
    SecurityBreachLink();
    ~SecurityBreachLink();

    bool create();
    bool valid() const;

    // Start of the shared memory, for the readers that walk a ring themselves.
    std::uint8_t* base() const { return base_; }

    void heartbeat();

    void write_sky_state(
        std::uint32_t flags,
        std::uint32_t world_id,
        std::uint32_t collision_epoch,
        double x,
        double y,
        double z,
        float yaw,
        float pitch,
        std::uint32_t teleport_seq,
        std::uint32_t viewport_w,
        std::uint32_t viewport_h,
        float game_hour
    );

    void push_input(
        std::uint16_t type,
        std::uint16_t code,
        std::int32_t a = 0,
        std::int32_t b = 0,
        std::int32_t c = 0
    );

    bool read_mc_state(skycraft::proto::McState& out) const;

    // Takes the next thing that happened in Minecraft (a hit on one of the
    // game's characters, an explosion). False when there is nothing new.
    bool read_event(skycraft::proto::McEvent& out);

    // Minecraft's crosshair is on one of its own blocks.
    bool minecraft_targets_block() const;

    // Queues one message for Minecraft's collision reader: `header` directly
    // followed by `body`. Returns false if the ring has no room right now.
    bool write_collision(
        std::uint32_t type,
        const void* header,
        std::size_t header_bytes,
        const void* body,
        std::size_t body_bytes
    );

    // Throws away what Minecraft queued for us and we don't use yet: its
    // events (hits, deaths, explosions). The render ring is read by the
    // block renderer; left alone it fills up and Minecraft then waits on it
    // every frame.
    void discard_unused_rings();

    // Minecraft's newest picture (hand, HUD, screens). Only the thread that
    // draws it may call these: acquiring swaps the front slot.
    bool acquire_overlay_frame();
    const skycraft::proto::OverlaySlotHdr* front_header() const;
    const std::uint8_t* front_pixels() const;

private:
    HANDLE mapping_{ nullptr };
    std::uint8_t* base_{ nullptr };
    std::uint32_t sky_state_seq_{ 0 };
    std::uint32_t overlay_front_{ 2 };
};