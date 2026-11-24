#include "imgui_overlay.h"
#include "config_editor.h"
#include "core/logging.h"
#include "core/config.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <MinHook.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

namespace vrc {

namespace {

using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

// MinHook trampolines and hooked addresses for the cursor hooks. MinHook is a
// static library inside this DLL, so its state is shared with the D3D hooks.
SetCursorPosFn g_orig_set_cursor_pos = nullptr;
ClipCursorFn g_orig_clip_cursor = nullptr;
void* g_set_cursor_pos_target = nullptr;
void* g_clip_cursor_target = nullptr;

bool foreground_window_is_ours() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Mouse "button up" messages are passed on to the game even while the overlay
// is visible: a button that was held when the overlay opened must still be
// released in the game, otherwise it stays logically pressed.
bool is_mouse_button_up(UINT msg) {
    return msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
           msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
}

void remove_hook(void*& target, const char* name) {
    if (!target) return;
    MH_STATUS s = MH_DisableHook(target);
    if (s == MH_OK || s == MH_ERROR_DISABLED) s = MH_RemoveHook(target);
    if (s != MH_OK) {
        // MH_ERROR_NOT_INITIALIZED means another component already called
        // MH_Uninitialize, which removed every hook including this one.
        Log::warn("Overlay: removing {} hook returned {}", name, MH_StatusToString(s));
    }
    target = nullptr;
}

} // namespace

ImGuiOverlay& ImGuiOverlay::instance() {
    static ImGuiOverlay overlay;
    return overlay;
}

bool ImGuiOverlay::initialize_d3d11(ID3D11Device* device, HWND hwnd) {
    if (initialized_) return true;
    if (!device) {
        Log::error("Overlay: initialize_d3d11 called without a device");
        return false;
    }
    Log::info("Initializing ImGui overlay (D3D11)");

    device->AddRef();
    d3d11_device_ = device;
    device->GetImmediateContext(&d3d11_ctx_);  // AddRef'd by the call

    if (!init_common(hwnd)) {
        shutdown();
        return false;
    }
    initialized_ = true;
    // Start hidden: while visible the overlay takes the game's mouse and
    // keyboard input. F2 opens it.
    Log::info("ImGui overlay initialized (D3D11); press F2 to open");
    return true;
}

bool ImGuiOverlay::initialize_d3d11on12(ID3D11Device* device, ID3D11DeviceContext* ctx,
                                        ID3D11On12Device* on12, HWND hwnd) {
    if (initialized_) return true;
    if (!device || !ctx || !on12) {
        Log::error("Overlay: initialize_d3d11on12 called with a null device/context/on12");
        return false;
    }
    Log::info("Initializing ImGui overlay (D3D12 via D3D11On12)");

    device->AddRef();
    ctx->AddRef();
    on12->AddRef();
    d3d11_device_ = device;
    d3d11_ctx_ = ctx;
    d3d11on12_ = on12;

    if (!init_common(hwnd)) {
        shutdown();
        return false;
    }
    initialized_ = true;
    Log::info("ImGui overlay initialized (D3D12 via D3D11On12); press F2 to open");
    return true;
}

bool ImGuiOverlay::init_common(HWND hwnd) {
    DWORD wnd_pid = 0;
    if (hwnd && IsWindow(hwnd)) GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid != GetCurrentProcessId()) {
        Log::error("Overlay: swap chain window {:p} is not a window of this process",
                   static_cast<void*>(hwnd));
        return false;
    }
    target_hwnd_ = hwnd;

    {
        std::lock_guard lock(imgui_mutex_);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imgui_context_created_ = true;

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // The OS cursor is not part of the captured frame, so ImGui draws its
        // own cursor into the image that is shown in the headset.
        io.MouseDrawCursor = true;
        // Keep imgui.ini (window positions) next to config.json rather than in
        // the game's working directory.
        ini_path_ = (Config::instance().config_dir() / "imgui.ini").string();
        io.IniFilename = ini_path_.c_str();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(hwnd)) {
            Log::error("ImGui_ImplWin32_Init failed");
            return false;
        }
        win32_initialized_ = true;

