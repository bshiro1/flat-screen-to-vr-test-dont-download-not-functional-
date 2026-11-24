#pragma once

#include <cstdint>
#include <cmath>
#include <array>
#include <string>
#include <optional>
#include <functional>

namespace vrc {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;

static constexpr f32 kPi = 3.14159265358979323846f;
static constexpr f32 kDegToRad = kPi / 180.0f;
static constexpr f32 kRadToDeg = 180.0f / kPi;

struct Vec2 {
    f32 x = 0, y = 0;
    Vec2() = default;
    Vec2(f32 x, f32 y) : x(x), y(y) {}
};

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(f32 x, f32 y, f32 z) : x(x), y(y), z(z) {}
};

struct Vec4 {
    f32 x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    Vec4(f32 x, f32 y, f32 z, f32 w) : x(x), y(y), z(z), w(w) {}
};

struct Quat {
    f32 x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(f32 x, f32 y, f32 z, f32 w) : x(x), y(y), z(z), w(w) {}

    Quat conjugated() const { return { -x, -y, -z, w }; }
    Quat normalized() const {
        f32 len = sqrtf(x*x + y*y + z*z + w*w);
        if (len < 1e-10f) return { 0, 0, 0, 1 };
        return { x/len, y/len, z/len, w/len };
    }
    void normalize() { *this = normalized(); }
};

inline Quat operator*(const Quat& a, const Quat& b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

// Quaternion slerp: spherical linear interpolation
inline Quat quat_slerp(const Quat& a, const Quat& b, f32 t) {
    f32 dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    Quat bt = b;
    if (dot < 0.0f) { dot = -dot; bt = { -b.x, -b.y, -b.z, -b.w }; }
    if (dot > 0.9995f) {
        // Linear interpolation for small angles
        Quat r = { a.x + t*(bt.x - a.x), a.y + t*(bt.y - a.y),
                   a.z + t*(bt.z - a.z), a.w + t*(bt.w - a.w) };
        return r.normalized();
    }
    f32 theta = acosf(dot);
    f32 sin_theta = sinf(theta);
    f32 sa = sinf((1.0f - t) * theta) / sin_theta;
    f32 sb = sinf(t * theta) / sin_theta;
    return { sa*a.x + sb*bt.x, sa*a.y + sb*bt.y,
             sa*a.z + sb*bt.z, sa*a.w + sb*bt.w };
}

// Quaternion log: maps unit quaternion to rotation vector (angle-axis / 2)
inline Vec3 quat_log(const Quat& q) {
    f32 len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z);
    if (len < 1e-10f) return { 0, 0, 0 };
    f32 half_angle = atan2f(len, q.w);
    f32 s = half_angle / len;
    return { q.x * s, q.y * s, q.z * s };
}

// Quaternion exp: maps rotation vector (angle-axis / 2) back to unit quaternion
inline Quat quat_exp(const Vec3& v) {
    f32 len = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len < 1e-10f) return { 0, 0, 0, 1 };
    f32 s = sinf(len) / len;
    return { v.x * s, v.y * s, v.z * s, cosf(len) };
}

// Angular velocity (rad/s) between two quaternions sampled dt seconds apart
inline Vec3 angular_velocity_between(const Quat& from, const Quat& to, f32 dt) {
    if (dt < 1e-6f) return { 0, 0, 0 };
    Quat delta = to * from.conjugated();
    Vec3 log_delta = quat_log(delta);
    return { log_delta.x * 2.0f / dt, log_delta.y * 2.0f / dt, log_delta.z * 2.0f / dt };
}

// Extrapolate quaternion forward by dt seconds given angular velocity (rad/s)
inline Quat quat_extrapolate(const Quat& q, const Vec3& angular_vel, f32 dt) {
    Vec3 half_step = { angular_vel.x * dt * 0.5f,
                       angular_vel.y * dt * 0.5f,
                       angular_vel.z * dt * 0.5f };
    Quat dq = quat_exp(half_step);
    return (q * dq).normalized();
}

struct Matrix4 {
    std::array<f32, 16> m = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    f32& operator[](i32 i) { return m[i]; }
    const f32& operator[](i32 i) const { return m[i]; }

    static Matrix4 identity() { return Matrix4(); }

    friend Matrix4 multiply(const Matrix4& a, const Matrix4& b);
    static Matrix4 perspective(f32 fov_y, f32 aspect, f32 z_near, f32 z_far) {
        Matrix4 m;
        f32 f = 1.0f / tanf(fov_y / 2.0f);
        m.m[0] = f / aspect;
        m.m[5] = f;
        m.m[10] = (z_far + z_near) / (z_near - z_far);
        m.m[11] = -1.0f;
        m.m[14] = 2.0f * z_far * z_near / (z_near - z_far);
        m.m[15] = 0.0f;
        return m;
    }

