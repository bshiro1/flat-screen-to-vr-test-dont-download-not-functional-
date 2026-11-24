#pragma once

#include "core/types.h"
#include <mutex>

namespace vrc {

// Latest head pose for consumers outside the OpenXR frame thread (InputMapper's
// head-aim reads it). The frame thread publishes the head-centre pose in LOCAL
// space right after xrLocateViews, before InputMapper::process_frame runs.
class TrackingSystem {
public:
    static TrackingSystem& instance();

    void initialize(f32 ipd = 0.064f);
    void shutdown();

    HeadPose get_head_pose() const;
    void set_head_pose(const Vec3& position, const Quat& rotation, f64 timestamp_ms);

    bool is_tracking() const;

private:
    TrackingSystem() = default;

    mutable std::mutex mutex_;
    HeadPose pose_{};
    bool tracking_ = false;
};

} // namespace vrc
