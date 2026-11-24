// Big Walk game-side mod for VR Game Converter's 6DoF Bridge.
//
// Injected by the companion's Play in VR button when the active config profile
// has game_mod_enabled = true. The mod hooks PlayerArms::LateUpdate in the
// Unity IL2CPP GameAssembly, reads VrcXRState from vr_converter.dll, and moves
// the local player's hand transforms (and limb splines) to the tracked
// controller grip poses.
//
// This is intentionally standalone: it resolves IL2CPP through GetProcAddress
// and reads the bridge through GetProcAddress, so it has no link-time dependency
// on GameAssembly.dll or vr_converter.dll.

#include <windows.h>

#include <MinHook.h>

#include "input/xr_shared_state.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace {

// ─── Logging ──────────────────────────────────────────────────────────────────

std::string temp_log_path() {
    char temp[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableA("TEMP", temp, MAX_PATH);
    std::string dir = (len > 0 && len < MAX_PATH) ? temp : ".";
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir + "\\vrc_big_walk_mod.log";
}

std::mutex g_log_mutex;

void rotate_previous_log() {
    // Caller holds g_log_mutex.
    static bool done = false;
    if (done) return;
    done = true;

    const std::string path = temp_log_path();
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    char stamp[32]{};
    std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now) == 0) {
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    }
    std::string rotated = path + "." + (stamp[0] ? stamp : "old");
    for (int i = 1; i < 20; ++i) {
        if (GetFileAttributesA(rotated.c_str()) == INVALID_FILE_ATTRIBUTES) break;
        char suffix[16]{};
        std::snprintf(suffix, sizeof(suffix), ".%d", i);
        rotated = path + "." + (stamp[0] ? stamp : "old") + suffix;
    }
    MoveFileA(path.c_str(), rotated.c_str());
}

void logf_line(const char* fmt, ...) {
    char msg[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Hooks can run on different Unity threads. Serialise rotation and all
    // writes so lines never interleave and the log is rotated only once.
    std::lock_guard<std::mutex> lock(g_log_mutex);
    rotate_previous_log();

    OutputDebugStringA("[vrc-big-walk] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    if (FILE* f = std::fopen(temp_log_path().c_str(), "a")) {
        std::fprintf(f, "%s\n", msg);
        std::fclose(f);
    }
}

#define MOD_LOG(...) logf_line(__VA_ARGS__)

// ─── IL2CPP exports ──────────────────────────────────────────────────────────

using FnDomainGet = void* (*)();
using FnDomainGetAssemblies = void** (*)(void* /*domain*/, size_t* /*count*/);
using FnAssemblyGetImage = void* (*)(void* /*assembly*/);
using FnClassFromName = void* (*)(void* /*image*/, const char* /*ns*/, const char* /*name*/);
using FnClassGetParent = void* (*)(void* /*class*/);
using FnClassGetField = void* (*)(void* /*class*/, const char* /*name*/);
using FnClassGetMethod = void* (*)(void* /*class*/, const char* /*name*/, int /*param_count*/);
using FnFieldGetValue = void (*)(void* /*obj*/, void* /*field*/, void* /*out*/);
using FnFieldStaticGetValue = void (*)(void* /*field*/, void* /*out*/);
using FnRuntimeInvoke = void* (*)(void* /*method*/, void* /*obj*/, void** /*params*/, void** /*exception*/);
using FnObjectGetClass = void* (*)(void* /*object*/);
using FnThreadAttach = void* (*)(void* /*domain*/);
using FnGcHandleNewWeakRef = unsigned int (*)(void* /*obj*/, bool /*track_resurrection*/);
using FnGcHandleGetTarget = void* (*)(unsigned int /*handle*/);
using FnGcHandleFree = void (*)(unsigned int /*handle*/);

struct Il2CppApi {
    HMODULE game_assembly = nullptr;
    FnDomainGet domain_get = nullptr;
    FnDomainGetAssemblies domain_get_assemblies = nullptr;
    FnAssemblyGetImage assembly_get_image = nullptr;
    FnClassFromName class_from_name = nullptr;
    FnClassGetParent class_get_parent = nullptr;
    FnClassGetField class_get_field = nullptr;
    FnClassGetMethod class_get_method = nullptr;
    FnFieldGetValue field_get_value = nullptr;
    FnFieldStaticGetValue field_static_get_value = nullptr;
    FnRuntimeInvoke runtime_invoke = nullptr;
    FnObjectGetClass object_get_class = nullptr;
    FnThreadAttach thread_attach = nullptr;
    FnGcHandleNewWeakRef gc_handle_new_weakref = nullptr;
    FnGcHandleGetTarget gc_handle_get_target = nullptr;
    FnGcHandleFree gc_handle_free = nullptr;
    void* domain = nullptr;
} g_il2cpp;

template <typename T>
T get_export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool load_il2cpp_api() {
    g_il2cpp.game_assembly = GetModuleHandleW(L"GameAssembly.dll");
    if (!g_il2cpp.game_assembly) return false;

    HMODULE ga = g_il2cpp.game_assembly;
    g_il2cpp.domain_get = get_export<FnDomainGet>(ga, "il2cpp_domain_get");
    g_il2cpp.domain_get_assemblies = get_export<FnDomainGetAssemblies>(ga, "il2cpp_domain_get_assemblies");
    g_il2cpp.assembly_get_image = get_export<FnAssemblyGetImage>(ga, "il2cpp_assembly_get_image");
    g_il2cpp.class_from_name = get_export<FnClassFromName>(ga, "il2cpp_class_from_name");
    g_il2cpp.class_get_parent = get_export<FnClassGetParent>(ga, "il2cpp_class_get_parent");
    g_il2cpp.class_get_field = get_export<FnClassGetField>(ga, "il2cpp_class_get_field_from_name");
    g_il2cpp.class_get_method = get_export<FnClassGetMethod>(ga, "il2cpp_class_get_method_from_name");
    g_il2cpp.field_get_value = get_export<FnFieldGetValue>(ga, "il2cpp_field_get_value");
    g_il2cpp.field_static_get_value = get_export<FnFieldStaticGetValue>(ga, "il2cpp_field_static_get_value");
    g_il2cpp.runtime_invoke = get_export<FnRuntimeInvoke>(ga, "il2cpp_runtime_invoke");
    g_il2cpp.object_get_class = get_export<FnObjectGetClass>(ga, "il2cpp_object_get_class");
    g_il2cpp.thread_attach = get_export<FnThreadAttach>(ga, "il2cpp_thread_attach");
    // Optional: weak handles keep cached UnityEngine objects observable without
    // pinning dead scene objects in memory. All three exports are required.
    g_il2cpp.gc_handle_new_weakref =
        get_export<FnGcHandleNewWeakRef>(ga, "il2cpp_gchandle_new_weakref");
    g_il2cpp.gc_handle_get_target =
        get_export<FnGcHandleGetTarget>(ga, "il2cpp_gchandle_get_target");
    g_il2cpp.gc_handle_free = get_export<FnGcHandleFree>(ga, "il2cpp_gchandle_free");
    if (!g_il2cpp.gc_handle_new_weakref || !g_il2cpp.gc_handle_get_target ||
        !g_il2cpp.gc_handle_free) {
        g_il2cpp.gc_handle_new_weakref = nullptr;
        g_il2cpp.gc_handle_get_target = nullptr;
        g_il2cpp.gc_handle_free = nullptr;
    }

    if (!g_il2cpp.domain_get || !g_il2cpp.domain_get_assemblies ||
        !g_il2cpp.assembly_get_image || !g_il2cpp.class_from_name ||
        !g_il2cpp.class_get_parent || !g_il2cpp.class_get_field ||
        !g_il2cpp.class_get_method || !g_il2cpp.field_get_value ||
        !g_il2cpp.field_static_get_value || !g_il2cpp.runtime_invoke ||
        !g_il2cpp.object_get_class ||
        !g_il2cpp.thread_attach) {
        return false;
    }

    g_il2cpp.domain = g_il2cpp.domain_get();
    return g_il2cpp.domain != nullptr;
}

void* find_class(const char* ns, const char* name) {
    if (!g_il2cpp.domain || !g_il2cpp.class_from_name) return nullptr;
    size_t count = 0;
    void** assemblies = g_il2cpp.domain_get_assemblies(g_il2cpp.domain, &count);
    if (!assemblies) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = g_il2cpp.assembly_get_image(assemblies[i]);
        if (!image) continue;
        void* cls = g_il2cpp.class_from_name(image, ns, name);
        if (cls) return cls;
    }
    return nullptr;
}

