#pragma once

// Pure math shared by the XR frame thread (C++) and the HLSL in
// depth_reprojection.cpp. The shader functions mirror these line for line, so
// tests/unit/test_reprojection_math.cpp covers the GPU path's geometry too.
//
// Coordinate conventions
//   * LOCAL space is OpenXR's: right-handed, +X right, +Y up, -Z forward,
//     origin at the head's starting position (eye level).
//   * The game camera sits at the LOCAL origin with identity orientation, so
//     "game camera space" and LOCAL space are the same frame. One game unit
//     maps to 1 / world_scale metres (see content_eye_position()).
//   * Image UVs have (0,0) at the top-left and (1,1) at the bottom-right.
//   * A "tangent" coordinate is (x / -z, y / -z) of a view-space direction:
//     the position on the plane one unit in front of the camera.

#include "core/types.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vrc::reproj {

// Fixed-point iterations per output pixel in the Depth3D backward warp. The
// shader receives this value as the WARP_ITERATIONS macro.
inline constexpr int kWarpIterations = 6;

// Depth values this close to the cleared value (0 with reversed-Z, 1 without)
// are treated as sky: infinitely far away, so they get no parallax.
inline constexpr f32 kSkyEpsilon = 1e-7f;

// ─── Game camera and depth linearisation ────────────────────────────────────

struct GameCamera {
    f32 tan_half_x = 1.0f;          // tan(horizontal FOV / 2)
    f32 tan_half_y = 1.0f;          // tan(vertical FOV / 2)
    f32 inv_far = 0.0f;             // 1 / far
    f32 inv_near_minus_far = 1.0f;  // 1 / near - 1 / far
    bool reversed_z = true;
};

// vertical_fov_degrees is the game camera's vertical FOV (Unity convention);
// the horizontal extent follows from the game frame's aspect ratio.
inline GameCamera make_game_camera(f32 vertical_fov_degrees, f32 aspect,
                                   f32 near_plane, f32 far_plane, bool reversed_z) {
    const f32 fov = std::clamp(vertical_fov_degrees, 10.0f, 170.0f) * kDegToRad;
    const f32 n = std::max(near_plane, 1e-4f);
    const f32 f = std::max(far_plane, n * 1.001f);
    GameCamera c;
    c.tan_half_y = tanf(fov * 0.5f);
    c.tan_half_x = c.tan_half_y * (aspect > 0.0f ? aspect : 16.0f / 9.0f);
    c.inv_far = 1.0f / f;
    c.inv_near_minus_far = 1.0f / n - 1.0f / f;
    c.reversed_z = reversed_z;
    return c;
}

// Maps a hardware depth value to "fraction toward the near plane": 1 at the
// near plane and 0 at the far plane, for both depth conventions.
inline f32 depth_toward_near(f32 depth, bool reversed_z) {
    return reversed_z ? depth : 1.0f - depth;
}

inline bool is_sky_depth(f32 depth, bool reversed_z) {
    return depth_toward_near(depth, reversed_z) <= kSkyEpsilon;
}

// 1 / view-space distance for a D3D perspective depth value, 0 for sky.
//   normal Z:   d = f (z - n) / (z (f - n))   ->  1/z = 1/n - d (1/n - 1/f)
//   reversed Z: d = n (f - z) / (z (f - n))   ->  1/z = 1/f + d (1/n - 1/f)
// Both reduce to 1/f + t (1/n - 1/f) with t = depth_toward_near(d).
inline f32 inverse_linear_depth(f32 depth, const GameCamera& c) {
    const f32 t = depth_toward_near(depth, c.reversed_z);
    if (t <= kSkyEpsilon) return 0.0f;
    return c.inv_far + t * c.inv_near_minus_far;
}

// View-space distance in game units; +infinity for sky.
inline f32 linear_depth(f32 depth, const GameCamera& c) {
    const f32 inv = inverse_linear_depth(depth, c);
    return inv > 0.0f ? 1.0f / inv : std::numeric_limits<f32>::infinity();
}

// ─── Screen mode ────────────────────────────────────────────────────────────

struct ScreenQuad {
    f32 distance = 0.0f;  // metres in front of the viewer (pose.z = -distance)
    f32 width = 0.0f;     // metres
    f32 height = 0.0f;    // metres
};

