#include "stereo_renderer.h"
#include "core/logging.h"
#include "core/config.h"
#include "overlay/imgui_overlay.h"
#include <cassert>
#include <windows.h>

namespace vrc {

StereoRenderer& StereoRenderer::instance() {
    static StereoRenderer renderer;
    return renderer;
}

bool StereoRenderer::initialize(GraphicsAPI api) {
    if (initialized_) return true;
    api_ = api;

    Log::info("Initializing StereoRenderer v2 (Phase 2)");

    // Initialize Phase 2 subsystems
    perf_.initialize();
    latency_.initialize();

    switch (api) {
        case GraphicsAPI::D3D11:
            if (!init_d3d11()) return false;
            break;
        case GraphicsAPI::D3D12:
            if (!init_d3d12()) return false;
            break;
        default:
            Log::error("Unsupported graphics API for stereo rendering");
            return false;
    }

    // Initialize OpenXR (instance + system, session deferred)
    if (!openxr_.initialize(api)) {
        Log::error("OpenXR initialization failed");
        return false;
    }

    // Initialize depth reprojection
    if (api == GraphicsAPI::D3D11 && d3d11_device_) {
        if (!depth_.initialize(d3d11_device_)) {
            Log::error("DepthReprojection::initialize failed — blit will be skipped");
        }
    }

    camera_rig_.initialize(Config::instance().current_profile().ipd);

    initialized_ = true;
    Log::info("StereoRenderer Phase 2 initialized successfully");
    return true;
}

void StereoRenderer::shutdown() {
    if (!initialized_) return;

    depth_.shutdown();
    latency_.shutdown();
    perf_.shutdown();

    if (api_ == GraphicsAPI::D3D12) {
        if (d3d12_fence_event_) { CloseHandle(d3d12_fence_event_); d3d12_fence_event_ = nullptr; }
        if (d3d12_fence_) { d3d12_fence_->Release(); d3d12_fence_ = nullptr; }
        if (d3d12_cmd_list_) { d3d12_cmd_list_->Release(); d3d12_cmd_list_ = nullptr; }
        if (d3d12_allocator_) { d3d12_allocator_->Release(); d3d12_allocator_ = nullptr; }
        if (d3d11_on12_) { d3d11_on12_->Release(); d3d11_on12_ = nullptr; }
    }

    if (api_ == GraphicsAPI::D3D11) {
        for (int i = 0; i < 2; i++) {
            if (stereo_rtv_[i]) { stereo_rtv_[i]->Release(); stereo_rtv_[i] = nullptr; }
            if (stereo_rt_[i]) { stereo_rt_[i]->Release(); stereo_rt_[i] = nullptr; }
        }
        if (game_frame_srv_) { game_frame_srv_->Release(); game_frame_srv_ = nullptr; }
        if (game_frame_copy_) { game_frame_copy_->Release(); game_frame_copy_ = nullptr; }
    }

    openxr_.shutdown();
    initialized_ = false;
    Log::info("StereoRenderer Phase 2 shut down");
}

bool StereoRenderer::init_d3d11() {
    Log::info("Init D3D11 stereo rendering (Phase 2)");
    // Devices obtained from hook context via OpenXRContext::bind_d3d11_device()
    return true;
}

bool StereoRenderer::init_d3d12() {
    Log::info("Init D3D12 stereo rendering (Phase 2)");

    // Devices are bound later via OpenXRContext::bind_d3d12_device()
    // Create command allocator and list when device becomes available
    d3d12_device_ = OpenXRContext::instance().d3d12_device();
    d3d12_queue_ = OpenXRContext::instance().d3d12_queue();

    if (!d3d12_device_ || !d3d12_queue_) {
        Log::info("D3D12 device/queue not yet bound, will init on first render");
        return true;
    }

    return init_d3d12_resources();
}

bool StereoRenderer::init_d3d12_resources() {
    if (d3d12_allocator_) return true;

    if (FAILED(d3d12_device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d3d12_allocator_)))) {
        Log::error("Failed to create D3D12 command allocator");
        return false;
    }

    if (FAILED(d3d12_device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            d3d12_allocator_, nullptr, IID_PPV_ARGS(&d3d12_cmd_list_)))) {
        Log::error("Failed to create D3D12 command list");
        return false;
    }
    d3d12_cmd_list_->Close();

    if (FAILED(d3d12_device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&d3d12_fence_)))) {
        Log::error("Failed to create D3D12 fence");
        return false;
    }
    d3d12_fence_value_ = 0;

    d3d12_fence_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!d3d12_fence_event_) {
        Log::error("Failed to create D3D12 fence event");
        return false;
    }

    Log::info("D3D12 rendering resources initialized");
    return true;
}