void* find_method(void* cls, const char* name, int param_count) {
    while (cls) {
        void* method = g_il2cpp.class_get_method(cls, name, param_count);
        if (method) return method;
        cls = g_il2cpp.class_get_parent(cls);
    }
    return nullptr;
}

void* field_value(void* obj, void* field) {
    void* value = nullptr;
    if (obj && field) g_il2cpp.field_get_value(obj, field, &value);
    return value;
}

void* static_field_value(void* field) {
    void* value = nullptr;
    if (field) g_il2cpp.field_static_get_value(field, &value);
    return value;
}

void* invoke(void* method, void* obj, void** params = nullptr) {
    if (!method) return nullptr;
    void* exception = nullptr;
    void* result = g_il2cpp.runtime_invoke(method, obj, params, &exception);
    if (exception) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) MOD_LOG("IL2CPP exception during runtime_invoke");
    }
    return result;
}

// ─── Unity types / small math helpers ────────────────────────────────────────

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

Quat quat_mul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat quat_conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

Quat quat_normalize(Quat q) {
    float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (n <= 1e-8f) return {0, 0, 0, 1};
    n = 1.0f / std::sqrt(n);
    return {q.x * n, q.y * n, q.z * n, q.w * n};
}

Vec3 quat_rotate(const Quat& q, const Vec3& v) {
    Quat p{v.x, v.y, v.z, 0.0f};
    Quat r = quat_mul(quat_mul(q, p), quat_conjugate(q));
    return {r.x, r.y, r.z};
}

Vec3 vec_sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// OpenXR LOCAL is right-handed (X right, Y up, Z back). Unity is left-handed
// (Z forward). Position mirrors Z; a right-handed quaternion maps with
// (-x, -y, z, w).
Quat xr_quat_to_unity(const Quat& q) {
    return quat_normalize({-q.x, -q.y, q.z, q.w});
}

bool pose_usable(const vrc::VrcPoseState& pose) {
    constexpr vrc::u32 kNeed = vrc::VrcPoseFlag_PositionValid | vrc::VrcPoseFlag_OrientationValid;
    return (pose.flags & kNeed) == kNeed;
}

// ─── Cached IL2CPP reflection data ───────────────────────────────────────────

struct Cached {
    void* player_character_class = nullptr;
    void* all_players_field = nullptr;
    void* pc_arms_field = nullptr;
    void* pc_hands_field = nullptr;
    void* pc_camera_field = nullptr;
    void* pc_networking_field = nullptr;

    void* hands_grasper_guide = nullptr;
    void* hands_grasper = nullptr;

    void* arms_left_hand = nullptr;
    void* arms_right_hand = nullptr;
    void* arms_local_left_hand = nullptr;
    void* arms_local_right_hand = nullptr;
    void* arms_left_spline = nullptr;
    void* arms_right_spline = nullptr;
    void* arms_local_left_spline = nullptr;
    void* arms_local_right_spline = nullptr;
    void* arms_player_character = nullptr;

    void* limb_spline_refresh = nullptr;
    void* limb_spline_mesh_renderer = nullptr;
    void* renderer_set_enabled = nullptr;
    bool arms_hidden = false;

    void* player = nullptr;
    void* arms = nullptr;
    unsigned int player_handle = 0;
    unsigned int arms_handle = 0;

    void* list_get_count = nullptr;
    void* list_get_item = nullptr;

    void* transform_get_position = nullptr;
    void* transform_set_position = nullptr;
    void* transform_get_rotation = nullptr;
    void* transform_set_rotation = nullptr;
    void* transform_set_local_scale = nullptr;
} g_cached;

void free_gc_handle(unsigned int& handle) {
    if (handle && g_il2cpp.gc_handle_free) g_il2cpp.gc_handle_free(handle);
    handle = 0;
}

void release_cached_handles() {
    free_gc_handle(g_cached.player_handle);
    free_gc_handle(g_cached.arms_handle);
    g_cached.player = nullptr;
    g_cached.arms = nullptr;
}

