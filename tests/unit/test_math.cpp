// Matrix4 tests for src/core/types.h.
//
// Convention (see types.h): Matrix4::m is row-major with element (row i,
// column j) at m[i * 4 + j], and points are ROW vectors transformed as v * M.
// So multiply(A, B) applies A first, then B. look_at builds a right-handed view
// matrix (camera looks down -Z) and perspective_d3d maps view-space depth
// z = -near to NDC 0 and z = -far to NDC 1 (D3D clip-space Z in [0, w]).

#include "test_framework.h"

#include "core/types.h"

#include <algorithm>
#include <cmath>

using vrc::f32;
using vrc::Matrix4;
using vrc::Vec3;
using vrc::Vec4;

namespace {

Vec4 transform(const Vec4& v, const Matrix4& M) {
    const auto& m = M.m;
    return {
        v.x * m[0] + v.y * m[4] + v.z * m[8]  + v.w * m[12],
        v.x * m[1] + v.y * m[5] + v.z * m[9]  + v.w * m[13],
        v.x * m[2] + v.y * m[6] + v.z * m[10] + v.w * m[14],
        v.x * m[3] + v.y * m[7] + v.z * m[11] + v.w * m[15],
    };
}

f32 max_abs_diff(const Matrix4& a, const Matrix4& b) {
    f32 worst = 0.0f;
    for (int i = 0; i < 16; ++i) worst = std::max(worst, std::fabs(a.m[i] - b.m[i]));
    return worst;
}

f32 ndc_depth(const Matrix4& proj, f32 view_x, f32 view_y, f32 view_z) {
    const Vec4 clip = transform({ view_x, view_y, view_z, 1.0f }, proj);
    return clip.z / clip.w;
}

constexpr f32 kFovY = 60.0f * vrc::kDegToRad;
constexpr f32 kAspect = 16.0f / 9.0f;
constexpr f32 kNear = 0.1f;
constexpr f32 kFar = 1000.0f;

Matrix4 sample_projection() { return Matrix4::perspective_d3d(kFovY, kAspect, kNear, kFar); }

Matrix4 sample_view() {
    return Matrix4::look_at({ 1.0f, 2.0f, 3.0f }, { 4.0f, -1.0f, 0.5f }, { 0.0f, 1.0f, 0.0f });
}

} // namespace

VRC_TEST(identity_is_multiplicative_neutral) {
    const Matrix4 a = Matrix4::rotation_ypr(0.3f, -0.2f, 0.1f);
    VRC_CHECK_NEAR(max_abs_diff(multiply(Matrix4::identity(), a), a), 0.0, 1e-6);
    VRC_CHECK_NEAR(max_abs_diff(multiply(a, Matrix4::identity()), a), 0.0, 1e-6);
}

VRC_TEST(inverse_of_perspective_d3d_times_itself_is_identity) {
    const Matrix4 p = sample_projection();
    const Matrix4 inv = vrc::inverse(p);
    VRC_CHECK_NEAR(max_abs_diff(multiply(inv, p), Matrix4::identity()), 0.0, 1e-4);
    VRC_CHECK_NEAR(max_abs_diff(multiply(p, inv), Matrix4::identity()), 0.0, 1e-4);
}

VRC_TEST(inverse_of_look_at_times_itself_is_identity) {
    const Matrix4 v = sample_view();
    const Matrix4 inv = vrc::inverse(v);
    VRC_CHECK_NEAR(max_abs_diff(multiply(inv, v), Matrix4::identity()), 0.0, 1e-5);
    VRC_CHECK_NEAR(max_abs_diff(multiply(v, inv), Matrix4::identity()), 0.0, 1e-5);
}

VRC_TEST(inverse_of_singular_matrix_returns_identity) {
    Matrix4 zero;
    zero.m.fill(0.0f);
    VRC_CHECK_NEAR(max_abs_diff(vrc::inverse(zero), Matrix4::identity()), 0.0, 0.0);
}

VRC_TEST(look_at_maps_eye_to_origin_and_target_down_negative_z) {
    const Vec3 eye{ 1.0f, 2.0f, 3.0f };
    const Vec3 target{ 4.0f, -1.0f, 0.5f };
    const Matrix4 v = sample_view();

    const Vec4 e = transform({ eye.x, eye.y, eye.z, 1.0f }, v);
    VRC_CHECK_NEAR(e.x, 0.0, 1e-5);
    VRC_CHECK_NEAR(e.y, 0.0, 1e-5);
    VRC_CHECK_NEAR(e.z, 0.0, 1e-5);
    VRC_CHECK_NEAR(e.w, 1.0, 1e-6);

    const f32 dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const Vec4 t = transform({ target.x, target.y, target.z, 1.0f }, v);
    VRC_CHECK_NEAR(t.x, 0.0, 1e-5);
    VRC_CHECK_NEAR(t.y, 0.0, 1e-5);
    VRC_CHECK_NEAR(t.z, -dist, 1e-5);
}

