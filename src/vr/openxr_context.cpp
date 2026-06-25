#include "openxr_context.h"
#include "render/stereo_renderer.h"
#include "input/xr_input.h"
#include "input/input_mapper.h"
#include "core/logging.h"
#include "core/config.h"
#include <openxr/openxr.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cassert>

namespace vrc {

OpenXRContext& OpenXRContext::instance() {
    static OpenXRContext ctx;
    return ctx;
}

bool OpenXRContext::initialize(GraphicsAPI api) {
    if (initialized_) return true;

    Log::info("Initializing OpenXR context (API: {})", static_cast<int>(api));
    graphics_api_ = api;

    if (!create_instance()) return false;
    if (!get_system()) return false;

    initialized_ = true;
    Log::info("OpenXR instance+system initialized (session deferred until device bound)");
    return true;
}

void OpenXRContext::shutdown() {
    if (!initialized_) return;

    stop_frame_thread();

    destroy_eye_rtvs();

    if (local_space_) { xrDestroySpace(local_space_); local_space_ = XR_NULL_HANDLE; }
    if (view_space_) { xrDestroySpace(view_space_); view_space_ = XR_NULL_HANDLE; }

    for (auto& sc : swapchains_) {
        if (sc.handle) {
            if (graphics_api_ == GraphicsAPI::D3D11) {
                for (auto& rtv : sc.d3d11_rtvs) {
                    if (rtv) {
                        static_cast<ID3D11RenderTargetView*>(rtv)->Release();
                    }
                }
                sc.d3d11_rtvs.clear();
            }
            sc.d3d12_rtvs.clear();
            xrDestroySwapchain(sc.handle);
            sc.handle = XR_NULL_HANDLE;
        }
    }

    if (d3d12_rtv_heap_) { d3d12_rtv_heap_->Release(); d3d12_rtv_heap_ = nullptr; }
    d3d12_rtv_heap_size_ = 0;

    if (session_) { xrDestroySession(session_); session_ = XR_NULL_HANDLE; }
    if (instance_) { xrDestroyInstance(instance_); instance_ = XR_NULL_HANDLE; }

    d3d11_device_ = nullptr;
    d3d11_context_ = nullptr;
    d3d12_device_ = nullptr;
    d3d12_queue_ = nullptr;
    device_bound_ = false;
    session_created_ = false;
    swapchains_created_ = false;
    session_running_ = false;

    if (frame_ready_event_) { CloseHandle(frame_ready_event_); frame_ready_event_ = nullptr; }
    if (frame_done_event_) { CloseHandle(frame_done_event_); frame_done_event_ = nullptr; }

    initialized_ = false;
    Log::info("OpenXR shutdown complete");
}

// ─── Real Device Binding ─────────────────────────────────────────────────────

void OpenXRContext::bind_d3d11_device(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (device_bound_ && session_created_) return;

    d3d11_device_ = device;
    d3d11_context_ = ctx;
    graphics_api_ = GraphicsAPI::D3D11;
    device_bound_ = true;

    // Enable D3D11 multithread protection so frame thread can safely flush
    // (fails silently if device was created with D3D11_CREATE_DEVICE_SINGLETHREADED)
    multithread_protected_ = false;
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
        multithread_protected_ = SUCCEEDED(mt->SetMultithreadProtected(TRUE));
        mt->Release();
        Log::info("D3D11 multithread protection: {}", multithread_protected_ ? "enabled" : "unavailable (singlethreaded device)");
    }

    Log::info("D3D11 device bound to OpenXR context");

    // Also propagate device to StereoRenderer for rendering
    auto& renderer = StereoRenderer::instance();
    renderer.set_d3d11_device(device, ctx);

    if (initialized_ && !session_created_) {
        if (!ensure_session_created()) {
            Log::error("Failed to create OpenXR session with D3D11 device, will retry");
            device_bound_ = false;
        }
    }
}