void* cached_player() {
    if (!g_cached.player) return nullptr;
    if (!g_il2cpp.gc_handle_get_target || !g_cached.player_handle) return g_cached.player;
    void* target = g_il2cpp.gc_handle_get_target(g_cached.player_handle);
    if (!target) {
        free_gc_handle(g_cached.player_handle);
        g_cached.player = nullptr;
        return nullptr;
    }
    return target;
}

void* cached_arms() {
    if (!g_cached.arms) return nullptr;
    if (!g_il2cpp.gc_handle_get_target || !g_cached.arms_handle) return g_cached.arms;
    void* target = g_il2cpp.gc_handle_get_target(g_cached.arms_handle);
    if (!target) {
        free_gc_handle(g_cached.arms_handle);
        g_cached.arms = nullptr;
        return nullptr;
    }
    return target;
}

std::atomic<bool> g_reflect_ready{false};

// Which PlayerArms instance had its spline meshes hidden. Tracked separately
// from the boolean so a scene reload can hide a new instance and stale-state
// recovery can restore the old one.
void* g_hidden_spline_arms = nullptr;
unsigned int g_hidden_spline_arms_handle = 0;

void set_arm_splines_enabled(void* arms, bool enabled) {
    if (!arms || !g_reflect_ready.load()) return;

    void* splines[4] = {
        field_value(arms, g_cached.arms_left_spline),
        field_value(arms, g_cached.arms_right_spline),
        field_value(arms, g_cached.arms_local_left_spline),
        field_value(arms, g_cached.arms_local_right_spline),
    };

    for (void* spline : splines) {
        if (!spline || !g_cached.limb_spline_mesh_renderer) continue;
        void* renderer = field_value(spline, g_cached.limb_spline_mesh_renderer);
        if (!renderer) continue;
        if (!g_cached.renderer_set_enabled) {
            void* cls = g_il2cpp.object_get_class(renderer);
            g_cached.renderer_set_enabled = find_method(cls, "set_enabled", 1);
        }
        if (!g_cached.renderer_set_enabled) continue;
        void* args[] = {&enabled};
        invoke(g_cached.renderer_set_enabled, renderer, args);
    }
}

bool restore_arm_splines() {
    if (!g_cached.arms_hidden) return false;

    void* arms = g_hidden_spline_arms;
    if (g_il2cpp.gc_handle_get_target && g_hidden_spline_arms_handle) {
        arms = g_il2cpp.gc_handle_get_target(g_hidden_spline_arms_handle);
    }
    if (arms) set_arm_splines_enabled(arms, true);

    free_gc_handle(g_hidden_spline_arms_handle);
    g_hidden_spline_arms = nullptr;
    g_cached.arms_hidden = false;
    MOD_LOG("Vanilla arm spline meshes restored");
    return true;
}

void release_hidden_spline_handle() {
    free_gc_handle(g_hidden_spline_arms_handle);
    g_hidden_spline_arms = nullptr;
    g_cached.arms_hidden = false;
}

std::atomic<bool> g_logged_local{false};
std::atomic<bool> g_logged_state{false};
std::atomic<bool> g_logged_apply{false};
std::atomic<int> g_debug_samples{0};
std::atomic<unsigned long long> g_tick{0};
std::atomic<unsigned int> g_hold_log_mask{0};
std::atomic<bool> g_logged_stale{false};

// Last state accepted from the bridge, reused by secondary hooks that run
// after PlayerArms::LateUpdate and would otherwise reapply vanilla transforms.
vrc::VrcXRState g_last_state{};
std::mutex g_last_state_mutex;
vrc::u32 g_last_sequence = 0;
std::atomic<bool> g_last_state_valid{false};
std::atomic<long long> g_last_state_ms{0};

// If the bridge stops publishing (converter unloaded, VR session ended), the
// secondary hooks must stop suppressing vanilla arm behavior and stop holding
// the old hand poses. One second is long enough to survive a frame hiccup.
constexpr long long kStateFreshMs = 1000;

bool state_fresh() {
    const auto deactivate = []() {
        if (restore_arm_splines() && !g_logged_stale.exchange(true)) {
            MOD_LOG("XR state inactive; vanilla arm behavior restored");
        }
    };

    if (!g_last_state_valid.load()) {
        deactivate();
        return false;
    }
    const long long last = g_last_state_ms.load();
    if (last <= 0) {
        deactivate();
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last > kStateFreshMs) {
        g_last_state_valid.store(false);
        g_last_state_ms.store(0);
        deactivate();
        return false;
    }
    return true;
}

