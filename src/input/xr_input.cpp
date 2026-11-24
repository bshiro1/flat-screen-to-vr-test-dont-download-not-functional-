#include "xr_input.h"
#include "core/config.h"
#include "core/logging.h"
#include "input/xr_shared_state.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vrc {

// ─── Helpers ────────────────────────────────────────────────────────────────

static bool xr_ok(XrResult r, const char* tag) {
    if (XR_FAILED(r)) {
        Log::warn("XrInput {}: result={}", tag, static_cast<int>(r));
        return false;
    }
    return true;
}

static XrPath mk_path(XrInstance inst, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

static bool pose_valid(const VrcPoseState& pose) {
    constexpr u32 kValid = VrcPoseFlag_PositionValid | VrcPoseFlag_OrientationValid;
    return (pose.flags & kValid) == kValid;
}

static void copy_pose_to_legacy(const VrcPoseState& src, Vec3& pos, Quat& rot) {
    pos.x = src.position[0];
    pos.y = src.position[1];
    pos.z = src.position[2];
    rot.x = src.rotation[0];
    rot.y = src.rotation[1];
    rot.z = src.rotation[2];
    rot.w = src.rotation[3];
}

// ─── Singleton ──────────────────────────────────────────────────────────────

XrInput& XrInput::instance() {
    static XrInput inp;
    return inp;
}

bool XrInput::initialize(XrInstance xr_instance, XrSession session) {
    if (active_) return true;
    instance_ = xr_instance;

    // A previous session may have published hand joints or frame metrics that
    // the next runtime does not support. Never carry that stale state forward.
    state_ = XrControllerState{};
    hand_tracking_active_ = false;
    display_period_s_ = 0.0f;
    ipd_ = 0.0f;
    for (auto& eye : projection_tangents_) {
        for (auto& tangent : eye) tangent = 0.0f;
    }

    const auto profile = Config::instance().current_profile_copy();
    bridge_enabled_ = profile.injection_method == InjectionMethod::SixDofBridge;
    trackers_enabled_ = profile.fbt_enabled && htcx_tracker_supported_;

    // Must match VrcTrackerSlot in xr_shared_state.h:
    // hips, chest, left foot, right foot, left shoulder, right shoulder.
    tracker_role_names_ = {
        profile.fbt_hips_role,
        profile.fbt_chest_role,
        profile.fbt_left_foot_role,
        profile.fbt_right_foot_role,
        profile.fbt_left_shoulder_role,
        profile.fbt_right_shoulder_role,
    };

    if (profile.fbt_enabled && !htcx_tracker_supported_) {
        Log::warn("FBT requested but XR_HTCX_vive_tracker_interaction is not available");
    }

    if (!create_action_set(xr_instance)) return false;
    suggest_bindings(xr_instance);      // best-effort; missing profiles are ignored
    if (!attach(session)) return false;

    if (hand_tracking_supported_) {
        xrGetInstanceProcAddr(instance_, "xrCreateHandTrackerEXT",
            reinterpret_cast<PFN_xrVoidFunction*>(&create_hand_tracker_));
        xrGetInstanceProcAddr(instance_, "xrDestroyHandTrackerEXT",
            reinterpret_cast<PFN_xrVoidFunction*>(&destroy_hand_tracker_));
        xrGetInstanceProcAddr(instance_, "xrLocateHandJointsEXT",
            reinterpret_cast<PFN_xrVoidFunction*>(&locate_hand_joints_));

        if (create_hand_tracker_ && destroy_hand_tracker_ && locate_hand_joints_) {
            for (u32 hand = 0; hand < 2; ++hand) {
                XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
                info.hand = (hand == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
                info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                if (!xr_ok(create_hand_tracker_(session, &info, &hand_tracker_[hand]),
                           "CreateHandTrackerEXT")) {
                    hand_tracker_[hand] = XR_NULL_HANDLE;
                }
            }
        } else {
            Log::warn("XR_EXT_hand_tracking enabled but entry points are missing");
        }
    }

    active_ = true;

    Log::info("XrInput: action set attached (4 controller profiles{}), bridge={}, FBT={}, hands={}",
              trackers_enabled_ ? " + HTCX trackers" : "",
              bridge_enabled_ ? "on" : "off",
              trackers_enabled_ ? "on" : "off",
              (hand_tracker_[0] || hand_tracker_[1]) ? "on" : "off");
    return true;
}

void XrInput::shutdown() {
    for (auto& tracker : hand_tracker_) {
        if (tracker && destroy_hand_tracker_) {
            destroy_hand_tracker_(tracker);
        }
        tracker = XR_NULL_HANDLE;
    }
    create_hand_tracker_ = nullptr;
    destroy_hand_tracker_ = nullptr;
    locate_hand_joints_ = nullptr;

    for (auto& space : space_tracker_) {
        if (space) xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    if (space_grip_left_)  xrDestroySpace(space_grip_left_);
    if (space_grip_right_) xrDestroySpace(space_grip_right_);
    if (space_aim_left_)   xrDestroySpace(space_aim_left_);
    if (space_aim_right_)  xrDestroySpace(space_aim_right_);
    if (action_set_)       xrDestroyActionSet(action_set_);
    space_grip_left_ = space_grip_right_ = space_aim_left_ = space_aim_right_ = XR_NULL_HANDLE;
    action_set_ = XR_NULL_HANDLE;
    a_tracker_pose_ = XR_NULL_HANDLE;
    tracker_paths_.fill(XR_NULL_PATH);
    active_ = false;

    state_ = XrControllerState{};
    hand_tracking_active_ = false;
    display_period_s_ = 0.0f;
    ipd_ = 0.0f;
    for (auto& eye : projection_tangents_) {
        for (auto& tangent : eye) tangent = 0.0f;
    }
}

void XrInput::set_head_pose_from_views(const XrPosef& left, const XrPosef& right,
                                       XrSpaceLocationFlags flags) {
    // Reset first: invalid tracking must not leave last frame's pose published.
    state_.head = VrcPoseState{};
    auto& head = state_.head;

    if (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        head.flags |= VrcPoseFlag_PositionValid;
        // xrLocateViews returns eye poses; the bridge wants head center.
        head.position[0] = (left.position.x + right.position.x) * 0.5f;
        head.position[1] = (left.position.y + right.position.y) * 0.5f;
        head.position[2] = (left.position.z + right.position.z) * 0.5f;
    }
    if (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
        head.flags |= VrcPoseFlag_OrientationValid;

        // Average the two eye orientations with hemisphere correction so q and
        // -q do not cancel out. Most runtimes return identical eye rotations.
        f32 q[4] = {
            left.orientation.x + right.orientation.x,
            left.orientation.y + right.orientation.y,
            left.orientation.z + right.orientation.z,
            left.orientation.w + right.orientation.w,
        };
        const f32 dot =
            left.orientation.x * right.orientation.x +
            left.orientation.y * right.orientation.y +
            left.orientation.z * right.orientation.z +
            left.orientation.w * right.orientation.w;
        if (dot < 0.0f) {
            q[0] = left.orientation.x - right.orientation.x;
            q[1] = left.orientation.y - right.orientation.y;
            q[2] = left.orientation.z - right.orientation.z;
            q[3] = left.orientation.w - right.orientation.w;
        }
        const f32 len = std::sqrt(q[0] * q[0] + q[1] * q[1] +
                                  q[2] * q[2] + q[3] * q[3]);
        if (len > 1e-6f) {
            head.rotation[0] = q[0] / len;
            head.rotation[1] = q[1] / len;
            head.rotation[2] = q[2] / len;
            head.rotation[3] = q[3] / len;
        } else {
            head.rotation[0] = left.orientation.x;
            head.rotation[1] = left.orientation.y;
            head.rotation[2] = left.orientation.z;
            head.rotation[3] = left.orientation.w;
        }
    }
    if (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
        head.flags |= VrcPoseFlag_PositionTracked;
    if (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
        head.flags |= VrcPoseFlag_OrientationTracked;
}

void XrInput::set_frame_metrics(XrDuration predicted_display_period,
                                const XrView* views, u32 view_count,
                                XrSpaceLocationFlags view_flags) {
    display_period_s_ = predicted_display_period > 0
        ? static_cast<f32>(static_cast<f64>(predicted_display_period) / 1'000'000'000.0)
        : 0.0f;

    // Reset every frame: a runtime that loses tracking must not leave stale
    // projection/IPD values in the bridge.
    ipd_ = 0.0f;
    for (auto& eye : projection_tangents_) {
        for (auto& tangent : eye) tangent = 0.0f;
    }
    if (!views || view_count < 2) return;

    for (u32 eye = 0; eye < 2; ++eye) {
        projection_tangents_[eye][VrcProjection_Left]  = std::tan(views[eye].fov.angleLeft);
        projection_tangents_[eye][VrcProjection_Right] = std::tan(views[eye].fov.angleRight);
        projection_tangents_[eye][VrcProjection_Up]    = std::tan(views[eye].fov.angleUp);
        projection_tangents_[eye][VrcProjection_Down]  = std::tan(views[eye].fov.angleDown);
    }

    if (view_flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        const XrVector3f& l = views[0].pose.position;
        const XrVector3f& r = views[1].pose.position;
        const f64 dx = static_cast<f64>(l.x) - r.x;
        const f64 dy = static_cast<f64>(l.y) - r.y;
        const f64 dz = static_cast<f64>(l.z) - r.z;
        const f64 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > 0.001 && distance < 0.2) ipd_ = static_cast<f32>(distance);
    }
}

// ─── Action Set Creation ─────────────────────────────────────────────────────

bool XrInput::create_action_set(XrInstance xr_instance) {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy_s(set_info.actionSetName,
              "vrc_input", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy_s(set_info.localizedActionSetName,
              "VRC Input", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    set_info.priority = 0;
    if (!xr_ok(xrCreateActionSet(xr_instance, &set_info, &action_set_),
               "CreateActionSet"))
        return false;

    auto make = [&](XrActionType t, const char* n, const char* loc,
                    XrAction* out) -> bool {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strncpy_s(info.actionName,
                  n, XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy_s(info.localizedActionName,
                  loc, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.actionType = t;
        info.countSubactionPaths = 0;
        return xr_ok(xrCreateAction(action_set_, &info, out), n);
    };

    using T = XrActionType;
    bool ok =
        make(T::XR_ACTION_TYPE_FLOAT_INPUT,    "trigger_left",           "Left Trigger",        &a_trigger_left_)           &&
        make(T::XR_ACTION_TYPE_FLOAT_INPUT,    "trigger_right",          "Right Trigger",       &a_trigger_right_)          &&
        make(T::XR_ACTION_TYPE_FLOAT_INPUT,    "grip_left",              "Left Grip",           &a_grip_left_)              &&
        make(T::XR_ACTION_TYPE_FLOAT_INPUT,    "grip_right",             "Right Grip",          &a_grip_right_)             &&
        make(T::XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_left",        "Left Thumbstick",     &a_thumbstick_left_)        &&
        make(T::XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick_right",       "Right Thumbstick",    &a_thumbstick_right_)       &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "button_a",               "A Button",            &a_button_a_)               &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "button_b",               "B Button",            &a_button_b_)               &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "button_x",               "X Button",            &a_button_x_)               &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "button_y",               "Y Button",            &a_button_y_)               &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu",                   "Menu",                &a_menu_)                   &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "thumbstick_click_l",     "Left Stick Click",    &a_thumbstick_click_left_)  &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "thumbstick_click_r",     "Right Stick Click",   &a_thumbstick_click_right_) &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "grip_click_left",        "Left Grip Click",     &a_grip_click_left_)        &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "grip_click_right",       "Right Grip Click",    &a_grip_click_right_)       &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "select_left",            "Left Select",         &a_select_left_)            &&
        make(T::XR_ACTION_TYPE_BOOLEAN_INPUT,  "select_right",           "Right Select",        &a_select_right_)           &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "grip_pose_left",         "Left Grip Pose",      &a_grip_pose_left_)         &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "grip_pose_right",        "Right Grip Pose",     &a_grip_pose_right_)        &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "aim_pose_left",          "Left Aim Pose",       &a_aim_pose_left_)          &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "aim_pose_right",         "Right Aim Pose",      &a_aim_pose_right_)         &&
        make(T::XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_left",          "Left Haptic",         &a_haptic_left_)            &&
        make(T::XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_right",         "Right Haptic",        &a_haptic_right_);
    if (!ok) return false;

    if (!trackers_enabled_) return true;

    // HTCX trackers are addressed by subaction path (role), so one pose action
    // serves all six body slots. xrStringToPath accepts these paths on any
    // runtime; bindings only arrive when the HTCX profile is supported.
    for (u32 i = 0; i < kVrcTrackerSlotCount; ++i) {
        tracker_paths_[i] = mk_path(xr_instance, tracker_role_names_[i].c_str());
        if (tracker_paths_[i] == XR_NULL_PATH) {
            Log::warn("XrInput: invalid FBT role path '{}'", tracker_role_names_[i].c_str());
            trackers_enabled_ = false;
            return true;
        }
    }

    XrActionCreateInfo tracker_info{XR_TYPE_ACTION_CREATE_INFO};
    strncpy_s(tracker_info.actionName, "body_tracker_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy_s(tracker_info.localizedActionName, "Body Tracker Pose",
              XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    tracker_info.actionType = T::XR_ACTION_TYPE_POSE_INPUT;
    tracker_info.countSubactionPaths = kVrcTrackerSlotCount;
    tracker_info.subactionPaths = tracker_paths_.data();
    if (!xr_ok(xrCreateAction(action_set_, &tracker_info, &a_tracker_pose_),
               "CreateBodyTrackerPose")) {
        trackers_enabled_ = false;
    }
    return true;
}

// ─── Interaction Profile Suggestions ────────────────────────────────────────

bool XrInput::suggest_bindings(XrInstance inst) {
    // Suggest for each profile; failures are non-fatal (runtime picks what it supports)
    struct BP { XrAction* action; const char* path; };

    auto suggest = [&](const char* profile, std::initializer_list<BP> pairs) {
        XrPath prof = mk_path(inst, profile);
        if (prof == XR_NULL_PATH) return;
        std::vector<XrActionSuggestedBinding> bindings;
        bindings.reserve(pairs.size());
        for (auto& p : pairs) {
            XrPath xp = mk_path(inst, p.path);
            if (xp != XR_NULL_PATH)
                bindings.push_back({*p.action, xp});
        }
        XrInteractionProfileSuggestedBinding info{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = prof;
        info.suggestedBindings = bindings.data();
        info.countSuggestedBindings = static_cast<u32>(bindings.size());
        const XrResult res = xrSuggestInteractionProfileBindings(inst, &info);
        if (XR_FAILED(res)) {
            Log::warn("XrInput: binding suggestion failed for {}: {}",
                      profile, static_cast<int>(res));
        }
    };

    // ── Oculus / Meta Touch ──────────────────────────────────────────────
    suggest("/interaction_profiles/oculus/touch_controller", {
        {&a_trigger_left_,           "/user/hand/left/input/trigger/value"},
        {&a_trigger_right_,          "/user/hand/right/input/trigger/value"},
        {&a_grip_left_,              "/user/hand/left/input/squeeze/value"},
        {&a_grip_right_,             "/user/hand/right/input/squeeze/value"},
        {&a_thumbstick_left_,        "/user/hand/left/input/thumbstick"},
        {&a_thumbstick_right_,       "/user/hand/right/input/thumbstick"},
        {&a_button_a_,               "/user/hand/right/input/a/click"},
        {&a_button_b_,               "/user/hand/right/input/b/click"},
        {&a_button_x_,               "/user/hand/left/input/x/click"},
        {&a_button_y_,               "/user/hand/left/input/y/click"},
        {&a_menu_,                   "/user/hand/left/input/menu/click"},
        {&a_thumbstick_click_left_,  "/user/hand/left/input/thumbstick/click"},
        {&a_thumbstick_click_right_, "/user/hand/right/input/thumbstick/click"},
        {&a_grip_pose_left_,         "/user/hand/left/input/grip/pose"},
        {&a_grip_pose_right_,        "/user/hand/right/input/grip/pose"},
        {&a_aim_pose_left_,          "/user/hand/left/input/aim/pose"},
        {&a_aim_pose_right_,         "/user/hand/right/input/aim/pose"},
        {&a_haptic_left_,            "/user/hand/left/output/haptic"},
        {&a_haptic_right_,           "/user/hand/right/output/haptic"},
    });

    // ── Valve Index / Knuckles ───────────────────────────────────────────
    suggest("/interaction_profiles/valve/index_controller", {
        {&a_trigger_left_,           "/user/hand/left/input/trigger/value"},
        {&a_trigger_right_,          "/user/hand/right/input/trigger/value"},
        {&a_grip_left_,              "/user/hand/left/input/squeeze/value"},
        {&a_grip_right_,             "/user/hand/right/input/squeeze/value"},
        {&a_thumbstick_left_,        "/user/hand/left/input/thumbstick"},
        {&a_thumbstick_right_,       "/user/hand/right/input/thumbstick"},
        {&a_button_a_,               "/user/hand/right/input/a/click"},
        {&a_button_b_,               "/user/hand/right/input/b/click"},
        {&a_thumbstick_click_left_,  "/user/hand/left/input/thumbstick/click"},
        {&a_thumbstick_click_right_, "/user/hand/right/input/thumbstick/click"},
        {&a_grip_pose_left_,         "/user/hand/left/input/grip/pose"},
        {&a_grip_pose_right_,        "/user/hand/right/input/grip/pose"},
        {&a_aim_pose_left_,          "/user/hand/left/input/aim/pose"},
        {&a_aim_pose_right_,         "/user/hand/right/input/aim/pose"},
        {&a_haptic_left_,            "/user/hand/left/output/haptic"},
        {&a_haptic_right_,           "/user/hand/right/output/haptic"},
    });

    // ── Pimax (uses Oculus Touch bindings via compatibility layer) ───────
    suggest("/interaction_profiles/htc/vive_controller", {
        {&a_trigger_left_,    "/user/hand/left/input/trigger/value"},
        {&a_trigger_right_,   "/user/hand/right/input/trigger/value"},
        {&a_grip_click_left_,  "/user/hand/left/input/squeeze/click"},
        {&a_grip_click_right_, "/user/hand/right/input/squeeze/click"},
        {&a_menu_,            "/user/hand/left/input/menu/click"},
        {&a_grip_pose_left_,  "/user/hand/left/input/grip/pose"},
        {&a_grip_pose_right_, "/user/hand/right/input/grip/pose"},
        {&a_aim_pose_left_,   "/user/hand/left/input/aim/pose"},
        {&a_aim_pose_right_,  "/user/hand/right/input/aim/pose"},
        {&a_haptic_left_,     "/user/hand/left/output/haptic"},
        {&a_haptic_right_,    "/user/hand/right/output/haptic"},
    });

    // ── KHR simple controller (universal fallback) ───────────────────────
    suggest("/interaction_profiles/khr/simple_controller", {
        {&a_select_left_,     "/user/hand/left/input/select/click"},
        {&a_select_right_,    "/user/hand/right/input/select/click"},
        {&a_menu_,            "/user/hand/left/input/menu/click"},
        {&a_grip_pose_left_,  "/user/hand/left/input/grip/pose"},
        {&a_grip_pose_right_, "/user/hand/right/input/grip/pose"},
        {&a_aim_pose_left_,   "/user/hand/left/input/aim/pose"},
        {&a_aim_pose_right_,  "/user/hand/right/input/aim/pose"},
        {&a_haptic_left_,     "/user/hand/left/output/haptic"},
        {&a_haptic_right_,    "/user/hand/right/output/haptic"},
    });

    // ── HTCX Vive trackers (FBT) ─────────────────────────────────────────
    if (trackers_enabled_ && a_tracker_pose_) {
        std::vector<XrActionSuggestedBinding> bindings;
        bindings.reserve(kVrcTrackerSlotCount);
        for (const auto& role : tracker_role_names_) {
            XrPath path = mk_path(inst, (role + "/input/grip/pose").c_str());
            if (path != XR_NULL_PATH) {
                bindings.push_back({a_tracker_pose_, path});
            }
        }
        XrInteractionProfileSuggestedBinding info{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile =
            mk_path(inst, "/interaction_profiles/htc/vive_tracker_htcx");
        info.suggestedBindings = bindings.data();
        info.countSuggestedBindings = static_cast<u32>(bindings.size());
        xr_ok(xrSuggestInteractionProfileBindings(inst, &info), "SuggestHTCX");
    }

    return true;
}

// ─── Attach ──────────────────────────────────────────────────────────────────

bool XrInput::attach(XrSession session) {
    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.countActionSets = 1;
    attach_info.actionSets = &action_set_;
    if (!xr_ok(xrAttachSessionActionSets(session, &attach_info), "AttachActionSets"))
        return false;

    auto make_space = [&](XrAction action, XrSpace* out) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.action = action;
        info.subactionPath = XR_NULL_PATH;
        info.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        xr_ok(xrCreateActionSpace(session, &info, out), "CreateActionSpace");
    };

    make_space(a_grip_pose_left_,  &space_grip_left_);
    make_space(a_grip_pose_right_, &space_grip_right_);
    make_space(a_aim_pose_left_,   &space_aim_left_);
    make_space(a_aim_pose_right_,  &space_aim_right_);

    if (trackers_enabled_ && a_tracker_pose_) {
        for (u32 i = 0; i < kVrcTrackerSlotCount; ++i) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = a_tracker_pose_;
            info.subactionPath = tracker_paths_[i];
            info.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
            if (!xr_ok(xrCreateActionSpace(session, &info, &space_tracker_[i]),
                       "CreateBodyTrackerSpace")) {
                space_tracker_[i] = XR_NULL_HANDLE;
            }
        }
    }
    return true;
}

// ─── Frame Sync ──────────────────────────────────────────────────────────────

void XrInput::sync(XrSession session, XrSpace ref_space, XrTime predicted_time) {
    if (!active_) return;

    // Apply queued game-thread haptics even if action syncing fails this frame.
    // This thread owns the session and all OpenXR calls.
    XRStateBridge::instance().drain_haptics([&](const auto& request) {
        trigger_haptic(session, request.left, request.amplitude, request.duration_s);
    });

    XrActiveActionSet active_set{action_set_, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_set;
    if (XR_FAILED(xrSyncActions(session, &sync_info))) return;

    auto get_f32 = [&](XrAction action, f32& out) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = XR_NULL_PATH;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &s)) && s.isActive)
            out = s.currentState;
        else
            out = 0.0f; // controller tracked-out / off -> release (no stuck trigger)
    };
    auto get_bool = [&](XrAction action, bool& out) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = XR_NULL_PATH;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &s)) && s.isActive)
            out = s.currentState != XR_FALSE;
        else
            out = false;
    };
    auto get_vec2 = [&](XrAction action, Vec2& out) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = XR_NULL_PATH;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &info, &s)) && s.isActive) {
            out.x = s.currentState.x;
            out.y = s.currentState.y;
        } else {
            out.x = 0.0f;
            out.y = 0.0f;
        }
    };
    auto get_pose_state = [&](XrSpace space, VrcPoseState& out) {
        // Always clear: invalid/untracked poses must not reuse last frame's
        // position, rotation, flags, or velocities.
        out = VrcPoseState{};
        if (!space) return;

        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        loc.next = &velocity;
        if (XR_FAILED(xrLocateSpace(space, ref_space, predicted_time, &loc))) return;

        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            out.flags |= VrcPoseFlag_PositionValid;
        if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
            out.flags |= VrcPoseFlag_OrientationValid;
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
            out.flags |= VrcPoseFlag_PositionTracked;
        if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
            out.flags |= VrcPoseFlag_OrientationTracked;

        if (out.flags & VrcPoseFlag_PositionValid) {
            out.position[0] = loc.pose.position.x;
            out.position[1] = loc.pose.position.y;
            out.position[2] = loc.pose.position.z;
        }
        if (out.flags & VrcPoseFlag_OrientationValid) {
            out.rotation[0] = loc.pose.orientation.x;
            out.rotation[1] = loc.pose.orientation.y;
            out.rotation[2] = loc.pose.orientation.z;
            out.rotation[3] = loc.pose.orientation.w;
        }
        if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
            out.linear_velocity[0] = velocity.linearVelocity.x;
            out.linear_velocity[1] = velocity.linearVelocity.y;
            out.linear_velocity[2] = velocity.linearVelocity.z;
        }
        if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
            out.angular_velocity[0] = velocity.angularVelocity.x;
            out.angular_velocity[1] = velocity.angularVelocity.y;
            out.angular_velocity[2] = velocity.angularVelocity.z;
        }
    };

    get_f32 (a_trigger_left_,           state_.trigger_left);
    get_f32 (a_trigger_right_,          state_.trigger_right);
    get_f32 (a_grip_left_,              state_.grip_left);
    get_f32 (a_grip_right_,             state_.grip_right);
    get_vec2(a_thumbstick_left_,        state_.thumbstick_left);
    get_vec2(a_thumbstick_right_,       state_.thumbstick_right);
    get_bool(a_button_a_,               state_.button_a);
    get_bool(a_button_b_,               state_.button_b);
    get_bool(a_button_x_,               state_.button_x);
    get_bool(a_button_y_,               state_.button_y);
    get_bool(a_menu_,                   state_.menu);
    get_bool(a_thumbstick_click_left_,  state_.thumbstick_click_left);
    get_bool(a_thumbstick_click_right_, state_.thumbstick_click_right);
    // Boolean fallbacks for Vive squeeze/click and KHR select/click. Profiles
    // with analog squeeze/trigger simply leave these actions inactive.
    get_bool(a_grip_click_left_,  state_.grip_click_left);
    get_bool(a_grip_click_right_, state_.grip_click_right);
    get_bool(a_select_left_,      state_.trigger_click_left);
    get_bool(a_select_right_,     state_.trigger_click_right);

    // Combine analog thresholds with the boolean fallback actions.
    state_.trigger_click_left  = state_.trigger_click_left  || state_.trigger_left  > 0.7f;
    state_.trigger_click_right = state_.trigger_click_right || state_.trigger_right > 0.7f;
    state_.grip_click_left     = state_.grip_click_left     || state_.grip_left     > 0.7f;
    state_.grip_click_right    = state_.grip_click_right    || state_.grip_right    > 0.7f;

    // Expose a usable 0/1 axis to consumers when only a click binding exists.
    if (state_.trigger_click_left  && state_.trigger_left  <= 0.0f) state_.trigger_left  = 1.0f;
    if (state_.trigger_click_right && state_.trigger_right <= 0.0f) state_.trigger_right = 1.0f;
    if (state_.grip_click_left     && state_.grip_left     <= 0.0f) state_.grip_left     = 1.0f;
    if (state_.grip_click_right    && state_.grip_right    <= 0.0f) state_.grip_right    = 1.0f;

    // Grip and aim each need their own tracked flag. Sharing one bool used to
    // let whichever xrLocateSpace call ran last overwrite the other's validity.
    get_pose_state(space_grip_left_,  state_.grip[0]);
    get_pose_state(space_grip_right_, state_.grip[1]);
    get_pose_state(space_aim_left_,   state_.aim[0]);
    get_pose_state(space_aim_right_,  state_.aim[1]);

    copy_pose_to_legacy(state_.grip[0], state_.left_grip_pos,  state_.left_grip_rot);
    copy_pose_to_legacy(state_.grip[1], state_.right_grip_pos, state_.right_grip_rot);
    copy_pose_to_legacy(state_.aim[0],  state_.left_aim_pos,   state_.left_aim_rot);
    copy_pose_to_legacy(state_.aim[1],  state_.right_aim_pos,  state_.right_aim_rot);

    state_.left_grip_tracked  = pose_valid(state_.grip[0]);
    state_.right_grip_tracked = pose_valid(state_.grip[1]);
    state_.left_aim_tracked   = pose_valid(state_.aim[0]);
    state_.right_aim_tracked  = pose_valid(state_.aim[1]);
    state_.left_tracked  = state_.left_grip_tracked  || state_.left_aim_tracked;
    state_.right_tracked = state_.right_grip_tracked || state_.right_aim_tracked;

    if (trackers_enabled_) {
        for (u32 i = 0; i < kVrcTrackerSlotCount; ++i) {
            get_pose_state(space_tracker_[i], state_.trackers[i]);
        }
    } else {
        for (auto& tracker : state_.trackers) tracker = VrcPoseState{};
    }

    sync_hand_joints(ref_space, predicted_time);

    state_.frame_id++;
    state_.timestamp_ns = static_cast<u64>(predicted_time);
    publish_bridge();
}

