#pragma once

#include "core/types.h"
#include "core/perf_monitor.h"
#include "render/depth_reprojection.h"
#include "vr/openxr_context.h"
#include <d3d11.h>
#include <d3d11on12.h>
#include <atomic>
#include <mutex>

namespace vrc {

// Moves game frames from the game's Present thread to the OpenXR frame thread
// and draws them into XR swapchain images.
//
// Present thread (on_frame_present): copies the backbuffer — and, for Depth3D,
// the game depth buffer — into keyed-mutex shared textures with the game's
// device. It never blocks: if the XR thread holds a key, that copy is dropped.
//
// XR frame thread (xr_* methods, called only by OpenXRContext): when a new
// frame was published it copies the shared texture into an XR-device-local
// texture and releases the key at once. All drawing reads the local copies, so
// with no new game frame the last one is simply shown again.
class StereoRenderer {
public:
    static StereoRenderer& instance();

    bool initialize(GraphicsAPI api);
    // Safe to call repeatedly and before or after OpenXRContext::shutdown().
    void shutdown();
    bool is_initialized() const { return initialized_; }

    bool on_frame_present(const FrameCapture& capture);

    u32 frame_count() const { return frame_count_.load(); }

    // The game's D3D11 device (or our D3D11On12 device for D3D12 games); used
    // only on the Present thread.
    void set_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx);
    // Native D3D11 devices are borrowed from the game; D3D11On12 devices are
    // created by us. The context from GetImmediateContext() AddRefs and is owned.
    void set_d3d11_device_ownership(bool owns_device, bool owns_context) {
        owns_d3d11_device_ = owns_device;
        owns_d3d11_context_ = owns_context;
    }
    void set_d3d11_on12_device(ID3D11On12Device* device) { d3d11_on12_ = device; }

    PerfMonitor& perf() { return perf_; }

    // ─── OpenXR frame thread interface ────────────────────────────────────
    struct XrSource {
        bool has_color = false;    // a game frame is available to draw
        u32 width = 0;             // game frame size
        u32 height = 0;
        u64 color_generation = 0;  // increments with every new game frame copied
        bool has_depth = false;    // depth from the current capture is available
    };

    // Picks up newly published frames on the XR device. Call once per XR frame
    // before drawing.
    XrSource xr_update_source(ID3D11Device* xr_device, ID3D11DeviceContext* xr_ctx);
    // Screen mode: the whole game frame into the quad swapchain image.
    void xr_draw_screen(ID3D11RenderTargetView* rtv, u32 width, u32 height);
    // Depth3D: the view rendered FOR swapchain eye `eye`, using the poses and
    // FOVs the projection layer submits (views are LOCAL-space xrLocateViews).
    void xr_draw_depth3d_eye(int eye, const XrView (&views)[2], const ConfigProfile& cfg,
                             ID3D11RenderTargetView* rtv, u32 width, u32 height);
    // Releases every XR-device resource. Called by the frame thread as it exits.
    void xr_release_resources();

private:
    StereoRenderer() = default;

    // Present-thread side of one shared texture (created on the game device).
    struct SharedSurface {
        ID3D11Texture2D* texture = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;  // to detect changes
        DXGI_FORMAT resolve_format = DXGI_FORMAT_UNKNOWN; // typed format for MSAA resolve
        u32 width = 0;
        u32 height = 0;
    };
    // What the XR thread needs to open a SharedSurface. Guarded by publish_mutex_.
    struct Published {
        HANDLE handle = nullptr;  // legacy GetSharedHandle(): never CloseHandle
        DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
        bool decode_srgb = false;
    };
    // XR-thread side: the opened shared texture plus a local copy to draw from.
    struct XrSurface {
        ID3D11Texture2D* shared = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        ID3D11Texture2D* local = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        u32 width = 0;
        u32 height = 0;
        bool decode_srgb = false;
        bool has_content = false;
        u64 generation = 0;   // publication generation this was opened from
        u32 retry_delay = 0;  // XR frames to wait before retrying a failed open
    };

    bool capture_color(ID3D11Texture2D* source);
    bool capture_depth(ID3D11Texture2D* depth);
    bool capture_d3d12(const FrameCapture& capture);
    void release_shared_surfaces();

    void xr_refresh(XrSurface& surface, const Published& published,
                    const std::atomic<u64>& generation, const char* what);
    bool xr_pull(XrSurface& surface);

    GraphicsAPI api_ = GraphicsAPI::Unknown;
    bool initialized_ = false;
    std::atomic<u32> frame_count_{0};
    RenderMode logged_mode_ = static_cast<RenderMode>(0xFF);

    OpenXRContext& openxr_ = OpenXRContext::instance();
    PerfMonitor& perf_ = PerfMonitor::instance();

    // Present thread ───────────────────────────────────────────────────────
    // Serialises on_frame_present() against shutdown(). The XR thread never
    // takes it.
    std::mutex present_mutex_;
    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_ctx_ = nullptr;
    bool owns_d3d11_device_ = false;
    bool owns_d3d11_context_ = false;
    ID3D11On12Device* d3d11_on12_ = nullptr;
    SharedSurface color_;
    SharedSurface depth_;

    // Publication (Present -> XR) ──────────────────────────────────────────
    // Held only while a shared texture is replaced and while the XR thread
    // opens it, so a handle is never opened after its texture was released.
    // No GPU or keyed-mutex wait ever happens under it.
    std::mutex publish_mutex_;
    Published color_published_;
    Published depth_published_;
    std::atomic<u64> color_generation_{0};
    std::atomic<u64> depth_generation_{0};
    // frame_count_ of the last Present that copied a depth buffer (0 = none).
    // Depth counts as current for kDepthStaleFrames game frames, so a depth
    // buffer missing on the odd frame does not flip Depth3D <-> Screen.
    std::atomic<u32> last_depth_frame_{0};
    static constexpr u32 kDepthStaleFrames = 15;

    // XR frame thread ──────────────────────────────────────────────────────
    ID3D11Device* xr_device_ = nullptr;      // borrowed from OpenXRContext
    ID3D11DeviceContext* xr_ctx_ = nullptr;
    XrSurface xr_color_;
    XrSurface xr_depth_;
    u64 xr_color_frames_ = 0;
    DepthReprojection xr_draw_;
    bool xr_draw_failed_ = false;
};

} // namespace vrc
