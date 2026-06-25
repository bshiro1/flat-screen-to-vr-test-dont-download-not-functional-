#pragma once

#include "core/types.h"
#include <d3d11.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <memory>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

namespace vrc {

struct OpenXRSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    i32 width = 0;
    i32 height = 0;
    u32 acquired_index = 0;
    bool image_acquired = false;
    std::vector<XrSwapchainImageD3D11KHR> d3d11_images;
    std::vector<XrSwapchainImageD3D12KHR> d3d12_images;
    std::vector<void*> d3d11_rtvs;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> d3d12_rtvs;
};

struct FrameThreadData {
    u32 acquired_eye_indices[2] = {};
    XrView views[2] = {};
    XrViewState view_state = {};
    XrFrameState frame_state = {};
};

class OpenXRContext {
public:
    static OpenXRContext& instance();

    bool initialize(GraphicsAPI api);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    void bind_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void bind_d3d12_device(ID3D12Device* device, ID3D12CommandQueue* queue);
    void bind_swap_chain(void* swap_chain);

    bool is_device_bound() const { return device_bound_; }
    ID3D11Device* d3d11_device() const { return d3d11_device_; }
    ID3D11DeviceContext* d3d11_context() const { return d3d11_context_; }
    ID3D12Device* d3d12_device() const { return d3d12_device_; }
    ID3D12CommandQueue* d3d12_queue() const { return d3d12_queue_; }

    // Frame pipeline (Present-thread API — non-blocking OpenXR calls)
    bool begin_frame();
    bool end_frame();
    bool submit_frame();

    bool acquire_eyes(EyeViews& views);
    f64 predicted_display_time() const;

    bool begin_eye_image(i32 eye, u32* out_image_index = nullptr);
    bool end_eye_image(i32 eye);

    void* eye_rtv(i32 eye) const { return eye_rtvs_[eye]; }
    void* eye_swapchain_image(i32 eye, u32 index) const;
    XrSwapchain eye_swapchain(i32 eye) const { return swapchains_[eye].handle; }
    u32 eye_swapchain_width(i32 eye) const { return swapchains_[eye].width; }
    u32 eye_swapchain_height(i32 eye) const { return swapchains_[eye].height; }

    XrInstance xr_instance() const { return instance_; }
    XrSession session() const { return session_; }
    XrSpace local_space() const { return local_space_; }
    XrSpace view_space() const { return view_space_; }
    XrSystemId system_id() const { return system_id_; }

    XrViewState view_state() const { return frame_data_.view_state; }
    const std::array<XrView, 2>& views() const { return reinterpret_cast<const std::array<XrView, 2>&>(frame_data_.views); }
    XrFrameState frame_state() const { return frame_data_.frame_state; }

    GraphicsAPI graphics_api() const { return graphics_api_; }
    i32 viewport_width() const { return viewport_width_; }
    i32 viewport_height() const { return viewport_height_; }

    f32 ipd() const { return ipd_; }
    void set_ipd(f32 ipd) { ipd_ = ipd; }

    LatencyStats& latency_stats() { return latency_stats_; }

    bool create_eye_rtvs();
    void destroy_eye_rtvs();

private:
    OpenXRContext() = default;

    bool create_instance();
    bool get_system();
    bool create_swapchains();
    bool create_reference_spaces();
    void poll_events();

    bool ensure_session_created();

    void start_frame_thread();
    void stop_frame_thread();
    void frame_thread_proc();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_space_ = XR_NULL_HANDLE;
    XrSpace view_space_ = XR_NULL_HANDLE;
    XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
    XrFormFactor form_factor_ = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrViewConfigurationType view_config_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    std::array<OpenXRSwapchain, 2> swapchains_;
    std::array<void*, 2> eye_rtvs_ = {};

    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    bool initialized_ = false;
    bool session_running_ = false;
    bool device_bound_ = false;
    bool session_created_ = false;
    bool swapchains_created_ = false;

    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_context_ = nullptr;
    ID3D12Device* d3d12_device_ = nullptr;
    ID3D12CommandQueue* d3d12_queue_ = nullptr;
    void* bound_swap_chain_ = nullptr;
    bool multithread_protected_ = false;
    ID3D12DescriptorHeap* d3d12_rtv_heap_ = nullptr;
    u32 d3d12_rtv_heap_size_ = 0;

    // Config
    GraphicsAPI graphics_api_ = GraphicsAPI::D3D11;
    i32 viewport_width_ = 0;
    i32 viewport_height_ = 0;
    f32 ipd_ = 0.064f;

    // Background frame thread
    std::thread frame_thread_;
    std::atomic<bool> thread_exit_flag_{false};
    void* frame_ready_event_ = nullptr;  // HANDLE, set by frame thread
    void* frame_done_event_ = nullptr;   // HANDLE, set by Present thread

    // Data shared between frame thread and Present thread
    FrameThreadData frame_data_;

    LatencyStats latency_stats_;
};

} // namespace vrc
