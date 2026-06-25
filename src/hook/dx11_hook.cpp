#include "dx11_hook.h"
#include "dx12_hook.h"
#include "hook_manager.h"
#include "core/logging.h"
#include <MinHook.h>

namespace vrc {

D3D11Hook& D3D11Hook::instance() {
    static D3D11Hook hook;
    return hook;
}

// Helper: create a temp D3D11 device and return its DXGI factory
static IDXGIFactory2* get_dxgi_factory() {
    ID3D11Device* dev = nullptr;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (FAILED(hr) || !dev) return nullptr;

    IDXGIDevice* dxgi_dev = nullptr;
    hr = dev->QueryInterface(&dxgi_dev);
    if (FAILED(hr) || !dxgi_dev) { dev->Release(); return nullptr; }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgi_dev->GetAdapter(&adapter);
    dxgi_dev->Release();
    if (FAILED(hr) || !adapter) { dev->Release(); return nullptr; }

    IDXGIFactory2* factory = nullptr;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    adapter->Release();
    dev->Release();
    return factory;
}

bool D3D11Hook::hook_present_globally() {
    if (present_func_) return true;

    HINSTANCE inst = GetModuleHandleA(nullptr);
    if (!inst) return false;

    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.lpszClassName = "VRC_TempHook_Window";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    HWND hwnd = CreateWindowExA(0, "VRC_TempHook_Window", "", WS_DISABLED,
                                0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return false;

    // Get an IDXGIFactory2 for creating FLIP-mode swap chains
    IDXGIFactory2* factory = get_dxgi_factory();
    if (!factory) {
        Log::warn("Failed to get DXGI factory for temp swap chain");
        DestroyWindow(hwnd);
        return false;
    }

    // Try all swap effects and log each address
    struct SwapEffectTest {
        const char* name;
        DXGI_SWAP_EFFECT effect;
        bool flip;
    } tests[] = {
        { "FLIP_DISCARD",    DXGI_SWAP_EFFECT_FLIP_DISCARD,    true },
        { "FLIP_SEQUENTIAL", DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, true },
        { "SEQUENTIAL",      DXGI_SWAP_EFFECT_SEQUENTIAL,      false },
        { "DISCARD",         DXGI_SWAP_EFFECT_DISCARD,         false },
    };

    int count_installed = 0;

    for (auto& t : tests) {
        void* pf = nullptr;

        if (t.flip) {
            DXGI_SWAP_CHAIN_DESC1 scd1 = {};
            scd1.Width = 1;
            scd1.Height = 1;
            scd1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd1.Stereo = FALSE;
            scd1.SampleDesc.Count = 1;
            scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd1.BufferCount = 2;
            scd1.Scaling = DXGI_SCALING_STRETCH;
            scd1.SwapEffect = t.effect;
            scd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            scd1.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

            ID3D11Device* d = nullptr;
            ID3D11DeviceContext* c = nullptr;
            IDXGISwapChain1* sc1 = nullptr;

            if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)) &&
                d && SUCCEEDED(factory->CreateSwapChainForHwnd(d, hwnd, &scd1, nullptr, nullptr, &sc1)) &&
                sc1) {
                IDXGISwapChain* sc = nullptr;
                if (SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&sc))) && sc) {
                    pf = (*reinterpret_cast<void***>(sc))[8];
                    sc->Release();
                }
                sc1->Release();
            }
            if (d) d->Release();
            if (c) c->Release();
        } else {
            ID3D11Device* d = nullptr;
            ID3D11DeviceContext* c = nullptr;
            IDXGISwapChain* sc = nullptr;
            DXGI_SWAP_CHAIN_DESC scd = {};
            scd.BufferCount = 1;
            scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd.BufferDesc.Width = 1;
            scd.BufferDesc.Height = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.OutputWindow = hwnd;
            scd.SampleDesc.Count = 1;
            scd.Windowed = TRUE;
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                         nullptr, 0, D3D11_SDK_VERSION, &scd,
                                                         &sc, &d, nullptr, &c)) && sc) {
                pf = (*reinterpret_cast<void***>(sc))[8];
                sc->Release();
            }
            if (d) d->Release();
            if (c) c->Release();
        }

        Log::info("Present vtable[8] {} = {:p}", t.name, pf);

        // Skip if already hooked (duplicate address across swap effects)
        bool already = (pf && pf == present_func_);
        if (!already && pf) {
            for (auto& [a, o] : extra_present_hooks_) {
                if (a == pf) { already = true; break; }
            }
        }

        if (pf && !already) {
            PresentFn new_orig = nullptr;
            Log::info("MH_CreateHook: pf={:p}, detour_addr={:p}", pf, (void*)(uintptr_t)&present_detour);
            if (MH_CreateHook(pf, &present_detour, reinterpret_cast<void**>(&new_orig)) == MH_OK && new_orig) {
                // Set originals BEFORE EnableHook to avoid race: game thread may
                // call Present immediately after enable, and present_detour needs
                // present_func_/original_present_ to be valid.
                if (!present_func_) {
                    present_func_ = pf;
                    original_present_ = new_orig;
                } else {
                    extra_present_hooks_.push_back({pf, new_orig});
                }
                Log::info("new_orig (trampoline) = {:p}", (void*)new_orig);
                // Dump trampoline bytes to verify relay structure
                unsigned char tb[32] = {0};
                memcpy(tb, new_orig, 32);
                Log::info("trampoline[0:20] = {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                    tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6], tb[7], tb[8], tb[9],
                    tb[10], tb[11], tb[12], tb[13], tb[14], tb[15], tb[16], tb[17], tb[18], tb[19]);
                Log::info("trampoline[20:32] = {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                    tb[20], tb[21], tb[22], tb[23], tb[24], tb[25], tb[26], tb[27], tb[28], tb[29], tb[30], tb[31]);
                if (MH_EnableHook(pf) == MH_OK) {
                    count_installed++;
                    Log::info("D3D11 Present hook installed for {}", t.name);
                    // Read full JMP instruction to verify redirect target
                    unsigned char b[8] = {0};
                    memcpy(b, pf, 8);
                    if (b[0] == 0xE9) {
                        int32_t rel = *(int32_t*)&b[1];
                        void* target = (char*)pf + 5 + rel;
                        void* detour_addr = (void*)(uintptr_t)present_detour;
                        Log::info("Present JMP target = {:p}, present_detour = {:p}",
                                  target, detour_addr);
                    }
                }
            }
        }
    }

    // Also check Present1 (vtable[22] on IDXGISwapChain1)
    // Some games call Present1 instead of Present
    ID3D11Device* check_d = nullptr;
    ID3D11DeviceContext* check_c = nullptr;
    IDXGISwapChain1* check_sc1 = nullptr;
    DXGI_SWAP_CHAIN_DESC1 check_scd1 = {};
    check_scd1.Width = 1;
    check_scd1.Height = 1;
    check_scd1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    check_scd1.Stereo = FALSE;
    check_scd1.SampleDesc.Count = 1;
    check_scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    check_scd1.BufferCount = 2;
    check_scd1.Scaling = DXGI_SCALING_STRETCH;
    check_scd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    check_scd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    check_scd1.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                     nullptr, 0, D3D11_SDK_VERSION, &check_d, nullptr, &check_c)) &&
        check_d && SUCCEEDED(factory->CreateSwapChainForHwnd(check_d, hwnd, &check_scd1, nullptr, nullptr, &check_sc1)) &&
        check_sc1) {
        void** check_vtbl = *reinterpret_cast<void***>(check_sc1);
        void* present1_addr = check_vtbl[22];  // IDXGISwapChain1::Present1
        Log::info("Present1 vtable[22] = {:p} (Present = {:p})", present1_addr, (void*)present_func_);
        if (present1_addr && present1_addr != present_func_) {
            bool already = false;
            for (auto& [a, o] : extra_present_hooks_) {
                if (a == present1_addr) { already = true; break; }
            }
            if (!already) {
                PresentFn new_orig = nullptr;
                if (MH_CreateHook(present1_addr, &present_detour, reinterpret_cast<void**>(&new_orig)) == MH_OK && new_orig) {
                    extra_present_hooks_.push_back({present1_addr, new_orig});
                    if (MH_EnableHook(present1_addr) == MH_OK) {
                        count_installed++;
                        Log::info("D3D11 Present1 (vtable[22]) hook installed");
                    }
                }
            }
        }
        check_sc1->Release();
    }
    if (check_d) check_d->Release();
    if (check_c) check_c->Release();

    factory->Release();
    DestroyWindow(hwnd);

    if (count_installed > 0) {
        Log::info("D3D11 Present hook installed globally via temp swap chain ({} effects)", count_installed);
        // Verify patch after a short delay
        if (present_func_) {
            unsigned char v[2] = {0};
            memcpy(v, present_func_, 2);
            Log::info("Present first bytes AFTER install: {:02x} {:02x}", v[0], v[1]);
        }

    } else {
        Log::warn("Failed to hook Present globally");
    }
    return count_installed > 0;
}