void OpenXRContext::bind_d3d12_device(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (device_bound_ && session_created_) return;

    if (!device || !queue) {
        Log::warn("bind_d3d12_device: device or queue is null, deferring");
        d3d12_device_ = device;
        d3d12_queue_ = queue;
        return;
    }

    d3d12_device_ = device;
    d3d12_queue_ = queue;
    graphics_api_ = GraphicsAPI::D3D12;
    device_bound_ = true;

    Log::info("D3D12 device bound to OpenXR context");

    if (initialized_ && !session_created_) {
        if (!ensure_session_created()) {
            Log::error("Failed to create OpenXR session with D3D12 device, will retry");
            device_bound_ = false;
        }
    }
}

void OpenXRContext::bind_swap_chain(void* swap_chain) {
    bound_swap_chain_ = swap_chain;
}

bool OpenXRContext::ensure_session_created() {
    if (session_created_) return true;
    if (!device_bound_) return false;

    Log::info("Creating OpenXR session with bound D3D device");

    if (graphics_api_ == GraphicsAPI::D3D11 && d3d11_device_) {
        PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReqs = nullptr;
        xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetReqs));
        if (pfnGetReqs) {
            XrGraphicsRequirementsD3D11KHR gpuReqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
            XrResult reqRes = pfnGetReqs(instance_, system_id_, &gpuReqs);
            if (XR_FAILED(reqRes)) {
                Log::warn("xrGetD3D11GraphicsRequirementsKHR failed: {}", static_cast<int>(reqRes));
            }
        }

        XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        binding.device = d3d11_device_;

        XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
        sci.systemId = system_id_;
        sci.next = &binding;

        XrResult res = xrCreateSession(instance_, &sci, &session_);
        if (XR_FAILED(res)) {
            Log::error("xrCreateSession (D3D11) failed: {}", static_cast<int>(res));
            return false;
        }
        Log::info("OpenXR session created with real D3D11 device");
    }
    else if (graphics_api_ == GraphicsAPI::D3D12 && d3d12_device_ && d3d12_queue_) {
        XrGraphicsBindingD3D12KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        binding.device = d3d12_device_;
        binding.queue = d3d12_queue_;

        XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
        sci.systemId = system_id_;
        sci.next = &binding;

        XrResult res = xrCreateSession(instance_, &sci, &session_);
        if (XR_FAILED(res)) {
            Log::error("xrCreateSession (D3D12) failed: {}", static_cast<int>(res));
            return false;
        }
        Log::info("OpenXR session created with real D3D12 device");
    }
    else {
        Log::error("Cannot create session: device not properly bound");
        return false;
    }

    session_created_ = true;

    if (!create_swapchains()) return false;
    if (!create_reference_spaces()) return false;
    if (!create_eye_rtvs()) return false;

    // Initialize XR input action bindings before xrBeginSession is called
    // (xrAttachSessionActionSets must happen before the session begins)
    XrInput::instance().initialize(instance_, session_);

    // Poll events once to catch READY state and call xrBeginSession
    poll_events();

    // Submit an empty kickstart frame to transition from SYNCHRONIZED → VISIBLE/FOCUSED.
    // This must happen BEFORE the background frame thread takes over OpenXR calls,
    // because xrWaitFrame blocks indefinitely in SYNCHRONIZED state on SteamVR.
    if (session_running_) {
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        if (d3d11_context_) d3d11_context_->Flush();
        XrResult res = xrWaitFrame(session_, &fwi, &fs);
        if (XR_SUCCEEDED(res)) {
            XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
            res = xrBeginFrame(session_, &fbi);
            if (XR_SUCCEEDED(res)) {
                XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
                fei.displayTime = fs.predictedDisplayTime;
                fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                fei.layerCount = 0;
                fei.layers = nullptr;
                xrEndFrame(session_, &fei);
                Log::info("Submitted empty kickstart frame");
            }
        }
    }

    // Create events for frame synchronization
    frame_ready_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    frame_done_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    // Start the background OpenXR frame thread
    start_frame_thread();

    return true;
}

// ─── Background Frame Thread ─────────────────────────────────────────────────

void OpenXRContext::start_frame_thread() {
    if (frame_thread_.joinable()) return;

    thread_exit_flag_.store(false);
    frame_thread_ = std::thread(&OpenXRContext::frame_thread_proc, this);
}

