#include "camera_rig.h"
#include "core/logging.h"
#include <cmath>
#include <numbers>

namespace vrc {

CameraRig& CameraRig::instance() {
    static CameraRig rig;
    return rig;
}

void CameraRig::initialize(f32 eye_separation) {
    eye_separation_ = eye_separation;
    Log::info("Camera rig initialized (eye sep: {:.4f}m)", eye_separation_);
}

void CameraRig::shutdown() {
    Log::info("Camera rig shut down");
}

// ─── Build View Matrix from Position + Quaternion ───────────────────────────

Matrix4 CameraRig::build_view_matrix(const Vec3& position, const Quat& rotation) {
    // Convert quaternion to rotation matrix
    f32 x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;
    f32 x2 = x + x, y2 = y + y, z2 = z + z;
    f32 xx = x * x2, xy = x * y2, xz = x * z2;
    f32 yy = y * y2, yz = y * z2, zz = z * z2;
    f32 wx = w * x2, wy = w * y2, wz = w * z2;

    Matrix4 rot;
    rot.m[0] = 1.0f - (yy + zz);
    rot.m[4] = xy - wz;
    rot.m[8] = xz + wy;
    rot.m[1] = xy + wz;
    rot.m[5] = 1.0f - (xx + zz);
    rot.m[9] = yz - wx;
    rot.m[2] = xz - wy;
    rot.m[6] = yz + wx;
    rot.m[10] = 1.0f - (xx + yy);

    // The view matrix is the inverse of the camera's world transform.
    // For a pure rotation + translation: V = [R^T, -R^T * t]
    // Transpose the rotation part and compute new translation
    Matrix4 view;
    // Transpose of rotation = inverse for orthonormal matrix
    view.m[0] = rot.m[0]; view.m[4] = rot.m[1]; view.m[8]  = rot.m[2];
    view.m[1] = rot.m[4]; view.m[5] = rot.m[5]; view.m[9]  = rot.m[6];
    view.m[2] = rot.m[8]; view.m[6] = rot.m[9]; view.m[10] = rot.m[10];

    // Translation: -R^T * position
    view.m[3] = -(view.m[0] * position.x + view.m[4] * position.y + view.m[8] * position.z);
    view.m[7] = -(view.m[1] * position.x + view.m[5] * position.y + view.m[9] * position.z);
    view.m[11] = -(view.m[2] * position.x + view.m[6] * position.y + view.m[10] * position.z);

    return view;
}

Matrix4 CameraRig::build_projection(f32 fov_y, f32 aspect,
                                     f32 z_near, f32 z_far)
{
    return Matrix4::perspective(fov_y, aspect, z_near, z_far);
}

Matrix4 CameraRig::build_view_projection(const Vec3& position,
                                          const Quat& rotation,
                                          const Matrix4& projection)
{
    Matrix4 view = build_view_matrix(position, rotation);
    return multiply(view, projection);
}

// ─── Eye View Computation ───────────────────────────────────────────────────

EyeViews CameraRig::compute_eye_views(const ViewSetup& mono_setup) const {
    EyeViews ev;

    f32 half_sep = eye_separation_ / 2.0f;

    // Compute the right vector from the view matrix
    Vec3 right(
        mono_setup.view.m[0],
        mono_setup.view.m[4],
        mono_setup.view.m[8]
    );

    // Compute stereo projection matrices
    Matrix4 left_proj, right_proj;
    mono_to_stereo_projections(
        mono_setup.projection, left_proj, right_proj,
        eye_separation_, convergence_distance_);

    // Left eye
    ev.left = mono_setup;
    ev.left.position = Vec3(
        mono_setup.position.x - right.x * half_sep,
        mono_setup.position.y - right.y * half_sep,
        mono_setup.position.z - right.z * half_sep
    );
    ev.left.view = build_view_matrix(ev.left.position, mono_setup.rotation);
    ev.left.projection = left_proj;
    ev.left.view_projection = multiply(ev.left.view, ev.left.projection);

    // Right eye
    ev.right = mono_setup;
    ev.right.position = Vec3(
        mono_setup.position.x + right.x * half_sep,
        mono_setup.position.y + right.y * half_sep,
        mono_setup.position.z + right.z * half_sep
    );
    ev.right.view = build_view_matrix(ev.right.position, mono_setup.rotation);
    ev.right.projection = right_proj;
    ev.right.view_projection = multiply(ev.right.view, ev.right.projection);

    return ev;
}

// ─── Head Tracking Integration ──────────────────────────────────────────────

ViewSetup CameraRig::apply_head_tracking(const ViewSetup& game_camera,
                                          const HeadPose& head_pose) const
{
    ViewSetup result = game_camera;

    // Apply head tracking position offset
    result.position = Vec3(
        game_camera.position.x + head_pose.position.x * world_scale_,
        game_camera.position.y + head_pose.position.y * world_scale_,
        game_camera.position.z + head_pose.position.z * world_scale_
    );

    // Combine game camera rotation with head tracking rotation
    // The head rotation is applied after the game camera rotation
    // In quaternion space: result_rot = head_rot * game_rot
    Quat& g = result.rotation;
    Quat& h = const_cast<HeadPose&>(head_pose).rotation;

    // Quaternion multiplication: result = head * game
    result.rotation = Quat(
        h.w * g.x + h.x * g.w + h.y * g.z - h.z * g.y,
        h.w * g.y - h.x * g.z + h.y * g.w + h.z * g.x,
        h.w * g.z + h.x * g.y - h.y * g.x + h.z * g.w,
        h.w * g.w - h.x * g.x - h.y * g.y - h.z * g.z
    );

    // Rebuild view matrix from combined pose
    result.view = build_view_matrix(result.position, result.rotation);
    result.view_projection = multiply(result.view, result.projection);

    return result;
}

// ─── Stereo Projection ──────────────────────────────────────────────────────

void CameraRig::mono_to_stereo_projections(
    const Matrix4& mono_proj,
    Matrix4& left_proj, Matrix4& right_proj,
    f32 eye_separation, f32 convergence_distance)
{
    f32 a = mono_proj.m[0];
    f32 b = mono_proj.m[5];

    f32 fov_y = 2.0f * atanf(1.0f / b);
    f32 aspect = b / a;
    f32 z_near = mono_proj.m[14] / (mono_proj.m[10] - 1.0f);
    f32 z_far = mono_proj.m[14] / (mono_proj.m[10] + 1.0f);

    f32 top = z_near * tanf(fov_y / 2.0f);
    f32 bottom = -top;
    f32 half_sep = eye_separation / 2.0f;

    f32 left_left   = -aspect * z_near * tanf(fov_y / 2.0f) + half_sep * z_near / convergence_distance;
    f32 left_right  =  aspect * z_near * tanf(fov_y / 2.0f) + half_sep * z_near / convergence_distance;
    left_proj.m[0]  = 2.0f * z_near / (left_right - left_left);
    left_proj.m[2]  = (left_right + left_left) / (left_right - left_left);
    left_proj.m[5]  = 2.0f * z_near / (top - bottom);
    left_proj.m[6]  = (top + bottom) / (top - bottom);
    left_proj.m[10] = -(z_far + z_near) / (z_far - z_near);
    left_proj.m[11] = -1.0f;
    left_proj.m[14] = -2.0f * z_far * z_near / (z_far - z_near);
    left_proj.m[15] = 0.0f;

    f32 right_left  = -aspect * z_near * tanf(fov_y / 2.0f) - half_sep * z_near / convergence_distance;
    f32 right_right =  aspect * z_near * tanf(fov_y / 2.0f) - half_sep * z_near / convergence_distance;
    right_proj.m[0] = 2.0f * z_near / (right_right - right_left);
    right_proj.m[2] = (right_right + right_left) / (right_right - right_left);
    right_proj.m[5] = 2.0f * z_near / (top - bottom);
    right_proj.m[6] = (top + bottom) / (top - bottom);
    right_proj.m[10] = -(z_far + z_near) / (z_far - z_near);
    right_proj.m[11] = -1.0f;
    right_proj.m[14] = -2.0f * z_far * z_near / (z_far - z_near);
    right_proj.m[15] = 0.0f;
}

} // namespace vrc
