#include "stereo_renderer.h"
#include "core/logging.h"
#include "core/config.h"
#include <algorithm>
#include <windows.h>

namespace vrc {

// All shared handles here come from IDXGIResource::GetSharedHandle(). They are
// legacy (non-NT) handles whose lifetime is tied to the texture, so they are
// never passed to CloseHandle.
//
// Keyed-mutex protocol for every shared texture:
//   writer (Present thread) acquires key 1 (an unread frame: overwrite it) or
//   key 0 (the reader consumed the last one), copies, releases key 1;
//   reader (XR thread) acquires key 1, copies to its local texture, releases
//   key 0. Both sides use timeout 0, so neither thread ever waits on the other.

template <typename T>
static void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static bool owns_key(HRESULT hr) {
    // WAIT_ABANDONED still transfers ownership (the other device went away
    // while holding the key). WAIT_TIMEOUT is a positive HRESULT, so
    // SUCCEEDED() cannot be used here.
    return hr == S_OK || hr == static_cast<HRESULT>(WAIT_ABANDONED);
}

// Formats for a captured colour buffer. The shared/local copies use the
// TYPELESS family so the XR side can read 8-bit frames through an *_SRGB view
// (hardware sRGB decode, correct linear-light filtering). Formats without an
// sRGB view are decoded in the shader (10-bit) or are already linear (FP16).
struct ColorFormats {
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;      // fallback texture + MSAA resolve
    DXGI_FORMAT srgb_view = DXGI_FORMAT_UNKNOWN;  // UNKNOWN if none exists
    bool linear = false;                          // float: already linear light
};

static bool color_formats(DXGI_FORMAT source, ColorFormats& out) {
    switch (source) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        out = { DXGI_FORMAT_R8G8B8A8_TYPELESS,
                source == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? source : DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false };
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        out = { DXGI_FORMAT_B8G8R8A8_TYPELESS,
                source == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? source : DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, false };
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        out = { DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM,
                DXGI_FORMAT_UNKNOWN, false };
        return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        out = { DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_UNKNOWN, true };
        return true;
    default:
        return false;
    }
}

static bool is_srgb_format(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// Shared depth copies use the typeless family of the game's depth format plus
// the SRV format that reads the depth channel as a float.
static bool depth_formats(DXGI_FORMAT source, DXGI_FORMAT& texture_format,
                          DXGI_FORMAT& srv_format) {
    switch (source) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        texture_format = DXGI_FORMAT_R32_TYPELESS;
        srv_format = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        texture_format = DXGI_FORMAT_R32G8X24_TYPELESS;
        srv_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        texture_format = DXGI_FORMAT_R24G8_TYPELESS;
        srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        texture_format = DXGI_FORMAT_R16_TYPELESS;
        srv_format = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

// Creates a keyed-mutex shared texture on the game device. KEYEDMUTEX alone
// implies sharing; it must not be combined with D3D11_RESOURCE_MISC_SHARED.
static bool create_shared_texture(ID3D11Device* device, u32 width, u32 height,
                                  DXGI_FORMAT format, ID3D11Texture2D*& texture,
                                  IDXGIKeyedMutex*& mutex, HANDLE& handle) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;

    IDXGIResource* dxgi = nullptr;
    handle = nullptr;
    if (SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        dxgi->GetSharedHandle(&handle);
        dxgi->Release();
    }
    if (!handle || FAILED(texture->QueryInterface(IID_PPV_ARGS(&mutex)))) {
        safe_release(mutex);
        safe_release(texture);
        handle = nullptr;
        return false;
    }
    return true;
}

// Writer side of the protocol above.
static bool write_shared(ID3D11DeviceContext* ctx, IDXGIKeyedMutex* mutex,
                         ID3D11Texture2D* dst, ID3D11Texture2D* src,
                         bool resolve, DXGI_FORMAT resolve_format) {
    HRESULT hr = mutex->AcquireSync(1, 0);
    if (!owns_key(hr)) hr = mutex->AcquireSync(0, 0);
    if (!owns_key(hr)) return false;  // XR thread is copying it right now: drop
    if (resolve) ctx->ResolveSubresource(dst, 0, src, 0, resolve_format);
    else         ctx->CopyResource(dst, src);
    mutex->ReleaseSync(1);
    return true;
}

StereoRenderer& StereoRenderer::instance() {
    static StereoRenderer renderer;
    return renderer;
}

bool StereoRenderer::initialize(GraphicsAPI api) {
    if (initialized_) return true;
    if (api != GraphicsAPI::D3D11 && api != GraphicsAPI::D3D12) {
        Log::error("StereoRenderer: unsupported graphics API {}", static_cast<int>(api));
        return false;
    }
    if (!openxr_.initialize(api)) {
        Log::error("OpenXR initialization failed");
        return false;
    }
    api_ = api;
    initialized_ = true;
    Log::info("StereoRenderer initialized ({})",
              api == GraphicsAPI::D3D11 ? "D3D11" : "D3D12 via D3D11On12");
    return true;
}

void StereoRenderer::shutdown() {
    // Joins the XR frame thread, which releases its XR-device resources
    // (xr_release_resources) before exiting. Idempotent, so it is safe whether
    // or not OpenXRContext::shutdown() already ran.
    openxr_.shutdown();
    xr_release_resources();

    std::lock_guard<std::mutex> lock(present_mutex_);
    release_shared_surfaces();
    safe_release(d3d11_on12_);
    if (owns_d3d11_context_ && d3d11_ctx_) d3d11_ctx_->Release();
    if (owns_d3d11_device_ && d3d11_device_) d3d11_device_->Release();
    owns_d3d11_context_ = false;
    owns_d3d11_device_ = false;
    d3d11_device_ = nullptr;
    d3d11_ctx_ = nullptr;
    logged_mode_ = static_cast<RenderMode>(0xFF);

    if (initialized_) Log::info("StereoRenderer shut down");
    initialized_ = false;
    api_ = GraphicsAPI::Unknown;
}

void StereoRenderer::set_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    d3d11_device_ = device;
    d3d11_ctx_ = ctx;
}