void XrInput::sync_hand_joints(XrSpace ref_space, XrTime predicted_time) {
    if (!locate_hand_joints_) return;

    bool any_active = false;
    for (u32 hand = 0; hand < 2; ++hand) {
        // Clear every slot before locating so lost hands never publish stale
        // finger poses to game-side mods.
        for (u32 joint = 0; joint < kVrcHandJointCount; ++joint) {
            state_.hand_joints[hand][joint] = VrcPoseState{};
        }
        if (!hand_tracker_[hand]) continue;

        XrHandJointsLocateInfoEXT locate{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        locate.baseSpace = ref_space;
        locate.time = predicted_time;

        std::array<XrHandJointLocationEXT, kVrcHandJointCount> joints{};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = static_cast<u32>(joints.size());
        locations.jointLocations = joints.data();

        if (XR_FAILED(locate_hand_joints_(hand_tracker_[hand], &locate, &locations)) ||
            locations.isActive == XR_FALSE) {
            continue;
        }
        any_active = true;

        const u32 count = std::min<u32>(
            locations.jointCount, static_cast<u32>(joints.size()));
        for (u32 joint = 0; joint < count; ++joint) {
            const XrHandJointLocationEXT& src = joints[joint];
            VrcPoseState& out = state_.hand_joints[hand][joint];
            out.radius = src.radius;
            if (src.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                out.flags |= VrcPoseFlag_PositionValid;
                out.position[0] = src.pose.position.x;
                out.position[1] = src.pose.position.y;
                out.position[2] = src.pose.position.z;
            }
            if (src.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
                out.flags |= VrcPoseFlag_OrientationValid;
                out.rotation[0] = src.pose.orientation.x;
                out.rotation[1] = src.pose.orientation.y;
                out.rotation[2] = src.pose.orientation.z;
                out.rotation[3] = src.pose.orientation.w;
            }
            if (src.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) {
                out.flags |= VrcPoseFlag_PositionTracked;
            }
            if (src.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) {
                out.flags |= VrcPoseFlag_OrientationTracked;
            }
        }
    }
    hand_tracking_active_ = any_active;
}

void XrInput::publish_bridge() const {
    if (!bridge_enabled_) return;

    VrcXRState out{};
    out.magic = kVrcXRStateMagic;
    out.version = kVrcXRStateVersion;
    out.size_bytes = sizeof(VrcXRState);
    out.sequence = static_cast<u32>(state_.frame_id);
    out.timestamp_ns = state_.timestamp_ns;
    out.reference_space = reference_space_type_;
    const u32 effective_mode = XRStateBridge::instance().injection_method();
    out.injection_method = effective_mode != 0
        ? effective_mode
        : static_cast<u32>(InjectionMethod::SixDofBridge);
    out.fbt_enabled = trackers_enabled_ ? 1u : 0u;
    out.world_scale = Config::instance().current_profile_copy().world_scale;
    out.display_period_s = display_period_s_;
    out.ipd = ipd_ > 0.0f ? ipd_ : Config::instance().current_profile_copy().ipd;
    for (u32 eye = 0; eye < 2; ++eye) {
        for (u32 tangent = 0; tangent < 4; ++tangent) {
            out.projection_tangents[eye][tangent] = projection_tangents_[eye][tangent];
        }
    }

    u32 state_flags = 0;
    if (pose_valid(state_.head)) state_flags |= VrcState_HeadTracked;
    if (pose_valid(state_.grip[0])) state_flags |= VrcState_LeftGripTracked;
    if (pose_valid(state_.grip[1])) state_flags |= VrcState_RightGripTracked;
    if (pose_valid(state_.aim[0])) state_flags |= VrcState_LeftAimTracked;
    if (pose_valid(state_.aim[1])) state_flags |= VrcState_RightAimTracked;
    if (trackers_enabled_) state_flags |= VrcState_FbtEnabled;
    if (hand_tracking_active_) state_flags |= VrcState_HandTrackingActive;
    if (a_haptic_left_ && a_haptic_right_) state_flags |= VrcState_HapticsAvailable;
    out.flags = state_flags;

    out.head = state_.head;
    for (u32 i = 0; i < 2; ++i) {
        out.grip[i] = state_.grip[i];
        out.aim[i] = state_.aim[i];
        // Approximate controller/aim sphere radius in metres for consumers
        // doing near-field overlap tests rather than exact mesh traces.
        out.grip[i].radius = 0.04f;
        out.aim[i].radius = 0.02f;
        out.trigger[i] = (i == 0) ? state_.trigger_left : state_.trigger_right;
        out.grip_axis[i] = (i == 0) ? state_.grip_left : state_.grip_right;
        out.thumbstick[i][0] = (i == 0) ? state_.thumbstick_left.x : state_.thumbstick_right.x;
        out.thumbstick[i][1] = (i == 0) ? state_.thumbstick_left.y : state_.thumbstick_right.y;
        for (u32 j = 0; j < kVrcHandJointCount; ++j) {
            out.hand_joints[i][j] = state_.hand_joints[i][j];
        }
    }
    for (u32 i = 0; i < kVrcTrackerSlotCount; ++i) {
        out.trackers[i] = state_.trackers[i];
        out.trackers[i].radius = 0.05f;
    }

    u32 buttons = 0;
    if (state_.button_a) buttons |= VrcButton_A;
    if (state_.button_b) buttons |= VrcButton_B;
    if (state_.button_x) buttons |= VrcButton_X;
    if (state_.button_y) buttons |= VrcButton_Y;
    if (state_.menu) buttons |= VrcButton_Menu;
    if (state_.thumbstick_click_left) buttons |= VrcButton_StickClickLeft;
    if (state_.thumbstick_click_right) buttons |= VrcButton_StickClickRight;
    if (state_.trigger_click_left) buttons |= VrcButton_TriggerClickLeft;
    if (state_.trigger_click_right) buttons |= VrcButton_TriggerClickRight;
    if (state_.grip_click_left) buttons |= VrcButton_GripClickLeft;
    if (state_.grip_click_right) buttons |= VrcButton_GripClickRight;
    out.button_flags = buttons;

    XRStateBridge::instance().publish(out);
}

// ─── Haptics ─────────────────────────────────────────────────────────────────

void XrInput::trigger_haptic(XrSession session, bool left,
                                f32 amplitude, f32 duration_s) {
    if (!active_ || session == XR_NULL_HANDLE) return;
    XrAction action = left ? a_haptic_left_ : a_haptic_right_;
    if (action == XR_NULL_HANDLE) return;
    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = amplitude;
    vib.duration  = static_cast<XrDuration>(duration_s * 1'000'000'000.0f);
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action        = action;
    info.subactionPath = XR_NULL_PATH;
    xrApplyHapticFeedback(session, &info,
        reinterpret_cast<const XrHapticBaseHeader*>(&vib));
}

} // namespace vrc