        if (!ImGui_ImplDX11_Init(d3d11_device_, d3d11_ctx_)) {
            Log::error("ImGui_ImplDX11_Init failed");
            return false;
        }
        dx11_backend_initialized_ = true;
    }

    if (!install_cursor_hooks()) {
        Log::warn("Overlay: cursor hooks unavailable; games that recentre the "
                  "mouse every frame will pin the overlay cursor");
    }

    // Subclass the swap chain's window. If our proc is still in the chain from
    // an earlier session (another hook subclassed after us, so shutdown() left
    // it in place), subclassing again would make the chain call itself.
    if (wnd_proc_installed_) {
        Log::warn("Overlay: WndProc hook from a previous session is still installed; reusing it");
        return true;
    }
    // Publish the original proc before swapping, so a message that arrives on
    // the window thread right after SetWindowLongPtrW can already be chained.
    auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    original_wnd_proc_ = current;
    SetLastError(0);
    LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                      reinterpret_cast<LONG_PTR>(&wnd_proc_hook));
    if (prev == 0 && GetLastError() != 0) {
        Log::warn("Overlay: failed to subclass the game window (error {}); overlay input disabled",
                  GetLastError());
        return true;
    }
    original_wnd_proc_ = reinterpret_cast<WNDPROC>(prev);
    wnd_proc_installed_ = true;
    return true;
}

bool ImGuiOverlay::install_cursor_hooks() {
    if (cursor_hooks_installed_) return true;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log::warn("Overlay: MH_Initialize failed: {}", MH_StatusToString(s));
        return false;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return false;

    auto hook = [](HMODULE mod, const char* name, void* detour, void** original,
                   void*& target_out) -> bool {
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!target) {
            Log::warn("Overlay: {} not found in user32", name);
            return false;
        }
        MH_STATUS st = MH_CreateHook(target, detour, original);
        if (st != MH_OK) {
            Log::warn("Overlay: MH_CreateHook({}) failed: {}", name, MH_StatusToString(st));
            return false;
        }
        st = MH_EnableHook(target);
        if (st != MH_OK) {
            Log::warn("Overlay: MH_EnableHook({}) failed: {}", name, MH_StatusToString(st));
            MH_RemoveHook(target);
            return false;
        }
        target_out = target;
        return true;
    };

    bool ok = hook(user32, "SetCursorPos", reinterpret_cast<void*>(&set_cursor_pos_detour),
                   reinterpret_cast<void**>(&g_orig_set_cursor_pos), g_set_cursor_pos_target);
    ok = hook(user32, "ClipCursor", reinterpret_cast<void*>(&clip_cursor_detour),
              reinterpret_cast<void**>(&g_orig_clip_cursor), g_clip_cursor_target) && ok;

    cursor_hooks_installed_ = g_set_cursor_pos_target || g_clip_cursor_target;
    return ok;
}

void ImGuiOverlay::remove_cursor_hooks() {
    if (!cursor_hooks_installed_) return;
    remove_hook(g_set_cursor_pos_target, "SetCursorPos");
    remove_hook(g_clip_cursor_target, "ClipCursor");
    cursor_hooks_installed_ = false;
}

BOOL WINAPI ImGuiOverlay::set_cursor_pos_detour(int x, int y) {
    // FPS games recentre the cursor every frame; while the overlay is open the
    // cursor must stay where the user moved it.
    if (instance().visible_.load(std::memory_order_acquire)) return TRUE;
    return g_orig_set_cursor_pos(x, y);
}

BOOL WINAPI ImGuiOverlay::clip_cursor_detour(const RECT* rect) {
    // Suppress confining the cursor while the overlay is open. ClipCursor(NULL)
    // (release) is always allowed through.
    if (rect && instance().visible_.load(std::memory_order_acquire)) return TRUE;
    return g_orig_clip_cursor(rect);
}

