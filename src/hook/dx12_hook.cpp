#include "dx12_hook.h"
#include "hook_manager.h"
#include "core/logging.h"
#include <MinHook.h>

namespace vrc {

namespace {

// Set while this thread creates our own probe device, so create_device_detour
// does not treat it as the game's device.
thread_local bool tl_creating_probe_device = false;

constexpr u32 kDrainTimeoutMs = 2000;
constexpr DWORD kWatcherJoinTimeoutMs = 5000;

} // namespace

D3D12Hook& D3D12Hook::instance() {
    static D3D12Hook hook;
    return hook;
}

void D3D12Hook::hook() {
    if (hooked_) return;
    hooked_ = true;
    stopping_.store(false, std::memory_order_release);

    if (try_install()) return;

    // vr_converter.dll delay-loads d3d12.dll, so it is only present once the
    // game loads it. Poll for it instead of loading it into D3D11 games.
    watcher_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (watcher_stop_event_) {
        watcher_thread_ = CreateThread(nullptr, 0, watcher_thread_proc, nullptr, 0, nullptr);
    }
    if (!watcher_thread_) {
        if (watcher_stop_event_) { CloseHandle(watcher_stop_event_); watcher_stop_event_ = nullptr; }
        Log::warn("Could not start the d3d12.dll watcher thread; D3D12 queue capture disabled");
        return;
    }
    Log::info("d3d12.dll not loaded yet; D3D12 hooks will be installed when it loads");
}

DWORD WINAPI D3D12Hook::watcher_thread_proc(LPVOID) {
    auto& self = instance();
    // Poll once per second for up to five minutes; exit at once on unhook().
    for (int i = 0; i < 300; ++i) {
        if (WaitForSingleObject(self.watcher_stop_event_, 1000) == WAIT_OBJECT_0) return 0;
        if (self.try_install()) return 0;
    }
    Log::info("d3d12.dll was not loaded within 5 minutes; D3D12 hooks not installed");
    return 0;
}

bool D3D12Hook::try_install() {
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) return false;

    std::lock_guard<std::mutex> lock(install_mutex_);
    if (stopping_.load(std::memory_order_acquire)) return true;
    if (!ensure_minhook_initialized()) return true;

    // The CreateDevice hook covers a game that has not created its device
    // yet and the case where our probe device cannot be created.
    install_create_device_hook(d3d12);
    install_ecl_hook_via_probe_device(d3d12);
    return true;
}

void D3D12Hook::install_create_device_hook(HMODULE d3d12) {
    if (create_device_func_) {
        // Left disabled by an unhook() whose detours did not drain.
        MH_EnableHook(create_device_func_);
        return;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!target) {
        Log::warn("D3D12CreateDevice not found in d3d12.dll");
        return;
    }
    CreateDeviceFn original = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&create_device_detour),
                      reinterpret_cast<void**>(&original)) != MH_OK || !original) {
        Log::warn("MH_CreateHook failed for D3D12CreateDevice");
        return;
    }
    // Publish the trampoline before enabling: the detour may run immediately.
    original_create_device_ = original;
    create_device_func_ = target;
    if (MH_EnableHook(target) != MH_OK) {
        Log::warn("MH_EnableHook failed for D3D12CreateDevice");
        return;
    }
    Log::info("D3D12CreateDevice hook installed");
}

void D3D12Hook::install_ecl_hook_via_probe_device(HMODULE d3d12) {
    if (void* existing = execute_func_.load(std::memory_order_acquire)) {
        MH_EnableHook(existing);
        return;
    }

    // ExecuteCommandLists is shared by every queue of the runtime, so any
    // device gives the game's address. D3D12 devices are per-adapter
    // singletons, so this usually returns the game's own device with one more
    // reference, which is released right away.
    auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!create) return;

    ID3D12Device* device = nullptr;
    tl_creating_probe_device = true;
    const HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    tl_creating_probe_device = false;
    if (FAILED(hr) || !device) {
        Log::info("No probe D3D12 device ({:08x}); ExecuteCommandLists is hooked when the game "
                  "creates its device", static_cast<u32>(hr));
        return;
    }
    install_ecl_hook(device);
    device->Release();
}

bool D3D12Hook::install_ecl_hook(ID3D12Device* device) {
    if (execute_func_.load(std::memory_order_acquire)) return true;

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* probe_queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probe_queue))) || !probe_queue) {
        Log::warn("Could not create a probe command queue for the ExecuteCommandLists hook");
        return false;
    }

    // ID3D12CommandQueue vtable: 0 QueryInterface, 1 AddRef, 2 Release,
    // 3 GetPrivateData, 4 SetPrivateData, 5 SetPrivateDataInterface, 6 SetName,
    // 7 GetDevice, 8 UpdateTileMappings, 9 CopyTileMappings,
    // 10 ExecuteCommandLists.
    void* target = (*reinterpret_cast<void***>(probe_queue))[10];
    probe_queue->Release();

    ExecuteCommandListsFn original = nullptr;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&execute_command_lists_detour),
                      reinterpret_cast<void**>(&original)) != MH_OK || !original) {
        Log::warn("MH_CreateHook failed for ExecuteCommandLists");
        return false;
    }
    original_execute_command_lists_ = original;
    execute_func_.store(target, std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK) {
        Log::warn("MH_EnableHook failed for ExecuteCommandLists");
        return false;
    }
    Log::info("D3D12 ExecuteCommandLists hook installed");
    return true;
}

