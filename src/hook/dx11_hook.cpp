#include "dx11_hook.h"
#include "dx12_hook.h"
#include "hook_manager.h"
#include "core/logging.h"
#include <MinHook.h>
#include <chrono>
#include <cmath>
#include <vector>

namespace vrc {

namespace {

constexpr u32 kDrainTimeoutMs = 2000;
// Depth views cached at once. Past this, new views are ignored until the next
// Present flushes the cache.
constexpr size_t kMaxCachedDsvs = 128;
// Presents between cache flushes, so views the game destroyed are released.
constexpr u32 kDsvCacheFlushInterval = 600;
// Depth must match the backbuffer within this fraction in width, height and
// aspect ratio; this rejects shadow maps, probe targets and UI buffers.
constexpr f32 kDepthSizeTolerance = 0.15f;

// The Present body runs on the game's render thread; a runtime compositor can
// call Present again from inside it. Per thread, so a second swap chain on
// another thread is still processed.
thread_local bool tl_in_pipeline = false;
// OMSetRenderTargets state, per thread. tl_last_dsv is compared only, never
// dereferenced; tl_depth_frame resets it every Present.
thread_local bool tl_in_om_detour = false;
thread_local ID3D11DepthStencilView* tl_last_dsv = nullptr;
thread_local u64 tl_depth_frame = 0;

struct ThreadFlag {
    explicit ThreadFlag(bool& flag) : flag_(flag) { flag_ = true; }
    ~ThreadFlag() { flag_ = false; }
    bool& flag_;
};

// The trampoline is published before the hook is enabled because the detour
// can run on another thread as soon as MH_EnableHook returns.
template <typename Fn>
bool create_and_enable_hook(void* target, void* detour, Fn& original_out, const char* name) {
    Fn original = nullptr;
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK || !original) {
        Log::warn("MH_CreateHook failed for {}", name);
        return false;
    }
    original_out = original;
    if (MH_EnableHook(target) != MH_OK) {
        Log::warn("MH_EnableHook failed for {}", name);
        MH_RemoveHook(target);  // never enabled, so no thread can be inside it
        original_out = nullptr;
        return false;
    }
    return true;
}

void warn_if_slow(std::chrono::steady_clock::time_point start, const char* what) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    static std::atomic<u32> slow_count{0};
    if (ms > 100 && slow_count.fetch_add(1, std::memory_order_relaxed) < 10) {
        Log::warn("original {} took {} ms", what, ms);
    }
}

bool matches_backbuffer(const D3D11_TEXTURE2D_DESC& desc, u32 bb_width, u32 bb_height) {
    if (!desc.Width || !desc.Height) return false;
    const f32 bb_aspect = static_cast<f32>(bb_width) / static_cast<f32>(bb_height);
    const f32 tex_aspect = static_cast<f32>(desc.Width) / static_cast<f32>(desc.Height);
    return std::fabs(static_cast<f32>(desc.Width) - bb_width) <= bb_width * kDepthSizeTolerance &&
           std::fabs(static_cast<f32>(desc.Height) - bb_height) <= bb_height * kDepthSizeTolerance &&
           std::fabs(tex_aspect - bb_aspect) <= bb_aspect * kDepthSizeTolerance;
}

// Creates a 1x1 swap chain for reading the DXGI vtables: flip-model first,
// legacy DISCARD if the factory or OS does not support flip-model.
IDXGISwapChain* create_probe_swap_chain(ID3D11Device* device, HWND hwnd) {
    IDXGIDevice* dxgi_device = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) return nullptr;
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (FAILED(hr)) return nullptr;
    IDXGIFactory* factory = nullptr;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    adapter->Release();
    if (FAILED(hr)) return nullptr;

    IDXGISwapChain* swap_chain = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = 1;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &sc1))) {
            swap_chain = sc1;
        }
        factory2->Release();
    }
    if (!swap_chain) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = 1;
        desc.BufferDesc.Height = 1;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(factory->CreateSwapChain(device, &desc, &swap_chain))) swap_chain = nullptr;
    }
    factory->Release();
    return swap_chain;
}

} // namespace

D3D11Hook& D3D11Hook::instance() {
    static D3D11Hook hook;
    return hook;
}