void StereoRenderer::release_shared_surfaces() {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    for (SharedSurface* s : { &color_, &depth_ }) {
        safe_release(s->mutex);
        safe_release(s->texture);
        *s = SharedSurface{};
    }
    color_published_ = Published{};
    depth_published_ = Published{};
    color_generation_.fetch_add(1, std::memory_order_release);
    depth_generation_.fetch_add(1, std::memory_order_release);
    last_depth_frame_.store(0, std::memory_order_release);
}

// ─── Present thread ─────────────────────────────────────────────────────────

bool StereoRenderer::on_frame_present(const FrameCapture& capture) {
    // The hook AddRefs the depth texture for this capture; release it exactly
    // once on every path.
    struct DepthReference {
        void* texture;
        ~DepthReference() {
            if (texture) static_cast<ID3D11Texture2D*>(texture)->Release();
        }
    } depth_reference{ capture.depth_texture };

    std::lock_guard<std::mutex> lock(present_mutex_);
    if (!initialized_ || !capture.resource || !d3d11_device_ || !d3d11_ctx_) return false;

    // PerfMonitor frame timing is driven once per Present by dllmain.
    const u32 frame_index = frame_count_.fetch_add(1) + 1;
    if (frame_index == 1) Log::info("First game frame received");

    const ConfigProfile cfg = Config::instance().current_profile();
    if (cfg.render_mode != logged_mode_) {
        logged_mode_ = cfg.render_mode;
        if (cfg.render_mode != RenderMode::Depth3D) {
            Log::info("Render mode: Screen");
        } else if (api_ == GraphicsAPI::D3D12) {
            Log::warn("Render mode: Depth3D needs D3D11 depth capture; this D3D12 "
                      "game is shown in Screen mode");
        } else if (!cfg.volumetric_parallax) {
            Log::info("Render mode: Depth3D selected but volumetric parallax is off; "
                      "showing Screen mode");
        } else {
            Log::info("Render mode: Depth3D (Screen mode until a usable depth buffer "
                      "is captured)");
        }
    }

    bool captured = false;
    if (openxr_.is_streaming()) {
        if (api_ == GraphicsAPI::D3D12) {
            captured = capture_d3d12(capture);
        } else {
            // Depth first, then colour: when the XR thread sees a new colour
            // frame, the depth of the same Present is already published.
            if (cfg.render_mode == RenderMode::Depth3D && cfg.volumetric_parallax &&
                capture.depth_texture &&
                capture_depth(static_cast<ID3D11Texture2D*>(capture.depth_texture))) {
                last_depth_frame_.store(frame_index, std::memory_order_release);
            }
            captured = capture_color(static_cast<ID3D11Texture2D*>(capture.resource));
        }
    }

    return captured;
}

