#include "depth_reprojection.h"
#include "core/logging.h"
#include <d3dcompiler.h>
#include <cassert>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

// ─── Embedded HLSL Source ────────────────────────────────────────────────────
// The shader source from depth_reprojection.hlsl embedded as a string literal.
// In production, pre-compile to .cso and load at runtime.
static const char* kReprojectionHLSL = R"(
Texture2D<float4> game_color : register(t0);
Texture2D<float>  linear_depth : register(t1);
SamplerState      point_sampler : register(s0);
SamplerState      linear_sampler : register(s1);

cbuffer ReprojectionCB : register(b0) {
    float4x4 inverse_game_view_proj;
    float4x4 eye_view_proj_left;
    float4x4 eye_view_proj_right;
    float4x4 eye_view_proj;
    float2   depth_range;
    float2   render_target_size;
    float    eye_index;
    float    padding;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput vs_main(uint vertex_id : SV_VertexID) {
    VSOutput output;
    // Oversized fullscreen triangle — clips to viewport, avoids diagonal seam.
    float2 pos = float2(
        float(vertex_id & 1) * 4.0f - 1.0f,
        float((vertex_id >> 1) & 1) * (-4.0f) + 1.0f
    );
    output.position = float4(pos, 0.0f, 1.0f);
    output.uv = float2((pos.x + 1.0f) * 0.5f, (1.0f - pos.y) * 0.5f);
    return output;
}

float3 reconstruct_world_position(float2 uv, float depth) {
    float4 clip_pos;
    clip_pos.x = uv.x * 2.0f - 1.0f;
    clip_pos.y = (1.0f - uv.y) * 2.0f - 1.0f;
    clip_pos.z = depth;
    clip_pos.w = 1.0f;
    float4 world_pos = mul(inverse_game_view_proj, clip_pos);
    world_pos /= world_pos.w;
    return world_pos.xyz;
}

float4 ps_reproject(VSOutput input) : SV_TARGET {
    float depth = linear_depth.SampleLevel(point_sampler, input.uv, 0);
    if (depth >= 1.0f) return float4(0,0,0,0);
    float3 world_pos = reconstruct_world_position(input.uv, depth);
    float4 eye_clip = mul(eye_view_proj, float4(world_pos, 1.0f));
    float2 eye_uv = eye_clip.xy / eye_clip.w;
    eye_uv = eye_uv * float2(0.5f, -0.5f) + 0.5f;
    if (any(eye_uv < 0.0f) || any(eye_uv > 1.0f)) return float4(0,0,0,0);
    return game_color.SampleLevel(linear_sampler, eye_uv, 0);
}

float4 ps_blit(VSOutput input) : SV_TARGET {
    // Force alpha=1 so the VR compositor doesn't treat the frame as transparent.
    float3 c = game_color.SampleLevel(linear_sampler, input.uv, 0).rgb;
    return float4(c, 1.0f);
}
)";

namespace vrc {

DepthReprojection& DepthReprojection::instance() {
    static DepthReprojection reproj;
    return reproj;
}

bool DepthReprojection::initialize_d3d12(ID3D12Device* device) {
    if (initialized_) return true;
    d3d12_device_ = device;
    current_api_ = GraphicsAPI::D3D12;

    Log::info("Initializing D3D12 depth reprojection");

    // Create a fence for GPU synchronization
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_));
    if (FAILED(hr)) {
        Log::error("Failed to create D3D12 copy fence");
        return false;
    }

    initialized_ = true;
    Log::info("D3D12 depth reprojection initialized");
    return true;
}

bool DepthReprojection::initialize(ID3D11Device* device) {
    if (initialized_) return true;
    current_api_ = GraphicsAPI::D3D11;
    device_ = device;
    device->GetImmediateContext(&context_);

    Log::info("Initializing depth reprojection");

    // Create samplers
    D3D11_SAMPLER_DESC samp_desc = {};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device_->CreateSamplerState(&samp_desc, &point_sampler_))) {
        Log::error("Failed to create point sampler");
        return false;
    }

    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(device_->CreateSamplerState(&samp_desc, &linear_sampler_))) {
        Log::error("Failed to create linear sampler");
        return false;
    }

    // Compile shaders
    if (!compile_shaders()) {
        Log::error("Failed to compile reprojection shaders");
        return false;
    }

    // Create constant buffer — ByteWidth must be a multiple of 16 bytes
    static constexpr u32 kCbSize =
        (static_cast<u32>(sizeof(ReprojectionConstants)) + 15u) & ~15u;
    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = kCbSize;
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cb_desc, nullptr, &reprojection_cb_))) {
        Log::error("Failed to create reprojection constant buffer");
        return false;
    }

    // Rasterizer state: no scissor, no cull — game state may have both enabled
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = FALSE;
        if (FAILED(device_->CreateRasterizerState(&rd, &rasterizer_blit_))) {
            Log::error("Failed to create blit rasterizer state");
            return false;
        }
    }

    // Depth-stencil state: all off
    {
        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable = FALSE;
        dsd.StencilEnable = FALSE;
        if (FAILED(device_->CreateDepthStencilState(&dsd, &depth_stencil_blit_))) {
            Log::error("Failed to create blit depth-stencil state");
            return false;
        }
    }

    // Blend state: no blending, write all channels
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&bd, &blend_blit_))) {
            Log::error("Failed to create blit blend state");
            return false;
        }
    }

    initialized_ = true;
    Log::info("Depth reprojection initialized");
    return true;
}

