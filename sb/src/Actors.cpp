#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Actors.h"
#include "World.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FHitResult.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace actors
{
    namespace
    {
        namespace proto = skycraft::proto;

        // The list of characters is looked up again this often: finding them means going
        // through the game's objects.
        constexpr ULONGLONG kRefreshMs = 1000;
        // Characters further from the player than this (Unreal units, 48 blocks) are left out.
        constexpr double kRange = 48.0 * proto::kUnitsPerBlock;

        struct Tracked
        {
            RC::Unreal::FWeakObjectPtr object;
            std::uint32_t id = 0;
            float radius = 0.3f;  // blocks
            float height = 1.8f;  // blocks
            char name[24]{};
        };

        std::vector<Tracked> g_tracked;
        std::unordered_map<RC::Unreal::UObject*, std::uint32_t> g_ids;
        std::uint32_t g_next_id = 1;
        std::uint32_t g_table_seq = 0;
        ULONGLONG g_refreshed = 0;
        // Only ever compared, never used: a stale pointer does no harm.
        std::set<RC::Unreal::UObject*> g_ignored;
        int g_last_count = -1;
        std::set<std::wstring> g_known_classes;
        ULONGLONG g_door_report = 0;

        // Who is standing in fire or lava, and when each last got burnt.
        struct Burning
        {
            RC::Unreal::FWeakObjectPtr object;
            int hazard = 0;
        };

        constexpr ULONGLONG kBurnEveryMs = 1000;

        std::vector<Burning> g_burning;
        std::unordered_map<RC::Unreal::UObject*, ULONGLONG> g_burnt;

        float capsule_value(RC::Unreal::UObject* character, const wchar_t* name, float fallback)
        {
            auto* capsule = character->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("CapsuleComponent"));

            if (capsule == nullptr || *capsule == nullptr)
            {
                return fallback;
            }

            auto* value = (*capsule)->GetValuePtrByPropertyNameInChain<float>(name);

            return value != nullptr ? *value : fallback;
        }

        void refresh(RC::Unreal::UObject* player)
        {
            std::vector<RC::Unreal::UObject*> pawns;

            RC::Unreal::UObjectGlobals::FindAllOf(STR("Pawn"), pawns);

            std::unordered_map<RC::Unreal::UObject*, std::uint32_t> ids;

            g_tracked.clear();

            for (auto* pawn : pawns)
            {
                if (pawn == nullptr || pawn == player || g_ignored.count(pawn) != 0 || g_tracked.size() >= proto::kMaxActors)
                {
                    continue;
                }

                Tracked tracked;

                tracked.object = pawn;

                // Blueprint characters only: those are the ones that can be summoned later.
                if (auto path = pawn->GetClassPrivate()->GetPathName(); path.rfind(STR("/Game/"), 0) == 0)
                {
                    g_known_classes.insert(std::move(path));
                }

                // The same character keeps its number, so Minecraft keeps its stand-in.
                const auto known = g_ids.find(pawn);

                tracked.id = known != g_ids.end() ? known->second : g_next_id++;
                ids[pawn] = tracked.id;

                // Bots without a capsule get a body about Steve's size.
                tracked.radius = capsule_value(pawn, STR("CapsuleRadius"), 21.0f) / float(proto::kUnitsPerBlock);
                tracked.height = capsule_value(pawn, STR("CapsuleHalfHeight"), 63.0f) * 2.0f / float(proto::kUnitsPerBlock);

                // "Freddy_C_2147480236" -> "Freddy".
                const auto full = pawn->GetName();
                std::string name;

                for (const auto c : full)
                {
                    name.push_back(c < 128 ? static_cast<char>(c) : '?');
                }

                if (const auto cut = name.find("_C_"); cut != std::string::npos)
                {
                    name.resize(cut);
                }

                std::strncpy(tracked.name, name.c_str(), sizeof(tracked.name) - 1);

                g_tracked.push_back(tracked);
            }

            g_ids = std::move(ids);
        }

        // How far a character standing at (x, z) with its feet at y has to move
        // to be out of the Minecraft blocks it is in. All in blocks.
        // UE's EMovementMode.
        constexpr std::uint8_t kMoveFalling = 3;
        constexpr std::uint8_t kMoveFlying = 5;

        // Characters the mod holds up on Minecraft blocks (Unreal has no
        // floor there, so they would sink into them and be pushed aside).
        std::set<RC::Unreal::UObject*> g_on_blocks;
        // Characters the mod is lowering into a hole dug into the game's floor.
        std::set<RC::Unreal::UObject*> g_in_hole;
        // How far a character drops into a hole each tick (blocks).
        constexpr double kHoleDropBlocks = 0.12;

        void set_movement_mode(RC::Unreal::UObject* character, std::uint8_t mode)
        {
            auto* movement = character->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("CharacterMovement"));

            if (movement == nullptr || *movement == nullptr)
            {
                return;
            }

            auto* function = (*movement)->GetFunctionByNameInChain(STR("SetMovementMode"));

            if (function == nullptr)
            {
                return;
            }

            struct
            {
                std::uint8_t NewMovementMode;
                std::uint8_t NewCustomMode;
            } params{ mode, 0 };

            (*movement)->ProcessEvent(function, &params);
        }

        // The top of the Minecraft blocks this character stands on or can
        // step onto: under any part of its feet, at most a block up (what a
        // mob jumps) and a little down, with room for its body above. False:
        // nothing of Minecraft's carries it.
        bool block_floor(double x, double y, double z, double radius, double height, double& top)
        {
            const int x0 = static_cast<int>(std::floor(x - radius));
            const int x1 = static_cast<int>(std::floor(x + radius));
            const int z0 = static_cast<int>(std::floor(z - radius));
            const int z1 = static_cast<int>(std::floor(z + radius));
            const int low = static_cast<int>(std::floor(y - 0.3));
            const int high = static_cast<int>(std::floor(y + 0.99));

            int found = low - 1;

            for (int bz = z0; bz <= z1; ++bz)
            {
                for (int bx = x0; bx <= x1; ++bx)
                {
                    for (int by = high; by >= low && by > found; --by)
                    {
                        if (world::solid_at(bx, by, bz))
                        {
                            found = by;
                            break;
                        }
                    }
                }
            }

            // Nothing under it, or more than a block up: that is a wall.
            if (found < low || found + 1.0 - y > 1.05)
            {
                return false;
            }

            // Its whole body has to fit on top.
            const int head = static_cast<int>(std::floor(found + 1.0 + height - 0.05));

            for (int by = found + 1; by <= head; ++by)
            {
                for (int bz = z0; bz <= z1; ++bz)
                {
                    for (int bx = x0; bx <= x1; ++bx)
                    {
                        if (world::solid_at(bx, by, bz))
                        {
                            return false;
                        }
                    }
                }
            }

            top = found + 1.0;

            return true;
        }

        bool push_out_of_blocks(double x, double y, double z, double radius, double height, double& push_x, double& push_z)
        {
            push_x = 0.0;
            push_z = 0.0;

            // From a bit above the feet: the block it stands on is not in its way.
            const int y0 = static_cast<int>(std::floor(y + 0.4));
            const int y1 = static_cast<int>(std::floor(y + height - 0.05));
            const int x0 = static_cast<int>(std::floor(x - radius));
            const int x1 = static_cast<int>(std::floor(x + radius));
            const int z0 = static_cast<int>(std::floor(z - radius));
            const int z1 = static_cast<int>(std::floor(z + radius));

            bool blocked = false;

            for (int by = y0; by <= y1; ++by)
            {
                for (int bz = z0; bz <= z1; ++bz)
                {
                    for (int bx = x0; bx <= x1; ++bx)
                    {
                        if (!world::solid_at(bx, by, bz))
                        {
                            continue;
                        }

                        // The shortest way out of this block, along one axis.
                        const double left = (x + radius) - bx;          // push towards -x
                        const double right = (bx + 1.0) - (x - radius);  // push towards +x
                        const double back = (z + radius) - bz;
                        const double front = (bz + 1.0) - (z - radius);

                        const double out_x = left < right ? -left : right;
                        const double out_z = back < front ? -back : front;

                        if (std::abs(out_x) < std::abs(out_z))
                        {
                            if (std::abs(out_x) > std::abs(push_x))
                            {
                                push_x = out_x;
                            }
                        }
                        else if (std::abs(out_z) > std::abs(push_z))
                        {
                            push_z = out_z;
                        }

                        blocked = true;
                    }
                }
            }

            // Doors, fences, slabs: their real boxes, so an open door is open.
            static std::vector<std::array<float, 6>> boxes;

            const double low[3] = { x - radius, y + 0.4, z - radius };
            const double high[3] = { x + radius, y + height - 0.05, z + radius };

            boxes.clear();
            world::shapes_in(low, high, boxes);

            for (const auto& box : boxes)
            {
                const double left = (x + radius) - box[0];
                const double right = box[3] - (x - radius);
                const double back = (z + radius) - box[2];
                const double front = box[5] - (z - radius);

                const double out_x = left < right ? -left : right;
                const double out_z = back < front ? -back : front;

                if (std::abs(out_x) < std::abs(out_z))
                {
                    if (std::abs(out_x) > std::abs(push_x))
                    {
                        push_x = out_x;
                    }
                }
                else if (std::abs(out_z) > std::abs(push_z))
                {
                    push_z = out_z;
                }

                blocked = true;

                // Proof that doors do their work, at most every two seconds.
                if (const ULONGLONG tick = ::GetTickCount64(); tick - g_door_report > 2000)
                {
                    g_door_report = tick;

                    RC::Output::send<RC::LogLevel::Verbose>(
                        STR("SBMP actors: a character at ({}, {}, {}) is held back by a door or fence\n"),
                        x,
                        y,
                        z
                    );
                }
            }

            return blocked;
        }

        void write_table(SecurityBreachLink& link, const std::vector<proto::ActorRecord>& records)
        {
            auto* table = reinterpret_cast<proto::ActorTable*>(link.base() + proto::kOffActorTable);

            // Seqlock, like the other state blocks: odd while it is being written.
            std::atomic_ref<std::uint32_t> seq(table->seq);

            seq.store(g_table_seq + 1, std::memory_order_release);

            table->count = static_cast<std::uint32_t>(records.size());

            if (!records.empty())
            {
                std::memcpy(table->actors, records.data(), records.size() * sizeof(proto::ActorRecord));
            }

            g_table_seq += 2;
            seq.store(g_table_seq, std::memory_order_release);
        }
    }

    void update(SecurityBreachLink& link, RC::Unreal::UObject* player)
    {
        if (!link.valid() || player == nullptr)
        {
            return;
        }

        const ULONGLONG now = ::GetTickCount64();

        if (now - g_refreshed > kRefreshMs)
        {
            g_refreshed = now;
            refresh(player);
        }

        const auto player_location = reinterpret_cast<RC::Unreal::AActor*>(player)->K2_GetActorLocation();

        std::vector<proto::ActorRecord> records;

        for (const auto& tracked : g_tracked)
        {
            auto* object = tracked.object.Get();

            if (object == nullptr)
            {
                continue;
            }

            auto* actor = reinterpret_cast<RC::Unreal::AActor*>(object);
            auto location = actor->K2_GetActorLocation();

            const double dx = location.X() - player_location.X();
            const double dy = location.Y() - player_location.Y();
            const double dz = location.Z() - player_location.Z();

            if (dx * dx + dy * dy + dz * dz > kRange * kRange)
            {
                continue;
            }

            // Minecraft coordinates: feet, in blocks.
            double x = location.X() / proto::kUnitsPerBlock;
            double z = location.Y() / proto::kUnitsPerBlock;
            double y = location.Z() / proto::kUnitsPerBlock - tracked.height * 0.5;

            // Minecraft blocks are floor too: a character steps onto them (a
            // block up, like a mob) and stands there. Unreal knows nothing of
            // that floor, so it is held up without gravity while it lasts.
            {
                double top = 0.0;
                const bool carried = block_floor(x, y, z, tracked.radius, tracked.height, top);
                const bool was_carried = g_on_blocks.count(object) != 0;

                if (carried)
                {
                    if (!was_carried)
                    {
                        g_on_blocks.insert(object);
                        set_movement_mode(object, kMoveFlying);

                        // Held up it "flies", but it should still walk at its walking pace.
                        if (auto* movement = object->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("CharacterMovement")); movement != nullptr && *movement != nullptr)
                        {
                            auto* walk = (*movement)->GetValuePtrByPropertyNameInChain<float>(STR("MaxWalkSpeed"));
                            auto* fly = (*movement)->GetValuePtrByPropertyNameInChain<float>(STR("MaxFlySpeed"));

                            if (walk != nullptr && fly != nullptr)
                            {
                                *fly = *walk;
                            }
                        }

                        RC::Output::send<RC::LogLevel::Verbose>(
                            STR("SBMP actors: {} stands on Minecraft blocks\n"),
                            object->GetFullName()
                        );
                    }

                    if (std::abs(top - y) > 0.01)
                    {
                        y = top;

                        RC::Unreal::FVector lifted{
                            location.X(),
                            location.Y(),
                            static_cast<float>((y + tracked.height * 0.5) * proto::kUnitsPerBlock)
                        };

                        RC::Unreal::FHitResult sweep_hit{};

                        actor->K2_SetActorLocation(lifted, false, sweep_hit, false);
                        location = actor->K2_GetActorLocation();
                    }
                }
                else if (was_carried)
                {
                    g_on_blocks.erase(object);
                    set_movement_mode(object, kMoveFalling);
                }

                // A hole dug into the game's floor under it: for Unreal the floor
                // is still there, so the mod lowers it in. On the hole's stone
                // bottom (a Minecraft block) the block floor above holds it; in a
                // hole through a thin floor it drops out of the bottom and falls
                // on by itself.
                if (!carried)
                {
                    const int bx = static_cast<int>(std::floor(x));
                    const int bz = static_cast<int>(std::floor(z));
                    const bool over_hole = world::dug_at(bx, static_cast<int>(std::floor(y - 0.3)), bz);
                    const bool in_hole = g_in_hole.count(object) != 0;

                    if (over_hole)
                    {
                        if (!in_hole)
                        {
                            g_in_hole.insert(object);
                            set_movement_mode(object, kMoveFlying);

                            RC::Output::send<RC::LogLevel::Default>(
                                STR("SBMP actors: {} drops into a hole\n"),
                                object->GetFullName()
                            );
                        }

                        y -= kHoleDropBlocks;

                        RC::Unreal::FVector lowered{
                            location.X(),
                            location.Y(),
                            static_cast<float>((y + tracked.height * 0.5) * proto::kUnitsPerBlock)
                        };

                        RC::Unreal::FHitResult sweep_hit{};

                        actor->K2_SetActorLocation(lowered, false, sweep_hit, false);
                        location = actor->K2_GetActorLocation();
                    }
                    else if (in_hole)
                    {
                        // Out of the bottom of a hole through a thin floor: Unreal's own fall.
                        g_in_hole.erase(object);
                        set_movement_mode(object, kMoveFalling);
                    }
                }
                else
                {
                    g_in_hole.erase(object);
                }
            }

            double push_x = 0.0;
            double push_z = 0.0;

            if (push_out_of_blocks(x, y, z, tracked.radius, tracked.height, push_x, push_z))
            {
                x += push_x;
                z += push_z;

                RC::Unreal::FVector moved{
                    static_cast<float>(x * proto::kUnitsPerBlock),
                    static_cast<float>(z * proto::kUnitsPerBlock),
                    location.Z()
                };

                RC::Unreal::FHitResult sweep_hit{};

                actor->K2_SetActorLocation(moved, false, sweep_hit, false);
            }

            // Fire and lava at its feet or body.
            {
                const int bx = static_cast<int>(std::floor(x));
                const int bz = static_cast<int>(std::floor(z));
                int hazard = world::hazard_at(bx, static_cast<int>(std::floor(y + 0.1)), bz);

                if (hazard == 0)
                {
                    hazard = world::hazard_at(bx, static_cast<int>(std::floor(y + 1.1)), bz);
                }

                // A magma block burns what stands on top of it.
                if (hazard == 0 && world::hazard_at(bx, static_cast<int>(std::floor(y - 0.5)), bz) == proto::kHazardMagma)
                {
                    hazard = proto::kHazardMagma;
                }

                auto& burnt = g_burnt[object];

                if (hazard != 0 && now - burnt > kBurnEveryMs)
                {
                    burnt = now;
                    g_burning.push_back({ tracked.object, hazard });
                }
            }

            proto::ActorRecord record{};

            record.formId = tracked.id;
            // Everyone but Freddy is out to get you, and Minecraft's villagers.
            record.flags = std::strstr(tracked.name, "Freddy") != nullptr ? 0u : static_cast<std::uint32_t>(proto::kActorHostile);
            record.x = static_cast<float>(x);
            record.y = static_cast<float>(y);
            record.z = static_cast<float>(z);
            record.yaw = 0.0f;
            record.width = tracked.radius * 2.0f;
            record.height = tracked.height;
            record.healthFrac = 1.0f;
            record.level = 1;
            std::memcpy(record.name, tracked.name, sizeof(record.name));

            records.push_back(record);
        }

        write_table(link, records);

        if (static_cast<int>(records.size()) != g_last_count)
        {
            g_last_count = static_cast<int>(records.size());

            RC::Output::send<RC::LogLevel::Verbose>(
                STR("SBMP actors: {} characters near the player are mirrored in Minecraft\n"),
                g_last_count
            );
        }
    }

    RC::Unreal::UObject* find(std::uint32_t id)
    {
        for (const auto& tracked : g_tracked)
        {
            if (tracked.id == id)
            {
                return tracked.object.Get();
            }
        }

        return nullptr;
    }

    void for_each(void (*visit)(RC::Unreal::UObject* character, void* context), void* context)
    {
        for (const auto& tracked : g_tracked)
        {
            if (auto* object = tracked.object.Get())
            {
                visit(object, context);
            }
        }
    }

    void take_burning(void (*visit)(RC::Unreal::UObject* character, int hazard, void* context), void* context)
    {
        for (const auto& burning : g_burning)
        {
            if (auto* object = burning.object.Get())
            {
                visit(object, burning.hazard, context);
            }
        }

        g_burning.clear();

        // Don't remember characters that are long gone.
        if (g_burnt.size() > 512)
        {
            g_burnt.clear();
        }
    }

    const std::set<std::wstring>& known_classes()
    {
        return g_known_classes;
    }

    std::uint32_t id_of(RC::Unreal::UObject* character)
    {
        for (const auto& tracked : g_tracked)
        {
            if (tracked.object.Get() == character)
            {
                return tracked.id;
            }
        }

        return 0;
    }

    void ignore(RC::Unreal::UObject* character, bool ignored)
    {
        if (ignored)
        {
            g_ignored.insert(character);
        }
        else
        {
            g_ignored.erase(character);
        }

        // Next tick's list is made anew.
        g_refreshed = 0;
    }

    void clear(SecurityBreachLink& link)
    {
        if (!link.valid() || g_last_count == 0)
        {
            return;
        }

        g_last_count = 0;
        write_table(link, {});
    }
}
