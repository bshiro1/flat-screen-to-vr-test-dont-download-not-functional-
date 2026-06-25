#include "perf_monitor.h"
#include "logging.h"
#include <algorithm>
#include <numeric>

namespace vrc {

PerfMonitor& PerfMonitor::instance() {
    static PerfMonitor mon;
    return mon;
}

void PerfMonitor::initialize() {
    std::lock_guard lock(mutex_);
    Log::info("Performance monitor initialized");
    reset();
}

void PerfMonitor::shutdown() {
    Log::info("Performance monitor shut down");
}

void PerfMonitor::begin_frame(u64 frame_index) {
    std::lock_guard lock(mutex_);
    frame_index_ = frame_index;
    total_frame_count_++;

    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    t_frame_start_ = static_cast<f64>(now) / 1000.0;
}

void PerfMonitor::end_cpu_work() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    t_cpu_end_ = static_cast<f64>(now) / 1000.0;
}

void PerfMonitor::end_gpu_work() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    t_gpu_end_ = static_cast<f64>(now) / 1000.0;
}

void PerfMonitor::end_frame() {
    std::lock_guard lock(mutex_);

    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    t_frame_end_ = static_cast<f64>(now) / 1000.0;

    PerfSample sample{};
    sample.frame_start_ms = t_frame_start_;
    sample.cpu_work_ms = t_cpu_end_ - t_frame_start_;
    sample.gpu_work_ms = t_gpu_end_ - t_cpu_end_;
    sample.present_ms = t_frame_end_ - t_gpu_end_;
    sample.frame_total_ms = t_frame_end_ - t_frame_start_;
    sample.frame_index = frame_index_;

    history_.push_back(sample);
    if (history_.size() > kMaxHistory) {
        auto& oldest = history_.front();
        sum_frame_ms_ -= oldest.frame_total_ms;
        sum_cpu_ms_ -= oldest.cpu_work_ms;
        sum_gpu_ms_ -= oldest.gpu_work_ms;
        history_.pop_front();
    } else {
        sum_frame_ms_ += sample.frame_total_ms;
        sum_cpu_ms_ += sample.cpu_work_ms;
        sum_gpu_ms_ += sample.gpu_work_ms;
    }

    min_frame_ms_ = std::min(min_frame_ms_, sample.frame_total_ms);
    max_frame_ms_ = std::max(max_frame_ms_, sample.frame_total_ms);

    // 11.1ms = 90fps threshold
    if (sample.frame_total_ms > 11.1) {
        dropped_frame_count_++;
    }
}

u32 PerfMonitor::fps() const {
    std::lock_guard lock(mutex_);
    if (history_.size() < 2) return 0;

    f64 total_time = history_.back().frame_start_ms - history_.front().frame_start_ms;
    if (total_time <= 0.0) return 0;

    return static_cast<u32>((history_.size() - 1) / total_time * 1000.0);
}

f64 PerfMonitor::avg_cpu_ms() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return 0.0;
    return sum_cpu_ms_ / history_.size();
}

f64 PerfMonitor::avg_gpu_ms() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return 0.0;
    return sum_gpu_ms_ / history_.size();
}

f64 PerfMonitor::avg_frame_ms() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return 0.0;
    return sum_frame_ms_ / history_.size();
}

f64 PerfMonitor::min_frame_ms() const {
    std::lock_guard lock(mutex_);
    return min_frame_ms_;
}

f64 PerfMonitor::max_frame_ms() const {
    std::lock_guard lock(mutex_);
    return max_frame_ms_;
}

f64 PerfMonitor::percentile_99_frame_ms() const {
    std::lock_guard lock(mutex_);
    if (history_.size() < 100) return max_frame_ms_;

    std::vector<f64> times;
    times.reserve(history_.size());
    for (auto& s : history_) {
        times.push_back(s.frame_total_ms);
    }
    std::sort(times.begin(), times.end());

    size_t idx = static_cast<size_t>(times.size() * 0.99);
    return times[std::min(idx, times.size() - 1)];
}

bool PerfMonitor::is_comfortable() const {
    u32 current_fps = fps();
    return (current_fps >= 90);
}

f32 PerfMonitor::dropped_frames() const {
    std::lock_guard lock(mutex_);
    if (total_frame_count_ == 0) return 0.0f;
    return static_cast<f32>(dropped_frame_count_) / static_cast<f32>(total_frame_count_);
}

std::vector<f64> PerfMonitor::recent_frame_times(u32 count) const {
    std::lock_guard lock(mutex_);
    std::vector<f64> result;
    result.reserve(std::min(count, static_cast<u32>(history_.size())));

    auto start = history_.end() - std::min(static_cast<size_t>(count), history_.size());
    for (auto it = start; it != history_.end(); it++) {
        result.push_back(it->frame_total_ms);
    }
    return result;
}

void PerfMonitor::reset() {
    history_.clear();
    sum_frame_ms_ = 0.0;
    sum_cpu_ms_ = 0.0;
    sum_gpu_ms_ = 0.0;
    min_frame_ms_ = 1000.0;
    max_frame_ms_ = 0.0;
    dropped_frame_count_ = 0;
    total_frame_count_ = 0;
}

LatencyStats PerfMonitor::get_latency_stats() const {
    LatencyStats stats;
    stats.fps = fps();
    stats.present_latency_ms = avg_gpu_ms() + avg_frame_ms() * 0.5;
    stats.render_latency_ms = avg_cpu_ms();
    stats.tracking_latency_ms = avg_cpu_ms() * 0.3;
    stats.total_motion_to_photon_ms = avg_frame_ms() + avg_gpu_ms();
    return stats;
}

} // namespace vrc
