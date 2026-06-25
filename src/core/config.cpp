#include "config.h"
#include "logging.h"
#include "render/camera_rig.h"
#include "vr/tracking.h"
#include <fstream>
#include <nlohmann/json.hpp>

namespace vrc {

Config& Config::instance() {
    static Config cfg;
    return cfg;
}

std::filesystem::path Config::config_dir() const {
    auto path = std::filesystem::path(getenv("APPDATA")) / "VRGameConverter";
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
    bool ok = false;
    {
        std::lock_guard lock(mutex_);
        config_file_ = path;
        if (!std::filesystem::exists(path)) {
            Log::warn("Config not found at {}, using defaults", path.string());
            set_default_config();
            return false;
        }
        try {
            std::ifstream ifs(path);
            auto j = nlohmann::json::parse(ifs);
            if (j.contains("profiles")) {
                for (auto& [name, p] : j["profiles"].items()) {
                    ConfigProfile prof;
                    prof.name = name;
                    prof.ipd = p.value("ipd", 0.064f);
                    prof.world_scale = p.value("world_scale", 1.0f);
                    prof.convergence_distance = p.value("convergence_distance", 5.0f);
                    prof.eye_height = p.value("eye_height", 1.6f);
                    prof.enable_head_tracking = p.value("enable_head_tracking", true);
                    prof.fov_override = p.value("fov_override", 0.0f);
                    prof.foveated_rendering = p.value("foveated_rendering", false);
                    prof.dynamic_resolution = p.value("dynamic_resolution", true);
                    prof.render_scale = p.value("render_scale", 1.0f);
                    prof.input_profile = p.value("input_profile", "default_vr");
                    profiles_[name] = prof;
                }
            }
            if (j.contains("current_profile") && profiles_.contains(j["current_profile"])) {
                current_ = profiles_[j["current_profile"]];
            }
            enable_overlay_ = j.value("enable_overlay", true);
            loaded = current_;
            ok = true;
            Log::info("Config loaded from {}", path.string());
        } catch (const std::exception& e) {
            Log::error("Failed to load config: {}", e.what());
            set_default_config();
            return false;
        }
    }
    // Apply outside the lock so subsystem calls can't deadlock against Config.
    if (ok) apply_to_subsystems(loaded);
    return ok;
}

bool Config::save(const std::filesystem::path& path) const {
    std::lock_guard lock(mutex_);
    try {
        nlohmann::json j;
        j["current_profile"] = current_.name;
        j["enable_overlay"] = enable_overlay_;
        nlohmann::json profiles_obj;
        for (auto& [name, prof] : profiles_) {
            // Use current_ for the active profile — slider edits update current_
            // but don't write back to the profiles_ map until save.
            const ConfigProfile& effective = (name == current_.name) ? current_ : prof;
            nlohmann::json p;
            p["ipd"] = effective.ipd;
            p["world_scale"] = effective.world_scale;
            p["convergence_distance"] = effective.convergence_distance;
            p["eye_height"] = effective.eye_height;
            p["enable_head_tracking"] = effective.enable_head_tracking;
            p["fov_override"] = effective.fov_override;
            p["foveated_rendering"] = effective.foveated_rendering;
            p["dynamic_resolution"] = effective.dynamic_resolution;
            p["render_scale"] = effective.render_scale;
            p["input_profile"] = effective.input_profile;
            profiles_obj[name] = p;
        }
        j["profiles"] = profiles_obj;
        std::ofstream ofs(path);
        ofs << j.dump(4);
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
            auto j = nlohmann::json::parse(ifs);
            prof.name = name;
            prof.ipd = j.value("ipd", 0.064f);
            prof.world_scale = j.value("world_scale", 1.0f);
            prof.convergence_distance = j.value("convergence_distance", 5.0f);
            prof.eye_height = j.value("eye_height", 1.6f);
            prof.enable_head_tracking = j.value("enable_head_tracking", true);
            prof.fov_override = j.value("fov_override", 0.0f);
            prof.foveated_rendering = j.value("foveated_rendering", false);
            prof.dynamic_resolution = j.value("dynamic_resolution", true);
            prof.render_scale = j.value("render_scale", 1.0f);
            prof.input_profile = j.value("input_profile", "default_vr");
            profiles_[name] = prof;
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
    auto it = profiles_.find(name);
    if (it == profiles_.end()) {
        Log::warn("Profile {} not found, creating new", name);
        return false;
    }
    try {
        const auto& prof = it->second;
        nlohmann::json j;
        j["ipd"] = prof.ipd;
        j["world_scale"] = prof.world_scale;
        j["convergence_distance"] = prof.convergence_distance;
        j["eye_height"] = prof.eye_height;
        j["enable_head_tracking"] = prof.enable_head_tracking;
        j["fov_override"] = prof.fov_override;
        j["foveated_rendering"] = prof.foveated_rendering;
        j["dynamic_resolution"] = prof.dynamic_resolution;
        j["render_scale"] = prof.render_scale;
        j["input_profile"] = prof.input_profile;
        std::ofstream ofs(path);
        ofs << j.dump(4);
        Log::info("Saved profile: {}", name);
        return true;
    } catch (const std::exception& e) {
        Log::error("Failed to save profile {}: {}", name, e.what());
        return false;
    }
}

void Config::set_current_profile(const ConfigProfile& profile) {
    std::lock_guard lock(mutex_);
    current_ = profile;
    profiles_[profile.name] = profile;
}

ConfigProfile& Config::current_profile() {
    std::lock_guard lock(mutex_);
    return current_;
}

const ConfigProfile& Config::current_profile() const {
    return current_;
}

void Config::set_default_config() {
    current_ = ConfigProfile{};
    profiles_["default"] = current_;
}

std::vector<std::string> Config::available_profiles() const {
    std::vector<std::string> names;
    for (auto& [name, _] : profiles_) {
        names.push_back(name);
    }
    return names;
}

void Config::apply_to_subsystems(const ConfigProfile& prof) {
    CameraRig::instance().set_eye_separation(prof.ipd);
    CameraRig::instance().set_convergence_distance(prof.convergence_distance);
    CameraRig::instance().set_world_scale(prof.world_scale);
    TrackingSystem::instance().set_eye_height(prof.eye_height);
}

} // namespace vrc