void ImGuiOverlay::shutdown() {
    remove_cursor_hooks();

    // Restore the original WndProc only if ours is still the head of the
    // chain. If something subclassed the window after us, restoring would cut
    // that hook out; leave ours in place (it passes everything through once
    // visible_ is false). original_wnd_proc_ is never cleared because the
    // window thread may be inside wnd_proc_hook right now.
    if (wnd_proc_installed_ && target_hwnd_) {
        if (!IsWindow(target_hwnd_)) {
            wnd_proc_installed_ = false;
        } else if (GetWindowLongPtrW(target_hwnd_, GWLP_WNDPROC) ==
                   reinterpret_cast<LONG_PTR>(&wnd_proc_hook)) {
            SetWindowLongPtrW(target_hwnd_, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(original_wnd_proc_.load()));
            wnd_proc_installed_ = false;
        } else {
            Log::warn("Overlay: game window was subclassed after the overlay; "
                      "leaving the overlay WndProc in the chain");
        }
    }
    visible_.store(false, std::memory_order_release);
    hotkey_was_down_ = false;

    {
        std::lock_guard lock(imgui_mutex_);
        if (dx11_backend_initialized_) {
            ImGui_ImplDX11_Shutdown();
            dx11_backend_initialized_ = false;
        }
        if (win32_initialized_) {
            ImGui_ImplWin32_Shutdown();
            win32_initialized_ = false;
        }
        if (imgui_context_created_) {
            ImGui::DestroyContext();
            imgui_context_created_ = false;
        }
    }

    if (d3d11on12_) { d3d11on12_->Release(); d3d11on12_ = nullptr; }
    if (d3d11_ctx_) { d3d11_ctx_->Release(); d3d11_ctx_ = nullptr; }
    if (d3d11_device_) { d3d11_device_->Release(); d3d11_device_ = nullptr; }

    if (initialized_) Log::info("ImGui overlay shut down");
    initialized_ = false;
}

void ImGuiOverlay::poll_hotkey() {
    if (!initialized_) return;
    const bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    // GetAsyncKeyState is global; ignore F2 typed into another application.
    if (down && !hotkey_was_down_ && foreground_window_is_ours()) {
        toggle_visible();
    }
    hotkey_was_down_ = down;
}

void ImGuiOverlay::render_d3d11(ID3D11Texture2D* backbuffer) {
    if (!initialized_ || d3d11on12_ || !backbuffer || !is_visible()) return;

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);

    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = d3d11_device_->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            Log::warn("Overlay: CreateRenderTargetView on backbuffer failed (hr=0x{:08x})",
                      static_cast<u32>(hr));
            logged = true;
        }
        return;
    }
    draw_frame(rtv, desc.Width, desc.Height);
    rtv->Release();
}

void ImGuiOverlay::render_d3d12(ID3D12Resource* backbuffer) {
    if (!initialized_ || !d3d11on12_ || !backbuffer || !is_visible()) return;

    const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();

    // The game transitions the backbuffer to PRESENT before calling Present,
    // and D3D11On12 must hand it back in that same state.
    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Resource* wrapped = nullptr;
    HRESULT hr = d3d11on12_->CreateWrappedResource(
        backbuffer, &flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
        IID_PPV_ARGS(&wrapped));
    if (FAILED(hr) || !wrapped) {
        static bool logged = false;
        if (!logged) {
            Log::warn("Overlay: CreateWrappedResource on D3D12 backbuffer failed (hr=0x{:08x})",
                      static_cast<u32>(hr));
            logged = true;
        }
        return;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    hr = d3d11_device_->CreateRenderTargetView(wrapped, nullptr, &rtv);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            Log::warn("Overlay: CreateRenderTargetView on wrapped backbuffer failed (hr=0x{:08x})",
                      static_cast<u32>(hr));
            logged = true;
        }
        wrapped->Release();
        return;
    }

    d3d11on12_->AcquireWrappedResources(&wrapped, 1);
    draw_frame(rtv, static_cast<u32>(desc.Width), desc.Height);
    d3d11on12_->ReleaseWrappedResources(&wrapped, 1);

    // Drop every reference to the backbuffer before submitting, so nothing
    // outlives this call and the game's ResizeBuffers can succeed.
    rtv->Release();
    wrapped->Release();
    d3d11_ctx_->Flush();
}