bool StereoRenderer::create_stereo_targets(u32 width, u32 height) {
    if (!d3d11_device_) return false;

    for (int i = 0; i < 2; i++) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;

        if (stereo_rt_[i]) { stereo_rt_[i]->Release(); stereo_rt_[i] = nullptr; }
        if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &stereo_rt_[i]))) {
            Log::error("Failed to create stereo render target {}", i);
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = desc.Format;
        rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        if (stereo_rtv_[i]) { stereo_rtv_[i]->Release(); stereo_rtv_[i] = nullptr; }
        if (FAILED(d3d11_device_->CreateRenderTargetView(
                stereo_rt_[i], &rtv_desc, &stereo_rtv_[i]))) {
            Log::error("Failed to create RTV for eye {}", i);
            return false;
        }
    }

    // Create game frame copy texture for reprojection
    D3D11_TEXTURE2D_DESC copy_desc = {};
    copy_desc.Width = width;
    copy_desc.Height = height;
    copy_desc.MipLevels = 1;
    copy_desc.ArraySize = 1;
    copy_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    copy_desc.SampleDesc.Count = 1;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;

    if (game_frame_copy_) { game_frame_copy_->Release(); game_frame_copy_ = nullptr; }
    if (FAILED(d3d11_device_->CreateTexture2D(&copy_desc, nullptr, &game_frame_copy_))) {
        Log::error("Failed to create game frame copy texture");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = copy_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    if (game_frame_srv_) { game_frame_srv_->Release(); game_frame_srv_ = nullptr; }
    if (FAILED(d3d11_device_->CreateShaderResourceView(
            game_frame_copy_, &srv_desc, &game_frame_srv_))) {
        Log::error("Failed to create game frame SRV");
        return false;
    }

    return true;
}

// ─── Frame Pipeline ─────────────────────────────────────────────────────────

bool StereoRenderer::on_frame_present(const FrameCapture& capture) {
    return render_frame(capture);
}

bool StereoRenderer::render_frame(const FrameCapture& capture) {
    if (!initialized_) return false;

    frame_count_++;
    if (frame_count_.load() == 1) {
        Log::info("First render_frame call");
    }
    u64 frame_idx = frame_count_.load();

    // Poll F2 here so the overlay can be toggled even when the game window
    // doesn't have keyboard focus (common while wearing the headset).
    {
        bool f2_now = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (f2_now && !f2_was_down_) {
            ImGuiOverlay::instance().toggle_visible();
        }
        f2_was_down_ = f2_now;
    }

    // Phase 2: Track pipeline latency
    perf_.begin_frame(frame_idx);
    latency_.begin_frame(frame_idx);

    // Start OpenXR frame (waits for predicted display time)
    if (!openxr_.begin_frame()) {
        return false;
    }

    // Acquire eye views from OpenXR (head tracking data)
    EyeViews eye_views;
    if (!openxr_.acquire_eyes(eye_views)) {
        Log::warn("Failed to acquire eye views");
        return false;
    }

    // Phase 2: Feed tracking data to tracking system
    tracking_.set_head_pose_from_openxr(
        eye_views.left.position,
        eye_views.left.rotation,
        openxr_.predicted_display_time()
    );

    // Phase 2: Latency compensation - predict head pose at display time
    tracking_.apply_latency_compensation(openxr_.predicted_display_time());

    if (pre_render_) pre_render_();

    // Render based on API
    bool result = false;
    switch (api_) {
        case GraphicsAPI::D3D11:
            result = render_d3d11(capture);
            break;
        case GraphicsAPI::D3D12:
            result = render_d3d12(capture);
            break;
        default:
            result = false;
    }

    perf_.end_cpu_work();

    if (post_render_) post_render_();

    // Phase 2: Apply asynchronous timewarp before submit
    if (latency_.needs_timewarp()) {
        apply_timewarp();
    }

    perf_.end_gpu_work();

    // Submit frame to OpenXR
    if (result) {
        result = openxr_.submit_frame();
    }

    // Phase 2: End frame tracking
    latency_.end_frame();
    if (frame_idx % 60 == 0) {
        Log::info("Frame {} rendered successfully", frame_idx);
    }

    perf_.end_frame();

    // Update config with latest latency stats
    Config::instance().latency_stats() = perf_.get_latency_stats();

    return result;
}