bool D3D11Hook::hook_present(HookManager& mgr) {
    if (hooked_) {
        Log::warn("D3D11 hook already installed");
        return true;
    }

    hook_manager_ = &mgr;

    HMODULE d3d11_mod = GetModuleHandleA("d3d11.dll");
    if (!d3d11_mod) return false;

    MH_STATUS init_status = MH_Initialize();
    if (init_status != MH_OK && init_status != MH_ERROR_ALREADY_INITIALIZED) {
        Log::error("MinHook initialization failed: {}", static_cast<int>(init_status));
        return false;
    }

    // Hook D3D11CreateDevice
    auto create_dev = GetProcAddress(d3d11_mod, "D3D11CreateDevice");
    if (create_dev) {
        if (MH_CreateHook(create_dev, &create_device_detour,
                          reinterpret_cast<void**>(&original_create_device_)) != MH_OK) {
            Log::warn("MH_CreateHook failed for D3D11CreateDevice");
        } else if (MH_EnableHook(create_dev) != MH_OK) {
            Log::warn("MH_EnableHook failed for D3D11CreateDevice");
        } else {
            Log::info("D3D11CreateDevice hook installed");
        }
    }

    // Hook D3D11CreateDeviceAndSwapChain
    auto create_dev_sc = GetProcAddress(d3d11_mod, "D3D11CreateDeviceAndSwapChain");
    if (create_dev_sc) {
        if (MH_CreateHook(create_dev_sc, &create_device_and_swapchain_detour,
                          reinterpret_cast<void**>(&original_create_device_and_swapchain_)) != MH_OK) {
            Log::warn("MH_CreateHook failed for D3D11CreateDeviceAndSwapChain");
        } else if (MH_EnableHook(create_dev_sc) != MH_OK) {
            Log::warn("MH_EnableHook failed for D3D11CreateDeviceAndSwapChain");
        } else {
            Log::info("D3D11CreateDeviceAndSwapChain hook installed");
        }
    }

    // Hook Present globally — works even if game already has a swap chain
    if (!hook_present_globally()) {
        Log::warn("No existing swap chain found — will hook on next creation");
    }

    // Log captured swap chain's vtable[8] for comparison
    if (captured_swap_chain_) {
        void* captured_vtbl8 = (*reinterpret_cast<void***>(captured_swap_chain_))[8];
        Log::info("VRC: captured swap chain vtable[8] = {:p} (hooked addr = {:p})",
                  captured_vtbl8, (void*)present_func_);
    }

    hooked_ = true;
    Log::info("D3D11 hooks installed");
    return true;
}