    // D3D convention: clip-space Z in [0, w], matching the hardware depth
    // buffer directly. Required when reconstructing world positions from a
    // captured depth texture without a remap.
    static Matrix4 perspective_d3d(f32 fov_y, f32 aspect, f32 z_near, f32 z_far) {
        Matrix4 m;
        f32 f = 1.0f / tanf(fov_y / 2.0f);
        m.m[0] = f / aspect;
        m.m[5] = f;
        m.m[10] = z_far / (z_near - z_far);
        m.m[11] = -1.0f;
        m.m[14] = (z_far * z_near) / (z_near - z_far);
        m.m[15] = 0.0f;
        return m;
    }

    static Matrix4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = { target.x - eye.x, target.y - eye.y, target.z - eye.z };
        f32 flen = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z);
        if (flen > 0) { f.x /= flen; f.y /= flen; f.z /= flen; }
        Vec3 s = { f.y*up.z - f.z*up.y, f.z*up.x - f.x*up.z, f.x*up.y - f.y*up.x };
        f32 slen = sqrtf(s.x*s.x + s.y*s.y + s.z*s.z);
        if (slen > 0) { s.x /= slen; s.y /= slen; s.z /= slen; }
        Vec3 u = { s.y*f.z - s.z*f.y, s.z*f.x - s.x*f.z, s.x*f.y - s.y*f.x };
        Matrix4 m;
        m.m[0] = s.x; m.m[4] = s.y; m.m[8]  = s.z; m.m[12] = -(s.x*eye.x + s.y*eye.y + s.z*eye.z);
        m.m[1] = u.x; m.m[5] = u.y; m.m[9]  = u.z; m.m[13] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
        m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z; m.m[14] = f.x*eye.x + f.y*eye.y + f.z*eye.z;
        m.m[3] = 0; m.m[7] = 0; m.m[11] = 0; m.m[15] = 1;
        return m;
    }

    static Matrix4 translation(f32 x, f32 y, f32 z) {
        Matrix4 m;
        m.m[12] = x; m.m[13] = y; m.m[14] = z;
        return m;
    }

    static Matrix4 rotation_ypr(f32 yaw, f32 pitch, f32 roll) {
        f32 cy = cosf(yaw), sy = sinf(yaw);
        f32 cp = cosf(pitch), sp = sinf(pitch);
        f32 cr = cosf(roll), sr = sinf(roll);
        Matrix4 m;
        m.m[0] = cy*cr + sy*sp*sr; m.m[4] = -cy*sr + sy*sp*cr; m.m[8]  = sy*cp;
        m.m[1] = cp*sr;            m.m[5] = cp*cr;             m.m[9]  = -sp;
        m.m[2] = -sy*cr + cy*sp*sr; m.m[6] = sy*sr + cy*sp*cr; m.m[10] = cy*cp;
        return m;
    }
};

// General 4x4 inverse (cofactor expansion). Returns identity if the matrix
// is singular. Needed to feed inverse view-projection to the reprojection
// shader for depth -> world-position reconstruction.
inline Matrix4 inverse(const Matrix4& src) {
    const auto& m = src.m;
    std::array<f32, 16> inv;

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
               m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
               m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
               m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
               m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
               m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
               m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
               m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
               m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
               m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
               m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
               m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
               m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
               m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
               m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
               m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
               m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    f32 det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-12f) return Matrix4::identity();

    f32 inv_det = 1.0f / det;
    Matrix4 out;
    for (i32 i = 0; i < 16; i++) out.m[i] = inv[i] * inv_det;
    return out;
}

inline Matrix4 multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 r;
    for (i32 i = 0; i < 4; i++) {
        for (i32 j = 0; j < 4; j++) {
            r.m[i * 4 + j] =
                a.m[i * 4 + 0] * b.m[0 * 4 + j] +
                a.m[i * 4 + 1] * b.m[1 * 4 + j] +
                a.m[i * 4 + 2] * b.m[2 * 4 + j] +
                a.m[i * 4 + 3] * b.m[3 * 4 + j];
        }
    }
    return r;
}

struct HeadPose {
    Vec3 position;
    Quat rotation;
    f64  timestamp_ms;
    f64  confidence;
};

struct ViewSetup {
    Matrix4 view;
    Matrix4 projection;
    Matrix4 view_projection;
    Vec3    position;
    Quat    rotation;
    f32     fov_horizontal = 90.0f;
    f32     fov_vertical   = 80.0f;
    f32     near_plane     = 0.01f;
    f32     far_plane      = 1000.0f;
    f32     eye_separation = 0.064f;
};

struct EyeViews {
    ViewSetup left;
    ViewSetup right;
};

enum class GraphicsAPI : u8 {
    Unknown,
    D3D11,
    D3D12,
    OpenGL,
    Vulkan
};

enum class VRBackend : u8 {
    None,
    OpenXR,
    SteamVR,
    OculusSDK
};

