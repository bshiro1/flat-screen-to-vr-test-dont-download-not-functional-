#pragma once

#include "types.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <atomic>
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
    // Always returns a locked snapshot. Callers that edit it must write it back
    // with set_current_profile(); this prevents unsynchronized mutation while
    // the XR/Present threads are reading config.
    ConfigProfile current_profile() const;
    ConfigProfile current_profile_copy() const { return current_profile(); }

    void set_default_config();

    std::vector<std::string> available_profiles() const;

    GraphicsAPI detected_api() const { return detected_api_; }
    void set_detected_api(GraphicsAPI api) { detected_api_ = api; }

    // Written by the Present thread, read by the overlay: always copy under lock.
    LatencyStats latency_stats() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return latency_stats_;
    }
    void set_latency_stats(const LatencyStats& stats) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        latency_stats_ = stats;
    }

    bool enable_overlay() const { return enable_overlay_.load(std::memory_order_relaxed); }
    void set_enable_overlay(bool v) { enable_overlay_.store(v, std::memory_order_relaxed); }

    std::filesystem::path config_dir() const;
    std::filesystem::path profiles_dir() const;
    std::filesystem::path log_path() const;

private:
    Config() = default;
    // Loads the InputMapper profile named by the newly active VR profile.
    // Called without mutex_ held.
    void apply_to_subsystems(const ConfigProfile& prof);
    // Caller must already hold mutex_. Used by load() error paths so default
    // setup cannot self-deadlock on the non-recursive mutex.
    void set_default_config_unlocked();
    std::unordered_map<std::string, ConfigProfile> profiles_;
    // The JSON objects read from disk (top level, and one per profile), kept
    // as serialized text so this header does not depend on nlohmann/json.
    // save() starts from them and overwrites only the keys this DLL owns, so
    // keys written by the companion app (or by older versions) survive.
    std::string raw_root_json_;
    std::unordered_map<std::string, std::string> raw_profile_json_;
    ConfigProfile current_;
    GraphicsAPI detected_api_ = GraphicsAPI::Unknown;
    LatencyStats latency_stats_;
    mutable std::mutex stats_mutex_;
    std::atomic<bool> enable_overlay_{true};
    mutable std::mutex mutex_;
    std::filesystem::path config_file_;
};

} // namespace vrc