void ImGuiOverlay::feed_cursor_pos_locked() {
    if (!imgui_context_created_ || !target_hwnd_) return;
    // The game may centre the cursor or consume raw input, so read the real
    // OS cursor instead of trusting WM_MOUSEMOVE coordinates.
    POINT p{};
    if (GetCursorPos(&p) && ScreenToClient(target_hwnd_, &p)) {
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(p.x) * mouse_scale_x_,
                                        static_cast<float>(p.y) * mouse_scale_y_);
    }
}

void ImGuiOverlay::draw_frame(ID3D11RenderTargetView* rtv, u32 width, u32 height) {
    if (width == 0 || height == 0) return;

    std::lock_guard lock(imgui_mutex_);
    if (!imgui_context_created_ || !dx11_backend_initialized_) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // ImGui_ImplWin32_NewFrame sizes the UI to the window's client area. The
    // backbuffer can differ (render resolution, DPI virtualisation), so draw
    // in backbuffer pixels and map client-space cursor positions to them.
    RECT rc{};
    GetClientRect(target_hwnd_, &rc);
    const LONG cw = rc.right - rc.left;
    const LONG ch = rc.bottom - rc.top;
    mouse_scale_x_ = cw > 0 ? static_cast<float>(width) / static_cast<float>(cw) : 1.0f;
    mouse_scale_y_ = ch > 0 ? static_cast<float>(height) / static_cast<float>(ch) : 1.0f;
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    feed_cursor_pos_locked();

    // Games that register raw input with RIDEV_NOLEGACY never receive
    // WM_*BUTTON* messages, so also poll the buttons while the game window is
    // focused. ImGui drops events that repeat the current state, so this does
    // not double up with the WndProc path. GetAsyncKeyState reports physical
    // buttons, hence the swap check.
    if (foreground_window_is_ours()) {
        const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        const int vk_left = swapped ? VK_RBUTTON : VK_LBUTTON;
        const int vk_right = swapped ? VK_LBUTTON : VK_RBUTTON;
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, (GetAsyncKeyState(vk_left) & 0x8000) != 0);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, (GetAsyncKeyState(vk_right) & 0x8000) != 0);
        io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    }

    ImGui::NewFrame();
    build_ui();
    ImGui::Render();

    // ImGui's DX11 backend saves and restores most pipeline state but not the
    // output-merger targets, so do that here.
    ID3D11RenderTargetView* old_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* old_dsv = nullptr;
    d3d11_ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtvs, &old_dsv);

    d3d11_ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    d3d11_ctx_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtvs, old_dsv);
    for (auto* v : old_rtvs) if (v) v->Release();
    if (old_dsv) old_dsv->Release();
}

void ImGuiOverlay::build_ui() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("VR Converter")) {
            bool editor_open = ConfigEditor::instance().is_open();
            if (ImGui::MenuItem("Config Editor", nullptr, editor_open)) {
                ConfigEditor::instance().set_open(!editor_open);
            }
            if (ImGui::MenuItem("Hide Overlay", "F2")) {
                set_visible(false);
            }
            ImGui::EndMenu();
        }
        ImGui::Text("| FPS: %u", Config::instance().latency_stats().fps);
        ImGui::EndMainMenuBar();
    }

    ConfigEditor::instance().draw();
}

