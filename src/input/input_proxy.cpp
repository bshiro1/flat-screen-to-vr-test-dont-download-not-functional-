#include "input_proxy.h"
#include "core/logging.h"
#include <XInput.h>
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

    // Left stick = movement
    action_states_[InputAction::MoveForward]  = std::max(0.0f, -gamepad.sThumbLY / 32768.0f);
    action_states_[InputAction::MoveBackward] = std::max(0.0f,  gamepad.sThumbLY / 32768.0f);
    action_states_[InputAction::MoveLeft]     = std::max(0.0f, -gamepad.sThumbLX / 32768.0f);
    action_states_[InputAction::MoveRight]    = std::max(0.0f,  gamepad.sThumbLX / 32768.0f);

    // Right stick = look
    action_states_[InputAction::LookUp]     += gamepad.sThumbRY / 32768.0f;
    action_states_[InputAction::LookDown]   += -gamepad.sThumbRY / 32768.0f;
    action_states_[InputAction::LookLeft]   += -gamepad.sThumbRX / 32768.0f;
    action_states_[InputAction::LookRight]  += gamepad.sThumbRX / 32768.0f;

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

void InputProxy::inject_keyboard_input(u16 vk_code, bool press) {
    INPUT ip = {};
    ip.type = INPUT_KEYBOARD;
    ip.ki.wVk = vk_code;
    ip.ki.dwFlags = press ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &ip, sizeof(INPUT));
}

void InputProxy::inject_mouse_move(i32 dx, i32 dy) {
    INPUT ip = {};
    ip.type = INPUT_MOUSE;
    ip.mi.dx = dx;
    ip.mi.dy = dy;
    ip.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &ip, sizeof(INPUT));
}

void InputProxy::inject_mouse_button(u8 button, bool press) {
    INPUT ip = {};
    ip.type = INPUT_MOUSE;
    DWORD flags = 0;
    switch (button) {
        case 0: flags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: flags = press ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 2: flags = press ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    }
    ip.mi.dwFlags = flags;
    SendInput(1, &ip, sizeof(INPUT));
}

void InputProxy::inject_gamepad_button(u8 button, bool press) {
    // XInput gamepad button injection is done via XInputSend or virtual driver
    // For MVP, fall back to keyboard injection
    Log::debug("Gamepad injection not yet implemented");
}

void InputProxy::fire_event(const InputEvent& event) {
    if (callback_) {
        callback_(event);
    }
}

} // namespace vrc
