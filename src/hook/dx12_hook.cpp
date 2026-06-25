#include "dx12_hook.h"
#include "hook_manager.h"
#include "core/logging.h"
#include <MinHook.h>
#include <thread>

namespace vrc {

D3D12Hook& D3D12Hook::instance() {
    static D3D12Hook hook;
    return hook;
}

bool D3D12Hook::try_hook_create_device() {
    HMODULE d3d12_mod = GetModuleHandleA("d3d12.dll");
    if (!d3d12_mod) return false;

    auto addr = GetProcAddress(d3d12_mod, "D3D12CreateDevice");
    if (!addr) {
        Log::error("D3D12CreateDevice not found in d3d12.dll");
        return false;
    }

    if (MH_CreateHook(addr, &create_device_detour,
                      reinterpret_cast<void**>(&original_create_device_)) != MH_OK) {
        Log::error("MinHook CreateHook failed for D3D12CreateDevice");
        return false;
    }

    if (MH_EnableHook(addr) != MH_OK) {
        Log::error("MinHook EnableHook failed for D3D12CreateDevice");
        return false;
    }

    Log::info("D3D12CreateDevice hook installed");
    return true;
}

DWORD WINAPI deferred_d3d12_init(LPVOID) {
    for (int i = 0; i < 300; i++) {
        if (D3D12Hook::instance().try_hook_create_device()) {
            return 0;
        }
        Sleep(1000);
    }
    Log::warn("D3D12 hook deferred: d3d12.dll not loaded within 5 minutes");
    return 0;
}

bool D3D12Hook::install_ecl_hook(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    ID3D12CommandQueue* temp_queue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&temp_queue));
    if (FAILED(hr) || !temp_queue) {
        Log::error("Failed to create temp queue for ECL vtable");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(temp_queue);
    void* ecl_func = vtable[8];

    if (MH_CreateHook(ecl_func, &execute_command_lists_detour,
                      reinterpret_cast<void**>(&original_execute_command_lists_)) != MH_OK) {
        Log::error("MinHook CreateHook failed for ExecuteCommandLists");
        temp_queue->Release();
        return false;
    }

    if (MH_EnableHook(ecl_func) != MH_OK) {
        Log::error("MinHook EnableHook failed for ExecuteCommandLists");
        temp_queue->Release();
        return false;
    }

    execute_func_ = ecl_func;
    temp_queue->Release();

    Log::info("D3D12 ExecuteCommandLists hook installed");
    return true;
}

bool D3D12Hook::hook_present(HookManager& mgr) {
    if (hooked_) {
        Log::warn("D3D12 hook already installed");
        return true;
    }

    hook_manager_ = &mgr;

    // Try to hook D3D12CreateDevice immediately if d3d12.dll is already loaded
    if (try_hook_create_device()) {
        // D3D12CreateDevice hook is installed, but the game may have already
        // created its device.  Try to install the ECL hook immediately via a
        // temporary device — the vtable address is shared across all instances.
        skip_next_create_ = true;
        ID3D12Device* temp_device = nullptr;
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                       IID_PPV_ARGS(&temp_device));
        if (SUCCEEDED(hr) && temp_device) {
            if (install_ecl_hook(temp_device)) {
                // ECL hook is live — it will capture the game's queue + device
                // on the next ExecuteCommandLists call.
                Log::info("D3D12 ECL hook pre-installed via temp device (late-injection path)");
            }
            temp_device->Release();
        }

        hooked_ = true;
        Log::info("D3D12 hook installed (CreateDevice + ECL)");
        return true;
    }

    // Otherwise start a background thread to wait for d3d12.dll to load
    HANDLE t = CreateThread(nullptr, 0, deferred_d3d12_init, nullptr, 0, nullptr);
    if (t) CloseHandle(t);

    // Mark as hooked so we don't try again, even though the hook isn't live yet
    hooked_ = true;
    Log::info("D3D12 hook deferred — d3d12.dll not loaded, waiting...");
    return true;
}

void D3D12Hook::unhook() {
    if (!hooked_) return;
    if (execute_func_) {
        MH_DisableHook(execute_func_);
        MH_RemoveHook(execute_func_);
    }
    if (original_create_device_) {
        HMODULE d3d12_mod = GetModuleHandleA("d3d12.dll");
        if (d3d12_mod) {
            auto addr = GetProcAddress(d3d12_mod, "D3D12CreateDevice");
            if (addr) {
                MH_DisableHook(addr);
                MH_RemoveHook(addr);
            }
        }
    }
    if (device_) { device_->Release(); device_ = nullptr; }
    captured_queue_ = nullptr;
    hooked_ = false;
    Log::info("D3D12 hook removed");
}

HRESULT WINAPI D3D12Hook::create_device_detour(
    IUnknown* adapter, D3D_FEATURE_LEVEL min_level,
    REFIID riid, void** pp_device)
{
    auto& self = instance();

    HRESULT hr = self.original_create_device_(adapter, min_level, riid, pp_device);
    if (FAILED(hr) || !pp_device || !*pp_device) return hr;

    // Skip capturing if this is our own temp device for ECL hook installation
    if (self.skip_next_create_) {
        self.skip_next_create_ = false;
        return hr;
    }

    ID3D12Device* game_device = static_cast<ID3D12Device*>(*pp_device);

    self.device_ = game_device;
    game_device->AddRef();

    Log::info("D3D12CreateDevice hook captured game device");

    if (!self.install_ecl_hook(game_device)) {
        Log::error("Failed to install ECL hook on game device");
    }

    return hr;
}

void __stdcall D3D12Hook::execute_command_lists_detour(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList** lists)
{
    auto& self = instance();
    self.captured_queue_ = queue;

    // If CreateDevice hook didn't fire (late injection), get device from queue
    if (!self.device_) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
            self.device_ = dev;
            Log::info("D3D12 ECL detour: captured device from command queue");
        }
    }

    if (self.hook_manager_ && !self.pipeline_initialized_ && self.device_) {
        self.pipeline_initialized_ = true;
        Log::info("D3D12 ECL detour: initializing VR pipeline");

        auto& ctx = self.hook_manager_->context();
        ctx.api = GraphicsAPI::D3D12;
        ctx.device = self.device_;
        ctx.command_queue = queue;
        ctx.swap_chain = nullptr;
        ctx.width = 0;
        ctx.height = 0;

        self.hook_manager_->fire_on_present(ctx);
    }

    self.original_execute_command_lists_(queue, count, lists);
}

} // namespace vrc