VRC_TEST(perspective_d3d_maps_near_to_zero_and_far_to_one) {
    const Matrix4 p = sample_projection();

    // On the view axis.
    VRC_CHECK_NEAR(ndc_depth(p, 0.0f, 0.0f, -kNear), 0.0, 1e-5);
    VRC_CHECK_NEAR(ndc_depth(p, 0.0f, 0.0f, -kFar), 1.0, 1e-5);

    // Depth does not depend on x/y.
    VRC_CHECK_NEAR(ndc_depth(p, 0.03f, -0.02f, -kNear), 0.0, 1e-5);
    VRC_CHECK_NEAR(ndc_depth(p, 250.0f, 120.0f, -kFar), 1.0, 1e-5);

    // Clip w is the positive view distance (-z), so points in front survive
    // clipping, and depth increases monotonically between the planes.
    const Vec4 clip = transform({ 0.0f, 0.0f, -10.0f, 1.0f }, p);
    VRC_CHECK_NEAR(clip.w, 10.0, 1e-5);
    const f32 d1 = ndc_depth(p, 0.0f, 0.0f, -1.0f);
    const f32 d10 = ndc_depth(p, 0.0f, 0.0f, -10.0f);
    const f32 d100 = ndc_depth(p, 0.0f, 0.0f, -100.0f);
    VRC_CHECK(d1 > 0.0f);
    VRC_CHECK(d1 < d10);
    VRC_CHECK(d10 < d100);
    VRC_CHECK(d100 < 1.0f);
}

VRC_TEST(perspective_d3d_maps_fov_edge_to_ndc_one) {
    const Matrix4 p = sample_projection();
    const f32 z = -5.0f;
    const f32 half_h = 5.0f * std::tan(kFovY * 0.5f);
    const f32 half_w = half_h * kAspect;
    const Vec4 top = transform({ 0.0f, half_h, z, 1.0f }, p);
    const Vec4 right = transform({ half_w, 0.0f, z, 1.0f }, p);
    VRC_CHECK_NEAR(top.y / top.w, 1.0, 1e-5);
    VRC_CHECK_NEAR(right.x / right.w, 1.0, 1e-5);
}

VRC_TEST(multiply_is_associative) {
    const Matrix4 a = Matrix4::rotation_ypr(0.3f, -0.2f, 0.1f);
    const Matrix4 b = Matrix4::translation(1.0f, -2.0f, 3.0f);
    const Matrix4 c = sample_projection();
    const Matrix4 left = multiply(multiply(a, b), c);
    const Matrix4 right = multiply(a, multiply(b, c));
    VRC_CHECK_NEAR(max_abs_diff(left, right), 0.0, 1e-5);
}

VRC_TEST(multiply_applies_left_operand_first_for_row_vectors) {
    const Matrix4 a = Matrix4::rotation_ypr(0.7f, 0.2f, -0.4f);
    const Matrix4 b = Matrix4::translation(0.5f, 1.5f, -2.5f);
    const Vec4 v{ 0.25f, -1.0f, 2.0f, 1.0f };
    const Vec4 step = transform(transform(v, a), b);
    const Vec4 combined = transform(v, multiply(a, b));
    VRC_CHECK_NEAR(step.x, combined.x, 1e-5);
    VRC_CHECK_NEAR(step.y, combined.y, 1e-5);
    VRC_CHECK_NEAR(step.z, combined.z, 1e-5);
    VRC_CHECK_NEAR(step.w, combined.w, 1e-6);
}

VRC_TEST(inverse_view_projection_reconstructs_world_point_from_ndc) {
    // The depth-reprojection path rebuilds world positions as
    // (ndc.x, ndc.y, depth, 1) * inverse(view * proj), then divides by w.
    const Matrix4 vp = multiply(sample_view(), sample_projection());
    const Matrix4 inv_vp = vrc::inverse(vp);

    const Vec4 world{ 3.0f, -0.5f, 0.0f, 1.0f };
    const Vec4 clip = transform(world, vp);
    VRC_CHECK(clip.w > 0.0f);
    const Vec4 ndc{ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.0f };
    VRC_CHECK(ndc.z > 0.0f && ndc.z < 1.0f);

    const Vec4 back = transform(ndc, inv_vp);
    VRC_CHECK_NEAR(back.x / back.w, world.x, 1e-3);
    VRC_CHECK_NEAR(back.y / back.w, world.y, 1e-3);
    VRC_CHECK_NEAR(back.z / back.w, world.z, 1e-3);
}
