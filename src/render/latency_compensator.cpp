#include "latency_compensator.h"
#include "core/logging.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace vrc {

// ─── Kalman Filter ──────────────────────────────────────────────────────────

void KalmanState::predict(f32 dt) {
    // Simple constant-velocity prediction
    x.x += v.x * dt;
    x.y += v.y * dt;
    x.z += v.z * dt;

    // Update position covariance diagonal only.
    // P is Matrix4 (4x4, 16 elements) — not a full 6x6 state covariance.
    // update() only reads the position diagonal (m[0], m[5], m[10]).
    for (int i = 0; i < 3; i++) {
        P.m[i * 4 + i] += pos_noise * dt * dt;
    }
}

void KalmanState::update(const Vec3& measurement) {
    // Compute Kalman gain K = P * H^T * (H * P * H^T + R)^-1
    // H = [I 0]  (we only measure position)
    f32 s0 = P.m[0] + meas_noise;   // P[0][0] + R
    f32 s1 = P.m[5] + meas_noise;   // P[1][1] + R
    f32 s2 = P.m[10] + meas_noise;  // P[2][2] + R

    if (std::abs(s0) < 1e-10f || std::abs(s1) < 1e-10f || std::abs(s2) < 1e-10f)
        return;

    f32 k0 = P.m[0] / s0;
    f32 k1 = P.m[5] / s1;
    f32 k2 = P.m[10] / s2;

    f32 y0 = measurement.x - x.x;
    f32 y1 = measurement.y - x.y;
    f32 y2 = measurement.z - x.z;

    x.x += k0 * y0;
    x.y += k1 * y1;
    x.z += k2 * y2;

    // Update covariance: P = (I - K*H) * P
    P.m[0]  *= (1.0f - k0);
    P.m[5]  *= (1.0f - k1);
    P.m[10] *= (1.0f - k2);
}

// ─── Latency Compensator ────────────────────────────────────────────────────

LatencyCompensator& LatencyCompensator::instance() {
    static LatencyCompensator comp;
    return comp;
}

void LatencyCompensator::initialize() {
    Log::info("Latency compensator initialized");
    kalman_ = KalmanState{};
    history_.clear();
    frame_rate_ = 0.0f;
    mtp_smoothed_ms_ = 0.0f;
}

void LatencyCompensator::shutdown() {
    Log::info("Latency compensator shut down");
}

void LatencyCompensator::begin_frame(u64 frame_index) {
    std::lock_guard lock(mutex_);
    current_frame_index_ = frame_index;
    ts_render_submit_ = 0.0;
    ts_gpu_complete_ = 0.0;
}

void LatencyCompensator::on_tracking_sample(f64 sample_time_ms) {
    std::lock_guard lock(mutex_);
    ts_tracking_ = sample_time_ms;
}

void LatencyCompensator::on_render_submit() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ts_render_submit_ = static_cast<f64>(now) / 1000.0;
}

void LatencyCompensator::on_gpu_complete() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ts_gpu_complete_ = static_cast<f64>(now) / 1000.0;
}

void LatencyCompensator::on_present() {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ts_present_ = static_cast<f64>(now) / 1000.0;
}

void LatencyCompensator::end_frame() {
    std::lock_guard lock(mutex_);

    f64 now = static_cast<f64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;

    LatencySnapshot snap{};
    snap.sample_time_ms = now;
    snap.tracking_sample_ms = ts_tracking_;
    snap.render_submit_ms = ts_render_submit_;
    snap.gpu_complete_ms = ts_gpu_complete_;
    snap.present_ms = ts_present_;
    snap.display_ms = now;
    snap.frame_index = current_frame_index_;

    // Motion-to-photon: from tracking sample to display
    f64 tracking_to_display = now - ts_tracking_;
    snap.total_motion_to_photon_ms = std::max(0.0, tracking_to_display);

    history_.push_back(snap);
    if (history_.size() > kMaxHistory)
        history_.pop_front();

    // Compute smoothed motion-to-photon
    if (history_.size() >= 10) {
        f64 sum = 0.0;
        auto it = history_.end() - 10;
        for (int i = 0; i < 10; i++, it++) {
            sum += it->total_motion_to_photon_ms;
        }
        mtp_smoothed_ms_ = static_cast<f32>(sum / 10.0);
    }

    // Compute frame rate
    if (history_.size() >= 2) {
        f64 dt = history_.back().sample_time_ms - history_.front().sample_time_ms;
        f64 count = static_cast<f64>(history_.size() - 1);
        if (dt > 0.0) {
            frame_rate_ = static_cast<f32>(count / dt * 1000.0);
        }
    }
}

LatencySnapshot LatencyCompensator::latest_snapshot() const {
    std::lock_guard lock(mutex_);
    if (history_.empty()) return LatencySnapshot{};
    return history_.back();
}

