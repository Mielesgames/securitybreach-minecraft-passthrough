#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Actors.h"
#include "World.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/AActor.hpp>
#include <Unreal/FHitResult.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
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
        int g_last_count = -1;

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
                if (pawn == nullptr || pawn == player || g_tracked.size() >= proto::kMaxActors)
                {
                    continue;
                }

                Tracked tracked;

                tracked.object = pawn;

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
            const double y = location.Z() / proto::kUnitsPerBlock - tracked.height * 0.5;

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

            proto::ActorRecord record{};

            record.formId = tracked.id;
            record.flags = 0;
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
