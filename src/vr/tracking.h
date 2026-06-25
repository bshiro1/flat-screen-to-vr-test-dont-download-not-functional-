#pragma once

#include "core/types.h"
#include "render/latency_compensator.h"
#include <functional>
#include <deque>
#include <mutex>
#include <array>

namespace vrc {

struct TrackingSample {
    Vec3  position;
    Quat  rotation;
    Vec3  linear_velocity;
    Vec3  angular_velocity;
    f64   timestamp_ms;
    u64   sequence;
};

enum class TrackingOrigin : u8 {
    Local,    // Seated / standing, recenterable
    Stage,    // Room-scale, fixed origin
    View      // Head-locked
};

class TrackingSystem {
public:
    static TrackingSystem& instance();

    void initialize(f32 ipd = 0.064f);
    void shutdown();

    // ─── Pose Management ─────────────────────────────────────────────

    HeadPose get_head_pose() const;
    void set_head_pose(const Vec3& pos, const Quat& rot);
    void set_head_pose_from_openxr(const Vec3& pos, const Quat& rot,
                                    f64 timestamp_ms);

    void apply_latency_compensation(f64 render_timestamp_ms);

    // Kalman-filtered prediction
    HeadPose predict_pose_at(f64 target_timestamp_ms) const;
    void update_kalman_filter(const HeadPose& measurement);

    // ─── Predictive Filtering ────────────────────────────────────────

    void set_prediction_enabled(bool enabled) { prediction_enabled_ = enabled; }
    bool prediction_enabled() const { return prediction_enabled_; }

    f32 ipd() const { return ipd_; }
    void set_ipd(f32 ipd) { ipd_ = ipd; }

    // ─── Tracking Quality ─────────────────────────────────────────────

    bool is_tracking() const { return tracking_active_; }
    f32 tracking_confidence() const { return confidence_; }
    void set_tracking_quality(f32 confidence) { confidence_ = confidence; }

    // ─── Calibration / Origin ────────────────────────────────────────

    void recenter();
    void reset_origin();

    TrackingOrigin origin_mode() const { return origin_mode_; }
    void set_origin_mode(TrackingOrigin mode);

    // Room-scale bounds
    bool has_room_bounds() const { return room_bounds_valid_; }
    void set_room_bounds(f32 width, f32 depth);
    Vec2 room_bounds() const { return room_bounds_; }

    // Seated/standing height
    f32 eye_height() const { return eye_height_; }
    void set_eye_height(f32 h) { eye_height_ = h; }

private:
    TrackingSystem() = default;

    HeadPose predict_pose_linear(f64 target_timestamp_ms) const;
    void update_history(const HeadPose& pose);
    void compute_angular_velocity();

    f32 ipd_ = 0.064f;
    f32 eye_height_ = 1.6f;      // Average seated eye height
    bool tracking_active_ = false;
    bool prediction_enabled_ = true;
    f32 confidence_ = 1.0f;
    TrackingOrigin origin_mode_ = TrackingOrigin::Local;

    mutable std::mutex mutex_;
    HeadPose current_pose_;
    Vec3 angular_velocity_{};
    Quat yaw_offset_{ 0, 0, 0, 1 };   // inverse yaw applied after recenter()
    std::deque<TrackingSample> history_;
    static constexpr size_t kMaxHistory = 256;
    static constexpr f32 kPredictionWindowMs = 30.0f;

    // Room-scale
    bool room_bounds_valid_ = false;
    Vec2 room_bounds_{}; // width, depth

    // Reference to latency compensator for Kalman integration
    LatencyCompensator& latency_ = LatencyCompensator::instance();
};

} // namespace vrc
