#pragma once

#include "core/types.h"
#include <openxr/openxr.h>

namespace vrc {

struct XrControllerState {
    // Analog axes
    f32  trigger_left  = 0.0f;
    f32  trigger_right = 0.0f;
    f32  grip_left     = 0.0f;
    f32  grip_right    = 0.0f;
    Vec2 thumbstick_left{};
    Vec2 thumbstick_right{};

    // Digital buttons
    bool button_a                = false;
    bool button_b                = false;
    bool button_x                = false;
    bool button_y                = false;
    bool menu                    = false;
    bool thumbstick_click_left   = false;
    bool thumbstick_click_right  = false;
    bool trigger_click_left      = false;   // trigger > 0.7
    bool trigger_click_right     = false;
    bool grip_click_left         = false;   // grip > 0.7
    bool grip_click_right        = false;

    // Hand poses (grip space)
    bool left_tracked  = false;
    bool right_tracked = false;
    Vec3 left_grip_pos{};
    Quat left_grip_rot{0, 0, 0, 1};
    Vec3 right_grip_pos{};
    Quat right_grip_rot{0, 0, 0, 1};

    // Aim poses
    Vec3 left_aim_pos{};
    Quat left_aim_rot{0, 0, 0, 1};
    Vec3 right_aim_pos{};
    Quat right_aim_rot{0, 0, 0, 1};
};

class XrInput {
public:
    static XrInput& instance();

    bool initialize(XrInstance xr_instance, XrSession session);
    void shutdown();

    // Call once per frame from the OpenXR frame thread, after xrBeginFrame
    void sync(XrSession session, XrSpace ref_space, XrTime predicted_time);

    const XrControllerState& state() const { return state_; }
    bool is_active() const { return active_; }

    void trigger_haptic(XrSession session, bool left, f32 amplitude, f32 duration_s);

private:
    XrInput() = default;

    bool create_action_set(XrInstance xr_instance);
    bool suggest_bindings(XrInstance xr_instance);
    bool attach(XrSession session);

    XrActionSet action_set_ = XR_NULL_HANDLE;

    // Float actions
    XrAction a_trigger_left_            = XR_NULL_HANDLE;
    XrAction a_trigger_right_           = XR_NULL_HANDLE;
    XrAction a_grip_left_               = XR_NULL_HANDLE;
    XrAction a_grip_right_              = XR_NULL_HANDLE;
    XrAction a_thumbstick_left_         = XR_NULL_HANDLE;
    XrAction a_thumbstick_right_        = XR_NULL_HANDLE;

    // Bool actions
    XrAction a_button_a_                = XR_NULL_HANDLE;
    XrAction a_button_b_                = XR_NULL_HANDLE;
    XrAction a_button_x_                = XR_NULL_HANDLE;
    XrAction a_button_y_                = XR_NULL_HANDLE;
    XrAction a_menu_                    = XR_NULL_HANDLE;
    XrAction a_thumbstick_click_left_   = XR_NULL_HANDLE;
    XrAction a_thumbstick_click_right_  = XR_NULL_HANDLE;

    // Pose actions
    XrAction a_grip_pose_left_          = XR_NULL_HANDLE;
    XrAction a_grip_pose_right_         = XR_NULL_HANDLE;
    XrAction a_aim_pose_left_           = XR_NULL_HANDLE;
    XrAction a_aim_pose_right_          = XR_NULL_HANDLE;

    // Haptic output
    XrAction a_haptic_left_             = XR_NULL_HANDLE;
    XrAction a_haptic_right_            = XR_NULL_HANDLE;

    // Action spaces for pose queries
    XrSpace space_grip_left_   = XR_NULL_HANDLE;
    XrSpace space_grip_right_  = XR_NULL_HANDLE;
    XrSpace space_aim_left_    = XR_NULL_HANDLE;
    XrSpace space_aim_right_   = XR_NULL_HANDLE;

    XrControllerState state_{};
    bool active_ = false;
};

} // namespace vrc
