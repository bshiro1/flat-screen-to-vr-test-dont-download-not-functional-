#pragma once

#include "types.h"
#include <string>
#include <unordered_map>
#include <filesystem>
#include <mutex>

namespace vrc {

class Config {
public:
    static Config& instance();

    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;
    bool load_profile(const std::string& name);
    bool save_profile(const std::string& name) const;

    void set_current_profile(const ConfigProfile& profile);
    ConfigProfile& current_profile();
    const ConfigProfile& current_profile() const;

    void set_default_config();

    std::vector<std::string> available_profiles() const;

    GraphicsAPI detected_api() const { return detected_api_; }
    void set_detected_api(GraphicsAPI api) { detected_api_ = api; }

    LatencyStats& latency_stats() { return latency_stats_; }
    const LatencyStats& latency_stats() const { return latency_stats_; }

    bool enable_overlay() const { return enable_overlay_; }
    void set_enable_overlay(bool v) { enable_overlay_ = v; }

    std::filesystem::path config_dir() const;
    std::filesystem::path profiles_dir() const;
    std::filesystem::path log_path() const;

private:
    Config() = default;
    std::unordered_map<std::string, ConfigProfile> profiles_;
    ConfigProfile current_;
    GraphicsAPI detected_api_ = GraphicsAPI::Unknown;
    LatencyStats latency_stats_;
    bool enable_overlay_ = true;
    mutable std::mutex mutex_;
    std::filesystem::path config_file_;
};

} // namespace vrc
