#pragma once

#include "core/types.h"
#include "input/xr_input.h"
#include <windows.h>
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>

namespace vrc {

enum class InputDevice : u8 {
    Keyboard,
    Mouse,
    Gamepad,
    VRController,
    Hybrid
};

enum class InputAction : u32 {
    MoveForward    = 0,
    MoveBackward   = 1,
    MoveLeft       = 2,
    MoveRight      = 3,
    LookUp         = 4,
    LookDown       = 5,
    LookLeft       = 6,
    LookRight      = 7,
    Jump           = 8,
    Crouch         = 9,
    Sprint         = 10,
    Interact       = 11,
    PrimaryFire    = 12,
    SecondaryFire  = 13,
    Reload         = 14,
    Menu           = 15,
    Map            = 16,
    Inventory      = 17,
    Custom0        = 32,
    Custom1        = 33,
    Custom2        = 34,
};

struct InputEvent {
    InputAction action;
    InputDevice device;
    f32 value = 0.0f;      // 0.0-1.0 for analog, 0/1 for digital
    Vec2 axis_value;        // For joystick/thumbstick
    bool pressed = false;
    bool released = false;
    u64 timestamp_us = 0;
};

class InputProxy {
public:
    static InputProxy& instance();

    void initialize();
    void shutdown();

    // Poll current state
    bool is_action_active(InputAction action) const;
    f32 get_action_value(InputAction action) const;

    // Event callback
    using InputCallback = std::function<void(const InputEvent&)>;
    void set_callback(InputCallback cb) { callback_ = cb; }

    // Inject simulated input back into the game
    void inject_keyboard_input(u16 vk_code, bool press);
    void inject_mouse_move(i32 dx, i32 dy);
    void inject_mouse_button(u8 button, bool press);
    void inject_gamepad_button(u8 button, bool press);

    // Poll all input devices (keyboard, mouse, gamepad, VR controllers)
    void poll();

    // Register custom bindings
    void set_action_mapping(InputAction action, const std::string& binding);

    // VR controller state (populated by poll_vr_controllers via XrInput)
    const XrControllerState& vr_state() const { return XrInput::instance().state(); }

    // State access
    const std::unordered_map<InputAction, f32>& action_states() const {
        return action_states_;
    }

private:
    InputProxy() = default;

    void poll_keyboard();
    void poll_mouse();
    void poll_gamepad();
    void poll_vr_controllers();

    void fire_event(const InputEvent& event);

    InputCallback callback_;
    std::unordered_map<InputAction, f32> action_states_;
    std::unordered_map<InputAction, f32> previous_action_states_;

    // Raw input state
    u8 keyboard_state_[256] = {};
    bool gamepad_connected_ = false;
};

} // namespace vrc
