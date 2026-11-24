#include "depth_reprojection.h"
#include "core/logging.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

// HLSL for both XR draw paths, compiled at runtime with D3DCompile.
// ps_depth3d mirrors reproj::backward_warp() and reproj::inverse_linear_depth()
// in reprojection_math.h; keep the two in sync (the unit tests cover the C++
// side). WARP_ITERATIONS and SKY_EPSILON are injected from the C++ constants.
static const char* kDrawHLSL = R"HLSL(
Texture2D<float4> game_color : register(t0);
Texture2D<float>  game_depth : register(t1);
SamplerState      point_clamp  : register(s0);
SamplerState      linear_clamp : register(s1);

cbuffer DrawCB : register(b0) {
    float4 eye_rot0;      // rows of the eye -> LOCAL rotation (xyz)
    float4 eye_rot1;
    float4 eye_rot2;
    float4 eye_pos;       // xyz: eye position the content is viewed from, game units
    float4 eye_tan;       // tan of XrFovf angleLeft, angleRight, angleUp, angleDown
    float4 game_tan;      // x: tan(horizontal FOV / 2), y: tan(vertical FOV / 2)
    float4 depth_params;  // x: 1/far, y: 1/near - 1/far, z: 1 = reversed-Z
    float4 flags;         // x: 1 = decode sRGB colour, y: debug tint (0 off, 1 red, 2 blue)
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Oversized fullscreen triangle; uv (0,0) is the top-left of the target.
VSOutput vs_fullscreen(uint vertex_id : SV_VertexID) {
    VSOutput o;
    float2 pos = float2(float(vertex_id & 1) * 4.0 - 1.0,
                        1.0 - float((vertex_id >> 1) & 1) * 4.0);
    o.position = float4(pos, 0.0, 1.0);
    o.uv = float2((pos.x + 1.0) * 0.5, (1.0 - pos.y) * 0.5);
    return o;
}

float3 srgb_to_linear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

// Linear-light colour of the game frame, alpha forced to 1 (opaque layer).
float4 sample_game_color(float2 uv) {
    float3 c = game_color.SampleLevel(linear_clamp, uv, 0).rgb;
    if (flags.x > 0.5)
        c = srgb_to_linear(saturate(c));
    return float4(c, 1.0);
}

float4 ps_screen(VSOutput input) : SV_TARGET {
    return sample_game_color(input.uv);
}

float2 game_tan_to_uv(float2 t) {
    return float2(0.5 + 0.5 * t.x / game_tan.x, 0.5 - 0.5 * t.y / game_tan.y);
}

// 1 / view distance at a game-image uv; 0 for sky (infinitely far).
float inverse_linear_depth(float2 uv) {
    float d = game_depth.SampleLevel(point_clamp, uv, 0);
    float t = depth_params.z > 0.5 ? d : 1.0 - d;
    return t <= SKY_EPSILON ? 0.0 : depth_params.x + t * depth_params.y;
}

float4 ps_depth3d(VSOutput input) : SV_TARGET {
    // Ray through this output pixel, in LOCAL (= game camera) space.
    float3 d_eye = float3(lerp(eye_tan.x, eye_tan.y, input.uv.x),
                          lerp(eye_tan.z, eye_tan.w, input.uv.y),
                          -1.0);
    float3 dir = float3(dot(eye_rot0.xyz, d_eye),
                        dot(eye_rot1.xyz, d_eye),
                        dot(eye_rot2.xyz, d_eye));

    float4 color = float4(0.0, 0.0, 0.0, 1.0);
    if (dir.z <= -1e-4) {
        // Backward warp: find the game pixel whose reconstructed point lies on
        // this ray by iterating src = d_inf * (1 + E.z/z) + E.xy/z, with z the
        // depth found at the current estimate of src.
        float2 d_inf = dir.xy / -dir.z;
        float2 src_tan = d_inf;
        bool valid = true;
        [unroll]
        for (int k = 0; k < WARP_ITERATIONS; ++k) {
            float inv_z = inverse_linear_depth(game_tan_to_uv(src_tan));
            float w = 1.0 + eye_pos.z * inv_z;
            if (w <= 0.0) {  // surface would be behind the eye
                valid = false;
                w = 1e-4;
            }
            src_tan = d_inf * w + eye_pos.xy * inv_z;
        }
        float2 src_uv = game_tan_to_uv(src_tan);
        if (valid && all(src_uv >= 0.0) && all(src_uv <= 1.0))
            color = sample_game_color(src_uv);
    }

    // Eye identification patch in the upper-right quadrant: red in the image
    // rendered for the left eye, blue for the right eye.
    if (flags.y > 0.5 && input.uv.x > 0.62 && input.uv.x < 0.78 &&
        input.uv.y > 0.22 && input.uv.y < 0.32) {
        float3 tint = flags.y < 1.5 ? float3(1.0, 0.1, 0.1) : float3(0.1, 0.25, 1.0);
        color.rgb = lerp(color.rgb, tint, 0.85);
    }
    return color;
}
)HLSL";

