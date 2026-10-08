#pragma once

#include <protocol/skycraft_protocol.h>

#include <Windows.h>

#include <cstdint>

class SecurityBreachLink
{
public:
    SecurityBreachLink();
    ~SecurityBreachLink();

    bool create();
    bool valid() const;

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

private:
    HANDLE mapping_{ nullptr };
    std::uint8_t* base_{ nullptr };
    std::uint32_t sky_state_seq_{ 0 };
};