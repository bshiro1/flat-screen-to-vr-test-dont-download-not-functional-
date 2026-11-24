#pragma once

#include "core/types.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifdef VRC_BRIDGE_EXPORTS
#    define VRC_BRIDGE_API extern "C" __declspec(dllexport)
#  else
#    define VRC_BRIDGE_API extern "C" __declspec(dllimport)
#  endif
#else
#  define VRC_BRIDGE_API extern "C"
#endif

namespace vrc {

// Fixed-layout state published by the "6DoF Bridge" injection method. This
// struct is consumed by game-side mods through either the exported C API or
// the named shared-memory mapping. Do not add std:: types or virtuals here.
//
// Coordinate system: OpenXR LOCAL reference space, metres, XrPosef quaternion
// order (x, y, z, w). Poses are sampled at the predicted display time.
constexpr u32 kVrcXRStateMagic = 0x58435256; // 'VRCX'
constexpr u32 kVrcXRStateVersion = 1;
constexpr u32 kVrcTrackerSlotCount = 6;
constexpr u32 kVrcHandJointCount = 26;

enum VrcPoseFlags : u32 {
    VrcPoseFlag_PositionValid    = 1u << 0,
    VrcPoseFlag_OrientationValid = 1u << 1,
    VrcPoseFlag_PositionTracked  = 1u << 2,
    VrcPoseFlag_OrientationTracked = 1u << 3,
};

enum VrcTrackerSlot : u32 {
    VrcTracker_Hips = 0,
    VrcTracker_Chest,
    VrcTracker_LeftFoot,
    VrcTracker_RightFoot,
    VrcTracker_LeftShoulder,
    VrcTracker_RightShoulder,
};

// Slot indices for VrcXRState::hand_joints[hand]. These match the OpenXR
// XR_EXT_hand_tracking joint order so consumers can index by enum value.
// The current bridge reserves the array but does not populate it until hand
// tracking is enabled; always check VrcPoseState::flags before use.
enum VrcHandJoint : u32 {
    VrcHandJoint_Palm = 0,
    VrcHandJoint_Wrist,
    VrcHandJoint_ThumbMetacarpal,
    VrcHandJoint_ThumbPhalanxProximal,
    VrcHandJoint_ThumbPhalanxDistal,
    VrcHandJoint_ThumbTip,
    VrcHandJoint_IndexFingerMetacarpal,
    VrcHandJoint_IndexFingerPhalanxProximal,
    VrcHandJoint_IndexFingerPhalanxIntermediate,
    VrcHandJoint_IndexFingerPhalanxDistal,
    VrcHandJoint_IndexFingerTip,
    VrcHandJoint_MiddleFingerMetacarpal,
    VrcHandJoint_MiddleFingerPhalanxProximal,
    VrcHandJoint_MiddleFingerPhalanxIntermediate,
    VrcHandJoint_MiddleFingerPhalanxDistal,
    VrcHandJoint_MiddleFingerTip,
    VrcHandJoint_RingFingerMetacarpal,
    VrcHandJoint_RingFingerPhalanxProximal,
    VrcHandJoint_RingFingerPhalanxIntermediate,
    VrcHandJoint_RingFingerPhalanxDistal,
    VrcHandJoint_RingFingerTip,
    VrcHandJoint_LittleFingerMetacarpal,
    VrcHandJoint_LittleFingerPhalanxProximal,
    VrcHandJoint_LittleFingerPhalanxIntermediate,
    VrcHandJoint_LittleFingerPhalanxDistal,
    VrcHandJoint_LittleFingerTip,
};

enum VrcButtonFlags : u32 {
    VrcButton_A              = 1u << 0,
    VrcButton_B              = 1u << 1,
    VrcButton_X              = 1u << 2,
    VrcButton_Y              = 1u << 3,
    VrcButton_Menu           = 1u << 4,
    VrcButton_StickClickLeft = 1u << 5,
    VrcButton_StickClickRight = 1u << 6,
    VrcButton_TriggerClickLeft = 1u << 7,
    VrcButton_TriggerClickRight = 1u << 8,
    VrcButton_GripClickLeft  = 1u << 9,
    VrcButton_GripClickRight = 1u << 10,
};

// Global state flags published in VrcXRState::flags. Pose-level validity is
// still carried per pose in VrcPoseState::flags; these are convenience bits for
// consumers that need a single cheap health/tracking check.
enum VrcStateFlags : u32 {
    VrcState_HeadTracked       = 1u << 0,
    VrcState_LeftGripTracked   = 1u << 1,
    VrcState_RightGripTracked  = 1u << 2,
    VrcState_LeftAimTracked    = 1u << 3,
    VrcState_RightAimTracked   = 1u << 4,
    VrcState_FbtEnabled        = 1u << 5,
    VrcState_HapticsAvailable  = 1u << 6,
    VrcState_HandTrackingActive = 1u << 7,
};

// Indices into VrcXRState::projection_tangents[eye]. Values are the standard
// OpenXR frustum tangents (tan of the half-angle), not degrees.
enum VrcProjectionTangent : u32 {
    VrcProjection_Left  = 0,
    VrcProjection_Right = 1,
    VrcProjection_Up    = 2,
    VrcProjection_Down  = 3,
};

enum VrcReferenceSpace : u32 {
    VrcReferenceSpace_Local      = 0,
    VrcReferenceSpace_LocalFloor = 1,
};

struct VrcPoseState {
    f32 position[3] = { 0, 0, 0 };
    f32 rotation[4] = { 0, 0, 0, 1 };
    u32 flags = 0;
    f32 radius = 0.0f;
    f32 linear_velocity[3] = { 0, 0, 0 };
    f32 angular_velocity[3] = { 0, 0, 0 };
};

struct VrcXRState {
    u32 magic = kVrcXRStateMagic;
    u32 version = kVrcXRStateVersion;
    u32 size_bytes = sizeof(VrcXRState);
    u32 sequence = 0;