void D3D11Hook::unhook() {
    if (!hooked_) return;

    // Remove extra present hooks
    for (auto& [addr, orig] : extra_present_hooks_) {
        MH_DisableHook(addr);
        MH_RemoveHook(addr);
    }
    extra_present_hooks_.clear();

    if (present_func_) {
        MH_DisableHook(present_func_);
        MH_RemoveHook(present_func_);
        present_func_ = nullptr;
    }
    if (original_create_device_and_swapchain_) {
        HMODULE d3d11_mod = GetModuleHandleA("d3d11.dll");
        if (d3d11_mod) {
            auto addr = GetProcAddress(d3d11_mod, "D3D11CreateDeviceAndSwapChain");
            if (addr) { MH_DisableHook(addr); MH_RemoveHook(addr); }
        }
    }
    if (original_create_device_) {
        HMODULE d3d11_mod = GetModuleHandleA("d3d11.dll");
        if (d3d11_mod) {
            auto addr = GetProcAddress(d3d11_mod, "D3D11CreateDevice");
            if (addr) { MH_DisableHook(addr); MH_RemoveHook(addr); }
        }
    }
    hooked_ = false;
    Log::info("D3D11 hook removed");
}

void D3D11Hook::hook_swapchain_present(IDXGISwapChain* swap_chain) {
    __try {
        void** vtable = *reinterpret_cast<void***>(swap_chain);
        void* present_func = vtable[8];
        Log::info("VRC: hook_swapchain_present called — vtable[8]={:p}", present_func);

        if (present_func_) {
            if (present_func != present_func_) {
                Log::warn("VRC: swap chain vtable[8] DIFFERS from hooked address! hooked={:p}, actual={:p}",
                          present_func_, present_func);
            }
            return;
        }

        PresentFn new_orig = nullptr;
        if (MH_CreateHook(present_func, &present_detour,
                          reinterpret_cast<void**>(&new_orig)) != MH_OK || !new_orig) {
            Log::warn("MH_CreateHook failed for Present on real swap chain");
            return;
        }

        // Set originals BEFORE enable to avoid race
        present_func_ = present_func;
        original_present_ = new_orig;

        if (MH_EnableHook(present_func) != MH_OK) {
            Log::warn("MH_EnableHook failed for Present on real swap chain");
            present_func_ = nullptr;
            original_present_ = nullptr;
            return;
        }

        Log::info("D3D11 Present hook installed on real game swap chain");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::error("Exception in hook_swapchain_present: code={:08x}", GetExceptionCode());
    }
}

