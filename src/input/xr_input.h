#pragma once

#include "core/types.h"
#include "input/xr_shared_state.h"
#include <openxr/openxr.h>
#include <array>
#include <string>

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

    // Hand poses (grip space). Kept for source compatibility with existing
    // callers; the unified grip/aim arrays below carry the same data.
    bool left_grip_tracked  = false;
    bool right_grip_tracked = false;
    Vec3 left_grip_pos{};
    Quat left_grip_rot{0, 0, 0, 1};
    Vec3 right_grip_pos{};
    Quat right_grip_rot{0, 0, 0, 1};

    // Aim poses
    bool left_aim_tracked  = false;
    bool right_aim_tracked = false;
    Vec3 left_aim_pos{};
    Quat left_aim_rot{0, 0, 0, 1};
    Vec3 right_aim_pos{};
    Quat right_aim_rot{0, 0, 0, 1};

    // Unified 6DoF state in the OpenXR LOCAL reference space.
    VrcPoseState head{};
    VrcPoseState grip[2]{};
    VrcPoseState aim[2]{};
    VrcPoseState trackers[kVrcTrackerSlotCount]{};
    VrcPoseState hand_joints[2][kVrcHandJointCount]{};

    u64 frame_id = 0;
    u64 timestamp_ns = 0;

    // Legacy aggregate tracked flags (either grip or aim valid) kept so
    // existing callers that check "controller present" keep working.
    bool left_tracked  = false;
    bool right_tracked = false;
};

class XrInput {
public:
    static XrInput& instance();

    bool initialize(XrInstance xr_instance, XrSession session);
    void shutdown();

    // Called by OpenXRContext after xrLocateViews so the bridge can expose the
    // head-center pose. xrLocateViews returns per-eye poses, so both are passed
    // in and averaged rather than publishing one eye's lateral offset.
    void set_head_pose_from_views(const XrPosef& left, const XrPosef& right,
                                  XrSpaceLocationFlags flags);

    // Allows extensions discovered at instance creation to be used by input.
    // Per-frame timing/projection data from the XR frame thread. Publishing it
    // through the bridge lets game-side mods use the runtime's real IPD, frustum
    // and display period instead of guessing from the config profile.
    void set_frame_metrics(XrDuration predicted_display_period,
                           const XrView* views, u32 view_count,
                           XrSpaceLocationFlags view_flags);

    void configure_extensions(bool htcx_tracker_supported,
                              bool hand_tracking_supported) {
        htcx_tracker_supported_ = htcx_tracker_supported;
        hand_tracking_supported_ = hand_tracking_supported;
    }

    // Call once per frame from the OpenXR frame thread, after xrBeginFrame
    void sync(XrSession session, XrSpace ref_space, XrTime predicted_time);

    const XrControllerState& state() const { return state_; }
    bool is_active() const { return active_; }
    bool trackers_active() const { return trackers_enabled_; }

    void trigger_haptic(XrSession session, bool left, f32 amplitude, f32 duration_s);

private:
    XrInput() = default;

    bool create_action_set(XrInstance xr_instance);
    bool suggest_bindings(XrInstance xr_instance);
    bool attach(XrSession session);
    void sync_hand_joints(XrSpace ref_space, XrTime predicted_time);
    void publish_bridge() const;

    XrInstance instance_ = XR_NULL_HANDLE;
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
    // Boolean fallbacks for profiles without analog squeeze/trigger bindings.
    XrAction a_grip_click_left_         = XR_NULL_HANDLE;
    XrAction a_grip_click_right_        = XR_NULL_HANDLE;
    XrAction a_select_left_             = XR_NULL_HANDLE;
    XrAction a_select_right_            = XR_NULL_HANDLE;

    // Pose actions
    XrAction a_grip_pose_left_          = XR_NULL_HANDLE;
    XrAction a_grip_pose_right_         = XR_NULL_HANDLE;
    XrAction a_aim_pose_left_           = XR_NULL_HANDLE;
    XrAction a_aim_pose_right_          = XR_NULL_HANDLE;
    XrAction a_tracker_pose_            = XR_NULL_HANDLE;

    // Haptic output
    XrAction a_haptic_left_             = XR_NULL_HANDLE;
    XrAction a_haptic_right_            = XR_NULL_HANDLE;

    // Action spaces for pose queries
    XrSpace space_grip_left_   = XR_NULL_HANDLE;
    XrSpace space_grip_right_  = XR_NULL_HANDLE;
    XrSpace space_aim_left_    = XR_NULL_HANDLE;
    XrSpace space_aim_right_   = XR_NULL_HANDLE;
    std::array<XrSpace, kVrcTrackerSlotCount> space_tracker_{};
    std::array<XrPath, kVrcTrackerSlotCount> tracker_paths_{};
    std::array<std::string, kVrcTrackerSlotCount> tracker_role_names_{};

    bool htcx_tracker_supported_ = false;
    bool hand_tracking_supported_ = false;
    bool hand_tracking_active_ = false;
    bool trackers_enabled_ = false;
    bool bridge_enabled_ = false;
    u32 reference_space_type_ = VrcReferenceSpace_Local;

    XrControllerState state_{};
    bool active_ = false;

    // Frame metrics published alongside controller poses.
    f32 display_period_s_ = 0.0f;
    f32 ipd_ = 0.0f;
    f32 projection_tangents_[2][4] = {};

    // XR_EXT_hand_tracking is optional; function pointers are resolved at
    // initialize() and all calls stay on the XR frame thread.
    PFN_xrCreateHandTrackerEXT create_hand_tracker_ = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy_hand_tracker_ = nullptr;
    PFN_xrLocateHandJointsEXT locate_hand_joints_ = nullptr;
    XrHandTrackerEXT hand_tracker_[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
};

} // namespace vrc
