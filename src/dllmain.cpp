#include "core/logging.h"
#include "core/config.h"
#include "core/types.h"
#include "core/perf_monitor.h"
#include "hook/hook_manager.h"
#include "render/stereo_renderer.h"
#include "vr/openxr_context.h"
#include "vr/tracking.h"
#include "overlay/imgui_overlay.h"
#include "input/input_proxy.h"
#include "input/input_mapper.h"
#include "input/xr_shared_state.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <chrono>
#include <mutex>

namespace vrc {

namespace {

// Binding the game device to OpenXR is retried a few times, spaced out, in
// case the first attempt hits a transient failure; then it gives up.
constexpr u32 kMaxBindAttempts = 3;
constexpr u32 kFramesBetweenBindAttempts = 120;
constexpr auto kInputPollInterval = std::chrono::milliseconds(16);

const char* api_name(GraphicsAPI api) {
    switch (api) {
        case GraphicsAPI::D3D11: return "D3D11";
        case GraphicsAPI::D3D12: return "D3D12";
        case GraphicsAPI::OpenGL: return "OpenGL";
        case GraphicsAPI::Vulkan: return "Vulkan";
        default: return "unknown";
    }
}

} // namespace

class VRConverter {
public:
    static VRConverter& instance() {
        static VRConverter conv;
        return conv;
    }

    // start() and stop() are serialized by mutex_ and are idempotent.
    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Running) return true;

        Log::init(Config::instance().log_path(), LogLevel::Debug);
        Log::info("=== VR Game Converter v{} ===", "0.2.0");
        Config::instance().load(Config::instance().config_dir() / "config.json");

        const auto profile = Config::instance().current_profile_copy();
        const bool use_bridge = profile.injection_method == InjectionMethod::SixDofBridge;
        Log::info("Injection method: {} (legacy actions {})",
                  use_bridge ? "6DoF Bridge" : "Legacy SendInput",
                  (!use_bridge || profile.bridge_send_legacy_input) ? "enabled" : "disabled");
        if (use_bridge) {
            XRStateBridge::instance().initialize(
                profile.bridge_name, static_cast<u32>(profile.injection_method));
        } else {
            XRStateBridge::instance().shutdown();
        }

        PerfMonitor::instance().initialize();

        InputProxy::instance().initialize();
        const std::string input_profile =
            profile.input_profile.empty() ? "default_vr" : profile.input_profile;
        if (!InputMapper::instance().load_profile(input_profile)) {
            Log::warn("Input profile '{}' not found, falling back to default_vr", input_profile);
            InputMapper::instance().load_profile("default_vr");
        }

        reset_present_state();

        // The callbacks must be set before the hooks can call them.
        auto& hooks = HookManager::instance();
        hooks.initialize();
        hooks.set_on_present([this](HookContext& ctx) { return on_game_present(ctx); });
        hooks.set_on_frame([this](const FrameCapture& capture) { on_frame_capture(capture); });
        const bool hooked = hooks.install_hooks();

        // Running even when hooking failed, so stop() still tears down what
        // was started above.
        state_ = State::Running;
        Log::info(hooked ? "VR Game Converter started"
                         : "VR Game Converter started without a Present hook");
        return hooked;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Stopped) return;
        Log::info("Stopping VR Game Converter");

        // Unhook first and wait for the detours to leave, so no Present
        // callback runs while the subsystems below are torn down.
        HookManager::instance().shutdown();
        // Joins the XR frame thread, which renders with StereoRenderer's
        // resources; the renderer must outlive it.
        OpenXRContext::instance().shutdown();
        StereoRenderer::instance().shutdown();
        ImGuiOverlay::instance().shutdown();
        TrackingSystem::instance().shutdown();
        // Release injected keys while InputProxy can still send the key-ups.
        InputMapper::instance().release_all_held();
        InputProxy::instance().shutdown();
        XRStateBridge::instance().shutdown();
        PerfMonitor::instance().shutdown();

        Config::instance().save(Config::instance().config_dir() / "config.json");
        Log::info("VR Game Converter stopped");
        Log::shutdown();

        reset_present_state();
        state_ = State::Stopped;
    }

