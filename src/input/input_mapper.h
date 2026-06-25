#pragma once

#include "core/types.h"
#include "input_proxy.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vrc {

struct Binding {
    InputAction action;
    std::string source;     // "vr.right.trigger", "vr.left.thumbstick.up", etc.
    u16 vk_code = 0;        // If nonzero, inject this VK key when source > deadzone
    f32 scale = 1.0f;
    f32 deadzone = 0.1f;
    bool invert = false;
    bool turbo = false;
    std::string macro;
};

struct InputProfile {
    std::string name = "default_vr";
    std::string game = "*";
    std::vector<Binding> bindings;
    f32 look_sensitivity = 1.0f;
    f32 movement_smoothing = 0.5f;
    bool head_aim = true;           // Drive mouse look from head rotation delta
    bool snap_turn = true;
    f32 snap_turn_angle = 45.0f;
    i32 snap_turn_mouse_pixels = 300; // Mouse delta injected per snap turn
    bool vignette_on_move = true;
};

class InputMapper {
public:
    static InputMapper& instance();

    bool load_profile(const std::string& name);
    bool save_profile(const std::string& name) const;
    const InputProfile& current_profile() const { return current_profile_; }
    InputProfile& current_profile() { return current_profile_; }

    void set_profile(const InputProfile& profile);
    InputEvent map_event(const InputEvent& event) const;
    void apply_deadzone(InputEvent& event) const;

    // Called once per OpenXR frame to translate all VR input into game input.
    // dt_ms is the frame interval in milliseconds.
    void process_frame(f64 dt_ms);

    // VR controller -> game input translation
    void process_vr_aim(const Vec3& aim_direction);
    void process_vr_thumbstick(const Vec2& stick, InputAction horizontal, InputAction vertical);

    std::vector<std::string> available_profiles() const;

private:
    InputMapper() = default;

    // Read a numeric value (0..1) from a binding source string
    f32 resolve_source_value(const std::string& source) const;

    // Inject a key or mouse button, tracking edge (press/release) transitions
    void inject_digital(u16 vk_code, bool active,
                        std::unordered_map<u16, bool>& press_state);

    InputProfile current_profile_;
    std::unordered_map<std::string, InputProfile> profiles_;

    // Per-frame state for process_frame()
    std::unordered_map<u16, bool> vk_press_state_;
    Quat    last_head_rot_{0, 0, 0, 1};
    f32     prev_snap_rx_   = 0.0f;
    f64     snap_cooldown_  = 0.0;   // ms remaining before next snap is allowed
};

} // namespace vrc
