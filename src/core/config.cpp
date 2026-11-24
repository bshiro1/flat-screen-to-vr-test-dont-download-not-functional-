#include "config.h"
#include "logging.h"
#include "input/input_mapper.h"
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace vrc {

namespace {

using json = nlohmann::json;

// Reads `key` from a JSON object, falling back to `def` when the key is
// missing, null or of an incompatible type. One bad value must not discard
// the whole config file.
template <typename T>
T read_or(const json& obj, const char* key, const T& def) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return def;
    try {
        return it->template get<T>();
    } catch (const json::exception&) {
        Log::warn("Config: key '{}' has an unexpected type, using the default", key);
        return def;
    }
}

f32 clamp_finite(f32 v, f32 lo, f32 hi, f32 def) {
    if (!std::isfinite(v)) return def;
    return std::clamp(v, lo, hi);
}

// Clamps values to ranges the renderer can use. Applied to everything that
// enters Config, whether from disk or from the overlay.
void sanitize(ConfigProfile& p) {
    const ConfigProfile def;
    p.ipd = clamp_finite(p.ipd, 0.03f, 0.10f, def.ipd);
    p.world_scale = clamp_finite(p.world_scale, 0.1f, 10.0f, def.world_scale);
    p.convergence_distance = clamp_finite(p.convergence_distance, 0.1f, 1000.0f,
                                          def.convergence_distance);
    p.fov_override = clamp_finite(p.fov_override, 0.0f, 170.0f, def.fov_override);
    p.render_scale = clamp_finite(p.render_scale, 0.25f, 2.0f, def.render_scale);
    p.depth_fov_degrees = clamp_finite(p.depth_fov_degrees, 10.0f, 170.0f, def.depth_fov_degrees);
    p.parallax_strength = clamp_finite(p.parallax_strength, 0.0f, 10.0f, def.parallax_strength);

    if (!std::isfinite(p.depth_near_plane) || p.depth_near_plane <= 0.0f)
        p.depth_near_plane = def.depth_near_plane;
    if (!std::isfinite(p.depth_far_plane) || p.depth_far_plane <= p.depth_near_plane)
        p.depth_far_plane = std::max(def.depth_far_plane, p.depth_near_plane * 10.0f);

    if (p.render_mode != RenderMode::Screen && p.render_mode != RenderMode::Depth3D)
        p.render_mode = RenderMode::Screen;
    if (p.injection_method != InjectionMethod::LegacySendInput &&
        p.injection_method != InjectionMethod::SixDofBridge)
        p.injection_method = InjectionMethod::LegacySendInput;
}

RenderMode parse_render_mode(int v) {
    if (v == static_cast<int>(RenderMode::Screen)) return RenderMode::Screen;
    if (v == static_cast<int>(RenderMode::Depth3D)) return RenderMode::Depth3D;
    // 2 was the removed TrueStereo mode; anything else is invalid too.
    static bool warned = false;
    if (!warned) {
        warned = true;
        Log::warn("Config: render_mode {} is not supported (2 = removed TrueStereo); using Screen", v);
    }
    return RenderMode::Screen;
}

ConfigProfile profile_from_json(const std::string& name, const json& p) {
    const ConfigProfile d;
    ConfigProfile prof;
    prof.name = name;
    prof.ipd = read_or(p, "ipd", d.ipd);
    prof.world_scale = read_or(p, "world_scale", d.world_scale);
    prof.convergence_distance = read_or(p, "convergence_distance", d.convergence_distance);
    prof.enable_head_tracking = read_or(p, "enable_head_tracking", d.enable_head_tracking);
    prof.fov_override = read_or(p, "fov_override", d.fov_override);
    prof.render_scale = read_or(p, "render_scale", d.render_scale);
    prof.render_mode = parse_render_mode(
        read_or(p, "render_mode", static_cast<int>(d.render_mode)));
    prof.input_profile = read_or(p, "input_profile", d.input_profile);
    prof.swap_eyes = read_or(p, "swap_eyes", d.swap_eyes);
    prof.show_eye_debug = read_or(p, "show_eye_debug", d.show_eye_debug);
    prof.injection_method = static_cast<InjectionMethod>(
        read_or(p, "injection_method", static_cast<int>(d.injection_method)));
    prof.bridge_send_legacy_input = read_or(p, "bridge_send_legacy_input", d.bridge_send_legacy_input);
    prof.bridge_name = read_or(p, "bridge_name", d.bridge_name);
    prof.game_mod_enabled = read_or(p, "game_mod_enabled", d.game_mod_enabled);
    prof.game_mod_path = read_or(p, "game_mod_path", d.game_mod_path);
    prof.fbt_enabled = read_or(p, "fbt_enabled", d.fbt_enabled);
    prof.fbt_hips_role = read_or(p, "fbt_hips_role", d.fbt_hips_role);
    prof.fbt_left_foot_role = read_or(p, "fbt_left_foot_role", d.fbt_left_foot_role);
    prof.fbt_right_foot_role = read_or(p, "fbt_right_foot_role", d.fbt_right_foot_role);
    prof.fbt_chest_role = read_or(p, "fbt_chest_role", d.fbt_chest_role);
    prof.fbt_left_shoulder_role = read_or(p, "fbt_left_shoulder_role", d.fbt_left_shoulder_role);
    prof.fbt_right_shoulder_role = read_or(p, "fbt_right_shoulder_role", d.fbt_right_shoulder_role);
    prof.volumetric_parallax = read_or(p, "volumetric_parallax", d.volumetric_parallax);
    prof.depth_near_plane = read_or(p, "depth_near_plane", d.depth_near_plane);
    prof.depth_far_plane = read_or(p, "depth_far_plane", d.depth_far_plane);
    prof.depth_fov_degrees = read_or(p, "depth_fov_degrees", d.depth_fov_degrees);
    prof.parallax_strength = read_or(p, "parallax_strength", d.parallax_strength);
    prof.depth_reversed_z = read_or(p, "depth_reversed_z", d.depth_reversed_z);
    sanitize(prof);
    return prof;
}

// Writes the keys this DLL owns into `p`, leaving every other key untouched.
void profile_to_json(const ConfigProfile& prof, json& p) {
    p["kind"] = "vr";
    p["ipd"] = prof.ipd;
    p["world_scale"] = prof.world_scale;
    p["convergence_distance"] = prof.convergence_distance;
    p["enable_head_tracking"] = prof.enable_head_tracking;
    p["fov_override"] = prof.fov_override;
    p["render_scale"] = prof.render_scale;
    p["render_mode"] = static_cast<int>(prof.render_mode);
    p["input_profile"] = prof.input_profile;
    p["swap_eyes"] = prof.swap_eyes;
    p["show_eye_debug"] = prof.show_eye_debug;
    p["injection_method"] = static_cast<int>(prof.injection_method);
    p["bridge_send_legacy_input"] = prof.bridge_send_legacy_input;
    p["bridge_name"] = prof.bridge_name;
    p["game_mod_enabled"] = prof.game_mod_enabled;
    p["game_mod_path"] = prof.game_mod_path;
    p["fbt_enabled"] = prof.fbt_enabled;
    p["fbt_hips_role"] = prof.fbt_hips_role;
    p["fbt_left_foot_role"] = prof.fbt_left_foot_role;
    p["fbt_right_foot_role"] = prof.fbt_right_foot_role;
    p["fbt_chest_role"] = prof.fbt_chest_role;
    p["fbt_left_shoulder_role"] = prof.fbt_left_shoulder_role;
    p["fbt_right_shoulder_role"] = prof.fbt_right_shoulder_role;
    p["volumetric_parallax"] = prof.volumetric_parallax;
    p["depth_near_plane"] = prof.depth_near_plane;
    p["depth_far_plane"] = prof.depth_far_plane;
    p["depth_fov_degrees"] = prof.depth_fov_degrees;
    p["parallax_strength"] = prof.parallax_strength;
    p["depth_reversed_z"] = prof.depth_reversed_z;
}

// Parses preserved raw text back into an object; anything unusable becomes {}.
json parse_raw_object(const std::string& text) {
    if (text.empty()) return json::object();
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : json::object();
}

// Writes via a temporary file and a rename, so a crash or a concurrent reader
// (the companion app) never sees a half-written file.
bool write_json_file(const std::filesystem::path& path, const json& j) {
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs << j.dump(4);
        ofs.flush();
        if (!ofs) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

Config& Config::instance() {
    static Config cfg;
    return cfg;
}

std::filesystem::path Config::config_dir() const {
    const char* appdata = std::getenv("APPDATA");
    // APPDATA is normally set, but may be missing in services or stripped
    // environments. Fall back to the temp directory instead of constructing a
    // path from nullptr.
    auto base = appdata ? std::filesystem::path(appdata)
                        : std::filesystem::temp_directory_path();
    auto path = base / "VRGameConverter";
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path Config::profiles_dir() const {
    auto path = config_dir() / "profiles";
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path Config::log_path() const {
    return config_dir() / "vr_converter.log";
}

bool Config::load(const std::filesystem::path& path) {
    ConfigProfile loaded;
    {
        std::lock_guard lock(mutex_);
        config_file_ = path;
        if (!std::filesystem::exists(path)) {
            Log::warn("Config not found at {}, using defaults", path.string());
            set_default_config_unlocked();
            return false;
        }
        try {
            std::ifstream ifs(path);
            auto j = json::parse(ifs);
            if (!j.is_object()) throw std::runtime_error("top level is not an object");

            // A reload is authoritative: profiles removed from the file must
            // not survive in memory and reappear on the next save.
            profiles_.clear();
            raw_profile_json_.clear();
            current_ = ConfigProfile{};

            auto profiles_it = j.find("profiles");
            if (profiles_it != j.end() && profiles_it->is_object()) {
                for (auto& [name, p] : profiles_it->items()) {
                    if (!p.is_object()) {
                        Log::warn("Config: profile '{}' is not an object, skipped", name);
                        continue;
                    }
                    profiles_[name] = profile_from_json(name, p);
                    raw_profile_json_[name] = p.dump();
                }
            }
            if (profiles_.empty()) profiles_["default"] = current_;

            const std::string current_name = read_or(j, "current_profile", std::string("default"));
            auto cur = profiles_.find(current_name);
            current_ = (cur != profiles_.end()) ? cur->second : profiles_.begin()->second;

            enable_overlay_.store(read_or(j, "enable_overlay", true), std::memory_order_relaxed);

            // Keep the top-level object without "profiles": that part is
            // rebuilt from profiles_ on save.
            j.erase("profiles");
            raw_root_json_ = j.dump();

            loaded = current_;
            Log::info("Config loaded from {}", path.string());
        } catch (const std::exception& e) {
            Log::error("Failed to load config: {}", e.what());
            raw_root_json_.clear();
            set_default_config_unlocked();
            return false;
        }
    }
    // Outside the lock: subsystems may call back into Config.
    apply_to_subsystems(loaded);
    return true;
}

bool Config::save(const std::filesystem::path& path) const {
    std::lock_guard lock(mutex_);
    try {
        json j = parse_raw_object(raw_root_json_);
        j["current_profile"] = current_.name;
        j["enable_overlay"] = enable_overlay_.load(std::memory_order_relaxed);

        json profiles_obj = json::object();
        auto write_profile = [&](const ConfigProfile& prof) {
            auto raw = raw_profile_json_.find(prof.name);
            json p = raw != raw_profile_json_.end() ? parse_raw_object(raw->second)
                                                    : json::object();
            profile_to_json(prof, p);
            profiles_obj[prof.name] = std::move(p);
        };
        for (const auto& [name, prof] : profiles_) {
            write_profile(name == current_.name ? current_ : prof);
        }
        if (!profiles_.contains(current_.name)) write_profile(current_);
        j["profiles"] = std::move(profiles_obj);

        if (!write_json_file(path, j)) {
            Log::error("Failed to write config to {}", path.string());
            return false;
        }
        Log::info("Config saved to {}", path.string());
        return true;
    } catch (const std::exception& e) {
        Log::error("Failed to save config: {}", e.what());
        return false;
    }
}

bool Config::load_profile(const std::string& name) {
    auto path = profiles_dir() / (name + ".json");
    if (!std::filesystem::exists(path)) {
        Log::warn("Profile {} not found", name);
        return false;
    }
    ConfigProfile prof;
    {
        std::lock_guard lock(mutex_);
        try {
            std::ifstream ifs(path);
            auto j = json::parse(ifs);
            if (!j.is_object()) throw std::runtime_error("profile is not an object");
            prof = profile_from_json(name, j);
            profiles_[name] = prof;
            raw_profile_json_[name] = j.dump();
            current_ = prof;
            Log::info("Loaded profile: {}", name);
        } catch (const std::exception& e) {
            Log::error("Failed to load profile {}: {}", name, e.what());
            return false;
        }
    }
    apply_to_subsystems(prof);
    return true;
}

bool Config::save_profile(const std::string& name) const {
    auto path = profiles_dir() / (name + ".json");
    std::lock_guard lock(mutex_);
    const ConfigProfile* prof = nullptr;
    if (name == current_.name) {
        prof = &current_;
    } else if (auto it = profiles_.find(name); it != profiles_.end()) {
        prof = &it->second;
    }
    if (!prof) {
        Log::warn("Profile {} not found, nothing saved", name);
        return false;
    }
    try {
        auto raw = raw_profile_json_.find(name);
        json j = raw != raw_profile_json_.end() ? parse_raw_object(raw->second)
                                                : json::object();
        profile_to_json(*prof, j);
        if (!write_json_file(path, j)) {
            Log::error("Failed to write profile {} to {}", name, path.string());
            return false;
        }
        Log::info("Saved profile: {}", name);
        return true;
    } catch (const std::exception& e) {
        Log::error("Failed to save profile {}: {}", name, e.what());
        return false;
    }
}

void Config::set_current_profile(const ConfigProfile& profile) {
    ConfigProfile p = profile;
    sanitize(p);
    std::lock_guard lock(mutex_);
    current_ = p;
    profiles_[p.name] = p;
}

ConfigProfile Config::current_profile() const {
    std::lock_guard lock(mutex_);
    return current_;
}

void Config::set_default_config_unlocked() {
    // Per-profile raw JSON belongs to the profiles being dropped. The top-level
    // raw object is kept so unrelated keys survive a reset.
    current_ = ConfigProfile{};
    profiles_.clear();
    raw_profile_json_.clear();
    profiles_["default"] = current_;
}

void Config::set_default_config() {
    ConfigProfile def;
    {
        std::lock_guard lock(mutex_);
        set_default_config_unlocked();
        def = current_;
    }
    apply_to_subsystems(def);
}

std::vector<std::string> Config::available_profiles() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (auto& [name, _] : profiles_) {
        names.push_back(name);
    }
    return names;
}

void Config::apply_to_subsystems(const ConfigProfile& prof) {
    // Rendering, tracking and the 6DoF bridge read current_profile_copy() every
    // frame, so they need no push. The input mapping is the exception: it is a
    // separate file, (re)loaded whenever a VR profile becomes active so the
    // Input tab and the mapper follow the profile's input_profile.
    if (prof.input_profile.empty()) return;
    if (!InputMapper::instance().load_profile(prof.input_profile)) {
        Log::warn("Input profile '{}' named by VR profile '{}' could not be loaded",
                  prof.input_profile, prof.name);
    }
}

} // namespace vrc