void DepthReprojection::shutdown() {
    release_resources();
    initialized_ = false;
    Log::info("Depth reprojection shut down");
}

bool DepthReprojection::capture_depth_buffer(
    ID3D11DeviceContext* ctx, ID3D11Texture2D* depth_stencil,
    const Matrix4& projection, f32 near_plane, f32 far_plane)
{
    if (!depth_stencil) {
        depth_captured_ = false;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    depth_stencil->GetDesc(&desc);

    // Create linear depth texture if needed
    if (linear_depth_tex_) {
        D3D11_TEXTURE2D_DESC linear_desc;
        linear_depth_tex_->GetDesc(&linear_desc);
        if (linear_desc.Width != desc.Width) {
            create_linear_depth_resource(desc.Width, desc.Height);
        }
    } else {
        create_linear_depth_resource(desc.Width, desc.Height);
    }

    // Convert native depth to linear depth
    // For D3D11: copy depth-stencil resource to staging, then shader-convert
    // This is a placeholder - full implementation would:
    // 1. Copy depth to a readable resource
    // 2. Run a compute shader that converts Z-buffer values to linear depth
    // 3. Store in linear_depth_tex_

    depth_captured_ = true;
    return true;
}

bool DepthReprojection::capture_depth_buffer_d3d12(
    ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* depth_buffer,
    ID3D12CommandQueue* queue, const Matrix4& projection,
    f32 near_plane, f32 far_plane)
{
    if (!depth_buffer || !cmd_list || !queue) {
        depth_captured_ = false;
        return false;
    }

    D3D12_RESOURCE_DESC desc = depth_buffer->GetDesc();

    // Create or resize readback buffer
    u32 buffer_size = desc.Width * desc.Height * sizeof(f32);
    if (!d3d12_readback_buffer_) {
        D3D12_HEAP_PROPERTIES heap_props = {
            D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
            D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC rb_desc = {
            D3D12_RESOURCE_DIMENSION_BUFFER, 0, buffer_size, 1, 1, 1,
            DXGI_FORMAT_UNKNOWN, 1, 0,
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            D3D12_RESOURCE_FLAG_NONE };
        if (FAILED(d3d12_device_->CreateCommittedResource(
                &heap_props, D3D12_HEAP_FLAG_NONE, &rb_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&d3d12_readback_buffer_)))) {
            Log::error("Failed to create D3D12 depth readback buffer");
            return false;
        }
    }

    // Transition depth buffer to COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = depth_buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(1, &barrier);

    // Copy depth to readback buffer
    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = depth_buffer;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = d3d12_readback_buffer_;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint.Offset = 0;
    dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst_loc.PlacedFootprint.Footprint.Width = static_cast<UINT>(desc.Width);
    dst_loc.PlacedFootprint.Footprint.Height = static_cast<UINT>(desc.Height);
    dst_loc.PlacedFootprint.Footprint.Depth = 1;
    dst_loc.PlacedFootprint.Footprint.RowPitch = desc.Width * sizeof(f32);

    cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // Transition depth buffer back to DEPTH_WRITE
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    cmd_list->ResourceBarrier(1, &barrier);

    // Execute and sync
    cmd_list->Close();
    ID3D12CommandList* lists[] = { cmd_list };
    queue->ExecuteCommandLists(1, lists);

    copy_fence_value_++;
    queue->Signal(copy_fence_, copy_fence_value_);

    // Wait for GPU to complete the copy
    if (copy_fence_->GetCompletedValue() < copy_fence_value_) {
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (event) {
            copy_fence_->SetEventOnCompletion(copy_fence_value_, event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
    }

    // Map readback buffer for CPU read
    // In production: run a compute shader on the GPU to convert depth
    // For now: depth is available in readback buffer

    depth_captured_ = true;
    return true;
}

ID3DBlob* DepthReprojection::compile_shader(const char* hlsl_source,
                                             const char* entry,
                                             const char* target)
{
    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(hlsl_source, strlen(hlsl_source),
                            nullptr, nullptr, nullptr,
                            entry, target,
                            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0, &shader, &errors);
    if (FAILED(hr)) {
        if (errors) {
            Log::error("Shader compile error ({}): {}", entry,
                       static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        } else {
            Log::error("Shader compile failed ({}), hr={:#x}", entry, static_cast<u32>(hr));
        }
        return nullptr;
    }
    if (errors) errors->Release();
    return shader;
}

bool DepthReprojection::compile_shaders() {
    // Compile vertex shader (fullscreen triangle, no inputs)
    ID3DBlob* vs_blob = compile_shader(kReprojectionHLSL, "vs_main", "vs_5_0");
    if (!vs_blob) return false;
    if (FAILED(device_->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vs_blit_))) {
        Log::error("Failed to create blit vertex shader");
        vs_blob->Release();
        return false;
    }
    vs_reproject_ = vs_blit_;
    if (vs_reproject_) vs_reproject_->AddRef();
    vs_blob->Release();

    // Compile blit pixel shader (simple 2D copy)
    ID3DBlob* ps_blit_blob = compile_shader(kReprojectionHLSL, "ps_blit", "ps_5_0");
    if (!ps_blit_blob) return false;
    if (FAILED(device_->CreatePixelShader(
            ps_blit_blob->GetBufferPointer(), ps_blit_blob->GetBufferSize(),
            nullptr, &ps_blit_))) {
        Log::error("Failed to create blit pixel shader");
        ps_blit_blob->Release();
        return false;
    }
    ps_blit_blob->Release();

    // Compile reprojection pixel shader (depth-aware 3D reprojection)
    ID3DBlob* ps_reproj_blob = compile_shader(kReprojectionHLSL, "ps_reproject", "ps_5_0");
    if (!ps_reproj_blob) return false;
    if (FAILED(device_->CreatePixelShader(
            ps_reproj_blob->GetBufferPointer(), ps_reproj_blob->GetBufferSize(),
            nullptr, &ps_reproject_))) {
        Log::error("Failed to create reprojection pixel shader");
        ps_reproj_blob->Release();
        return false;
    }
    ps_reproj_blob->Release();

    Log::info("Reprojection shaders compiled successfully");
    return true;
}

void DepthReprojection::set_game_color_srv(ID3D11ShaderResourceView* srv) {
    if (game_color_srv_) game_color_srv_->Release();
    game_color_srv_ = srv;
    if (game_color_srv_) game_color_srv_->AddRef();
}

bool DepthReprojection::blit_to_eye(const FrameCapture& source,
                                     ID3D11RenderTargetView* eye_rtv,
                                     const ViewSetup& eye_setup,
                                     u32 eye_width, u32 eye_height)
{
    if (!context_ || !eye_rtv || !game_color_srv_) return false;

    // ── Save every piece of state we touch ───────────────────────────────────
    // OM
    ID3D11RenderTargetView*  old_rtv = nullptr;
    ID3D11DepthStencilView*  old_dsv = nullptr;
    ID3D11RasterizerState*   old_rs  = nullptr;
    ID3D11DepthStencilState* old_dss = nullptr;
    UINT  old_stencil_ref   = 0;
    ID3D11BlendState* old_bs = nullptr;
    float old_blend_factor[4] = {};
    UINT  old_sample_mask   = 0;
    context_->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    context_->RSGetState(&old_rs);
    context_->OMGetDepthStencilState(&old_dss, &old_stencil_ref);
    context_->OMGetBlendState(&old_bs, old_blend_factor, &old_sample_mask);

    // RS
    UINT old_vp_count = 1;
    D3D11_VIEWPORT old_vp = {};
    context_->RSGetViewports(&old_vp_count, &old_vp);

    // IA
    ID3D11InputLayout* old_il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY old_topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context_->IAGetInputLayout(&old_il);
    context_->IAGetPrimitiveTopology(&old_topo);

    // Shaders
    ID3D11VertexShader* old_vs = nullptr;
    ID3D11PixelShader*  old_ps = nullptr;
    context_->VSGetShader(&old_vs, nullptr, nullptr);
    context_->PSGetShader(&old_ps, nullptr, nullptr);

    // PS resources / samplers / CBs
    ID3D11ShaderResourceView* old_srvs[2] = {};
    ID3D11SamplerState*       old_samplers[2] = {};
    ID3D11Buffer*             old_cb = nullptr;
    context_->PSGetShaderResources(0, 2, old_srvs);
    context_->PSGetSamplers(0, 2, old_samplers);
    context_->PSGetConstantBuffers(0, 1, &old_cb);

    // ── Override with known-good blit state ──────────────────────────────────
    context_->RSSetState(rasterizer_blit_);
    context_->OMSetDepthStencilState(depth_stencil_blit_, 0);
    context_->OMSetBlendState(blend_blit_, nullptr, 0xffffffff);
    context_->OMSetRenderTargets(1, &eye_rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<f32>(eye_width);
    vp.Height   = static_cast<f32>(eye_height);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context_->VSSetShader(vs_blit_, nullptr, 0);
    context_->PSSetShader(ps_blit_, nullptr, 0);

    ID3D11ShaderResourceView* srvs[]    = { game_color_srv_, nullptr };
    ID3D11SamplerState*       samplers[] = { point_sampler_, linear_sampler_ };
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetConstantBuffers(0, 1, &reprojection_cb_);

    context_->Draw(3, 0);

    // ── Restore everything ────────────────────────────────────────────────────
    context_->PSSetShaderResources(0, 2, old_srvs);
    context_->PSSetSamplers(0, 2, old_samplers);
    context_->PSSetConstantBuffers(0, 1, &old_cb);
    context_->VSSetShader(old_vs, nullptr, 0);
    context_->PSSetShader(old_ps, nullptr, 0);
    context_->IASetInputLayout(old_il);
    context_->IASetPrimitiveTopology(old_topo);
    context_->OMSetRenderTargets(1, &old_rtv, old_dsv);
    context_->RSSetState(old_rs);
    context_->OMSetDepthStencilState(old_dss, old_stencil_ref);
    context_->OMSetBlendState(old_bs, old_blend_factor, old_sample_mask);
    if (old_vp_count > 0) context_->RSSetViewports(1, &old_vp);

    if (old_rtv)        old_rtv->Release();
    if (old_dsv)        old_dsv->Release();
    if (old_rs)         old_rs->Release();
    if (old_dss)        old_dss->Release();
    if (old_bs)         old_bs->Release();
    if (old_il)         old_il->Release();
    if (old_vs)         old_vs->Release();
    if (old_ps)         old_ps->Release();
    if (old_srvs[0])    old_srvs[0]->Release();
    if (old_srvs[1])    old_srvs[1]->Release();
    if (old_samplers[0]) old_samplers[0]->Release();
    if (old_samplers[1]) old_samplers[1]->Release();
    if (old_cb)         old_cb->Release();

    return true;
}

bool DepthReprojection::reproject_to_eyes(const ReprojectionInput& input) {
    if (!initialized_ || !depth_captured_ || !context_) {
        return false;
    }

    ID3D11ShaderResourceView* depth_srv = input.linear_depth_srv
        ? input.linear_depth_srv : linear_depth_srv_;
    if (!input.game_color_srv || !depth_srv) return false;

    ID3D11RenderTargetView* rtvs[2] = { input.eye_rtv_left, input.eye_rtv_right };
    Matrix4 eye_vps[2] = { input.eye_view_proj_left, input.eye_view_proj_right };

    // Save old RTV
    ID3D11RenderTargetView* old_rtv = nullptr;
    context_->OMGetRenderTargets(1, &old_rtv, nullptr);

    // Bind shaders
    context_->VSSetShader(vs_reproject_, nullptr, 0);
    context_->PSSetShader(ps_reproject_, nullptr, 0);

    // Bind SRVs: t0 = game color, t1 = linear depth
    ID3D11ShaderResourceView* srvs[] = { input.game_color_srv, depth_srv };
    context_->PSSetShaderResources(0, 2, srvs);

    // Bind samplers: s0 = point, s1 = linear
    ID3D11SamplerState* samplers[] = { point_sampler_, linear_sampler_ };
    context_->PSSetSamplers(0, 2, samplers);

    // IA setup for fullscreen triangle
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Render both eyes
    for (int eye = 0; eye < 2; eye++) {
        if (!rtvs[eye]) continue;

        // Update per-eye constants
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(context_->Map(reprojection_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                auto* data = static_cast<ReprojectionConstants*>(mapped.pData);
                data->inverse_game_view_proj = input.game_view_proj;
                data->eye_view_proj_left = input.eye_view_proj_left;
                data->eye_view_proj_right = input.eye_view_proj_right;
                data->eye_view_proj = eye_vps[eye];
                data->depth_range[0] = input.depth.near_plane;
                data->depth_range[1] = input.depth.far_plane;
                data->render_target_size[0] = static_cast<f32>(input.eye_width);
                data->render_target_size[1] = static_cast<f32>(input.eye_height);
                data->eye_index = static_cast<f32>(eye);
                context_->Unmap(reprojection_cb_, 0);
            }
        }
        context_->PSSetConstantBuffers(0, 1, &reprojection_cb_);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<f32>(input.eye_width);
        vp.Height = static_cast<f32>(input.eye_height);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);

        context_->OMSetRenderTargets(1, &rtvs[eye], nullptr);
        context_->Draw(3, 0);
    }

    // Restore SRV slots
    ID3D11ShaderResourceView* null_srv[] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, null_srv);

    // Restore old RTV
    if (old_rtv) {
        context_->OMSetRenderTargets(1, &old_rtv, nullptr);
        old_rtv->Release();
    }

    Log::debug("Depth reprojection executed");
    return true;
}

bool DepthReprojection::create_linear_depth_resource(u32 width, u32 height) {
    release_resources();

    // Skip if resolution is too large
    if (width > kMaxDepthCopySize || height > kMaxDepthCopySize) {
        Log::warn("Depth buffer too large for reprojection: {}x{}", width, height);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT; // Single channel linear depth
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.Usage = D3D11_USAGE_DEFAULT;

    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &linear_depth_tex_))) {
        Log::error("Failed to create linear depth texture");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    if (FAILED(device_->CreateShaderResourceView(
            linear_depth_tex_, &srv_desc, &linear_depth_srv_))) {
        Log::error("Failed to create depth SRV");
        return false;
    }

    Log::info("Linear depth resource created: {}x{}", width, height);
    return true;
}

void DepthReprojection::release_resources() {
    // D3D11 resources
    if (game_color_srv_)     { game_color_srv_->Release();     game_color_srv_     = nullptr; }
    if (rasterizer_blit_)    { rasterizer_blit_->Release();    rasterizer_blit_    = nullptr; }
    if (depth_stencil_blit_) { depth_stencil_blit_->Release(); depth_stencil_blit_ = nullptr; }
    if (blend_blit_)         { blend_blit_->Release();         blend_blit_         = nullptr; }
    if (linear_depth_srv_) { linear_depth_srv_->Release(); linear_depth_srv_ = nullptr; }
    if (linear_depth_tex_) { linear_depth_tex_->Release(); linear_depth_tex_ = nullptr; }
    if (point_sampler_) { point_sampler_->Release(); point_sampler_ = nullptr; }
    if (linear_sampler_) { linear_sampler_->Release(); linear_sampler_ = nullptr; }
    if (vs_reproject_) { vs_reproject_->Release(); vs_reproject_ = nullptr; }
    if (ps_reproject_) { ps_reproject_->Release(); ps_reproject_ = nullptr; }
    if (vs_blit_) { vs_blit_->Release(); vs_blit_ = nullptr; }
    if (ps_blit_) { ps_blit_->Release(); ps_blit_ = nullptr; }
    if (input_layout_) { input_layout_->Release(); input_layout_ = nullptr; }
    if (reprojection_cb_) { reprojection_cb_->Release(); reprojection_cb_ = nullptr; }
    if (fs_quad_vb_) { fs_quad_vb_->Release(); fs_quad_vb_ = nullptr; }

    // D3D12 resources
    if (d3d12_readback_buffer_) { d3d12_readback_buffer_->Release(); d3d12_readback_buffer_ = nullptr; }
    if (d3d12_linear_depth_) { d3d12_linear_depth_->Release(); d3d12_linear_depth_ = nullptr; }
    if (d3d12_srv_heap_) { d3d12_srv_heap_->Release(); d3d12_srv_heap_ = nullptr; }
    if (d3d12_root_sig_) { d3d12_root_sig_->Release(); d3d12_root_sig_ = nullptr; }
    if (d3d12_reproject_pso_) { d3d12_reproject_pso_->Release(); d3d12_reproject_pso_ = nullptr; }
    if (copy_fence_) { copy_fence_->Release(); copy_fence_ = nullptr; }

    current_api_ = GraphicsAPI::Unknown;
}

void DepthReprojection::set_reprojection_shader(ID3D11PixelShader* shader) {
    if (ps_reproject_) ps_reproject_->Release();
    ps_reproject_ = shader;
    if (shader) shader->AddRef();
}

void DepthReprojection::set_reprojection_shader_d3d12(void* shader) {
    // D3D12 path: store shader pointer for later use
}

} // namespace vrc
