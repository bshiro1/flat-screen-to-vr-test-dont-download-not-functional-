#include "openxr_context.h"
#include "render/stereo_renderer.h"
#include "render/reprojection_math.h"
#include "input/xr_input.h"
#include "input/input_mapper.h"
#include "vr/tracking.h"
#include "core/logging.h"
#include "core/config.h"
#include <d3d11_4.h>
#include <dxgi.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace vrc {

template <typename T>
static void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static bool same_luid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static std::string narrow(const wchar_t* text) {
    char buffer[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
    return buffer;
}

static const char* session_state_name(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE:         return "IDLE";
    case XR_SESSION_STATE_READY:        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:     return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:      return "EXITING";
    default:                            return "UNKNOWN";
    }
}

// Smaller of two limits where 0 means "no limit".
static u32 min_limit(u32 a, u32 b) {
    if (a == 0) return b;
    if (b == 0) return a;
    return std::min(a, b);
}

OpenXRContext& OpenXRContext::instance() {
    static OpenXRContext ctx;
    return ctx;
}

// ─── Lifetime ───────────────────────────────────────────────────────────────

bool OpenXRContext::initialize(GraphicsAPI api) {
    if (initialized_.load()) return true;
    Log::info("Initializing OpenXR (game API {}; the XR session always uses D3D11)",
              static_cast<int>(api));
    if (!create_instance() || !get_system()) {
        destroy_instance();
        return false;
    }
    initialized_.store(true, std::memory_order_release);
    return true;
}

void OpenXRContext::shutdown() {
    // The frame thread destroys the session and the renderer's XR-device
    // resources as it exits.
    stop_frame_thread();
    destroy_session();
    destroy_instance();
    safe_release(xr_ctx_);
    safe_release(xr_device_);
    xr_adapter_luid_ = {};
    device_bound_.store(false, std::memory_order_release);
    if (initialized_.exchange(false)) Log::info("OpenXR shut down");
}

void OpenXRContext::bind_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    // One attempt only: dllmain retries binding on every Present while this is
    // false, which would leak a context reference (or a D3D11On12 device) per
    // frame if setup kept failing.
    if (device_bound_.exchange(true)) return;

    // The renderer copies frames with the game device and owns the references
    // handed over by dllmain, even if VR cannot start below.
    StereoRenderer::instance().set_d3d11_device(device, ctx);
    if (!device || !initialized_.load()) {
        Log::error("bind_d3d11_device: {}", device ? "OpenXR is not initialized" : "no device");
        return;
    }
    if (!create_xr_device(device)) return;
    start_frame_thread();
}

bool OpenXRContext::graphics_requirements(LUID& adapter_luid,
                                          D3D_FEATURE_LEVEL& min_feature_level) {
    PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(
            instance_, "xrGetD3D11GraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) ||
        !get_requirements) {
        Log::error("xrGetD3D11GraphicsRequirementsKHR is unavailable");
        return false;
    }
    XrGraphicsRequirementsD3D11KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    const XrResult res = get_requirements(instance_, system_id_, &requirements);
    if (XR_FAILED(res)) {
        handle_result(res, "xrGetD3D11GraphicsRequirementsKHR", session_log_);
        return false;
    }
    adapter_luid = requirements.adapterLuid;
    min_feature_level = requirements.minFeatureLevel;
    return true;
}