    u64 timestamp_ns = 0;
    u32 flags = 0;
    u32 reference_space = 0;     // 0 = LOCAL, 1 = LOCAL_FLOOR
    u32 injection_method = 0;    // 0 = Legacy SendInput, 1 = 6DoF Bridge
    u32 fbt_enabled = 0;
    f32 world_scale = 1.0f;

    VrcPoseState head{};
    VrcPoseState grip[2]{};      // 0 = left, 1 = right
    VrcPoseState aim[2]{};
    VrcPoseState trackers[kVrcTrackerSlotCount]{};
    // Indexed by VrcHandJoint. Populated when XR_EXT_hand_tracking is available;
    // otherwise zero/invalid. Check VrcState_HandTrackingActive and per-joint
    // flags before consuming finger poses.
    VrcPoseState hand_joints[2][kVrcHandJointCount]{};

    f32 trigger[2] = { 0, 0 };
    f32 grip_axis[2] = { 0, 0 };
    f32 thumbstick[2][2] = {};
    u32 button_flags = 0;

    // Frame-timing and per-eye projection data. These reuse former reserved
    // floats, so sizeof(VrcXRState) is unchanged and v1 consumers remain ABI
    // compatible; older readers simply see reserved values become non-zero.
    f32 display_period_s = 0.0f;              // XrFrameState::predictedDisplayPeriod in seconds
    f32 ipd = 0.0f;                           // measured from the two located eye poses
    f32 projection_tangents[2][4] = {};       // [eye][VrcProjectionTangent]
    f32 reserved[22] = {};
};

// Shared-memory layout. Writer performs a seqlock publish; readers retry when
// the sequence is odd or changes between reads.
struct VrcXRSharedMapping {
    volatile u32 sequence = 0;
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    u32 reserved2 = 0;
    VrcXRState state{};
};

// v1 consumers rely on these exact sizes. Frame metrics reused former reserved
// floats, so the layout must not grow.
static_assert(sizeof(VrcXRState) == 3992, "VrcXRState ABI size changed");
static_assert(alignof(VrcXRState) == 8, "VrcXRState ABI alignment changed");
static_assert(sizeof(VrcXRSharedMapping) == 4008, "Shared mapping ABI size changed");

class XRStateBridge {
public:
    static XRStateBridge& instance();

