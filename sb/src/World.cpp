// The block part of SkyCraft's skse/src/WorldRender.cpp, cut down to what Security Breach can
// use so far: sections, entities and their textures, Minecraft's own light levels, and the
// game's depth buffer to hide what is behind its geometry.
#include "World.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <d3dcompiler.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace world
{
    namespace
    {
        namespace proto = skycraft::proto;

        using Vertex = proto::RenVertex;

        constexpr std::uint32_t kFlagCutout = 1;
        constexpr std::uint32_t kFlagTranslucent = 2;
        constexpr std::uint32_t kFlagUntextured = 4;
        constexpr std::uint32_t kFlagNormalFromFaces = 7u << 4;
        constexpr std::uint32_t kFullSkyLight = 15u << 8;
        constexpr float kPi = 3.14159265358979f;

        // Unreal's default near plane. The game's depth buffer holds near / distance, so this
        // has to be the game's value for its walls to hide blocks at the right distance.
        constexpr float kNearPlane = 10.0f;

        // ID3D11DeviceContext's vtable.
        constexpr int kSetTargetsIndex = 33;        // OMSetRenderTargets
        constexpr int kSetTargetsUavIndex = 34;     // OMSetRenderTargetsAndUnorderedAccessViews
        constexpr int kClearDepthIndex = 53;        // ClearDepthStencilView

        struct Section
        {
            std::int32_t sx = 0;
            std::int32_t sy = 0;
            std::int32_t sz = 0;
            ID3D11Buffer* vb = nullptr;
            // Opaque and cutout triangles come first in the buffer, translucent ones after.
            std::uint32_t opaque = 0;
            std::uint32_t translucent = 0;
        };

        struct alignas(16) Constants
        {
            float row_x[4];   // clip x = dot(row_x, rel)
            float row_y[4];   // clip y = dot(row_y, rel)
            float row_w[4];   // clip w = dot(row_w, rel): distance along the view direction
            float offset[4];  // section origin relative to the camera, Unreal units; w = near plane
            float depth[4];   // game depth texels per back buffer pixel (x, y); z: 1 = use the game's depth
        };

        // Everything but the blocks, as Minecraft drew it this frame: mobs, items, particles.
        struct Batch
        {
            std::uint32_t texture = 0;
            std::uint32_t first = 0;
            std::uint32_t count = 0;
            bool translucent = false;
        };

        constexpr char kShader[] = R"(
cbuffer Constants : register(b0) { float4 rowX; float4 rowY; float4 rowW; float4 offset; float4 depthInfo; };
Texture2D atlas : register(t0);
Texture2D<float> sceneDepth : register(t1);
SamplerState samp : register(s0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; uint light : TEXCOORD1; uint flags : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; float2 light : TEXCOORD1; nointerpolation uint flags : TEXCOORD2; float viewZ : TEXCOORD3; };
VSOut VSMain(VSIn i) {
    VSOut o;
    // Minecraft -> Unreal axes: x stays, Minecraft's up (y) is Unreal's z.
    float3 rel = offset.xyz + float3(i.pos.x, i.pos.z, i.pos.y) * 70.0;
    // Reversed depth with an infinite far plane, like Unreal's own: z / w = near / distance.
    o.pos = float4(dot(rowX.xyz, rel), dot(rowY.xyz, rel), offset.w, dot(rowW.xyz, rel));
    o.uv = i.uv;
    o.color = i.color;
    o.light = float2(i.light & 0xFF, (i.light >> 8) & 0xFF) / 15.0;
    o.flags = i.flags;
    o.viewZ = o.pos.w;
    return o;
}
// Minecraft's fixed shading per face, by Direction ordinal + 1 (0: none).
static const float kFaceShade[8] = { 1.0, 0.5, 1.0, 0.8, 0.8, 0.6, 0.6, 1.0 };
float4 PSMain(VSOut i) : SV_Target {
    if (depthInfo.z > 0.5) {
        // The game's depth: near / distance, 0 where there is nothing (sky).
        float d = sceneDepth.Load(int3(i.pos.xy * depthInfo.xy, 0));
        if (d > 0 && i.viewZ > (offset.w / d) * 1.002 + 1.5) discard;  // behind the game's geometry
    }
    float4 t = (i.flags & 4) ? float4(1, 1, 1, 1) : atlas.Sample(samp, i.uv);
    bool translucent = (i.flags & 2) != 0;
    if (!translucent && t.a < 0.5) discard;
    float light = depthInfo.w > 0.5 ? 1.0 : 0.3 + 0.7 * max(i.light.x, i.light.y);  // w: night vision
    float3 c = t.rgb * i.color.rgb * light * kFaceShade[(i.flags >> 4) & 7];
    return float4(c, translucent ? t.a * i.color.a : 1.0);
}
// A triangle over the whole screen that outputs one colour (offset.xyz): for brightening the picture.
float4 VSFull(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSFull() : SV_Target {
    return float4(offset.xyz, 0);
}
)";

        // Which blocks of a section are solid, one bit each (x + 16z + 256y). Read by the game thread.
        std::mutex g_solids_mutex;
        std::unordered_map<std::uint64_t, std::array<std::uint8_t, 512>> g_solids;

        // Burning blocks per section (same lock): packed x | y << 4 | z << 8 | hazard << 12.
        std::unordered_map<std::uint64_t, std::vector<std::uint16_t>> g_hazards;
        // Boxes of the blocks that aren't full cubes, per section (same lock), in blocks.
        std::unordered_map<std::uint64_t, std::vector<std::array<float, 6>>> g_shapes;

        std::mutex g_camera_mutex;
        Camera g_camera;

        std::unordered_map<std::uint64_t, Section> g_sections;
        std::vector<Vertex> g_scratch;

        // Entity textures (skins, mobs, armour) by Minecraft's id; 0 is the atlas.
        std::unordered_map<std::uint32_t, ID3D11ShaderResourceView*> g_textures;

        // Arrows, dropped items, block cracks and the outline of the targeted block: Minecraft
        // describes them (WorldEntities) and they are built into triangles here every frame.
        proto::WorldEntities g_entities{};
        std::vector<Vertex> g_dynamic;        // solid triangles, then cracks, then the outline's lines
        std::uint32_t g_dynamic_solid = 0;
        std::uint32_t g_dynamic_cracks = 0;
        std::uint32_t g_dynamic_lines = 0;
        ID3D11Buffer* g_dynamic_vb = nullptr;
        UINT g_dynamic_capacity = 0;
        double g_dynamic_origin[3]{};

        // The player's own body in third person: sent every frame like the scene, relative to its feet.
        std::vector<Batch> g_avatar_batches;
        ID3D11Buffer* g_avatar_vb = nullptr;
        UINT g_avatar_vb_capacity = 0;
        std::mutex g_player_mutex;
        double g_player_position[3]{};
        std::vector<std::array<double, 6>> g_ropes;  // also under g_player_mutex
        std::atomic<bool> g_night_vision{ false };
        ID3D11VertexShader* g_vs_full = nullptr;
        ID3D11PixelShader* g_ps_full = nullptr;
        ID3D11BlendState* g_blend_multiply = nullptr;
        ID3D11BlendState* g_blend_add = nullptr;
        ID3D11DepthStencilState* g_depth_off = nullptr;

        std::vector<Batch> g_scene_batches;
        ID3D11Buffer* g_scene_vb = nullptr;
        UINT g_scene_vb_capacity = 0;
        UINT g_scene_vertices = 0;
        double g_scene_origin[3]{};

        // The game's context, with our ClearDepthStencilView in its vtable.
        using ClearDepthFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
        using SetTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
        using SetTargetsUavFn = void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
            UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
        void** g_context_vtable = nullptr;
        ClearDepthFn g_original_clear_depth = nullptr;
        SetTargetsFn g_original_set_targets = nullptr;
        SetTargetsUavFn g_original_set_targets_uav = nullptr;

        // Depth views already looked at this frame (the game binds the same few many times).
        ID3D11DepthStencilView* g_seen[32]{};
        int g_seen_count = 0;
        // Distinct depth buffers reported in the log so far.
        ID3D11Texture2D* g_reported[12]{};
        int g_reported_count = 0;

        // The depth buffer the game filled for the frame being presented: the
        // biggest one it drew into this frame that has the back buffer's shape.
        ID3D11Texture2D* g_candidate = nullptr;
        UINT g_candidate_w = 0;
        UINT g_candidate_h = 0;
        ID3D11Texture2D* g_scene_depth = nullptr;
        ID3D11ShaderResourceView* g_scene_depth_srv = nullptr;
        UINT g_scene_depth_w = 0;
        UINT g_scene_depth_h = 0;
        UINT g_back_w = 0;
        UINT g_back_h = 0;

        ID3D11Texture2D* g_atlas = nullptr;
        ID3D11ShaderResourceView* g_atlas_srv = nullptr;
        UINT g_atlas_w = 0;
        UINT g_atlas_h = 0;

        ID3D11VertexShader* g_vs = nullptr;
        ID3D11PixelShader* g_ps = nullptr;
        ID3D11InputLayout* g_layout = nullptr;
        ID3D11Buffer* g_constants = nullptr;
        ID3D11SamplerState* g_sampler = nullptr;
        ID3D11RasterizerState* g_raster = nullptr;
        ID3D11BlendState* g_blend_off = nullptr;
        ID3D11BlendState* g_blend_alpha = nullptr;
        ID3D11DepthStencilState* g_depth_write = nullptr;
        ID3D11DepthStencilState* g_depth_read = nullptr;
        ID3D11Texture2D* g_depth = nullptr;
        ID3D11DepthStencilView* g_dsv = nullptr;
        UINT g_depth_w = 0;
        UINT g_depth_h = 0;
        bool g_init_done = false;
        bool g_init_ok = false;

        template <class T>
        void safe_release(T*& ptr)
        {
            if (ptr != nullptr)
            {
                ptr->Release();
                ptr = nullptr;
            }
        }

        std::uint64_t section_key(std::int32_t sx, std::int32_t sy, std::int32_t sz)
        {
            return
                (std::uint64_t(std::uint32_t(sx) & 0x1FFFFF) << 42) |
                (std::uint64_t(std::uint32_t(sy) & 0x1FFFFF) << 21) |
                std::uint64_t(std::uint32_t(sz) & 0x1FFFFF);
        }

        void clear_sections()
        {
            for (auto& [key, section] : g_sections)
            {
                safe_release(section.vb);
            }

            g_sections.clear();
        }

        bool compile(const char* entry, const char* target, ID3DBlob** out)
        {
            ID3DBlob* errors = nullptr;

            const auto hr = D3DCompile(
                kShader,
                sizeof(kShader) - 1,
                "sbmp_world",
                nullptr,
                nullptr,
                entry,
                target,
                D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0,
                out,
                &errors
            );

            safe_release(errors);

            return SUCCEEDED(hr);
        }

        void STDMETHODCALLTYPE clear_depth_hook(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
        void STDMETHODCALLTYPE set_targets_hook(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
        void STDMETHODCALLTYPE set_targets_uav_hook(
            ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
            UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
        void patch_vtable(int index, void* target);

        bool init(ID3D11Device* device)
        {
            if (g_init_done)
            {
                return g_init_ok;
            }

            g_init_done = true;

            // Watch which depth buffers the game clears and draws into, to find the scene's.
            ID3D11DeviceContext* immediate = nullptr;
            device->GetImmediateContext(&immediate);

            if (immediate != nullptr)
            {
                g_context_vtable = *reinterpret_cast<void***>(immediate);
                g_original_clear_depth = reinterpret_cast<ClearDepthFn>(g_context_vtable[kClearDepthIndex]);
                g_original_set_targets = reinterpret_cast<SetTargetsFn>(g_context_vtable[kSetTargetsIndex]);
                g_original_set_targets_uav = reinterpret_cast<SetTargetsUavFn>(g_context_vtable[kSetTargetsUavIndex]);
                patch_vtable(kClearDepthIndex, reinterpret_cast<void*>(&clear_depth_hook));
                patch_vtable(kSetTargetsIndex, reinterpret_cast<void*>(&set_targets_hook));
                patch_vtable(kSetTargetsUavIndex, reinterpret_cast<void*>(&set_targets_uav_hook));
                immediate->Release();
            }

            ID3DBlob* vs_blob = nullptr;
            ID3DBlob* ps_blob = nullptr;

            if (!compile("VSMain", "vs_5_0", &vs_blob) || !compile("PSMain", "ps_5_0", &ps_blob))
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP world: a shader failed to compile, blocks can't be drawn\n")
                );

                return false;
            }

            device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_vs);
            device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_ps);

            const D3D11_INPUT_ELEMENT_DESC elements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };

            device->CreateInputLayout(elements, 5, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_layout);

            safe_release(vs_blob);
            safe_release(ps_blob);

            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth = sizeof(Constants);
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cbd, nullptr, &g_constants);

            // Minecraft's textures are pixel art: no filtering.
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &g_sampler);

            // No culling: swapping Minecraft's y and z flips every triangle's winding.
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            device->CreateRasterizerState(&rd, &g_raster);

            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device->CreateBlendState(&bd, &g_blend_off);

            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            device->CreateBlendState(&bd, &g_blend_alpha);

            // Night vision: picture * colour + picture, and picture + colour.
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            device->CreateBlendState(&bd, &g_blend_multiply);

            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            device->CreateBlendState(&bd, &g_blend_add);

            ID3DBlob* full_vs_blob = nullptr;
            ID3DBlob* full_ps_blob = nullptr;

            if (compile("VSFull", "vs_5_0", &full_vs_blob) && compile("PSFull", "ps_5_0", &full_ps_blob))
            {
                device->CreateVertexShader(full_vs_blob->GetBufferPointer(), full_vs_blob->GetBufferSize(), nullptr, &g_vs_full);
                device->CreatePixelShader(full_ps_blob->GetBufferPointer(), full_ps_blob->GetBufferSize(), nullptr, &g_ps_full);
            }

            safe_release(full_vs_blob);
            safe_release(full_ps_blob);

            D3D11_DEPTH_STENCIL_DESC no_depth{};
            device->CreateDepthStencilState(&no_depth, &g_depth_off);

            // Reversed depth: nearer is greater.
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = TRUE;
            dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            device->CreateDepthStencilState(&dd, &g_depth_write);

            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            device->CreateDepthStencilState(&dd, &g_depth_read);

            g_init_ok =
                g_vs && g_ps && g_layout && g_constants && g_sampler && g_raster &&
                g_blend_off && g_blend_alpha && g_depth_write && g_depth_read;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP world: block renderer {}\n"),
                g_init_ok ? STR("ready") : STR("failed to initialize")
            );

            return g_init_ok;
        }

        bool ensure_depth(ID3D11Device* device, UINT width, UINT height)
        {
            if (g_dsv != nullptr && g_depth_w == width && g_depth_h == height)
            {
                return true;
            }

            safe_release(g_dsv);
            safe_release(g_depth);

            D3D11_TEXTURE2D_DESC td{};
            td.Width = width;
            td.Height = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_D32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

            if (
                FAILED(device->CreateTexture2D(&td, nullptr, &g_depth)) ||
                FAILED(device->CreateDepthStencilView(g_depth, nullptr, &g_dsv))
            )
            {
                return false;
            }

            g_depth_w = width;
            g_depth_h = height;

            return true;
        }

        // Looks at a depth buffer the game is using this frame and remembers
        // it if it could be the main view's.
        void consider(ID3D11DepthStencilView* view)
        {
            if (view == nullptr || view == g_dsv || g_back_w == 0 || g_back_h == 0)
            {
                return;
            }

            for (int i = 0; i < g_seen_count; ++i)
            {
                if (g_seen[i] == view)
                {
                    return;
                }
            }

            if (g_seen_count < 32)
            {
                g_seen[g_seen_count++] = view;
            }

            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);

            if (resource == nullptr)
            {
                return;
            }

            ID3D11Texture2D* texture = nullptr;
            resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
            resource->Release();

            if (texture == nullptr)
            {
                return;
            }

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);

            // Shadow maps are square, and small buffers belong to cameras and
            // reflections. Of equally big ones the first is the scene's: its
            // depth pass is the first thing the game draws.
            const float shape = float(desc.Width) / float(desc.Height);
            const float wanted = float(g_back_w) / float(g_back_h);

            const bool fits =
                desc.SampleDesc.Count == 1 &&
                (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 &&
                shape > wanted * 0.97f &&
                shape < wanted * 1.03f &&
                desc.Width * 3 >= g_back_w &&
                desc.Width > g_candidate_w;

            bool reported = false;

            for (int i = 0; i < g_reported_count; ++i)
            {
                reported = reported || g_reported[i] == texture;
            }

            if (!reported && g_reported_count < 12)
            {
                g_reported[g_reported_count++] = texture;

                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP world: the game draws into a depth buffer of {}x{} (format {}, samples {}, bind {:#x}): {}\n"),
                    desc.Width,
                    desc.Height,
                    static_cast<int>(desc.Format),
                    desc.SampleDesc.Count,
                    desc.BindFlags,
                    fits ? STR("could be the scene") : STR("not the scene")
                );
            }

            if (fits)
            {
                safe_release(g_candidate);
                g_candidate = texture;
                g_candidate_w = desc.Width;
                g_candidate_h = desc.Height;
            }
            else
            {
                texture->Release();
            }
        }

        void STDMETHODCALLTYPE clear_depth_hook(
            ID3D11DeviceContext* context,
            ID3D11DepthStencilView* view,
            UINT flags,
            FLOAT depth,
            UINT8 stencil
        )
        {
            g_original_clear_depth(context, view, flags, depth, stencil);
            consider(view);
        }

        void STDMETHODCALLTYPE set_targets_hook(
            ID3D11DeviceContext* context,
            UINT count,
            ID3D11RenderTargetView* const* targets,
            ID3D11DepthStencilView* depth
        )
        {
            g_original_set_targets(context, count, targets, depth);
            consider(depth);
        }

        void STDMETHODCALLTYPE set_targets_uav_hook(
            ID3D11DeviceContext* context,
            UINT count,
            ID3D11RenderTargetView* const* targets,
            ID3D11DepthStencilView* depth,
            UINT uav_start,
            UINT uav_count,
            ID3D11UnorderedAccessView* const* uavs,
            const UINT* uav_counts
        )
        {
            g_original_set_targets_uav(context, count, targets, depth, uav_start, uav_count, uavs, uav_counts);
            consider(depth);
        }

        void patch_vtable(int index, void* target)
        {
            DWORD old_protect = 0;

            ::VirtualProtect(&g_context_vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect);
            g_context_vtable[index] = target;
            ::VirtualProtect(&g_context_vtable[index], sizeof(void*), old_protect, &old_protect);
        }

        // Makes this frame's candidate the depth buffer to test against.
        void adopt_scene_depth(ID3D11Device* device)
        {
            ID3D11Texture2D* texture = g_candidate;

            g_candidate = nullptr;
            g_candidate_w = 0;
            g_seen_count = 0;

            if (texture == nullptr)
            {
                // Nothing cleared this frame (paused?): what we had still holds the picture's depth.
                return;
            }

            if (texture == g_scene_depth)
            {
                texture->Release();
                return;
            }

            safe_release(g_scene_depth_srv);
            safe_release(g_scene_depth);

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);

            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;

            switch (desc.Format)
            {
            case DXGI_FORMAT_R24G8_TYPELESS: vd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
            case DXGI_FORMAT_R32G8X24_TYPELESS: vd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
            case DXGI_FORMAT_R32_TYPELESS: vd.Format = DXGI_FORMAT_R32_FLOAT; break;
            case DXGI_FORMAT_R16_TYPELESS: vd.Format = DXGI_FORMAT_R16_UNORM; break;
            default: vd.Format = DXGI_FORMAT_UNKNOWN; break;
            }

            if (
                vd.Format == DXGI_FORMAT_UNKNOWN ||
                FAILED(device->CreateShaderResourceView(texture, &vd, &g_scene_depth_srv))
            )
            {
                static bool logged = false;

                if (!logged)
                {
                    logged = true;

                    RC::Output::send<RC::LogLevel::Error>(
                        STR("SBMP world: can't read the game's depth buffer (format {}), walls won't hide blocks\n"),
                        static_cast<int>(desc.Format)
                    );
                }

                texture->Release();
                return;
            }

            g_scene_depth = texture;
            g_scene_depth_w = desc.Width;
            g_scene_depth_h = desc.Height;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP world: using the game's depth buffer, {}x{} (format {})\n"),
                desc.Width,
                desc.Height,
                static_cast<int>(desc.Format)
            );
        }

        void on_texture(ID3D11Device* device, const std::uint8_t* data, std::uint64_t bytes)
        {
            if (bytes < sizeof(proto::RenTexture))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenTexture*>(data);

            if (
                hdr->id == 0 ||
                hdr->width == 0 ||
                hdr->height == 0 ||
                bytes < sizeof(proto::RenTexture) + std::uint64_t(hdr->width) * hdr->height * 4
            )
            {
                return;
            }

            if (auto it = g_textures.find(hdr->id); it != g_textures.end())
            {
                safe_release(it->second);
                g_textures.erase(it);
            }

            D3D11_TEXTURE2D_DESC td{};
            td.Width = hdr->width;
            td.Height = hdr->height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA init{ data + sizeof(proto::RenTexture), hdr->width * 4, 0 };

            ID3D11Texture2D* texture = nullptr;
            ID3D11ShaderResourceView* view = nullptr;

            if (
                SUCCEEDED(device->CreateTexture2D(&td, &init, &texture)) &&
                SUCCEEDED(device->CreateShaderResourceView(texture, nullptr, &view))
            )
            {
                g_textures[hdr->id] = view;
            }

            safe_release(texture);
        }

        // Minecraft sends the whole scene again every frame; only the newest one is kept.
        void on_scene(ID3D11Device* device, ID3D11DeviceContext* context, const std::uint8_t* data, std::uint64_t bytes)
        {
            g_scene_batches.clear();
            g_scene_vertices = 0;

            if (bytes < sizeof(proto::RenScene))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenScene*>(data);

            const std::uint64_t batch_bytes = std::uint64_t(hdr->batchCount) * sizeof(proto::RenBatch);
            const std::uint64_t vertex_bytes = std::uint64_t(hdr->vertexCount) * sizeof(Vertex);

            if (
                hdr->batchCount == 0 ||
                hdr->vertexCount == 0 ||
                bytes < sizeof(proto::RenScene) + batch_bytes + vertex_bytes
            )
            {
                return;
            }

            if (g_scene_vb == nullptr || g_scene_vb_capacity < hdr->vertexCount)
            {
                safe_release(g_scene_vb);

                g_scene_vb_capacity = hdr->vertexCount + hdr->vertexCount / 2 + 1024;

                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = static_cast<UINT>(g_scene_vb_capacity * sizeof(Vertex));
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                if (FAILED(device->CreateBuffer(&bd, nullptr, &g_scene_vb)))
                {
                    g_scene_vb_capacity = 0;
                    return;
                }
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};

            if (FAILED(context->Map(g_scene_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                return;
            }

            std::memcpy(mapped.pData, data + sizeof(proto::RenScene) + batch_bytes, vertex_bytes);
            context->Unmap(g_scene_vb, 0);

            const auto* batches = reinterpret_cast<const proto::RenBatch*>(data + sizeof(proto::RenScene));

            for (std::uint32_t i = 0; i < hdr->batchCount; ++i)
            {
                const auto& batch = batches[i];

                if (batch.count == 0 || std::uint64_t(batch.first) + batch.count > hdr->vertexCount)
                {
                    continue;
                }

                g_scene_batches.push_back({ batch.texture, batch.first, batch.count, (batch.flags & 1) != 0 });
            }

            g_scene_vertices = hdr->vertexCount;
            g_scene_origin[0] = hdr->originX;
            g_scene_origin[1] = hdr->originY;
            g_scene_origin[2] = hdr->originZ;
        }

        void on_solids(const std::uint8_t* data, std::uint64_t bytes)
        {
            if (bytes < sizeof(proto::RenSolids))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenSolids*>(data);
            const auto key = section_key(hdr->sx, hdr->sy, hdr->sz);

            std::scoped_lock lock(g_solids_mutex);

            if (hdr->count == 0 || bytes < sizeof(proto::RenSolids) + 512)
            {
                g_solids.erase(key);
                return;
            }

            std::memcpy(g_solids[key].data(), data + sizeof(proto::RenSolids), 512);
        }

        void on_avatar(ID3D11Device* device, ID3D11DeviceContext* context, const std::uint8_t* data, std::uint64_t bytes)
        {
            g_avatar_batches.clear();

            if (bytes < sizeof(proto::RenAvatar))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenAvatar*>(data);

            const std::uint64_t batch_bytes = std::uint64_t(hdr->batchCount) * sizeof(proto::RenBatch);
            const std::uint64_t vertex_bytes = std::uint64_t(hdr->vertexCount) * sizeof(Vertex);

            // No batches: first person, the body isn't shown.
            if (
                hdr->batchCount == 0 ||
                hdr->vertexCount == 0 ||
                bytes < sizeof(proto::RenAvatar) + batch_bytes + vertex_bytes
            )
            {
                return;
            }

            if (g_avatar_vb == nullptr || g_avatar_vb_capacity < hdr->vertexCount)
            {
                safe_release(g_avatar_vb);

                g_avatar_vb_capacity = hdr->vertexCount + 1024;

                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = static_cast<UINT>(g_avatar_vb_capacity * sizeof(Vertex));
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                if (FAILED(device->CreateBuffer(&bd, nullptr, &g_avatar_vb)))
                {
                    g_avatar_vb_capacity = 0;
                    return;
                }
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};

            if (FAILED(context->Map(g_avatar_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                return;
            }

            std::memcpy(mapped.pData, data + sizeof(proto::RenAvatar) + batch_bytes, vertex_bytes);
            context->Unmap(g_avatar_vb, 0);

            const auto* batches = reinterpret_cast<const proto::RenBatch*>(data + sizeof(proto::RenAvatar));

            for (std::uint32_t i = 0; i < hdr->batchCount; ++i)
            {
                const auto& batch = batches[i];

                if (batch.count == 0 || std::uint64_t(batch.first) + batch.count > hdr->vertexCount)
                {
                    continue;
                }

                g_avatar_batches.push_back({ batch.texture, batch.first, batch.count, (batch.flags & 1) != 0 });
            }
        }

        // The lights of a section. Not used as light yet: only what burns is kept.
        void on_lights(const std::uint8_t* data, std::uint64_t bytes)
        {
            if (bytes < sizeof(proto::RenLights))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenLights*>(data);
            const auto key = section_key(hdr->sx, hdr->sy, hdr->sz);

            std::vector<std::uint16_t> burning;

            if (bytes >= sizeof(proto::RenLights) + std::uint64_t(hdr->count) * sizeof(proto::RenLight))
            {
                const auto* lights = reinterpret_cast<const proto::RenLight*>(data + sizeof(proto::RenLights));

                for (std::uint32_t i = 0; i < hdr->count; ++i)
                {
                    // The colour's top byte: light kind in the low bits, hazard in the high ones.
                    const std::uint32_t hazard = (lights[i].color >> 28) & 0xF;

                    if (hazard != proto::kHazardNone)
                    {
                        burning.push_back(static_cast<std::uint16_t>(
                            (lights[i].x & 15) | ((lights[i].y & 15) << 4) | ((lights[i].z & 15) << 8) | (hazard << 12)
                        ));
                    }
                }
            }

            std::scoped_lock lock(g_solids_mutex);

            if (burning.empty())
            {
                g_hazards.erase(key);
            }
            else
            {
                g_hazards[key] = std::move(burning);
            }
        }

        void on_shapes(const std::uint8_t* data, std::uint64_t bytes)
        {
            if (bytes < sizeof(proto::RenShapes))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenShapes*>(data);
            const auto key = section_key(hdr->sx, hdr->sy, hdr->sz);

            std::vector<std::array<float, 6>> boxes;

            if (bytes >= sizeof(proto::RenShapes) + std::uint64_t(hdr->count) * sizeof(proto::RenShape))
            {
                const auto* shapes = reinterpret_cast<const proto::RenShape*>(data + sizeof(proto::RenShapes));

                boxes.reserve(hdr->count);

                for (std::uint32_t i = 0; i < hdr->count; ++i)
                {
                    const auto& shape = shapes[i];
                    const float bx = float(hdr->sx * 16 + shape.x);
                    const float by = float(hdr->sy * 16 + shape.y);
                    const float bz = float(hdr->sz * 16 + shape.z);

                    boxes.push_back({
                        bx + shape.min[0] / 16.0f, by + shape.min[1] / 16.0f, bz + shape.min[2] / 16.0f,
                        bx + shape.max[0] / 16.0f, by + shape.max[1] / 16.0f, bz + shape.max[2] / 16.0f,
                    });
                }
            }

            static int reported = 0;

            if (reported < 5)
            {
                ++reported;

                RC::Output::send<RC::LogLevel::Verbose>(
                    STR("SBMP world: section ({}, {}, {}) has {} boxes of doors, fences and the like\n"),
                    hdr->sx,
                    hdr->sy,
                    hdr->sz,
                    boxes.size()
                );
            }

            std::scoped_lock lock(g_solids_mutex);

            if (boxes.empty())
            {
                g_shapes.erase(key);
            }
            else
            {
                g_shapes[key] = std::move(boxes);
            }
        }

        void on_atlas(ID3D11Device* device, const std::uint8_t* data, std::uint64_t bytes)
        {
            if (bytes < sizeof(proto::RenAtlas))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenAtlas*>(data);

            if (
                hdr->width == 0 ||
                hdr->height == 0 ||
                bytes < sizeof(proto::RenAtlas) + std::uint64_t(hdr->width) * hdr->height * 4
            )
            {
                return;
            }

            safe_release(g_atlas_srv);
            safe_release(g_atlas);

            D3D11_TEXTURE2D_DESC td{};
            td.Width = hdr->width;
            td.Height = hdr->height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA init{ data + sizeof(proto::RenAtlas), hdr->width * 4, 0 };

            if (
                FAILED(device->CreateTexture2D(&td, &init, &g_atlas)) ||
                FAILED(device->CreateShaderResourceView(g_atlas, nullptr, &g_atlas_srv))
            )
            {
                safe_release(g_atlas);
                return;
            }

            g_atlas_w = hdr->width;
            g_atlas_h = hdr->height;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP world: got Minecraft's texture atlas, {}x{}\n"),
                hdr->width,
                hdr->height
            );
        }

        // An animated texture's (water, lava, fire) current frame.
        void on_atlas_region(ID3D11DeviceContext* context, const std::uint8_t* data, std::uint64_t bytes)
        {
            if (g_atlas == nullptr || bytes < sizeof(proto::RenAtlasRegion))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenAtlasRegion*>(data);

            if (
                hdr->width == 0 ||
                hdr->height == 0 ||
                hdr->x + hdr->width > g_atlas_w ||
                hdr->y + hdr->height > g_atlas_h ||
                bytes < sizeof(proto::RenAtlasRegion) + std::uint64_t(hdr->width) * hdr->height * 4
            )
            {
                return;
            }

            const D3D11_BOX box{ hdr->x, hdr->y, 0, hdr->x + hdr->width, hdr->y + hdr->height, 1 };

            context->UpdateSubresource(
                g_atlas,
                0,
                &box,
                data + sizeof(proto::RenAtlasRegion),
                hdr->width * 4,
                0
            );
        }

        void on_section(ID3D11Device* device, const std::uint8_t* data, std::uint64_t bytes)
        {
            if (bytes < sizeof(proto::RenSection))
            {
                return;
            }

            const auto* hdr = reinterpret_cast<const proto::RenSection*>(data);
            const auto key = section_key(hdr->sx, hdr->sy, hdr->sz);

            if (auto it = g_sections.find(key); it != g_sections.end())
            {
                safe_release(it->second.vb);
                g_sections.erase(it);
            }

            const std::uint32_t count = hdr->vertexCount - hdr->vertexCount % 3;

            if (count == 0 || bytes < sizeof(proto::RenSection) + std::uint64_t(count) * sizeof(Vertex))
            {
                return;
            }

            // Opaque/cutout triangles first, translucent (water, stained glass) after.
            const auto* src = reinterpret_cast<const Vertex*>(data + sizeof(proto::RenSection));

            Section section{};

            g_scratch.clear();
            g_scratch.reserve(count);

            for (int pass = 0; pass < 2; ++pass)
            {
                for (std::uint32_t t = 0; t < count; t += 3)
                {
                    const bool translucent = (src[t].flags & kFlagTranslucent) != 0;

                    if (translucent == (pass == 1))
                    {
                        g_scratch.insert(g_scratch.end(), src + t, src + t + 3);
                    }
                }

                if (pass == 0)
                {
                    section.opaque = static_cast<std::uint32_t>(g_scratch.size());
                }
            }

            section.translucent = static_cast<std::uint32_t>(g_scratch.size()) - section.opaque;
            section.sx = hdr->sx;
            section.sy = hdr->sy;
            section.sz = hdr->sz;

            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = static_cast<UINT>(g_scratch.size() * sizeof(Vertex));
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

            D3D11_SUBRESOURCE_DATA init{ g_scratch.data(), 0, 0 };

            if (SUCCEEDED(device->CreateBuffer(&bd, &init, &section.vb)))
            {
                g_sections[key] = section;
            }
        }

        // Sets where the next draws' vertices are: a point in Minecraft
        // coordinates (blocks), turned into Unreal units relative to the
        // camera (in doubles: the map is far from the origin).
        bool set_origin(ID3D11DeviceContext* context, const Camera& camera, const Constants& frame, double x, double y, double z)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};

            if (FAILED(context->Map(g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                return false;
            }

            Constants constants = frame;

            constants.offset[0] = static_cast<float>(x * proto::kUnitsPerBlock - camera.position[0]);
            constants.offset[1] = static_cast<float>(z * proto::kUnitsPerBlock - camera.position[1]);
            constants.offset[2] = static_cast<float>(y * proto::kUnitsPerBlock - camera.position[2]);
            constants.offset[3] = kNearPlane;

            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(g_constants, 0);

            return true;
        }

        // ---- per-frame geometry: arrows, items, cracks, the outline (from SkyCraft's WorldRender) ----

        void quad(const float p[4][3], const float rect[4], std::uint32_t color = 0xFFFFFFFFu, std::uint32_t flags = kFlagCutout)
        {
            const float uv[4][2] = { { rect[0], rect[1] }, { rect[2], rect[1] }, { rect[2], rect[3] }, { rect[0], rect[3] } };

            for (int k : { 0, 1, 2, 0, 2, 3 })
            {
                g_dynamic.push_back({ p[k][0], p[k][1], p[k][2], uv[k][0], uv[k][1], color, kFullSkyLight, flags });
            }
        }

        // A box (turned `yaw` about its vertical centre line) with a texture for
        // the sides, the top and the bottom.
        void box(const float min[3], const float size[3], float yaw, const float side[4], const float top[4], const float bottom[4], std::uint32_t flags)
        {
            const float cx = min[0] + size[0] * 0.5f;
            const float cz = min[2] + size[2] * 0.5f;
            const float c = std::cos(yaw);
            const float s = std::sin(yaw);

            const auto corner = [&](int i, float out[3])
            {
                const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0];
                const float lz = ((i & 4) ? 0.5f : -0.5f) * size[2];

                out[0] = cx + lx * c - lz * s;
                out[1] = min[1] + ((i & 2) ? size[1] : 0.0f);
                out[2] = cz + lx * s + lz * c;
            };

            // corner bits: 1 = +x, 2 = +y, 4 = +z; each face listed TL, TR, BR, BL seen from outside
            static constexpr int kFaces[6][4] = {
                { 6, 7, 5, 4 },  // south (+z)
                { 3, 2, 0, 1 },  // north (-z)
                { 7, 3, 1, 5 },  // east (+x)
                { 2, 6, 4, 0 },  // west (-x)
                { 2, 3, 7, 6 },  // top
                { 4, 5, 1, 0 },  // bottom
            };

            for (int f = 0; f < 6; ++f)
            {
                float p[4][3];

                for (int k = 0; k < 4; ++k)
                {
                    corner(kFaces[f][k], p[k]);
                }

                quad(p, f == 4 ? top : f == 5 ? bottom : side, 0xFFFFFFFFu, flags | kFlagNormalFromFaces);
            }
        }

        // Minecraft's arrow (or a trident) at a position, flying along d (Minecraft axes, unit
        // length): two crossed fins and, for arrows, the back plate.
        void arrow(float px, float py, float pz, const float d[3], const float* uv_side, const float* uv_back, bool trident)
        {
            float s[3] = { d[2], 0.0f, -d[0] };
            float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);

            if (sl < 1e-3f)
            {
                s[0] = 1.0f;
                s[2] = 0.0f;
                sl = 1.0f;
            }

            s[0] /= sl;
            s[2] /= sl;

            const float u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
            constexpr float r = 0.70710678f;
            const float fins[2][3] = {
                { (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r },
                { (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r },
            };

            const auto at = [&](float along, const float* q, float side, const float* q2, float side2, float out[3])
            {
                for (int k = 0; k < 3; ++k)
                {
                    out[k] = (k == 0 ? px : k == 1 ? py : pz) + d[k] * along + q[k] * side + (q2 ? q2[k] * side2 : 0.0f);
                }
            };

            if (!trident)
            {
                // Minecraft's ArrowModel (1/16 block units, scaled 0.9): two fins 16 long and 4
                // wide from x -12 (fletching) to +4 (head), and a 4x4 back plate at x -11.
                constexpr float k = 0.9f / 16.0f;

                for (const auto& q : fins)
                {
                    float p[4][3];

                    at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
                    at(4 * k, q, -2 * k, nullptr, 0, p[1]);
                    at(4 * k, q, 2 * k, nullptr, 0, p[2]);
                    at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
                    quad(p, uv_side);
                }

                float p[4][3];

                at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
                at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
                at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
                at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
                quad(p, uv_back);
            }
            else
            {
                // Tridents: the item icon, whose diagonal runs handle (bottom-left) to tip (top-right).
                constexpr float h = 0.9f;

                for (const auto& q : fins)
                {
                    float p[4][3];

                    at(0, q, h, nullptr, 0, p[0]);
                    at(h, q, 0, nullptr, 0, p[1]);
                    at(0, q, -h, nullptr, 0, p[2]);
                    at(-h, q, 0, nullptr, 0, p[3]);
                    quad(p, uv_side);
                }
            }
        }

        // Builds this frame's entities and outline, relative to a whole block near the camera.
        void build_dynamic(ID3D11Device* device, ID3D11DeviceContext* context, const Camera& camera)
        {
            g_dynamic.clear();
            g_dynamic_solid = 0;
            g_dynamic_cracks = 0;
            g_dynamic_lines = 0;

            const auto& entities = g_entities;

            std::vector<std::array<double, 6>> ropes;

            {
                std::scoped_lock lock(g_player_mutex);

                ropes = g_ropes;
            }

            if (entities.count == 0 && entities.hasSelection == 0 && ropes.empty())
            {
                return;
            }

            const double o[3] = {
                std::floor(camera.position[0] / proto::kUnitsPerBlock),
                std::floor(camera.position[2] / proto::kUnitsPerBlock),
                std::floor(camera.position[1] / proto::kUnitsPerBlock),
            };

            std::memcpy(g_dynamic_origin, o, sizeof(o));

            std::vector<Vertex> cracks;
            const std::uint32_t count = entities.count < proto::kMaxWorldEntities ? entities.count : proto::kMaxWorldEntities;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto& e = entities.entities[i];
                const float px = float(e.x - o[0]);
                const float py = float(e.y - o[1]);
                const float pz = float(e.z - o[2]);

                if (e.kind == proto::kWeBlock)
                {
                    // A dropped block: a small cube spinning about its centre, like in Minecraft.
                    const float size = e.scale;
                    const float min[3] = { px - size * 0.5f, py - size * 0.5f, pz - size * 0.5f };
                    const float extent[3] = { size, size, size };

                    box(min, extent, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], kFlagCutout);
                }
                else if (e.kind == proto::kWeCrack)
                {
                    // Drawn after the solid things: move them to their own list.
                    const float min[3] = { px, py, pz };
                    const auto before = g_dynamic.size();

                    box(min, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], kFlagTranslucent);
                    cracks.insert(cracks.end(), g_dynamic.begin() + static_cast<std::ptrdiff_t>(before), g_dynamic.end());
                    g_dynamic.resize(before);
                }
                else if (e.kind == proto::kWeArrow || e.kind == proto::kWeTrident)
                {
                    // Minecraft arrows face (sin yaw, sin pitch, cos yaw).
                    const float yaw = e.yaw * kPi / 180.0f;
                    const float pitch = e.pitch * kPi / 180.0f;
                    const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };

                    arrow(px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
                }
                else if (e.kind == proto::kWeItem)
                {
                    // A flat sprite turning about the vertical, like a dropped item.
                    const float spin = e.yaw * kPi / 180.0f;
                    const float half = e.scale * 0.5f;
                    const float rx = std::cos(spin) * half;
                    const float rz = std::sin(spin) * half;
                    const float p[4][3] = {
                        { px - rx, py + half, pz - rz },
                        { px + rx, py + half, pz + rz },
                        { px + rx, py - half, pz + rz },
                        { px - rx, py - half, pz - rz },
                    };

                    quad(p, e.uv[0]);
                }
            }

            g_dynamic_solid = static_cast<std::uint32_t>(g_dynamic.size());
            g_dynamic.insert(g_dynamic.end(), cracks.begin(), cracks.end());
            g_dynamic_cracks = static_cast<std::uint32_t>(cracks.size());

            // Leads: a sagging rope from the player to each character on one.
            // Lines are a pixel wide, so each rope is a few of them side by side.
            for (const auto& rope : ropes)
            {
                constexpr int kPieces = 10;
                constexpr std::uint32_t kRope = 0xFF2B5A8Bu;  // Minecraft's lead brown

                // Unreal to blocks, in Minecraft's order of axes.
                const double from[3] = { rope[0] / proto::kUnitsPerBlock - o[0], rope[2] / proto::kUnitsPerBlock - o[1], rope[1] / proto::kUnitsPerBlock - o[2] };
                const double to[3] = { rope[3] / proto::kUnitsPerBlock - o[0], rope[5] / proto::kUnitsPerBlock - o[1], rope[4] / proto::kUnitsPerBlock - o[2] };
                const double span = std::sqrt(
                    (to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
                // A slack rope hangs; at full length (6 blocks) it is taut.
                const double sag = span < 6.0 ? (6.0 - span) * 0.2 : 0.0;

                static constexpr float kStrands[5][2] = { { 0.0f, 0.0f }, { 0.015f, 0.0f }, { -0.015f, 0.0f }, { 0.0f, 0.015f }, { 0.0f, -0.015f } };

                for (const auto& strand : kStrands)
                {
                    for (int piece = 0; piece < kPieces; ++piece)
                    {
                        for (int end = 0; end < 2; ++end)
                        {
                            const double t = static_cast<double>(piece + end) / kPieces;

                            g_dynamic.push_back({
                                static_cast<float>(from[0] + (to[0] - from[0]) * t) + strand[0],
                                static_cast<float>(from[1] + (to[1] - from[1]) * t - sag * 4.0 * t * (1.0 - t)) + strand[1],
                                static_cast<float>(from[2] + (to[2] - from[2]) * t) + strand[0],
                                0,
                                0,
                                kRope,
                                kFullSkyLight,
                                kFlagUntextured,
                            });

                            ++g_dynamic_lines;
                        }
                    }
                }
            }

            if (entities.hasSelection != 0)
            {
                // The outline of the block the crosshair is on: its twelve edges, a hair outside it.
                constexpr float g = 0.002f;
                const float lo[3] = { float(entities.selMin[0] - o[0]) - g, float(entities.selMin[1] - o[1]) - g, float(entities.selMin[2] - o[2]) - g };
                const float hi[3] = { float(entities.selMax[0] - o[0]) + g, float(entities.selMax[1] - o[1]) + g, float(entities.selMax[2] - o[2]) + g };

                static constexpr int kEdges[12][2] = {
                    { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                    { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
                };
                constexpr std::uint32_t kOutline = 0xB3000000u;  // black, a bit stronger than Minecraft's: the game is dark

                for (const auto& edge : kEdges)
                {
                    for (int k : edge)
                    {
                        g_dynamic.push_back({
                            (k & 1) ? hi[0] : lo[0],
                            (k & 2) ? hi[1] : lo[1],
                            (k & 4) ? hi[2] : lo[2],
                            0,
                            0,
                            kOutline,
                            kFullSkyLight,
                            kFlagUntextured | kFlagTranslucent,
                        });
                    }
                }

                g_dynamic_lines += 24;
            }

            if (g_dynamic.empty())
            {
                return;
            }

            const UINT wanted = static_cast<UINT>(g_dynamic.size());

            if (g_dynamic_vb == nullptr || g_dynamic_capacity < wanted)
            {
                safe_release(g_dynamic_vb);

                g_dynamic_capacity = wanted * 2 + 1024;

                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = static_cast<UINT>(g_dynamic_capacity * sizeof(Vertex));
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                if (FAILED(device->CreateBuffer(&bd, nullptr, &g_dynamic_vb)))
                {
                    g_dynamic_capacity = 0;
                    g_dynamic.clear();
                    return;
                }
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};

            if (FAILED(context->Map(g_dynamic_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                g_dynamic.clear();
                return;
            }

            std::memcpy(mapped.pData, g_dynamic.data(), g_dynamic.size() * sizeof(Vertex));
            context->Unmap(g_dynamic_vb, 0);
        }

        // Draws a stretch of the per-frame geometry (it all uses the block atlas).
        void draw_dynamic(ID3D11DeviceContext* context, const Camera& camera, const Constants& frame, std::uint32_t first, std::uint32_t count)
        {
            if (
                count == 0 ||
                g_dynamic.empty() ||
                !set_origin(context, camera, frame, g_dynamic_origin[0], g_dynamic_origin[1], g_dynamic_origin[2])
            )
            {
                return;
            }

            const UINT stride = sizeof(Vertex);
            const UINT offset = 0;

            context->IASetVertexBuffers(0, 1, &g_dynamic_vb, &stride, &offset);
            context->PSSetShaderResources(0, 1, &g_atlas_srv);
            context->Draw(count, first);
        }

        // Entities or the player's body: batches of one vertex buffer, each with its texture.
        void draw_batches(
            ID3D11DeviceContext* context,
            const Camera& camera,
            const Constants& frame,
            bool translucent,
            const std::vector<Batch>& batches,
            ID3D11Buffer* buffer,
            const double origin[3]
        )
        {
            if (batches.empty() || buffer == nullptr || !set_origin(context, camera, frame, origin[0], origin[1], origin[2]))
            {
                return;
            }

            const UINT stride = sizeof(Vertex);
            const UINT offset = 0;

            context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);

            for (const auto& batch : batches)
            {
                if (batch.translucent != translucent)
                {
                    continue;
                }

                ID3D11ShaderResourceView* texture = g_atlas_srv;

                if (batch.texture != 0)
                {
                    const auto it = g_textures.find(batch.texture);

                    if (it == g_textures.end())
                    {
                        continue;
                    }

                    texture = it->second;
                }

                context->PSSetShaderResources(0, 1, &texture);
                context->Draw(batch.count, batch.first);
            }
        }

        void draw_pass(ID3D11DeviceContext* context, const Camera& camera, const Constants& frame, bool translucent)
        {
            const UINT stride = sizeof(Vertex);
            const UINT offset = 0;

            context->PSSetShaderResources(0, 1, &g_atlas_srv);

            for (const auto& [key, section] : g_sections)
            {
                const std::uint32_t count = translucent ? section.translucent : section.opaque;

                if (count == 0 || !set_origin(context, camera, frame, section.sx * 16.0, section.sy * 16.0, section.sz * 16.0))
                {
                    continue;
                }

                context->IASetVertexBuffers(0, 1, &section.vb, &stride, &offset);
                context->Draw(count, translucent ? section.opaque : 0);
            }

            double player[3]{};

            {
                std::scoped_lock lock(g_player_mutex);

                std::memcpy(player, g_player_position, sizeof(player));
            }

            draw_batches(context, camera, frame, translucent, g_scene_batches, g_scene_vb, g_scene_origin);
            draw_batches(context, camera, frame, translucent, g_avatar_batches, g_avatar_vb, player);
        }
    }

    namespace
    {
        // Night vision for the game's own picture. It is already finished and
        // tone-mapped, so this is a lift of what is there, not real light.
        void brighten(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width, UINT height)
        {
            if (g_vs_full == nullptr || g_ps_full == nullptr || g_blend_multiply == nullptr || g_blend_add == nullptr)
            {
                return;
            }

            const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
            const float factor[4]{};

            context->OMSetRenderTargets(1, &target, nullptr);
            context->OMSetDepthStencilState(g_depth_off, 0);
            context->RSSetState(g_raster);
            context->RSSetViewports(1, &vp);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(nullptr);
            context->VSSetShader(g_vs_full, nullptr, 0);
            context->PSSetShader(g_ps_full, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &g_constants);

            const auto pass = [&](ID3D11BlendState* blend, float r, float g, float b)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};

                if (FAILED(context->Map(g_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    return;
                }

                Constants constants{};

                constants.offset[0] = r;
                constants.offset[1] = g;
                constants.offset[2] = b;

                std::memcpy(mapped.pData, &constants, sizeof(constants));
                context->Unmap(g_constants, 0);

                context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
                context->Draw(3, 0);
            };

            // Four times as bright, then a floor under the pure black.
            pass(g_blend_multiply, 3.0f, 3.0f, 3.0f);
            pass(g_blend_add, 0.035f, 0.045f, 0.035f);
        }
    }

    void set_player_position(double x, double y, double z)
    {
        std::scoped_lock lock(g_player_mutex);

        g_player_position[0] = x;
        g_player_position[1] = y;
        g_player_position[2] = z;
    }

    void set_ropes(const std::vector<std::array<double, 6>>& ropes)
    {
        std::scoped_lock lock(g_player_mutex);

        g_ropes = ropes;
    }

    void set_night_vision(bool on)
    {
        g_night_vision.store(on, std::memory_order_relaxed);
    }

    void set_camera(const Camera& camera)
    {
        std::scoped_lock lock(g_camera_mutex);

        g_camera = camera;
    }

    void consume(SecurityBreachLink& link, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        auto* ring = link.base() + proto::kOffRenderRing;

        auto& head_ref = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingHeadOff);
        auto& tail_ref = *reinterpret_cast<std::uint64_t*>(ring + proto::kRenRingTailOff);

        const auto head = std::atomic_ref<std::uint64_t>(head_ref).load(std::memory_order_acquire);
        auto tail = std::atomic_ref<std::uint64_t>(tail_ref).load(std::memory_order_relaxed);

        const auto* ring_data = ring + proto::kRenRingDataOff;

        while (tail < head)
        {
            const auto pos = tail % proto::kRenRingDataBytes;

            proto::ColMsgHeader msg{};  // same {type, payloadBytes} header as the collision ring
            std::memcpy(&msg, ring_data + pos, sizeof(msg));

            if (msg.type == proto::kRenPad)
            {
                // The writer didn't have room for its message at the end: it continues at the start.
                tail += proto::kRenRingDataBytes - pos;
                continue;
            }

            const auto* payload = ring_data + pos + sizeof(msg);

            // A header that can't be right means we lost our place. Start over at the head.
            if (pos + sizeof(msg) + msg.payloadBytes > proto::kRenRingDataBytes)
            {
                tail = head;
                break;
            }

            switch (msg.type)
            {
            case proto::kRenAtlas:
                on_atlas(device, payload, msg.payloadBytes);
                break;

            case proto::kRenAtlasRegion:
                on_atlas_region(context, payload, msg.payloadBytes);
                break;

            case proto::kRenSection:
                on_section(device, payload, msg.payloadBytes);
                break;

            case proto::kRenClearAll:
                clear_sections();
                {
                    std::scoped_lock lock(g_solids_mutex);

                    g_solids.clear();
                    g_hazards.clear();
                    g_shapes.clear();
                }
                break;

            case proto::kRenLights:
                on_lights(payload, msg.payloadBytes);
                break;

            case proto::kRenShapes:
                on_shapes(payload, msg.payloadBytes);
                break;

            case proto::kRenSolids:
                on_solids(payload, msg.payloadBytes);
                break;

            case proto::kRenTexture:
                on_texture(device, payload, msg.payloadBytes);
                break;

            case proto::kRenScene:
                on_scene(device, context, payload, msg.payloadBytes);
                break;

            case proto::kRenAvatar:
                on_avatar(device, context, payload, msg.payloadBytes);
                break;

            default:
                // The player's own body, lights: not drawn yet.
                break;
            }

            tail += (sizeof(msg) + msg.payloadBytes + 7) & ~std::uint64_t(7);
        }

        std::atomic_ref<std::uint64_t>(tail_ref).store(tail, std::memory_order_release);

        // Arrows, items and the outline are a state block, not messages: take this frame's.
        auto* entities = reinterpret_cast<proto::WorldEntities*>(link.base() + proto::kOffWorldEntities);
        std::atomic_ref<std::uint32_t> seq(entities->seq);

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto before = seq.load(std::memory_order_acquire);

            if (before & 1)
            {
                continue;
            }

            std::memcpy(&g_entities, entities, sizeof(g_entities));

            if (seq.load(std::memory_order_acquire) == before)
            {
                break;
            }

            // Torn by a write: draw none rather than garbage.
            g_entities.count = 0;
            g_entities.hasSelection = 0;
        }
    }

    void draw(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* target,
        UINT width,
        UINT height
    )
    {
        g_back_w = width;
        g_back_h = height;

        if (!init(device))
        {
            return;
        }

        adopt_scene_depth(device);

        const bool night_vision = g_night_vision.load(std::memory_order_relaxed);

        if (night_vision)
        {
            brighten(context, target, width, height);
        }

        if (
            (
                g_sections.empty() && g_scene_batches.empty() && g_avatar_batches.empty() &&
                g_entities.count == 0 && g_entities.hasSelection == 0
            ) ||
            g_atlas_srv == nullptr ||
            !ensure_depth(device, width, height)
        )
        {
            return;
        }

        Camera camera;

        {
            std::scoped_lock lock(g_camera_mutex);

            camera = g_camera;
        }

        if (!camera.valid || camera.tan_half_fov <= 0.0f || height == 0)
        {
            return;
        }

        build_dynamic(device, context, camera);

        const float aspect = float(width) / float(height);

        Constants frame{};

        for (int i = 0; i < 3; ++i)
        {
            frame.row_x[i] = camera.right[i] / camera.tan_half_fov;
            frame.row_y[i] = camera.up[i] * aspect / camera.tan_half_fov;
            frame.row_w[i] = camera.forward[i];
        }

        if (g_scene_depth_srv != nullptr)
        {
            frame.depth[0] = float(g_scene_depth_w) / float(width);
            frame.depth[1] = float(g_scene_depth_h) / float(height);
            frame.depth[2] = 1.0f;
        }

        frame.depth[3] = night_vision ? 1.0f : 0.0f;

        const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
        const float factor[4]{};

        context->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
        context->OMSetRenderTargets(1, &target, g_dsv);
        context->RSSetState(g_raster);
        context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(g_layout);
        context->VSSetShader(g_vs, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &g_constants);
        context->PSSetShader(g_ps, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &g_constants);
        context->PSSetShaderResources(1, 1, &g_scene_depth_srv);
        context->PSSetSamplers(0, 1, &g_sampler);

        context->OMSetBlendState(g_blend_off, factor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(g_depth_write, 0);
        draw_pass(context, camera, frame, false);
        draw_dynamic(context, camera, frame, 0, g_dynamic_solid);

        context->OMSetBlendState(g_blend_alpha, factor, 0xFFFFFFFF);
        context->OMSetDepthStencilState(g_depth_read, 0);
        draw_pass(context, camera, frame, true);
        draw_dynamic(context, camera, frame, g_dynamic_solid, g_dynamic_cracks);

        if (g_dynamic_lines != 0)
        {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            draw_dynamic(context, camera, frame, g_dynamic_solid + g_dynamic_cracks, g_dynamic_lines);
        }

        // The game binds its depth buffer for writing again next frame.
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(1, 1, &none);
    }

    bool solid_at(int x, int y, int z)
    {
        std::scoped_lock lock(g_solids_mutex);

        const auto it = g_solids.find(section_key(x >> 4, y >> 4, z >> 4));

        if (it == g_solids.end())
        {
            return false;
        }

        const int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);

        return (it->second[bit >> 3] >> (bit & 7)) & 1;
    }

    int hazard_at(int x, int y, int z)
    {
        std::scoped_lock lock(g_solids_mutex);

        const auto it = g_hazards.find(section_key(x >> 4, y >> 4, z >> 4));

        if (it == g_hazards.end())
        {
            return 0;
        }

        const std::uint16_t cell = static_cast<std::uint16_t>((x & 15) | ((y & 15) << 4) | ((z & 15) << 8));

        for (const auto packed : it->second)
        {
            if ((packed & 0x0FFF) == cell)
            {
                return packed >> 12;
            }
        }

        return 0;
    }

    void shapes_in(const double low[3], const double high[3], std::vector<std::array<float, 6>>& out)
    {
        std::scoped_lock lock(g_solids_mutex);

        if (g_shapes.empty())
        {
            return;
        }

        const int sx0 = static_cast<int>(std::floor(low[0])) >> 4, sx1 = static_cast<int>(std::floor(high[0])) >> 4;
        const int sy0 = static_cast<int>(std::floor(low[1])) >> 4, sy1 = static_cast<int>(std::floor(high[1])) >> 4;
        const int sz0 = static_cast<int>(std::floor(low[2])) >> 4, sz1 = static_cast<int>(std::floor(high[2])) >> 4;

        for (int sy = sy0; sy <= sy1; ++sy)
        {
            for (int sz = sz0; sz <= sz1; ++sz)
            {
                for (int sx = sx0; sx <= sx1; ++sx)
                {
                    const auto it = g_shapes.find(section_key(sx, sy, sz));

                    if (it == g_shapes.end())
                    {
                        continue;
                    }

                    for (const auto& box : it->second)
                    {
                        if (
                            box[0] < high[0] && box[3] > low[0] &&
                            box[1] < high[1] && box[4] > low[1] &&
                            box[2] < high[2] && box[5] > low[2]
                        )
                        {
                            out.push_back(box);
                        }
                    }
                }
            }
        }
    }

    void shutdown()
    {
        if (g_context_vtable != nullptr)
        {
            patch_vtable(kClearDepthIndex, reinterpret_cast<void*>(g_original_clear_depth));
            patch_vtable(kSetTargetsIndex, reinterpret_cast<void*>(g_original_set_targets));
            patch_vtable(kSetTargetsUavIndex, reinterpret_cast<void*>(g_original_set_targets_uav));
            g_context_vtable = nullptr;
        }
    }
}
