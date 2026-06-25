#include "tracking.h"
#include "core/logging.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace vrc {

TrackingSystem& TrackingSystem::instance() {
    static TrackingSystem sys;
    return sys;
}

void TrackingSystem::initialize(f32 ipd) {
    ipd_ = ipd;
    tracking_active_ = true;
    confidence_ = 1.0f;
    origin_mode_ = TrackingOrigin::Local;
    Log::info("Tracking system initialized (IPD: {:.4f}m, origin: local)", ipd_);
}

void TrackingSystem::shutdown() {
    tracking_active_ = false;
    history_.clear();
    Log::info("Tracking system shut down");
}

void TrackingSystem::set_head_pose(const Vec3& pos, const Quat& rot) {
    std::lock_guard lock(mutex_);

    auto now_ms = static_cast<f64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;

    HeadPose pose;
    pose.position = pos;
    pose.rotation = rot;
    pose.timestamp_ms = now_ms;
    pose.confidence = 1.0;
    current_pose_ = pose;

    update_history(pose);
    compute_angular_velocity();
}

void TrackingSystem::set_head_pose_from_openxr(const Vec3& pos, const Quat& rot,
                                                f64 timestamp_ms)
{
    std::lock_guard lock(mutex_);

    HeadPose pose;
    pose.position = pos;
    pose.rotation = rot;
    pose.timestamp_ms = timestamp_ms;
    pose.confidence = 1.0f; // Confidence from OpenXR view state would go here

    // Apply yaw recenter offset
    pose.rotation = (yaw_offset_ * pose.rotation).normalized();

    // Apply origin offset based on mode
    if (origin_mode_ == TrackingOrigin::Local) {
        // Apply eye height offset for seated mode
        pose.position.y += eye_height_;
    }

    current_pose_ = pose;
    update_history(pose);
    compute_angular_velocity();

    // Feed into Kalman filter
    latency_.on_tracking_sample(timestamp_ms);
    latency_.update_kalman(pose);
}

HeadPose TrackingSystem::get_head_pose() const {
    std::lock_guard lock(mutex_);
    return current_pose_;
}

void TrackingSystem::apply_latency_compensation(f64 render_timestamp_ms) {
    if (!prediction_enabled_) return;
    HeadPose predicted = predict_pose_at(render_timestamp_ms);

    std::lock_guard lock(mutex_);
    current_pose_ = predicted;
}

HeadPose TrackingSystem::predict_pose_at(f64 target_timestamp_ms) const {
    std::lock_guard lock(mutex_);

    // Use Kalman-predicted pose from latency compensator
    HeadPose predicted = latency_.predict_head_pose_at(
        target_timestamp_ms, current_pose_);

    // If Kalman prediction isn't available, fall back to linear
    if (predicted.confidence < 0.1f) {
        predicted = predict_pose_linear(target_timestamp_ms);
    }

    return predicted;
}

void TrackingSystem::update_kalman_filter(const HeadPose& measurement) {
    latency_.update_kalman(measurement);
}

HeadPose TrackingSystem::predict_pose_linear(f64 target_timestamp_ms) const {
    if (history_.size() < 3) return current_pose_;

    auto& newest = history_.back();
    auto& oldest = history_.front();

    f64 dt = newest.timestamp_ms - oldest.timestamp_ms;
    if (dt < 0.001) return current_pose_;

    f64 target_dt = target_timestamp_ms - newest.timestamp_ms;
    if (target_dt < 0.0) return current_pose_;

    HeadPose predicted;
    predicted.position.x = newest.position.x +
        (newest.linear_velocity.x * static_cast<f32>(target_dt / 1000.0));
    predicted.position.y = newest.position.y +
        (newest.linear_velocity.y * static_cast<f32>(target_dt / 1000.0));
    predicted.position.z = newest.position.z +
        (newest.linear_velocity.z * static_cast<f32>(target_dt / 1000.0));

    predicted.rotation = newest.rotation;

    // Apply angular velocity to rotation (simplified slerp extrapolation)
    f32 angle = sqrtf(angular_velocity_.x * angular_velocity_.x +
                      angular_velocity_.y * angular_velocity_.y +
                      angular_velocity_.z * angular_velocity_.z)
                * static_cast<f32>(target_dt / 1000.0);
    if (angle > 0.001f) {
        predicted.rotation = quat_extrapolate(newest.rotation, angular_velocity_,
                                               static_cast<f32>(target_dt / 1000.0));
    }

    predicted.timestamp_ms = target_timestamp_ms;
    predicted.confidence = std::max(0.0f, 1.0f - static_cast<f32>(target_dt / 100.0f));

    return predicted;
}

