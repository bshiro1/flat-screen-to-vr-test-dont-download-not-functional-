#include "core/logging.h"
#include "core/config.h"
#include "core/types.h"
#include "core/perf_monitor.h"
#include "hook/hook_manager.h"
#include "hook/dx11_hook.h"
#include "hook/dx12_hook.h"
#include "render/stereo_renderer.h"
#include "render/latency_compensator.h"
#include "render/depth_reprojection.h"
#include "vr/openxr_context.h"
#include "vr/tracking.h"
#include "overlay/imgui_overlay.h"
#include "overlay/config_editor.h"
#include "input/input_proxy.h"
#include "input/input_mapper.h"

#include <windows.h>
#include <d3d11on12.h>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <mutex>

namespace vrc {

// ─── Global Bootstrap ───────────────────────────────────────────────────────

class VRConverter {
public:
    static VRConverter& instance() {
        static VRConverter conv;
        return conv;
    }

    bool initialize() {
        if (initialized_.load(std::memory_order_acquire)) return true;

        auto log_path = Config::instance().log_path();
        Log::init(log_path, LogLevel::Debug);
        Log::info("=== VR Game Converter v{} (Phase 2) ===", "0.2.0");

        auto config_path = Config::instance().config_dir() / "config.json";
        Config::instance().load(config_path);

        // Initialize Phase 2 subsystems early
        PerfMonitor::instance().initialize();
        LatencyCompensator::instance().initialize();

        // Initialize input
        InputProxy::instance().initialize();
        InputMapper::instance().load_profile("default_vr");

        // Initialize hooking framework
        auto& hooks = HookManager::instance();
        hooks.initialize();

        auto api = hooks.detected_api();
        if (api == GraphicsAPI::Unknown) {
            Log::warn("No supported graphics API detected yet — installing D3D11+D3D12 hooks speculatively");
        }

        // Always install both D3D11 and D3D12 hooks:
        //   - D3D11 hook uses global Present vtable patching (works even if d3d11.dll isn't loaded yet)
        //   - D3D12 hook starts a deferred thread that polls for d3d12.dll to load
        // UE4 games load D3D11 early but may use D3D12 later — we need both.
        if (hooks.hook_present(GraphicsAPI::D3D11)) {
            Log::info("Present hook installed for D3D11");
        }
        if (hooks.hook_present(GraphicsAPI::D3D12)) {
            Log::info("Present hook installed for D3D12");
        }
        if (api != GraphicsAPI::D3D11 && api != GraphicsAPI::D3D12 && api != GraphicsAPI::Unknown) {
            if (hooks.hook_present(api)) {
                Log::info("Present hook installed for API {}", static_cast<int>(api));
            } else {
                Log::error("Failed to install Present hook for API {}", static_cast<int>(api));
            }
        }

        // Set up present callback — main rendering loop
        hooks.set_on_present([this](HookContext& ctx) -> bool {
            return on_game_present(ctx);
        });

        // Set up frame capture callback — stereo rendering + depth
        hooks.set_on_frame([this](const FrameCapture& capture) {
            on_frame_capture(capture);
        });

        initialized_ = true;
        Log::info("VR Game Converter Phase 2 initialized");
        return true;
    }

    void shutdown() {
        Log::info("Shutting down VR Game Converter Phase 2");

        StereoRenderer::instance().shutdown();
        ImGuiOverlay::instance().shutdown();
        TrackingSystem::instance().shutdown();
        HookManager::instance().shutdown();
        InputProxy::instance().shutdown();
        PerfMonitor::instance().shutdown();
        LatencyCompensator::instance().shutdown();

        Config::instance().save(Config::instance().config_dir() / "config.json");
        Log::shutdown();
    }

