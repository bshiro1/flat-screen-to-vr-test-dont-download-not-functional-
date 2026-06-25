#include "xr_input.h"
#include "core/logging.h"
#include <cstring>

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

// ─── Singleton ──────────────────────────────────────────────────────────────

XrInput& XrInput::instance() {
    static XrInput inp;
    return inp;
}

bool XrInput::initialize(XrInstance xr_instance, XrSession session) {
    if (active_) return true;
    if (!create_action_set(xr_instance)) return false;
    suggest_bindings(xr_instance);      // best-effort; missing profiles are ignored
    if (!attach(session)) return false;
    active_ = true;
    Log::info("XrInput: action set attached ({} controller profiles suggested)",
              3 /* oculus + index + simple */);
    return true;
}

void XrInput::shutdown() {
    if (!active_) return;
    if (space_grip_left_)  xrDestroySpace(space_grip_left_);
    if (space_grip_right_) xrDestroySpace(space_grip_right_);
    if (space_aim_left_)   xrDestroySpace(space_aim_left_);
    if (space_aim_right_)  xrDestroySpace(space_aim_right_);
    if (action_set_)       xrDestroyActionSet(action_set_);
    space_grip_left_ = space_grip_right_ = space_aim_left_ = space_aim_right_ = XR_NULL_HANDLE;
    action_set_ = XR_NULL_HANDLE;
    active_ = false;
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
    return
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
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "grip_pose_left",         "Left Grip Pose",      &a_grip_pose_left_)         &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "grip_pose_right",        "Right Grip Pose",     &a_grip_pose_right_)        &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "aim_pose_left",          "Left Aim Pose",       &a_aim_pose_left_)          &&
        make(T::XR_ACTION_TYPE_POSE_INPUT,     "aim_pose_right",         "Right Aim Pose",      &a_aim_pose_right_)         &&
        make(T::XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_left",          "Left Haptic",         &a_haptic_left_)            &&
        make(T::XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic_right",         "Right Haptic",        &a_haptic_right_);
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
        xrSuggestInteractionProfileBindings(inst, &info);
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
        {&a_grip_left_,       "/user/hand/left/input/squeeze/click"},
        {&a_grip_right_,      "/user/hand/right/input/squeeze/click"},
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
        {&a_trigger_left_,    "/user/hand/left/input/select/click"},
        {&a_trigger_right_,   "/user/hand/right/input/select/click"},
        {&a_menu_,            "/user/hand/left/input/menu/click"},
        {&a_grip_pose_left_,  "/user/hand/left/input/grip/pose"},
        {&a_grip_pose_right_, "/user/hand/right/input/grip/pose"},
        {&a_aim_pose_left_,   "/user/hand/left/input/aim/pose"},
        {&a_aim_pose_right_,  "/user/hand/right/input/aim/pose"},
        {&a_haptic_left_,     "/user/hand/left/output/haptic"},
        {&a_haptic_right_,    "/user/hand/right/output/haptic"},
    });

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
    return true;
}

// ─── Frame Sync ──────────────────────────────────────────────────────────────

void XrInput::sync(XrSession session, XrSpace ref_space, XrTime predicted_time) {
    if (!active_) return;

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
    };
    auto get_bool = [&](XrAction action, bool& out) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = XR_NULL_PATH;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &s)) && s.isActive)
            out = s.currentState != XR_FALSE;
    };
    auto get_vec2 = [&](XrAction action, Vec2& out) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = XR_NULL_PATH;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &info, &s)) && s.isActive) {
            out.x = s.currentState.x;
            out.y = s.currentState.y;
        }
    };
    auto get_pose = [&](XrSpace space, Vec3& pos, Quat& rot, bool& tracked) {
        if (!space) return;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(space, ref_space, predicted_time, &loc))) return;
        bool pos_ok  = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        bool rot_ok  = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        tracked = pos_ok && rot_ok;
        if (pos_ok) { pos.x = loc.pose.position.x; pos.y = loc.pose.position.y; pos.z = loc.pose.position.z; }
        if (rot_ok) { rot.x = loc.pose.orientation.x; rot.y = loc.pose.orientation.y; rot.z = loc.pose.orientation.z; rot.w = loc.pose.orientation.w; }
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

    // Derive click states from analog thresholds
    state_.trigger_click_left  = state_.trigger_left  > 0.7f;
    state_.trigger_click_right = state_.trigger_right > 0.7f;
    state_.grip_click_left     = state_.grip_left     > 0.7f;
    state_.grip_click_right    = state_.grip_right    > 0.7f;

    get_pose(space_grip_left_,  state_.left_grip_pos,  state_.left_grip_rot,  state_.left_tracked);
    get_pose(space_grip_right_, state_.right_grip_pos, state_.right_grip_rot, state_.right_tracked);
    get_pose(space_aim_left_,   state_.left_aim_pos,   state_.left_aim_rot,   state_.left_tracked);
    get_pose(space_aim_right_,  state_.right_aim_pos,  state_.right_aim_rot,  state_.right_tracked);
}

// ─── Haptics ─────────────────────────────────────────────────────────────────

void XrInput::trigger_haptic(XrSession session, bool left,
                               f32 amplitude, f32 duration_s) {
    if (!active_) return;
    XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = amplitude;
    vib.duration  = static_cast<XrDuration>(duration_s * 1'000'000'000.0f);
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action        = left ? a_haptic_left_ : a_haptic_right_;
    info.subactionPath = XR_NULL_PATH;
    xrApplyHapticFeedback(session, &info,
        reinterpret_cast<const XrHapticBaseHeader*>(&vib));
}

} // namespace vrc
