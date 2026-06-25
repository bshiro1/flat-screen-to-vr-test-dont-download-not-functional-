#pragma once

#include "core/types.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace vrc {

class HookManager;

class D3D12Hook {
public:
    static D3D12Hook& instance();

    bool hook_present(HookManager& mgr);
    void unhook();
    bool try_hook_create_device();

    ID3D12Device* device() const { return device_; }
    ID3D12CommandQueue* command_queue() const { return captured_queue_; }

    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using ExecuteCommandListsFn = void(__stdcall*)(
        ID3D12CommandQueue*, UINT, ID3D12CommandList**);

private:
    D3D12Hook() = default;

    static HRESULT WINAPI create_device_detour(
        IUnknown* adapter, D3D_FEATURE_LEVEL min_level,
        REFIID riid, void** pp_device);
    static void __stdcall execute_command_lists_detour(
        ID3D12CommandQueue* queue, UINT count, ID3D12CommandList** lists);

    bool install_ecl_hook(ID3D12Device* device);

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* captured_queue_ = nullptr;

    CreateDeviceFn original_create_device_ = nullptr;
    ExecuteCommandListsFn original_execute_command_lists_ = nullptr;
    void* execute_func_ = nullptr;

    HookManager* hook_manager_ = nullptr;
    bool hooked_ = false;
    bool pipeline_initialized_ = false;
    bool skip_next_create_ = false;
};

} // namespace vrc
