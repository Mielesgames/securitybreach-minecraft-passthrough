#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Collision.h"
#include "World.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace collision
{
    namespace
    {
        namespace proto = skycraft::proto;

        // Must match SkyCollision.REGION_SIZE on the Minecraft side.
        constexpr int kRegionBlocks = 8;
        // Cells per block along each axis: a cell is an eighth of a block (8.75 Unreal units).
        // Was a quarter (14 Oct): doorways came in up to a quarter block narrower per side, and
        // Steve needed a thinner body to fit through them.
        constexpr int kCellsPerBlock = 8;
        // Minecraft's voxels are eighths of a block: how many go along one side of a cell.
        constexpr int kVoxelsPerCell = 8 / kCellsPerBlock;
        constexpr int kRegionCells = kRegionBlocks * kCellsPerBlock;
        constexpr double kCellUnits = proto::kUnitsPerBlock / kCellsPerBlock;

        // Regions kept up to date around the player: this many to each side.
        constexpr int kRadius = 1;
        // A region that is done is looked at again after this long (doors open and close).
        constexpr ULONGLONG kRescanMs = 5000;
        // Time per tick spent on queries: more while the player's surroundings are still unknown.
        // Eighth-block cells take more looks: a little more time while regions are unknown.
        constexpr double kBudgetUrgentMs = 6.0;
        constexpr double kBudgetIdleMs = 0.6;

        constexpr std::uint32_t kEpoch = 1;

        // ---- asking Unreal ------------------------------------------------------------------

        // KismetSystemLibrary's trace functions, called through reflection so no
        // parameter layout is assumed.
        struct TraceFunction
        {
            RC::Unreal::UFunction* function = nullptr;
            std::vector<std::uint8_t> params;
            int world_context = -1;
            int start = -1;
            int end = -1;
            int half_size = -1;
            int profile = -1;
            int ignore_self = -1;
            int result = -1;
            int impact_point = -1;
            // Optional: without them other characters can't be told from walls.
            int actors_to_ignore = -1;
            int hit_actor = -1;
            int hit_component = -1;

            static int offset_of(RC::Unreal::UStruct* owner, const wchar_t* name)
            {
                auto* property = owner->FindProperty(RC::Unreal::FName(name, RC::Unreal::FNAME_Find));

                return property != nullptr ? property->GetOffset_Internal() : -1;
            }

            bool resolve(const wchar_t* path, bool box, bool by_channel = false)
            {
                function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(nullptr, nullptr, path);

                if (function == nullptr)
                {
                    return false;
                }

                params.assign(function->GetParmsSize(), 0);

                world_context = offset_of(function, STR("WorldContextObject"));
                start = offset_of(function, STR("Start"));
                end = offset_of(function, STR("End"));
                half_size = box ? offset_of(function, STR("HalfSize")) : 0;
                // By channel: the channel parameter stays 0, which is the visibility channel.
                profile = by_channel ? -1 : offset_of(function, STR("ProfileName"));
                ignore_self = offset_of(function, STR("bIgnoreSelf"));
                result = offset_of(function, STR("ReturnValue"));
                actors_to_ignore = offset_of(function, STR("ActorsToIgnore"));

                auto* hit = function->FindProperty(RC::Unreal::FName(STR("OutHit"), RC::Unreal::FNAME_Find));

                if (hit != nullptr)
                {
                    auto hit_struct = static_cast<RC::Unreal::FStructProperty*>(hit)->GetStruct();
                    auto* inner_property = hit_struct->FindProperty(RC::Unreal::FName(STR("ImpactPoint"), RC::Unreal::FNAME_Find));
                    const int inner = inner_property != nullptr ? inner_property->GetOffset_Internal() : -1;

                    impact_point = inner >= 0 ? hit->GetOffset_Internal() + inner : -1;

                    auto* actor_property = hit_struct->FindProperty(RC::Unreal::FName(STR("Actor"), RC::Unreal::FNAME_Find));

                    hit_actor = actor_property != nullptr ? hit->GetOffset_Internal() + actor_property->GetOffset_Internal() : -1;

                    auto* component_property = hit_struct->FindProperty(RC::Unreal::FName(STR("Component"), RC::Unreal::FNAME_Find));

                    hit_component = component_property != nullptr ? hit->GetOffset_Internal() + component_property->GetOffset_Internal() : -1;
                }

                return
                    world_context >= 0 && start >= 0 && end >= 0 && half_size >= 0 &&
                    (profile >= 0 || by_channel) && ignore_self >= 0 && result >= 0 && impact_point >= 0;
            }
        };

        RC::Unreal::UObject* g_statics = nullptr;
        TraceFunction g_box;
        TraceFunction g_line;
        TraceFunction g_visible;
        TraceFunction g_visible_box;
        bool g_visible_resolved = false;
        RC::Unreal::FName g_profile;
        bool g_resolve_tried = false;
        bool g_resolved = false;

        bool resolve()
        {
            if (g_resolve_tried)
            {
                return g_resolved;
            }

            g_resolve_tried = true;

            g_statics = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UObject*>(
                nullptr,
                nullptr,
                STR("/Script/Engine.Default__KismetSystemLibrary")
            );

            // "Pawn": blocked by exactly what blocks a walking character.
            g_profile = RC::Unreal::FName(STR("Pawn"), RC::Unreal::FNAME_Add);

            g_resolved =
                g_statics != nullptr &&
                g_box.resolve(STR("/Script/Engine.KismetSystemLibrary:BoxTraceSingleByProfile"), true) &&
                g_line.resolve(STR("/Script/Engine.KismetSystemLibrary:LineTraceSingleByProfile"), false);

            g_visible_resolved =
                g_resolved &&
                g_visible.resolve(STR("/Script/Engine.KismetSystemLibrary:LineTraceSingle"), false, true) &&
                g_visible_box.resolve(STR("/Script/Engine.KismetSystemLibrary:BoxTraceSingle"), true, true);

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP collision: {}\n"),
                g_resolved ? STR("ready to scan the game's geometry") : STR("the engine's trace functions were not found, no hitboxes")
            );

            return g_resolved;
        }

        void put_vector(TraceFunction& trace, int offset, double x, double y, double z)
        {
            const float v[3]{ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };

            std::memcpy(trace.params.data() + offset, v, sizeof(v));
        }

        bool call(TraceFunction& trace, RC::Unreal::UObject* character)
        {
            std::memcpy(trace.params.data() + trace.world_context, &character, sizeof(character));
            if (trace.profile >= 0)
            {
                std::memcpy(trace.params.data() + trace.profile, &g_profile, sizeof(g_profile));
            }

            trace.params[trace.ignore_self] = 1;
            trace.params[trace.result] = 0;

            g_statics->ProcessEvent(trace.function, trace.params.data());

            return trace.params[trace.result] != 0;
        }

        // The actor of the hit the last call returned.
        RC::Unreal::UObject* hit_actor(const TraceFunction& trace)
        {
            if (trace.hit_actor < 0)
            {
                return nullptr;
            }

            RC::Unreal::FWeakObjectPtr actor;

            std::memcpy(static_cast<void*>(&actor), trace.params.data() + trace.hit_actor, sizeof(actor));

            return actor.Get();
        }

        // The component of the hit the last call returned.
        RC::Unreal::UObject* hit_component(const TraceFunction& trace)
        {
            if (trace.hit_component < 0)
            {
                return nullptr;
            }

            RC::Unreal::FWeakObjectPtr component;

            std::memcpy(static_cast<void*>(&component), trace.params.data() + trace.hit_component, sizeof(component));

            return component.Get();
        }

        // Makes the next call skip these actors. The engine only reads the array.
        void ignore_actors(TraceFunction& trace, RC::Unreal::UObject** actors, int count)
        {
            if (trace.actors_to_ignore < 0)
            {
                return;
            }

            struct
            {
                RC::Unreal::UObject** data;
                std::int32_t num;
                std::int32_t max;
            } array{ count > 0 ? actors : nullptr, count, count };

            std::memcpy(trace.params.data() + trace.actors_to_ignore, &array, sizeof(array));
        }

        // Freddy, the animatronics, the bots: things that walk around. They
        // must not end up in Minecraft as walls that stay where they stood.
        bool is_pawn(RC::Unreal::UObject* actor)
        {
            static std::unordered_map<RC::Unreal::UClass*, bool> by_class;

            auto* actor_class = actor->GetClassPrivate();
            const auto known = by_class.find(actor_class);

            if (known != by_class.end())
            {
                return known->second;
            }

            // Only pawns have a controller to ask for.
            const bool pawn = actor->GetFunctionByNameInChain(STR("GetController")) != nullptr;

            by_class[actor_class] = pawn;

            return pawn;
        }

        // ---- Minecraft cells <-> Unreal ---------------------------------------------------------
        // A point in cells (eighth blocks, Minecraft axes) is (x, z, y) * kCellUnits in Unreal.

        // Is anything that blocks a pawn inside this cube of cells?
        bool box_blocked(RC::Unreal::UObject* character, int cx, int cy, int cz, int size)
        {
            // Inside Minecraft's inverted blocks the game's geometry is dug away.
            if (world::any_dug())
            {
                const int x0 = cx / kCellsPerBlock - (cx < 0 && cx % kCellsPerBlock != 0);
                const int y0 = cy / kCellsPerBlock - (cy < 0 && cy % kCellsPerBlock != 0);
                const int z0 = cz / kCellsPerBlock - (cz < 0 && cz % kCellsPerBlock != 0);
                const int x1 = (cx + size - 1) / kCellsPerBlock - (cx + size - 1 < 0 && (cx + size - 1) % kCellsPerBlock != 0);
                const int y1 = (cy + size - 1) / kCellsPerBlock - (cy + size - 1 < 0 && (cy + size - 1) % kCellsPerBlock != 0);
                const int z1 = (cz + size - 1) / kCellsPerBlock - (cz + size - 1 < 0 && (cz + size - 1) % kCellsPerBlock != 0);

                bool all_dug = true;

                for (int by = y0; by <= y1 && all_dug; ++by)
                {
                    for (int bz = z0; bz <= z1 && all_dug; ++bz)
                    {
                        for (int bx = x0; bx <= x1 && all_dug; ++bx)
                        {
                            all_dug = world::dug_at(bx, by, bz);
                        }
                    }
                }

                if (all_dug)
                {
                    return false;
                }
            }

            const double half = size * kCellUnits * 0.5;
            const double ux = cx * kCellUnits + half;
            const double uy = cz * kCellUnits + half;
            const double uz = cy * kCellUnits + half;

            // A little smaller than the cube, so a wall that only touches its side doesn't count.
            const double extent = half - 1.0;

            RC::Unreal::UObject* walkers[4]{};
            int walker_count = 0;

            for (;;)
            {
                // A sweep that goes nowhere reports nothing: move it a hair.
                std::fill(g_box.params.begin(), g_box.params.end(), std::uint8_t{ 0 });
                put_vector(g_box, g_box.start, ux, uy, uz - 0.05);
                put_vector(g_box, g_box.end, ux, uy, uz + 0.05);
                put_vector(g_box, g_box.half_size, extent, extent, extent);
                ignore_actors(g_box, walkers, walker_count);

                if (!call(g_box, character))
                {
                    return false;
                }

                // Hit a character: look again without it, there may be a wall behind.
                auto* actor = hit_actor(g_box);

                if (actor == nullptr || walker_count == 4 || g_box.actors_to_ignore < 0 || !is_pawn(actor))
                {
                    return true;
                }

                walkers[walker_count++] = actor;
            }
        }

        // Height (in cells) of the first surface under the top of a cell's centre, or a negative
        // number if a ray straight down the cell doesn't hit anything.
        double surface_in_cell(RC::Unreal::UObject* character, int cx, int cy, int cz)
        {
            const double ux = (cx + 0.5) * kCellUnits;
            const double uy = (cz + 0.5) * kCellUnits;
            const double top = (cy + 1) * kCellUnits;
            const double bottom = cy * kCellUnits;

            std::fill(g_line.params.begin(), g_line.params.end(), std::uint8_t{ 0 });
            put_vector(g_line, g_line.start, ux, uy, top + 0.5);
            put_vector(g_line, g_line.end, ux, uy, bottom - 0.5);

            if (!call(g_line, character))
            {
                return -1.0;
            }

            float impact[3]{};
            std::memcpy(impact, g_line.params.data() + g_line.impact_point, sizeof(impact));

            return std::clamp(double(impact[2]), bottom, top) / kCellUnits;
        }

        // ---- one region -------------------------------------------------------------------------

        struct Node
        {
            std::int16_t x, y, z;  // cell within the region
            std::int16_t size;     // cells
        };

        struct Region
        {
            int rx = 0;
            int ry = 0;
            int rz = 0;

            // Scan in progress.
            bool scanning = false;
            std::vector<Node> pending;
            std::vector<std::uint8_t> solid;   // kRegionCells^3, index (y * N + z) * N + x
            std::vector<float> top;            // for solid cells: where the solid really ends, in cells
            int refine_at = 0;

            // What Minecraft has.
            bool sent = false;
            // What Minecraft has is known to be out of date.
            bool dirty = false;
            std::uint64_t sent_hash = 0;
            ULONGLONG scanned_tick = 0;

            static int index(int x, int y, int z)
            {
                return (y * kRegionCells + z) * kRegionCells + x;
            }

            bool is_solid(int x, int y, int z) const
            {
                // Outside the region counts as open: the neighbouring region brings its own faces.
                return
                    x >= 0 && y >= 0 && z >= 0 &&
                    x < kRegionCells && y < kRegionCells && z < kRegionCells &&
                    solid[index(x, y, z)] != 0;
            }
        };

        std::unordered_map<std::uint64_t, std::unique_ptr<Region>> g_regions;
        bool g_cleared = false;
        int g_regions_sent = 0;

        std::uint64_t region_key(int rx, int ry, int rz)
        {
            return
                (std::uint64_t(std::uint32_t(rx) & 0x1FFFFF) << 42) |
                (std::uint64_t(std::uint32_t(ry) & 0x1FFFFF) << 21) |
                std::uint64_t(std::uint32_t(rz) & 0x1FFFFF);
        }

        void begin_scan(Region& region)
        {
            region.scanning = true;
            region.dirty = false;
            region.solid.assign(std::size_t(kRegionCells) * kRegionCells * kRegionCells, 0);
            region.top.assign(region.solid.size(), 0.0f);
            region.pending.clear();
            region.pending.push_back({ 0, 0, 0, static_cast<std::int16_t>(kRegionCells) });
            region.refine_at = 0;
        }

        double now_ms()
        {
            static LARGE_INTEGER frequency{};

            if (frequency.QuadPart == 0)
            {
                ::QueryPerformanceFrequency(&frequency);
            }

            LARGE_INTEGER counter{};
            ::QueryPerformanceCounter(&counter);

            return double(counter.QuadPart) * 1000.0 / double(frequency.QuadPart);
        }

        void add_quad(std::vector<proto::ColTri>& tris, const float a[3], const float b[3], const float c[3], const float d[3])
        {
            proto::ColTri first{};
            proto::ColTri second{};

            std::memcpy(first.v + 0, a, 12);
            std::memcpy(first.v + 3, b, 12);
            std::memcpy(first.v + 6, c, 12);
            std::memcpy(second.v + 0, a, 12);
            std::memcpy(second.v + 3, c, 12);
            std::memcpy(second.v + 6, d, 12);

            tris.push_back(first);
            tris.push_back(second);
        }

        // Turns the finished scan into what Minecraft takes, and sends it if it
        // differs from what Minecraft already has. Returns false if the ring
        // is full (try again next tick).
        bool publish(SecurityBreachLink& link, Region& region)
        {
            const int n = kRegionCells;
            const double cell = 1.0 / kCellsPerBlock;  // blocks
            const double ox = region.rx * double(kRegionBlocks);
            const double oy = region.ry * double(kRegionBlocks);
            const double oz = region.rz * double(kRegionBlocks);

            std::vector<proto::ColTri> tris;
            std::unordered_map<std::uint32_t, proto::ColBlock> blocks;
            std::uint64_t hash = 1469598103934665603ull;

            for (int y = 0; y < n; ++y)
            {
                for (int z = 0; z < n; ++z)
                {
                    for (int x = 0; x < n; ++x)
                    {
                        if (!region.is_solid(x, y, z))
                        {
                            continue;
                        }

                        const float top_cells = region.top[Region::index(x, y, z)];

                        hash = (hash ^ std::uint64_t(Region::index(x, y, z))) * 1099511628211ull;
                        hash = (hash ^ std::uint64_t(std::llround(top_cells * 64.0))) * 1099511628211ull;

                        const float x0 = static_cast<float>(ox + x * cell);
                        const float x1 = static_cast<float>(ox + (x + 1) * cell);
                        const float z0 = static_cast<float>(oz + z * cell);
                        const float z1 = static_cast<float>(oz + (z + 1) * cell);
                        const float y0 = static_cast<float>(oy + y * cell);
                        const float y1 = static_cast<float>(region.ry * double(kRegionBlocks) + top_cells * cell);

                        // The solid part of the cell is the box (x0..x1, y0..y1, z0..z1); every
                        // side that borders open space becomes two triangles.
                        if (!region.is_solid(x, y + 1, z))
                        {
                            const float a[3]{ x0, y1, z0 }, b[3]{ x1, y1, z0 }, c[3]{ x1, y1, z1 }, d[3]{ x0, y1, z1 };
                            add_quad(tris, a, b, c, d);
                        }

                        if (!region.is_solid(x, y - 1, z))
                        {
                            const float a[3]{ x0, y0, z0 }, b[3]{ x1, y0, z0 }, c[3]{ x1, y0, z1 }, d[3]{ x0, y0, z1 };
                            add_quad(tris, a, b, c, d);
                        }

                        if (y1 - y0 > 1.0e-4f)
                        {
                            if (!region.is_solid(x - 1, y, z))
                            {
                                const float a[3]{ x0, y0, z0 }, b[3]{ x0, y0, z1 }, c[3]{ x0, y1, z1 }, d[3]{ x0, y1, z0 };
                                add_quad(tris, a, b, c, d);
                            }

                            if (!region.is_solid(x + 1, y, z))
                            {
                                const float a[3]{ x1, y0, z0 }, b[3]{ x1, y0, z1 }, c[3]{ x1, y1, z1 }, d[3]{ x1, y1, z0 };
                                add_quad(tris, a, b, c, d);
                            }

                            if (!region.is_solid(x, y, z - 1))
                            {
                                const float a[3]{ x0, y0, z0 }, b[3]{ x1, y0, z0 }, c[3]{ x1, y1, z0 }, d[3]{ x0, y1, z0 };
                                add_quad(tris, a, b, c, d);
                            }

                            if (!region.is_solid(x, y, z + 1))
                            {
                                const float a[3]{ x0, y0, z1 }, b[3]{ x1, y0, z1 }, c[3]{ x1, y1, z1 }, d[3]{ x0, y1, z1 };
                                add_quad(tris, a, b, c, d);
                            }
                        }

                        // The same cell as voxels (an eighth of a block), for mobs and items.
                        const int bx = x / kCellsPerBlock;
                        const int by = y / kCellsPerBlock;
                        const int bz = z / kCellsPerBlock;
                        const std::uint32_t block_key = (std::uint32_t(by) << 16) | (std::uint32_t(bz) << 8) | std::uint32_t(bx);

                        auto& block = blocks[block_key];

                        block.x = region.rx * kRegionBlocks + bx;
                        block.y = region.ry * kRegionBlocks + by;
                        block.z = region.rz * kRegionBlocks + bz;

                        const int vx = (x % kCellsPerBlock) * kVoxelsPerCell;
                        const int vy = (y % kCellsPerBlock) * kVoxelsPerCell;
                        const int vz = (z % kCellsPerBlock) * kVoxelsPerCell;

                        for (int dy = 0; dy < kVoxelsPerCell; ++dy)
                        {
                            for (int dz = 0; dz < kVoxelsPerCell; ++dz)
                            {
                                for (int dx = 0; dx < kVoxelsPerCell; ++dx)
                                {
                                    block.bits[vy + dy] |= std::uint64_t(1) << ((vz + dz) * 8 + (vx + dx));
                                }
                            }
                        }
                    }
                }
            }

            if (region.sent && hash == region.sent_hash)
            {
                return true;
            }

            proto::ColRegion header{};

            header.minX = region.rx * kRegionBlocks;
            header.minY = region.ry * kRegionBlocks;
            header.minZ = region.rz * kRegionBlocks;
            header.maxX = header.minX + kRegionBlocks - 1;
            header.maxY = header.minY + kRegionBlocks - 1;
            header.maxZ = header.minZ + kRegionBlocks - 1;
            header.epoch = kEpoch;

            std::vector<proto::ColBlock> block_list;
            block_list.reserve(blocks.size());

            for (const auto& [key, block] : blocks)
            {
                block_list.push_back(block);
            }

            // Triangles first: the region message is what tells Minecraft the area is known.
            header.count = static_cast<std::uint32_t>(tris.size());

            if (!link.write_collision(proto::kColTris, &header, sizeof(header), tris.data(), tris.size() * sizeof(proto::ColTri)))
            {
                return false;
            }

            header.count = static_cast<std::uint32_t>(block_list.size());

            if (!link.write_collision(proto::kColRegion, &header, sizeof(header), block_list.data(), block_list.size() * sizeof(proto::ColBlock)))
            {
                return false;
            }

            if (!region.sent)
            {
                ++g_regions_sent;
            }

            region.sent = true;
            region.sent_hash = hash;

            return true;
        }

        // Works on a region's scan until it is done or the time is up. Returns true when done.
        bool scan(SecurityBreachLink& link, Region& region, RC::Unreal::UObject* character, double deadline)
        {
            const int base_x = region.rx * kRegionCells;
            const int base_y = region.ry * kRegionCells;
            const int base_z = region.rz * kRegionCells;

            // 1) Which cells have something in them: split every blocked cube into eight.
            while (!region.pending.empty())
            {
                if (now_ms() > deadline)
                {
                    return false;
                }

                const Node node = region.pending.back();
                region.pending.pop_back();

                if (!box_blocked(character, base_x + node.x, base_y + node.y, base_z + node.z, node.size))
                {
                    continue;
                }

                if (node.size == 1)
                {
                    const int i = Region::index(node.x, node.y, node.z);

                    region.solid[i] = 1;
                    region.top[i] = static_cast<float>(node.y + 1);
                    continue;
                }

                const auto half = static_cast<std::int16_t>(node.size / 2);

                for (int i = 0; i < 8; ++i)
                {
                    region.pending.push_back({
                        static_cast<std::int16_t>(node.x + ((i & 1) ? half : 0)),
                        static_cast<std::int16_t>(node.y + ((i & 2) ? half : 0)),
                        static_cast<std::int16_t>(node.z + ((i & 4) ? half : 0)),
                        half,
                    });
                }
            }

            // 2) Where the floor really is: the top cell of every stack holds a surface somewhere
            //    inside it. A ray finds the exact height, so floors and ramps aren't eighth-block steps.
            const int total = kRegionCells * kRegionCells * kRegionCells;

            for (; region.refine_at < total; ++region.refine_at)
            {
                const int i = region.refine_at;

                if (region.solid[i] == 0)
                {
                    continue;
                }

                const int x = i % kRegionCells;
                const int z = (i / kRegionCells) % kRegionCells;
                const int y = i / (kRegionCells * kRegionCells);

                if (region.is_solid(x, y + 1, z))
                {
                    continue;
                }

                if (now_ms() > deadline)
                {
                    return false;
                }

                const double surface = surface_in_cell(character, base_x + x, base_y + y, base_z + z);

                if (surface >= 0.0)
                {
                    region.top[i] = static_cast<float>(surface - base_y);
                }
            }

            // 3) Hand it to Minecraft.
            if (!publish(link, region))
            {
                return false;
            }

            region.scanning = false;
            region.scanned_tick = ::GetTickCount64();
            region.solid = {};
            region.top = {};

            return true;
        }
    }

    void update(SecurityBreachLink& link, RC::Unreal::UObject* character, double x, double y, double z)
    {
        if (character == nullptr || !link.valid() || !resolve())
        {
            return;
        }

        if (!g_cleared)
        {
            // Minecraft may hold regions from an earlier run of the game.
            const std::uint32_t epoch = kEpoch;

            if (!link.write_collision(proto::kColClear, &epoch, sizeof(epoch), nullptr, 0))
            {
                return;
            }

            g_cleared = true;
        }

        const int prx = static_cast<int>(std::floor(x / kRegionBlocks));
        const int pry = static_cast<int>(std::floor(y / kRegionBlocks));
        const int prz = static_cast<int>(std::floor(z / kRegionBlocks));

        // The regions around the player, nearest first: unknown ones before stale ones.
        struct Wanted
        {
            Region* region;
            double distance;
            bool unknown;
        };

        std::vector<Wanted> wanted;
        const ULONGLONG tick = ::GetTickCount64();
        bool urgent = false;

        for (int dy = -kRadius; dy <= kRadius; ++dy)
        {
            for (int dz = -kRadius; dz <= kRadius; ++dz)
            {
                for (int dx = -kRadius; dx <= kRadius; ++dx)
                {
                    const int rx = prx + dx;
                    const int ry = pry + dy;
                    const int rz = prz + dz;

                    auto& slot = g_regions[region_key(rx, ry, rz)];

                    if (!slot)
                    {
                        slot = std::make_unique<Region>();
                        slot->rx = rx;
                        slot->ry = ry;
                        slot->rz = rz;
                    }

                    if (slot->sent && !slot->scanning && !slot->dirty && tick - slot->scanned_tick < kRescanMs)
                    {
                        continue;
                    }

                    const double cx = (rx + 0.5) * kRegionBlocks - x;
                    const double cy = (ry + 0.5) * kRegionBlocks - y;
                    const double cz = (rz + 0.5) * kRegionBlocks - z;

                    wanted.push_back({ slot.get(), cx * cx + cy * cy + cz * cz, !slot->sent || slot->dirty });
                    urgent = urgent || !slot->sent || slot->dirty;
                }
            }
        }

        std::sort(wanted.begin(), wanted.end(), [](const Wanted& a, const Wanted& b)
        {
            return a.unknown != b.unknown ? a.unknown : a.distance < b.distance;
        });

        const double deadline = now_ms() + (urgent ? kBudgetUrgentMs : kBudgetIdleMs);

        for (const auto& item : wanted)
        {
            if (!item.region->scanning)
            {
                begin_scan(*item.region);
            }

            if (!scan(link, *item.region, character, deadline))
            {
                break;
            }
        }
    }

    RC::Unreal::UObject* trace(
        RC::Unreal::UObject* character,
        const double from[3],
        const double to[3],
        double thickness,
        RC::Unreal::UObject** component,
        double* impact
    )
    {
        if (component != nullptr)
        {
            *component = nullptr;
        }

        if (character == nullptr || !resolve())
        {
            return nullptr;
        }

        auto& function = thickness > 0.0 ? g_box : g_line;

        std::fill(function.params.begin(), function.params.end(), std::uint8_t{ 0 });
        put_vector(function, function.start, from[0], from[1], from[2]);
        put_vector(function, function.end, to[0], to[1], to[2]);

        if (thickness > 0.0)
        {
            put_vector(function, function.half_size, thickness, thickness, thickness);
        }

        if (!call(function, character))
        {
            return nullptr;
        }

        if (component != nullptr)
        {
            *component = hit_component(function);
        }

        if (impact != nullptr)
        {
            float point[3]{};

            std::memcpy(point, function.params.data() + function.impact_point, sizeof(point));

            for (int i = 0; i < 3; ++i)
            {
                impact[i] = point[i];
            }
        }

        return hit_actor(function);
    }

    RC::Unreal::UObject* trace_visible(
        RC::Unreal::UObject* character,
        const double from[3],
        const double to[3],
        double thickness
    )
    {
        if (character == nullptr || !resolve() || !g_visible_resolved)
        {
            return nullptr;
        }

        auto& function = thickness > 0.0 ? g_visible_box : g_visible;

        std::fill(function.params.begin(), function.params.end(), std::uint8_t{ 0 });
        put_vector(function, function.start, from[0], from[1], from[2]);
        put_vector(function, function.end, to[0], to[1], to[2]);

        if (thickness > 0.0)
        {
            put_vector(function, function.half_size, thickness, thickness, thickness);
        }

        return call(function, character) ? hit_actor(function) : nullptr;
    }

    void invalidate(double x, double y, double z, double radius)
    {
        const auto region_of = [](double v)
        {
            return static_cast<int>(std::floor(v / kRegionBlocks));
        };

        for (int ry = region_of(y - radius); ry <= region_of(y + radius); ++ry)
        {
            for (int rz = region_of(z - radius); rz <= region_of(z + radius); ++rz)
            {
                for (int rx = region_of(x - radius); rx <= region_of(x + radius); ++rx)
                {
                    const auto it = g_regions.find(region_key(rx, ry, rz));

                    if (it == g_regions.end() || !it->second->sent)
                    {
                        continue;
                    }

                    it->second->dirty = true;

                    // A scan that is half done saw the old situation: start over.
                    if (it->second->scanning)
                    {
                        begin_scan(*it->second);
                        it->second->dirty = true;
                    }
                }
            }
        }
    }

    int regions_sent()
    {
        return g_regions_sent;
    }
}
