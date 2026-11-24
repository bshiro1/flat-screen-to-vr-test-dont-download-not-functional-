#pragma once

#include "core/types.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vrc {

class HookManager;

// Hooks IDXGISwapChain::Present and IDXGISwapChain1::Present1, which every
// D3D11 and D3D12 swap chain goes through, and feeds each game frame into the
// HookManager callbacks. For D3D11 games it also hooks OMSetRenderTargets to
// find the game's main depth buffer.
class D3D11Hook {
public:
    static D3D11Hook& instance();

    bool hook_present(HookManager& mgr);
    void unhook();

    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    // Present1 has a fourth parameter, so it needs its own trampoline type.
    using Present1Fn = HRESULT(__stdcall*)(
        IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using OMSetRenderTargetsFn = void(__stdcall*)(
        ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
        ID3D11DepthStencilView*);

private:
    D3D11Hook() = default;

    static HRESULT __stdcall present_detour(IDXGISwapChain* swap_chain,
                                            UINT sync_interval, UINT flags);
    static HRESULT __stdcall present1_detour(IDXGISwapChain1* swap_chain,
                                             UINT sync_interval, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* params);
    static void __stdcall omset_render_targets_detour(
        ID3D11DeviceContext* context, UINT num_views,
        ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);

    // Present-thread body shared by both Present detours.
    void on_present(IDXGISwapChain* swap_chain);
    void present_d3d11(IDXGISwapChain* swap_chain, ID3D11Device* device,
                       const DXGI_SWAP_CHAIN_DESC& desc);
    void present_d3d12(IDXGISwapChain* swap_chain, ID3D12Device* device,
                       const DXGI_SWAP_CHAIN_DESC& desc);

    // install_mutex_ must be held.
    bool install_hooks_via_probe();

    // Depth capture. All of these take depth_mutex_.
    void note_depth_bind(ID3D11DepthStencilView* dsv);
    void select_frame_depth(ID3D11Device* device, u32 bb_width, u32 bb_height);
    void fill_depth_capture(FrameCapture& capture);
    void release_depth_state();
    // depth_mutex_ must be held. These queue releases in pending_releases_
    // instead of releasing under the lock.
    void select_frame_depth_locked(ID3D11Device* device, u32 bb_width, u32 bb_height);
    void flush_dsv_cache_locked();
    void set_published_depth_locked(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC* desc);
    // Releases queued objects outside depth_mutex_. A game thread can enter
    // the OMSetRenderTargets detour (and take depth_mutex_) while holding the
    // D3D11 multithread lock, so no D3D call that might take that lock may
    // run under depth_mutex_.
    void release_pending();

    HookManager* hook_manager_ = nullptr;
    std::mutex install_mutex_;

    void* present_func_ = nullptr;
    PresentFn original_present_ = nullptr;
    void* present1_func_ = nullptr;
    Present1Fn original_present1_ = nullptr;
    void* om_set_render_targets_func_ = nullptr;
    OMSetRenderTargetsFn original_om_set_render_targets_ = nullptr;
    std::atomic<i32> in_flight_{0};

    // Serializes the VR pipeline callbacks when two swap chains present on
    // different threads.
    std::mutex pipeline_mutex_;
    bool logged_waiting_for_queue_ = false;

    // Classification of a game depth-stencil view, cached by view pointer.
    // The key is AddRef'd so the address cannot be reused while cached; the
    // texture is kept alive by the view.
    struct DsvEntry {
        ID3D11Texture2D* texture = nullptr;
        D3D11_TEXTURE2D_DESC desc = {};
        bool qualifies = false;  // game device, single-sample, backbuffer-sized
        u32 binds = 0;           // bind transitions this frame
        u32 first_bind = 0;      // order of the first bind this frame
    };

    mutable std::mutex depth_mutex_;
    std::unordered_map<ID3D11DepthStencilView*, DsvEntry> dsv_cache_;
    ID3D11Device* depth_device_ = nullptr;  // AddRef'd game device
    u32 depth_bb_width_ = 0;
    u32 depth_bb_height_ = 0;
    u32 bind_sequence_ = 0;
    u32 presents_since_flush_ = 0;
    // Bumped every Present so each thread forgets its last-seen view.
    std::atomic<u64> depth_frame_{1};

    // Depth texture chosen at the last Present (AddRef'd).
    ID3D11Texture2D* published_depth_ = nullptr;
    D3D11_TEXTURE2D_DESC published_depth_desc_ = {};
    std::vector<IUnknown*> pending_releases_;
};

} // namespace vrc
