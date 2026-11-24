#pragma once

#include "core/types.h"
#include <atomic>

namespace vrc {

// Counts threads executing a detour body. unhook() disables the hooks, then
// waits for the count to reach zero before MinHook frees the trampolines those
// threads call through.
class InFlightGuard {
public:
    explicit InFlightGuard(std::atomic<i32>& count) : count_(count) {
        count_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~InFlightGuard() { count_.fetch_sub(1, std::memory_order_acq_rel); }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;

private:
    std::atomic<i32>& count_;
};

// Waits until `count` is zero. Call only after MH_DisableHook so no new thread
// can enter the detours. Returns false if threads are still inside after
// timeout_ms; the caller must then leave the trampolines allocated.
bool wait_for_detours_to_drain(const std::atomic<i32>& count, u32 timeout_ms);

// Initializes MinHook once; safe to call from any hook installer.
bool ensure_minhook_initialized();

class HookManager {
public:
    static HookManager& instance();

    // Logs which graphics runtimes are already loaded. This is only a hint:
    // the API actually used is read from each swap chain's device at Present.
    void initialize();

    // Installs the DXGI Present/Present1 hooks (all D3D11 and D3D12 swap chains
    // go through them) and the D3D12 command-queue hooks. The D3D12 part is
    // deferred to a watcher thread when d3d12.dll is not loaded yet.
    bool install_hooks();

    // Removes the D3D11 and the D3D12 hooks and waits for detours to drain.
    void shutdown();

    GraphicsAPI api_hint() const { return api_hint_; }

    // Set the callbacks before install_hooks(); the detours read them without
    // a lock.
    void set_on_present(OnPresentCallback callback) { on_present_ = std::move(callback); }
    void set_on_frame(OnFrameCallback callback) { on_frame_ = std::move(callback); }

    void fire_on_present(HookContext& ctx) { if (on_present_) on_present_(ctx); }
    // The capture's depth_texture is AddRef'd for the consumer; it is released
    // here when no frame callback is set.
    void fire_on_frame(const FrameCapture& capture);

private:
    HookManager() = default;

    GraphicsAPI api_hint_ = GraphicsAPI::Unknown;
    OnPresentCallback on_present_;
    OnFrameCallback on_frame_;
};

} // namespace vrc
