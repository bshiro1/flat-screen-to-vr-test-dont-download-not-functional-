#include "hook_manager.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "core/logging.h"
#include <windows.h>
#include <MinHook.h>

namespace vrc {

bool wait_for_detours_to_drain(const std::atomic<i32>& count, u32 timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (count.load(std::memory_order_acquire) != 0) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
    return true;
}

bool ensure_minhook_initialized() {
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log::error("MinHook initialization failed: {}", static_cast<int>(status));
        return false;
    }
    return true;
}

HookManager& HookManager::instance() {
    static HookManager mgr;
    return mgr;
}

void HookManager::initialize() {
    // vr_converter.dll delay-loads d3d12.dll, so a loaded d3d12.dll means the
    // game (or one of its libraries) loaded it. A game can still load both
    // runtimes, so this never decides which hooks to install.
    if (GetModuleHandleW(L"d3d12.dll")) {
        api_hint_ = GraphicsAPI::D3D12;
    } else if (GetModuleHandleW(L"d3d11.dll")) {
        api_hint_ = GraphicsAPI::D3D11;
    } else {
        api_hint_ = GraphicsAPI::Unknown;
    }
    Log::info("HookManager: graphics API hint = {} (the swap chain device decides at Present)",
              api_hint_ == GraphicsAPI::D3D12 ? "D3D12" :
              api_hint_ == GraphicsAPI::D3D11 ? "D3D11" : "none loaded yet");
}

bool HookManager::install_hooks() {
    const bool present_ok = D3D11Hook::instance().hook_present(*this);
    if (!present_ok) {
        Log::error("DXGI Present hook could not be installed; no frames will be captured");
    }
    // D3D12 swap chains are handled by the DXGI Present hook too; the D3D12
    // hook only records the game's DIRECT command queue, which D3D11On12 needs.
    D3D12Hook::instance().hook();
    return present_ok;
}

void HookManager::shutdown() {
    Log::info("HookManager shutting down");
    // Present first: its detour is what drives the VR pipeline, and it can
    // submit D3D12 work through ExecuteCommandLists while it runs.
    D3D11Hook::instance().unhook();
    D3D12Hook::instance().unhook();
}

void HookManager::fire_on_frame(const FrameCapture& capture) {
    if (on_frame_) {
        on_frame_(capture);
    } else if (capture.depth_texture) {
        static_cast<IUnknown*>(capture.depth_texture)->Release();
    }
}

} // namespace vrc
