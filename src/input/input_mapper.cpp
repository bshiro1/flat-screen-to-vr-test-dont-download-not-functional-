#include "input_mapper.h"
#include "input/xr_input.h"
#include "vr/tracking.h"
#include "overlay/imgui_overlay.h"
#include "core/logging.h"
#include "core/config.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace vrc {

InputMapper& InputMapper::instance() {
    static InputMapper mapper;
    return mapper;
}

bool InputMapper::load_profile(const std::string& name) {
    auto path = Config::instance().profiles_dir() / (name + ".json");
    if (!std::filesystem::exists(path)) {
        // Fall back: look for the profile in a profiles/ folder next to vr_converter.dll
        char dll_path[MAX_PATH] = {};
        HMODULE hm = GetModuleHandleA("vr_converter.dll");
        if (hm && GetModuleFileNameA(hm, dll_path, MAX_PATH)) {
            auto dll_dir = std::filesystem::path(dll_path).parent_path();
            auto candidate = dll_dir / "profiles" / (name + ".json");
            if (std::filesystem::exists(candidate)) {
                std::filesystem::create_directories(path.parent_path());
                std::filesystem::copy_file(candidate, path,
                    std::filesystem::copy_options::skip_existing);
                Log::info("Deployed input profile '{}' from DLL directory", name);
            }
        }
    }
    if (!std::filesystem::exists(path)) {
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
        prof.head_aim = j.value("head_aim", false);
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
        Log::info("Loaded input profile: {} ({} bindings)", name, prof.bindings.size());
        return true;
    } catch (const std::exception& e) {
        Log::error("Failed to load input profile: {}", e.what());
        return false;
    }
}

bool InputMapper::save_profile(const std::string& name) const {
    auto path = Config::instance().profiles_dir() / (name + ".json");
    try {
        auto it = profiles_.find(name);
        if (it == profiles_.end()) {
            Log::warn("Profile {} not found", name);
            return false;
        }

        const auto& prof = it->second;
        nlohmann::json j;
        j["name"] = prof.name;
        j["game"] = prof.game;
        j["look_sensitivity"] = prof.look_sensitivity;
        j["movement_smoothing"] = prof.movement_smoothing;
        j["head_aim"] = prof.head_aim;
        j["snap_turn"] = prof.snap_turn;
        j["snap_turn_angle"] = prof.snap_turn_angle;
        j["vignette_on_move"] = prof.vignette_on_move;

        nlohmann::json bindings = nlohmann::json::array();
        for (auto& b : prof.bindings) {
            nlohmann::json bj;
            bj["action"] = static_cast<u32>(b.action);
            bj["source"] = b.source;
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
    std::vector<std::string> names;
    for (auto& [name, _] : profiles_) {
        names.push_back(name);
    }
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

void InputMapper::process_frame(f64 dt_ms) {
    if (!XrInput::instance().is_active()) return;
    if (dt_ms <= 0.0 || dt_ms > 200.0) return;

    const auto& vr    = XrInput::instance().state();
    auto&       proxy = InputProxy::instance();

    // ── 0. Overlay open → switch to cursor-navigation mode ───────────────
    if (ImGuiOverlay::instance().is_visible()) {
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
        constexpr f32 kDeadzone = 0.15f;
        f32 lx = vr.thumbstick_left.x;
        f32 ly = vr.thumbstick_left.y;

        inject_digital(0x57, ly >  kDeadzone, vk_press_state_);  // W = forward
        inject_digital(0x53, ly < -kDeadzone, vk_press_state_);  // S = back
        inject_digital(0x44, lx >  kDeadzone, vk_press_state_);  // D = right
        inject_digital(0x41, lx < -kDeadzone, vk_press_state_);  // A = left
    }

    // ── 3. Look / snap-turn (right thumbstick or head rotation) ──────────
    if (current_profile_.head_aim) {
        // Drive mouse look from head rotation delta each frame
        auto  pose     = TrackingSystem::instance().get_head_pose();
        Quat& cur      = pose.rotation;
        Quat& prev     = last_head_rot_;

        // Quaternion delta = cur * prev^-1 (local rotation since last frame)
        Quat prev_inv  = { -prev.x, -prev.y, -prev.z, prev.w };
        float norm     = prev.x*prev.x + prev.y*prev.y + prev.z*prev.z + prev.w*prev.w;
        if (norm > 0.0001f) {
            float inv_n = 1.0f / norm;
            prev_inv = { -prev.x*inv_n, -prev.y*inv_n, -prev.z*inv_n, prev.w*inv_n };
        }

        Quat dq;
        dq.x = cur.w*prev_inv.x + cur.x*prev_inv.w + cur.y*prev_inv.z - cur.z*prev_inv.y;
        dq.y = cur.w*prev_inv.y - cur.x*prev_inv.z + cur.y*prev_inv.w + cur.z*prev_inv.x;
        dq.z = cur.w*prev_inv.z + cur.x*prev_inv.y - cur.y*prev_inv.x + cur.z*prev_inv.w;
        dq.w = cur.w*prev_inv.w - cur.x*prev_inv.x - cur.y*prev_inv.y - cur.z*prev_inv.z;

        // Extract yaw (y) and pitch (x) from delta quaternion in degrees
        float yaw_rad   = 2.0f * std::atan2f(dq.y, dq.w);
        float pitch_rad = 2.0f * std::asinf(std::clamp(dq.x, -1.0f, 1.0f));

        const float kPixelsPerRad = 300.0f * current_profile_.look_sensitivity;
        i32 dx = static_cast<i32>(yaw_rad   * kPixelsPerRad);
        i32 dy = static_cast<i32>(pitch_rad * kPixelsPerRad);

        if (dx != 0 || dy != 0) proxy.inject_mouse_move(dx, dy);

        last_head_rot_ = cur;
    }

    if (current_profile_.snap_turn) {
        snap_cooldown_ = std::max(0.0, snap_cooldown_ - dt_ms);
        if (snap_cooldown_ <= 0.0) {
            f32 rx = vr.thumbstick_right.x;
            constexpr f32 kThreshold = 0.7f;
            bool snap_right = rx >  kThreshold && prev_snap_rx_ <=  kThreshold;
            bool snap_left  = rx < -kThreshold && prev_snap_rx_ >= -kThreshold;
            if (snap_right || snap_left) {
                i32 dx = current_profile_.snap_turn_mouse_pixels;
                proxy.inject_mouse_move(snap_right ? dx : -dx, 0);
                snap_cooldown_ = 400.0;   // 400 ms lockout prevents double-snap
                Log::debug("Snap turn: {}", snap_right ? "right" : "left");
            }
            prev_snap_rx_ = rx;
        }
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