Vec3 g_last_applied[2]{};
Quat g_last_applied_rot[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
bool g_has_last_applied[2] = {false, false};

// Hold the last accepted pose head-relative, not world-space, so a camera move
// during a tracking dropout does not leave the hand floating in old space.
Vec3 g_last_hold_rel_pos[2]{};
Quat g_last_hold_rel_rot[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
std::atomic<int> g_after_log_samples{0};

bool cache_transform_methods(void* transform) {
    if (!transform || !g_il2cpp.object_get_class) return false;
    void* cls = g_il2cpp.object_get_class(transform);
    if (!cls) return false;
    g_cached.transform_get_position = find_method(cls, "get_position", 0);
    g_cached.transform_set_position = find_method(cls, "set_position", 1);
    g_cached.transform_get_rotation = find_method(cls, "get_rotation", 0);
    g_cached.transform_set_rotation = find_method(cls, "set_rotation", 1);
    g_cached.transform_set_local_scale = find_method(cls, "set_localScale", 1);
    return g_cached.transform_get_position && g_cached.transform_set_position &&
           g_cached.transform_get_rotation && g_cached.transform_set_rotation;
}

Vec3 get_position(void* transform) {
    Vec3 out{};
    void* result = invoke(g_cached.transform_get_position, transform);
    if (result) std::memcpy(&out, result, sizeof(Vec3));
    return out;
}

Quat get_rotation(void* transform) {
    Quat out{0, 0, 0, 1};
    void* result = invoke(g_cached.transform_get_rotation, transform);
    if (result) std::memcpy(&out, result, sizeof(Quat));
    return out;
}

void set_position(void* transform, const Vec3& value) {
    void* args[] = {const_cast<Vec3*>(&value)};
    invoke(g_cached.transform_set_position, transform, args);
}

void set_rotation(void* transform, const Quat& value) {
    void* args[] = {const_cast<Quat*>(&value)};
    invoke(g_cached.transform_set_rotation, transform, args);
}

void set_unit_scale(void* transform) {
    if (!g_cached.transform_set_local_scale) return;
    Vec3 one{1.0f, 1.0f, 1.0f};
    void* args[] = {&one};
    invoke(g_cached.transform_set_local_scale, transform, args);
}

int list_count(void* list) {
    void* result = invoke(g_cached.list_get_count, list);
    return result ? *static_cast<int*>(result) : 0;
}

void* list_item(void* list, int index) {
    void* args[] = {&index};
    void* result = invoke(g_cached.list_get_item, list, args);
    return result ? *static_cast<void**>(result) : nullptr;
}

bool is_local_network_behaviour(void* behaviour) {
    if (!behaviour || !g_il2cpp.object_get_class) return false;
    void* cls = g_il2cpp.object_get_class(behaviour);
    void* method = find_method(cls, "get_isLocalPlayer", 0);
    void* result = invoke(method, behaviour);
    return result ? *static_cast<bool*>(result) : false;
}

bool prepare_reflection() {
    g_cached.player_character_class = find_class("", "PlayerCharacter");
    if (!g_cached.player_character_class) {
        MOD_LOG("PlayerCharacter class not found yet");
        return false;
    }

    g_cached.all_players_field = g_il2cpp.class_get_field(g_cached.player_character_class, "allPlayerCharacters");
    g_cached.pc_arms_field = g_il2cpp.class_get_field(g_cached.player_character_class, "arms");
    g_cached.pc_hands_field = g_il2cpp.class_get_field(g_cached.player_character_class, "hands");
    g_cached.pc_camera_field = g_il2cpp.class_get_field(g_cached.player_character_class, "cameraTransform");
    g_cached.pc_networking_field = g_il2cpp.class_get_field(g_cached.player_character_class, "playerNetworking");
    if (!g_cached.all_players_field || !g_cached.pc_arms_field || !g_cached.pc_camera_field) {
        MOD_LOG("PlayerCharacter fields missing (game version mismatch)");
        return false;
    }

    void* arms_class = find_class("", "PlayerArms");
    if (!arms_class) {
        MOD_LOG("PlayerArms class not found");
        return false;
    }
    g_cached.arms_left_hand = g_il2cpp.class_get_field(arms_class, "leftHand");
    g_cached.arms_right_hand = g_il2cpp.class_get_field(arms_class, "rightHand");
    g_cached.arms_local_left_hand = g_il2cpp.class_get_field(arms_class, "localLeftHand");
    g_cached.arms_local_right_hand = g_il2cpp.class_get_field(arms_class, "localRightHand");
    g_cached.arms_left_spline = g_il2cpp.class_get_field(arms_class, "leftArmLimbSpline");
    g_cached.arms_right_spline = g_il2cpp.class_get_field(arms_class, "rightArmLimbSpline");
    g_cached.arms_local_left_spline = g_il2cpp.class_get_field(arms_class, "localLeftArmSpline");
    g_cached.arms_local_right_spline = g_il2cpp.class_get_field(arms_class, "localRightArmSpline");
    g_cached.arms_player_character = g_il2cpp.class_get_field(arms_class, "playerCharacter");
    if (!g_cached.arms_left_hand || !g_cached.arms_right_hand) {
        MOD_LOG("PlayerArms hand fields missing");
        return false;
    }

    void* hands_class = find_class("", "PlayerHands");
    if (!hands_class) {
        MOD_LOG("PlayerHands class not found");
        return false;
    }
    g_cached.hands_grasper_guide = g_il2cpp.class_get_field(hands_class, "grasperGuide");
    g_cached.hands_grasper = g_il2cpp.class_get_field(hands_class, "grasper");
    if (!g_cached.hands_grasper_guide || !g_cached.hands_grasper) {
        MOD_LOG("PlayerHands grasper fields missing");
        return false;
    }

    void* spline_class = find_class("", "LimbSpline");
    if (!spline_class) {
        MOD_LOG("LimbSpline class not found");
        return false;
    }
    g_cached.limb_spline_refresh = find_method(spline_class, "Refresh", 0);
    if (!g_cached.limb_spline_refresh) MOD_LOG("LimbSpline.Refresh not found");
    g_cached.limb_spline_mesh_renderer = g_il2cpp.class_get_field(spline_class, "meshRenderer");
    if (!g_cached.limb_spline_mesh_renderer) MOD_LOG("LimbSpline.meshRenderer not found");

    g_reflect_ready.store(true);
    MOD_LOG("Reflection ready: PlayerCharacter/PlayerArms/PlayerHands/LimbSpline resolved");
    return true;
}

bool cache_local_player(void* pc, void* arms) {
    if (!pc || !arms) return false;
    void* networking = field_value(pc, g_cached.pc_networking_field);
    if (networking && !is_local_network_behaviour(networking)) return false;

    void* camera = field_value(pc, g_cached.pc_camera_field);
    if (!camera) return false;
    if (!g_cached.transform_get_position && !cache_transform_methods(camera)) return false;

    // Replace the old targets first. The new strong handles keep these objects
    // valid even if the scene is reloaded between hook calls.
    release_cached_handles();
    g_cached.player = pc;
    g_cached.arms = arms;
    if (g_il2cpp.gc_handle_new_weakref) {
        g_cached.player_handle = g_il2cpp.gc_handle_new_weakref(pc, false);
        g_cached.arms_handle = g_il2cpp.gc_handle_new_weakref(arms, false);
        if (!g_cached.player_handle || !g_cached.arms_handle) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                MOD_LOG("GC handle creation failed; falling back to raw object cache");
            }
        }
    }
    if (!g_logged_local.exchange(true)) {
        MOD_LOG("Local PlayerCharacter found; controller hands active");
    }
    return true;
}

