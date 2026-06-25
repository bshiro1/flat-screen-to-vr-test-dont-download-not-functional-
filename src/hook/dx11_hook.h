#pragma once

#include "core/types.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <functional>
#include <vector>

namespace vrc {

class HookManager; // forward

class D3D11Hook {
public:
    static D3D11Hook& instance();

    bool hook_present(HookManager& mgr);
    void unhook();

    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }
    IDXGISwapChain* swap_chain() const { return captured_swap_chain_; }

    using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT,
        const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    using CreateDeviceFn = HRESULT(WINAPI*)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT,
        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);

private:
    D3D11Hook() = default;

    static HRESULT WINAPI create_device_and_swapchain_detour(
        IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE module,
        UINT flags, const D3D_FEATURE_LEVEL* feature_levels, UINT num_levels,
        UINT sdk_version, const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
        IDXGISwapChain** swap_chain, ID3D11Device** device,
        D3D_FEATURE_LEVEL* obtained_level, ID3D11DeviceContext** context);

    static HRESULT __stdcall present_detour(IDXGISwapChain* swap_chain,
                                             UINT sync_interval, UINT flags);

    void hook_swapchain_present(IDXGISwapChain* swap_chain);
    bool hook_present_globally();

    static HRESULT WINAPI create_device_detour(
        IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE module,
        UINT flags, const D3D_FEATURE_LEVEL* feature_levels, UINT num_levels,
        UINT sdk_version, ID3D11Device** device,
        D3D_FEATURE_LEVEL* obtained_level, ID3D11DeviceContext** context);

    CreateDeviceAndSwapChainFn original_create_device_and_swapchain_ = nullptr;
    CreateDeviceFn original_create_device_ = nullptr;
    PresentFn original_present_ = nullptr;
    void* present_func_ = nullptr;
    std::vector<std::pair<void*, PresentFn>> extra_present_hooks_;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* captured_swap_chain_ = nullptr;

    HookManager* hook_manager_ = nullptr;
    bool hooked_ = false;
    bool hook_present_on_next_swapchain_ = false;

};

} // namespace vrc