namespace vrc {

template <typename T>
static void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static ID3DBlob* compile_hlsl(const char* entry, const char* target,
                              const D3D_SHADER_MACRO* macros) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(kDrawHLSL, strlen(kDrawHLSL), "xr_draw.hlsl",
                                  macros, nullptr, entry, target,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, &code, &errors);
    if (FAILED(hr)) {
        Log::error("Shader compile failed ({}): {}", entry,
                   errors ? static_cast<const char*>(errors->GetBufferPointer())
                          : "no compiler output");
        safe_release(errors);
        safe_release(code);
        return nullptr;
    }
    safe_release(errors);
    return code;
}

bool DepthReprojection::compile_shaders() {
    char iterations[16];
    char sky_epsilon[32];
    snprintf(iterations, sizeof(iterations), "%d", reproj::kWarpIterations);
    snprintf(sky_epsilon, sizeof(sky_epsilon), "%.9g", static_cast<double>(reproj::kSkyEpsilon));
    const D3D_SHADER_MACRO macros[] = {
        { "WARP_ITERATIONS", iterations },
        { "SKY_EPSILON", sky_epsilon },
        { nullptr, nullptr },
    };

    ID3DBlob* vs = compile_hlsl("vs_fullscreen", "vs_5_0", macros);
    ID3DBlob* ps_screen = compile_hlsl("ps_screen", "ps_5_0", macros);
    ID3DBlob* ps_depth3d = compile_hlsl("ps_depth3d", "ps_5_0", macros);
    bool ok = vs && ps_screen && ps_depth3d;
    if (ok) {
        ok = SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(),
                                                   vs->GetBufferSize(), nullptr,
                                                   &vs_fullscreen_)) &&
             SUCCEEDED(device_->CreatePixelShader(ps_screen->GetBufferPointer(),
                                                  ps_screen->GetBufferSize(), nullptr,
                                                  &ps_screen_)) &&
             SUCCEEDED(device_->CreatePixelShader(ps_depth3d->GetBufferPointer(),
                                                  ps_depth3d->GetBufferSize(), nullptr,
                                                  &ps_depth3d_));
        if (!ok) Log::error("Failed to create XR draw shaders");
    }
    safe_release(vs);
    safe_release(ps_screen);
    safe_release(ps_depth3d);
    return ok;
}

bool DepthReprojection::initialize(ID3D11Device* device) {
    if (initialized_) return true;
    if (!device) return false;
    device_ = device;

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    bool ok = SUCCEEDED(device_->CreateSamplerState(&sd, &point_sampler_));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ok = ok && SUCCEEDED(device_->CreateSamplerState(&sd, &linear_sampler_));

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(DrawConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device_->CreateBuffer(&cb, nullptr, &constants_));

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(device_->CreateRasterizerState(&rd, &rasterizer_));

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(device_->CreateDepthStencilState(&dsd, &depth_stencil_));

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(device_->CreateBlendState(&bd, &blend_));

    ok = ok && compile_shaders();
    if (!ok) {
        Log::error("XR draw pipeline initialisation failed");
        shutdown();
        return false;
    }
    initialized_ = true;
    Log::info("XR draw pipeline ready (Depth3D warp: {} iterations)",
              reproj::kWarpIterations);
    return true;
}