// ─── D3D11 Render Paths ────────────────────────────────────────────────────

bool StereoRenderer::render_d3d11(const FrameCapture& capture) {
    if (depth_.depth_captured()) {
        return render_d3d11_with_depth(capture);
    }
    return render_d3d11_blit(capture);
}

bool StereoRenderer::render_d3d11_with_depth(const FrameCapture& capture) {
    if (!d3d11_device_ || !d3d11_ctx_) return false;

    // Get camera rig eye views with head tracking applied
    HeadPose head_pose = tracking_.get_head_pose();

    // Build view setups for each eye using the camera rig
    ViewSetup mono_setup;
    // Use a real perspective instead of identity — identity causes z_near=0/0=NaN
    // in mono_to_stereo_projections, producing white frames on every slider drag.
    mono_setup.projection = Matrix4::perspective(
        kPi / 2.0f, 16.0f / 9.0f, 0.01f, 1000.0f);

    // Apply head tracking to the mono camera
    ViewSetup tracked = camera_rig_.apply_head_tracking(mono_setup, head_pose);

    // Compute stereo eye views from the tracked mono setup
    EyeViews eye_views = camera_rig_.compute_eye_views(tracked);

    // Remap game frame as SRV for the reprojection shaders
    ID3D11ShaderResourceView* game_srv = nullptr;
    if (capture.resource) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(capture.resource);
        if (SUCCEEDED(d3d11_device_->CreateShaderResourceView(tex, &srv_desc, &game_srv))) {
            depth_.set_game_color_srv(game_srv);
            game_srv->Release();
        }
    }

    // Use depth-aware reprojection (both eyes in one pass)
    ReprojectionInput input{};
    input.game_color_srv = game_srv;
    input.linear_depth_srv = depth_.linear_depth_srv();
    input.game_view_proj = eye_views.left.view_projection; // game's mono VP
    input.eye_view_proj_left = eye_views.left.view_projection;
    input.eye_view_proj_right = eye_views.right.view_projection;
    input.eye_left = eye_views.left;
    input.eye_right = eye_views.right;
    input.eye_width = openxr_.eye_swapchain_width(0);
    input.eye_height = openxr_.eye_swapchain_height(0);

    // Acquire eye RTVs from OpenXR
    for (int eye = 0; eye < 2; eye++) {
        u32 image_index = 0;
        if (!openxr_.begin_eye_image(eye, &image_index)) continue;
        ID3D11RenderTargetView* rtv =
            static_cast<ID3D11RenderTargetView*>(openxr_.eye_rtv(eye));
        if (eye == 0) input.eye_rtv_left = rtv;
        else input.eye_rtv_right = rtv;
    }

    if (input.eye_rtv_left && input.eye_rtv_right) {
        depth_.reproject_to_eyes(input);
    } else {
        // Fallback: per-eye blit
        ViewSetup identity_setup;
        for (int eye = 0; eye < 2; eye++) {
            ID3D11RenderTargetView* rtv = (eye == 0)
                ? input.eye_rtv_left : input.eye_rtv_right;
            if (rtv) {
                depth_.blit_to_eye(capture, rtv, identity_setup,
                    openxr_.eye_swapchain_width(eye),
                    openxr_.eye_swapchain_height(eye));
            }
        }
    }

    // End eye images
    for (int eye = 0; eye < 2; eye++) {
        openxr_.end_eye_image(eye);
    }

    if (game_srv) game_srv->Release();

    latency_.on_render_submit();
    return true;
}