enum class LogLevel : u8 {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

// Selects how the captured game frame is shown in the headset.
//   Screen  — the game frame on a flat virtual screen (XrCompositionLayerQuad).
//             The runtime renders the quad per eye, so stereo, IPD and
//             reprojection are correct by construction. Default, D3D11+D3D12.
//   Depth3D — per-pixel reprojection of the game frame using the game's depth
//             buffer (D3D11 only). Falls back to Screen when no usable depth
//             buffer has been captured. Experimental.
// The old value 2 ("TrueStereo", mouse-yaw alternate-frame capture) was
// removed: rotating a camera produces no depth-dependent parallax. Config
// files that still contain 2 are loaded as Screen.
enum class RenderMode : u8 {
    Screen     = 0,
    Depth3D    = 1,
};

// Selects how VR input reaches the game.
//   LegacySendInput — current Win32 SendInput path. Default and safest.
//   SixDofBridge    — still injects legacy actions when enabled, and publishes
//                     head/controller/FBT poses through a named shared-memory
//                     bridge plus exported C API for game-side mods.
enum class InjectionMethod : u8 {
    LegacySendInput = 0,
    SixDofBridge    = 1,
};

struct ConfigProfile {
    std::string name = "default";
    f32 ipd = 0.064f;
    f32 world_scale = 1.0f;
    // Screen mode: distance from the viewer to the virtual screen (metres).
    // Depth3D: distance at which the game image plane is placed.
    f32 convergence_distance = 5.0f;
    // Screen mode: true = screen fixed in the room (world-locked, LOCAL space);
    // false = screen follows the head (head-locked, VIEW space).
    bool enable_head_tracking = true;
    // Horizontal angular size of the virtual screen in degrees. 0 = default
    // (kDefaultScreenFovDegrees). Also the image-plane FOV in Depth3D.
    f32 fov_override = 0.0f;
    // Multiplier on eye/quad swapchain resolution. Applied when the XR session
    // (and its swapchains) is created, i.e. on the next start, not live.
    f32 render_scale = 1.0f;
    RenderMode render_mode = RenderMode::Screen;
    std::string input_profile = "default_vr";
    // Depth3D only: swap which reprojected view goes to which eye. Screen mode
    // is rendered per-eye by the runtime and cannot be swapped.
    bool swap_eyes = false;
    // Depth3D only: tints the top-right corner of the view rendered FOR the
    // left eye red and FOR the right eye blue (after swap_eyes is applied).
    bool show_eye_debug = false;

    // Input injection selection and the optional 6DoF/FBT data bridge.
    InjectionMethod injection_method = InjectionMethod::LegacySendInput;
    bool bridge_send_legacy_input = true;
    std::string bridge_name = "Local\\VRGameConverter_6DoF_v1";

    // Optional game-side mod injected by the companion's Play in VR loader.
    // Empty path means "use the bundled vrc_big_walk_mod.dll" when available.
    bool game_mod_enabled = false;
    std::string game_mod_path;

    bool fbt_enabled = false;
    std::string fbt_hips_role = "/user/vive_tracker_htcx/role/waist";
    std::string fbt_left_foot_role = "/user/vive_tracker_htcx/role/left_foot";
    std::string fbt_right_foot_role = "/user/vive_tracker_htcx/role/right_foot";
    std::string fbt_chest_role = "/user/vive_tracker_htcx/role/chest";
    std::string fbt_left_shoulder_role = "/user/vive_tracker_htcx/role/left_shoulder";
    std::string fbt_right_shoulder_role = "/user/vive_tracker_htcx/role/right_shoulder";

    // Volumetric parallax. When depth capture succeeds and render_mode is
    // Depth3D, the real depth reprojection path is used instead of the fake
    // stereo blit. Falls back to the blit when depth is unavailable.
    bool volumetric_parallax = false;
    f32 depth_near_plane = 0.01f;
    f32 depth_far_plane = 1000.0f;
    f32 depth_fov_degrees = 90.0f;
    f32 parallax_strength = 1.0f;
    // Unity (D3D11) and Unreal both use reversed-Z depth (near = 1, far = 0).
    bool depth_reversed_z = true;
};

// Default horizontal angular size of the Screen-mode virtual screen.
inline constexpr f32 kDefaultScreenFovDegrees = 60.0f;

struct HookContext {
    void* device = nullptr;
    void* swap_chain = nullptr;
    void* command_queue = nullptr;
    GraphicsAPI api = GraphicsAPI::Unknown;
    u32 width = 0;
    u32 height = 0;
    bool is_fullscreen = false;
};

struct FrameCapture {
    void* resource = nullptr;
    u32 width = 0;
    u32 height = 0;
    u64 frame_index = 0;
    f64 timestamp_ms = 0.0;

    // Optional game depth-stencil texture captured by the D3D11 hook. The hook
    // AddRefs it for the capture; the consumer must Release it exactly once.
    void* depth_texture = nullptr;
    u32 depth_width = 0;
    u32 depth_height = 0;
    u32 depth_format = 0;        // DXGI_FORMAT as u32; 0 = unknown
    u32 depth_sample_count = 0;  // 0 = no depth captured, 1 = single-sample
};

struct LatencyStats {
    f64 present_latency_ms = 0.0;
    f64 render_latency_ms = 0.0;
    f64 tracking_latency_ms = 0.0;
    f64 total_motion_to_photon_ms = 0.0;
    u32 fps = 0;
};

enum class CompatibilityGrade : u8 {
    A = 0,
    B,
    C,
    D,
    Untested
};

using OnFrameCallback = std::function<void(const FrameCapture&)>;
using OnPresentCallback = std::function<bool(HookContext& ctx)>;

} // namespace vrc
