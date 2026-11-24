#pragma once

#include "core/types.h"
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <atomic>
#include <thread>
#include <vector>

namespace vrc {

// Owns the OpenXR instance, session, spaces and swapchains plus the
// XR-dedicated D3D11 device, and runs the OpenXR frame loop on its own thread.
//
// Every OpenXR call after start-up, and every write to a swapchain image, runs
// on that frame thread with the XR device's immediate context. The game's
// Present thread only copies frames into shared textures (StereoRenderer).
// D3D12 games reach this through a D3D11On12 device, so the session always
// uses XR_KHR_D3D11_enable.
//
// Each frame the thread submits either
//   * a quad layer (Screen mode, and the Depth3D fallback): the game frame on a
//     virtual screen, world-locked (LOCAL) or head-locked (VIEW); or
//   * a projection layer (Depth3D): per-eye reprojected views rendered with
//     exactly the poses and FOVs from xrLocateViews that are submitted.
class OpenXRContext {
public:
    static OpenXRContext& instance();

    // Creates the instance and finds the HMD system. The session is created on
    // the frame thread once a device is bound.
    bool initialize(GraphicsAPI api);
    // Stops the frame thread and destroys everything. Idempotent.
    void shutdown();
    bool is_initialized() const { return initialized_.load(std::memory_order_acquire); }

    // Present thread, once: checks that the game renders on the adapter the
    // runtime requires, creates the XR device there and starts the frame thread.
    void bind_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx);
    bool is_device_bound() const { return device_bound_.load(std::memory_order_acquire); }

    // True while the frame thread runs; the Present thread skips its copies
    // otherwise.
    bool is_streaming() const { return streaming_.load(std::memory_order_acquire); }

private:
    OpenXRContext() = default;

    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        u32 width = 0;
        u32 height = 0;
        std::vector<ID3D11RenderTargetView*> rtvs;  // one per image, on the XR device
    };

    // Counts occurrences of a recurring condition and says when to log it:
    // the first few times, then every 600th.
    struct LogLimiter {
        u64 count = 0;
        bool should_log() { ++count; return count <= 5 || count % 600 == 0; }
    };

    // Start-up / instance (any thread before the frame thread starts, then
    // the frame thread only).
    bool create_instance();
    bool get_system();
    void destroy_instance();
    bool graphics_requirements(LUID& adapter_luid, D3D_FEATURE_LEVEL& min_feature_level);
    bool create_xr_device(ID3D11Device* game_device);

    // Frame thread.
    void start_frame_thread();
    void stop_frame_thread();
    void frame_thread_proc();
    void sleep_interruptible(u32 ms);
    bool create_session();
    void destroy_session();
    bool choose_swapchain_format();
    bool create_swapchain(Swapchain& sc, u32 width, u32 height, const char* label);
    void destroy_swapchain(Swapchain& sc);
    bool acquire_image(Swapchain& sc, u32& index);
    bool release_image(Swapchain& sc);
    void poll_events();
    void handle_result(XrResult result, const char* what, LogLimiter& limiter);
    bool run_frame();
    bool render_quad_layer(const ConfigProfile& cfg, u32 source_width, u32 source_height,
                           u64 source_generation, XrCompositionLayerQuad& layer);
    bool render_projection_layer(const ConfigProfile& cfg, const XrView (&views)[2],
                                 XrCompositionLayerProjection& layer,
                                 XrCompositionLayerProjectionView (&layer_views)[2]);

    // Instance / system.
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
    const XrFormFactor form_factor_ = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrViewConfigurationType view_config_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool htcx_tracker_supported_ = false;
    bool hand_tracking_supported_ = false;
    u32 max_swapchain_width_ = 0;
    u32 max_swapchain_height_ = 0;
    u32 eye_recommended_width_ = 0;
    u32 eye_recommended_height_ = 0;
    u32 eye_max_width_ = 0;
    u32 eye_max_height_ = 0;

    // XR-dedicated device on the runtime's adapter (multithread-protected).
    ID3D11Device* xr_device_ = nullptr;
    ID3D11DeviceContext* xr_ctx_ = nullptr;
    LUID xr_adapter_luid_ = {};

    // Session and its children (frame thread).
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_space_ = XR_NULL_HANDLE;
    XrSpace view_space_ = XR_NULL_HANDLE;
    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    bool session_running_ = false;     // between xrBeginSession and xrEndSession
    bool session_lost_ = false;        // destroy and recreate the session
    bool instance_lost_ = false;       // destroy and recreate the instance
    i64 swapchain_format_ = 0;         // DXGI_FORMAT chosen for every swapchain
    f32 render_scale_ = 1.0f;          // ConfigProfile::render_scale at session creation
    Swapchain quad_;
    Swapchain eyes_[2];
    bool quad_has_image_ = false;      // quad_ has a released image to show
    u64 quad_generation_ = 0;          // source frame currently in quad_
    int active_layer_ = -1;            // last submitted kind, for logging changes

    std::thread frame_thread_;
    std::atomic<bool> exit_flag_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> device_bound_{false};
    std::atomic<bool> streaming_{false};

    LogLimiter session_log_;
    LogLimiter frame_log_;
    LogLimiter locate_log_;
    LogLimiter swapchain_log_;
    LogLimiter event_log_;
};

} // namespace vrc
