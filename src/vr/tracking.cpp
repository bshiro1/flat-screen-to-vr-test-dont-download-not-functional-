#include "tracking.h"
#include "core/logging.h"

namespace vrc {

TrackingSystem& TrackingSystem::instance() {
    static TrackingSystem sys;
    return sys;
}

void TrackingSystem::initialize(f32 ipd) {
    std::lock_guard lock(mutex_);
    pose_ = HeadPose{};
    tracking_ = false;
    // Rendering uses the runtime's per-eye poses, so the configured IPD is
    // informational only.
    Log::info("Tracking system initialized (configured IPD {:.4f} m; runtime eye poses are used)",
              ipd);
}

void TrackingSystem::shutdown() {
    std::lock_guard lock(mutex_);
    pose_ = HeadPose{};
    tracking_ = false;
}

HeadPose TrackingSystem::get_head_pose() const {
    std::lock_guard lock(mutex_);
    return pose_;
}

void TrackingSystem::set_head_pose(const Vec3& position, const Quat& rotation,
                                   f64 timestamp_ms) {
    std::lock_guard lock(mutex_);
    pose_.position = position;
    pose_.rotation = rotation.normalized();
    pose_.timestamp_ms = timestamp_ms;
    pose_.confidence = 1.0;
    tracking_ = true;
}

bool TrackingSystem::is_tracking() const {
    std::lock_guard lock(mutex_);
    return tracking_;
}

} // namespace vrc