void OpenXRContext::stop_frame_thread() {
    if (!frame_thread_.joinable()) return;

    thread_exit_flag_.store(true);

    // Wake the frame thread if it's waiting
    if (frame_done_event_) SetEvent(frame_done_event_);
    if (frame_ready_event_) SetEvent(frame_ready_event_);

    frame_thread_.join();
}

void OpenXRContext::frame_thread_proc() {
    Log::info("Frame thread started");

    // Wait for session to be running (READY state received → xrBeginSession)
    while (!thread_exit_flag_.load()) {
        poll_events();
        if (session_running_) break;
        Sleep(1);
    }

    // Poll events once more to catch any pending state transitions
    poll_events();

    u64 frame_count = 0;

    // Main OpenXR frame loop
    while (!thread_exit_flag_.load()) {
        // Poll events before xrWaitFrame to catch state transitions
        poll_events();
        if (!session_running_) {
            Log::info("Session no longer running, stopping frame thread");
            break;
        }

        frame_count++;

        // Wait for the next frame
        XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState fs = { XR_TYPE_FRAME_STATE };
        XrResult res = xrWaitFrame(session_, &fwi, &fs);

        if (XR_FAILED(res) || thread_exit_flag_.load()) {
            if (XR_FAILED(res)) Log::error("xrWaitFrame failed: {}", static_cast<int>(res));
            break;
        }

        if (thread_exit_flag_.load()) break;

        // Begin the frame
        XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
        res = xrBeginFrame(session_, &fbi);
        if (XR_FAILED(res)) {
            Log::error("xrBeginFrame failed: {}", static_cast<int>(res));
            break;
        }

        if (thread_exit_flag_.load()) break;

        // Poll events (handles session state changes)
        poll_events();
        if (!session_running_) {
            Log::info("Session no longer running, stopping frame thread");
            break;
        }

        // Sync XR controller input (must run after xrBeginFrame, before xrEndFrame)
        if (XrInput::instance().is_active()) {
            XrInput::instance().sync(session_, local_space_,
                                     fs.predictedDisplayTime);
        }

        // Translate controller state to game input (WASD / mouse / buttons)
        {
            f64 dt_ms = static_cast<f64>(fs.predictedDisplayPeriod) / 1'000'000.0;
            InputMapper::instance().process_frame(dt_ms);
        }

        // Get head poses for this frame (must be on the frame thread)
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = view_config_;
        vli.space = local_space_;
        vli.displayTime = fs.predictedDisplayTime;

        frame_data_.view_state = { XR_TYPE_VIEW_STATE };
        frame_data_.views[0] = { XR_TYPE_VIEW };
        frame_data_.views[1] = { XR_TYPE_VIEW };

        u32 view_count = 2;
        res = xrLocateViews(session_, &vli, &frame_data_.view_state,
                            view_count, &view_count, frame_data_.views);
        if (XR_FAILED(res)) {
            Log::error("xrLocateViews failed: {}", static_cast<int>(res));
        }

        // Store frame state for Present thread
        frame_data_.frame_state = fs;

        // Acquire swapchain images for both eyes
        for (int eye = 0; eye < 2; eye++) {
            auto& sc = swapchains_[eye];

            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            u32 index = 0;
            res = xrAcquireSwapchainImage(sc.handle, &ai, &index);
            if (XR_FAILED(res)) {
                Log::error("xrAcquireSwapchainImage eye {} failed: {}", eye, static_cast<int>(res));
                frame_data_.acquired_eye_indices[eye] = 0;
                continue;
            }

            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            res = xrWaitSwapchainImage(sc.handle, &wi);
            if (XR_FAILED(res)) {
                Log::error("xrWaitSwapchainImage eye {} failed: {}", eye, static_cast<int>(res));
            }

            frame_data_.acquired_eye_indices[eye] = index;
        }

        // Signal Present thread that images are ready
        SetEvent(frame_ready_event_);

        // Wait for Present thread to finish rendering
        WaitForSingleObject(frame_done_event_, INFINITE);

        if (thread_exit_flag_.load()) break;

        // Release swapchain images
        for (int eye = 0; eye < 2; eye++) {
            auto& sc = swapchains_[eye];
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(sc.handle, &ri);
        }

        // End the frame with projection layers
        XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = local_space_;

        XrCompositionLayerProjectionView proj_views[2] = {};
        for (int eye = 0; eye < 2; eye++) {
            auto& sc = swapchains_[eye];
            proj_views[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            proj_views[eye].pose = frame_data_.views[eye].pose;
            proj_views[eye].fov = frame_data_.views[eye].fov;
            proj_views[eye].subImage.swapchain = sc.handle;
            proj_views[eye].subImage.imageRect.offset = {0, 0};
            proj_views[eye].subImage.imageRect.extent = { sc.width, sc.height };
        }

        layer.viewCount = 2;
        layer.views = proj_views;

        fei.layerCount = 1;
        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer) };
        fei.layers = layers;

        // Flush D3D11 commands so the compositor can see rendered content
        // Only flush from frame thread when multithread protection is active
        if (d3d11_context_ && multithread_protected_) d3d11_context_->Flush();

        res = xrEndFrame(session_, &fei);
        if (XR_FAILED(res)) {
            Log::warn("xrEndFrame failed: {} (first frame often fails, non-fatal)", static_cast<int>(res));
        }
    }

    Log::info("Frame thread stopped");
}

