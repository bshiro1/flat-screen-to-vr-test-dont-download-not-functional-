#pragma once

#include "core/types.h"
#include "render/reprojection_math.h"
#include <d3d11.h>

namespace vrc {

// The captured game frame as the XR device sees it.
struct ColorSource {
    ID3D11ShaderResourceView* srv = nullptr;
    // True when the SRV returns gamma-encoded sRGB values (a UNORM view such
    // as R10G10B10A2_UNORM); the shader then decodes them. False when the view
    // already returns linear values (an *_SRGB view, or a float format).
    bool decode_srgb = false;
};

// Draws the captured game frame into XR swapchain images. Used only on the
// OpenXR frame thread with the XR-dedicated device and its immediate context.
//
//   draw_screen  — resamples the whole game frame into the Screen-mode quad
//                  swapchain image.
//   draw_depth3d — renders one eye of the Depth3D view with the backward warp
//                  described in reprojection_math.h.
//
// Both write LINEAR colour. The swapchain RTV is either *_SRGB (the hardware
// encodes on write) or, when the runtime offers no sRGB format, UNORM, which
// OpenXR defines as linear.
class DepthReprojection {
public:
    bool initialize(ID3D11Device* device);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    void draw_screen(ID3D11DeviceContext* ctx, const ColorSource& color,
                     ID3D11RenderTargetView* rtv, u32 width, u32 height);

    // debug_tint: 0 = none, 1 = red patch (left eye), 2 = blue patch (right eye).
    void draw_depth3d(ID3D11DeviceContext* ctx, const ColorSource& color,
                      ID3D11ShaderResourceView* depth_srv,
                      const reproj::EyeView& eye, const reproj::GameCamera& camera,
                      i32 debug_tint, ID3D11RenderTargetView* rtv,
                      u32 width, u32 height);

private:
    // Matches cbuffer DrawCB in the HLSL source.
    struct DrawConstants {
        f32 eye_rot[3][4];
        f32 eye_pos[4];
        f32 eye_tan[4];
        f32 game_tan[4];
        f32 depth_params[4];
        f32 flags[4];
    };
    static_assert(sizeof(DrawConstants) == 128, "DrawConstants must match cbuffer DrawCB");

    bool compile_shaders();
    void draw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
              ID3D11ShaderResourceView* color_srv, ID3D11ShaderResourceView* depth_srv,
              const DrawConstants& constants, ID3D11RenderTargetView* rtv,
              u32 width, u32 height);

    ID3D11Device* device_ = nullptr;  // borrowed
    bool initialized_ = false;

    ID3D11VertexShader* vs_fullscreen_ = nullptr;
    ID3D11PixelShader* ps_screen_ = nullptr;
    ID3D11PixelShader* ps_depth3d_ = nullptr;
    ID3D11SamplerState* point_sampler_ = nullptr;
    ID3D11SamplerState* linear_sampler_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11RasterizerState* rasterizer_ = nullptr;
    ID3D11DepthStencilState* depth_stencil_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
};

} // namespace vrc