bool D3D11Hook::hook_present(HookManager& mgr) {
    std::lock_guard<std::mutex> lock(install_mutex_);
    hook_manager_ = &mgr;
    if (!ensure_minhook_initialized()) return false;

    if (present_func_ || present1_func_) {
        // Left disabled by an unhook() whose detours did not drain.
        if (present_func_) MH_EnableHook(present_func_);
        if (present1_func_) MH_EnableHook(present1_func_);
        if (om_set_render_targets_func_) MH_EnableHook(om_set_render_targets_func_);
        Log::info("DXGI Present hooks re-enabled");
        return true;
    }
    return install_hooks_via_probe();
}

bool D3D11Hook::install_hooks_via_probe() {
    // Injection is late, so the game's swap chain already exists. The DXGI and
    // D3D11 method addresses are shared by every object of the same class, so
    // a throwaway device, context and swap chain give us the game's addresses.
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"VRC_ProbeWindow";
    const bool registered = RegisterClassExW(&wc) != 0;
    if (!registered && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log::error("Could not register the probe window class: {}", GetLastError());
        return false;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_DISABLED,
                                0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        Log::error("Could not create the probe window: {}", GetLastError());
        if (registered) UnregisterClassW(wc.lpszClassName, inst);
        return false;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                         nullptr, 0, D3D11_SDK_VERSION,
                                         &device, nullptr, &context);
    if (SUCCEEDED(hr) && device) {
        swap_chain = create_probe_swap_chain(device, hwnd);
    } else {
        Log::error("Could not create the probe D3D11 device: {:08x}", static_cast<u32>(hr));
    }

    if (swap_chain) {
        void* present = (*reinterpret_cast<void***>(swap_chain))[8];
        void* present1 = nullptr;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            present1 = (*reinterpret_cast<void***>(sc1))[22];
            sc1->Release();
        }

        if (present && create_and_enable_hook(present, reinterpret_cast<void*>(&present_detour),
                                              original_present_, "IDXGISwapChain::Present")) {
            present_func_ = present;
        }
        if (present1 && present1 != present &&
            create_and_enable_hook(present1, reinterpret_cast<void*>(&present1_detour),
                                   original_present1_, "IDXGISwapChain1::Present1")) {
            present1_func_ = present1;
        }
        Log::info("DXGI hooks: Present {:p} ({}), Present1 {:p} ({})",
                  present, present_func_ ? "hooked" : "failed",
                  present1, present1_func_ ? "hooked" : "failed");
    } else if (device) {
        Log::error("Could not create a probe swap chain; Present cannot be hooked");
    }

    // Depth capture is optional: without it Depth3D falls back to Screen mode.
    // ID3D11DeviceContext vtable slot 33 is OMSetRenderTargets.
    if ((present_func_ || present1_func_) && context) {
        void* om = (*reinterpret_cast<void***>(context))[33];
        if (om && create_and_enable_hook(om, reinterpret_cast<void*>(&omset_render_targets_detour),
                                         original_om_set_render_targets_,
                                         "ID3D11DeviceContext::OMSetRenderTargets")) {
            om_set_render_targets_func_ = om;
        }
    }

    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    if (registered) UnregisterClassW(wc.lpszClassName, inst);

    return present_func_ != nullptr || present1_func_ != nullptr;
}

void D3D11Hook::unhook() {
    std::lock_guard<std::mutex> lock(install_mutex_);
    void* targets[] = { present_func_, present1_func_, om_set_render_targets_func_ };
    for (void* target : targets) {
        if (target) MH_DisableHook(target);
    }

    if (wait_for_detours_to_drain(in_flight_, kDrainTimeoutMs)) {
        for (void* target : targets) {
            if (target) MH_RemoveHook(target);
        }
        present_func_ = nullptr;
        original_present_ = nullptr;
        present1_func_ = nullptr;
        original_present1_ = nullptr;
        om_set_render_targets_func_ = nullptr;
        original_om_set_render_targets_ = nullptr;
    } else {
        // A thread is still inside a detour (e.g. a Present blocked on vsync)
        // and will return through its trampoline, so keep the hooks allocated
        // and disabled. hook_present() re-enables them on the next start.
        Log::warn("DXGI detours still running after {} ms; hooks left disabled", kDrainTimeoutMs);
    }

    release_depth_state();
    logged_waiting_for_queue_ = false;
    Log::info("DXGI Present hooks removed");
}