bool OpenXRContext::create_xr_device(ID3D11Device* game_device) {
    LUID required = {};
    D3D_FEATURE_LEVEL min_level = D3D_FEATURE_LEVEL_11_0;
    if (!graphics_requirements(required, min_level)) return false;

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapter_desc = {};
    if (FAILED(game_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapter_desc))) {
        Log::error("Cannot identify the game's GPU adapter");
        safe_release(adapter);
        safe_release(dxgi_device);
        return false;
    }
    safe_release(dxgi_device);

    const std::string adapter_name = narrow(adapter_desc.Description);
    if (!same_luid(adapter_desc.AdapterLuid, required)) {
        Log::error("The OpenXR runtime needs the GPU with adapter LUID {:08x}:{:08x}, but "
                   "the game renders on '{}' (LUID {:08x}:{:08x}). Game frames cannot be "
                   "shared across GPUs, so VR output is disabled. Run the game on the GPU "
                   "the headset is connected to.",
                   static_cast<u32>(required.HighPart), required.LowPart, adapter_name,
                   static_cast<u32>(adapter_desc.AdapterLuid.HighPart),
                   adapter_desc.AdapterLuid.LowPart);
        adapter->Release();
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL actual = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   &xr_device_, &actual, &xr_ctx_);
    if (hr == E_INVALIDARG) {
        // Pre-11.1 runtimes reject D3D_FEATURE_LEVEL_11_1 in the list.
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
                               static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                               &xr_device_, &actual, &xr_ctx_);
    }
    adapter->Release();
    if (FAILED(hr) || !xr_device_ || !xr_ctx_) {
        Log::error("Failed to create the XR D3D11 device: 0x{:08x}", static_cast<u32>(hr));
        safe_release(xr_ctx_);
        safe_release(xr_device_);
        return false;
    }
    if (actual < min_level) {
        Log::error("The XR device reached feature level 0x{:x}, the runtime needs 0x{:x}",
                   static_cast<u32>(actual), static_cast<u32>(min_level));
        safe_release(xr_ctx_);
        safe_release(xr_device_);
        return false;
    }

    // Runtimes may touch the session device from their own threads. Protect
    // the XR device only; the game's device is left alone.
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(xr_device_->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }

    xr_adapter_luid_ = required;
    Log::info("XR D3D11 device created on '{}' (feature level 0x{:x})", adapter_name,
              static_cast<u32>(actual));
    return true;
}

// ─── Instance / system ──────────────────────────────────────────────────────

