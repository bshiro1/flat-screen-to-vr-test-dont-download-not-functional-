#pragma once

#include "core/types.h"
#include <d3d11.h>
#include <d3d12.h>
#include <windows.h>
#include <functional>

namespace vrc {

class ImGuiOverlay {
public:
    static ImGuiOverlay& instance();

    bool initialize(void* device, GraphicsAPI api);
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // Render the overlay (called after the game's Present)
    void render();

    // Enable/disable
    void set_visible(bool v);
    bool is_visible() const { return visible_; }
    void toggle_visible();

    // Overlay key toggle
    void set_toggle_key(u16 vk) { toggle_key_ = vk; }

    // Register custom windows
    using DrawCallback = std::function<void()>;
    void register_window(const std::string& name, DrawCallback cb);
    void unregister_window(const std::string& name);

private:
    ImGuiOverlay() = default;

    bool init_d3d11(ID3D11Device* device);
    bool init_d3d12(ID3D12Device* device);
    bool init_win32();

    void begin_frame();
    void end_frame();

    static LRESULT CALLBACK wnd_proc_hook(HWND hwnd, UINT msg,
                                           WPARAM w_param, LPARAM l_param);

    bool initialized_ = false;
    bool visible_ = false;
    u16 toggle_key_ = VK_F2;

    bool cursor_managed_ = false;

    GraphicsAPI api_ = GraphicsAPI::Unknown;

    // D3D11 state
    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_ctx_ = nullptr;

    // D3D12 state
    ID3D12Device* d3d12_device_ = nullptr;

    // Registered custom windows
    std::unordered_map<std::string, DrawCallback> custom_windows_;

    // Original WndProc
    WNDPROC original_wnd_proc_ = nullptr;
    HWND target_hwnd_ = nullptr;
};

} // namespace vrc
