#pragma once

#include "core/types.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <atomic>
#include <mutex>
#include <string>

namespace vrc {

// In-game settings overlay (Dear ImGui, DX11 backend).
//
// Threading: the Present hook thread calls poll_hotkey()/render_*(); the
// game's window thread runs the subclassed WndProc. ImGui is not thread-safe,
// so both sides serialise on imgui_mutex_. The overlay is drawn into the
// game's backbuffer right before it is captured, which is what makes it
// visible in the headset; io.MouseDrawCursor makes ImGui draw the cursor into
// that same image.
class ImGuiOverlay {
public:
    static ImGuiOverlay& instance();

    // Native D3D11 game. hwnd is the swap chain's OutputWindow.
    bool initialize_d3d11(ID3D11Device* device, HWND hwnd);
    // D3D12 game: the overlay draws with the DX11 backend on a D3D11On12
    // device created over the game's command queue.
    bool initialize_d3d11on12(ID3D11Device* device, ID3D11DeviceContext* ctx,
                              ID3D11On12Device* on12, HWND hwnd);
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // Call once per Present. Edge-detects the toggle key (F2) with
    // GetAsyncKeyState while the game window is in the foreground.
    void poll_hotkey();

    // Draw into the backbuffer that is about to be presented/captured. Does
    // nothing while hidden. No backbuffer reference is kept after return, so
    // the game's ResizeBuffers keeps working.
    void render_d3d11(ID3D11Texture2D* backbuffer);
    void render_d3d12(ID3D12Resource* backbuffer);

    void set_visible(bool v);
    bool is_visible() const { return visible_.load(std::memory_order_acquire); }
    void toggle_visible();

private:
    ImGuiOverlay() = default;

    bool init_common(HWND hwnd);
    bool install_cursor_hooks();
    void remove_cursor_hooks();

    // Draws the UI into `rtv`, which targets a backbuffer of the given size.
    // Saves and restores the context's render targets around the draw.
    void draw_frame(ID3D11RenderTargetView* rtv, u32 width, u32 height);
    void build_ui();

    static LRESULT CALLBACK wnd_proc_hook(HWND hwnd, UINT msg,
                                          WPARAM w_param, LPARAM l_param);
    static BOOL WINAPI set_cursor_pos_detour(int x, int y);
    static BOOL WINAPI clip_cursor_detour(const RECT* rect);

    // Feeds the current OS cursor position to ImGui in backbuffer pixels.
    // Caller holds imgui_mutex_.
    void feed_cursor_pos_locked();

    static constexpr int kToggleKey = VK_F2;

    bool initialized_ = false;
    // Each init stage is tracked so shutdown() can unwind a partial init.
    bool imgui_context_created_ = false;
    bool win32_initialized_ = false;
    bool dx11_backend_initialized_ = false;
    bool cursor_hooks_installed_ = false;

    // Read by the window thread, the cursor detours and InputMapper's thread.
    std::atomic<bool> visible_{false};
    bool hotkey_was_down_ = false;

    // AddRef'd in initialize_*, released in shutdown().
    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_ctx_ = nullptr;
    ID3D11On12Device* d3d11on12_ = nullptr;   // null for native D3D11 games

    // Guards every ImGui call. Never held while calling into the game's
    // WndProc. Recursive so overlay code running inside a frame (e.g. the
    // editor calling set_visible) can take it again.
    std::recursive_mutex imgui_mutex_;
    // Backbuffer size / client size from the last frame, used to map client
    // cursor coordinates to backbuffer pixels. Guarded by imgui_mutex_.
    float mouse_scale_x_ = 1.0f;
    float mouse_scale_y_ = 1.0f;

    std::string ini_path_;

    // Read by wnd_proc_hook on the window thread while init/shutdown run on
    // the Present thread.
    std::atomic<WNDPROC> original_wnd_proc_{nullptr};
    HWND target_hwnd_ = nullptr;
    bool wnd_proc_installed_ = false;
};

} // namespace vrc