// ─── RTV Management ─────────────────────────────────────────────────────────

bool OpenXRContext::create_eye_rtvs() {
    if (!session_created_ || !swapchains_created_) return false;

    if (graphics_api_ == GraphicsAPI::D3D11) {
        for (u32 eye = 0; eye < 2; eye++) {
            auto& sc = swapchains_[eye];
            sc.d3d11_rtvs.clear();

            for (size_t i = 0; i < sc.d3d11_images.size(); i++) {
                ID3D11Texture2D* image = sc.d3d11_images[i].texture;
                ID3D11RenderTargetView* rtv = nullptr;

                D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
                rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

                // Try UNORM view first — game backbuffers are already gamma-encoded,
                // so writing via SRGB RTV would double-apply gamma correction.
                // Pimax typically creates textures as TYPELESS internally, which
                // allows both UNORM and SRGB views.
                static const DXGI_FORMAT kFmtOrder[] = {
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                };
                for (auto fmt : kFmtOrder) {
                    rtv_desc.Format = fmt;
                    if (d3d11_device_ && SUCCEEDED(d3d11_device_->CreateRenderTargetView(
                            image, &rtv_desc, &rtv))) {
                        if (i == 0) Log::info("Eye {} swapchain RTV format: {}", eye, static_cast<u32>(fmt));
                        break;
                    }
                }
                if (rtv) {
                    sc.d3d11_rtvs.push_back(rtv);
                } else {
                    Log::error("Eye {} image {}: all RTV formats failed", eye, i);
                }
            }

            if (!sc.d3d11_rtvs.empty()) {
                eye_rtvs_[eye] = sc.d3d11_rtvs[0];
                Log::info("Eye {} RTV created ({} images)", eye, sc.d3d11_rtvs.size());
            }
        }
    } else if (graphics_api_ == GraphicsAPI::D3D12 && d3d12_device_) {
        // Count total RTVs needed
        u32 total_rtvs = 0;
        for (auto& sc : swapchains_) {
            total_rtvs += (u32)sc.d3d12_images.size();
        }

        if (total_rtvs == 0) return false;

        // Create RTV descriptor heap
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = total_rtvs;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (d3d12_rtv_heap_) { d3d12_rtv_heap_->Release(); d3d12_rtv_heap_ = nullptr; }

        if (FAILED(d3d12_device_->CreateDescriptorHeap(&heap_desc,
                IID_PPV_ARGS(&d3d12_rtv_heap_)))) {
            Log::error("Failed to create D3D12 RTV descriptor heap");
            return false;
        }
        d3d12_rtv_heap_size_ = total_rtvs;

        u32 heap_inc = d3d12_device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = d3d12_rtv_heap_->GetCPUDescriptorHandleForHeapStart();

        for (u32 eye = 0; eye < 2; eye++) {
            auto& sc = swapchains_[eye];
            sc.d3d12_rtvs.clear();

            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
            rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

            for (size_t i = 0; i < sc.d3d12_images.size(); i++) {
                ID3D12Resource* image = sc.d3d12_images[i].texture;
                d3d12_device_->CreateRenderTargetView(image, &rtv_desc, rtv_handle);
                sc.d3d12_rtvs.push_back(rtv_handle);
                rtv_handle.ptr += heap_inc;
            }

            if (!sc.d3d12_rtvs.empty()) {
                eye_rtvs_[eye] = reinterpret_cast<void*>(sc.d3d12_rtvs[0].ptr);
                Log::info("Eye {} D3D12 RTV created ({} images)", eye, sc.d3d12_rtvs.size());
            }
        }
    }

    return true;
}