void find_local_player(void* known_arms = nullptr, void* known_player = nullptr) {
    if (!g_reflect_ready.load()) return;

    // Fastest path: PlayerCharacter::LateUpdate already gives us the object.
    // A rejected candidate (remote player, missing camera) must not prevent the
    // slower fallbacks from finding the real local player.
    if (known_player) {
        if (cache_local_player(known_player,
                               known_arms ? known_arms
                                          : field_value(known_player, g_cached.pc_arms_field))) {
            return;
        }
    }

    // Next best: PlayerArms has a private playerCharacter reference. This avoids
    // depending on the static allPlayerCharacters list during early frames.
    if (known_arms && g_cached.arms_player_character) {
        void* pc = field_value(known_arms, g_cached.arms_player_character);
        if (pc && cache_local_player(pc, known_arms)) return;
    }

    // Fallback: static List<PlayerCharacter> allPlayerCharacters.
    void* list = static_field_value(g_cached.all_players_field);
    if (!list || !g_il2cpp.object_get_class) return;

    if (!g_cached.list_get_count || !g_cached.list_get_item) {
        void* list_class = g_il2cpp.object_get_class(list);
        g_cached.list_get_count = find_method(list_class, "get_Count", 0);
        g_cached.list_get_item = find_method(list_class, "get_Item", 1);
    }
    if (!g_cached.list_get_count || !g_cached.list_get_item) return;

    const int count = list_count(list);
    for (int i = 0; i < count; ++i) {
        void* pc = list_item(list, i);
        if (!pc) continue;
        void* networking = field_value(pc, g_cached.pc_networking_field);
        bool local = networking ? is_local_network_behaviour(networking) : (count == 1);
        if (!local) continue;

        void* arms = field_value(pc, g_cached.pc_arms_field);
        cache_local_player(pc, arms);
        return;
    }
}

void refresh_spline(void* spline) {
    if (spline && g_cached.limb_spline_refresh) invoke(g_cached.limb_spline_refresh, spline);
}

// The vanilla first-person arm splines stretch like rubber bands when the hand
// transforms are moved to real controller positions. The hands themselves are
// separate transforms, so hide only the spline meshes and leave the hands
// visible. This is the same tier-0 approach UEVR uses for first-person arms.
void hide_arm_splines(void* arms) {
    if (!arms) return;

    // Resolve through the weak handle before comparing. A collected old
    // PlayerArms can reuse the same address as the new instance; comparing the
    // stale raw pointer would skip hiding the new object's vanilla splines.
    void* hidden = g_hidden_spline_arms;
    if (g_il2cpp.gc_handle_get_target && g_hidden_spline_arms_handle) {
        hidden = g_il2cpp.gc_handle_get_target(g_hidden_spline_arms_handle);
        if (!hidden) release_hidden_spline_handle();
    }
    if (g_cached.arms_hidden && hidden == arms) return;

    // A new PlayerArms instance (scene reload) must restore the old meshes and
    // then hide its own; otherwise vanilla arms stay invisible forever.
    if (g_cached.arms_hidden) restore_arm_splines();

    set_arm_splines_enabled(arms, false);

    free_gc_handle(g_hidden_spline_arms_handle);
    g_hidden_spline_arms = arms;
    if (g_il2cpp.gc_handle_new_weakref) {
        g_hidden_spline_arms_handle = g_il2cpp.gc_handle_new_weakref(arms, false);
    }
    g_cached.arms_hidden = true;
    MOD_LOG("Vanilla arm spline meshes hidden; tracked hands remain visible");
}

// Writes one hand's full pose (visual hand, local hand and physics grasper).
// Also used when a controller pose is temporarily invalid: holding the last
// accepted pose is much better than letting the game snap the hand back.
void write_hand_pose(void* arms, void* player, int hand_index,
                     const Vec3& pos, const Quat& rot) {
    void* hand_field = (hand_index == 0) ? g_cached.arms_left_hand : g_cached.arms_right_hand;
    void* local_hand_field = (hand_index == 0)
        ? g_cached.arms_local_left_hand : g_cached.arms_local_right_hand;

    void* hand = field_value(arms, hand_field);
    void* local_hand = field_value(arms, local_hand_field);
    if (hand) {
        set_position(hand, pos);
        set_rotation(hand, rot);
        set_unit_scale(hand);
    }
    if (local_hand) {
        set_position(local_hand, pos);
        set_rotation(local_hand, rot);
        set_unit_scale(local_hand);
    }

    void* hands = field_value(player, g_cached.pc_hands_field);
    void* grasper = field_value(hands, g_cached.hands_grasper);
    void* grasper_guide = field_value(hands, g_cached.hands_grasper_guide);
    if (grasper) {
        set_position(grasper, pos);
        set_rotation(grasper, rot);
    }
    if (grasper_guide) {
        set_position(grasper_guide, pos);
        set_rotation(grasper_guide, rot);
    }
}