// ─── Hooks ──────────────────────────────────────────────────────────────────

HRESULT WINAPI D3D11Hook::create_device_and_swapchain_detour(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE module,
    UINT flags, const D3D_FEATURE_LEVEL* feature_levels, UINT num_levels,
    UINT sdk_version, const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
    IDXGISwapChain** swap_chain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* obtained_level, ID3D11DeviceContext** context)
{
    auto& self = instance();

    HRESULT hr = self.original_create_device_and_swapchain_(
        adapter, driver_type, module, flags,
        feature_levels, num_levels, sdk_version,
        swap_chain_desc, swap_chain, device,
        obtained_level, context);

    if (FAILED(hr)) return hr;

    if (device && *device) {
        if (self.device_) self.device_->Release();
        self.device_ = *device;
        self.device_->AddRef();
    }
    if (context && *context) {
        if (self.context_) self.context_->Release();
        self.context_ = *context;
        self.context_->AddRef();
    }
    if (swap_chain && *swap_chain) {
        if (self.captured_swap_chain_) self.captured_swap_chain_->Release();
        self.captured_swap_chain_ = *swap_chain;
        self.captured_swap_chain_->AddRef();
        self.hook_swapchain_present(*swap_chain);
    }

    OutputDebugStringA("VRC: D3D11CreateDeviceAndSwapChain hook captured game device + swap chain\n");
    Log::info("D3D11CreateDeviceAndSwapChain hook captured game device + swap chain");
    return hr;
}

HRESULT WINAPI D3D11Hook::create_device_detour(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE module,
    UINT flags, const D3D_FEATURE_LEVEL* feature_levels, UINT num_levels,
    UINT sdk_version, ID3D11Device** device,
    D3D_FEATURE_LEVEL* obtained_level, ID3D11DeviceContext** context)
{
    auto& self = instance();

    HRESULT hr = self.original_create_device_(
        adapter, driver_type, module, flags,
        feature_levels, num_levels, sdk_version,
        device, obtained_level, context);

    if (FAILED(hr)) return hr;

    if (device && *device) {
        if (self.device_) self.device_->Release();
        self.device_ = *device;
        self.device_->AddRef();
    }
    if (context && *context) {
        if (self.context_) self.context_->Release();
        self.context_ = *context;
        self.context_->AddRef();
    }

    OutputDebugStringA("VRC: D3D11CreateDevice hook captured game device\n");
    Log::info("D3D11CreateDevice hook captured game device");
    return hr;
}

