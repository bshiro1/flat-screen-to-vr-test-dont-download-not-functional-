#include "imgui_overlay.h"
#include "config_editor.h"
#include "core/logging.h"
#include "core/config.h"
#include "render/stereo_renderer.h"
#include "vr/openxr_context.h"
#include "vr/tracking.h"
#include "input/input_proxy.h"
#include "input/input_mapper.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>

// Forward declare ImGui_ImplWin32_WndProcHandler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

namespace vrc {

ImGuiOverlay& ImGuiOverlay::instance() {
    static ImGuiOverlay overlay;
    return overlay;
}

bool ImGuiOverlay::initialize(void* device, GraphicsAPI api) {
    if (initialized_) return true;
    api_ = api;
    Log::info("Initializing ImGui overlay");

    if (!init_win32()) return false;

    switch (api) {
        case GraphicsAPI::D3D11:
            if (!init_d3d11(static_cast<ID3D11Device*>(device))) return false;
            break;
        case GraphicsAPI::D3D12:
            if (!init_d3d12(static_cast<ID3D12Device*>(device))) return false;
            break;
        default:
            Log::error("Unsupported API for overlay");
            return false;
    }

    initialized_ = true;
    Log::info("ImGui overlay initialized");
    return true;
}

void ImGuiOverlay::shutdown() {
    if (!initialized_) return;

    if (api_ == GraphicsAPI::D3D11) {
        ImGui_ImplDX11_Shutdown();
    } else if (api_ == GraphicsAPI::D3D12) {
        ImGui_ImplDX12_Shutdown();
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    initialized_ = false;
    Log::info("ImGui overlay shut down");
}

bool ImGuiOverlay::init_win32() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Style
    ImGui::StyleColorsDark();

    // Find target window
    target_hwnd_ = GetActiveWindow();
    if (!target_hwnd_) {
        target_hwnd_ = FindWindowA(nullptr, nullptr);
    }

    if (!ImGui_ImplWin32_Init(target_hwnd_)) {
        Log::error("ImGui_ImplWin32_Init failed");
        return false;
    }

    // Subclass the window for input handling
    original_wnd_proc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(target_hwnd_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(wnd_proc_hook)));
    if (!original_wnd_proc_) {
        Log::warn("Failed to subclass WndProc, overlay input may not work");
    }

    return true;
}

bool ImGuiOverlay::init_d3d11(ID3D11Device* device) {
    d3d11_device_ = device;
    device->GetImmediateContext(&d3d11_ctx_);

    if (!ImGui_ImplDX11_Init(d3d11_device_, d3d11_ctx_)) {
        Log::error("ImGui_ImplDX11_Init failed");
        return false;
    }
    return true;
}

bool ImGuiOverlay::init_d3d12(ID3D12Device* device) {
    d3d12_device_ = device;
    // D3D12 init requires descriptor heaps, SRV descriptor size, etc.
    // These must be passed from the hook context
    Log::warn("D3D12 overlay init requires descriptor heap info");
    return false;
}

void ImGuiOverlay::begin_frame() {
    ImGui_ImplWin32_NewFrame();
    if (api_ == GraphicsAPI::D3D11) {
        ImGui_ImplDX11_NewFrame();
    }
    ImGui::NewFrame();
}

void ImGuiOverlay::end_frame() {
    ImGui::Render();
    if (api_ == GraphicsAPI::D3D11) {
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void ImGuiOverlay::render() {
    if (!visible_ || !initialized_) return;

    begin_frame();

    // Main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("VR Converter")) {
            ImGui::MenuItem("Config Editor", nullptr, &visible_);
            ImGui::Separator();
            if (ImGui::MenuItem("Toggle Overlay", "F2")) {
                toggle_visible();
            }
            ImGui::MenuItem("Exit", nullptr, false);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Performance")) {}
            if (ImGui::MenuItem("Tracking")) {}
            if (ImGui::MenuItem("Input")) {}
            ImGui::EndMenu();
        }

        ImGui::Text("| FPS: %d  Frame: %lld",
                    Config::instance().latency_stats().fps,
                    StereoRenderer::instance().frame_count());
        ImGui::EndMainMenuBar();
    }

    // Draw config editor window by default
    ConfigEditor::instance().draw();

    // Draw custom registered windows
    for (auto& [name, cb] : custom_windows_) {
        if (ImGui::Begin(name.c_str(), nullptr)) {
            cb();
        }
        ImGui::End();
    }

    end_frame();
}

void ImGuiOverlay::register_window(const std::string& name, DrawCallback cb) {
    custom_windows_[name] = cb;
}

void ImGuiOverlay::unregister_window(const std::string& name) {
    custom_windows_.erase(name);
}

LRESULT CALLBACK ImGuiOverlay::wnd_proc_hook(
    HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    auto& self = instance();

    // Toggle overlay with F2
    if (msg == WM_KEYDOWN && w_param == self.toggle_key_) {
        self.toggle_visible();
    }

    if (self.visible_) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param))
            return TRUE;
    }

    // Chain to original WndProc
    if (self.original_wnd_proc_) {
        return CallWindowProcA(self.original_wnd_proc_, hwnd, msg, w_param, l_param);
    }

    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

} // namespace vrc