    bool on_game_present(HookContext& ctx) {
        if (!stereo_initialized_.load(std::memory_order_relaxed) && !init_failed_.load(std::memory_order_relaxed)) {
            if (!initialize_vr_pipeline(ctx)) {
                init_failed_.store(true, std::memory_order_release);
                return true;
            }
            stereo_initialized_.store(true, std::memory_order_release);
        }

        // Phase 2: Bind real D3D device to OpenXR if not yet bound
        if (!OpenXRContext::instance().is_device_bound()) {
            if (ctx.api == GraphicsAPI::D3D11 && ctx.device) {
                ID3D11Device* d3d11 = static_cast<ID3D11Device*>(ctx.device);
                ID3D11DeviceContext* d3d11_ctx = nullptr;
                d3d11->GetImmediateContext(&d3d11_ctx);
                OpenXRContext::instance().bind_d3d11_device(d3d11, d3d11_ctx);
                Log::info("Bound D3D11 device to OpenXR");
            } else if (ctx.api == GraphicsAPI::D3D12 && ctx.device) {
                ID3D12Device* d3d12 = static_cast<ID3D12Device*>(ctx.device);
                ID3D12CommandQueue* queue = static_cast<ID3D12CommandQueue*>(ctx.command_queue);
                if (!queue) { queue = D3D12Hook::instance().command_queue(); }
                if (d3d12 && queue) {
                    // Create D3D11On12 device wrapping the game's D3D12 device + queue.
                    // This allows us to use D3D11 OpenXR binding (which works on all
                    // runtimes) while operating on the game's D3D12 resources.
                    ID3D11Device* d3d11_dev = nullptr;
                    ID3D11DeviceContext* d3d11_on12_ctx = nullptr;
                    IUnknown* q[] = { queue };
                    HRESULT hr = D3D11On12CreateDevice(
                        d3d12, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                        nullptr, 0, q, 1, 0,
                        &d3d11_dev, &d3d11_on12_ctx, nullptr);
                    if (SUCCEEDED(hr) && d3d11_dev && d3d11_on12_ctx) {
                        ID3D11On12Device* d3d11_on12 = nullptr;
                        if (SUCCEEDED(d3d11_dev->QueryInterface(IID_PPV_ARGS(&d3d11_on12)))) {
                            StereoRenderer::instance().set_d3d11_on12_device(d3d11_on12);
                        }
                        StereoRenderer::instance().set_d3d11_device(d3d11_dev, d3d11_on12_ctx);
                        OpenXRContext::instance().bind_d3d11_device(d3d11_dev, d3d11_on12_ctx);
                        Log::info("Bound D3D11On12 device to OpenXR (D3D12 game via interop)");
                    } else {
                        Log::error("D3D11On12CreateDevice failed: {:08x}", hr);
                    }
                }
            }
        }

        // Phase 2: Begin latency compensation + performance tracking
        auto& perf = PerfMonitor::instance();
        auto& latency = LatencyCompensator::instance();
        auto& tracking = TrackingSystem::instance();
        u64 frame_idx = StereoRenderer::instance().frame_count() + 1;

        perf.begin_frame(frame_idx);
        latency.begin_frame(frame_idx);

        // Phase 2: Feed hook context into depth capture pipeline
        if (ctx.api == GraphicsAPI::D3D11) {
            // Depth buffer capture handled inside stereo_renderer
        }

        // Phase 2: Apply latency compensation to tracking
        f64 display_time = OpenXRContext::instance().predicted_display_time();
        if (display_time > 0.0) {
            tracking.apply_latency_compensation(display_time);
        }

        latency.on_render_submit();

        // Poll input at ~60 Hz
        static auto last_poll = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll);
        if (elapsed.count() >= 16) {
            InputProxy::instance().poll();
            last_poll = now;
        }

        // Render overlay
        if (Config::instance().enable_overlay()) {
            ImGuiOverlay::instance().render();
        }

        latency.end_frame();
        perf.end_frame();

        // Update config with latest latency stats for overlay display
        Config::instance().latency_stats() = perf.get_latency_stats();

        return true;
    }

    void on_frame_capture(const FrameCapture& capture) {
        // Phase 2: Full stereo rendering pipeline
        StereoRenderer::instance().on_frame_present(capture);
    }

private:
    bool initialize_vr_pipeline(const HookContext& ctx) {
        Log::info("Initializing VR pipeline on first Present");

        auto api = ctx.api;
        if (api == GraphicsAPI::Unknown) {
            api = Config::instance().detected_api();
        }

        // Initialize stereo renderer (initializes OpenXR, camera rig,
        // depth reprojection, and all Phase 2 subsystems)
        if (!StereoRenderer::instance().initialize(api)) {
            Log::error("Failed to initialize stereo renderer");
            return false;
        }

        // Initialize tracking system
        TrackingSystem::instance().initialize(
            Config::instance().current_profile().ipd);

        // Initialize overlay with the D3D device
        void* device = ctx.device;
        if (device) {
            ImGuiOverlay::instance().initialize(device, api);
        }

        Log::info("VR pipeline fully initialized (Phase 2)");
        return true;
    }

    std::atomic<bool> initialized_{false};
    std::atomic<bool> stereo_initialized_{false};
    std::atomic<bool> init_failed_{false};
};

