#pragma once

#include "core/types.h"
#include <array>
#include <deque>
#include <mutex>

namespace vrc {

struct KalmanState {
    Vec3 x;       // Position estimate
    Vec3 v;       // Velocity estimate
    Matrix4 P;    // Error covariance (6x6 flattened; we store 3x3 pos + 3x3 vel)
    f32 pos_noise = 0.01f;
    f32 vel_noise = 0.1f;
    f32 meas_noise = 0.05f;

    void predict(f32 dt);
    void update(const Vec3& measurement);
};

struct LatencySnapshot {
    f64 sample_time_ms;
    f64 tracking_sample_ms;
    f64 render_submit_ms;
    f64 gpu_complete_ms;
    f64 present_ms;
    f64 display_ms;
    f64 total_motion_to_photon_ms;
    u64 frame_index;
};

class LatencyCompensator {
public:
    static LatencyCompensator& instance();

    void initialize();
    void shutdown();

    // ─── Motion-to-Photon Pipeline ───────────────────────────────────

    void begin_frame(u64 frame_index);
    void on_tracking_sample(f64 sample_time_ms);
    void on_render_submit();
    void on_gpu_complete();
    void on_present();
    void end_frame();

    LatencySnapshot latest_snapshot() const;
    f64 current_motion_to_photon_ms() const;

    // ─── Kalman Filter Pose Prediction ───────────────────────────────

    HeadPose predict_head_pose_at(f64 target_display_time_ms,
                                  const HeadPose& latest_pose) const;

    void update_kalman(const HeadPose& measurement);

    // ─── Asynchronous Timewarp ───────────────────────────────────────

    struct TimewarpData {
        Matrix4 reprojection_matrix;   // Game world → VR eye
        Vec3    hmd_position;
        Quat    hmd_rotation;
        f64     timestamp_ms;
        bool    valid = false;
    };

    void set_timewarp_data(const TimewarpData& data);
    TimewarpData get_timewarp_data() const;
    bool needs_timewarp() const;

    // ─── Late-Stage Reprojection ─────────────────────────────────────

    void update_latest_pose_for_atw(const HeadPose& latest_pose);
    Matrix4 compute_atw_matrix(const Matrix4& original_view_proj,
                                const HeadPose& rendered_pose,
                                const HeadPose& current_pose) const;

    // ─── VR Comfort Metrics ──────────────────────────────────────────

    f32 frame_rate() const { return frame_rate_; }
    f32 motion_to_photon_smoothed() const { return mtp_smoothed_ms_; }

    void set_prediction_window(f32 ms) { prediction_window_ms_ = ms; }
    f32 prediction_window() const { return prediction_window_ms_; }

    void set_motion_smoothing(bool v) { motion_smoothing_ = v; }
    bool motion_smoothing() const { return motion_smoothing_; }

private:
    LatencyCompensator() = default;

    f32 frame_rate_ = 0.0f;
    f32 mtp_smoothed_ms_ = 0.0f;
    f32 prediction_window_ms_ = 30.0f;
    bool motion_smoothing_ = true;

    mutable std::mutex mutex_;

    // Pipeline timestamps (ms, steady clock)
    f64 ts_tracking_ = 0.0;
    f64 ts_render_submit_ = 0.0;
    f64 ts_gpu_complete_ = 0.0;
    f64 ts_present_ = 0.0;
    u64 current_frame_index_ = 0;

    KalmanState kalman_;

    // Angular velocity (rad/s) estimated from quaternion history
    Vec3 angular_velocity_{};
    f64 last_rotation_timestamp_ms_ = 0.0;
    Quat last_rotation_{};

    std::deque<LatencySnapshot> history_;
    static constexpr size_t kMaxHistory = 256;

    TimewarpData timewarp_data_;
    HeadPose latest_pose_for_atw_;

    // Quaternion + angular velocity history for smoothing
    struct RotationSample {
        Quat rotation;
        f64  timestamp_ms;
    };
    std::deque<RotationSample> rotation_history_;
    static constexpr size_t kMaxRotationHistory = 16;
};

} // namespace vrc