bool StereoRenderer::render_d3d11_blit(const FrameCapture& capture) {
    if (!d3d11_device_ || !d3d11_ctx_) {
        Log::error("render_d3d11_blit: device or ctx null");
        return false;
    }

    // The game backbuffer has only D3D11_BIND_RENDER_TARGET — CreateShaderResourceView
    // on it directly will always fail. CopyResource it into a SHADER_RESOURCE texture
    // we own, then create the SRV from that copy.
    if (capture.resource) {
        ID3D11Texture2D* backbuffer = static_cast<ID3D11Texture2D*>(capture.resource);
        D3D11_TEXTURE2D_DESC bb;
        backbuffer->GetDesc(&bb);

        // Resolve typeless formats to a concrete typed view
        DXGI_FORMAT srv_fmt = bb.Format;
        if (srv_fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS)    srv_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (srv_fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS)    srv_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (srv_fmt == DXGI_FORMAT_R10G10B10A2_TYPELESS)  srv_fmt = DXGI_FORMAT_R10G10B10A2_UNORM;

        // Recreate copy texture when format or dimensions change
        bool need_new = !game_frame_copy_;
        if (game_frame_copy_) {
            D3D11_TEXTURE2D_DESC cur;
            game_frame_copy_->GetDesc(&cur);
            need_new = cur.Format != srv_fmt || cur.Width != bb.Width || cur.Height != bb.Height;
        }

        if (need_new) {
            if (game_frame_srv_)  { game_frame_srv_->Release();  game_frame_srv_  = nullptr; }
            if (game_frame_copy_) { game_frame_copy_->Release(); game_frame_copy_ = nullptr; }

            D3D11_TEXTURE2D_DESC copy_desc = {};
            copy_desc.Width           = bb.Width;
            copy_desc.Height          = bb.Height;
            copy_desc.MipLevels       = 1;
            copy_desc.ArraySize       = 1;
            copy_desc.Format          = srv_fmt;
            copy_desc.SampleDesc      = { 1, 0 };
            copy_desc.BindFlags       = D3D11_BIND_SHADER_RESOURCE;
            copy_desc.Usage           = D3D11_USAGE_DEFAULT;

            if (FAILED(d3d11_device_->CreateTexture2D(&copy_desc, nullptr, &game_frame_copy_))) {
                Log::error("Failed to create backbuffer copy texture (fmt={})", static_cast<u32>(srv_fmt));
            } else {
                D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                srv_desc.Format                    = srv_fmt;
                srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                srv_desc.Texture2D.MipLevels       = 1;
                if (FAILED(d3d11_device_->CreateShaderResourceView(
                        game_frame_copy_, &srv_desc, &game_frame_srv_))) {
                    Log::error("Failed to create SRV for copy texture (fmt={})", static_cast<u32>(srv_fmt));
                    game_frame_copy_->Release();
                    game_frame_copy_ = nullptr;
                } else {
                    depth_.set_game_color_srv(game_frame_srv_);
                    Log::info("Backbuffer copy SRV ready: fmt={} {}x{}",
                              static_cast<u32>(srv_fmt), bb.Width, bb.Height);
                }
            }
        }

        // Copy backbuffer contents into our SRV-capable texture
        if (game_frame_copy_) {
            if (bb.SampleDesc.Count > 1)
                d3d11_ctx_->ResolveSubresource(game_frame_copy_, 0, backbuffer, 0, srv_fmt);
            else
                d3d11_ctx_->CopyResource(game_frame_copy_, backbuffer);
        }
    }

    // Save the game's active render target before begin_eye_image overrides it.
    // After all eye blits we restore it so the game's next frame renders to its
    // own backbuffer and not to the VR eye swapchain texture.
    ID3D11RenderTargetView* game_rtv = nullptr;
    ID3D11DepthStencilView* game_dsv = nullptr;
    d3d11_ctx_->OMGetRenderTargets(1, &game_rtv, &game_dsv);

    // Simple blit path when depth isn't available
    for (int eye = 0; eye < 2; eye++) {
        u32 image_index = 0;
        if (!openxr_.begin_eye_image(eye, &image_index))
            continue;

        u32 eye_w = openxr_.eye_swapchain_width(eye);
        u32 eye_h = openxr_.eye_swapchain_height(eye);

        ID3D11RenderTargetView* rtv =
            static_cast<ID3D11RenderTargetView*>(openxr_.eye_rtv(eye));

        ViewSetup identity_setup;
        if (rtv) {
            depth_.blit_to_eye(capture, rtv, identity_setup, eye_w, eye_h);
        }

        openxr_.end_eye_image(eye);
    }

    // Restore the game's render target so its next frame goes to its own backbuffer.
    d3d11_ctx_->OMSetRenderTargets(1, &game_rtv, game_dsv);
    if (game_rtv) game_rtv->Release();
    if (game_dsv) game_dsv->Release();

    latency_.on_render_submit();
    return true;
}

