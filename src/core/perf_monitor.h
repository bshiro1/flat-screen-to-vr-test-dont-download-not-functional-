#pragma once

#include "types.h"
#include <array>
#include <deque>
#include <mutex>
#include <chrono>

namespace vrc {

struct PerfSample {
    f64 frame_start_ms;
    f64 cpu_work_ms;
    f64 gpu_work_ms;
    f64 present_ms;
    f64 frame_total_ms;
    u64 frame_index;
};

class PerfMonitor {
public:
    static PerfMonitor& instance();

    void initialize();
    void shutdown();

    void begin_frame(u64 frame_index);
    void end_cpu_work();
    void end_gpu_work();
    void end_frame();

    // Aggregate metrics
    u32 fps() const;
    f64 avg_cpu_ms() const;
    f64 avg_gpu_ms() const;
    f64 avg_frame_ms() const;
    f64 min_frame_ms() const;
    f64 max_frame_ms() const;
    f64 percentile_99_frame_ms() const;

    // VR comfort
    bool is_comfortable() const;  // 90+ FPS, <20ms MTP
    f32 dropped_frames() const;   // Fraction of frames that miss 11ms window

    // Frame timing history
    std::vector<f64> recent_frame_times(u32 count = 120) const;

    // Reset stats
    void reset();

    // Get latest latency stats for config
    LatencyStats get_latency_stats() const;

private:
    PerfMonitor() = default;

    mutable std::mutex mutex_;

    u64 frame_index_ = 0;
    f64 last_frame_timestamp_ = 0.0;

    f64 t_frame_start_ = 0.0;
    f64 t_cpu_end_ = 0.0;
    f64 t_gpu_end_ = 0.0;
    f64 t_frame_end_ = 0.0;

    std::deque<PerfSample> history_;
    static constexpr size_t kMaxHistory = 1024;

    // Rolling aggregates
    f64 sum_frame_ms_ = 0.0;
    f64 sum_cpu_ms_ = 0.0;
    f64 sum_gpu_ms_ = 0.0;
    f64 min_frame_ms_ = 1000.0;
    f64 max_frame_ms_ = 0.0;
    u32 dropped_frame_count_ = 0;
    u32 total_frame_count_ = 0;
};

} // namespace vrc