// Size of the Screen-mode XrCompositionLayerQuad. fov_override_degrees is the
// horizontal angular size at world_scale 1 (0 selects the default);
// world_scale then scales the physical size at a fixed distance.
inline ScreenQuad screen_quad(f32 distance, f32 fov_override_degrees,
                              f32 world_scale, f32 aspect) {
    ScreenQuad q;
    q.distance = std::clamp(distance, 0.1f, 100.0f);
    const f32 fov_deg = fov_override_degrees > 0.0f ? fov_override_degrees
                                                    : kDefaultScreenFovDegrees;
    const f32 fov = std::clamp(fov_deg, 5.0f, 170.0f) * kDegToRad;
    const f32 scale = std::max(world_scale, 0.01f);
    const f32 safe_aspect = aspect > 0.0f ? aspect : 16.0f / 9.0f;
    q.width = 2.0f * q.distance * tanf(fov * 0.5f) * scale;
    q.height = q.width / safe_aspect;
    return q;
}

struct Extent {
    u32 width = 0;
    u32 height = 0;
};

// Swapchain size for a width x height source scaled by render_scale, shrunk
// uniformly (aspect preserved) to fit the runtime's maximum image size.
inline Extent fit_swapchain_extent(u32 width, u32 height, f32 render_scale,
                                   u32 max_width, u32 max_height) {
    const f32 scale = std::clamp(render_scale, 0.1f, 4.0f);
    f32 w = std::max(1.0f, static_cast<f32>(width) * scale);
    f32 h = std::max(1.0f, static_cast<f32>(height) * scale);
    f32 fit = 1.0f;
    if (max_width > 0) fit = std::min(fit, static_cast<f32>(max_width) / w);
    if (max_height > 0) fit = std::min(fit, static_cast<f32>(max_height) / h);
    Extent e;
    e.width = std::max(1u, static_cast<u32>(w * fit));
    e.height = std::max(1u, static_cast<u32>(h * fit));
    if (max_width > 0) e.width = std::min(e.width, max_width);
    if (max_height > 0) e.height = std::min(e.height, max_height);
    return e;
}

// ─── Eye views ──────────────────────────────────────────────────────────────

// Row-major 3x3 rotation, world = M * eye (column vectors).
inline void rotation_from_quat(const Quat& q_in, f32 m[9]) {
    const Quat q = q_in.normalized();
    const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1.0f - 2.0f * (yy + zz); m[1] = 2.0f * (xy - wz);        m[2] = 2.0f * (xz + wy);
    m[3] = 2.0f * (xy + wz);        m[4] = 1.0f - 2.0f * (xx + zz); m[5] = 2.0f * (yz - wx);
    m[6] = 2.0f * (xz - wy);        m[7] = 2.0f * (yz + wx);        m[8] = 1.0f - 2.0f * (xx + yy);
}

inline Vec3 rotate(const f32 m[9], const Vec3& v) {
    return { m[0] * v.x + m[1] * v.y + m[2] * v.z,
             m[3] * v.x + m[4] * v.y + m[5] * v.z,
             m[6] * v.x + m[7] * v.y + m[8] * v.z };
}

inline Vec3 rotate_inverse(const f32 m[9], const Vec3& v) {
    return { m[0] * v.x + m[3] * v.y + m[6] * v.z,
             m[1] * v.x + m[4] * v.y + m[7] * v.z,
             m[2] * v.x + m[5] * v.y + m[8] * v.z };
}

