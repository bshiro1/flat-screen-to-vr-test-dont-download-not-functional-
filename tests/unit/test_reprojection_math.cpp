// Tests for src/render/reprojection_math.h: the geometry behind the Screen
// quad layer and the Depth3D shader (ps_depth3d mirrors backward_warp()).

#include "test_framework.h"
#include "render/reprojection_math.h"

#include <cmath>

using namespace vrc;
using namespace vrc::reproj;

namespace {

constexpr f32 kNear = 0.1f;
constexpr f32 kFar = 1000.0f;

// Hardware depth a D3D projection writes for view distance z.
f32 normal_z_depth(f32 z) { return kFar * (z - kNear) / (z * (kFar - kNear)); }
f32 reversed_z_depth(f32 z) { return kNear * (kFar - z) / (z * (kFar - kNear)); }

Quat yaw_pitch(f32 yaw_deg, f32 pitch_deg) {
    const f32 y = yaw_deg * kDegToRad * 0.5f;
    const f32 p = pitch_deg * kDegToRad * 0.5f;
    return Quat(0.0f, sinf(y), 0.0f, cosf(y)) * Quat(sinf(p), 0.0f, 0.0f, cosf(p));
}

EyeView straight_eye(f32 x) {
    // Symmetric 90 degree FOV looking down -Z from (x, 0, 0).
    const f32 a = 45.0f * kDegToRad;
    return make_eye_view({ x, 0.0f, 0.0f }, Quat(), -a, a, a, -a);
}

// A tilted head: rotated, translated, with an asymmetric FOV like a real HMD.
EyeView tilted_eye() {
    return make_eye_view({ 0.03f, 0.02f, 0.05f }, yaw_pitch(10.0f, -5.0f),
                         -50.0f * kDegToRad, 45.0f * kDegToRad,
                         48.0f * kDegToRad, -52.0f * kDegToRad);
}

GameCamera test_camera() {
    return make_game_camera(60.0f, 16.0f / 9.0f, kNear, kFar, true);
}

} // namespace

// ─── Depth linearisation ───────────────────────────────────────────────────

VRC_TEST(reversed_z_linearisation) {
    const GameCamera c = make_game_camera(60.0f, 16.0f / 9.0f, kNear, kFar, true);
    VRC_CHECK_NEAR(linear_depth(1.0f, c), kNear, 1e-5);
    for (f32 z : { 0.5f, 10.0f, 250.0f }) {
        VRC_CHECK_NEAR(linear_depth(reversed_z_depth(z), c), z, z * 1e-3);
        VRC_CHECK_NEAR(inverse_linear_depth(reversed_z_depth(z), c), 1.0f / z, 1e-3 / z);
    }
    // Reversed-Z clears to 0: sky, infinitely far, no parallax.
    VRC_CHECK(is_sky_depth(0.0f, true));
    VRC_CHECK(!is_sky_depth(1.0f, true));
    VRC_CHECK(inverse_linear_depth(0.0f, c) == 0.0f);
    VRC_CHECK(std::isinf(linear_depth(0.0f, c)));
}

VRC_TEST(normal_z_linearisation) {
    const GameCamera c = make_game_camera(60.0f, 16.0f / 9.0f, kNear, kFar, false);
    VRC_CHECK_NEAR(linear_depth(0.0f, c), kNear, 1e-5);
    for (f32 z : { 0.5f, 10.0f, 250.0f }) {
        VRC_CHECK_NEAR(linear_depth(normal_z_depth(z), c), z, z * 1e-3);
    }
    // Normal Z clears to 1.
    VRC_CHECK(is_sky_depth(1.0f, false));
    VRC_CHECK(!is_sky_depth(0.0f, false));
    VRC_CHECK(inverse_linear_depth(1.0f, c) == 0.0f);
}

VRC_TEST(depth_conventions_agree) {
    // The same distance must linearise identically in both conventions.
    const GameCamera rev = make_game_camera(60.0f, 1.5f, kNear, kFar, true);
    const GameCamera nor = make_game_camera(60.0f, 1.5f, kNear, kFar, false);
    for (f32 z : { 0.2f, 3.0f, 40.0f }) {
        VRC_CHECK_NEAR(linear_depth(reversed_z_depth(z), rev),
                       linear_depth(normal_z_depth(z), nor), z * 1e-3);
    }
}

// ─── Screen quad ───────────────────────────────────────────────────────────