HRESULT __stdcall D3D11Hook::present_detour(
    IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags)
{
    // Guard: skip processing if swap_chain is invalid
    if (!swap_chain) {
        OutputDebugStringA("VRC: present_detour SKIP (null swap_chain)\n");
        // Can't call original without swap_chain — just return S_OK
        // (the real system Present would also crash with null)
        return S_OK;
    }

    auto& self = instance();
    if (!self.hook_manager_) {
        OutputDebugStringA("VRC: present_detour SKIP (null hook_manager_)\n");
        PresentFn orig = self.original_present_;
        return orig ? orig(swap_chain, sync_interval, flags) : S_OK;
    }

    auto& mgr = *self.hook_manager_;

    // Find the correct original for THIS swap chain's Present address
    void* sc_present = nullptr;
    __try {
        sc_present = (*reinterpret_cast<void***>(swap_chain))[8];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("VRC: present_detour SKIP (AV reading vtable)\n");
        PresentFn orig = self.original_present_;
        return orig ? orig(swap_chain, sync_interval, flags) : S_OK;
    }

    PresentFn original = self.original_present_;
    if (sc_present != self.present_func_) {
        for (auto& [addr, orig] : self.extra_present_hooks_) {
            if (addr == sc_present) { original = orig; break; }
        }
    }

    static bool first = true;
    if (first) {
        first = false;
        Log::info("VRC: present_detour entered (first call) — vtable[8]={:p}, our_func={:p}",
                  sc_present, self.present_func_);
        OutputDebugStringA("VRC: present_detour first call\n");
        char buf[256];
        sprintf_s(buf, "VRC: vtable[8]=%p our_func=%p\n", sc_present, self.present_func_);
        OutputDebugStringA(buf);
    }

    __try {
        auto& ctx = mgr.context();
        ctx.swap_chain = swap_chain;

        // Try D3D12 first — some swap chains are D3D12
        ID3D12Device* d3d12_dev = nullptr;
        if (SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&d3d12_dev)))) {
            ctx.api = GraphicsAPI::D3D12;
            ctx.device = d3d12_dev;
            ctx.command_queue = D3D12Hook::instance().command_queue();

            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                u32 bb_idx = sc3->GetCurrentBackBufferIndex();
                ID3D12Resource* bb = nullptr;
                if (SUCCEEDED(swap_chain->GetBuffer(bb_idx, IID_PPV_ARGS(&bb)))) {
                    DXGI_SWAP_CHAIN_DESC desc;
                    swap_chain->GetDesc(&desc);
                    ctx.width = desc.BufferDesc.Width;
                    ctx.height = desc.BufferDesc.Height;

                    mgr.fire_on_present(ctx);

                    FrameCapture capture{};
                    capture.resource = bb;
                    capture.width = ctx.width;
                    capture.height = ctx.height;
                    mgr.fire_on_frame(capture);

                    bb->Release();
                }
                sc3->Release();
            }
            d3d12_dev->Release();
        } else {
            // D3D11 — get device from the swap chain itself
            ID3D11Device* d3d11_dev = nullptr;
            if (SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&d3d11_dev)))) {
                ctx.api = GraphicsAPI::D3D11;
                ctx.device = d3d11_dev;

                ID3D11Texture2D* back_buffer = nullptr;
                if (SUCCEEDED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
                    D3D11_TEXTURE2D_DESC desc;
                    back_buffer->GetDesc(&desc);
                    ctx.width = desc.Width;
                    ctx.height = desc.Height;

                    mgr.fire_on_present(ctx);

                    FrameCapture capture{};
                    capture.resource = back_buffer;
                    capture.width = ctx.width;
                    capture.height = ctx.height;
                    mgr.fire_on_frame(capture);

                    back_buffer->Release();
                }
                d3d11_dev->Release();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log::error("Exception in D3D11 Present detour: code={:08x}", GetExceptionCode());
    }

    return original ? original(swap_chain, sync_interval, flags) : S_OK;
}

} // namespace vrc