bool StereoRenderer::capture_color(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC sd = {};
    source->GetDesc(&sd);

    ColorFormats formats;
    if (!color_formats(sd.Format, formats)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log::error("Unsupported backbuffer format {}; VR output disabled",
                       static_cast<u32>(sd.Format));
        }
        return false;
    }

    const bool changed = color_.source_format != sd.Format ||
                         color_.width != sd.Width || color_.height != sd.Height;
    if (changed) {
        std::lock_guard<std::mutex> lock(publish_mutex_);
        safe_release(color_.mutex);
        safe_release(color_.texture);
        color_ = SharedSurface{};
        color_.source_format = sd.Format;
        color_.resolve_format = formats.typed;
        color_.width = sd.Width;
        color_.height = sd.Height;

        HANDLE handle = nullptr;
        Published pub;
        if (create_shared_texture(d3d11_device_, sd.Width, sd.Height, formats.typeless,
                                  color_.texture, color_.mutex, handle)) {
            pub.srv_format = formats.srgb_view != DXGI_FORMAT_UNKNOWN ? formats.srgb_view
                                                                     : formats.typed;
            pub.decode_srgb = formats.srgb_view == DXGI_FORMAT_UNKNOWN && !formats.linear;
        } else if (create_shared_texture(d3d11_device_, sd.Width, sd.Height, formats.typed,
                                         color_.texture, color_.mutex, handle)) {
            // Some drivers refuse typeless shared textures; read the typed
            // copy and decode in the shader instead.
            pub.srv_format = formats.typed;
            pub.decode_srgb = !is_srgb_format(formats.typed) && !formats.linear;
        } else {
            Log::error("Failed to create the shared game-frame texture ({}x{}, format {})",
                       sd.Width, sd.Height, static_cast<u32>(sd.Format));
        }
        pub.handle = handle;
        color_published_ = pub;
        color_generation_.fetch_add(1, std::memory_order_release);
        if (pub.handle) {
            Log::info("Shared game frame: {}x{} format {} (view {}, shader sRGB decode {})",
                      sd.Width, sd.Height, static_cast<u32>(sd.Format),
                      static_cast<u32>(pub.srv_format), pub.decode_srgb ? "on" : "off");
        }
    }
    if (!color_.texture) return false;

    return write_shared(d3d11_ctx_, color_.mutex, color_.texture, source,
                        sd.SampleDesc.Count > 1, color_.resolve_format);
}

bool StereoRenderer::capture_depth(ID3D11Texture2D* depth) {
    D3D11_TEXTURE2D_DESC dd = {};
    depth->GetDesc(&dd);

    DXGI_FORMAT texture_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    if (dd.SampleDesc.Count != 1 || !depth_formats(dd.Format, texture_format, srv_format)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log::warn("Depth3D: game depth buffer not usable (format {}, {} samples); "
                      "showing Screen mode", static_cast<u32>(dd.Format),
                      dd.SampleDesc.Count);
        }
        return false;
    }

    const bool changed = depth_.source_format != dd.Format ||
                         depth_.width != dd.Width || depth_.height != dd.Height;
    if (changed) {
        std::lock_guard<std::mutex> lock(publish_mutex_);
        safe_release(depth_.mutex);
        safe_release(depth_.texture);
        depth_ = SharedSurface{};
        depth_.source_format = dd.Format;
        depth_.width = dd.Width;
        depth_.height = dd.Height;

        HANDLE handle = nullptr;
        Published pub;
        if (create_shared_texture(d3d11_device_, dd.Width, dd.Height, texture_format,
                                  depth_.texture, depth_.mutex, handle)) {
            pub.handle = handle;
            pub.srv_format = srv_format;
            Log::info("Shared depth buffer: {}x{} format {}", dd.Width, dd.Height,
                      static_cast<u32>(dd.Format));
        } else {
            Log::warn("Depth3D: failed to create the shared depth texture ({}x{}, format {})",
                      dd.Width, dd.Height, static_cast<u32>(dd.Format));
        }
        depth_published_ = pub;
        depth_generation_.fetch_add(1, std::memory_order_release);
    }
    if (!depth_.texture) return false;

    // Some drivers mis-handle copying a depth texture that is still bound as
    // the DSV, so unbind it for the copy (only if it is this texture).
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    d3d11_ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    bool unbound = false;
    if (dsv) {
        ID3D11Resource* bound = nullptr;
        dsv->GetResource(&bound);
        if (bound == depth) {
            d3d11_ctx_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, nullptr);
            unbound = true;
        }
        safe_release(bound);
    }

    const bool written = write_shared(d3d11_ctx_, depth_.mutex, depth_.texture, depth,
                                      false, DXGI_FORMAT_UNKNOWN);

    if (unbound) d3d11_ctx_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    for (auto*& rtv : rtvs) safe_release(rtv);
    safe_release(dsv);
    return written;
}

