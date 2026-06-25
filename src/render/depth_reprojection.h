#pragma once

#include "core/types.h"
#include <d3d11.h>
#include <d3d12.h>
#include <array>
#include <functional>

namespace vrc {

struct DepthBufferCapture {
    void* texture = nullptr;          // ID3D11Texture2D* or ID3D12Resource*
    u32 width = 0;
    u32 height = 0;
    bool linear_depth = false;
    f32 near_plane = 0.01f;
    f32 far_plane = 1000.0f;
    Matrix4 projection;               // Original game projection
};

struct ReprojectionInput {
    DepthBufferCapture depth;
    ID3D11ShaderResourceView* game_color_srv = nullptr;
    ID3D11ShaderResourceView* linear_depth_srv = nullptr;
    ID3D11RenderTargetView* eye_rtv_left = nullptr;
    ID3D11RenderTargetView* eye_rtv_right = nullptr;
    Matrix4 game_view_proj;
    Matrix4 eye_view_proj_left;
    Matrix4 eye_view_proj_right;
    ViewSetup eye_left;
    ViewSetup eye_right;
    u32 eye_width;
    u32 eye_height;
};

// Matches cbuffer ReprojectionCB in depth_reprojection.hlsl
struct ReprojectionConstants {
    Matrix4 inverse_game_view_proj;
    Matrix4 eye_view_proj_left;
    Matrix4 eye_view_proj_right;
    Matrix4 eye_view_proj;
    f32     depth_range[2];
    f32     render_target_size[2];
    f32     eye_index;
    f32     padding;
};

class DepthReprojection {
public:
    static DepthReprojection& instance();

    bool initialize(ID3D11Device* device);
    bool initialize_d3d12(ID3D12Device* device);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Capture depth buffer from game pipeline (D3D11)
    bool capture_depth_buffer(ID3D11DeviceContext* ctx,
                              ID3D11Texture2D* depth_stencil,
                              const Matrix4& projection,
                              f32 near_plane, f32 far_plane);

    // Capture depth buffer (D3D12)
    bool capture_depth_buffer_d3d12(ID3D12GraphicsCommandList* cmd_list,
                                    ID3D12Resource* depth_buffer,
                                    ID3D12CommandQueue* queue,
                                    const Matrix4& projection,
                                    f32 near_plane, f32 far_plane);

    // Perform 3D reprojection into VR eye views
    bool reproject_to_eyes(const ReprojectionInput& input);

    // Shader-based world position reconstruction from depth
    void set_reprojection_shader(ID3D11PixelShader* shader);
    void set_reprojection_shader_d3d12(void* shader);

    // Set the game color SRV for blit operations
    void set_game_color_srv(ID3D11ShaderResourceView* srv);

    // Fallback: simple 2D blit when depth isn't available
    bool blit_to_eye(const FrameCapture& source,
                     ID3D11RenderTargetView* eye_rtv,
                     const ViewSetup& eye_setup,
                     u32 eye_width, u32 eye_height);

    // Query depth availability
    bool depth_captured() const { return depth_captured_; }

    // Get linear depth SRV (for D3D11 reprojection)
    ID3D11ShaderResourceView* linear_depth_srv() const { return linear_depth_srv_; }

private:
    DepthReprojection() = default;

    bool create_reprojection_resources(u32 width, u32 height);
    bool create_linear_depth_resource(u32 width, u32 height);
    void release_resources();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    bool initialized_ = false;
    bool depth_captured_ = false;

    // Graphics API tracking
    GraphicsAPI current_api_ = GraphicsAPI::Unknown;

    // D3D11 depth resources
    ID3D11Texture2D* linear_depth_tex_ = nullptr;
    ID3D11ShaderResourceView* linear_depth_srv_ = nullptr;

    // D3D12 depth resources
    ID3D12Device* d3d12_device_ = nullptr;
    ID3D12Resource* d3d12_readback_buffer_ = nullptr;   // Staging for depth copy
    ID3D12Resource* d3d12_linear_depth_ = nullptr;       // Linear depth buffer
    ID3D12DescriptorHeap* d3d12_srv_heap_ = nullptr;     // SRV heap for depth

    // Fullscreen quad vertex buffer
    ID3D11Buffer* fs_quad_vb_ = nullptr;

    // Samplers
    ID3D11SamplerState* point_sampler_ = nullptr;
    ID3D11SamplerState* linear_sampler_ = nullptr;

    // Shaders (loaded from compiled HLSL)
    ID3D11VertexShader* vs_reproject_ = nullptr;
    ID3D11PixelShader* ps_reproject_ = nullptr;
    ID3D11VertexShader* vs_blit_ = nullptr;
    ID3D11PixelShader* ps_blit_ = nullptr;
    ID3D11InputLayout* input_layout_ = nullptr;

    // D3D12 shaders
    ID3D12RootSignature* d3d12_root_sig_ = nullptr;
    ID3D12PipelineState* d3d12_reproject_pso_ = nullptr;

    // Constant buffer for reprojection parameters
    ID3D11Buffer* reprojection_cb_ = nullptr;
    static constexpr u32 kMaxDepthCopySize = 4096;

    ID3D11ShaderResourceView* game_color_srv_ = nullptr;

    // Pipeline state for blit pass (override game state)
    ID3D11RasterizerState*   rasterizer_blit_     = nullptr;
    ID3D11DepthStencilState* depth_stencil_blit_  = nullptr;
    ID3D11BlendState*        blend_blit_          = nullptr;

    // Shader compilation
    bool compile_shaders();
    ID3DBlob* compile_shader(const char* hlsl_source, const char* entry,
                             const char* target);

    // D3D12 copy fence
    ID3D12Fence* copy_fence_ = nullptr;
    u64 copy_fence_value_ = 0;
};

} // namespace vrc