void OpenXRContext::destroy_eye_rtvs() {
    for (auto& sc : swapchains_) {
        if (graphics_api_ == GraphicsAPI::D3D11) {
            for (auto& rtv : sc.d3d11_rtvs) {
                if (rtv) {
                    static_cast<ID3D11RenderTargetView*>(rtv)->Release();
                }
            }
            sc.d3d11_rtvs.clear();
        }
        sc.d3d12_rtvs.clear();
    }
    eye_rtvs_ = {};
    if (d3d12_rtv_heap_) { d3d12_rtv_heap_->Release(); d3d12_rtv_heap_ = nullptr; }
    d3d12_rtv_heap_size_ = 0;
}

// ─── OpenXR Instance / System ───────────────────────────────────────────────

// ─── Pimax Runtime Detection ──────────────────────────────────────────────────

static std::string detect_pimax_runtime_json() {
    const char* pimax_json = "C:\\Program Files\\Pimax\\Runtime\\PiOpenXR_64.json";
    if (GetFileAttributesA(pimax_json) == INVALID_FILE_ATTRIBUTES)
        return {};

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return {};

    bool found = false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"PiService.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"pi_server.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found ? std::string(pimax_json) : std::string();
}

bool OpenXRContext::create_instance() {
    // Auto-detect Pimax runtime if user hasn't explicitly set XR_RUNTIME_JSON
    char* existing_runtime = nullptr;
    size_t existing_sz = 0;
    bool env_set = (_dupenv_s(&existing_runtime, &existing_sz, "XR_RUNTIME_JSON") == 0 && existing_runtime);
    if (!env_set) {
        auto pimax_json = detect_pimax_runtime_json();
        if (!pimax_json.empty()) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", pimax_json.c_str());
        }
    }
    free(existing_runtime);

    XrApplicationInfo app_info = {};
    strcpy_s(app_info.applicationName, "VR Game Converter");
    app_info.applicationVersion = 1;
    strcpy_s(app_info.engineName, "VR Converter Engine");
    app_info.engineVersion = 1;
    app_info.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    std::vector<const char*> extensions;
    extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if (graphics_api_ == GraphicsAPI::D3D12) {
        extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
    }
    extensions.push_back("XR_KHR_composition_layer_depth");

    u32 ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> available(ext_count,
        { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count,
                                           available.data());

    for (auto& ext : available) {
        Log::debug("Available OpenXR extension: {}", ext.extensionName);
    }

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo = app_info;
    ci.enabledExtensionCount = (u32)extensions.size();
    ci.enabledExtensionNames = extensions.data();

    XrResult res = xrCreateInstance(&ci, &instance_);
    if (XR_FAILED(res)) {
        Log::error("xrCreateInstance failed: {}", static_cast<int>(res));
        return false;
    }

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(instance_, &ip);
    Log::info("OpenXR runtime: {} v{}.{}.{}",
              ip.runtimeName,
              XR_VERSION_MAJOR(ip.runtimeVersion),
              XR_VERSION_MINOR(ip.runtimeVersion),
              XR_VERSION_PATCH(ip.runtimeVersion));

    return true;
}