bool OpenXRContext::create_instance() {
    htcx_tracker_supported_ = false;
    hand_tracking_supported_ = false;

    u32 ext_count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr))) {
        if (session_log_.should_log()) {
            Log::error("No OpenXR runtime available (is the headset software running?)");
        }
        return false;
    }
    std::vector<XrExtensionProperties> available(ext_count, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, available.data());
    auto has_extension = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const auto& ext) {
            return strcmp(ext.extensionName, name) == 0;
        });
    };

    if (!has_extension(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
        Log::error("The active OpenXR runtime does not support XR_KHR_D3D11_enable");
        return false;
    }
    // Only request what the runtime offers; an unavailable extension makes
    // xrCreateInstance fail with XR_ERROR_EXTENSION_NOT_PRESENT.
    std::vector<const char*> extensions{ XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    if (has_extension(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME)) {
        extensions.push_back(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
        htcx_tracker_supported_ = true;
    }
    if (has_extension(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        hand_tracking_supported_ = true;
    }

    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "VR Game Converter");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "VR Converter Engine");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = static_cast<u32>(extensions.size());
    ci.enabledExtensionNames = extensions.data();

    const XrResult res = xrCreateInstance(&ci, &instance_);
    if (XR_FAILED(res)) {
        instance_ = XR_NULL_HANDLE;
        if (session_log_.should_log()) Log::error("xrCreateInstance failed: {}", static_cast<int>(res));
        return false;
    }

    XrInstanceProperties props{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(instance_, &props);
    Log::info("OpenXR runtime: {} v{}.{}.{} (HTCX trackers {}, hand tracking {})",
              props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
              XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion),
              htcx_tracker_supported_ ? "yes" : "no", hand_tracking_supported_ ? "yes" : "no");
    return true;
}

bool OpenXRContext::get_system() {
    XrSystemGetInfo info{ XR_TYPE_SYSTEM_GET_INFO };
    info.formFactor = form_factor_;
    XrResult res = xrGetSystem(instance_, &info, &system_id_);
    if (XR_FAILED(res)) {
        system_id_ = XR_NULL_SYSTEM_ID;
        if (session_log_.should_log()) {
            Log::error("xrGetSystem failed ({}): is the headset connected and on?",
                       static_cast<int>(res));
        }
        return false;
    }

    XrSystemProperties props{ XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(instance_, system_id_, &props))) {
        max_swapchain_width_ = props.graphicsProperties.maxSwapchainImageWidth;
        max_swapchain_height_ = props.graphicsProperties.maxSwapchainImageHeight;
    }

    u32 view_count = 0;
    xrEnumerateViewConfigurationViews(instance_, system_id_, view_config_, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    res = xrEnumerateViewConfigurationViews(instance_, system_id_, view_config_, view_count,
                                            &view_count, views.data());
    if (XR_FAILED(res) || view_count < 2) {
        Log::error("The OpenXR system has no stereo view configuration");
        return false;
    }
    eye_recommended_width_ = views[0].recommendedImageRectWidth;
    eye_recommended_height_ = views[0].recommendedImageRectHeight;
    eye_max_width_ = views[0].maxImageRectWidth;
    eye_max_height_ = views[0].maxImageRectHeight;

    Log::info("OpenXR system '{}': eye {}x{} recommended, swapchain max {}x{}",
              props.systemName, eye_recommended_width_, eye_recommended_height_,
              max_swapchain_width_, max_swapchain_height_);
    return true;
}

void OpenXRContext::destroy_instance() {
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
    system_id_ = XR_NULL_SYSTEM_ID;
}

// ─── Frame thread ───────────────────────────────────────────────────────────

void OpenXRContext::start_frame_thread() {
    if (frame_thread_.joinable()) return;
    exit_flag_.store(false);
    streaming_.store(true, std::memory_order_release);
    frame_thread_ = std::thread(&OpenXRContext::frame_thread_proc, this);
}

void OpenXRContext::stop_frame_thread() {
    if (!frame_thread_.joinable()) return;
    exit_flag_.store(true);
    frame_thread_.join();
    streaming_.store(false, std::memory_order_release);
}

void OpenXRContext::sleep_interruptible(u32 ms) {
    for (u32 slept = 0; slept < ms && !exit_flag_.load(); slept += 10) {
        Sleep(std::min(10u, ms - slept));
    }
}

void OpenXRContext::frame_thread_proc() {
    Log::info("OpenXR frame thread started");

    // Recovery attempts (instance or session creation after a failure or a
    // loss) back off 0.5 s, 1 s, 2 s, 4 s, then 8 s between tries.
    u32 failures = 0;
    auto back_off = [&]() {
        sleep_interruptible(500u << std::min(failures, 4u));
        ++failures;
    };

    while (!exit_flag_.load()) {
        if (instance_lost_) {
            Log::warn("OpenXR instance lost; recreating it");
            destroy_session();
            destroy_instance();
            instance_lost_ = false;
        }
        if (instance_ == XR_NULL_HANDLE && (!create_instance() || !get_system())) {
            destroy_instance();
            back_off();
            continue;
        }
        if (session_ == XR_NULL_HANDLE && !create_session()) {
            back_off();
            continue;
        }

        poll_events();
        if (instance_lost_ || session_lost_) {
            destroy_session();
            back_off();
            continue;
        }
        if (!session_running_) {
            // IDLE (e.g. headset in standby or after STOPPING): wait for READY.
            sleep_interruptible(20);
            continue;
        }
        if (run_frame()) failures = 0;
    }

    destroy_session();
    StereoRenderer::instance().xr_release_resources();
    Log::info("OpenXR frame thread stopped");
}

bool OpenXRContext::create_session() {
    if (!xr_device_) return false;

    // Required before xrCreateSession, for every instance.
    LUID luid = {};
    D3D_FEATURE_LEVEL min_level = D3D_FEATURE_LEVEL_11_0;
    if (!graphics_requirements(luid, min_level)) return false;
    if (!same_luid(luid, xr_adapter_luid_)) {
        if (session_log_.should_log()) {
            Log::error("The OpenXR runtime now requires a different GPU than the one the "
                       "XR device was created on; restart the game");
        }
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = xr_device_;
    XrSessionCreateInfo ci{ XR_TYPE_SESSION_CREATE_INFO };
    ci.next = &binding;
    ci.systemId = system_id_;
    XrResult res = xrCreateSession(instance_, &ci, &session_);
    if (XR_FAILED(res)) {
        session_ = XR_NULL_HANDLE;
        handle_result(res, "xrCreateSession", session_log_);
        return false;
    }

    XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    res = xrCreateReferenceSpace(session_, &space_info, &local_space_);
    if (XR_SUCCEEDED(res)) {
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        res = xrCreateReferenceSpace(session_, &space_info, &view_space_);
    }
    if (XR_FAILED(res)) {
        handle_result(res, "xrCreateReferenceSpace", session_log_);
        destroy_session();
        return false;
    }
    if (!choose_swapchain_format()) {
        destroy_session();
        return false;
    }

    // Action sets must be attached before xrBeginSession.
    XrInput::instance().configure_extensions(htcx_tracker_supported_, hand_tracking_supported_);
    if (!XrInput::instance().initialize(instance_, session_)) {
        Log::error("OpenXR input initialisation failed; retrying session setup");
        destroy_session();
        return false;
    }

    render_scale_ = std::clamp(Config::instance().current_profile().render_scale, 0.25f, 4.0f);
    session_state_ = XR_SESSION_STATE_UNKNOWN;
    session_running_ = false;
    session_lost_ = false;
    quad_has_image_ = false;
    quad_generation_ = 0;
    active_layer_ = -1;
    Log::info("OpenXR session created (render scale {:.2f})", render_scale_);
    return true;
}

void OpenXRContext::destroy_session() {
    if (session_ == XR_NULL_HANDLE) return;
    // Input action spaces belong to this session.
    XrInput::instance().shutdown();
    destroy_swapchain(quad_);
    destroy_swapchain(eyes_[0]);
    destroy_swapchain(eyes_[1]);
    if (view_space_ != XR_NULL_HANDLE) xrDestroySpace(view_space_);
    if (local_space_ != XR_NULL_HANDLE) xrDestroySpace(local_space_);
    view_space_ = XR_NULL_HANDLE;
    local_space_ = XR_NULL_HANDLE;
    xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    session_state_ = XR_SESSION_STATE_UNKNOWN;
    session_running_ = false;
    session_lost_ = false;
    swapchain_format_ = 0;
    quad_has_image_ = false;
    quad_generation_ = 0;
    active_layer_ = -1;
    Log::info("OpenXR session destroyed");
}

bool OpenXRContext::choose_swapchain_format() {
    u32 count = 0;
    xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
    std::vector<i64> formats(count);
    if (count == 0 ||
        XR_FAILED(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()))) {
        Log::error("xrEnumerateSwapchainFormats returned no formats");
        return false;
    }

    // The runtime lists formats in its order of preference; take its first
    // match. sRGB first: the shaders write linear colour and the hardware
    // encodes it, which is what OpenXR expects of an sRGB swapchain.
    auto pick = [&](std::initializer_list<DXGI_FORMAT> wanted) {
        for (i64 format : formats) {
            for (DXGI_FORMAT w : wanted) {
                if (format == static_cast<i64>(w)) {
                    swapchain_format_ = format;
                    return true;
                }
            }
        }
        return false;
    };
    if (pick({ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB })) {
        Log::info("Swapchain format {} (sRGB)", swapchain_format_);
        return true;
    }
    if (pick({ DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM })) {
        // OpenXR treats UNORM images as linear, so linear colour is still
        // correct, just with 8-bit banding in dark areas.
        Log::warn("No 8-bit sRGB swapchain format offered; using UNORM format {} "
                  "(dark gradients may band)", swapchain_format_);
        return true;
    }
    Log::error("None of the runtime's {} swapchain formats is supported", count);
    return false;
}

bool OpenXRContext::create_swapchain(Swapchain& sc, u32 width, u32 height, const char* label) {
    destroy_swapchain(sc);

    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = swapchain_format_;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult res = xrCreateSwapchain(session_, &ci, &sc.handle);
    if (XR_FAILED(res)) {
        sc.handle = XR_NULL_HANDLE;
        handle_result(res, "xrCreateSwapchain", swapchain_log_);
        return false;
    }

    u32 count = 0;
    xrEnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    res = xrEnumerateSwapchainImages(sc.handle, count, &count,
                                     reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(res) || count == 0) {
        handle_result(res, "xrEnumerateSwapchainImages", swapchain_log_);
        destroy_swapchain(sc);
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = static_cast<DXGI_FORMAT>(swapchain_format_);
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (const auto& image : images) {
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(xr_device_->CreateRenderTargetView(image.texture, &rtv_desc, &rtv))) {
            Log::error("{} swapchain: CreateRenderTargetView failed (format {})", label,
                       swapchain_format_);
            destroy_swapchain(sc);
            return false;
        }
        sc.rtvs.push_back(rtv);
    }
    sc.width = width;
    sc.height = height;
    Log::info("{} swapchain created: {}x{} ({} images)", label, width, height, count);
    return true;
}

void OpenXRContext::destroy_swapchain(Swapchain& sc) {
    for (auto*& rtv : sc.rtvs) safe_release(rtv);
    sc.rtvs.clear();
    if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);
    sc.handle = XR_NULL_HANDLE;
    sc.width = 0;
    sc.height = 0;
}