// ─── Present ────────────────────────────────────────────────────────────────

HRESULT __stdcall D3D11Hook::present_detour(
    IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags)
{
    auto& self = instance();
    InFlightGuard guard(self.in_flight_);
    const PresentFn original = self.original_present_;

    static std::atomic<bool> logged_first{false};
    if (!logged_first.load(std::memory_order_relaxed) && !logged_first.exchange(true))
        Log::info("First IDXGISwapChain::Present intercepted");

    if (swap_chain) self.on_present(swap_chain);

    const auto start = std::chrono::steady_clock::now();
    const HRESULT hr = original(swap_chain, sync_interval, flags);
    warn_if_slow(start, "Present");
    return hr;
}

HRESULT __stdcall D3D11Hook::present1_detour(
    IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* params)
{
    auto& self = instance();
    InFlightGuard guard(self.in_flight_);
    const Present1Fn original = self.original_present1_;

    static std::atomic<bool> logged_first{false};
    if (!logged_first.load(std::memory_order_relaxed) && !logged_first.exchange(true))
        Log::info("First IDXGISwapChain1::Present1 intercepted");

    if (swap_chain) self.on_present(swap_chain);

    const auto start = std::chrono::steady_clock::now();
    const HRESULT hr = original(swap_chain, sync_interval, flags, params);
    warn_if_slow(start, "Present1");
    return hr;
}

void D3D11Hook::on_present(IDXGISwapChain* swap_chain) {
    if (tl_in_pipeline || !hook_manager_) return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swap_chain->GetDesc(&desc))) return;
    // Probe swap chains are 1x1 and never carry a game frame.
    if (desc.BufferDesc.Width <= 4 || desc.BufferDesc.Height <= 4) return;

    ThreadFlag in_pipeline(tl_in_pipeline);
    std::lock_guard<std::mutex> lock(pipeline_mutex_);

    // The swap chain's device is the source of truth for the API in use.
    ID3D12Device* d3d12_device = nullptr;
    if (SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&d3d12_device)))) {
        present_d3d12(swap_chain, d3d12_device, desc);
        d3d12_device->Release();
        return;
    }
    ID3D11Device* d3d11_device = nullptr;
    if (SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&d3d11_device)))) {
        present_d3d11(swap_chain, d3d11_device, desc);
        d3d11_device->Release();
    }
}

void D3D11Hook::present_d3d11(IDXGISwapChain* swap_chain, ID3D11Device* device,
                              const DXGI_SWAP_CHAIN_DESC& desc) {
    // The D3D11 runtime rotates flip-model buffer identities, so buffer 0 is
    // always the one being presented.
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
    D3D11_TEXTURE2D_DESC bb_desc = {};
    back_buffer->GetDesc(&bb_desc);

    select_frame_depth(device, bb_desc.Width, bb_desc.Height);

    HookContext ctx;
    ctx.api = GraphicsAPI::D3D11;
    ctx.device = device;
    ctx.swap_chain = swap_chain;
    ctx.width = bb_desc.Width;
    ctx.height = bb_desc.Height;
    ctx.is_fullscreen = !desc.Windowed;
    hook_manager_->fire_on_present(ctx);

    FrameCapture capture;
    capture.resource = back_buffer;
    capture.width = bb_desc.Width;
    capture.height = bb_desc.Height;
    fill_depth_capture(capture);
    hook_manager_->fire_on_frame(capture);

    back_buffer->Release();
}