void TrackingSystem::update_history(const HeadPose& pose) {
    TrackingSample sample;
    sample.position = pose.position;
    sample.rotation = pose.rotation;
    sample.timestamp_ms = pose.timestamp_ms;

    if (!history_.empty()) {
        auto& prev = history_.back();
        f64 dt = (sample.timestamp_ms - prev.timestamp_ms) / 1000.0;
        if (dt > 0.001) {
            sample.linear_velocity = Vec3(
                (sample.position.x - prev.position.x) / static_cast<f32>(dt),
                (sample.position.y - prev.position.y) / static_cast<f32>(dt),
                (sample.position.z - prev.position.z) / static_cast<f32>(dt)
            );
        }
    }

    sample.sequence = history_.empty() ? 0 : history_.back().sequence + 1;
    history_.push_back(sample);

    if (history_.size() > kMaxHistory) {
        history_.pop_front();
    }
}

void TrackingSystem::compute_angular_velocity() {
    if (history_.size() < 2) return;

    auto& newest = history_.back();
    auto& prev = history_[history_.size() - 2];
    f64 dt = (newest.timestamp_ms - prev.timestamp_ms) / 1000.0;
    if (dt < 0.001) return;

    // Simplified angular velocity from quaternion delta
    Quat& q0 = prev.rotation;
    Quat& q1 = newest.rotation;

    // Quaternion delta: dq = q1 * q0^-1
    f32 norm = q0.x*q0.x + q0.y*q0.y + q0.z*q0.z + q0.w*q0.w;
    if (norm < 0.0001f) return;
    f32 inv_norm = 1.0f / norm;
    Quat q0_inv = { -q0.x * inv_norm, -q0.y * inv_norm,
                    -q0.z * inv_norm,  q0.w * inv_norm };

    Quat dq;
    dq.x = q1.w * q0_inv.x + q1.x * q0_inv.w + q1.y * q0_inv.z - q1.z * q0_inv.y;
    dq.y = q1.w * q0_inv.y - q1.x * q0_inv.z + q1.y * q0_inv.w + q1.z * q0_inv.x;
    dq.z = q1.w * q0_inv.z + q1.x * q0_inv.y - q1.y * q0_inv.x + q1.z * q0_inv.w;
    dq.w = q1.w * q0_inv.w - q1.x * q0_inv.x - q1.y * q0_inv.y - q1.z * q0_inv.z;

    // Extract angle from quaternion axis-angle
    f32 angle = 2.0f * acosf(std::clamp(dq.w, -1.0f, 1.0f));
    f32 sin_half = sinf(angle / 2.0f);
    if (sin_half > 0.001f) {
        angular_velocity_.x = (dq.x / sin_half) * angle / static_cast<f32>(dt);
        angular_velocity_.y = (dq.y / sin_half) * angle / static_cast<f32>(dt);
        angular_velocity_.z = (dq.z / sin_half) * angle / static_cast<f32>(dt);
    }

    newest.angular_velocity = angular_velocity_;
}

// ─── Calibration ────────────────────────────────────────────────────────────

void TrackingSystem::recenter() {
    std::lock_guard lock(mutex_);

    // Extract only the yaw component from the current rotation
    Quat q = current_pose_.rotation.normalized();
    f32 yaw = atan2f(2.0f * (q.w * q.y + q.x * q.z),
                     1.0f - 2.0f * (q.y * q.y + q.z * q.z));

    // Build a pure-yaw quaternion (rotation around Y) and store its inverse
    f32 hy = yaw * 0.5f;
    Quat yaw_q = { 0.0f, sinf(hy), 0.0f, cosf(hy) };
    yaw_offset_ = yaw_q.conjugated();

    Log::info("Tracking recentered (yaw offset: {:.2f} deg)", yaw * kRadToDeg);
}

void TrackingSystem::reset_origin() {
    std::lock_guard lock(mutex_);
    history_.clear();
    current_pose_ = HeadPose{};
    angular_velocity_ = Vec3{};
    yaw_offset_ = { 0, 0, 0, 1 };
    Log::info("Tracking origin reset");
}

void TrackingSystem::set_origin_mode(TrackingOrigin mode) {
    std::lock_guard lock(mutex_);
    origin_mode_ = mode;
    Log::info("Tracking origin mode set to {}", static_cast<int>(mode));
}

void TrackingSystem::set_room_bounds(f32 width, f32 depth) {
    std::lock_guard lock(mutex_);
    room_bounds_ = Vec2(width, depth);
    room_bounds_valid_ = true;
    Log::info("Room bounds set: {}m x {}m", width, depth);
}

} // namespace vrc