bool StereoRenderer::render_d3d12(const FrameCapture& capture) {
    if (!d3d12_device_) return false;
    if (!d3d11_on12_ || !d3d11_ctx_) {
        Log::error("render_d3d12: D3D11On12 device not available");
        return false;
    }

    ID3D12Resource* game_backbuffer = static_cast<ID3D12Resource*>(capture.resource);
    if (!game_backbuffer) {
        for (int eye = 0; eye < 2; eye++) {
            openxr_.begin_eye_image(eye);
            openxr_.end_eye_image(eye);
        }
        return true;
    }

    // Wrap game D3D12 backbuffer as ID3D11Resource via D3D11On12 interop.
    // This lets us use D3D11 CopyResource which D3D11On12 translates to
    // D3D12 CopyResource on the game's own command queue.
    ID3D11Resource* wrapped_bb = nullptr;
    D3D11_RESOURCE_FLAGS bb_flags = {};
    bb_flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = d3d11_on12_->CreateWrappedResource(
        game_backbuffer, &bb_flags,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
        IID_PPV_ARGS(&wrapped_bb));
    if (FAILED(hr) || !wrapped_bb) {
        Log::error("CreateWrappedResource failed: {:08x}", hr);
        return false;
    }

    // Acquire wrapped resource for D3D11 usage (syncs with game queue via fence)
    d3d11_on12_->AcquireWrappedResources(&wrapped_bb, 1);

    // Copy wrapped backbuffer to each eye swapchain texture
    for (int eye = 0; eye < 2; eye++) {
        u32 img_idx = 0;
        if (!openxr_.begin_eye_image(eye, &img_idx)) continue;

        ID3D11Texture2D* eye_tex = static_cast<ID3D11Texture2D*>(
            openxr_.eye_swapchain_image(eye, img_idx));
        if (eye_tex) {
            d3d11_ctx_->CopyResource(eye_tex, wrapped_bb);
        }

        openxr_.end_eye_image(eye);
    }

    // Flush D3D11 commands — D3D11On12 translates them to D3D12 on the game queue
    d3d11_ctx_->Flush();

    // Release wrapped resource back to D3D12
    d3d11_on12_->ReleaseWrappedResources(&wrapped_bb, 1);
    wrapped_bb->Release();

    latency_.on_render_submit();
    return true;
}

bool StereoRenderer::copy_frame_to_eyes(const FrameCapture& capture) {
    return true;
}

// ─── Phase 2: ATW ──────────────────────────────────────────────────────────

void StereoRenderer::set_latency_compensation_enabled(bool v) {
    latency_.set_motion_smoothing(v);
    camera_rig_.set_latency_compensation_enabled(v);
}

bool StereoRenderer::latency_compensation_enabled() const {
    return camera_rig_.latency_compensation_enabled();
}

bool StereoRenderer::apply_timewarp() {
    if (!openxr_.is_initialized()) return false;

    // Get the latest head pose (very recent, after rendering finished)
    HeadPose latest_pose = tracking_.get_head_pose();

    // Store ATW data for the next frame submit
    LatencyCompensator::TimewarpData tw_data;
    tw_data.hmd_position = latest_pose.position;
    tw_data.hmd_rotation = latest_pose.rotation;
    tw_data.timestamp_ms = static_cast<f64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    tw_data.valid = true;

    latency_.set_timewarp_data(tw_data);
    latency_.update_latest_pose_for_atw(latest_pose);

    return true;
}

} // namespace vrc