bool OpenXRContext::get_system() {
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = form_factor_;

    XrResult res = xrGetSystem(instance_, &sgi, &system_id_);
    if (XR_FAILED(res)) {
        Log::error("xrGetSystem failed: no HMD detected? ({})",
                   static_cast<int>(res));
        return false;
    }

    u32 view_count = 0;
    xrEnumerateViewConfigurationViews(instance_, system_id_,
                                      view_config_, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(
        view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(instance_, system_id_,
                                      view_config_, view_count,
                                      &view_count, views.data());

    if (view_count >= 2) {
        viewport_width_ = views[0].recommendedImageRectWidth;
        viewport_height_ = views[0].recommendedImageRectHeight;
    }

    Log::info("OpenXR system acquired. Viewport: {}x{}",
              viewport_width_, viewport_height_);
    return true;
}

bool OpenXRContext::create_swapchains() {
    if (!session_created_) return false;

    u32 view_count = 0;
    xrEnumerateViewConfigurationViews(instance_, system_id_,
                                      view_config_, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(
        view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(instance_, system_id_,
                                      view_config_, view_count,
                                      &view_count, views.data());

    for (u32 i = 0; i < view_count && i < 2; i++) {
        auto& view = views[i];
        auto& sc = swapchains_[i];

        XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                         XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        sci.sampleCount = 1;
        sci.width = view.recommendedImageRectWidth;
        sci.height = view.recommendedImageRectHeight;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;

        XrResult res = xrCreateSwapchain(session_, &sci, &sc.handle);
        if (XR_FAILED(res)) {
            Log::error("xrCreateSwapchain for eye {} failed: {}",
                       i, static_cast<int>(res));
            return false;
        }

        sc.width = sci.width;
        sc.height = sci.height;

        u32 image_count = 0;
        xrEnumerateSwapchainImages(sc.handle, 0, &image_count, nullptr);

        if (graphics_api_ == GraphicsAPI::D3D11) {
            sc.d3d11_images.resize(image_count,
                { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            xrEnumerateSwapchainImages(sc.handle, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.d3d11_images.data()));
        } else if (graphics_api_ == GraphicsAPI::D3D12) {
            sc.d3d12_images.resize(image_count,
                { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
            xrEnumerateSwapchainImages(sc.handle, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.d3d12_images.data()));
        }

        Log::info("Eye {} swapchain created: {}x{} ({} images)",
                  i, sc.width, sc.height, image_count);
    }

    swapchains_created_ = true;
    return true;
}

bool OpenXRContext::create_reference_spaces() {
    if (!session_created_) return false;

    XrReferenceSpaceCreateInfo sci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    sci.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };

    XrResult res = xrCreateReferenceSpace(session_, &sci, &local_space_);
    if (XR_FAILED(res)) {
        Log::error("xrCreateReferenceSpace (local) failed: {}",
                   static_cast<int>(res));
        return false;
    }

    sci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrSpace stage_space = XR_NULL_HANDLE;
    res = xrCreateReferenceSpace(session_, &sci, &stage_space);
    if (XR_SUCCEEDED(res)) {
        xrDestroySpace(stage_space);
        Log::info("Room-scale (stage) space available");
    }

    return true;
}

void OpenXRContext::poll_events() {
    XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto& state = reinterpret_cast<XrEventDataSessionStateChanged&>(event);
                Log::info("Session state changed to: {}",
                          static_cast<int>(state.state));

                session_state_ = state.state;

                if (state.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = view_config_;
                    XrResult r = xrBeginSession(session_, &bi);
                    if (XR_SUCCEEDED(r)) {
                        session_running_ = true;
                        Log::info("OpenXR session begun");
                    }
                } else if (state.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session_);
                    session_running_ = false;
                } else if (state.state == XR_SESSION_STATE_EXITING ||
                           state.state == XR_SESSION_STATE_LOSS_PENDING) {
                    session_running_ = false;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                session_running_ = false;
                break;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ─── Frame Pipeline (Present Thread API) ────────────────────────────────────

bool OpenXRContext::begin_frame() {
    if (!session_created_) return false;
    if (!session_running_) return false;
    if (!frame_ready_event_) return false;

    // Wait for frame thread to acquire swapchain images (with 1s timeout)
    DWORD wait = WaitForSingleObject(frame_ready_event_, 1000);
    if (wait != WAIT_OBJECT_0) {
        Log::error("begin_frame: timed out (wait={})", wait);
        return false;
    }

    return true;
}

bool OpenXRContext::end_frame() {
    if (!session_created_) return false;
    if (!frame_done_event_) return false;

    // If multithread protection is unavailable (device created with SINGLETHREADED),
    // flush from Present thread instead of frame thread to avoid race conditions
    if (d3d11_context_ && !multithread_protected_) {
        d3d11_context_->Flush();
    }

    // Signal frame thread that rendering is done (triggers xrEndFrame)
    SetEvent(frame_done_event_);
    return true;
}

bool OpenXRContext::submit_frame() {
    return end_frame();
}

f64 OpenXRContext::predicted_display_time() const {
    // XrTime is in nanoseconds; callers expect milliseconds
    return static_cast<f64>(frame_data_.frame_state.predictedDisplayTime) / 1'000'000.0;
}

bool OpenXRContext::acquire_eyes(EyeViews& views) {
    if (!session_created_) return false;

    // Read cached view data from last frame thread xrLocateViews call
    for (i32 i = 0; i < 2; i++) {
        auto& xr_view = frame_data_.views[i];
        auto& v = (i == 0) ? views.left : views.right;

        v.position = Vec3(
            xr_view.pose.position.x,
            xr_view.pose.position.y,
            xr_view.pose.position.z
        );
        v.rotation = Quat(
            xr_view.pose.orientation.x,
            xr_view.pose.orientation.y,
            xr_view.pose.orientation.z,
            xr_view.pose.orientation.w
        );
        v.fov_horizontal = xr_view.fov.angleRight - xr_view.fov.angleLeft;
        v.fov_vertical = xr_view.fov.angleDown - xr_view.fov.angleUp;
    }

    return true;
}

bool OpenXRContext::begin_eye_image(i32 eye, u32* out_image_index) {
    if (eye < 0 || eye > 1) return false;
    if (!session_created_) return false;

    auto& sc = swapchains_[eye];

    if (sc.image_acquired) {
        if (out_image_index) *out_image_index = sc.acquired_index;
        return true;
    }

    // Use the index already acquired by the frame thread
    u32 index = frame_data_.acquired_eye_indices[eye];

    if (graphics_api_ == GraphicsAPI::D3D11 && index >= sc.d3d11_rtvs.size()) {
        Log::error("begin_eye_image: acquired index {} out of range for eye {} (rtvs={})",
                   index, eye, sc.d3d11_rtvs.size());
        return false;
    }
    if (graphics_api_ == GraphicsAPI::D3D12 && index >= sc.d3d12_rtvs.size()) {
        Log::error("begin_eye_image: acquired index {} out of range for eye {} (rtvs={})",
                   index, eye, sc.d3d12_rtvs.size());
        return false;
    }

    sc.acquired_index = index;
    sc.image_acquired = true;

    if (out_image_index) *out_image_index = index;

    if (graphics_api_ == GraphicsAPI::D3D11) {
        eye_rtvs_[eye] = sc.d3d11_rtvs[index];
        if (d3d11_context_) {
            ID3D11RenderTargetView* rtv =
                static_cast<ID3D11RenderTargetView*>(eye_rtvs_[eye]);
            d3d11_context_->OMSetRenderTargets(1, &rtv, nullptr);
        }
    } else if (graphics_api_ == GraphicsAPI::D3D12 && index < sc.d3d12_rtvs.size()) {
        eye_rtvs_[eye] = reinterpret_cast<void*>(sc.d3d12_rtvs[index].ptr);
    }

    return true;
}

bool OpenXRContext::end_eye_image(i32 eye) {
    if (eye < 0 || eye > 1) return false;
    if (!session_created_) return false;

    auto& sc = swapchains_[eye];
    sc.image_acquired = false;
    return true;
}

void* OpenXRContext::eye_swapchain_image(i32 eye, u32 index) const {
    if (eye < 0 || eye > 1) return nullptr;
    auto& sc = swapchains_[eye];
    if (graphics_api_ == GraphicsAPI::D3D11) {
        if (index >= sc.d3d11_images.size()) return nullptr;
        return sc.d3d11_images[index].texture;
    } else if (graphics_api_ == GraphicsAPI::D3D12) {
        if (index >= sc.d3d12_images.size()) return nullptr;
        return sc.d3d12_images[index].texture;
    }
    return nullptr;
}

} // namespace vrc