void ImGuiOverlay::set_visible(bool v) {
    // Runtime visibility only. Config::enable_overlay() means "overlay (and
    // its F2 key) enabled" and gates poll_hotkey() in the Present hook, so it
    // must not follow visibility or F2 could never reopen the overlay.
    const bool was = visible_.exchange(v, std::memory_order_acq_rel);
    if (was == v) return;

    if (v) {
        // Reopening the overlay always brings back the editor window, even if
        // it was closed with its X button.
        ConfigEditor::instance().set_open(true);
        // Release a clip the game applied before the overlay opened; further
        // clips are suppressed by clip_cursor_detour while visible.
        ClipCursor(nullptr);

        // Drop input state left over from the last time the overlay was open
        // (keys/buttons whose release was never seen while hidden).
        std::lock_guard lock(imgui_mutex_);
        if (imgui_context_created_) {
            ImGuiIO& io = ImGui::GetIO();
            io.ClearEventsQueue();
            io.ClearInputKeys();
            io.ClearInputMouse();
        }
    }
    // When hiding, the game re-applies its own cursor clip/position on its
    // next input update.
}

void ImGuiOverlay::toggle_visible() {
    set_visible(!is_visible());
}

LRESULT CALLBACK ImGuiOverlay::wnd_proc_hook(
    HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    auto& self = instance();
    WNDPROC original = self.original_wnd_proc_.load();

    if (self.visible_.load(std::memory_order_acquire)) {
        LRESULT imgui_result = 0;
        {
            std::lock_guard lock(self.imgui_mutex_);
            if (self.win32_initialized_) {
                if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
                    // Mouse messages are translated here instead of by
                    // ImGui_ImplWin32_WndProcHandler: its SetCapture/
                    // ReleaseCapture would synchronously send
                    // WM_CAPTURECHANGED into the game's proc while this lock
                    // is held. The position comes from GetCursorPos, scaled to
                    // the backbuffer, so clicks land where ImGui draws the
                    // cursor.
                    self.feed_cursor_pos_locked();
                    ImGuiIO& io = ImGui::GetIO();
                    switch (msg) {
                        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true);  break;
                        case WM_LBUTTONUP:                          io.AddMouseButtonEvent(0, false); break;
                        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true);  break;
                        case WM_RBUTTONUP:                          io.AddMouseButtonEvent(1, false); break;
                        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: io.AddMouseButtonEvent(2, true);  break;
                        case WM_MBUTTONUP:                          io.AddMouseButtonEvent(2, false); break;
                        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
                            io.AddMouseButtonEvent(GET_XBUTTON_WPARAM(w_param) == XBUTTON1 ? 3 : 4,
                                                   msg != WM_XBUTTONUP);
                            break;
                        case WM_MOUSEWHEEL:
                            io.AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / WHEEL_DELTA);
                            break;
                        case WM_MOUSEHWHEEL:
                            io.AddMouseWheelEvent(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) / WHEEL_DELTA, 0.0f);
                            break;
                        default: break;
                    }
                } else if (msg != WM_NCMOUSEMOVE && msg != WM_NCMOUSELEAVE) {
                    // Keyboard, characters, focus and WM_SETCURSOR. Non-client
                    // mouse moves are skipped: their unscaled coordinates would
                    // fight the GetCursorPos feed.
                    imgui_result = ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param);
                }
            }
        }
        if (imgui_result) return imgui_result;

        // While the overlay is open the game must not see clicks, key presses
        // or raw input, otherwise using a slider also fires the weapon and
        // turns the camera.
        switch (msg) {
            case WM_INPUT:
                // DefWindowProc performs the required raw-input cleanup.
                return DefWindowProcW(hwnd, msg, w_param, l_param);
            case WM_KEYDOWN:
            case WM_CHAR:
            case WM_DEADCHAR:
            case WM_SYSCHAR:
            case WM_SYSDEADCHAR:
                return 0;
            case WM_SYSKEYDOWN:
                // Hidden from the game, but DefWindowProc keeps system
                // shortcuts such as Alt+F4 working.
                return DefWindowProcW(hwnd, msg, w_param, l_param);
            case WM_KEYUP:
            case WM_SYSKEYUP:
                // Key releases reach the game so keys held when the overlay
                // opened do not stay stuck down.
                break;
            default:
                if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && !is_mouse_button_up(msg)) {
                    return 0;
                }
                break;
        }
    }

    if (original) {
        return CallWindowProcW(original, hwnd, msg, w_param, l_param);
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

} // namespace vrc
