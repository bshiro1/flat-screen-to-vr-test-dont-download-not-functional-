#include "input_mapper.h"
#include "input/xr_input.h"
#include "vr/tracking.h"
#include "overlay/imgui_overlay.h"
#include "core/logging.h"
#include "core/config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace vrc {

namespace {
// VR config profiles are stored in a sibling folder so the injected DLL can
// read them, but they are not input-binding profiles. Only accept files that
// actually look like input profiles.
bool is_input_profile_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    try {
        auto j = nlohmann::json::parse(in);
        if (!j.is_object()) return false;
        auto kind = j.value("kind", std::string{});
        if (kind == "vr") return false;
        return j.contains("bindings") || j.contains("game");
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace

InputMapper& InputMapper::instance() {
    static InputMapper mapper;
    return mapper;
}

bool InputMapper::load_profile(const std::string& name) {
    const auto vr_profiles_dir = Config::instance().profiles_dir();
    const auto input_dir = vr_profiles_dir.parent_path() / "input_profiles";
    auto path = input_dir / (name + ".json");
    const auto legacy_path = vr_profiles_dir / (name + ".json");

    // Migrate a valid legacy input profile into the dedicated directory.
    if (!is_input_profile_file(path) && is_input_profile_file(legacy_path)) {
        std::error_code ec;
        std::filesystem::create_directories(input_dir, ec);
        // The destination exists but is invalid (or a VR profile). Repair it
        // from the valid legacy copy instead of leaving the bad file in place.
        std::filesystem::copy_file(legacy_path, path,
            std::filesystem::copy_options::overwrite_existing, ec);
    }

    // Deploy shipped profiles from the DLL folder as a final fallback.
    if (!is_input_profile_file(path)) {
        char dll_path[MAX_PATH] = {};
        HMODULE hm = GetModuleHandleA("vr_converter.dll");
        if (hm && GetModuleFileNameA(hm, dll_path, MAX_PATH)) {
            auto dll_dir = std::filesystem::path(dll_path).parent_path();
            auto candidate = dll_dir / "profiles" / (name + ".json");
            if (is_input_profile_file(candidate)) {
                std::error_code ec;
                std::filesystem::create_directories(input_dir, ec);
                std::filesystem::copy_file(candidate, path,
                    std::filesystem::copy_options::overwrite_existing, ec);
                Log::info("Deployed input profile '{}' from DLL directory", name);
            }
        }
    }
    if (!is_input_profile_file(path)) {
        Log::warn("Input profile '{}' not found, using defaults", name);
        return false;
    }

    try {
        std::ifstream ifs(path);
        nlohmann::json j;
        ifs >> j;

        InputProfile prof;
        prof.name = name;
        prof.game = j.value("game", "*");
        prof.look_sensitivity = j.value("look_sensitivity", 1.0f);
        prof.movement_smoothing = j.value("movement_smoothing", 0.5f);
        prof.invert_x = j.value("invert_x", false);
        prof.invert_y = j.value("invert_y", false);
        prof.snap_turn = j.value("snap_turn", true);
        prof.snap_turn_angle = j.value("snap_turn_angle", 45.0f);
        prof.vignette_on_move = j.value("vignette_on_move", true);

        prof.snap_turn_mouse_pixels = j.value("snap_turn_mouse_pixels", 300);
        prof.head_aim = j.value("head_aim", true);

        if (j.contains("bindings")) {
            for (auto& b : j["bindings"]) {
                Binding binding;
                binding.action   = static_cast<InputAction>(b.value("action", 0));
                binding.source   = b.value("source", "");
                binding.vk_code  = static_cast<u16>(b.value("vk_code", 0));
                binding.scale    = b.value("scale", 1.0f);
                binding.deadzone = b.value("deadzone", 0.1f);
                binding.invert   = b.value("invert", false);
                binding.turbo    = b.value("turbo", false);
                binding.macro    = b.value("macro", "");
                prof.bindings.push_back(binding);
            }
        }

        current_profile_ = prof;
        profiles_[name] = prof;
        head_rot_initialized_ = false;
        look_remainder_x_ = look_remainder_y_ = 0.0;
        prev_snap_rx_ = 0.0f;
        snap_cooldown_ = 0.0;
        Log::info("Loaded input profile: {} ({} bindings)", name, prof.bindings.size());
        return true;
    } catch (const std::exception& e) {
        Log::error("Failed to load input profile: {}", e.what());
        return false;
    }
}

bool InputMapper::save_profile(const std::string& name) const {
    const auto input_dir =
        Config::instance().profiles_dir().parent_path() / "input_profiles";
    auto path = input_dir / (name + ".json");
    try {
        std::filesystem::create_directories(path.parent_path());
        auto it = profiles_.find(name);
        if (it == profiles_.end()) {
            Log::warn("Profile {} not found", name);
            return false;
        }

        const auto& prof = it->second;
        nlohmann::json j;
        j["kind"] = "input";
        j["name"] = prof.name;
        j["game"] = prof.game;
        j["look_sensitivity"] = prof.look_sensitivity;
        j["movement_smoothing"] = prof.movement_smoothing;
        j["head_aim"] = prof.head_aim;
        j["invert_x"] = prof.invert_x;
        j["invert_y"] = prof.invert_y;
        j["snap_turn"] = prof.snap_turn;
        j["snap_turn_angle"] = prof.snap_turn_angle;
        j["vignette_on_move"] = prof.vignette_on_move;
        j["snap_turn_mouse_pixels"] = prof.snap_turn_mouse_pixels;

        nlohmann::json bindings = nlohmann::json::array();
        for (auto& b : prof.bindings) {
            nlohmann::json bj;
            bj["action"] = static_cast<u32>(b.action);
            bj["source"] = b.source;
            bj["vk_code"] = b.vk_code;
            bj["scale"] = b.scale;
            bj["deadzone"] = b.deadzone;
            bj["invert"] = b.invert;
            bj["turbo"] = b.turbo;
            bj["macro"] = b.macro;
            bindings.push_back(bj);
        }
        j["bindings"] = bindings;

        std::ofstream ofs(path);
        ofs << j.dump(4);
        Log::info("Saved input profile: {}", name);
        return true;
    } catch (const std::exception& e) {
        Log::error("Failed to save input profile: {}", e.what());
        return false;
    }
}

void InputMapper::set_profile(const InputProfile& profile) {
    current_profile_ = profile;
    profiles_[profile.name] = profile;
    head_rot_initialized_ = false;
    look_remainder_x_ = look_remainder_y_ = 0.0;
    prev_snap_rx_ = 0.0f;
    snap_cooldown_ = 0.0;
}

InputEvent InputMapper::map_event(const InputEvent& event) const {
    InputEvent mapped = event;

    // Find binding for this action
    for (auto& binding : current_profile_.bindings) {
        if (binding.action == event.action) {
            mapped.value = event.value * binding.scale;
            apply_deadzone(mapped);
            if (binding.invert) {
                mapped.value = -mapped.value;
            }
            break;
        }
    }

    return mapped;
}

void InputMapper::apply_deadzone(InputEvent& event) const {
    for (auto& binding : current_profile_.bindings) {
        if (binding.action == event.action) {
            if (std::abs(event.value) < binding.deadzone) {
                event.value = 0.0f;
            }
            break;
        }
    }
}

void InputMapper::process_vr_aim(const Vec3& aim_direction) {
    // Map VR controller aim direction to mouse look
    f32 yaw = atan2f(aim_direction.x, aim_direction.z);
    f32 pitch = asinf(-aim_direction.y);

    i32 dx = static_cast<i32>(yaw * current_profile_.look_sensitivity * 100.0f);
    i32 dy = static_cast<i32>(pitch * current_profile_.look_sensitivity * 100.0f);

    if (std::abs(dx) > 1 || std::abs(dy) > 1) {
        InputProxy::instance().inject_mouse_move(dx, dy);
    }
}

void InputMapper::process_vr_thumbstick(const Vec2& stick,
                                         InputAction horizontal,
                                         InputAction vertical)
{
    (void)horizontal;
    (void)vertical;
    auto& proxy = InputProxy::instance();
    f32 deadzone = 0.15f;

    // Horizontal
    if (std::abs(stick.x) > deadzone) {
        if (stick.x > 0) proxy.inject_keyboard_input(0x44, true);  // D
        else proxy.inject_keyboard_input(0x41, true);               // A
    }

    // Vertical
    if (std::abs(stick.y) > deadzone) {
        if (stick.y > 0) proxy.inject_keyboard_input(0x57, true);  // W
        else proxy.inject_keyboard_input(0x53, true);               // S
    }
}

std::vector<std::string> InputMapper::available_profiles() const {
    // Enumerate the profiles directory so newly installed / user-created
    // profiles (e.g. big_walk.json) show up in the in-game selector instead of
    // only the ones that happened to be loaded at startup.
    std::vector<std::string> names;
    auto add_dir = [&](const std::filesystem::path& dir) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (entry.path().extension() != ".json") continue;
            if (!is_input_profile_file(entry.path())) continue;
            names.push_back(entry.path().stem().string());
        }
    };
    const auto vr_profiles_dir = Config::instance().profiles_dir();
    add_dir(vr_profiles_dir.parent_path() / "input_profiles");
    add_dir(vr_profiles_dir);