private:
    VRConverter() = default;

    enum class State { Stopped, Running };
    enum class PipelineState { Pending, Ready, Failed };
    enum class BindState { Pending, Bound, Failed };

    // ─── Present thread ──────────────────────────────────────────────────
    // The hook serializes these callbacks. stop() resets their state only
    // after the hooks have drained, so they need no lock.

    bool on_game_present(HookContext& ctx) {
        if (ctx.api != present_api_) {
            present_api_ = ctx.api;
            Config::instance().set_detected_api(ctx.api);
            Log::info("Game presents with {}", api_name(ctx.api));
        }

        if (pipeline_ == PipelineState::Failed &&
            std::chrono::steady_clock::now() >= next_pipeline_attempt_) {
            pipeline_ = PipelineState::Pending;
        }
        if (pipeline_ == PipelineState::Pending) {
            if (initialize_vr_pipeline(ctx)) {
                pipeline_ = PipelineState::Ready;
            } else {
                // Usually the headset or OpenXR runtime is not running yet.
                // Retry with backoff (5 s doubling to 60 s) instead of leaving
                // the game in 2D for the rest of the session.
                pipeline_ = PipelineState::Failed;
                next_pipeline_attempt_ = std::chrono::steady_clock::now() + pipeline_retry_delay_;
                Log::warn("VR pipeline unavailable; retrying in {} s",
                          pipeline_retry_delay_.count());
                pipeline_retry_delay_ = std::min(pipeline_retry_delay_ * 2,
                                                 std::chrono::seconds(60));
            }
        }
        if (pipeline_ != PipelineState::Ready) return true;

        bind_device(ctx);
        if (bind_ == BindState::Bound && !overlay_attempted_) initialize_overlay(ctx);

        auto& perf = PerfMonitor::instance();
        perf.begin_frame(StereoRenderer::instance().frame_count() + 1);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_input_poll_ >= kInputPollInterval) {
            InputProxy::instance().poll();
            last_input_poll_ = now;
        }

        // Draw before the frame callback copies the backbuffer, so the menu is
        // also in the image the headset shows.
        draw_overlay(ctx);

        perf.end_frame();
        Config::instance().set_latency_stats(perf.get_latency_stats());
        return true;
    }

    void on_frame_capture(const FrameCapture& capture) {
        if (pipeline_ != PipelineState::Ready) {
            // StereoRenderer is not initialized and will not consume the
            // capture, so release the depth reference the hook added for it.
            if (capture.depth_texture) static_cast<IUnknown*>(capture.depth_texture)->Release();
            return;
        }
        StereoRenderer::instance().on_frame_present(capture);
    }

    bool initialize_vr_pipeline(const HookContext& ctx) {
        Log::info("Initializing VR pipeline on first {} Present", api_name(ctx.api));
        if (!StereoRenderer::instance().initialize(ctx.api)) {
            Log::error("Failed to initialize stereo renderer");
            return false;
        }
        TrackingSystem::instance().initialize(Config::instance().current_profile_copy().ipd);
        Log::info("VR pipeline initialized");
        return true;
    }

    // Binds the presenting device to OpenXR once. A failed attempt is retried
    // a few times, spaced out, and errors are logged on the first attempt only.
    void bind_device(const HookContext& ctx) {
        if (bind_ != BindState::Pending) return;
        if (frames_until_bind_retry_ > 0) {
            --frames_until_bind_retry_;
            return;
        }

        const bool log_errors = bind_attempts_ == 0;
        bool bound = false;
        if (ctx.api == GraphicsAPI::D3D11) {
            bound = bind_d3d11(ctx);
        } else if (ctx.api == GraphicsAPI::D3D12) {
            bound = bind_d3d12(ctx, log_errors);
        }
        if (bound) {
            bind_ = BindState::Bound;
            return;
        }

        if (++bind_attempts_ >= kMaxBindAttempts) {
            bind_ = BindState::Failed;
            Log::error("Could not bind the game's {} device after {} attempts; VR output disabled",
                       api_name(ctx.api), bind_attempts_);
        } else {
            frames_until_bind_retry_ = kFramesBetweenBindAttempts;
        }
    }

    bool bind_d3d11(const HookContext& ctx) {
        auto* device = static_cast<ID3D11Device*>(ctx.device);
        if (!device) return false;
        ID3D11DeviceContext* immediate = nullptr;
        device->GetImmediateContext(&immediate);
        if (!immediate) return false;

        // The game owns the device. GetImmediateContext() added a reference
        // that StereoRenderer owns and releases in shutdown().
        StereoRenderer::instance().set_d3d11_device_ownership(false, true);
        OpenXRContext::instance().bind_d3d11_device(device, immediate);
        Log::info("Bound the game's D3D11 device to OpenXR");
        return true;
    }

    bool bind_d3d12(const HookContext& ctx, bool log_errors) {
        auto* device = static_cast<ID3D12Device*>(ctx.device);
        auto* queue = static_cast<ID3D12CommandQueue*>(ctx.command_queue);
        if (!device || !queue) return false;

        // Wrap the game's D3D12 device and DIRECT queue in a D3D11 device, so
        // the D3D11 OpenXR binding (supported by every runtime) can work on
        // the game's D3D12 backbuffers.
        ID3D11Device* d3d11_device = nullptr;
        ID3D11DeviceContext* d3d11_context = nullptr;
        IUnknown* queues[] = { queue };
        const HRESULT hr = D3D11On12CreateDevice(
            device, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            queues, 1, 0, &d3d11_device, &d3d11_context, nullptr);
        if (FAILED(hr) || !d3d11_device || !d3d11_context) {
            if (log_errors) Log::error("D3D11On12CreateDevice failed: {:08x}", static_cast<u32>(hr));
            if (d3d11_device) d3d11_device->Release();
            if (d3d11_context) d3d11_context->Release();
            return false;
        }

        // Without ID3D11On12Device the backbuffer cannot be wrapped, so do not
        // bind a device that could never deliver a frame.
        ID3D11On12Device* on12 = nullptr;
        if (FAILED(d3d11_device->QueryInterface(IID_PPV_ARGS(&on12)))) {
            if (log_errors) Log::error("D3D11On12 device does not expose ID3D11On12Device");
            d3d11_device->Release();
            d3d11_context->Release();
            return false;
        }

        // StereoRenderer owns all three references from here and releases
        // them in shutdown().
        auto& renderer = StereoRenderer::instance();
        renderer.set_d3d11_on12_device(on12);
        renderer.set_d3d11_device_ownership(true, true);
        // Also hands the device to StereoRenderer::set_d3d11_device().
        OpenXRContext::instance().bind_d3d11_device(d3d11_device, d3d11_context);

        on12_device_ = d3d11_device;
        on12_context_ = d3d11_context;
        on12_ = on12;
        Log::info("Bound a D3D11On12 device on the game's D3D12 device to OpenXR");
        return true;
    }

    // One attempt per session; the overlay is optional.
    void initialize_overlay(const HookContext& ctx) {
        overlay_attempted_ = true;

        HWND hwnd = nullptr;
        DXGI_SWAP_CHAIN_DESC desc = {};
        auto* swap_chain = static_cast<IDXGISwapChain*>(ctx.swap_chain);
        if (swap_chain && SUCCEEDED(swap_chain->GetDesc(&desc))) hwnd = desc.OutputWindow;
        if (!hwnd) {
            Log::warn("Overlay disabled: the swap chain has no output window");
            return;
        }

        auto& overlay = ImGuiOverlay::instance();
        bool ok = false;
        if (ctx.api == GraphicsAPI::D3D11) {
            ok = overlay.initialize_d3d11(static_cast<ID3D11Device*>(ctx.device), hwnd);
        } else if (ctx.api == GraphicsAPI::D3D12 && on12_) {
            ok = overlay.initialize_d3d11on12(on12_device_, on12_context_, on12_, hwnd);
        }
        if (!ok) Log::warn("ImGui overlay initialization failed; the F2 menu is unavailable");
    }

    void draw_overlay(const HookContext& ctx) {
        auto& overlay = ImGuiOverlay::instance();
        if (!Config::instance().enable_overlay() || !overlay.is_initialized()) return;
        overlay.poll_hotkey();

        auto* swap_chain = static_cast<IDXGISwapChain*>(ctx.swap_chain);
        if (ctx.api == GraphicsAPI::D3D11) {
            // Buffer 0 is always the one being presented in D3D11.
            ID3D11Texture2D* back_buffer = nullptr;
            if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
                overlay.render_d3d11(back_buffer);
                back_buffer->Release();
            }
        } else if (ctx.api == GraphicsAPI::D3D12) {
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                ID3D12Resource* back_buffer = nullptr;
                if (SUCCEEDED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(),
                                             IID_PPV_ARGS(&back_buffer)))) {
                    overlay.render_d3d12(back_buffer);
                    back_buffer->Release();
                }
                sc3->Release();
            }
        }
    }

    void reset_present_state() {
        pipeline_ = PipelineState::Pending;
        next_pipeline_attempt_ = {};
        pipeline_retry_delay_ = std::chrono::seconds(5);
        bind_ = BindState::Pending;
        bind_attempts_ = 0;
        frames_until_bind_retry_ = 0;
        overlay_attempted_ = false;
        present_api_ = GraphicsAPI::Unknown;
        on12_device_ = nullptr;
        on12_context_ = nullptr;
        on12_ = nullptr;
        last_input_poll_ = {};
    }

    std::mutex mutex_;
    State state_ = State::Stopped;

    PipelineState pipeline_ = PipelineState::Pending;
    std::chrono::steady_clock::time_point next_pipeline_attempt_{};
    std::chrono::seconds pipeline_retry_delay_{5};
    BindState bind_ = BindState::Pending;
    u32 bind_attempts_ = 0;
    u32 frames_until_bind_retry_ = 0;
    bool overlay_attempted_ = false;
    GraphicsAPI present_api_ = GraphicsAPI::Unknown;
    // D3D11On12 objects of a D3D12 game, owned by StereoRenderer.
    ID3D11Device* on12_device_ = nullptr;
    ID3D11DeviceContext* on12_context_ = nullptr;
    ID3D11On12Device* on12_ = nullptr;
    std::chrono::steady_clock::time_point last_input_poll_{};
};

} // namespace vrc