void apply_hands(void* arms_self, const vrc::VrcXRState& state) {
    if (!g_reflect_ready.load()) return;

    const unsigned long long tick = g_tick.load();
    void* cached_arms_ptr = cached_arms();
    if (!cached_arms_ptr || (tick % 120) == 0) {
        find_local_player(arms_self, nullptr);
        cached_arms_ptr = cached_arms();
    }

    void* arms = arms_self ? arms_self : cached_arms_ptr;
    void* player = cached_player();
    if (!arms || !player) return;
    // A PlayerArms hook only drives that player; PlayerCharacter hooks pass null.
    if (arms_self && arms_self != cached_arms_ptr) return;

    void* camera = field_value(player, g_cached.pc_camera_field);
    if (!camera) return;
    if (!g_cached.transform_get_position && !cache_transform_methods(camera)) return;

    // Do not hide vanilla splines unless this frame can actually drive hands.
    hide_arm_splines(arms);

    const Vec3 cam_pos = get_position(camera);
    const Quat cam_rot = quat_normalize(get_rotation(camera));

    const float scale_raw = state.world_scale > 0.0f ? state.world_scale : 1.0f;
    const float scale = scale_raw < 0.1f ? 0.1f : (scale_raw > 10.0f ? 10.0f : scale_raw);

    const Quat head_q = quat_normalize({state.head.rotation[0], state.head.rotation[1],
                                        state.head.rotation[2], state.head.rotation[3]});
    const Quat head_inv = quat_conjugate(head_q);
    const Vec3 head_p{state.head.position[0], state.head.position[1], state.head.position[2]};

    for (int i = 0; i < 2; ++i) {
        // Pimax/runtimes occasionally report a valid aim pose while the grip
        // pose is briefly invalid. Falling back keeps both hands driveable.
        const bool grip_ok = pose_usable(state.grip[i]);
        const auto& grip = grip_ok ? state.grip[i] : state.aim[i];
        if (!pose_usable(grip)) {
            // Tracking dropouts are common for one controller. Keep the last
            // accepted pose instead of letting the game snap the hand back to
            // the flat-screen position.
            if (g_has_last_applied[i]) {
                const Vec3 held_rotated = quat_rotate(cam_rot, g_last_hold_rel_pos[i]);
                const Vec3 held_world{
                    cam_pos.x + held_rotated.x,
                    cam_pos.y + held_rotated.y,
                    cam_pos.z + held_rotated.z,
                };
                const Quat held_rot = quat_normalize(quat_mul(cam_rot, g_last_hold_rel_rot[i]));
                write_hand_pose(arms, player, i, held_world, held_rot);
                const unsigned bit = 1u << i;
                if ((g_hold_log_mask.fetch_or(bit) & bit) == 0) {
                    MOD_LOG("hand%d pose invalid; holding last accepted head-relative pose", i);
                }
            }
            continue;
        }

        const unsigned valid_bit = 1u << i;
        if ((g_hold_log_mask.fetch_and(~valid_bit) & valid_bit) != 0) {
            MOD_LOG("hand%d tracking resumed via %s; resuming live controller pose",
                    i, grip_ok ? "grip" : "aim");
        }

        const Vec3 grip_p{grip.position[0], grip.position[1], grip.position[2]};
        const Quat grip_q = quat_normalize({grip.rotation[0], grip.rotation[1],
                                            grip.rotation[2], grip.rotation[3]});

        // Head-relative pose in OpenXR LOCAL space, then mirror into Unity space.
        const Vec3 rel_xr = quat_rotate(head_inv, vec_sub(grip_p, head_p));
        const Quat rel_rot_xr = quat_mul(head_inv, grip_q);
        const Vec3 rel_unity{rel_xr.x * scale, rel_xr.y * scale, -rel_xr.z * scale};
        const Quat rel_rot_unity = xr_quat_to_unity(rel_rot_xr);

        g_last_hold_rel_pos[i] = rel_unity;
        g_last_hold_rel_rot[i] = rel_rot_unity;

        const Vec3 rotated = quat_rotate(cam_rot, rel_unity);
        const Vec3 world_pos{
            cam_pos.x + rotated.x,
            cam_pos.y + rotated.y,
            cam_pos.z + rotated.z,
        };
        const Quat world_rot = quat_normalize(quat_mul(cam_rot, rel_rot_unity));

        void* hand_field = (i == 0) ? g_cached.arms_left_hand : g_cached.arms_right_hand;
        void* hand = field_value(arms, hand_field);

        const Vec3 before = hand ? get_position(hand) : Vec3{};
        if (i == 0 && (tick % 60) == 0 && g_debug_samples.fetch_add(1) < 20) {
            MOD_LOG("sample tick=%llu grips=%u/%u aims=%u/%u stateFlags=%u "
                    "src=%s grip=(%.2f,%.2f,%.2f) head=(%.2f,%.2f,%.2f) "
                    "cam=(%.2f,%.2f,%.2f) rel=(%.2f,%.2f,%.2f) "
                    "before=(%.2f,%.2f,%.2f) world=(%.2f,%.2f,%.2f)",
                    tick,
                    static_cast<unsigned int>(state.grip[0].flags),
                    static_cast<unsigned int>(state.grip[1].flags),
                    static_cast<unsigned int>(state.aim[0].flags),
                    static_cast<unsigned int>(state.aim[1].flags),
                    static_cast<unsigned int>(state.flags),
                    grip_ok ? "grip" : "aim",
                    grip_p.x, grip_p.y, grip_p.z,
                    head_p.x, head_p.y, head_p.z,
                    cam_pos.x, cam_pos.y, cam_pos.z,
                    rel_unity.x, rel_unity.y, rel_unity.z,
                    before.x, before.y, before.z,
                    world_pos.x, world_pos.y, world_pos.z);
        }
        write_hand_pose(arms, player, i, world_pos, world_rot);

        if (hand && (tick % 60) == 0 && g_after_log_samples.fetch_add(1) < 40) {
            const Vec3 after = get_position(hand);
            const Vec3 drift = g_has_last_applied[i]
                ? vec_sub(g_last_applied[i], after)
                : Vec3{0, 0, 0};
            MOD_LOG("hand%d tick=%llu after=(%.2f,%.2f,%.2f) "
                    "target=(%.2f,%.2f,%.2f) drift=(%.2f,%.2f,%.2f)",
                    i, tick,
                    after.x, after.y, after.z,
                    world_pos.x, world_pos.y, world_pos.z,
                    drift.x, drift.y, drift.z);
        }
        g_last_applied[i] = world_pos;
        g_last_applied_rot[i] = world_rot;
        g_has_last_applied[i] = true;
    }

    if (!g_logged_apply.exchange(true)) {
        MOD_LOG("Applying tracked controller grips to Big Walk hand transforms");
    }
}

// ─── Bridge consumer ─────────────────────────────────────────────────────────

using GetXRStateFn = bool (*)(vrc::VrcXRState*);
using CopyXRStateFn = bool (*)(vrc::VrcXRState*, vrc::u32);
GetXRStateFn g_get_xr_state = nullptr;
CopyXRStateFn g_copy_xr_state = nullptr;

bool load_bridge_api() {
    HMODULE converter = GetModuleHandleW(L"vr_converter.dll");
    if (!converter) return false;
    g_get_xr_state = reinterpret_cast<GetXRStateFn>(GetProcAddress(converter, "VRC_GetXRState"));
    g_copy_xr_state = reinterpret_cast<CopyXRStateFn>(
        GetProcAddress(converter, "VRC_CopyXRState"));
    if (!g_get_xr_state && !g_copy_xr_state) {
        MOD_LOG("vr_converter.dll loaded but no XR state export is available");
        return false;
    }
    MOD_LOG("Bridge export ready (%s)",
            g_copy_xr_state ? "VRC_CopyXRState" : "VRC_GetXRState");
    return true;
}