VRC_TEST(screen_quad_size_from_distance_fov_scale_aspect) {
    const f32 aspect = 16.0f / 9.0f;
    const ScreenQuad q = screen_quad(2.0f, 60.0f, 1.0f, aspect);
    VRC_CHECK_NEAR(q.distance, 2.0f, 1e-6);
    VRC_CHECK_NEAR(q.width, 2.0f * 2.0f * tanf(30.0f * kDegToRad), 1e-5);
    VRC_CHECK_NEAR(q.height, q.width / aspect, 1e-5);

    // fov_override 0 selects the default angular size.
    const ScreenQuad def = screen_quad(2.0f, 0.0f, 1.0f, aspect);
    VRC_CHECK_NEAR(def.width,
                   2.0f * 2.0f * tanf(kDefaultScreenFovDegrees * 0.5f * kDegToRad), 1e-5);

    // world_scale scales the size, not the distance.
    const ScreenQuad big = screen_quad(2.0f, 60.0f, 2.0f, aspect);
    VRC_CHECK_NEAR(big.width, 2.0f * q.width, 1e-5);
    VRC_CHECK_NEAR(big.distance, q.distance, 1e-6);

    // 90 degrees at 1 m is exactly 2 m wide; a 4:3 game gets a 4:3 quad.
    const ScreenQuad square = screen_quad(1.0f, 90.0f, 1.0f, 4.0f / 3.0f);
    VRC_CHECK_NEAR(square.width, 2.0f, 1e-5);
    VRC_CHECK_NEAR(square.height, 1.5f, 1e-5);
}

VRC_TEST(swapchain_extent_scales_and_clamps) {
    const Extent same = fit_swapchain_extent(1920, 1080, 1.0f, 4096, 4096);
    VRC_CHECK(same.width == 1920 && same.height == 1080);
    // render_scale 2 exceeds a 2048 limit: shrink uniformly, keep 16:9.
    const Extent clamped = fit_swapchain_extent(1920, 1080, 2.0f, 2048, 2048);
    VRC_CHECK(clamped.width == 2048);
    VRC_CHECK(clamped.height == 1152);
    const Extent half = fit_swapchain_extent(1920, 1080, 0.5f, 0, 0);
    VRC_CHECK(half.width == 960 && half.height == 540);
}

// ─── Rotation helpers ──────────────────────────────────────────────────────

VRC_TEST(positive_yaw_turns_forward_to_the_left) {
    f32 m[9];
    rotation_from_quat(yaw_pitch(90.0f, 0.0f), m);
    const Vec3 forward = rotate(m, { 0.0f, 0.0f, -1.0f });
    VRC_CHECK_NEAR(forward.x, -1.0f, 1e-5);
    VRC_CHECK_NEAR(forward.z, 0.0f, 1e-5);
    const Vec3 back = rotate_inverse(m, forward);
    VRC_CHECK_NEAR(back.z, -1.0f, 1e-5);
}

// ─── Depth3D warp direction ────────────────────────────────────────────────

VRC_TEST(forward_projection_gives_crossed_disparity) {
    const EyeView left = straight_eye(-0.032f);
    const EyeView right = straight_eye(0.032f);
    const Vec3 near_point{ 0.0f, 0.0f, -0.5f };
    const Vec3 far_point{ 0.0f, 0.0f, -100.0f };
    Vec2 ln, lf, rn, rf;
    VRC_CHECK(project_to_eye_uv(near_point, left, ln));
    VRC_CHECK(project_to_eye_uv(far_point, left, lf));
    VRC_CHECK(project_to_eye_uv(near_point, right, rn));
    VRC_CHECK(project_to_eye_uv(far_point, right, rf));
    // The nose is to the right of the left eye and to the left of the right eye.
    VRC_CHECK(ln.x > lf.x);
    VRC_CHECK(rn.x < rf.x);
}

VRC_TEST(backward_warp_shifts_near_content_toward_the_nose) {
    // Looking at the centre of each eye's image: with near content the left
    // eye must sample further LEFT in the game image (so the content appears
    // shifted right, toward the nose), the right eye further RIGHT. The old
    // shader had this backwards, which made near objects look far.
    const GameCamera cam = test_camera();
    auto near_depth = [](Vec2) { return 1.0f / 0.5f; };
    auto far_depth = [](Vec2) { return 1.0f / 100.0f; };
    const Vec2 centre{ 0.5f, 0.5f };
    Vec2 l_near, l_far, r_near, r_far;
    VRC_CHECK(backward_warp(centre, straight_eye(-0.032f), cam, near_depth, l_near));
    VRC_CHECK(backward_warp(centre, straight_eye(-0.032f), cam, far_depth, l_far));
    VRC_CHECK(backward_warp(centre, straight_eye(0.032f), cam, near_depth, r_near));
    VRC_CHECK(backward_warp(centre, straight_eye(0.032f), cam, far_depth, r_far));
    VRC_CHECK(l_near.x < l_far.x);
    VRC_CHECK(r_near.x > r_far.x);
    VRC_CHECK_NEAR(l_near.y, 0.5f, 1e-6);
}