bool StereoRenderer::capture_d3d12(const FrameCapture& capture) {
    if (!d3d11_on12_) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log::error("D3D12 capture: no D3D11On12 device");
        }
        return false;
    }

    auto* backbuffer = static_cast<ID3D12Resource*>(capture.resource);
    ID3D11Resource* wrapped = nullptr;
    D3D11_RESOURCE_FLAGS flags = {};
    flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const HRESULT hr = d3d11_on12_->CreateWrappedResource(
        backbuffer, &flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
        IID_PPV_ARGS(&wrapped));
    if (FAILED(hr) || !wrapped) {
        static u32 failures = 0;
        if (++failures <= 5 || failures % 600 == 0) {
            Log::error("D3D12 capture: CreateWrappedResource failed 0x{:08x} (count={})",
                       static_cast<u32>(hr), failures);
        }
        safe_release(wrapped);
        return false;
    }
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(wrapped->QueryInterface(IID_PPV_ARGS(&texture)))) {
        wrapped->Release();
        return false;
    }

    d3d11_on12_->AcquireWrappedResources(&wrapped, 1);
    const bool captured = capture_color(texture);
    // ReleaseWrappedResources records the transition back to PRESENT; it must
    // precede Flush, which submits it. All wrapper references are dropped
    // before the Flush so 11On12 can destroy the per-frame wrapper right away.
    d3d11_on12_->ReleaseWrappedResources(&wrapped, 1);
    texture->Release();
    wrapped->Release();
    d3d11_ctx_->Flush();
    return captured;
}

// ─── XR frame thread ────────────────────────────────────────────────────────

static void release_xr_surface(ID3D11ShaderResourceView*& srv, ID3D11Texture2D*& local,
                               IDXGIKeyedMutex*& mutex, ID3D11Texture2D*& shared) {
    safe_release(srv);
    safe_release(local);
    safe_release(mutex);
    safe_release(shared);
}

void StereoRenderer::xr_refresh(XrSurface& s, const Published& published,
                                const std::atomic<u64>& generation, const char* what) {
    if (generation.load(std::memory_order_acquire) == s.generation) return;
    if (s.retry_delay > 0) {
        --s.retry_delay;
        return;
    }
    release_xr_surface(s.srv, s.local, s.mutex, s.shared);
    s.has_content = false;
    s.width = s.height = 0;

    // Hold the publish lock while opening so the Present thread cannot release
    // the texture behind this handle in the meantime. No GPU wait happens here.
    std::lock_guard<std::mutex> lock(publish_mutex_);
    const u64 current = generation.load(std::memory_order_acquire);
    if (!published.handle) {
        s.generation = current;  // nothing to open until the next publication
        return;
    }

    bool ok = SUCCEEDED(xr_device_->OpenSharedResource(published.handle,
                                                       IID_PPV_ARGS(&s.shared))) &&
              SUCCEEDED(s.shared->QueryInterface(IID_PPV_ARGS(&s.mutex)));
    D3D11_TEXTURE2D_DESC desc = {};
    if (ok) {
        s.shared->GetDesc(&desc);
        desc.MiscFlags = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        ok = SUCCEEDED(xr_device_->CreateTexture2D(&desc, nullptr, &s.local));
    }
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = published.srv_format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(xr_device_->CreateShaderResourceView(s.local, &srv, &s.srv));
    }
    if (!ok) {
        release_xr_surface(s.srv, s.local, s.mutex, s.shared);
        s.retry_delay = 90;  // about one second at 90 Hz
        static u32 failures = 0;
        if (++failures <= 5 || failures % 100 == 0) {
            Log::error("XR thread failed to open the shared {} (count={})", what, failures);
        }
        return;
    }
    s.width = desc.Width;
    s.height = desc.Height;
    s.decode_srgb = published.decode_srgb;
    s.generation = current;
    Log::info("XR thread opened shared {} ({}x{})", what, desc.Width, desc.Height);
}

