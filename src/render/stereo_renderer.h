#pragma once

#include "core/types.h"
#include "camera_rig.h"
#include "latency_compensator.h"
#include "depth_reprojection.h"
#include "core/perf_monitor.h"
#include "vr/openxr_context.h"
#include "vr/tracking.h"
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <functional>
#include <atomic>

namespace vrc {

class StereoRenderer {
public:
    static StereoRenderer& instance();

    bool initialize(GraphicsAPI api);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // Called on each game frame Present
    bool on_frame_present(const FrameCapture& capture);
    bool render_frame(const FrameCapture& capture);

    enum class Mode : u8 {
        SideBySide,
        OverUnder,
        Reprojection,      // Full stereo reprojection (primary)
        ScreenCapture      // Fallback: capture + shader projection
    };

    Mode mode() const { return mode_; }
    void set_mode(Mode m) { mode_ = m; }

    bool vsync() const { return vsync_; }
    void set_vsync(bool v) { vsync_ = v; }

    CameraRig& camera_rig() { return camera_rig_; }
    const CameraRig& camera_rig() const { return camera_rig_; }

    void set_dynamic_resolution(bool v) { dynamic_resolution_ = v; }
    void set_render_scale(f32 scale) { render_scale_ = scale; }

    u32 frame_count() const { return frame_count_.load(); }

    // ─── Phase 2: ATW / Latency Integration ──────────────────────────

    void set_latency_compensation_enabled(bool v);
    bool latency_compensation_enabled() const;

    // Perform asynchronous timewarp before frame submit
    bool apply_timewarp();

    // Integration callbacks
    void set_pre_render_callback(std::function<void()> cb) { pre_render_ = cb; }
    void set_post_render_callback(std::function<void()> cb) { post_render_ = cb; }

    void set_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx) {
        d3d11_device_ = device;
        d3d11_ctx_ = ctx;
    }

    void set_d3d11_on12_device(ID3D11On12Device* device) { d3d11_on12_ = device; }
    ID3D11On12Device* d3d11_on12_device() const { return d3d11_on12_; }

    // Phase 2 subsystems
    LatencyCompensator& latency() { return latency_; }
    DepthReprojection& depth() { return depth_; }
    PerfMonitor& perf() { return perf_; }

private:
    StereoRenderer() = default;

    bool init_d3d11();
    bool init_d3d12();
    bool init_d3d12_resources();

    bool render_d3d11(const FrameCapture& capture);
    bool render_d3d12(const FrameCapture& capture);

    // Phase 2: D3D11 full pipeline
    bool render_d3d11_with_depth(const FrameCapture& capture);
    bool render_d3d11_blit(const FrameCapture& capture);

    bool create_stereo_targets(u32 width, u32 height);
    bool copy_frame_to_eyes(const FrameCapture& capture);

    GraphicsAPI api_ = GraphicsAPI::Unknown;
    Mode mode_ = Mode::Reprojection;
    bool initialized_ = false;
    bool vsync_ = true;
    bool dynamic_resolution_ = true;
    f32 render_scale_ = 1.0f;
    std::atomic<u32> frame_count_{0};

    CameraRig& camera_rig_ = CameraRig::instance();
    OpenXRContext& openxr_ = OpenXRContext::instance();

    // Phase 2 subsystems
    LatencyCompensator& latency_ = LatencyCompensator::instance();
    DepthReprojection& depth_ = DepthReprojection::instance();
    PerfMonitor& perf_ = PerfMonitor::instance();
    TrackingSystem& tracking_ = TrackingSystem::instance();

    std::function<void()> pre_render_;
    std::function<void()> post_render_;

    // D3D11 resources
    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_ctx_ = nullptr;
    ID3D11Texture2D* stereo_rt_[2] = {};
    ID3D11RenderTargetView* stereo_rtv_[2] = {};

    // D3D12 resources
    ID3D12Device* d3d12_device_ = nullptr;
    ID3D12CommandQueue* d3d12_queue_ = nullptr;
    ID3D12CommandAllocator* d3d12_allocator_ = nullptr;
    ID3D12GraphicsCommandList* d3d12_cmd_list_ = nullptr;
    ID3D12Fence* d3d12_fence_ = nullptr;
    HANDLE d3d12_fence_event_ = nullptr;
    u64 d3d12_fence_value_ = 0;

    // D3D11On12 interop (D3D12 game → D3D11 OpenXR binding)
    ID3D11On12Device* d3d11_on12_ = nullptr;

    // Game back buffer copy for reprojection
    ID3D11Texture2D* game_frame_copy_ = nullptr;
    ID3D11ShaderResourceView* game_frame_srv_ = nullptr;

    // F2 toggle state (polled via GetAsyncKeyState, works without window focus)
    bool f2_was_down_ = false;
};

} // namespace vrc