VRC_TEST(backward_warp_inverts_forward_projection_constant_depth) {
    const GameCamera cam = test_camera();
    const EyeView eye = tilted_eye();
    const f32 z = 2.0f;
    for (Vec2 src : { Vec2(0.3f, 0.6f), Vec2(0.5f, 0.5f), Vec2(0.8f, 0.2f) }) {
        Vec2 out;
        VRC_CHECK(project_to_eye_uv(game_point(src, z, cam), eye, out));
        Vec2 back;
        VRC_CHECK(backward_warp(out, eye, cam, [&](Vec2) { return 1.0f / z; }, back));
        VRC_CHECK_NEAR(back.x, src.x, 1e-4);
        VRC_CHECK_NEAR(back.y, src.y, 1e-4);
    }
}

VRC_TEST(backward_warp_inverts_forward_projection_sloped_depth) {
    // Depth varies across the image, so the fixed-point iteration must
    // actually converge rather than being exact after one step.
    const GameCamera cam = test_camera();
    const EyeView eye = tilted_eye();
    auto depth_at = [](Vec2 uv) { return 1.0f + 1.5f * uv.x + 0.5f * uv.y; };
    for (Vec2 src : { Vec2(0.4f, 0.45f), Vec2(0.15f, 0.7f), Vec2(0.75f, 0.3f) }) {
        Vec2 out;
        VRC_CHECK(project_to_eye_uv(game_point(src, depth_at(src), cam), eye, out));
        Vec2 back;
        VRC_CHECK(backward_warp(out, eye, cam,
                                [&](Vec2 uv) { return 1.0f / depth_at(uv); }, back));
        VRC_CHECK_NEAR(back.x, src.x, 1e-3);
        VRC_CHECK_NEAR(back.y, src.y, 1e-3);
    }
}

VRC_TEST(sky_and_zero_parallax_ignore_eye_translation) {
    // Sky (inverse depth 0) and parallax_strength 0 both reduce the warp to a
    // pure rotation: the eye position must not matter.
    const GameCamera cam = test_camera();
    const EyeView moved = tilted_eye();
    EyeView centred = moved;
    centred.position = content_eye_position({ 0.03f, 0.02f, 0.05f }, 0.0f, 1.0f);
    VRC_CHECK(centred.position.x == 0.0f && centred.position.z == 0.0f);

    const Vec2 out{ 0.45f, 0.55f };
    Vec2 sky_moved, sky_centred, near_centred;
    VRC_CHECK(backward_warp(out, moved, cam, [](Vec2) { return 0.0f; }, sky_moved));
    VRC_CHECK(backward_warp(out, centred, cam, [](Vec2) { return 0.0f; }, sky_centred));
    VRC_CHECK(backward_warp(out, centred, cam, [](Vec2) { return 2.0f; }, near_centred));
    VRC_CHECK_NEAR(sky_moved.x, sky_centred.x, 1e-6);
    VRC_CHECK_NEAR(sky_moved.y, sky_centred.y, 1e-6);
    VRC_CHECK_NEAR(near_centred.x, sky_centred.x, 1e-6);
}

VRC_TEST(content_eye_position_scales_baseline) {
    const Vec3 eye{ 0.032f, 0.0f, -0.1f };
    const Vec3 same = content_eye_position(eye, 1.0f, 1.0f);
    VRC_CHECK_NEAR(same.x, 0.032f, 1e-7);
    // A world twice as big is the same as a viewer half as big.
    const Vec3 big_world = content_eye_position(eye, 1.0f, 2.0f);
    VRC_CHECK_NEAR(big_world.x, 0.016f, 1e-7);
    VRC_CHECK_NEAR(big_world.z, -0.05f, 1e-7);
    const Vec3 stronger = content_eye_position(eye, 2.0f, 1.0f);
    VRC_CHECK_NEAR(stronger.x, 0.064f, 1e-7);
}

VRC_TEST(surface_behind_the_eye_is_rejected) {
    // Head moved 1 m forward past a surface 0.5 m away: nothing to show.
    const GameCamera cam = test_camera();
    const f32 a = 45.0f * kDegToRad;
    const EyeView forward = make_eye_view({ 0.0f, 0.0f, -1.0f }, Quat(), -a, a, a, -a);
    Vec2 src;
    VRC_CHECK(!backward_warp({ 0.5f, 0.5f }, forward, cam, [](Vec2) { return 2.0f; }, src));
}