void D3D12Hook::unhook() {
    if (!hooked_) return;
    stopping_.store(true, std::memory_order_release);

    if (watcher_thread_) {
        SetEvent(watcher_stop_event_);
        if (WaitForSingleObject(watcher_thread_, kWatcherJoinTimeoutMs) == WAIT_OBJECT_0) {
            CloseHandle(watcher_stop_event_);
        } else {
            // The thread still waits on the event; leave it open. It sees
            // stopping_ and exits without installing anything.
            Log::warn("d3d12.dll watcher thread did not exit in time");
        }
        CloseHandle(watcher_thread_);
        watcher_thread_ = nullptr;
        watcher_stop_event_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(install_mutex_);
        if (void* ecl = execute_func_.load(std::memory_order_acquire)) MH_DisableHook(ecl);
        if (create_device_func_) MH_DisableHook(create_device_func_);
    }

    // Wait without install_mutex_: a game thread inside create_device_detour
    // may be waiting for it.
    const bool drained = wait_for_detours_to_drain(in_flight_, kDrainTimeoutMs);

    {
        std::lock_guard<std::mutex> lock(install_mutex_);
        if (drained) {
            if (void* ecl = execute_func_.load(std::memory_order_acquire)) MH_RemoveHook(ecl);
            if (create_device_func_) MH_RemoveHook(create_device_func_);
            execute_func_.store(nullptr, std::memory_order_release);
            original_execute_command_lists_ = nullptr;
            create_device_func_ = nullptr;
            original_create_device_ = nullptr;
        } else {
            // A thread is still inside a detour and will return through its
            // trampoline, so keep the hooks allocated (disabled). hook()
            // re-enables them on the next start.
            Log::warn("D3D12 detours still running after {} ms; hooks left disabled", kDrainTimeoutMs);
        }
    }

    release_queues();
    hooked_ = false;
    Log::info("D3D12 hooks removed");
}

ID3D12CommandQueue* D3D12Hook::command_queue_for(ID3D12Device* device) {
    if (!device) return nullptr;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    int hit = -1;
    for (int i = 0; i < kMaxQueueSlots; ++i) {
        if (slot_device_[i].load(std::memory_order_relaxed) == device) { hit = i; break; }
    }
    if (hit < 0) return nullptr;

    ID3D12CommandQueue* queue = slot_queue_[hit].load(std::memory_order_relaxed);
    queue->AddRef();

    // The presenting device is the one that matters. Drop queues of other
    // devices so our references do not keep a device the game replaced alive.
    for (int i = 0; i < kMaxQueueSlots; ++i) {
        if (i == hit) continue;
        ID3D12CommandQueue* q = slot_queue_[i].exchange(nullptr, std::memory_order_acq_rel);
        ID3D12Device* d = slot_device_[i].exchange(nullptr, std::memory_order_acq_rel);
        if (q) q->Release();
        if (d) d->Release();
    }
    return queue;
}

void D3D12Hook::release_queues() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (int i = 0; i < kMaxQueueSlots; ++i) {
        ID3D12CommandQueue* q = slot_queue_[i].exchange(nullptr, std::memory_order_acq_rel);
        ID3D12Device* d = slot_device_[i].exchange(nullptr, std::memory_order_acq_rel);
        if (q) q->Release();
        if (d) d->Release();
    }
}

void D3D12Hook::record_direct_queue(ID3D12CommandQueue* queue) {
    // Fast path, no lock: this queue is already captured.
    for (const auto& slot : slot_queue_) {
        if (slot.load(std::memory_order_acquire) == queue) return;
    }
    if (stopping_.load(std::memory_order_relaxed)) return;

    // D3D11On12 and Present need the DIRECT queue; ignore compute/copy.
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;

    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) return;

    // Keep the first DIRECT queue captured for a device; another DIRECT queue
    // on the same device does not replace it.
    for (const auto& slot : slot_device_) {
        if (slot.load(std::memory_order_acquire) == device) {
            device->Release();
            return;
        }
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    int free_slot = -1;
    for (int i = 0; i < kMaxQueueSlots; ++i) {
        ID3D12Device* d = slot_device_[i].load(std::memory_order_relaxed);
        if (d == device) {
            device->Release();
            return;
        }
        if (!d && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        device->Release();
        return;
    }

    queue->AddRef();
    slot_queue_[free_slot].store(queue, std::memory_order_release);
    slot_device_[free_slot].store(device, std::memory_order_release); // keeps GetDevice's ref

    static int logged = 0;
    if (logged < 8) {
        ++logged;
        Log::info("Captured D3D12 DIRECT command queue {:p} for device {:p}",
                  static_cast<void*>(queue), static_cast<void*>(device));
    }
}

HRESULT WINAPI D3D12Hook::create_device_detour(
    IUnknown* adapter, D3D_FEATURE_LEVEL min_level,
    REFIID riid, void** pp_device)
{
    auto& self = instance();
    InFlightGuard guard(self.in_flight_);

    const HRESULT hr = self.original_create_device_(adapter, min_level, riid, pp_device);
    if (FAILED(hr) || !pp_device || !*pp_device || tl_creating_probe_device) return hr;
    if (self.execute_func_.load(std::memory_order_acquire) ||
        self.stopping_.load(std::memory_order_acquire)) {
        return hr;
    }

    // riid can name any ID3D12Device revision (or something else entirely),
    // so ask the returned object for the base interface.
    ID3D12Device* device = nullptr;
    if (FAILED(static_cast<IUnknown*>(*pp_device)->QueryInterface(IID_PPV_ARGS(&device)))) {
        return hr;
    }
    {
        std::lock_guard<std::mutex> lock(self.install_mutex_);
        if (!self.stopping_.load(std::memory_order_acquire)) {
            self.install_ecl_hook(device);
        }
    }
    device->Release();
    return hr;
}

void __stdcall D3D12Hook::execute_command_lists_detour(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto& self = instance();
    InFlightGuard guard(self.in_flight_);
    if (queue) self.record_direct_queue(queue);
    self.original_execute_command_lists_(queue, count, lists);
}

} // namespace vrc