// ─── LateUpdate hooks ─────────────────────────────────────────────────────────

// Big Walk 31/08/2026 IL2CPP build:
//   PlayerArms::LateUpdate      @ RVA 0x4EF630
//   PlayerCharacter::LateUpdate @ RVA 0x3941F0
// Apply in both places: the arms hook sets transforms before the game uses
// them, and the character hook re-applies after any later IK correction.
constexpr uintptr_t kPlayerArmsLateUpdateRva = 0x4EF630;
constexpr uintptr_t kPlayerCharacterLateUpdateRva = 0x3941F0;
constexpr uintptr_t kPlayerArmsUpdateScreenSpaceRva = 0x4F23C0;
// ScreenSpacize(Transform neutral, Transform camera, Transform target,
//                float amount) is what actually scales/moves the hand meshes
// toward the flat-screen camera. In VR it must not run at all.
constexpr uintptr_t kScreenSpacizeRva = 0x4F2790;

using LateUpdateFn = void(__fastcall*)(void* /*this*/);
using ScreenSpacizeFn = void(__fastcall*)(void* /*neutral*/, void* /*camera*/,
                                          void* /*target*/, float /*amount*/);
LateUpdateFn g_original_arms_late_update = nullptr;
LateUpdateFn g_original_character_late_update = nullptr;
LateUpdateFn g_original_update_screen_space_arms = nullptr;
ScreenSpacizeFn g_original_screen_spacize = nullptr;

bool g_arms_hook_installed = false;
bool g_character_hook_installed = false;
bool g_update_hook_installed = false;
bool g_spacize_hook_installed = false;
bool g_hooks_installed = false;
bool g_owns_minhook = false;

bool live_xr_state(vrc::VrcXRState& state) {
    const bool got_state = g_copy_xr_state
        ? g_copy_xr_state(&state, static_cast<vrc::u32>(sizeof(state)))
        : (g_get_xr_state && g_get_xr_state(&state));
    if (!got_state) return false;
    if (state.magic != vrc::kVrcXRStateMagic || state.version != vrc::kVrcXRStateVersion) return false;
    if (state.size_bytes != sizeof(vrc::VrcXRState)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            MOD_LOG("Bridge XR state size mismatch (%u vs %zu); ignoring this state",
                   state.size_bytes, sizeof(vrc::VrcXRState));
        }
        return false;
    }
    if (state.injection_method != static_cast<vrc::u32>(vrc::InjectionMethod::SixDofBridge)) return false;
    if (!pose_usable(state.head)) return false;

    // A lower sequence means the converter bridge restarted; poses from the
    // previous session must not be held onto as if they were still valid.
    {
        std::lock_guard<std::mutex> lock(g_last_state_mutex);
        if (state.sequence < g_last_sequence) {
            for (int i = 0; i < 2; ++i) {
                g_has_last_applied[i] = false;
                g_last_hold_rel_pos[i] = {};
                g_last_hold_rel_rot[i] = {0, 0, 0, 1};
            }
            g_hold_log_mask.store(0);
            g_debug_samples.store(0);
            g_after_log_samples.store(0);
            g_last_state_valid.store(false);
            g_last_state_ms.store(0);
        }
        g_last_sequence = state.sequence;
        g_last_state = state;
    }
    g_last_state_valid.store(true);
    g_logged_stale.store(false);
    g_last_state_ms.store(static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));

    const unsigned long long tick = g_tick.load();
    if (!g_logged_state.exchange(true) || (tick % 300) == 0) {
        MOD_LOG("XR state live: seq=%u head=%d grips=%d/%d aims=%d/%d stateFlags=%u world_scale=%.2f",
                state.sequence, pose_usable(state.head) ? 1 : 0,
                pose_usable(state.grip[0]) ? 1 : 0,
                pose_usable(state.grip[1]) ? 1 : 0,
                pose_usable(state.aim[0]) ? 1 : 0,
                pose_usable(state.aim[1]) ? 1 : 0,
                static_cast<unsigned int>(state.flags), state.world_scale);
    }
    return true;
}

void __fastcall hooked_player_arms_late_update(void* self) {
    if (g_original_arms_late_update) g_original_arms_late_update(self);

    const unsigned long long tick = ++g_tick;
    (void)state_fresh();
    if (!g_reflect_ready.load()) {
        if ((tick % 120) == 0) prepare_reflection();
        return;
    }

    vrc::VrcXRState state{};
    if (!live_xr_state(state)) return;
    apply_hands(self, state);
}

void __fastcall hooked_player_character_late_update(void* self) {
    if (g_original_character_late_update) g_original_character_late_update(self);

    ++g_tick;
    (void)state_fresh();
    if (!g_reflect_ready.load()) return;
    void* current_player = cached_player();
    if (!current_player || self != current_player) {
        find_local_player(nullptr, self);
        current_player = cached_player();
    }
    if (!current_player || self != current_player) return;

    vrc::VrcXRState state{};
    if (!live_xr_state(state)) return;
    apply_hands(nullptr, state);
}

// UpdateScreenSpaceArms repositions the rendered hands relative to the camera
// after LateUpdate starts. Reapply the last tracked poses after it runs so the
// screen-space correction cannot pull the hands back to the flat-game pose.
void __fastcall hooked_update_screen_space_arms(void* self) {
    if (g_original_update_screen_space_arms) g_original_update_screen_space_arms(self);
    if (!g_reflect_ready.load() || !state_fresh()) return;
    void* current_arms = cached_arms();
    if (!self || (current_arms && self != current_arms)) return;

    vrc::VrcXRState state{};
    {
        std::lock_guard<std::mutex> lock(g_last_state_mutex);
        state = g_last_state;
    }
    apply_hands(self, state);
}

// Suppress the actual stretch/screen-space transform instead of trying to undo
// it after the fact. Keeping the original as a fallback preserves flat-screen
// behavior if the bridge stops publishing tracked state.
void __fastcall hooked_screen_spacize(void* neutral, void* camera,
                                      void* target, float amount) {
    vrc::VrcXRState state{};
    {
        std::lock_guard<std::mutex> lock(g_last_state_mutex);
        state = g_last_state;
    }
    if (state_fresh() && pose_usable(state.head)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            MOD_LOG("ScreenSpacize suppressed; hands remain at tracked controller poses");
        }
        return;
    }
    if (g_original_screen_spacize) {
        g_original_screen_spacize(neutral, camera, target, amount);
    }
}

