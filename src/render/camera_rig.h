#pragma once

#include "core/types.h"
#include "vr/tracking.h"
#include <memory>

namespace vrc {

class CameraRig {
public:
    static CameraRig& instance();

    void initialize(f32 eye_separation = 0.064f);
    void shutdown();

    // ─── Eye View Computation ────────────────────────────────────────

    EyeViews compute_eye_views(const ViewSetup& mono_setup) const;

    // ─── Head Tracking Integration ───────────────────────────────────

    ViewSetup apply_head_tracking(const ViewSetup& game_camera,
                                   const HeadPose& head_pose) const;

    // Build a proper view matrix from position + quaternion
    static Matrix4 build_view_matrix(const Vec3& position, const Quat& rotation);
    static Matrix4 build_projection(f32 fov_y, f32 aspect,
                                     f32 z_near, f32 z_far);

    // Compute view-projection from position + quaternion
    static Matrix4 build_view_projection(const Vec3& position,
                                          const Quat& rotation,
                                          const Matrix4& projection);

    // ─── Latency Compensation ────────────────────────────────────────

    void set_latency_compensation_enabled(bool v) { latency_comp_enabled_ = v; }
    bool latency_compensation_enabled() const { return latency_comp_enabled_; }

    f32 eye_separation() const { return eye_separation_; }
    void set_eye_separation(f32 ipd) { eye_separation_ = ipd; }

    f32 world_scale() const { return world_scale_; }
    void set_world_scale(f32 scale) { world_scale_ = scale; }

    // Convergence distance for stereo
    f32 convergence_distance() const { return convergence_distance_; }
    void set_convergence_distance(f32 d) { convergence_distance_ = d; }

    // ─── Stereo Projections ──────────────────────────────────────────

    static void mono_to_stereo_projections(
        const Matrix4& mono_proj,
        Matrix4& left_proj, Matrix4& right_proj,
        f32 eye_separation, f32 convergence_distance = 5.0f);

private:
    CameraRig() = default;

    f32 eye_separation_ = 0.064f;
    f32 world_scale_ = 1.0f;
    f32 convergence_distance_ = 5.0f;
    bool latency_comp_enabled_ = true;
};

} // namespace vrc
