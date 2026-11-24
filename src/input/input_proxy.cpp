#include "input_proxy.h"
#include "core/config.h"
#include "core/logging.h"
#include <XInput.h>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "XInput9_1_0.lib")

namespace vrc {

InputProxy& InputProxy::instance() {
    static InputProxy proxy;
    return proxy;
}

void InputProxy::initialize() {
    Log::info("Input proxy initialized");
    ZeroMemory(keyboard_state_, sizeof(keyboard_state_));
}

void InputProxy::shutdown() {
    Log::info("Input proxy shut down");
}

void InputProxy::poll_keyboard() {
    GetKeyboardState(keyboard_state_);
}

void InputProxy::poll_mouse() {
    // Mouse state is tracked via raw input or WM_INPUT messages
}

void InputProxy::poll_gamepad() {
    XINPUT_STATE state;
    DWORD result = XInputGetState(0, &state);
    gamepad_connected_ = (result == ERROR_SUCCESS);

    if (!gamepad_connected_) return;

    auto& gamepad = state.Gamepad;

    // Map gamepad inputs to actions
    auto map_button = [&](InputAction action, WORD mask) {
        bool pressed = (gamepad.wButtons & mask) != 0;
        action_states_[action] = pressed ? 1.0f : 0.0f;
    };

    map_button(InputAction::Interact,       XINPUT_GAMEPAD_A);
    map_button(InputAction::SecondaryFire,  XINPUT_GAMEPAD_B);
    map_button(InputAction::Reload,         XINPUT_GAMEPAD_X);
    map_button(InputAction::Menu,           XINPUT_GAMEPAD_Y);
    map_button(InputAction::Sprint,         XINPUT_GAMEPAD_LEFT_THUMB);
    map_button(InputAction::Crouch,         XINPUT_GAMEPAD_RIGHT_THUMB);
    map_button(InputAction::Jump,           XINPUT_GAMEPAD_LEFT_SHOULDER);

    // Left stick = movement. XInput sThumbLY is positive when pushed UP,
    // so up = forward.
    action_states_[InputAction::MoveForward]  = std::max(0.0f,  gamepad.sThumbLY / 32768.0f);
    action_states_[InputAction::MoveBackward] = std::max(0.0f, -gamepad.sThumbLY / 32768.0f);
    action_states_[InputAction::MoveLeft]     = std::max(0.0f, -gamepad.sThumbLX / 32768.0f);
    action_states_[InputAction::MoveRight]    = std::max(0.0f,  gamepad.sThumbLX / 32768.0f);

    // Right stick = look. Assigned, not accumulated: these are per-poll
    // instantaneous values.
    action_states_[InputAction::LookUp]     = std::max(0.0f,  gamepad.sThumbRY / 32768.0f);
    action_states_[InputAction::LookDown]   = std::max(0.0f, -gamepad.sThumbRY / 32768.0f);
    action_states_[InputAction::LookLeft]   = std::max(0.0f, -gamepad.sThumbRX / 32768.0f);
    action_states_[InputAction::LookRight]  = std::max(0.0f,  gamepad.sThumbRX / 32768.0f);

    // Triggers
    action_states_[InputAction::PrimaryFire] = gamepad.bLeftTrigger / 255.0f;
}

bool InputProxy::is_action_active(InputAction action) const {
    auto it = action_states_.find(action);
    return it != action_states_.end() && it->second > 0.1f;
}

f32 InputProxy::get_action_value(InputAction action) const {
    auto it = action_states_.find(action);
    return it != action_states_.end() ? it->second : 0.0f;
}

void InputProxy::poll() {
    poll_keyboard();
    poll_mouse();
    poll_gamepad();
    poll_vr_controllers();
}

void InputProxy::poll_vr_controllers() {
    // XrInput::sync() is called from the OpenXR frame thread each frame.
    // Here we just mirror VR button state into action_states_ so that
    // is_action_active() queries work without going through InputMapper.
    auto& vr = XrInput::instance().state();
    if (!XrInput::instance().is_active()) return;

    action_states_[InputAction::PrimaryFire]   = vr.trigger_right;
    action_states_[InputAction::SecondaryFire] = vr.grip_right;
    action_states_[InputAction::Jump]    = vr.button_a    ? 1.0f : 0.0f;
    action_states_[InputAction::Crouch]  = vr.button_b    ? 1.0f : 0.0f;
    action_states_[InputAction::Reload]  = vr.button_x    ? 1.0f : 0.0f;
    action_states_[InputAction::Interact]= vr.button_y    ? 1.0f : 0.0f;
    action_states_[InputAction::Menu]    = vr.menu        ? 1.0f : 0.0f;
    action_states_[InputAction::Sprint]  = vr.thumbstick_click_left ? 1.0f : 0.0f;

    // Left stick movement
    f32 lx = vr.thumbstick_left.x, ly = vr.thumbstick_left.y;
    action_states_[InputAction::MoveForward]  = std::max(0.0f,  ly);
    action_states_[InputAction::MoveBackward] = std::max(0.0f, -ly);
    action_states_[InputAction::MoveLeft]     = std::max(0.0f, -lx);
    action_states_[InputAction::MoveRight]    = std::max(0.0f,  lx);
}

void InputProxy::set_action_mapping(InputAction action, const std::string& binding) {
    Log::info("Setting action mapping: {} -> {}",
              static_cast<u32>(action), binding);
}

namespace {

bool legacy_injection_enabled() {
    const auto profile = Config::instance().current_profile_copy();
    return profile.injection_method != InjectionMethod::SixDofBridge ||
           profile.bridge_send_legacy_input;
}

// SendInput delivers to whichever window has keyboard focus. Only inject while
// a window of this (the game's) process is in the foreground, so VR input
// never types or clicks into other applications while the headset is on.
// Returns the foreground window's thread id via `fg_thread` when it is ours.
bool game_window_focused(DWORD* fg_thread = nullptr) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return false;
    if (fg_thread) *fg_thread = tid;
    return true;
}

// Keys whose scan code needs KEYEVENTF_EXTENDEDKEY (the E0 prefix). Listed
// explicitly because MapVirtualKey does not report the prefix for all of them.
bool is_extended_key(u16 vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:
        case VK_LEFT:   case VK_RIGHT:  case VK_UP:   case VK_DOWN:
        case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN:   case VK_RWIN:   case VK_APPS:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT: case VK_CANCEL:
            return true;
        default:
            return false;
    }
}

} // namespace