    bool initialize(const std::string& name, u32 injection_method = 0);
    void shutdown();

    bool is_initialized() const;
    // Effective mode fixed when the bridge was initialized, not a live config
    // value that can drift after runtime profile edits.
    u32 injection_method() const {
        return effective_injection_method_.load(std::memory_order_acquire);
    }
    // Legacy pointer API. Prefer copy_shared_name()/VRC_CopyXRSharedMemoryName
    // when the name must survive a bridge reinitialization.
    const char* shared_name() const;
    u32 copy_shared_name(char* dst, u32 dst_size) const;
    // Frame sequence from the last published VrcXRState (not the seqlock tick).
    u32 sequence_for_readers() const {
        return state_sequence_.load(std::memory_order_acquire);
    }

    void publish(const VrcXRState& state);
    bool read(VrcXRState& out) const;
    // Size-checked alternative to VRC_GetXRState: never writes past a caller
    // buffer that was compiled against a different struct size.
    bool copy_state(VrcXRState& out, u32 out_size) const;

    // Game-side mods request haptics here from the game thread. Requests are
    // drained by XrInput on the XR frame thread, which owns all OpenXR calls.
    bool request_haptic(bool left, f32 amplitude, f32 duration_s);

    template <typename Fn>
    void drain_haptics(Fn&& fn) {
        // Never hold the game-thread-facing lock while calling OpenXR: a slow
        // xrApplyHapticFeedback must not block VRC_RequestHaptic().
        std::vector<HapticRequest> requests;
        {
            std::lock_guard<std::mutex> lock(haptic_mutex_);
            requests.swap(haptic_queue_);
        }
        for (const auto& request : requests) fn(request);
    }

private:
    XRStateBridge() = default;

    struct HapticRequest {
        bool left = false;
        f32 amplitude = 0.0f;
        f32 duration_s = 0.0f;
    };

    void shutdown_locked(); // mapping_mutex_ must already be held
    void shutdown_mappings(); // takes mapping_mutex_, not init_mutex_
    bool initialize_locked(const std::string& name, u32 injection_method);

    void* mapping_ = nullptr;  // HANDLE
    void* view_ = nullptr;
    char shared_name_[256]{};
    std::atomic<u32> state_sequence_{0};
    std::atomic<u32> effective_injection_method_{0};
    // Serializes initialize()/shutdown() so two config reloads cannot leak or
    // silently replace an already-published shared-memory section.
    std::mutex init_mutex_;
    mutable std::mutex mapping_mutex_;

    std::mutex haptic_mutex_;
    std::vector<HapticRequest> haptic_queue_;
};

} // namespace vrc

// Exported bridge API for game-side mods. Prefer VRC_GetXRState() when the
// mod runs in the same process; cross-process consumers can open the named
// mapping directly using VRC_GetXRSharedMemoryName().
VRC_BRIDGE_API vrc::u32 VRC_GetBridgeApiVersion();
VRC_BRIDGE_API vrc::u32 VRC_GetInjectionMethod();
VRC_BRIDGE_API bool VRC_GetXRState(vrc::VrcXRState* out);
// Preferred for external mods: pass sizeof(VrcXRState) from your build. Returns
// false without writing when the caller's buffer is too small.
VRC_BRIDGE_API bool VRC_CopyXRState(vrc::VrcXRState* out, vrc::u32 out_size);
VRC_BRIDGE_API const char* VRC_GetXRSharedMemoryName();
// Safe alternative: copies into a caller-owned buffer and returns the number of
// bytes written (excluding the terminator). Additive API; ABI stays version 1.
VRC_BRIDGE_API vrc::u32 VRC_CopyXRSharedMemoryName(char* dst, vrc::u32 dst_size);
VRC_BRIDGE_API vrc::u32 VRC_GetXRStateSequence();
// Queue a controller vibration request. Safe to call from the game thread;
// applied on the XR frame thread on the next sync().
VRC_BRIDGE_API bool VRC_RequestHaptic(bool left, vrc::f32 amplitude,
                                      vrc::f32 duration_s);
