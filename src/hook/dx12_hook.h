#pragma once

#include "core/types.h"
#include <d3d12.h>
#include <atomic>
#include <mutex>

namespace vrc {

// Records the DIRECT command queue a D3D12 game submits on, so the Present
// path can create a D3D11On12 device on it. Present itself is hooked in
// D3D11Hook (DXGI), which handles D3D12 swap chains too.
class D3D12Hook {
public:
    static D3D12Hook& instance();

    // Installs the ExecuteCommandLists and D3D12CreateDevice hooks now if
    // d3d12.dll is loaded, otherwise starts a watcher thread that installs
    // them once it loads. unhook() stops the watcher.
    void hook();
    void unhook();

    // Returns an AddRef'd DIRECT queue that the game submitted on for
    // `device`, or nullptr if none has been seen yet. The caller releases it.
    ID3D12CommandQueue* command_queue_for(ID3D12Device* device);

    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using ExecuteCommandListsFn = void(__stdcall*)(
        ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

private:
    D3D12Hook() = default;

    static HRESULT WINAPI create_device_detour(
        IUnknown* adapter, D3D_FEATURE_LEVEL min_level,
        REFIID riid, void** pp_device);
    static void __stdcall execute_command_lists_detour(
        ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    static DWORD WINAPI watcher_thread_proc(LPVOID);

    // Returns false while d3d12.dll is not loaded.
    bool try_install();
    // install_mutex_ must be held for the three helpers below.
    void install_create_device_hook(HMODULE d3d12);
    void install_ecl_hook_via_probe_device(HMODULE d3d12);
    bool install_ecl_hook(ID3D12Device* device);

    void record_direct_queue(ID3D12CommandQueue* queue);
    void release_queues();

    std::mutex install_mutex_;
    // Set while unhook() runs so a detour cannot install new hooks meanwhile.
    std::atomic<bool> stopping_{false};
    bool hooked_ = false;
    HANDLE watcher_thread_ = nullptr;
    HANDLE watcher_stop_event_ = nullptr;

    void* create_device_func_ = nullptr;
    CreateDeviceFn original_create_device_ = nullptr;
    std::atomic<void*> execute_func_{nullptr};
    ExecuteCommandListsFn original_execute_command_lists_ = nullptr;
    std::atomic<i32> in_flight_{0};

    // One captured DIRECT queue per device. Both pointers in a slot are
    // AddRef'd. Written under queue_mutex_; the ExecuteCommandLists fast path
    // reads them without the lock and only compares pointers.
    static constexpr int kMaxQueueSlots = 4;
    std::mutex queue_mutex_;
    std::atomic<ID3D12Device*> slot_device_[kMaxQueueSlots] = {};
    std::atomic<ID3D12CommandQueue*> slot_queue_[kMaxQueueSlots] = {};
};

} // namespace vrc
