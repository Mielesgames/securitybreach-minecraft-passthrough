#include "Link.h"

#include <atomic>
#include <cstring>
#include <thread>

SecurityBreachLink::SecurityBreachLink()
{
}

SecurityBreachLink::~SecurityBreachLink()
{
    if (base_ != nullptr)
    {
        ::UnmapViewOfFile(base_);
        base_ = nullptr;
    }

    if (mapping_ != nullptr)
    {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool SecurityBreachLink::create()
{
    if (base_ != nullptr)
    {
        return true;
    }

    const auto size = skycraft::proto::kMappingBytes;

    mapping_ = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(size >> 32),
        static_cast<DWORD>(size & 0xFFFFFFFF),
        skycraft::proto::kMappingName
    );

    if (mapping_ == nullptr)
    {
        return false;
    }

    base_ = static_cast<std::uint8_t*>(
        ::MapViewOfFile(
            mapping_,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            0
        )
    );

    if (base_ == nullptr)
    {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    std::memset(
        base_ + skycraft::proto::kOffHeader,
        0,
        sizeof(skycraft::proto::Header)
    );

    std::memset(
        base_ + skycraft::proto::kOffSkyState,
        0,
        sizeof(skycraft::proto::SkyState)
    );

    std::memset(
        base_ + skycraft::proto::kOffInputRing,
        0,
        skycraft::proto::kInputRingDataOff
    );

    auto* header = reinterpret_cast<skycraft::proto::Header*>(
        base_ + skycraft::proto::kOffHeader
    );

    header->version = skycraft::proto::kVersion;
    header->skyrimPid = ::GetCurrentProcessId();
    header->skyrimHeartbeatMs = ::GetTickCount64();

    auto* sky_state = reinterpret_cast<skycraft::proto::SkyState*>(
        base_ + skycraft::proto::kOffSkyState
    );

    sky_state->seq = 0;

    header->magic = skycraft::proto::kMagic;

    return true;
}

bool SecurityBreachLink::valid() const
{
    return base_ != nullptr;
}

void SecurityBreachLink::heartbeat()
{
    if (base_ == nullptr)
    {
        return;
    }

    auto* header = reinterpret_cast<skycraft::proto::Header*>(
        base_ + skycraft::proto::kOffHeader
    );

    header->skyrimHeartbeatMs = ::GetTickCount64();
}

void SecurityBreachLink::write_sky_state(
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
)
{
    if (base_ == nullptr)
    {
        return;
    }

    auto* state = reinterpret_cast<skycraft::proto::SkyState*>(
        base_ + skycraft::proto::kOffSkyState
    );

    const auto seq = sky_state_seq_;

    state->seq = seq + 1;

    std::atomic_thread_fence(std::memory_order_release);

    state->flags = flags;
    state->worldId = world_id;
    state->collisionEpoch = collision_epoch;

    state->posX = x;
    state->posY = y;
    state->posZ = z;

    state->yaw = yaw;
    state->pitch = pitch;

    state->teleportSeq = teleport_seq;

    state->viewportW = viewport_w;
    state->viewportH = viewport_h;

    state->gameHour = game_hour;

    std::atomic_thread_fence(std::memory_order_release);

    state->seq = seq + 2;

    sky_state_seq_ = seq + 2;

    heartbeat();
}

void SecurityBreachLink::push_input(
    std::uint16_t type,
    std::uint16_t code,
    std::int32_t a,
    std::int32_t b,
    std::int32_t c
)
{
    if (base_ == nullptr)
    {
        return;
    }

    auto* ring =
        base_ + skycraft::proto::kOffInputRing;

    auto& head_ref =
        *reinterpret_cast<std::uint64_t*>(
            ring + skycraft::proto::kInputRingHeadOff
        );

    auto& tail_ref =
        *reinterpret_cast<std::uint64_t*>(
            ring + skycraft::proto::kInputRingTailOff
        );

    const auto head =
        std::atomic_ref<std::uint64_t>(head_ref)
            .load(std::memory_order_relaxed);

    const auto tail =
        std::atomic_ref<std::uint64_t>(tail_ref)
            .load(std::memory_order_acquire);

    if (
        head - tail >=
        skycraft::proto::kInputRingEntries
    )
    {
        return;
    }

    auto* entry =
        reinterpret_cast<skycraft::proto::InputEvent*>(
            ring + skycraft::proto::kInputRingDataOff
        ) + (
            head &
            (skycraft::proto::kInputRingEntries - 1)
        );

    *entry = {
        type,
        code,
        a,
        b,
        c
    };

    std::atomic_ref<std::uint64_t>(head_ref)
        .store(
            head + 1,
            std::memory_order_release
        );
}

bool SecurityBreachLink::read_mc_state(skycraft::proto::McState& out) const
{
    if (base_ == nullptr)
    {
        return false;
    }

    auto* state = reinterpret_cast<skycraft::proto::McState*>(
        base_ + skycraft::proto::kOffMcState
    );

    std::atomic_ref<std::uint32_t> seq(state->seq);

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const auto seq1 = seq.load(std::memory_order_acquire);

        if (seq1 & 1)
        {
            std::this_thread::yield();
            continue;
        }

        std::memcpy(
            &out,
            state,
            sizeof(skycraft::proto::McState)
        );

        std::atomic_thread_fence(std::memory_order_acquire);

        if (seq.load(std::memory_order_relaxed) == seq1)
        {
            return true;
        }
    }

    return false;
}

bool SecurityBreachLink::acquire_overlay_frame()
{
    if (base_ == nullptr)
    {
        return false;
    }

    auto* control = reinterpret_cast<skycraft::proto::OverlayCtl*>(
        base_ + skycraft::proto::kOffOverlayCtl
    );

    std::atomic_ref<std::uint32_t> state(control->state);

    if ((state.load(std::memory_order_acquire) & skycraft::proto::kOverlayDirty) == 0)
    {
        return false;
    }

    // Hand our old front slot to Minecraft and take the one it just filled.
    const auto old = state.exchange(overlay_front_, std::memory_order_acq_rel);

    overlay_front_ = old & 3;

    return true;
}

const skycraft::proto::OverlaySlotHdr* SecurityBreachLink::front_header() const
{
    return reinterpret_cast<const skycraft::proto::OverlaySlotHdr*>(
        base_ +
        skycraft::proto::kOffOverlaySlotHdr +
        sizeof(skycraft::proto::OverlaySlotHdr) * overlay_front_
    );
}

const std::uint8_t* SecurityBreachLink::front_pixels() const
{
    return
        base_ +
        skycraft::proto::kOffOverlayPixels +
        skycraft::proto::kOverlaySlotBytes * overlay_front_;
}

void SecurityBreachLink::discard_unused_rings()
{
    if (base_ == nullptr)
    {
        return;
    }

    const auto skip_to_head = [this](std::uint64_t ring, std::uint64_t head_off, std::uint64_t tail_off)
    {
        auto& head = *reinterpret_cast<std::uint64_t*>(base_ + ring + head_off);
        auto& tail = *reinterpret_cast<std::uint64_t*>(base_ + ring + tail_off);

        std::atomic_ref<std::uint64_t>(tail).store(
            std::atomic_ref<std::uint64_t>(head).load(std::memory_order_acquire),
            std::memory_order_release
        );
    };

    skip_to_head(
        skycraft::proto::kOffEventRing,
        skycraft::proto::kEventRingHeadOff,
        skycraft::proto::kEventRingTailOff
    );
}

bool SecurityBreachLink::write_collision(
    std::uint32_t type,
    const void* header,
    std::size_t header_bytes,
    const void* body,
    std::size_t body_bytes
)
{
    if (base_ == nullptr)
    {
        return false;
    }

    namespace proto = skycraft::proto;

    auto* ring = base_ + proto::kOffCollisionRing;
    auto& head_ref = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
    auto& tail_ref = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
    auto* data = ring + proto::kColRingDataOff;

    const std::uint64_t payload = header_bytes + body_bytes;
    const std::uint64_t message = (sizeof(proto::ColMsgHeader) + payload + 7) & ~std::uint64_t(7);

    auto head = std::atomic_ref<std::uint64_t>(head_ref).load(std::memory_order_relaxed);
    const auto tail = std::atomic_ref<std::uint64_t>(tail_ref).load(std::memory_order_acquire);

    auto pos = head % proto::kColRingDataBytes;

    // A message never wraps: if it doesn't fit before the end, a pad message
    // sends the reader back to the start.
    const std::uint64_t pad = pos + message > proto::kColRingDataBytes ? proto::kColRingDataBytes - pos : 0;

    if (proto::kColRingDataBytes - (head - tail) < message + pad)
    {
        return false;
    }

    if (pad > 0)
    {
        const proto::ColMsgHeader skip{ proto::kColPad, 0 };

        std::memcpy(data + pos, &skip, sizeof(skip));
        head += pad;
        pos = 0;
    }

    const proto::ColMsgHeader msg{ type, static_cast<std::uint32_t>(payload) };

    std::memcpy(data + pos, &msg, sizeof(msg));

    if (header_bytes > 0)
    {
        std::memcpy(data + pos + sizeof(msg), header, header_bytes);
    }

    if (body_bytes > 0)
    {
        std::memcpy(data + pos + sizeof(msg) + header_bytes, body, body_bytes);
    }

    std::atomic_ref<std::uint64_t>(head_ref).store(head + message, std::memory_order_release);

    return true;
}

bool SecurityBreachLink::minecraft_targets_block() const
{
    if (base_ == nullptr)
    {
        return false;
    }

    const auto* entities = reinterpret_cast<const skycraft::proto::WorldEntities*>(
        base_ + skycraft::proto::kOffWorldEntities
    );

    // A single word that flips with the selection: no need for the seqlock.
    return entities->hasSelection != 0;
}

bool SecurityBreachLink::read_event(skycraft::proto::McEvent& out)
{
    if (base_ == nullptr)
    {
        return false;
    }

    namespace proto = skycraft::proto;

    auto* ring = base_ + proto::kOffEventRing;
    auto& head_ref = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingHeadOff);
    auto& tail_ref = *reinterpret_cast<std::uint64_t*>(ring + proto::kEventRingTailOff);

    const auto head = std::atomic_ref<std::uint64_t>(head_ref).load(std::memory_order_acquire);
    auto tail = std::atomic_ref<std::uint64_t>(tail_ref).load(std::memory_order_relaxed);

    if (tail >= head)
    {
        return false;
    }

    // Minecraft got further ahead than the ring holds: the oldest events are gone.
    if (head - tail > proto::kEventRingEntries)
    {
        tail = head - proto::kEventRingEntries;
    }

    std::memcpy(
        &out,
        ring + proto::kEventRingDataOff + (tail & (proto::kEventRingEntries - 1)) * sizeof(proto::McEvent),
        sizeof(out)
    );

    std::atomic_ref<std::uint64_t>(tail_ref).store(tail + 1, std::memory_order_release);

    return true;
}