bool OpenXRContext::acquire_image(Swapchain& sc, u32& index) {
    XrSwapchainImageAcquireInfo acquire_info{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrResult res = xrAcquireSwapchainImage(sc.handle, &acquire_info, &index);
    if (XR_FAILED(res)) {
        handle_result(res, "xrAcquireSwapchainImage", swapchain_log_);
        return false;
    }

    // A timeout is not an error: keep waiting, but stay responsive to exit.
    // Only a successful wait makes the image writable and releasable; an
    // image left acquired on exit is reclaimed when the swapchain is destroyed.
    XrSwapchainImageWaitInfo wait_info{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait_info.timeout = 100'000'000;  // 100 ms
    for (;;) {
        res = xrWaitSwapchainImage(sc.handle, &wait_info);
        if (res != XR_TIMEOUT_EXPIRED) break;
        if (exit_flag_.load()) return false;
        if (swapchain_log_.should_log()) {
            Log::warn("xrWaitSwapchainImage timed out; still waiting (count={})",
                      swapchain_log_.count);
        }
    }
    if (XR_FAILED(res)) {
        handle_result(res, "xrWaitSwapchainImage", swapchain_log_);
        return false;
    }
    handle_result(res, "xrWaitSwapchainImage", swapchain_log_);  // XR_SESSION_LOSS_PENDING

    if (index >= sc.rtvs.size()) {
        Log::error("Swapchain image index {} out of range ({} images)", index, sc.rtvs.size());
        release_image(sc);
        return false;
    }
    return true;
}

bool OpenXRContext::release_image(Swapchain& sc) {
    XrSwapchainImageReleaseInfo release_info{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    const XrResult res = xrReleaseSwapchainImage(sc.handle, &release_info);
    if (XR_FAILED(res)) {
        handle_result(res, "xrReleaseSwapchainImage", swapchain_log_);
        return false;
    }
    return true;
}

void OpenXRContext::handle_result(XrResult result, const char* what, LogLimiter& limiter) {
    if (result == XR_ERROR_INSTANCE_LOST) {
        if (!instance_lost_) Log::warn("{}: OpenXR instance lost", what);
        instance_lost_ = true;
    } else if (result == XR_ERROR_SESSION_LOST || result == XR_SESSION_LOSS_PENDING) {
        if (!session_lost_) {
            Log::warn("{}: OpenXR session lost ({}); it will be recreated", what,
                      static_cast<int>(result));
        }
        session_lost_ = true;
    } else if (XR_FAILED(result) && limiter.should_log()) {
        Log::warn("{} failed: {} (count={})", what, static_cast<int>(result), limiter.count);
    }
}

void OpenXRContext::poll_events() {
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    for (;;) {
        const XrResult res = xrPollEvent(instance_, &event);
        if (res == XR_EVENT_UNAVAILABLE) break;
        if (XR_FAILED(res)) {
            handle_result(res, "xrPollEvent", event_log_);
            break;
        }

        switch (event.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            Log::warn("OpenXR instance loss pending");
            instance_lost_ = true;
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed =
                *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed.session != session_) break;
            session_state_ = changed.state;
            Log::info("OpenXR session state: {}", session_state_name(changed.state));

            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin_info{ XR_TYPE_SESSION_BEGIN_INFO };
                begin_info.primaryViewConfigurationType = view_config_;
                const XrResult begin = xrBeginSession(session_, &begin_info);
                if (XR_SUCCEEDED(begin)) {
                    session_running_ = true;
                } else {
                    handle_result(begin, "xrBeginSession", session_log_);
                }
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                // Dashboard / standby: end the session and wait for READY again.
                const XrResult end = xrEndSession(session_);
                if (XR_FAILED(end)) handle_result(end, "xrEndSession", session_log_);
                session_running_ = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING ||
                       changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                session_running_ = false;
                session_lost_ = true;
            }
            break;
        }
        default:
            break;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

bool OpenXRContext::run_frame() {
    XrFrameWaitInfo wait_info{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
    XrResult res = xrWaitFrame(session_, &wait_info, &frame_state);
    if (XR_FAILED(res)) {
        handle_result(res, "xrWaitFrame", frame_log_);
        sleep_interruptible(10);  // a failing xrWaitFrame returns at once; don't spin
        return false;
    }
    handle_result(res, "xrWaitFrame", frame_log_);  // XR_SESSION_LOSS_PENDING

    XrFrameBeginInfo begin_info{ XR_TYPE_FRAME_BEGIN_INFO };
    res = xrBeginFrame(session_, &begin_info);
    if (XR_FAILED(res)) {
        handle_result(res, "xrBeginFrame", frame_log_);
        return false;
    }
    // XR_FRAME_DISCARDED only says an earlier frame was dropped; this frame
    // has begun and is ended below like any other.

    // Eye poses in LOCAL space. Depth3D renders with exactly these poses and
    // FOVs and submits them unchanged.
    XrViewLocateInfo locate_info{ XR_TYPE_VIEW_LOCATE_INFO };
    locate_info.viewConfigurationType = view_config_;
    locate_info.displayTime = frame_state.predictedDisplayTime;
    locate_info.space = local_space_;
    XrViewState view_state{ XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    u32 view_count = 0;
    res = xrLocateViews(session_, &locate_info, &view_state, 2, &view_count, views);
    constexpr XrViewStateFlags kPoseValid =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    const bool views_valid = XR_SUCCEEDED(res) && view_count == 2 &&
                             (view_state.viewStateFlags & kPoseValid) == kPoseValid;
    if (XR_FAILED(res)) {
        handle_result(res, "xrLocateViews", locate_log_);
    } else if (!views_valid && locate_log_.should_log()) {
        Log::warn("xrLocateViews: eye poses not valid (flags 0x{:x}, count={})",
                  view_state.viewStateFlags, locate_log_.count);
    }

    // Input: 6DoF bridge, controller actions and head-aim.
    const auto view_flags = static_cast<XrSpaceLocationFlags>(view_state.viewStateFlags);
    XrInput::instance().set_head_pose_from_views(views[0].pose, views[1].pose, view_flags);
    XrInput::instance().set_frame_metrics(frame_state.predictedDisplayPeriod, views,
                                          view_count, view_flags);
    if (view_count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        const XrPosef& l = views[0].pose;
        const XrPosef& r = views[1].pose;
        TrackingSystem::instance().set_head_pose(
            Vec3((l.position.x + r.position.x) * 0.5f, (l.position.y + r.position.y) * 0.5f,
                 (l.position.z + r.position.z) * 0.5f),
            quat_slerp(Quat(l.orientation.x, l.orientation.y, l.orientation.z, l.orientation.w),
                       Quat(r.orientation.x, r.orientation.y, r.orientation.z, r.orientation.w),
                       0.5f),
            static_cast<f64>(frame_state.predictedDisplayTime) / 1'000'000.0);
    }
    if (XrInput::instance().is_active()) {
        XrInput::instance().sync(session_, local_space_, frame_state.predictedDisplayTime);
    }
    InputMapper::instance().process_frame(
        static_cast<f64>(frame_state.predictedDisplayPeriod) / 1'000'000.0);

    // Content.
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView projection_views[2] = {
        { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
        { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
    };
    const XrCompositionLayerBaseHeader* layer = nullptr;
    int layer_kind = 0;  // 0 = none, 1 = Screen quad, 2 = Depth3D projection
    if (frame_state.shouldRender && !exit_flag_.load()) {
        auto& renderer = StereoRenderer::instance();
        const auto source = renderer.xr_update_source(xr_device_, xr_ctx_);
        if (source.has_color) {
            const ConfigProfile cfg = Config::instance().current_profile();
            const bool depth3d = cfg.render_mode == RenderMode::Depth3D &&
                                 cfg.volumetric_parallax && source.has_depth && views_valid;
            if (depth3d && render_projection_layer(cfg, views, projection, projection_views)) {
                layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                layer_kind = 2;
            } else if (render_quad_layer(cfg, source.width, source.height,
                                         source.color_generation, quad)) {
                layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                layer_kind = 1;
            }
            xr_ctx_->Flush();
        }
    }
    if (layer_kind != active_layer_) {
        active_layer_ = layer_kind;
        static LogLimiter layer_log;
        if (layer_log.should_log()) {
            Log::info("Submitting {}", layer_kind == 2 ? "Depth3D projection layer"
                                     : layer_kind == 1 ? "Screen quad layer"
                                                       : "no layer (no game frame yet)");
        }
    }

    XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = layer ? 1 : 0;
    end_info.layers = layer ? &layer : nullptr;
    res = xrEndFrame(session_, &end_info);
    if (XR_FAILED(res)) {
        handle_result(res, "xrEndFrame", frame_log_);
        return false;
    }
    return true;
}

bool OpenXRContext::render_quad_layer(const ConfigProfile& cfg, u32 source_width,
                                      u32 source_height, u64 source_generation,
                                      XrCompositionLayerQuad& layer) {
    const reproj::Extent extent = reproj::fit_swapchain_extent(
        source_width, source_height, render_scale_, max_swapchain_width_,
        max_swapchain_height_);
    if (quad_.handle == XR_NULL_HANDLE || quad_.width != extent.width ||
        quad_.height != extent.height) {
        quad_has_image_ = false;
        if (!create_swapchain(quad_, extent.width, extent.height, "Screen")) return false;
    }

    // Write only when a new game frame arrived. Otherwise the layer is simply
    // resubmitted: the runtime shows the most recently released image.
    if (!quad_has_image_ || quad_generation_ != source_generation) {
        u32 index = 0;
        if (!acquire_image(quad_, index)) return false;
        StereoRenderer::instance().xr_draw_screen(quad_.rtvs[index], quad_.width, quad_.height);
        if (!release_image(quad_)) return false;
        quad_has_image_ = true;
        quad_generation_ = source_generation;
    }

    const reproj::ScreenQuad screen = reproj::screen_quad(
        cfg.convergence_distance, cfg.fov_override, cfg.world_scale,
        static_cast<f32>(source_width) / static_cast<f32>(std::max(source_height, 1u)));
    layer.layerFlags = 0;
    // World-locked screens live in LOCAL (eye level at the starting head
    // position); head-locked screens in VIEW. Either way: straight ahead.
    layer.space = cfg.enable_head_tracking ? local_space_ : view_space_;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = quad_.handle;
    layer.subImage.imageRect.offset = { 0, 0 };
    layer.subImage.imageRect.extent = { static_cast<i32>(quad_.width),
                                        static_cast<i32>(quad_.height) };
    layer.subImage.imageArrayIndex = 0;
    layer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    layer.pose.position = { 0.0f, 0.0f, -screen.distance };
    layer.size = { screen.width, screen.height };
    return true;
}

bool OpenXRContext::render_projection_layer(const ConfigProfile& cfg,
                                            const XrView (&views)[2],
                                            XrCompositionLayerProjection& layer,
                                            XrCompositionLayerProjectionView (&layer_views)[2]) {
    const reproj::Extent extent = reproj::fit_swapchain_extent(
        eye_recommended_width_, eye_recommended_height_, render_scale_,
        min_limit(eye_max_width_, max_swapchain_width_),
        min_limit(eye_max_height_, max_swapchain_height_));
    for (int eye = 0; eye < 2; ++eye) {
        Swapchain& sc = eyes_[eye];
        if (sc.handle == XR_NULL_HANDLE || sc.width != extent.width || sc.height != extent.height) {
            if (!create_swapchain(sc, extent.width, extent.height,
                                  eye == 0 ? "Left eye" : "Right eye")) {
                return false;
            }
        }
    }

    // Depth3D re-renders every XR frame: the eye poses change even when the
    // game frame does not.
    auto& renderer = StereoRenderer::instance();
    for (int eye = 0; eye < 2; ++eye) {
        Swapchain& sc = eyes_[eye];
        u32 index = 0;
        if (!acquire_image(sc, index)) return false;
        renderer.xr_draw_depth3d_eye(eye, views, cfg, sc.rtvs[index], sc.width, sc.height);
        if (!release_image(sc)) return false;

        XrCompositionLayerProjectionView& v = layer_views[eye];
        v.pose = views[eye].pose;
        v.fov = views[eye].fov;
        v.subImage.swapchain = sc.handle;
        v.subImage.imageRect.offset = { 0, 0 };
        v.subImage.imageRect.extent = { static_cast<i32>(sc.width), static_cast<i32>(sc.height) };
        v.subImage.imageArrayIndex = 0;
    }
    layer.layerFlags = 0;
    layer.space = local_space_;
    layer.viewCount = 2;
    layer.views = layer_views;
    return true;
}

} // namespace vrc