bool install_hook_raw(const char* name, uintptr_t rva, void* detour, void** original) {
    uintptr_t target = reinterpret_cast<uintptr_t>(g_il2cpp.game_assembly) + rva;
    MOD_LOG("Hooking %s at %p (RVA 0x%llx)", name, reinterpret_cast<void*>(target),
            static_cast<unsigned long long>(rva));

    if (original) *original = nullptr;
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (status != MH_OK) {
        MOD_LOG("MH_CreateHook failed for %s: %d", name, static_cast<int>(status));
        if (original) *original = nullptr;
        return false;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        MOD_LOG("MH_EnableHook failed for %s: %d", name, static_cast<int>(status));
        // MH_CreateHook succeeded, so the hook is still registered (disabled).
        // Remove it here or later retries fail with MH_ERROR_ALREADY_CREATED.
        (void)MH_RemoveHook(reinterpret_cast<void*>(target));
        if (original) *original = nullptr;
        return false;
    }
    MOD_LOG("%s hook installed", name);
    return true;
}

bool install_hook(const char* name, uintptr_t rva, LateUpdateFn detour,
                  LateUpdateFn* original) {
    return install_hook_raw(name, rva, reinterpret_cast<void*>(detour),
                            reinterpret_cast<void**>(original));
}

void remove_hook(const char* name, uintptr_t rva) {
    void* target = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(g_il2cpp.game_assembly) + rva);
    (void)MH_DisableHook(target);
    if (MH_RemoveHook(target) != MH_OK) {
        MOD_LOG("MH_RemoveHook failed for %s", name);
    }
}

bool install_hooks() {
    MH_STATUS status = MH_Initialize();
    const bool initialized_here = (status == MH_OK);
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        MOD_LOG("MH_Initialize failed: %d", static_cast<int>(status));
        return false;
    }

    bool arms_hooked = install_hook("PlayerArms::LateUpdate", kPlayerArmsLateUpdateRva,
                                    &hooked_player_arms_late_update,
                                    &g_original_arms_late_update);
    bool character_hooked = false;
    if (arms_hooked) {
        character_hooked = install_hook("PlayerCharacter::LateUpdate",
                                        kPlayerCharacterLateUpdateRva,
                                        &hooked_player_character_late_update,
                                        &g_original_character_late_update);
    }

    // A partial install would make retries fail with MH_ERROR_ALREADY_CREATED,
    // so roll the core hooks back and let mod_thread try again next tick.
    if (!arms_hooked || !character_hooked) {
        if (character_hooked) remove_hook("PlayerCharacter::LateUpdate",
                                          kPlayerCharacterLateUpdateRva);
        if (arms_hooked) remove_hook("PlayerArms::LateUpdate", kPlayerArmsLateUpdateRva);
        g_original_character_late_update = nullptr;
        g_original_arms_late_update = nullptr;
        if (initialized_here) (void)MH_Uninitialize();
        return false;
    }

    g_arms_hook_installed = true;
    g_character_hook_installed = true;
    g_owns_minhook = initialized_here;

    // This third hook is hardening, not required for basic hand tracking.
    g_update_hook_installed = install_hook(
        "PlayerArms::UpdateScreenSpaceArms", kPlayerArmsUpdateScreenSpaceRva,
        &hooked_update_screen_space_arms, &g_original_update_screen_space_arms);
    if (!g_update_hook_installed) {
        MOD_LOG("UpdateScreenSpaceArms hook unavailable; LateUpdate reapply still active");
    }
    // Root-cause fix for stretched hands: stop the screen-space rescale itself.
    g_spacize_hook_installed = install_hook_raw(
        "PlayerArms.ScreenSpacize", kScreenSpacizeRva,
        reinterpret_cast<void*>(&hooked_screen_spacize),
        reinterpret_cast<void**>(&g_original_screen_spacize));
    if (!g_spacize_hook_installed) {
        MOD_LOG("ScreenSpacize hook unavailable; localScale reset still applied");
    }
    g_hooks_installed = true;
    return true;
}

void uninstall_hooks() {
    if (!g_hooks_installed) return;
    g_hooks_installed = false;

    if (g_spacize_hook_installed) {
        remove_hook("PlayerArms.ScreenSpacize", kScreenSpacizeRva);
        g_spacize_hook_installed = false;
    }
    if (g_update_hook_installed) {
        remove_hook("PlayerArms::UpdateScreenSpaceArms", kPlayerArmsUpdateScreenSpaceRva);
        g_update_hook_installed = false;
    }
    if (g_character_hook_installed) {
        remove_hook("PlayerCharacter::LateUpdate", kPlayerCharacterLateUpdateRva);
        g_character_hook_installed = false;
    }
    if (g_arms_hook_installed) {
        remove_hook("PlayerArms::LateUpdate", kPlayerArmsLateUpdateRva);
        g_arms_hook_installed = false;
    }
    if (g_owns_minhook) {
        (void)MH_Uninitialize();
        g_owns_minhook = false;
    }
}

// ─── Module bootstrap ────────────────────────────────────────────────────────

DWORD WINAPI mod_thread(LPVOID) {
    MOD_LOG("Big Walk hands mod starting");

    // Wait for GameAssembly.dll and vr_converter.dll. Either DLL may be injected
    // first, and the converter publishes the bridge on its delayed-init thread.
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (load_il2cpp_api() && g_il2cpp.thread_attach(g_il2cpp.domain)) {
            if (load_bridge_api() && prepare_reflection() && install_hooks()) {
                MOD_LOG("Init complete: waiting for tracked XR frames");
                return 0;
            }
        }
        Sleep(100);
    }

    MOD_LOG("Init timed out; Big Walk hands mod disabled");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        // Only clean up on FreeLibrary. Process teardown unmaps everything, and
        // calling MinHook/IL2CPP under the loader lock there can deadlock.
        if (g_il2cpp.thread_attach && g_il2cpp.domain) {
            (void)g_il2cpp.thread_attach(g_il2cpp.domain);
        }
        uninstall_hooks();
        release_cached_handles();
        release_hidden_spline_handle();
    }
    return TRUE;
}