    char dll_path[MAX_PATH] = {};
    HMODULE hm = GetModuleHandleA("vr_converter.dll");
    if (hm && GetModuleFileNameA(hm, dll_path, MAX_PATH)) {
        add_dir(std::filesystem::path(dll_path).parent_path() / "profiles");
    }

    // Keep any in-memory profiles that aren't on disk (e.g. set_profile calls).
    for (auto& [name, _] : profiles_) {
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// ─── Binding Source Resolver ─────────────────────────────────────────────────

f32 InputMapper::resolve_source_value(const std::string& src) const {
    if (src.empty()) return 0.0f;

    const auto& vr = XrInput::instance().state();

    // ── VR controller inputs ──────────────────────────────────────────────
    if (src == "vr.right.trigger")          return vr.trigger_right;
    if (src == "vr.left.trigger")           return vr.trigger_left;
    if (src == "vr.right.grab"  ||
        src == "vr.right.grip")             return vr.grip_right;
    if (src == "vr.left.grab"   ||
        src == "vr.left.grip")              return vr.grip_left;
    if (src == "vr.right.button_a")         return vr.button_a  ? 1.0f : 0.0f;
    if (src == "vr.right.button_b")         return vr.button_b  ? 1.0f : 0.0f;
    if (src == "vr.left.button_x")          return vr.button_x  ? 1.0f : 0.0f;
    if (src == "vr.left.button_y")          return vr.button_y  ? 1.0f : 0.0f;
    if (src == "vr.left.menu"   ||
        src == "vr.menu")                   return vr.menu      ? 1.0f : 0.0f;
    if (src == "vr.left.thumbstick_click")  return vr.thumbstick_click_left  ? 1.0f : 0.0f;
    if (src == "vr.right.thumbstick_click") return vr.thumbstick_click_right ? 1.0f : 0.0f;

    // Thumbstick cardinal axes
    if (src == "vr.left.thumbstick.up")    return std::max(0.0f,  vr.thumbstick_left.y);
    if (src == "vr.left.thumbstick.down")  return std::max(0.0f, -vr.thumbstick_left.y);
    if (src == "vr.left.thumbstick.left")  return std::max(0.0f, -vr.thumbstick_left.x);
    if (src == "vr.left.thumbstick.right") return std::max(0.0f,  vr.thumbstick_left.x);
    if (src == "vr.right.thumbstick.up")   return std::max(0.0f,  vr.thumbstick_right.y);
    if (src == "vr.right.thumbstick.down") return std::max(0.0f, -vr.thumbstick_right.y);
    if (src == "vr.right.thumbstick.left") return std::max(0.0f, -vr.thumbstick_right.x);
    if (src == "vr.right.thumbstick.right")return std::max(0.0f,  vr.thumbstick_right.x);

    // Thumbstick as raw axis magnitude (used for look/movement blocks)
    if (src == "vr.left.thumbstick")  return std::sqrt(
        vr.thumbstick_left.x  * vr.thumbstick_left.x  +
        vr.thumbstick_left.y  * vr.thumbstick_left.y);
    if (src == "vr.right.thumbstick") return std::sqrt(
        vr.thumbstick_right.x * vr.thumbstick_right.x +
        vr.thumbstick_right.y * vr.thumbstick_right.y);

    // ── Keyboard fallback ─────────────────────────────────────────────────
    if (src.size() > 9 && src.substr(0, 9) == "keyboard.") {
        const std::string key = src.substr(9);
        u16 vk = 0;
        if      (key == "w")     vk = 0x57;
        else if (key == "a")     vk = 0x41;
        else if (key == "s")     vk = 0x53;
        else if (key == "d")     vk = 0x44;
        else if (key == "space") vk = VK_SPACE;
        else if (key == "ctrl")  vk = VK_LCONTROL;
        else if (key == "shift") vk = VK_LSHIFT;
        else if (key == "e")     vk = 0x45;
        else if (key == "r")     vk = 0x52;
        else if (key == "f")     vk = 0x46;
        else if (key == "esc")   vk = VK_ESCAPE;
        if (vk) return (GetAsyncKeyState(vk) & 0x8000) ? 1.0f : 0.0f;
    }

    return 0.0f;
}

// ─── Digital Injection Helper ─────────────────────────────────────────────────

void InputMapper::inject_digital(u16 vk_code, bool active,
                                  std::unordered_map<u16, bool>& press_state) {
    bool was = press_state[vk_code];
    if (active == was) return;
    press_state[vk_code] = active;

    auto& proxy = InputProxy::instance();
    if (vk_code == VK_LBUTTON)       proxy.inject_mouse_button(0, active);
    else if (vk_code == VK_RBUTTON)  proxy.inject_mouse_button(1, active);
    else if (vk_code == VK_MBUTTON)  proxy.inject_mouse_button(2, active);
    else                             proxy.inject_keyboard_input(vk_code, active);
}

// ─── Default VK code per action ───────────────────────────────────────────────

static u16 default_vk(InputAction action) {
    switch (action) {
        case InputAction::MoveForward:   return 0x57;       // W
        case InputAction::MoveBackward:  return 0x53;       // S
        case InputAction::MoveLeft:      return 0x41;       // A
        case InputAction::MoveRight:     return 0x44;       // D
        case InputAction::Jump:          return VK_SPACE;
        case InputAction::Crouch:        return 0x43;       // C
        case InputAction::Sprint:        return VK_LSHIFT;
        case InputAction::Interact:      return 0x46;       // F
        case InputAction::PrimaryFire:   return VK_LBUTTON;
        case InputAction::SecondaryFire: return VK_RBUTTON;
        case InputAction::Reload:        return 0x52;       // R
        case InputAction::Menu:          return VK_ESCAPE;
        case InputAction::Map:           return 0x4D;       // M
        case InputAction::Inventory:     return 0x49;       // I
        default:                         return 0;
    }
}

// ─── process_frame ────────────────────────────────────────────────────────────

void InputMapper::release_all_held() {
    // Snapshot the keys first: inject_digital mutates vk_press_state_ while
    // erasing entries from it would invalidate a live iterator.
    std::vector<u16> held;
    held.reserve(vk_press_state_.size());
    for (auto& [vk, pressed] : vk_press_state_) {
        if (pressed) held.push_back(vk);
    }
    for (u16 vk : held) inject_digital(vk, false, vk_press_state_);
}

void InputMapper::process_frame(f64 dt_ms) {
    if (!XrInput::instance().is_active()) {
        // Session lost / controller inactive: no further frames may arrive, so
        // release anything still held before bailing out.
        release_all_held();
        return;
    }
    if (dt_ms <= 0.0 || dt_ms > 200.0) return;

    const auto& vr    = XrInput::instance().state();
    auto&       proxy = InputProxy::instance();

    // ── 0. Overlay open → switch to cursor-navigation mode ───────────────
    if (ImGuiOverlay::instance().is_visible()) {
        // Release every key/button we were holding for the game — otherwise
        // e.g. W stays pressed and the player keeps walking while the menu
        // is up (this loop's early return would never send the key-up).
        for (auto& [vk, held] : vk_press_state_) {
            if (held && vk != VK_LBUTTON) inject_digital(vk, false, vk_press_state_);
        }
        // Right thumbstick moves the mouse cursor over the overlay
        f32 rx = vr.thumbstick_right.x;
        f32 ry = vr.thumbstick_right.y;
        constexpr f32 kCursorDeadzone = 0.15f;
        constexpr f32 kCursorSpeed    = 12.0f;
        i32 dx = std::abs(rx) > kCursorDeadzone ? static_cast<i32>(rx * kCursorSpeed) : 0;
        i32 dy = std::abs(ry) > kCursorDeadzone ? static_cast<i32>(-ry * kCursorSpeed) : 0;
        if (dx != 0 || dy != 0) proxy.inject_mouse_move(dx, dy);
        // Right trigger clicks
        inject_digital(VK_LBUTTON, vr.trigger_right > 0.7f, vk_press_state_);
        return; // Skip all game input injection while overlay is up
    }

    // ── 1. Profile bindings: VR source → game key injection ──────────────
    for (auto& b : current_profile_.bindings) {
        f32 raw = resolve_source_value(b.source);
        if (b.invert) raw = 1.0f - raw;
        raw *= b.scale;
        f32 val = std::abs(raw) < b.deadzone ? 0.0f : raw;

        u16 vk = b.vk_code ? b.vk_code : default_vk(b.action);

        // Actions that are purely analog look inputs — skip here, handled below
        if (b.action == InputAction::LookUp    || b.action == InputAction::LookDown  ||
            b.action == InputAction::LookLeft  || b.action == InputAction::LookRight)
            continue;

        if (vk) inject_digital(vk, val > 0.5f, vk_press_state_);
    }

    // ── 2. Movement injection (left thumbstick) ───────────────────────────
    {
        constexpr f32 kBaseDeadzone = 0.15f;
        // "Movement Smoothing" widens the deadzone so small stick wobble does
        // not produce twitchy WASD taps. This gives the overlay slider a real,
        // visible effect (previously it was parsed but never used).
        f32 deadzone = kBaseDeadzone
                     + std::clamp(current_profile_.movement_smoothing, 0.0f, 1.0f) * 0.25f;
        f32 lx = vr.thumbstick_left.x;
        f32 ly = vr.thumbstick_left.y;

        inject_digital(0x57, ly >  deadzone, vk_press_state_);  // W = forward
        inject_digital(0x53, ly < -deadzone, vk_press_state_);  // S = back
        inject_digital(0x44, lx >  deadzone, vk_press_state_);  // D = right
        inject_digital(0x41, lx < -deadzone, vk_press_state_);  // A = left
    }

    // ── 3. Look / snap-turn (right thumbstick or head rotation) ──────────
    if (current_profile_.head_aim) {
        Quat cur = TrackingSystem::instance().get_head_pose().rotation.normalized();

        // The first sample only establishes a baseline — without this, a head
        // pose starting on the opposite quaternion hemisphere injects a huge
        // one-frame look snap the moment the game loads.
        if (!head_rot_initialized_) {
            last_head_rot_ = cur;
            head_rot_initialized_ = true;
        } else {
            // Quaternions double-cover rotations: q and -q are the same pose.
            // Runtimes flip the sign past 180 degrees of yaw, which used to make
            // atan2(dq.y, dq.w) wrap and inject a ~1900-pixel horizontal jump.
            // Putting both samples on one hemisphere keeps dq.w non-negative.
            const f32 dot = cur.x*last_head_rot_.x + cur.y*last_head_rot_.y +
                            cur.z*last_head_rot_.z + cur.w*last_head_rot_.w;
            if (dot < 0.0f) {
                cur = { -cur.x, -cur.y, -cur.z, -cur.w };
            }

            Quat dq = (cur * last_head_rot_.conjugated()).normalized();
            Vec3 rv = quat_log(dq); // signed rotation vector, half-angle

            // OpenXR axes: +Y yaw is head turning LEFT, +X pitch is looking UP.
            // Windows mouse deltas are screen-space (+dx right, +dy down), so
            // both axes need their sign flipped. Previously neither was flipped,
            // which made looking inverted in both directions by default.
            f32 yaw_rad   =  2.0f * rv.y;
            f32 pitch_rad =  2.0f * rv.x;

            // Sanity bound: reject tracker glitches / residual prediction jumps.
            constexpr f32 kMaxStepRad = 0.35f; // ~20 degrees per frame
            yaw_rad   = std::clamp(yaw_rad,   -kMaxStepRad, kMaxStepRad);
            pitch_rad = std::clamp(pitch_rad, -kMaxStepRad, kMaxStepRad);

            const f32 kPixelsPerRad = 300.0f * current_profile_.look_sensitivity;
            f64 want_x = static_cast<f64>(-yaw_rad   * kPixelsPerRad);
            f64 want_y = static_cast<f64>(-pitch_rad * kPixelsPerRad);
            if (current_profile_.invert_x) want_x = -want_x;
            if (current_profile_.invert_y) want_y = -want_y;

            // Keep the fractional remainder so very slow head motion still
            // produces smooth sub-pixel movement instead of being truncated.
            want_x += look_remainder_x_;
            want_y += look_remainder_y_;
            i32 dx = static_cast<i32>(std::trunc(want_x));
            i32 dy = static_cast<i32>(std::trunc(want_y));
            look_remainder_x_ = want_x - static_cast<f64>(dx);
            look_remainder_y_ = want_y - static_cast<f64>(dy);

            if (dx != 0 || dy != 0) proxy.inject_mouse_move(dx, dy);
            last_head_rot_ = cur;
        }
    }

    if (current_profile_.snap_turn) {
        snap_cooldown_ = std::max(0.0, snap_cooldown_ - dt_ms);
        f32 rx = vr.thumbstick_right.x;
        constexpr f32 kThreshold = 0.7f;
        if (snap_cooldown_ <= 0.0) {
            bool snap_right = rx >  kThreshold && prev_snap_rx_ <=  kThreshold;
            bool snap_left  = rx < -kThreshold && prev_snap_rx_ >= -kThreshold;
            if (snap_right || snap_left) {
                // snap_turn_mouse_pixels is the calibrated pixel amount for the
                // default 45 degrees. Scale it by the user's Snap Turn Angle so
                // that slider actually changes how far the view rotates.
                f32 angle = current_profile_.snap_turn_angle;
                f32 scale = (angle > 1.0f) ? (angle / 45.0f) : 1.0f;
                i32 dx = static_cast<i32>(current_profile_.snap_turn_mouse_pixels * scale);
                proxy.inject_mouse_move(snap_right ? dx : -dx, 0);
                snap_cooldown_ = 400.0;   // 400 ms lockout prevents double-snap
                Log::debug("Snap turn: {}", snap_right ? "right" : "left");
            }
        }
        // Track the stick every frame, including during the cooldown. Otherwise
        // a re-flick while locked out uses a stale prev value and the crossing
        // is missed once the cooldown expires.
        prev_snap_rx_ = rx;
    } else if (!current_profile_.head_aim) {
        // Free-look: right thumbstick → continuous mouse look
        f32 rx = vr.thumbstick_right.x;
        f32 ry = vr.thumbstick_right.y;
        constexpr f32 kDeadzone = 0.15f;
        const f32 kScale = 8.0f * current_profile_.look_sensitivity;
        i32 dx = std::abs(rx) > kDeadzone ? static_cast<i32>(rx * kScale) : 0;
        i32 dy = std::abs(ry) > kDeadzone ? static_cast<i32>(-ry * kScale) : 0;
        if (dx != 0 || dy != 0) proxy.inject_mouse_move(dx, dy);
    }

    // ── 4. Trigger / grip → mouse buttons ────────────────────────────────
    constexpr f32 kClickThresh = 0.7f;
    inject_digital(VK_LBUTTON, vr.trigger_right > kClickThresh, vk_press_state_);
    inject_digital(VK_RBUTTON, vr.grip_right    > kClickThresh, vk_press_state_);
}

} // namespace vrc