bool StereoRenderer::xr_pull(XrSurface& s) {
    if (!s.mutex || !s.local) return false;
    if (!owns_key(s.mutex->AcquireSync(1, 0))) return false;  // nothing new
    xr_ctx_->CopyResource(s.local, s.shared);
    s.mutex->ReleaseSync(0);
    s.has_content = true;
    return true;
}

StereoRenderer::XrSource StereoRenderer::xr_update_source(ID3D11Device* xr_device,
                                                          ID3D11DeviceContext* xr_ctx) {
    XrSource out;
    if (!xr_device || !xr_ctx) return out;
    if (xr_device_ != xr_device) {
        xr_release_resources();
        xr_device_ = xr_device;
        xr_ctx_ = xr_ctx;
    }
    if (!xr_draw_.is_initialized()) {
        if (xr_draw_failed_) return out;
        if (!xr_draw_.initialize(xr_device_)) {
            xr_draw_failed_ = true;
            Log::error("XR draw pipeline unavailable; nothing will be shown in the headset");
            return out;
        }
    }

    xr_refresh(xr_color_, color_published_, color_generation_, "game frame");
    if (xr_pull(xr_color_)) ++xr_color_frames_;

    const u32 last_depth = last_depth_frame_.load(std::memory_order_acquire);
    const bool depth_current =
        last_depth != 0 && frame_count_.load() - last_depth <= kDepthStaleFrames;
    if (depth_current) {
        xr_refresh(xr_depth_, depth_published_, depth_generation_, "depth buffer");
        xr_pull(xr_depth_);
    }

    out.has_color = xr_color_.srv && xr_color_.has_content;
    out.width = xr_color_.width;
    out.height = xr_color_.height;
    out.color_generation = xr_color_frames_;
    out.has_depth = depth_current && xr_depth_.srv && xr_depth_.has_content;
    return out;
}

void StereoRenderer::xr_draw_screen(ID3D11RenderTargetView* rtv, u32 width, u32 height) {
    if (!xr_ctx_) return;
    xr_draw_.draw_screen(xr_ctx_, { xr_color_.srv, xr_color_.decode_srgb }, rtv,
                         width, height);
}

void StereoRenderer::xr_draw_depth3d_eye(int eye, const XrView (&views)[2],
                                         const ConfigProfile& cfg,
                                         ID3D11RenderTargetView* rtv,
                                         u32 width, u32 height) {
    if (!xr_ctx_ || !xr_color_.srv || !xr_depth_.srv || eye < 0 || eye > 1) return;

    const f32 aspect = static_cast<f32>(xr_color_.width) /
                       static_cast<f32>(std::max(xr_color_.height, 1u));
    const reproj::GameCamera camera = reproj::make_game_camera(
        cfg.depth_fov_degrees, aspect, cfg.depth_near_plane, cfg.depth_far_plane,
        cfg.depth_reversed_z);

    // Orientation and FOV are exactly those submitted for this eye. swap_eyes
    // exchanges only the viewpoint, so each eye sees the other eye's parallax.
    const int content_eye = cfg.swap_eyes ? 1 - eye : eye;
    const XrVector3f& p = views[content_eye].pose.position;
    const XrQuaternionf& q = views[eye].pose.orientation;
    const XrFovf& fov = views[eye].fov;
    const reproj::EyeView view = reproj::make_eye_view(
        reproj::content_eye_position({ p.x, p.y, p.z }, cfg.parallax_strength,
                                     cfg.world_scale),
        Quat(q.x, q.y, q.z, q.w), fov.angleLeft, fov.angleRight, fov.angleUp,
        fov.angleDown);

    // Tint by the eye the image is rendered FOR (after swap_eyes).
    const i32 tint = cfg.show_eye_debug ? (eye == 0 ? 1 : 2) : 0;
    xr_draw_.draw_depth3d(xr_ctx_, { xr_color_.srv, xr_color_.decode_srgb },
                          xr_depth_.srv, view, camera, tint, rtv, width, height);
}

void StereoRenderer::xr_release_resources() {
    for (XrSurface* s : { &xr_color_, &xr_depth_ }) {
        release_xr_surface(s->srv, s->local, s->mutex, s->shared);
        *s = XrSurface{};
    }
    xr_draw_.shutdown();
    xr_draw_failed_ = false;
    xr_color_frames_ = 0;
    xr_device_ = nullptr;
    xr_ctx_ = nullptr;
}

} // namespace vrc
