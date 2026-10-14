// Ported from SkyCraft's skse/src/Overlay.cpp (Skyrim is D3D11 too). What differs: the swap chain
// is found through a throwaway one instead of asking the game.
#include "Overlay.h"
#include "World.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cstring>

namespace overlay
{
    namespace
    {
        using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

        // IDXGISwapChain::Present in the vtable.
        constexpr int kPresentIndex = 8;

        State g_state;
        SecurityBreachLink* g_link = nullptr;
        void** g_vtable = nullptr;
        PresentFn g_original_present = nullptr;

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        UINT tex_w = 0;
        UINT tex_h = 0;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11PixelShader* ps_invert = nullptr;
        ID3D11PixelShader* ps_plain = nullptr;
        ID3D11BlendState* blend = nullptr;
        ID3D11BlendState* invert_blend = nullptr;
        ID3D11SamplerState* sampler = nullptr;
        ID3D11RasterizerState* raster = nullptr;
        ID3D11DepthStencilState* depth = nullptr;
        ID3D11Buffer* params = nullptr;
        bool have_frame = false;
        bool flip_y = true;
        bool init_failed = false;

        struct alignas(16) Params
        {
            float cursor[2];
            float cursor_on;
            float flip_y;
            float invert_rect[4];  // back buffer pixels: x0, y0, x1, y1 (empty: none)
        };

        constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float cursorOn; float flipY; float4 invertRect; };
Texture2D overlay : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
    if (flipY > 0.5) uv.y = 1 - uv.y;
    return overlay.Sample(samp, uv);   // premultiplied alpha straight from Minecraft
}
// Minecraft's crosshair and attack indicator: drawn with Minecraft's invert blend (out = src * (1 - dst)
// + dst * (1 - src)) against the game's picture, in a pass of their own; left out of the main one.
float4 PSInvert(VSOut i) : SV_Target {
    if (!InInvertRect(i.pos.xy)) discard;
    return float4(Overlay(i.uv).rgb, 0);
}
// A picture as it is (Freddy filmed by the game), into the viewport it is drawn in.
float4 PSPlain(VSOut i) : SV_Target {
    return float4(overlay.Sample(samp, i.uv).rgb, 1);
}
float4 PSMain(VSOut i) : SV_Target {
    if (InInvertRect(i.pos.xy)) return 0;
    float4 c = Overlay(i.uv);
    if (cursorOn > 0.5) {
        float2 p = i.pos.xy - cursor;
        if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
            bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
            c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
        }
    }
    return c;
}
)";

        template <class T>
        void safe_release(T*& ptr)
        {
            if (ptr != nullptr)
            {
                ptr->Release();
                ptr = nullptr;
            }
        }

        bool compile(const char* entry, const char* target, ID3DBlob** out)
        {
            ID3DBlob* errors = nullptr;

            const auto hr = D3DCompile(
                kShader,
                sizeof(kShader) - 1,
                "sbmp_overlay",
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

            if (FAILED(hr))
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP overlay: a shader failed to compile\n")
                );
            }

            return SUCCEEDED(hr);
        }

        bool init_resources(IDXGISwapChain* swap_chain)
        {
            if (device != nullptr)
            {
                return true;
            }

            if (init_failed)
            {
                return false;
            }

            if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))))
            {
                init_failed = true;

                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP overlay: the game is not rendering with Direct3D 11 (DX12?), Minecraft can't be drawn\n")
                );

                return false;
            }

            device->GetImmediateContext(&context);

            ID3DBlob* vs_blob = nullptr;
            ID3DBlob* ps_blob = nullptr;
            ID3DBlob* ps_invert_blob = nullptr;

            if (
                !compile("VSMain", "vs_5_0", &vs_blob) ||
                !compile("PSMain", "ps_5_0", &ps_blob) ||
                !compile("PSInvert", "ps_5_0", &ps_invert_blob)
            )
            {
                init_failed = true;
                return false;
            }

            device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
            device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);
            device->CreatePixelShader(ps_invert_blob->GetBufferPointer(), ps_invert_blob->GetBufferSize(), nullptr, &ps_invert);

            safe_release(vs_blob);
            safe_release(ps_blob);
            safe_release(ps_invert_blob);

            // Optional: a picture of the game in a box (Freddy in his mule screen).
            ID3DBlob* ps_plain_blob = nullptr;

            if (compile("PSPlain", "ps_5_0", &ps_plain_blob))
            {
                device->CreatePixelShader(ps_plain_blob->GetBufferPointer(), ps_plain_blob->GetBufferSize(), nullptr, &ps_plain);
            }

            safe_release(ps_plain_blob);

            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device->CreateBlendState(&bd, &blend);

            // Minecraft's BlendFunction.INVERT; the back buffer's alpha is left alone.
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            device->CreateBlendState(&bd, &invert_blend);

            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &sampler);

            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            device->CreateRasterizerState(&rd, &raster);

            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = FALSE;
            dd.StencilEnable = FALSE;
            device->CreateDepthStencilState(&dd, &depth);

            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth = sizeof(Params);
            cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cbd, nullptr, &params);

            const bool ok = vs && ps && ps_invert && blend && invert_blend && sampler && raster && depth && params;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP overlay: renderer {}\n"),
                ok ? STR("ready") : STR("failed to initialize")
            );

            init_failed = !ok;

            return ok;
        }

        bool ensure_texture(UINT w, UINT h)
        {
            if (texture != nullptr && tex_w == w && tex_h == h)
            {
                return true;
            }

            safe_release(srv);
            safe_release(texture);

            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (
                FAILED(device->CreateTexture2D(&td, nullptr, &texture)) ||
                FAILED(device->CreateShaderResourceView(texture, nullptr, &srv))
            )
            {
                RC::Output::send<RC::LogLevel::Error>(
                    STR("SBMP overlay: texture {}x{} creation failed\n"),
                    w,
                    h
                );

                return false;
            }

            tex_w = w;
            tex_h = h;

            RC::Output::send<RC::LogLevel::Default>(
                STR("SBMP overlay: Minecraft's picture is {}x{}\n"),
                w,
                h
            );

            return true;
        }

        void upload_latest_frame()
        {
            if (!g_link->acquire_overlay_frame())
            {
                return;
            }

            const auto* hdr = g_link->front_header();

            if (
                hdr->width == 0 ||
                hdr->height == 0 ||
                hdr->width > skycraft::proto::kMaxOverlayW ||
                hdr->height > skycraft::proto::kMaxOverlayH
            )
            {
                return;
            }

            if (!ensure_texture(hdr->width, hdr->height))
            {
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};

            if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                return;
            }

            const auto* src = g_link->front_pixels();
            const auto row_bytes = hdr->width * 4;
            auto* dst = static_cast<std::uint8_t*>(mapped.pData);

            if (mapped.RowPitch == row_bytes)
            {
                std::memcpy(dst, src, std::size_t(row_bytes) * hdr->height);
            }
            else
            {
                for (UINT y = 0; y < hdr->height; ++y)
                {
                    std::memcpy(
                        dst + std::size_t(y) * mapped.RowPitch,
                        src + std::size_t(y) * row_bytes,
                        row_bytes
                    );
                }
            }

            context->Unmap(texture, 0);

            flip_y = (hdr->flags & 1) != 0;
            have_frame = true;
        }

        // UE4SS can open a window with a swap chain of its own in this process.
        bool is_game_window(HWND window)
        {
            wchar_t name[64]{};

            ::GetClassNameW(window, name, 64);

            return std::wcscmp(name, L"UnrealWindow") == 0;
        }

        void draw_overlay(IDXGISwapChain* swap_chain)
        {
            DXGI_SWAP_CHAIN_DESC desc{};

            if (FAILED(swap_chain->GetDesc(&desc)) || !is_game_window(desc.OutputWindow))
            {
                return;
            }

            g_state.window.store(desc.OutputWindow, std::memory_order_relaxed);
            g_state.viewport_w.store(static_cast<int>(desc.BufferDesc.Width), std::memory_order_relaxed);
            g_state.viewport_h.store(static_cast<int>(desc.BufferDesc.Height), std::memory_order_relaxed);

            if (!g_link->valid() || !init_resources(swap_chain))
            {
                return;
            }

            // Keep taking what Minecraft sends while hidden: the picture is
            // then current the moment it shows, and Minecraft never waits on us.
            world::consume(*g_link, device, context);
            upload_latest_frame();

            if (!g_state.show.load(std::memory_order_relaxed))
            {
                return;
            }

            ID3D11Texture2D* back_buffer = nullptr;

            if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))))
            {
                return;
            }

            ID3D11RenderTargetView* rtv = nullptr;
            const auto hr = device->CreateRenderTargetView(back_buffer, nullptr, &rtv);
            D3D11_TEXTURE2D_DESC bb_desc{};
            back_buffer->GetDesc(&bb_desc);
            safe_release(back_buffer);

            if (FAILED(hr))
            {
                static bool logged = false;

                if (!logged)
                {
                    logged = true;

                    RC::Output::send<RC::LogLevel::Error>(
                        STR("SBMP overlay: can't draw to the back buffer (format {})\n"),
                        static_cast<int>(bb_desc.Format)
                    );
                }

                return;
            }

            // Save the pipeline state we touch: Unreal caches what it has set.
            ID3D11RenderTargetView* old_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* old_dsv = nullptr;
            ID3D11BlendState* old_blend = nullptr;
            float old_factor[4]{};
            UINT old_mask = 0;
            ID3D11RasterizerState* old_raster = nullptr;
            ID3D11DepthStencilState* old_depth = nullptr;
            UINT old_stencil = 0;
            D3D11_VIEWPORT old_vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT old_vp_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            D3D11_PRIMITIVE_TOPOLOGY old_topo{};
            ID3D11InputLayout* old_layout = nullptr;
            ID3D11VertexShader* old_vs = nullptr;
            ID3D11PixelShader* old_ps = nullptr;
            ID3D11ShaderResourceView* old_srv = nullptr;
            ID3D11ShaderResourceView* old_srv1 = nullptr;
            ID3D11SamplerState* old_sampler = nullptr;
            ID3D11Buffer* old_cb = nullptr;
            ID3D11Buffer* old_vs_cb = nullptr;
            ID3D11Buffer* old_vb = nullptr;
            UINT old_vb_stride = 0;
            UINT old_vb_offset = 0;

            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, &old_dsv);
            context->OMGetBlendState(&old_blend, old_factor, &old_mask);
            context->RSGetState(&old_raster);
            context->OMGetDepthStencilState(&old_depth, &old_stencil);
            context->RSGetViewports(&old_vp_count, old_vps);
            context->IAGetPrimitiveTopology(&old_topo);
            context->IAGetInputLayout(&old_layout);
            context->VSGetShader(&old_vs, nullptr, nullptr);
            context->PSGetShader(&old_ps, nullptr, nullptr);
            context->PSGetShaderResources(0, 1, &old_srv);
            context->PSGetShaderResources(1, 1, &old_srv1);
            context->PSGetSamplers(0, 1, &old_sampler);
            context->PSGetConstantBuffers(0, 1, &old_cb);
            context->VSGetConstantBuffers(0, 1, &old_vs_cb);
            context->IAGetVertexBuffers(0, 1, &old_vb, &old_vb_stride, &old_vb_offset);

            // Minecraft's blocks go under its hand and HUD.
            world::draw(device, context, rtv, bb_desc.Width, bb_desc.Height);

            if (have_frame)
            {
                bool invert = false;
                D3D11_MAPPED_SUBRESOURCE mapped{};

                if (SUCCEEDED(context->Map(params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    auto* p = static_cast<Params*>(mapped.pData);

                    p->flip_y = flip_y ? 1.0f : 0.0f;
                    p->cursor_on = g_state.cursor_on.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
                    p->cursor[0] = float(g_state.cursor_x.load(std::memory_order_relaxed));
                    p->cursor[1] = float(g_state.cursor_y.load(std::memory_order_relaxed));

                    // Around the screen centre, where Minecraft puts the crosshair (15 GUI pixels) and the
                    // attack indicator under it (16 x 16 from 9 GUI pixels below the centre). GUI pixels
                    // are overlay pixels; scale if the overlay and back buffer differ.
                    const int scale = g_state.gui_scale.load(std::memory_order_relaxed);
                    const float sx = tex_w ? float(bb_desc.Width) / float(tex_w) : 1.0f;
                    const float g = float(scale) * sx;
                    const float cx = float(bb_desc.Width) * 0.5f;
                    const float cy = float(bb_desc.Height) * 0.5f;

                    invert = g_state.crosshair.load(std::memory_order_relaxed) && scale > 0;

                    p->invert_rect[0] = invert ? cx - 12.0f * g : 0.0f;
                    p->invert_rect[1] = invert ? cy - 12.0f * g : 0.0f;
                    p->invert_rect[2] = invert ? cx + 12.0f * g : 0.0f;
                    p->invert_rect[3] = invert ? cy + 28.0f * g : 0.0f;

                    context->Unmap(params, 0);
                }

                D3D11_VIEWPORT vp{ 0, 0, float(bb_desc.Width), float(bb_desc.Height), 0, 1 };
                const float factor[4]{};

                context->OMSetRenderTargets(1, &rtv, nullptr);
                context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
                context->OMSetDepthStencilState(depth, 0);
                context->RSSetState(raster);
                context->RSSetViewports(1, &vp);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->IASetInputLayout(nullptr);
                context->VSSetShader(vs, nullptr, 0);
                context->PSSetShader(ps, nullptr, 0);
                context->PSSetShaderResources(0, 1, &srv);
                context->PSSetSamplers(0, 1, &sampler);
                context->PSSetConstantBuffers(0, 1, &params);
                context->Draw(3, 0);

                if (invert)
                {
                    context->OMSetBlendState(invert_blend, factor, 0xFFFFFFFF);
                    context->PSSetShader(ps_invert, nullptr, 0);
                    context->Draw(3, 0);
                }

                // Freddy in his mule screen's preview box, as the game films him.
                auto* picture = g_state.preview_on.load(std::memory_order_relaxed) ? world::capture_view(device) : nullptr;

                if (picture != nullptr && ps_plain != nullptr)
                {
                    const int scale = g_state.gui_scale.load(std::memory_order_relaxed);
                    const float g = float(scale) * (tex_w ? float(bb_desc.Width) / float(tex_w) : 1.0f);
                    const D3D11_VIEWPORT box{
                        float(g_state.preview_x.load(std::memory_order_relaxed)) * g,
                        float(g_state.preview_y.load(std::memory_order_relaxed)) * g,
                        float(g_state.preview_w.load(std::memory_order_relaxed)) * g,
                        float(g_state.preview_h.load(std::memory_order_relaxed)) * g,
                        0,
                        1
                    };

                    if (scale > 0 && box.Width > 1.0f && box.Height > 1.0f)
                    {
                        context->RSSetViewports(1, &box);
                        context->OMSetBlendState(nullptr, factor, 0xFFFFFFFF);
                        context->PSSetShader(ps_plain, nullptr, 0);
                        context->PSSetShaderResources(0, 1, &picture);
                        context->Draw(3, 0);
                    }
                }
            }

            // Restore.
            context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, old_dsv);
            context->OMSetBlendState(old_blend, old_factor, old_mask);
            context->OMSetDepthStencilState(old_depth, old_stencil);
            context->RSSetState(old_raster);
            context->RSSetViewports(old_vp_count, old_vps);
            context->IASetPrimitiveTopology(old_topo);
            context->IASetInputLayout(old_layout);
            context->VSSetShader(old_vs, nullptr, 0);
            context->PSSetShader(old_ps, nullptr, 0);
            context->PSSetShaderResources(0, 1, &old_srv);
            context->PSSetShaderResources(1, 1, &old_srv1);
            context->PSSetSamplers(0, 1, &old_sampler);
            context->PSSetConstantBuffers(0, 1, &old_cb);
            context->VSSetConstantBuffers(0, 1, &old_vs_cb);
            context->IASetVertexBuffers(0, 1, &old_vb, &old_vb_stride, &old_vb_offset);

            for (auto*& r : old_rtv)
            {
                safe_release(r);
            }

            safe_release(old_dsv);
            safe_release(old_blend);
            safe_release(old_raster);
            safe_release(old_depth);
            safe_release(old_layout);
            safe_release(old_vs);
            safe_release(old_ps);
            safe_release(old_srv);
            safe_release(old_srv1);
            safe_release(old_sampler);
            safe_release(old_cb);
            safe_release(old_vs_cb);
            safe_release(old_vb);
            safe_release(rtv);
        }

        HRESULT WINAPI present_hook(IDXGISwapChain* swap_chain, UINT sync, UINT flags)
        {
            try
            {
                draw_overlay(swap_chain);
            }
            catch (...)
            {
            }

            return g_original_present(swap_chain, sync, flags);
        }

        void patch_present(void* target)
        {
            DWORD old_protect = 0;

            ::VirtualProtect(&g_vtable[kPresentIndex], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect);
            g_vtable[kPresentIndex] = target;
            ::VirtualProtect(&g_vtable[kPresentIndex], sizeof(void*), old_protect, &old_protect);
        }
    }

    State& state()
    {
        return g_state;
    }

    bool install(SecurityBreachLink& link)
    {
        if (g_vtable != nullptr)
        {
            return true;
        }

        // Every DXGI swap chain shares one vtable, so a throwaway swap chain
        // on a hidden window gives us the game's Present too.
        HWND window = ::CreateWindowExW(
            0,
            L"STATIC",
            L"",
            WS_OVERLAPPED,
            0,
            0,
            16,
            16,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        );

        if (window == nullptr)
        {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = 16;
        desc.BufferDesc.Height = 16;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = window;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;

        IDXGISwapChain* swap_chain = nullptr;
        ID3D11Device* dummy_device = nullptr;
        ID3D11DeviceContext* dummy_context = nullptr;

        const auto hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &desc,
            &swap_chain,
            &dummy_device,
            nullptr,
            &dummy_context
        );

        if (SUCCEEDED(hr))
        {
            g_link = &link;
            g_vtable = *reinterpret_cast<void***>(swap_chain);
            g_original_present = reinterpret_cast<PresentFn>(g_vtable[kPresentIndex]);

            patch_present(reinterpret_cast<void*>(&present_hook));
        }

        safe_release(swap_chain);
        safe_release(dummy_context);
        safe_release(dummy_device);
        ::DestroyWindow(window);

        if (FAILED(hr))
        {
            RC::Output::send<RC::LogLevel::Error>(
                STR("SBMP overlay: could not create a Direct3D 11 swap chain ({:#x}), Minecraft can't be drawn\n"),
                static_cast<unsigned long>(hr)
            );

            return false;
        }

        RC::Output::send<RC::LogLevel::Default>(
            STR("SBMP overlay: Present hooked\n")
        );

        return true;
    }

    void uninstall()
    {
        if (g_vtable == nullptr)
        {
            return;
        }

        world::shutdown();
        patch_present(reinterpret_cast<void*>(g_original_present));
        g_vtable = nullptr;
    }
}