f64 LatencyCompensator::current_motion_to_photon_ms() const {
    std::lock_guard lock(mutex_);
    return mtp_smoothed_ms_;
}

HeadPose LatencyCompensator::predict_head_pose_at(
    f64 target_display_time_ms, const HeadPose& latest_pose) const
{
    std::lock_guard lock(mutex_);
    HeadPose predicted = latest_pose;

    f64 dt = (target_display_time_ms - latest_pose.timestamp_ms) / 1000.0;
    if (dt <= 0.0 || dt > 0.1) return predicted;

    // Apply Kalman-predicted position offset
    predicted.position.x += kalman_.v.x * static_cast<f32>(dt);
    predicted.position.y += kalman_.v.y * static_cast<f32>(dt);
    predicted.position.z += kalman_.v.z * static_cast<f32>(dt);

    // Quaternion slerp extrapolation using estimated angular velocity
    // If motion smoothing is enabled, use the smoothed angular velocity
    if (motion_smoothing_) {
        predicted.rotation = quat_extrapolate(latest_pose.rotation,
                                               angular_velocity_,
                                               static_cast<f32>(dt));
    }

    predicted.confidence = std::max(0.0, 1.0 - std::abs(dt) / 0.1);

    return predicted;
}

void LatencyCompensator::update_kalman(const HeadPose& measurement) {
    std::lock_guard lock(mutex_);

    // ─── Position Kalman filter ───────────────────────────────────────
    if (history_.empty()) {
        kalman_.x = measurement.position;
        kalman_.v = Vec3{};
    } else {
        f64 dt = (measurement.timestamp_ms - history_.back().tracking_sample_ms) / 1000.0;
        if (dt > 0.001) {
            kalman_.predict(static_cast<f32>(dt));
        }
        kalman_.update(measurement.position);
    }

    // ─── Rotation angular velocity estimation ─────────────────────────
    if (rotation_history_.empty()) {
        last_rotation_ = measurement.rotation;
        last_rotation_timestamp_ms_ = measurement.timestamp_ms;
    } else {
        f32 dt_rot = static_cast<f32>(
            (measurement.timestamp_ms - last_rotation_timestamp_ms_) / 1000.0);
        if (dt_rot > 0.001f) {
            // Compute instantaneous angular velocity
            Vec3 inst_ang_vel = angular_velocity_between(
                last_rotation_, measurement.rotation, dt_rot);

            // Smooth with exponential moving average
            f32 alpha = 0.3f; // Weight for new sample
            angular_velocity_.x += alpha * (inst_ang_vel.x - angular_velocity_.x);
            angular_velocity_.y += alpha * (inst_ang_vel.y - angular_velocity_.y);
            angular_velocity_.z += alpha * (inst_ang_vel.z - angular_velocity_.z);

            last_rotation_ = measurement.rotation;
            last_rotation_timestamp_ms_ = measurement.timestamp_ms;
        }
    }

    // Maintain rotation history for smoothing
    rotation_history_.push_back({ measurement.rotation, measurement.timestamp_ms });
    if (rotation_history_.size() > kMaxRotationHistory) {
        rotation_history_.pop_front();
    }
}

void LatencyCompensator::set_timewarp_data(const TimewarpData& data) {
    std::lock_guard lock(mutex_);
    timewarp_data_ = data;
}

LatencyCompensator::TimewarpData LatencyCompensator::get_timewarp_data() const {
    std::lock_guard lock(mutex_);
    return timewarp_data_;
}

bool LatencyCompensator::needs_timewarp() const {
    std::lock_guard lock(mutex_);
    if (!timewarp_data_.valid) return false;
    f64 age = static_cast<f64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    age -= timewarp_data_.timestamp_ms;
    return (age > 2.0); // Timewarp if more than 2ms old
}

void LatencyCompensator::update_latest_pose_for_atw(const HeadPose& latest_pose) {
    std::lock_guard lock(mutex_);
    latest_pose_for_atw_ = latest_pose;
}

Matrix4 LatencyCompensator::compute_atw_matrix(
    const Matrix4& original_view_proj,
    const HeadPose& rendered_pose,
    const HeadPose& current_pose) const
{
    // Late-Stage Reprojection:
    // Adjust the view-projection matrix by the delta between
    // the pose used for rendering and the latest available pose.
    //
    // ATW correction: V' = V_current * V_rendered^-1 * V_rendered_original
    // Simplified: reproject using the pose delta

    Matrix4 delta = Matrix4::identity();

    Vec3 pos_delta{
        current_pose.position.x - rendered_pose.position.x,
        current_pose.position.y - rendered_pose.position.y,
        current_pose.position.z - rendered_pose.position.z,
    };

    // Row-major layout: translation lives at m[12..14], not m[3/7/11]
    delta.m[12] = pos_delta.x;
    delta.m[13] = pos_delta.y;
    delta.m[14] = pos_delta.z;

    return multiply(original_view_proj, delta);
}

} // namespace vrc