// One rendered eye: where the content is viewed from (game units, LOCAL
// frame), the eye orientation and the asymmetric per-eye FOV from
// xrLocateViews, given as tangents of the XrFovf angles.
struct EyeView {
    Vec3 position;
    f32 rotation[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    f32 tan_left = -1.0f, tan_right = 1.0f, tan_up = 1.0f, tan_down = -1.0f;
};

inline EyeView make_eye_view(const Vec3& content_position, const Quat& orientation,
                             f32 angle_left, f32 angle_right,
                             f32 angle_up, f32 angle_down) {
    EyeView e;
    e.position = content_position;
    rotation_from_quat(orientation, e.rotation);
    e.tan_left = tanf(angle_left);
    e.tan_right = tanf(angle_right);
    e.tan_up = tanf(angle_up);
    e.tan_down = tanf(angle_down);
    return e;
}

// Converts a LOCAL-space eye position (metres) into game units.
// parallax_strength scales the baseline (0 = no parallax, 1 = geometric);
// world_scale > 1 makes the world look bigger, i.e. the viewer smaller.
inline Vec3 content_eye_position(const Vec3& eye_local, f32 parallax_strength,
                                 f32 world_scale) {
    const f32 k = std::clamp(parallax_strength, 0.0f, 4.0f) /
                  std::max(world_scale, 0.01f);
    return { eye_local.x * k, eye_local.y * k, eye_local.z * k };
}

// ─── Game image <-> tangent space ───────────────────────────────────────────

inline Vec2 game_uv_to_tan(const Vec2& uv, const GameCamera& c) {
    return { (uv.x * 2.0f - 1.0f) * c.tan_half_x, (1.0f - uv.y * 2.0f) * c.tan_half_y };
}

inline Vec2 game_tan_to_uv(const Vec2& t, const GameCamera& c) {
    return { 0.5f + 0.5f * t.x / c.tan_half_x, 0.5f - 0.5f * t.y / c.tan_half_y };
}

// The point seen at game-image uv with view-space distance z (game units).
inline Vec3 game_point(const Vec2& uv, f32 z, const GameCamera& c) {
    const Vec2 t = game_uv_to_tan(uv, c);
    return { t.x * z, t.y * z, -z };
}

// World-space (LOCAL) direction of the ray through output pixel out_uv.
inline Vec3 eye_ray(const Vec2& out_uv, const EyeView& e) {
    const Vec3 d_eye{ e.tan_left + (e.tan_right - e.tan_left) * out_uv.x,
                      e.tan_up + (e.tan_down - e.tan_up) * out_uv.y,
                      -1.0f };
    return rotate(e.rotation, d_eye);
}

// Forward projection of a LOCAL point into the eye's image (uv). Returns false
// when the point is behind the eye.
inline bool project_to_eye_uv(const Vec3& p, const EyeView& e, Vec2& out_uv) {
    const Vec3 v = rotate_inverse(e.rotation,
                                  { p.x - e.position.x, p.y - e.position.y,
                                    p.z - e.position.z });
    if (v.z >= -1e-6f) return false;
    const f32 tx = v.x / -v.z;
    const f32 ty = v.y / -v.z;
    out_uv = { (tx - e.tan_left) / (e.tan_right - e.tan_left),
               (e.tan_up - ty) / (e.tan_up - e.tan_down) };
    return true;
}

// ─── Depth3D backward warp ──────────────────────────────────────────────────
//
// For output pixel out_uv the eye ray is E + t*D (E = eye position, D = ray
// direction, both LOCAL/game frame). We need the game pixel whose
// reconstructed point lies on that ray. A point at view distance z on the ray
// projects into the game image at
//
//     src_tan = d_inf * (1 + E.z / z) + E.xy / z,    d_inf = D.xy / -D.z
//
// d_inf is where the ray lands for infinitely distant content (pure rotation);
// the 1/z terms are the depth-dependent disparity. z itself is the depth found
// at src, so we iterate the fixed point
//
//     src <- d_inf * (1 + E.z * invz(src)) + E.xy * invz(src)
//
// starting from src = d_inf. Each step samples the depth at the current
// estimate; on smooth surfaces it converges in a few steps, and at depth
// discontinuities it settles on one side of the edge (a small smear instead of
// a hole). invz = 1/z comes straight from inverse_linear_depth(), so sky
// (invz = 0) maps exactly to d_inf.
//
// Effect: a near point appears shifted toward the nose in each eye relative to
// a far point (crossed disparity), as in real stereo vision.
template <typename InvDepthAtUv>
inline bool backward_warp(const Vec2& out_uv, const EyeView& e, const GameCamera& c,
                          InvDepthAtUv inv_depth_at_uv, Vec2& src_uv,
                          int iterations = kWarpIterations) {
    const Vec3 dir = eye_ray(out_uv, e);
    if (dir.z > -1e-4f) return false;  // ray points sideways/backwards from the game view
    const Vec2 d_inf{ dir.x / -dir.z, dir.y / -dir.z };
    Vec2 src_tan = d_inf;
    bool valid = true;
    for (int i = 0; i < iterations; ++i) {
        const f32 inv_z = inv_depth_at_uv(game_tan_to_uv(src_tan, c));
        f32 w = 1.0f + e.position.z * inv_z;
        if (w <= 0.0f) {  // the surface would be behind the eye
            valid = false;
            w = 1e-4f;
        }
        src_tan = { d_inf.x * w + e.position.x * inv_z,
                    d_inf.y * w + e.position.y * inv_z };
    }
    src_uv = game_tan_to_uv(src_tan, c);
    return valid && src_uv.x >= 0.0f && src_uv.x <= 1.0f &&
           src_uv.y >= 0.0f && src_uv.y <= 1.0f;
}

} // namespace vrc::reproj