bool InputProxy::allow_injection(bool is_release, const char* what) {
    if (!legacy_injection_enabled()) return false;
    // Releases always go through: dropping one would leave the key logically
    // held system-wide (e.g. a stuck Shift) after focus returns.
    if (is_release || game_window_focused()) return true;

    ++dropped_while_unfocused_;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_drop_log_ >= std::chrono::seconds(5)) {
        Log::info("Input: game window not in foreground; dropped {} injected event(s) "
                  "(latest: {})", dropped_while_unfocused_, what);
        dropped_while_unfocused_ = 0;
        last_drop_log_ = now;
    }
    return false;
}

void InputProxy::inject_keyboard_input(u16 vk_code, bool press) {
    std::lock_guard<std::mutex> lock(injection_mutex_);
    if (!allow_injection(!press, "keyboard")) return;

    // Scan-code events reach raw-input and DirectInput games, which ignore
    // VK-only events. Map with the game window's keyboard layout.
    DWORD fg_thread = 0;
    game_window_focused(&fg_thread);
    HKL layout = GetKeyboardLayout(fg_thread);
    const UINT sc_ex = MapVirtualKeyExW(vk_code, MAPVK_VK_TO_VSC_EX, layout);
    const WORD scan = static_cast<WORD>(sc_ex & 0xFF);

    INPUT ip = {};
    ip.type = INPUT_KEYBOARD;
    if (scan != 0 && vk_code != VK_PAUSE) {
        const bool extended = is_extended_key(vk_code) ||
                              (sc_ex & 0xFF00) == 0xE000 || (sc_ex & 0xFF00) == 0xE100;
        ip.ki.wScan = scan;
        ip.ki.dwFlags = KEYEVENTF_SCANCODE |
                        (extended ? KEYEVENTF_EXTENDEDKEY : 0) |
                        (press ? 0 : KEYEVENTF_KEYUP);
    } else {
        // No scan code (media keys, Pause's E1 sequence): VK-only event.
        ip.ki.wVk = vk_code;
        ip.ki.dwFlags = press ? 0 : KEYEVENTF_KEYUP;
    }
    SendInput(1, &ip, sizeof(INPUT));
}

void InputProxy::inject_mouse_move(i32 dx, i32 dy) {
    std::lock_guard<std::mutex> lock(injection_mutex_);
    if (!allow_injection(false, "mouse move")) return;
    // A tracker glitch must never inject a single-frame look snap large
    // enough to whip the camera. Normal deltas are only a few pixels.
    constexpr i32 kMaxDelta = 200;
    dx = std::clamp(dx, -kMaxDelta, kMaxDelta);
    dy = std::clamp(dy, -kMaxDelta, kMaxDelta);

    INPUT ip = {};
    ip.type = INPUT_MOUSE;
    ip.mi.dx = dx;
    ip.mi.dy = dy;
    ip.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &ip, sizeof(INPUT));
}

void InputProxy::inject_mouse_button(u8 button, bool press) {
    std::lock_guard<std::mutex> lock(injection_mutex_);
    if (!allow_injection(!press, "mouse button")) return;
    INPUT ip = {};
    ip.type = INPUT_MOUSE;
    DWORD flags = 0;
    switch (button) {
        case 0: flags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: flags = press ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 2: flags = press ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        default: return;
    }
    ip.mi.dwFlags = flags;
    SendInput(1, &ip, sizeof(INPUT));
}

void InputProxy::inject_gamepad_button(u8 button, bool press) {
    (void)button;
    (void)press;
    // XInput has no injection API; a virtual gamepad driver would be needed.
    // Bindings that target the game use keyboard/mouse injection instead.
    Log::debug("Gamepad button injection is not supported");
}

void InputProxy::fire_event(const InputEvent& event) {
    if (callback_) {
        callback_(event);
    }
}

} // namespace vrc