void DepthReprojection::shutdown() {
    safe_release(vs_fullscreen_);
    safe_release(ps_screen_);
    safe_release(ps_depth3d_);
    safe_release(point_sampler_);
    safe_release(linear_sampler_);
    safe_release(constants_);
    safe_release(rasterizer_);
    safe_release(depth_stencil_);
    safe_release(blend_);
    device_ = nullptr;
    initialized_ = false;
}

void DepthReprojection::draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
                             ID3D11ShaderResourceView* color_srv,
                             ID3D11ShaderResourceView* depth_srv,
                             const DrawConstants& constants,
                             ID3D11RenderTargetView* rtv, u32 width, u32 height) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &constants, sizeof(constants));
    ctx->Unmap(constants_, 0);

    // The context belongs to the XR device; the runtime may also have used it
    // inside xrEndFrame, so start from a known state.
    ctx->ClearState();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<f32>(width);
    vp.Height = static_cast<f32>(height);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rasterizer_);
    ctx->OMSetDepthStencilState(depth_stencil_, 0);
    ctx->OMSetBlendState(blend_, nullptr, 0xffffffff);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_fullscreen_, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { color_srv, depth_srv };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[2] = { point_sampler_, linear_sampler_ };
    ctx->PSSetSamplers(0, 2, samplers);
    ctx->PSSetConstantBuffers(0, 1, &constants_);
    ctx->Draw(3, 0);

    // Unbind so the next CopyResource into the local frame copies never finds
    // them still bound as shader inputs.
    ID3D11ShaderResourceView* null_srvs[2] = {};
    ctx->PSSetShaderResources(0, 2, null_srvs);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void DepthReprojection::draw_screen(ID3D11DeviceContext* ctx, const ColorSource& color,
                                    ID3D11RenderTargetView* rtv, u32 width, u32 height) {
    if (!initialized_ || !ctx || !color.srv || !rtv) return;
    DrawConstants c = {};
    c.flags[0] = color.decode_srgb ? 1.0f : 0.0f;
    draw(ctx, ps_screen_, color.srv, nullptr, c, rtv, width, height);
}

void DepthReprojection::draw_depth3d(ID3D11DeviceContext* ctx, const ColorSource& color,
                                     ID3D11ShaderResourceView* depth_srv,
                                     const reproj::EyeView& eye,
                                     const reproj::GameCamera& camera, i32 debug_tint,
                                     ID3D11RenderTargetView* rtv, u32 width, u32 height) {
    if (!initialized_ || !ctx || !color.srv || !depth_srv || !rtv) return;
    DrawConstants c = {};
    for (int row = 0; row < 3; ++row) {
        c.eye_rot[row][0] = eye.rotation[row * 3 + 0];
        c.eye_rot[row][1] = eye.rotation[row * 3 + 1];
        c.eye_rot[row][2] = eye.rotation[row * 3 + 2];
    }
    c.eye_pos[0] = eye.position.x;
    c.eye_pos[1] = eye.position.y;
    c.eye_pos[2] = eye.position.z;
    c.eye_tan[0] = eye.tan_left;
    c.eye_tan[1] = eye.tan_right;
    c.eye_tan[2] = eye.tan_up;
    c.eye_tan[3] = eye.tan_down;
    c.game_tan[0] = camera.tan_half_x;
    c.game_tan[1] = camera.tan_half_y;
    c.depth_params[0] = camera.inv_far;
    c.depth_params[1] = camera.inv_near_minus_far;
    c.depth_params[2] = camera.reversed_z ? 1.0f : 0.0f;
    c.flags[0] = color.decode_srgb ? 1.0f : 0.0f;
    c.flags[1] = static_cast<f32>(debug_tint);
    draw(ctx, ps_depth3d_, color.srv, depth_srv, c, rtv, width, height);
}

} // namespace vrc