// ─── DLL Entry Point ─────────────────────────────────────────────────────────

HMODULE g_module_handle = nullptr;

// Vectored Exception Handler to catch any exceptions from our Present hook
static LONG CALLBACK VrcVeHandler(PEXCEPTION_POINTERS ei) {
    if (ei->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        ei->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        ei->ExceptionRecord->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION ||
        ei->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        char buf[512];
        sprintf_s(buf, "VRC_VEH: code=0x%08X addr=%p\n",
                  ei->ExceptionRecord->ExceptionCode,
                  ei->ExceptionRecord->ExceptionAddress);
        OutputDebugStringA(buf);
        Log::info("VRC_VEH: code=0x{:08X} addr={:p} flags=0x{:x}",
                  ei->ExceptionRecord->ExceptionCode,
                  ei->ExceptionRecord->ExceptionAddress,
                  ei->ExceptionRecord->ExceptionFlags);
        if (ei->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            Log::info("VRC_VEH: AV {} addr={:p}",
                      ei->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write",
                      (void*)ei->ExceptionRecord->ExceptionInformation[1]);
            // Dump module info
            HMODULE crash_mod = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCWSTR)ei->ExceptionRecord->ExceptionAddress,
                                    &crash_mod)) {
                wchar_t mod_path[MAX_PATH];
                if (GetModuleFileNameW(crash_mod, mod_path, MAX_PATH)) {
                    char mod_path_a[MAX_PATH];
                    mod_path_a[0] = 0;
                    wcstombs(mod_path_a, mod_path, MAX_PATH);
                    char* name = strrchr(mod_path_a, '\\');
                    name = name ? name + 1 : mod_path_a;
                    Log::info("VRC_VEH: crash module: {} (base={:p})",
                              name, (void*)crash_mod);
                }
            }
            // Dump stack return addresses
            CONTEXT* ctx = ei->ContextRecord;
            if (ctx) {
                u64* sp = (u64*)ctx->Rsp;
                Log::info("VRC_VEH: stack (RSP={:p}):", (void*)ctx->Rsp);
                for (int i = 0; i < 16; i++) {
                    Log::info("  [{:2d}] {:p}", i, (void*)sp[i]);
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static volatile LONG g_start_guard = 0;

void start_converter() {
    AddVectoredExceptionHandler(1, VrcVeHandler);
    if (InterlockedExchange(&g_start_guard, 1)) return;
    Log::info("Starting VR Converter Phase 2");
    VRConverter::instance().initialize();
}

void stop_converter() {
    VRConverter::instance().shutdown();
}

} // namespace vrc

// ─── DllMain ─────────────────────────────────────────────────────────────────

DWORD WINAPI delayed_init(LPVOID) {
    Sleep(200);
    vrc::start_converter();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            vrc::g_module_handle = h_module;
            DisableThreadLibraryCalls(h_module);
            // Pin this DLL so it survives UnhookWindowsHookEx (CBT injection)
            // Without this, Windows calls FreeLibrary on unhook and unloads us.
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"vr_converter.dll", &pinned);
            HANDLE thread = CreateThread(nullptr, 0, delayed_init, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            break;
        }
        case DLL_PROCESS_DETACH: {
            if (!reserved) {
                vrc::stop_converter();
            }
            break;
        }
    }
    return TRUE;
}

// ─── Exported Injection Entry Point ─────────────────────────────────────────

extern "C" __declspec(dllexport) void StartVRConverter() {
    vrc::start_converter();
}

extern "C" __declspec(dllexport) void StopVRConverter() {
    vrc::stop_converter();
}

// SetWindowsHookEx callback — Windows loads this DLL into the target
// process when the hooked message is processed. This bypasses anti-cheat
// that blocks CreateRemoteThread + VirtualAllocEx.
extern "C" __declspec(dllexport) LRESULT CALLBACK VRCbtProc(int nCode, WPARAM wParam, LPARAM lParam) {
    // Deliberately do NOT call start_converter() here — D3D11/DXGI APIs
    // behave differently when called from within a Windows CBT hook callback
    // (vtable[8] Present patching via MinHook is not effective).  Instead,
    // DLL_PROCESS_ATTACH spawns delayed_init which handles initialization
    // on a dedicated thread after 200 ms.
    //
    // The DLL is pinned by GET_MODULE_HANDLE_EX_FLAG_PIN during
    // DLL_PROCESS_ATTACH, so UnhookWindowsHookEx won't unload us.
    if (nCode < 0) return CallNextHookEx(nullptr, nCode, wParam, lParam);
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