void D3D11Hook::present_d3d12(IDXGISwapChain* swap_chain, ID3D12Device* device,
                              const DXGI_SWAP_CHAIN_DESC& desc) {
    // D3D11On12 must submit on the game's DIRECT queue, which is only known
    // once the game has called ExecuteCommandLists on it.
    ID3D12CommandQueue* queue = D3D12Hook::instance().command_queue_for(device);
    if (!queue) {
        if (!logged_waiting_for_queue_) {
            logged_waiting_for_queue_ = true;
            Log::info("D3D12 swap chain presenting; skipping frames until the game's DIRECT "
                      "command queue is seen");
        }
        return;
    }

    ID3D12Resource* back_buffer = nullptr;
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back_buffer));
        sc3->Release();
    }
    if (back_buffer) {
        HookContext ctx;
        ctx.api = GraphicsAPI::D3D12;
        ctx.device = device;
        ctx.swap_chain = swap_chain;
        ctx.command_queue = queue;
        ctx.width = desc.BufferDesc.Width;
        ctx.height = desc.BufferDesc.Height;
        ctx.is_fullscreen = !desc.Windowed;
        hook_manager_->fire_on_present(ctx);

        FrameCapture capture;
        capture.resource = back_buffer;
        capture.width = ctx.width;
        capture.height = ctx.height;
        hook_manager_->fire_on_frame(capture);

        back_buffer->Release();
    }
    queue->Release();
}

// ─── Depth capture ──────────────────────────────────────────────────────────

void __stdcall D3D11Hook::omset_render_targets_detour(
    ID3D11DeviceContext* context, UINT num_views,
    ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    auto& self = instance();
    InFlightGuard guard(self.in_flight_);
    self.original_om_set_render_targets_(context, num_views, rtvs, dsv);

    // This runs thousands of times per frame; keep the common path to a few
    // compares. Depth-only passes (num_views == 0) still count.
    if (!dsv || tl_in_om_detour) return;
    const u64 frame = self.depth_frame_.load(std::memory_order_relaxed);
    if (tl_depth_frame != frame) {
        tl_depth_frame = frame;
        tl_last_dsv = nullptr;
    }
    if (dsv == tl_last_dsv) return;
    tl_last_dsv = dsv;

    ThreadFlag in_detour(tl_in_om_detour);
    self.note_depth_bind(dsv);
}

void D3D11Hook::note_depth_bind(ID3D11DepthStencilView* dsv) {
    std::lock_guard<std::mutex> lock(depth_mutex_);
    // Nothing qualifies until a game Present has told us the device and size.
    if (!depth_device_) return;

    auto it = dsv_cache_.find(dsv);
    if (it == dsv_cache_.end()) {
        if (dsv_cache_.size() >= kMaxCachedDsvs) return;

        DsvEntry entry;
        ID3D11Device* view_device = nullptr;
        dsv->GetDevice(&view_device);
        // Views of other devices (our XR device, overlays) never qualify.
        const bool game_device = view_device == depth_device_;
        if (view_device) view_device->Release();

        ID3D11Resource* resource = nullptr;
        dsv->GetResource(&resource);
        ID3D11Texture2D* texture = nullptr;
        if (resource) {
            resource->QueryInterface(IID_PPV_ARGS(&texture));
            resource->Release();
        }
        if (texture) {
            texture->GetDesc(&entry.desc);
            entry.texture = texture;  // kept alive by the view we AddRef below
            texture->Release();
            // MSAA depth needs a resolve before it can be sampled, so skip it.
            entry.qualifies = game_device && entry.desc.SampleDesc.Count == 1 &&
                              matches_backbuffer(entry.desc, depth_bb_width_, depth_bb_height_);
        }
        dsv->AddRef();
        it = dsv_cache_.emplace(dsv, entry).first;
    }

    DsvEntry& entry = it->second;
    if (entry.qualifies && entry.binds++ == 0) entry.first_bind = bind_sequence_++;
}

void D3D11Hook::select_frame_depth(ID3D11Device* device, u32 bb_width, u32 bb_height) {
    {
        std::lock_guard<std::mutex> lock(depth_mutex_);
        select_frame_depth_locked(device, bb_width, bb_height);
    }
    release_pending();
}

