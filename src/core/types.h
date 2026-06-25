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

struct ConfigProfile {
    std::string name = "default";
    f32 ipd = 0.064f;
    f32 world_scale = 1.0f;
    f32 convergence_distance = 5.0f;
    f32 eye_height = 1.6f;
    bool enable_head_tracking = true;
    bool enable_chaperone = false;
    f32 fov_override = 0.0f;
    bool foveated_rendering = false;
    bool dynamic_resolution = true;
    f32 render_scale = 1.0f;
    std::string input_profile = "default_vr";
};

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