namespace {

DWORD WINAPI start_thread_proc(LPVOID) {
    vrc::VRConverter::instance().start();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h_module);
        // Pin this module so no FreeLibrary (for example UnhookWindowsHookEx
        // after CBT injection) can unload code our hooks and threads still
        // run. Pinning by address works under any file name.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&start_thread_proc), &pinned);
        // The thread starts running once the loader lock is released.
        HANDLE thread = CreateThread(nullptr, 0, start_thread_proc, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    // DLL_PROCESS_DETACH does nothing. The module is pinned, so detach only
    // happens at process exit, when the other threads are already gone and
    // nothing may be joined or unhooked under the loader lock.
    // StopVRConverter() is the teardown path.
    return TRUE;
}

// ─── Exports ────────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void StartVRConverter() {
    vrc::VRConverter::instance().start();
}

extern "C" __declspec(dllexport) void StopVRConverter() {
    vrc::VRConverter::instance().stop();
}

// SetWindowsHookEx callback. Windows loads this DLL into the target process
// when the hooked message is processed, which bypasses anti-cheat that blocks
// CreateRemoteThread + VirtualAllocEx. Nothing is initialized here: D3D/DXGI
// calls made from inside a CBT callback are unreliable, so DLL_PROCESS_ATTACH
// starts the converter on its own thread. The module is pinned there, so
// UnhookWindowsHookEx does not unload it.
extern "C" __declspec(dllexport) LRESULT CALLBACK VRCbtProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