void D3D11Hook::select_frame_depth_locked(ID3D11Device* device, u32 bb_width, u32 bb_height) {
    if (device != depth_device_ || bb_width != depth_bb_width_ || bb_height != depth_bb_height_) {
        // Every cached classification depends on the device and the size.
        flush_dsv_cache_locked();
        set_published_depth_locked(nullptr, nullptr);
        if (depth_device_) pending_releases_.push_back(depth_device_);
        depth_device_ = device;
        depth_device_->AddRef();
        depth_bb_width_ = bb_width;
        depth_bb_height_ = bb_height;
    } else {
        // Publish the qualifying texture bound most often this frame; ties go
        // to the larger texture, then to the one bound first. Several views
        // can share one texture (e.g. read-only depth views), so sum per texture.
        struct Tally { const DsvEntry* entry; u32 binds; u32 first_bind; };
        std::vector<Tally> tallies;
        for (const auto& [view, entry] : dsv_cache_) {
            if (!entry.qualifies || !entry.binds) continue;
            bool merged = false;
            for (auto& t : tallies) {
                if (t.entry->texture == entry.texture) {
                    t.binds += entry.binds;
                    if (entry.first_bind < t.first_bind) t.first_bind = entry.first_bind;
                    merged = true;
                    break;
                }
            }
            if (!merged) tallies.push_back({ &entry, entry.binds, entry.first_bind });
        }

        const Tally* best = nullptr;
        for (const auto& t : tallies) {
            if (!best || t.binds > best->binds) { best = &t; continue; }
            if (t.binds < best->binds) continue;
            const u64 area = static_cast<u64>(t.entry->desc.Width) * t.entry->desc.Height;
            const u64 best_area = static_cast<u64>(best->entry->desc.Width) * best->entry->desc.Height;
            if (area > best_area || (area == best_area && t.first_bind < best->first_bind)) best = &t;
        }
        set_published_depth_locked(best ? best->entry->texture : nullptr,
                                   best ? &best->entry->desc : nullptr);

        for (auto& [view, entry] : dsv_cache_) entry.binds = 0;
        if (++presents_since_flush_ >= kDsvCacheFlushInterval ||
            dsv_cache_.size() >= kMaxCachedDsvs) {
            flush_dsv_cache_locked();
        }
    }

    bind_sequence_ = 0;
    depth_frame_.fetch_add(1, std::memory_order_relaxed);
}

void D3D11Hook::set_published_depth_locked(ID3D11Texture2D* texture,
                                           const D3D11_TEXTURE2D_DESC* desc) {
    if (texture != published_depth_) {
        if (texture) texture->AddRef();
        if (published_depth_) pending_releases_.push_back(published_depth_);
        published_depth_ = texture;

        static int logged = 0;
        if (texture && logged < 4) {
            ++logged;
            Log::info("Game depth buffer selected: {}x{} format {}",
                      desc->Width, desc->Height, static_cast<u32>(desc->Format));
        }
    }
    published_depth_desc_ = desc ? *desc : D3D11_TEXTURE2D_DESC{};
}

void D3D11Hook::flush_dsv_cache_locked() {
    for (auto& [view, entry] : dsv_cache_) pending_releases_.push_back(view);
    dsv_cache_.clear();
    presents_since_flush_ = 0;
}

void D3D11Hook::release_pending() {
    std::vector<IUnknown*> releases;
    {
        std::lock_guard<std::mutex> lock(depth_mutex_);
        releases.swap(pending_releases_);
    }
    for (IUnknown* object : releases) object->Release();
}

void D3D11Hook::fill_depth_capture(FrameCapture& capture) {
    std::lock_guard<std::mutex> lock(depth_mutex_);
    if (!published_depth_) return;
    // The consumer releases this reference exactly once.
    published_depth_->AddRef();
    capture.depth_texture = published_depth_;
    capture.depth_width = published_depth_desc_.Width;
    capture.depth_height = published_depth_desc_.Height;
    capture.depth_format = static_cast<u32>(published_depth_desc_.Format);
    capture.depth_sample_count = published_depth_desc_.SampleDesc.Count;
}

void D3D11Hook::release_depth_state() {
    {
        std::lock_guard<std::mutex> lock(depth_mutex_);
        flush_dsv_cache_locked();
        set_published_depth_locked(nullptr, nullptr);
        if (depth_device_) pending_releases_.push_back(depth_device_);
        depth_device_ = nullptr;
        depth_bb_width_ = 0;
        depth_bb_height_ = 0;
        bind_sequence_ = 0;
    }
    release_pending();
}

} // namespace vrc
