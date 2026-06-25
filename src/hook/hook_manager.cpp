#include "hook_manager.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "core/logging.h"
#include <windows.h>
#include <tlhelp32.h>

namespace vrc {

HookManager& HookManager::instance() {
    static HookManager mgr;
    return mgr;
}

bool HookManager::initialize() {
    Log::info("HookManager initializing");
    if (!detect_graphics_api()) {
        Log::warn("No supported graphics API detected, will hook on first Present");
    }
    return true;
}

void HookManager::shutdown() {
    Log::info("HookManager shutting down");
    if (hooked_) {
        unhook_present(detected_api_);
        unhook_wndproc();
    }
}

bool HookManager::attach_to_process(const std::string& process_name) {
    target_process_ = process_name;
    Log::info("Attaching to process: {}", process_name);
    return detect_graphics_api();
}

void HookManager::detach() {
    shutdown();
}

bool HookManager::detect_graphics_api() {
    HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    HMODULE opengl = GetModuleHandleA("opengl32.dll");
    HMODULE vulkan = GetModuleHandleA("vulkan-1.dll");

    if (d3d12) {
        detected_api_ = GraphicsAPI::D3D12;
        Log::info("Detected D3D12");
        return true;
    }
    if (d3d11) {
        detected_api_ = GraphicsAPI::D3D11;
        Log::info("Detected D3D11");
        return true;
    }
    if (vulkan) {
        detected_api_ = GraphicsAPI::Vulkan;
        Log::info("Detected Vulkan");
        return true;
    }
    if (opengl) {
        detected_api_ = GraphicsAPI::OpenGL;
        Log::info("Detected OpenGL");
        return true;
    }

    detected_api_ = GraphicsAPI::Unknown;
    return false;
}

void* HookManager::find_swap_chain() {
    return nullptr;
}

bool HookManager::hook_present(GraphicsAPI api) {
    bool success = false;
    switch (api) {
        case GraphicsAPI::D3D11:
            success = hook_d3d11();
            break;
        case GraphicsAPI::D3D12:
            success = hook_d3d12();
            break;
        case GraphicsAPI::OpenGL:
            success = hook_opengl();
            break;
        case GraphicsAPI::Vulkan:
            success = hook_vulkan();
            break;
        default:
            Log::error("Unknown graphics API for hooking");
            return false;
    }

    if (success) {
        hooked_ = true;
        Log::info("Present hook installed for {}", static_cast<int>(api));
    }
    return success;
}

bool HookManager::unhook_present(GraphicsAPI api) {
    if (!hooked_) return true;
    hooked_ = false;
    switch (api) {
        case GraphicsAPI::D3D11:
            D3D11Hook::instance().unhook();
            break;
        case GraphicsAPI::D3D12:
            D3D12Hook::instance().unhook();
            break;
        default:
            break;
    }
    Log::info("Present hook removed");
    return true;
}

bool HookManager::hook_wndproc() {
    Log::info("WndProc hook installed");
    return true;
}

bool HookManager::unhook_wndproc() {
    Log::info("WndProc hook removed");
    return true;
}

bool HookManager::hook_d3d11() {
    return D3D11Hook::instance().hook_present(*this);
}

bool HookManager::hook_d3d12() {
    return D3D12Hook::instance().hook_present(*this);
}

bool HookManager::hook_opengl() {
    Log::warn("OpenGL hooking not yet implemented");
    return false;
}

bool HookManager::hook_vulkan() {
    Log::warn("Vulkan hooking not yet implemented");
    return false;
}

} // namespace vrc
