// Outlast Trials game-side mod scaffold for VR Game Converter's 6DoF Bridge.
//
// STATUS: exact-build-gated interaction engine with an opt-in runtime phase
// (see reports/Outlast Trials VR interaction plan.md). The game is UE4 4.27.1
// (OPP fork), always online, and protected by Easy Anti-Cheat since October
// 2025, so native hooks are derived from this exact build and must only be
// validated in solo/private sessions.
//
// This module implements:
//   * bridge loading, ABI/sequence/pose validation, and a 10 ms intent pump,
//   * OpenXR metres → UE head-relative centimetres and haptic requests,
//   * exact EXE hash + reflection map validation before any hook is attempted,
//   * a reversible ProcessEvent vtable-shadow hook used as the game-thread
//     entry point (no code-byte patching and no anti-cheat interaction),
//   * live UFunction reflection and original-ProcessEvent dispatch, gated by
//     VRC_OUTLAST_ENABLE_RUNTIME_HOOK=1, VRC_OUTLAST_SOLO=1,
//     VRC_OUTLAST_ENABLE_DISPATCH=1, and VRC_OUTLAST_TARGET_HINT=<object>.
//
// With no opt-ins the module remains dry-run. tools/verify_outlast_pdb.py checks
// a supplied PDB's GUID/age against the PE. tools/ingest_outlast_symbols.py
// turns build-fresh reflection into outlast_verb_map.validated.json with exact
// UFunction flags, parameter layouts, globals, and ProcessEvent confirmation.
// No address from another game build is baked into this module.

#include <windows.h>
#include <wincrypt.h>

#include "input/xr_shared_state.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vrc::f32;
using vrc::f64;
using vrc::i64;
using vrc::u8;
using vrc::u16;
using vrc::u32;
using vrc::u64;

std::string temp_log_path() {
    char temp[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableA("TEMP", temp, MAX_PATH);
    std::string dir = (len > 0 && len < MAX_PATH) ? temp : ".";
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir + "\\vrc_outlast_mod.log";
}

void rotate_previous_log() {
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
    rotate_previous_log();
    char msg[1024]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    OutputDebugStringA("[vrc-outlast] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    if (FILE* f = std::fopen(temp_log_path().c_str(), "a")) {
        std::fprintf(f, "%s\n", msg);
        std::fclose(f);
    }
}

#define MOD_LOG(...) logf_line(__VA_ARGS__)

using GetXRStateFn = bool (*)(vrc::VrcXRState*);
using CopyXRStateFn = bool (*)(vrc::VrcXRState*, vrc::u32);
using GetBridgeApiVersionFn = vrc::u32 (*)();
using RequestHapticFn = bool (*)(bool, vrc::f32, vrc::f32);

GetXRStateFn g_get_xr_state = nullptr;
CopyXRStateFn g_copy_xr_state = nullptr;
GetBridgeApiVersionFn g_get_api_version = nullptr;
RequestHapticFn g_request_haptic = nullptr;
std::atomic<bool> g_stop_requested{false};
HANDLE g_thread_done = nullptr;

bool read_bridge_state(vrc::VrcXRState& state) {
    if (g_copy_xr_state) {
        return g_copy_xr_state(&state, static_cast<vrc::u32>(sizeof(state)));
    }
    return g_get_xr_state && g_get_xr_state(&state);
}

bool pose_usable(const vrc::VrcPoseState& pose) {
    constexpr vrc::u32 kNeed =
        vrc::VrcPoseFlag_PositionValid | vrc::VrcPoseFlag_OrientationValid;
    return (pose.flags & kNeed) == kNeed;
}

bool load_bridge_api() {
    HMODULE converter = GetModuleHandleW(L"vr_converter.dll");
    if (!converter) return false;
    g_get_xr_state =
        reinterpret_cast<GetXRStateFn>(GetProcAddress(converter, "VRC_GetXRState"));
    g_copy_xr_state =
        reinterpret_cast<CopyXRStateFn>(GetProcAddress(converter, "VRC_CopyXRState"));
    g_get_api_version = reinterpret_cast<GetBridgeApiVersionFn>(
        GetProcAddress(converter, "VRC_GetBridgeApiVersion"));
    g_request_haptic = reinterpret_cast<RequestHapticFn>(
        GetProcAddress(converter, "VRC_RequestHaptic"));
    if ((!g_get_xr_state && !g_copy_xr_state) || !g_get_api_version) {
        MOD_LOG("vr_converter.dll bridge exports missing; mod disabled");
        return false;
    }
    MOD_LOG("Bridge export ready: api=%u state=%s haptics=%s",
            g_get_api_version(), g_copy_xr_state ? "CopyXRState" : "GetXRState",
            g_request_haptic ? "yes" : "no");
    return true;
}

std::string module_dir() {
    HMODULE module = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&load_bridge_api), &module);
    char path[MAX_PATH]{};
    DWORD len = module ? GetModuleFileNameA(module, path, MAX_PATH) : 0;
    if (len == 0 || len >= MAX_PATH) return ".";
    std::string full(path, len);
    const std::size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? "." : full.substr(0, slash);
}

// ─── Exact-build verification and verb-map model ────────────────────────────

std::string current_exe_path() {
    char path[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::string(path, len);
}

bool sha256_file(const std::string& path, std::string& hex) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        MOD_LOG("sha256_file: CryptAcquireContext failed err=%lu", GetLastError());
        return false;
    }
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        MOD_LOG("sha256_file: CryptCreateHash failed err=%lu", GetLastError());
        CryptReleaseContext(provider, 0);
        return false;
    }

    bool ok = true;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        MOD_LOG("sha256_file: open failed path=%s err=%lu",
                path.empty() ? "<unknown>" : path.c_str(), GetLastError());
        ok = false;
    }
    char buffer[64 * 1024]{};
    while (ok && file) {
        file.read(buffer, sizeof(buffer));
        const std::streamsize read = file.gcount();
        if (read > 0 &&
            !CryptHashData(hash, reinterpret_cast<const BYTE*>(buffer),
                           static_cast<DWORD>(read), 0)) {
            ok = false;
        }
    }

    DWORD digest_size = 0;
    if (!ok) {
        // keep the existing early-out below
    } else if (!CryptGetHashParam(hash, HP_HASHVAL, nullptr, &digest_size, 0)) {
        MOD_LOG("sha256_file: size query failed err=%lu", GetLastError());
        ok = false;
    }
    if (ok) {
        std::vector<BYTE> digest(digest_size);
        if (CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digest_size, 0)) {
            static constexpr char kDigits[] = "0123456789abcdef";
            hex.clear();
            hex.reserve(digest.size() * 2);
            for (BYTE byte : digest) {
                hex.push_back(kDigits[(byte >> 4) & 0xF]);
                hex.push_back(kDigits[byte & 0xF]);
            }
        } else {
            ok = false;
        }
    } else {
        ok = false;
    }

    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return ok;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

u64 parse_hex_u64(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return value.get<u64>();
    if (value.is_number_integer()) {
        const i64 signed_value = value.get<i64>();
        return signed_value < 0 ? 0 : static_cast<u64>(signed_value);
    }
    if (!value.is_string()) return 0;
    const std::string text = value.get<std::string>();
    if (text.empty()) return 0;
    char* end = nullptr;
    const u64 result = std::strtoull(text.c_str(), &end, 0);
    return end && end != text.c_str() ? result : 0;
}

int parse_i32(const nlohmann::json& value) {
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<int>();
    }
    if (value.is_string()) {
        return std::atoi(value.get<std::string>().c_str());
    }
    return 0;
}

struct ParamField {
    std::string name;
    std::string type;
    int offset = -1;
    int size = -1;
};

struct VerbFunction {
    std::string role;
    std::string tier;
    std::string object_class;
    std::string reflected_class;
    std::string function;
    u64 name_rva = 0;
    u64 emit_rva = 0;
    bool validated = false;
    bool layout_known = false;
    bool flags_known = false;
    u64 flags = 0;
    u64 fname_index = 0;
    u64 function_rva = 0;
    u64 params_size = 0;
    std::vector<ParamField> params;
};

enum class IntentKind : int { InteractStart, InteractEnd, PrimaryUse, ExitCustom };

struct InteractionIntent {
    IntentKind kind = IntentKind::InteractStart;
    int hand = 0;
    u64 target_hint = 0;
    u64 source_sequence = 0;
};

// Loads either the shipped static map or a map produced by
// tools/ingest_outlast_symbols.py. The loader itself is safe; it never patches
// code or calls Unreal. ready() is true only for an exact-build, reflection-
// validated map. Runtime hook installation remains a separate gated phase.
class GameAdapter {
public:
    bool load(const std::string& path, std::string& error) {
        std::ifstream file(path);
        if (!file) {
            error = "cannot open " + path;
            return false;
        }
        const nlohmann::json doc = nlohmann::json::parse(file, nullptr, false);
        if (doc.is_discarded()) {
            error = "invalid JSON in " + path;
            return false;
        }

        status_ = doc.value("status", "static-unvalidated");
        if (status_ != "static-unvalidated" && status_ != "symbol-validated" &&
            status_ != "reflection-validated") {
            error = "unsupported verb-map status: " + status_;
            return false;
        }
        if (!doc.contains("candidates") || !doc["candidates"].is_array()) {
            error = "verb map has no candidates array";
            return false;
        }

        functions_.clear();
        tier1_count_ = 0;
        for (const auto& entry : doc["candidates"]) {
            VerbFunction fn;
            fn.role = entry.value("role", "");
            fn.tier = entry.value("tier", "");
            fn.object_class = entry.value("class", "");
            fn.function = entry.value("function", "");
            if (fn.role.empty() || fn.object_class.empty() || fn.function.empty()) continue;
            fn.name_rva = parse_hex_u64(entry.value("name_rva", nlohmann::json{}));
            fn.emit_rva = parse_hex_u64(entry.value("emit_rva", nlohmann::json{}));
            if (fn.tier == "tier1") ++tier1_count_;

            if (entry.contains("reflection") && entry["reflection"].is_object()) {
                const auto& reflected = entry["reflection"];
                fn.reflected_class = reflected.value("reflected_class", "");
                if (fn.reflected_class.empty()) fn.reflected_class = fn.object_class;
                fn.validated = reflected.value("validated", false);
                fn.layout_known = reflected.value("layout_known", false);
                fn.flags_known = reflected.value("flags_known", false);
                fn.flags = parse_hex_u64(reflected.value("flags", nlohmann::json{}));
                fn.fname_index = parse_hex_u64(
                    reflected.value("fname_index", nlohmann::json{}));
                fn.function_rva = parse_hex_u64(
                    reflected.value("function_rva", nlohmann::json{}));
                fn.params_size = reflected.value("params_size", 0ULL);
                if (reflected.contains("parameters") &&
                    reflected["parameters"].is_array()) {
                    for (const auto& param : reflected["parameters"]) {
                        ParamField field;
                        field.name = param.value("name", "");
                        field.type = param.value("type", "unknown");
                        field.offset = parse_i32(
                            param.value("offset", nlohmann::json{}));
                        field.size =
                            parse_i32(param.value("size", nlohmann::json{}));
                        fn.params.push_back(std::move(field));
                    }
                }
            }
            functions_.push_back(std::move(fn));
        }

        build_sha256_ = doc.value("build", nlohmann::json::object())
                            .value("sha256", std::string{});
        image_base_ = parse_hex_u64(
            doc.value("build", nlohmann::json::object()).value("image_base", nlohmann::json{}));
        process_event_rva_ = parse_hex_u64(
            doc.value("process_event", nlohmann::json::object())
                .value("rva", nlohmann::json{}));
        process_event_index_ = parse_hex_u64(
            doc.value("process_event", nlohmann::json::object())
                .value("vtable_index", nlohmann::json{}));
        process_event_confirmed_ =
            doc.value("validation", nlohmann::json::object())
                .value("process_event_confirmed", false);

        if (doc.contains("validation") && doc["validation"].contains("globals") &&
            doc["validation"]["globals"].is_object()) {
            globals_.clear();
            for (auto it = doc["validation"]["globals"].begin();
                 it != doc["validation"]["globals"].end(); ++it) {
                globals_[it.key()] = parse_hex_u64(it.value());
            }
        }
        if (doc.contains("validation") && doc["validation"].contains("offsets") &&
            doc["validation"]["offsets"].is_object()) {
            offsets_.clear();
            for (auto it = doc["validation"]["offsets"].begin();
                 it != doc["validation"]["offsets"].end(); ++it) {
                offsets_[it.key()] = parse_hex_u64(it.value());
            }
        }

        const std::string exe_path = current_exe_path();
        std::string actual_sha;
        const bool hash_computed = sha256_file(exe_path, actual_sha);
        build_hash_matches_ = hash_computed && !build_sha256_.empty() &&
                              lower_copy(actual_sha) == lower_copy(build_sha256_);
        MOD_LOG("Build check: exe=%s actual=%s expected=%s computed=%s match=%s",
                exe_path.empty() ? "<unknown>" : exe_path.c_str(),
                actual_sha.empty() ? "<none>" : actual_sha.c_str(),
                build_sha256_.empty() ? "<none>" : build_sha256_.c_str(),
                hash_computed ? "yes" : "no",
                build_hash_matches_ ? "yes" : "no");
        map_dispatch_allowed_ = doc.value("dispatch_allowed", false);

        static constexpr const char* kRequiredRoles[] = {
            "generic_interact_start", "generic_interact_end",
            "item_pickup", "item_drop", "door_operation_rpc"};
        bool all_required = true;
        for (const char* role : kRequiredRoles) {
            const VerbFunction* fn = find_role(role);
            if (!fn || !fn->validated || !fn->layout_known || !fn->flags_known) {
                all_required = false;
                break;
            }
        }

        ready_ = status_ == "reflection-validated" && map_dispatch_allowed_ &&
                 build_hash_matches_ && process_event_confirmed_ &&
                 process_event_rva_ != 0 && image_base_ != 0 && all_required;
        error.clear();
        return true;
    }

    const std::vector<VerbFunction>& functions() const { return functions_; }
    const std::string& status() const { return status_; }
    const std::string& build_sha256() const { return build_sha256_; }
    std::size_t tier1_count() const { return tier1_count_; }
    bool ready() const { return ready_; }
    bool build_hash_matches() const { return build_hash_matches_; }
    bool map_dispatch_allowed() const { return map_dispatch_allowed_; }
    u64 image_base() const { return image_base_; }
    u64 process_event_rva() const { return process_event_rva_; }
    u64 process_event_index() const { return process_event_index_; }
    const std::map<std::string, u64>& globals() const { return globals_; }
    const std::map<std::string, u64>& offsets() const { return offsets_; }

    const VerbFunction* find_role(const std::string& role) const {
        for (const VerbFunction& fn : functions_) {
            if (fn.role == role) return &fn;
        }
        return nullptr;
    }

    // Defined after the runtime hook implementation. With no hook installed
    // this still returns false, so offline behaviour is unchanged.
    bool invoke(int kind, int hand, u64 target_hint) const;

private:
    std::vector<VerbFunction> functions_;
    std::map<std::string, u64> globals_;
    std::map<std::string, u64> offsets_;
    std::string status_;
    std::string build_sha256_;
    u64 image_base_ = 0;
    std::size_t tier1_count_ = 0;
    u64 process_event_rva_ = 0;
    u64 process_event_index_ = 0;
    bool process_event_confirmed_ = false;
    bool build_hash_matches_ = false;
    bool map_dispatch_allowed_ = false;
    bool ready_ = false;
};

// ─── Runtime hook phase (reversible vtable shadow, no code patching) ────────
//
// This phase is deliberately opt-in. It never writes executable bytes and
// never touches anti-cheat. It only:
//   1. rechecks the exact-build globals/offsets from the validated map,
//   2. copies a live UObject vtable into owned memory and swaps the object's
//      vptr to that shadow so ProcessEvent runs our detour on the game thread,
//   3. resolves UFunctions by live reflection and calls the original
//      ProcessEvent only when solo/dispatch opt-ins and a target hint are set.
// The original vptr is restored on shutdown, and the shadow table is freed.

struct RuntimeConfig {
    bool hook_enabled = false;
    bool dispatch_enabled = false;
    bool solo_confirmed = false;
    std::string target_hint;
    std::string hook_hint;

    static std::string env_string(const char* name) {
        char buffer[4096]{};
        const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
        return length > 0 && length < sizeof(buffer)
                   ? std::string(buffer, length)
                   : std::string{};
    }
    static bool env_flag(const char* name) {
        const std::string value = lower_copy(env_string(name));
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }
    static RuntimeConfig read() {
        RuntimeConfig config;
        config.hook_enabled = env_flag("VRC_OUTLAST_ENABLE_RUNTIME_HOOK");
        config.dispatch_enabled = env_flag("VRC_OUTLAST_ENABLE_DISPATCH");
        config.solo_confirmed = env_flag("VRC_OUTLAST_SOLO");
        config.target_hint = env_string("VRC_OUTLAST_TARGET_HINT");
        config.hook_hint = env_string("VRC_OUTLAST_HOOK_HINT");
        return config;
    }
};

bool readable_memory(const void* address, std::size_t length) {
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
    if (!start || length == 0 || start > UINTPTR_MAX - length) return false;
    std::uintptr_t cursor = start;
    const std::uintptr_t end = start + length;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &info, sizeof(info)) <
            sizeof(info)) {
            return false;
        }
        const DWORD protect = info.Protect;
        const bool usable = info.State == MEM_COMMIT &&
                            !(protect & PAGE_NOACCESS) && !(protect & PAGE_GUARD);
        if (!usable) return false;
        const std::uintptr_t region_end =
            reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (region_end <= cursor) return false;
        cursor = region_end;
    }
    return true;
}

bool executable_memory(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) <
        sizeof(info)) {
        return false;
    }
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & executable) != 0;
}

std::uintptr_t load_ptr(std::uintptr_t address) {
    if (!readable_memory(reinterpret_cast<void*>(address), sizeof(std::uintptr_t))) {
        return 0;
    }
    return *reinterpret_cast<const std::uintptr_t*>(address);
}

u32 load_u32(std::uintptr_t address) {
    if (!readable_memory(reinterpret_cast<void*>(address), sizeof(u32))) return 0;
    return *reinterpret_cast<const u32*>(address);
}

u16 load_u16(std::uintptr_t address) {
    if (!readable_memory(reinterpret_cast<void*>(address), sizeof(u16))) return 0;
    return *reinterpret_cast<const u16*>(address);
}

u8 load_u8(std::uintptr_t address) {
    if (!readable_memory(reinterpret_cast<void*>(address), sizeof(u8))) return 0;
    return *reinterpret_cast<const u8*>(address);
}

std::string normalize_class_name(std::string name) {
    const std::string suffix = "_C";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.resize(name.size() - suffix.size());
    }
    if (name.size() > 1 &&
        (name[0] == 'U' || name[0] == 'A' || name[0] == 'I') &&
        std::isupper(static_cast<unsigned char>(name[1]))) {
        name.erase(name.begin());
    }
    return name;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lower_copy(haystack).find(lower_copy(needle)) != std::string::npos;
}

std::string class_needle_from_type(const std::string& type) {
    std::string cleaned = type;
    const std::size_t star = cleaned.find('*');
    if (star != std::string::npos) cleaned.resize(star);
    std::string without_spaces;
    for (const char c : cleaned) {
        if (!std::isspace(static_cast<unsigned char>(c))) without_spaces.push_back(c);
    }
    for (const char* prefix : {"const", "class", "struct"}) {
        const std::string text(prefix);
        if (without_spaces.compare(0, text.size(), text) == 0) {
            without_spaces.erase(0, text.size());
        }
    }
    return normalize_class_name(without_spaces);
}

using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
void __fastcall hooked_process_event(void* self, void* function, void* params);

struct VTableHook {
    std::uintptr_t object = 0;
    void* original_vptr = nullptr;
    void* shadow_vtable = nullptr;
    std::string label;
};

struct ResolvedFunction {
    std::uintptr_t ufunction = 0;
    bool valid = false;
};

class LiveUnreal {
public:
    bool install(GameAdapter& adapter, const RuntimeConfig& config) {
        adapter_ = &adapter;
        config_ = config;

        if (!adapter.ready()) {
            MOD_LOG("Runtime hook refused: verb map is not reflection-validated");
            return false;
        }
        const std::uintptr_t module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!module || module < 0x10000 || adapter.image_base() == 0) {
            MOD_LOG("Runtime hook refused: cannot derive loaded module base");
            return false;
        }

        process_event_ = module + static_cast<std::uintptr_t>(adapter.process_event_rva());
        if (!executable_memory(process_event_)) {
            MOD_LOG("Runtime hook refused: ProcessEvent RVA is not executable in this process");
            return false;
        }
        original_process_event_ = reinterpret_cast<ProcessEventFn>(process_event_);

        const auto rebase = [&](u64 va) -> std::uintptr_t {
            return module + static_cast<std::uintptr_t>(va - adapter.image_base());
        };
        const auto global = [&](const char* name) -> std::uintptr_t {
            const auto it = adapter.globals().find(name);
            return it == adapter.globals().end() || it->second < adapter.image_base()
                       ? 0
                       : rebase(it->second);
        };
        const auto offset = [&](const char* name, u64 fallback) -> std::uintptr_t {
            const auto it = adapter.offsets().find(name);
            return static_cast<std::uintptr_t>(it == adapter.offsets().end() ? fallback
                                                                               : it->second);
        };

        uobject_array_ = global("GUObjectArray");
        names_global_ = global("GNames");
        world_global_ = global("GWorld");
        name_offset_ = offset("UObject::NamePrivate", 0x18);
        class_offset_ = offset("UObject::Class", 0x10);
        vtable_offset_ = offset("UObject::VTable", 0x0);
        field_next_offset_ = offset("UField::Next", 0x28);
        struct_children_offset_ = offset("UStruct::Children", 0x48);
        function_flags_offset_ = offset("UFunction::FunctionFlags", 0xB0);
        function_num_parms_offset_ = offset("UFunction::NumParms", 0xB4);
        function_parms_size_offset_ = offset("UFunction::ParmsSize", 0xB6);
        objects_offset_ = offset("TUObjectArray::Objects", 0x0);
        num_elements_offset_ = offset("TUObjectArray::NumElements", 0x14);
        num_chunks_offset_ = offset("TUObjectArray::NumChunks", 0x1C);
        object_item_offset_ = offset("FUObjectItem::Object", 0x0);
        object_item_size_ = offset("FUObjectItem::Size", 0x18);
        name_blocks_offset_ = offset("FNamePool::Blocks", 0x10);
        name_string_offset_ = offset("FNameEntry::Name", 0x2);
        process_event_index_ = static_cast<std::size_t>(adapter.process_event_index());

        const std::uintptr_t name_pool = load_ptr(names_global_);
        if (!readable_memory(reinterpret_cast<void*>(uobject_array_), 0x20) ||
            !readable_memory(reinterpret_cast<void*>(names_global_), sizeof(std::uintptr_t)) ||
            !readable_memory(reinterpret_cast<void*>(world_global_), sizeof(std::uintptr_t)) ||
            !name_pool || !load_ptr(world_global_)) {
            MOD_LOG("Runtime hook refused: validated globals are not readable in this process");
            return false;
        }
        if (name_pool > UINTPTR_MAX - name_blocks_offset_ ||
            !readable_memory(reinterpret_cast<void*>(name_pool + name_blocks_offset_),
                             0x2000U * sizeof(std::uintptr_t))) {
            MOD_LOG("Runtime hook refused: FNamePool block table is not readable");
            return false;
        }
        name_blocks_base_ = name_pool + name_blocks_offset_;
        if (!object_item_size_ || process_event_index_ == 0 ||
            process_event_index_ > 0x1000) {
            MOD_LOG("Runtime hook refused: invalid reflection offsets or vtable index");
            return false;
        }

        const u32 object_count = load_u32(uobject_array_ + num_elements_offset_);
        const u32 chunk_count = load_u32(uobject_array_ + num_chunks_offset_);
        if (object_count == 0 || chunk_count == 0 || chunk_count > 0x400) {
            MOD_LOG("Runtime hook refused: GUObjectArray looks uninitialised (objects=%u chunks=%u)",
                    object_count, chunk_count);
            return false;
        }
        const std::uintptr_t chunks = load_ptr(uobject_array_ + objects_offset_);
        if (!readable_memory(reinterpret_cast<void*>(chunks),
                             static_cast<std::size_t>(chunk_count) * sizeof(std::uintptr_t))) {
            MOD_LOG("Runtime hook refused: GUObjectArray chunk table is not readable");
            return false;
        }

        struct Candidate {
            std::uintptr_t object;
            std::string label;
        };
        std::vector<Candidate> candidates;
        const std::uintptr_t world = load_ptr(world_global_);
        if (valid_object(world) && object_exists(world)) {
            candidates.push_back({world, "GWorld/UWorld"});
        }
        enumerate_objects([&](std::uintptr_t object, const std::string& class_name,
                              const std::string& object_name) {
            if (candidates.size() >= 16) return;
            const std::string normalized = normalize_class_name(class_name);
            const bool wanted = contains_ci(normalized, "PlayerController") ||
                                contains_ci(normalized, "RBPlayer") ||
                                contains_ci(normalized, "GameInstance") ||
                                contains_ci(normalized, "GameViewportClient") ||
                                (!config_.hook_hint.empty() &&
                                 contains_ci(object_name, config_.hook_hint));
            if (wanted) {
                candidates.push_back({object, normalized + "/" + object_name});
            }
        });

        std::size_t hooked = 0;
        for (const Candidate& candidate : candidates) {
            const std::uintptr_t vptr = load_ptr(candidate.object + vtable_offset_);
            if (!vptr || !readable_memory(reinterpret_cast<void*>(vptr),
                                          (process_event_index_ + 2) * sizeof(std::uintptr_t))) {
                continue;
            }
            const std::uintptr_t slot = load_ptr(vptr + process_event_index_ * sizeof(std::uintptr_t));
            if (slot != process_event_) {
                MOD_LOG("Hook candidate %s skipped: slot 0x%llX is 0x%llX, expected 0x%llX",
                        candidate.label.c_str(),
                        static_cast<unsigned long long>(process_event_index_),
                        static_cast<unsigned long long>(slot),
                        static_cast<unsigned long long>(process_event_));
                continue;
            }
            if (install_shadow(candidate.object, candidate.label)) ++hooked;
        }

        if (hooked == 0) {
            MOD_LOG("Runtime hook not installed: no live UObject vtable matched slot 0x%llX",
                    static_cast<unsigned long long>(process_event_index_));
            return false;
        }
        installed_.store(true);
        MOD_LOG("Runtime hook installed on %zu UObject(s); game-thread pump %s, dispatch %s",
                hooked, config_.solo_confirmed ? "armed" : "inactive",
                (config_.dispatch_enabled && config_.solo_confirmed) ? "armed" : "dry-run");
        return true;
    }

    void shutdown() {
        installed_.store(false);
        for (auto it = hooks_.rbegin(); it != hooks_.rend(); ++it) {
            void* current = load_ptr(it->object + vtable_offset_)
                                ? reinterpret_cast<void*>(load_ptr(it->object + vtable_offset_))
                                : nullptr;
            if (current == it->shadow_vtable && it->original_vptr) {
                InterlockedExchangePointer(
                    reinterpret_cast<PVOID volatile*>(it->object + vtable_offset_),
                    it->original_vptr);
                MOD_LOG("Runtime hook restored vptr on %s", it->label.c_str());
            }
            if (it->shadow_vtable) VirtualFree(it->shadow_vtable, 0, MEM_RELEASE);
        }
        hooks_.clear();
    }

    bool installed() const { return installed_.load(); }
    ProcessEventFn original_process_event() const { return original_process_event_; }

    bool enqueue(const InteractionIntent& intent) {
        if (!installed_.load()) return false;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= 256) queue_.pop_front();
        queue_.push_back(intent);
        return true;
    }

    void on_game_thread_tick() {
        if (!installed_.load()) return;
        const DWORD thread = GetCurrentThreadId();
        if (game_thread_id_ == 0) {
            game_thread_id_ = thread;
            MOD_LOG("Game thread identified by ProcessEvent hook: tid=%lu",
                    static_cast<unsigned long>(thread));
        }
        if (game_thread_id_ != thread) return;

        bool expected = false;
        if (!pumping_.compare_exchange_strong(expected, true)) return;
        struct PumpReset {
            std::atomic<bool>& flag;
            ~PumpReset() { flag.store(false); }
        } reset{pumping_};

        std::deque<InteractionIntent> batch;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            batch.swap(queue_);
        }
        while (!batch.empty()) {
            const InteractionIntent intent = batch.front();
            batch.pop_front();
            if (!config_.dispatch_enabled || !config_.solo_confirmed) {
                ++hook_only_intents_;
                if (hook_only_intents_ == 1 || (hook_only_intents_ % 500) == 0) {
                    MOD_LOG("Game-thread intent kind=%d hand=%d seq=%llu observed; "
                            "dispatch opt-in not active",
                            static_cast<int>(intent.kind), intent.hand,
                            static_cast<unsigned long long>(intent.source_sequence));
                }
                continue;
            }
            if (!adapter_ || !adapter_->invoke(static_cast<int>(intent.kind), intent.hand,
                                               intent.target_hint)) {
                ++failed_dispatches_;
                if (failed_dispatches_ == 1 || (failed_dispatches_ % 100) == 0) {
                    MOD_LOG("Game-thread dispatch failed kind=%d hand=%d total=%llu",
                            static_cast<int>(intent.kind), intent.hand,
                            static_cast<unsigned long long>(failed_dispatches_));
                }
            }
        }
    }

    bool dispatch(int kind, int hand) {
        (void)hand;
        if (!installed_.load() || !adapter_ || !config_.dispatch_enabled ||
            !config_.solo_confirmed || !original_process_event_) {
            return false;
        }
        if (config_.target_hint.empty()) {
            ++failed_dispatches_;
            if (failed_dispatches_ == 1 || (failed_dispatches_ % 100) == 0) {
                MOD_LOG("Dispatch skipped: VRC_OUTLAST_TARGET_HINT is required");
            }
            return false;
        }
        const char* role = nullptr;
        switch (static_cast<IntentKind>(kind)) {
            case IntentKind::InteractStart: role = "generic_interact_start"; break;
            case IntentKind::InteractEnd: role = "generic_interact_end"; break;
            case IntentKind::PrimaryUse: role = "item_pickup"; break;
            case IntentKind::ExitCustom: role = "custom_interact_exit"; break;
        }
        if (!role) return false;
        const VerbFunction* fn = adapter_->find_role(role);
        if (!fn || !fn->validated || !fn->flags_known || !fn->layout_known) return false;

        std::string reason;
        const ResolvedFunction resolved = resolve_function(*fn);
        if (!resolved.valid) {
            reason = "live UFunction reflection mismatch";
        }
        const std::uintptr_t target = choose_target(*fn);
        if (!target) {
            reason = "no validated target UObject for " + std::string(role);
        }
        std::vector<u8> params;
        if (reason.empty() && !build_params(*fn, target, params, reason)) {
            // reason is filled by build_params.
        }
        if (!reason.empty()) {
            ++failed_dispatches_;
            if (failed_dispatches_ == 1 || (failed_dispatches_ % 100) == 0) {
                MOD_LOG("Dispatch skipped role=%s: %s", role, reason.c_str());
            }
            return false;
        }

        original_process_event_(reinterpret_cast<void*>(target),
                                reinterpret_cast<void*>(resolved.ufunction),
                                params.empty() ? nullptr : params.data());
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const int count = ++dispatch_counts_[role];
            if (count == 1 || (count % 100) == 0) {
                MOD_LOG("Dispatched %s via original ProcessEvent (count=%d)", role, count);
            }
        }
        return true;
    }

private:
    template <typename Callback>
    void enumerate_objects(Callback callback) const {
        if (!uobject_array_ || !object_item_size_) return;
        const u32 object_count = load_u32(uobject_array_ + num_elements_offset_);
        const u32 chunk_count = load_u32(uobject_array_ + num_chunks_offset_);
        const std::uintptr_t chunks = load_ptr(uobject_array_ + objects_offset_);
        if (!chunks || object_count == 0 || chunk_count == 0 || chunk_count > 0x400) return;
        const std::size_t chunk_limit = 0x10000;
        u32 remaining = object_count;
        for (u32 chunk_index = 0; chunk_index < chunk_count && remaining; ++chunk_index) {
            const std::uintptr_t chunk = load_ptr(chunks + chunk_index * sizeof(std::uintptr_t));
            if (!chunk) continue;
            const std::size_t in_chunk = static_cast<std::size_t>(
                std::min<u32>(remaining, static_cast<u32>(chunk_limit)));
            for (std::size_t i = 0; i < in_chunk; ++i) {
                const std::uintptr_t item = chunk + i * object_item_size_;
                const std::uintptr_t object = load_ptr(item + object_item_offset_);
                if (!object || !valid_object(object)) continue;
                const std::uintptr_t cls = load_ptr(object + class_offset_);
                callback(object, read_fname(cls + name_offset_),
                         read_fname(object + name_offset_));
            }
            remaining -= static_cast<u32>(in_chunk);
        }
    }

    bool valid_object(std::uintptr_t object) const {
        if (!object || !readable_memory(reinterpret_cast<void*>(object), 0x28)) return false;
        const std::uintptr_t cls = load_ptr(object + class_offset_);
        if (!cls || !readable_memory(reinterpret_cast<void*>(cls), 0x28)) return false;
        return !read_fname(cls + name_offset_).empty();
    }

    bool object_exists(std::uintptr_t object) const {
        bool found = false;
        enumerate_objects([&](std::uintptr_t candidate, const std::string&,
                              const std::string&) {
            if (candidate == object) found = true;
        });
        return found;
    }

    std::string read_fname(std::uintptr_t address) const {
        if (!address || !name_blocks_base_ ||
            !readable_memory(reinterpret_cast<void*>(address), 8)) {
            return {};
        }
        const u32 raw_index = load_u32(address);
        if ((raw_index & 0x80000000U) != 0) return {};
        const u32 block = raw_index >> 16;
        const u32 offset = raw_index & 0xFFFFU;
        if (block >= 0x2000U) return {};

        const std::uintptr_t block_ptr =
            load_ptr(name_blocks_base_ + block * sizeof(std::uintptr_t));
        if (!block_ptr) return {};
        const std::uintptr_t entry = block_ptr + offset;
        if (!readable_memory(reinterpret_cast<void*>(entry), sizeof(u16))) return {};
        const u16 header = load_u16(entry);
        const bool wide = (header & 1U) != 0;
        const std::size_t length = (header >> 6) & 0x3FFU;
        if (length == 0 || length >= 0x400) return {};

        const std::uintptr_t chars = entry + name_string_offset_;
        std::string text;
        if (wide) {
            if (!readable_memory(reinterpret_cast<void*>(chars), length * sizeof(wchar_t))) {
                return {};
            }
            const int size = WideCharToMultiByte(
                CP_UTF8, 0, reinterpret_cast<const wchar_t*>(chars),
                static_cast<int>(length), nullptr, 0, nullptr, nullptr);
            if (size <= 0) return {};
            text.resize(static_cast<std::size_t>(size));
            WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(chars),
                                static_cast<int>(length), text.data(), size, nullptr, nullptr);
        } else {
            if (!readable_memory(reinterpret_cast<void*>(chars), length)) return {};
            text.assign(reinterpret_cast<const char*>(chars), length);
        }
        const u32 number = load_u32(address + 4);
        if (number > 1) text += "_" + std::to_string(number - 1);
        return text;
    }

    bool install_shadow(std::uintptr_t object, const std::string& label) {
        const std::uintptr_t vptr = load_ptr(object + vtable_offset_);
        if (!vptr) return false;
        for (const VTableHook& existing : hooks_) {
            if (existing.object == object) return false;
        }
        const std::size_t slots = std::max<std::size_t>(process_event_index_ + 2, 0x80);
        if (!readable_memory(reinterpret_cast<void*>(vptr),
                             slots * sizeof(std::uintptr_t))) {
            return false;
        }
        void* shadow = VirtualAlloc(nullptr, slots * sizeof(std::uintptr_t),
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!shadow) return false;
        std::memcpy(shadow, reinterpret_cast<const void*>(vptr),
                    slots * sizeof(std::uintptr_t));
        reinterpret_cast<ProcessEventFn*>(shadow)[process_event_index_] =
            &hooked_process_event;

        void* previous = InterlockedExchangePointer(
            reinterpret_cast<PVOID volatile*>(object + vtable_offset_), shadow);
        if (previous != reinterpret_cast<void*>(vptr)) {
            InterlockedExchangePointer(
                reinterpret_cast<PVOID volatile*>(object + vtable_offset_), previous);
            VirtualFree(shadow, 0, MEM_RELEASE);
            MOD_LOG("Hook candidate %s skipped: vptr changed during install", label.c_str());
            return false;
        }
        hooks_.push_back({object, previous, shadow, label});
        MOD_LOG("Shadow-vtable hook installed on %s (vptr=0x%llX slot=0x%llX)",
                label.c_str(), static_cast<unsigned long long>(vptr),
                static_cast<unsigned long long>(process_event_index_));
        return true;
    }

    std::uintptr_t find_class_object(const std::string& wanted) const {
        const std::string needle = normalize_class_name(wanted);
        std::uintptr_t found = 0;
        enumerate_objects([&](std::uintptr_t object, const std::string& class_name,
                              const std::string& object_name) {
            if (normalize_class_name(class_name) != "Class") return;
            if (normalize_class_name(object_name) == needle) found = object;
        });
        return found;
    }

    ResolvedFunction resolve_function(const VerbFunction& fn) const {
        const std::string key = fn.role + "/" + fn.function;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto cached = resolved_functions_.find(key);
            if (cached != resolved_functions_.end()) return cached->second;
        }
        ResolvedFunction result;
        const std::uintptr_t cls = find_class_object(
            fn.reflected_class.empty() ? fn.object_class : fn.reflected_class);
        if (!cls) return result;

        std::uintptr_t field = load_ptr(cls + struct_children_offset_);
        for (std::size_t i = 0; field && i < 2048; ++i) {
            const std::string name = read_fname(field + name_offset_);
            if (name == fn.function &&
                normalize_class_name(read_fname(load_ptr(field + class_offset_) + name_offset_)) ==
                    "Function") {
                const u32 flags = load_u32(field + function_flags_offset_);
                const u16 parms_size = load_u16(field + function_parms_size_offset_);
                const u8 num_parms = load_u8(field + function_num_parms_offset_);
                if (flags == static_cast<u32>(fn.flags) &&
                    parms_size == fn.params_size &&
                    num_parms == fn.params.size()) {
                    result.ufunction = field;
                    result.valid = true;
                    break;
                }
                MOD_LOG("Live UFunction %s rejected: flags=0x%X/%0llX size=%u/%llu parms=%u/%zu",
                        fn.function.c_str(), flags,
                        static_cast<unsigned long long>(fn.flags), parms_size,
                        static_cast<unsigned long long>(fn.params_size), num_parms,
                        fn.params.size());
            }
            field = load_ptr(field + field_next_offset_);
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        resolved_functions_[key] = result;
        return result;
    }

    std::uintptr_t find_target_by_hint() const {
        if (config_.target_hint.empty()) return 0;
        std::uintptr_t found = 0;
        enumerate_objects([&](std::uintptr_t object, const std::string& class_name,
                              const std::string& object_name) {
            const std::string normalized = normalize_class_name(class_name);
            if (normalized == "Class" || normalized == "Function" ||
                normalized == "Package" || normalized == "ScriptStruct") {
                return;
            }
            if (contains_ci(object_name, config_.target_hint)) found = object;
        });
        return found;
    }

    std::uintptr_t find_object_by_class(const std::string& wanted) const {
        const std::string needle = normalize_class_name(wanted);
        if (needle.empty()) return 0;
        std::uintptr_t found = 0;
        enumerate_objects([&](std::uintptr_t object, const std::string& class_name,
                              const std::string& object_name) {
            const std::string normalized = normalize_class_name(class_name);
            if (!contains_ci(normalized, needle)) return;
            const bool hinted = !config_.target_hint.empty() &&
                                contains_ci(object_name, config_.target_hint);
            if (hinted) {
                found = object;
            } else if (!found && !contains_ci(object_name, "Default")) {
                found = object;
            }
        });
        return found;
    }

    std::uintptr_t find_local_player() const {
        std::uintptr_t found = 0;
        enumerate_objects([&](std::uintptr_t object, const std::string& class_name,
                              const std::string& object_name) {
            const std::string normalized = normalize_class_name(class_name);
            if (!contains_ci(normalized, "RBPlayer")) return;
            const bool hinted = !config_.target_hint.empty() &&
                                contains_ci(object_name, config_.target_hint);
            if (hinted) {
                found = object;
            } else if (!found && !contains_ci(object_name, "Default")) {
                found = object;
            }
        });
        return found;
    }

    std::uintptr_t choose_target(const VerbFunction& fn) const {
        const std::string class_name = normalize_class_name(
            fn.reflected_class.empty() ? fn.object_class : fn.reflected_class);
        if (class_name == "RBInteractable" || class_name == "RBInteractFocusable") {
            return find_target_by_hint();
        }
        if (class_name == "RBPlayer" || class_name == "RBPawn") {
            return find_local_player();
        }
        std::uintptr_t target = find_object_by_class(class_name);
        if (!target) target = find_target_by_hint();
        return target;
    }

    bool build_params(const VerbFunction& fn, std::uintptr_t target,
                      std::vector<u8>& params, std::string& reason) const {
        params.assign(static_cast<std::size_t>(fn.params_size), 0);
        const std::uintptr_t local_player = find_local_player();
        const std::string target_class = normalize_class_name(
            fn.reflected_class.empty() ? fn.object_class : fn.reflected_class);
        for (const ParamField& param : fn.params) {
            if (param.offset < 0 || param.size <= 0 ||
                static_cast<std::size_t>(param.offset) >= params.size()) {
                if (param.name != "ReturnValue" && param.size > 0) {
                    reason = "parameter layout does not fit params_size for " + param.name;
                    return false;
                }
                continue;
            }
            const std::size_t offset = static_cast<std::size_t>(param.offset);
            const std::size_t size = static_cast<std::size_t>(param.size);
            if (offset > params.size() || size > params.size() - offset) {
                reason = "parameter overflows params_size for " + param.name;
                return false;
            }
            if (param.type.find('*') != std::string::npos) {
                const std::string needle = class_needle_from_type(param.type);
                std::uintptr_t value = 0;
                if (needle == "RBPawn" || needle == "RBPlayer") {
                    value = local_player;
                    if (!value) {
                        reason = "local ARBPawn/ARBPlayer instance not found";
                        return false;
                    }
                } else if (!needle.empty() && needle == target_class && target) {
                    value = target;
                } else {
                    if (!needle.empty()) value = find_object_by_class(needle);
                    if (!value && !config_.target_hint.empty()) value = find_target_by_hint();
                    if (!value) {
                        reason = "no live target for parameter " + param.name + " (" + needle + ")";
                        return false;
                    }
                }
                std::memcpy(params.data() + offset, &value, std::min<std::size_t>(size, sizeof(value)));
            } else if (contains_ci(param.type, "bool")) {
                params[offset] = contains_ci(param.name, "predicted") ? 1 : 0;
            }
        }
        return true;
    }

    GameAdapter* adapter_ = nullptr;
    RuntimeConfig config_;
    std::uintptr_t process_event_ = 0;
    ProcessEventFn original_process_event_ = nullptr;
    std::uintptr_t uobject_array_ = 0;
    std::uintptr_t names_global_ = 0;
    std::uintptr_t name_blocks_base_ = 0;
    std::uintptr_t world_global_ = 0;
    std::size_t process_event_index_ = 0;
    std::size_t name_offset_ = 0x18;
    std::size_t class_offset_ = 0x10;
    std::size_t vtable_offset_ = 0;
    std::size_t field_next_offset_ = 0x28;
    std::size_t struct_children_offset_ = 0x48;
    std::size_t function_flags_offset_ = 0xB0;
    std::size_t function_num_parms_offset_ = 0xB4;
    std::size_t function_parms_size_offset_ = 0xB6;
    std::size_t objects_offset_ = 0;
    std::size_t num_elements_offset_ = 0x14;
    std::size_t num_chunks_offset_ = 0x1C;
    std::size_t object_item_offset_ = 0;
    std::size_t object_item_size_ = 0x18;
    std::size_t name_blocks_offset_ = 0x10;
    std::size_t name_string_offset_ = 0x2;

    std::vector<VTableHook> hooks_;
    std::deque<InteractionIntent> queue_;
    std::mutex queue_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::map<std::string, ResolvedFunction> resolved_functions_;
    std::map<std::string, int> dispatch_counts_;
    std::atomic<bool> installed_{false};
    std::atomic<bool> pumping_{false};
    DWORD game_thread_id_ = 0;
    u64 hook_only_intents_ = 0;
    u64 failed_dispatches_ = 0;
};

std::atomic<LiveUnreal*> g_runtime{nullptr};

void __fastcall hooked_process_event(void* self, void* function, void* params) {
    LiveUnreal* runtime = g_runtime.load();
    const ProcessEventFn original = runtime ? runtime->original_process_event() : nullptr;
    if (original) {
        original(self, function, params);
    }
    if (runtime) runtime->on_game_thread_tick();
}

bool GameAdapter::invoke(int kind, int hand, u64 target_hint) const {
    (void)target_hint;
    LiveUnreal* runtime = g_runtime.load();
    if (!ready_ || !runtime || !runtime->installed()) return false;
    return runtime->dispatch(kind, hand);
}

// ─── XR input → gameplay intent pipeline ────────────────────────────────────

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
};

Vec3 scale_add(const Vec3& a, const Vec3& b, f32 scale) {
    return {a.x + b.x * scale, a.y + b.y * scale, a.z + b.z * scale};
}

Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 xr_to_game_delta(const Vec3& v, f32 world_scale) {
    const f32 scale = world_scale * 100.0f; // metres to UE centimetres
    return {-v.z * scale, v.x * scale, v.y * scale};
}

f32 dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 normalized(Vec3 v) {
    const f32 length = std::sqrt(dot(v, v));
    if (length < 1e-6f) return {};
    return {v.x / length, v.y / length, v.z / length};
}

class DebouncedBool {
public:
    bool update(bool raw) {
        changed_ = false;
        if (raw == candidate_) {
            ++stable_frames_;
        } else {
            candidate_ = raw;
            stable_frames_ = 1;
        }
        if (stable_frames_ >= 2 && candidate_ != stable_) {
            stable_ = candidate_;
            stable_frames_ = 0;
            changed_ = true;
        }
        return changed_;
    }
    bool value() const { return stable_; }
    bool rising() const { return changed_ && stable_; }
    bool falling() const { return changed_ && !stable_; }

private:
    bool stable_ = false;
    bool candidate_ = false;
    bool changed_ = false;
    unsigned stable_frames_ = 0;
};

void request_haptic_safe(bool left, f32 amplitude, f32 duration_s) {
    if (g_request_haptic) g_request_haptic(left, amplitude, duration_s);
}

class InteractionEngine {
public:
    void update(const vrc::VrcXRState& state, GameAdapter& adapter) {
        if (state.sequence == last_sequence_) return;
        if (state.timestamp_ns != 0) {
            const u64 now = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            if (now > state.timestamp_ns && now - state.timestamp_ns > 100000000ULL) {
                ++stale_frames_;
                if (stale_frames_ == 1 || (stale_frames_ % 200) == 0) {
                    MOD_LOG("Bridge state stale by %.1fms; ignoring input intents",
                            static_cast<f64>(now - state.timestamp_ns) / 1000000.0);
                }
                return;
            }
        }
        last_sequence_ = state.sequence;

        const bool menu = (state.button_flags & vrc::VrcButton_Menu) != 0;
        if (menu_.update(menu) && menu_.rising()) {
            queue_.push({IntentKind::ExitCustom, 0, 0, state.sequence});
            request_haptic_safe(false, 0.3f, 0.04f);
        }

        for (int hand = 0; hand < 2; ++hand) {
            const bool left = hand == 0;
            const u32 grip_bit = left ? vrc::VrcButton_GripClickLeft
                                       : vrc::VrcButton_GripClickRight;
            const u32 trigger_bit = left ? vrc::VrcButton_TriggerClickLeft
                                          : vrc::VrcButton_TriggerClickRight;
            const bool grip = (state.button_flags & grip_bit) != 0 ||
                              state.grip_axis[hand] > 0.7f;
            const bool trigger = (state.button_flags & trigger_bit) != 0 ||
                                 state.trigger[hand] > 0.7f;

            if (grip_[hand].update(grip)) {
                if (grip_[hand].rising()) {
                    queue_.push({IntentKind::InteractStart, hand, 0, state.sequence});
                    request_haptic_safe(left, 0.4f, 0.06f);
                } else {
                    queue_.push({IntentKind::InteractEnd, hand, 0, state.sequence});
                    request_haptic_safe(left, 0.2f, 0.04f);
                }
            }
            if (trigger_[hand].update(trigger) && trigger_[hand].rising()) {
                queue_.push({IntentKind::PrimaryUse, hand, 0, state.sequence});
                request_haptic_safe(left, 0.35f, 0.05f);
            }

            if (pose_usable(state.aim[hand]) && pose_usable(state.head) &&
                (state.sequence % 240) == 0) {
                const Vec3 head{
                    state.head.position[0], state.head.position[1],
                    state.head.position[2]};
                const Vec3 aim{
                    state.aim[hand].position[0], state.aim[hand].position[1],
                    state.aim[hand].position[2]};
                const Vec3 direction = normalized(sub(aim, head));
                const Vec3 game_direction =
                    xr_to_game_delta(direction, state.world_scale);
                MOD_LOG("aim[%d] dir_game=(%.3f,%.3f,%.3f) grip=%s trigger=%s",
                        hand, game_direction.x, game_direction.y, game_direction.z,
                        grip_[hand].value() ? "down" : "up",
                        trigger_[hand].value() ? "down" : "up");
            }
        }

        if (adapter.ready() && !logged_ready_) {
            logged_ready_ = true;
            LiveUnreal* runtime = g_runtime.load();
            MOD_LOG("Validated verb map accepted; runtime hook %s",
                    runtime && runtime->installed() ? "active" : "not installed");
        }
    }

    void pump(GameAdapter& adapter) {
        (void)adapter;
        while (!queue_.empty()) {
            const InteractionIntent intent = queue_.front();
            queue_.pop();
            LiveUnreal* runtime = g_runtime.load();
            if (runtime && runtime->enqueue(intent)) continue;
            ++dry_run_intents_;
            if (dry_run_intents_ == 1 || (dry_run_intents_ % 100) == 0) {
                MOD_LOG("Dry-run intent kind=%d hand=%d seq=%llu total=%llu; "
                        "no game verb dispatched",
                        static_cast<int>(intent.kind), intent.hand,
                        static_cast<unsigned long long>(intent.source_sequence),
                        static_cast<unsigned long long>(dry_run_intents_));
            }
        }
    }

private:
    DebouncedBool grip_[2];
    DebouncedBool trigger_[2];
    DebouncedBool menu_;
    std::queue<InteractionIntent> queue_;
    u64 last_sequence_ = 0;
    u64 stale_frames_ = 0;
    u64 dry_run_intents_ = 0;
    bool logged_ready_ = false;
};

bool file_exists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

DWORD WINAPI mod_thread(LPVOID) {
    struct ThreadDoneSignal {
        ~ThreadDoneSignal() {
            if (g_thread_done) SetEvent(g_thread_done);
        }
    } done_signal;

    MOD_LOG("Outlast Trials interaction engine starting (runtime-gated)");

    for (int attempt = 0; attempt < 600; ++attempt) {
        if (load_bridge_api()) break;
        Sleep(100);
    }
    if (!g_get_xr_state && !g_copy_xr_state) {
        MOD_LOG("Init timed out waiting for vr_converter.dll; mod disabled");
        return 0;
    }

    GameAdapter adapter;
    std::string adapter_error;
    const std::string base = module_dir();
    const std::string validated_path = base + "\\outlast_verb_map.validated.json";
    const std::string static_path = base + "\\outlast_verb_map.json";
    const std::string map_path =
        file_exists(validated_path) ? validated_path : static_path;
    if (!adapter.load(map_path, adapter_error)) {
        MOD_LOG("Verb map not loaded (%s); interaction engine disabled",
                adapter_error.c_str());
        return 0;
    }

    MOD_LOG("Verb map: status=%s candidates=%zu tier1=%zu build=%s hash_match=%s "
            "map_dispatch=%s ready=%s",
            adapter.status().c_str(), adapter.functions().size(),
            adapter.tier1_count(), adapter.build_sha256().substr(0, 16).c_str(),
            adapter.build_hash_matches() ? "yes" : "no",
            adapter.map_dispatch_allowed() ? "yes" : "no",
            adapter.ready() ? "yes" : "no");
    MOD_LOG("ProcessEvent RVA=0x%llX slot=%llu confirmed=%s; globals=%zu offsets=%zu",
            static_cast<unsigned long long>(adapter.process_event_rva()),
            static_cast<unsigned long long>(adapter.process_event_index()),
            adapter.ready() ? "yes" : "no", adapter.globals().size(),
            adapter.offsets().size());
    if (!adapter.ready()) {
        MOD_LOG("No authorized validation path is active; intents are dry-run only");
    }

    const RuntimeConfig runtime_config = RuntimeConfig::read();
    LiveUnreal runtime;
    struct RuntimeLifetime {
        LiveUnreal& value;
        ~RuntimeLifetime() {
            if (g_runtime.load() == &value) {
                value.shutdown();
                g_runtime.store(nullptr);
            }
        }
    } runtime_lifetime{runtime};
    g_runtime.store(&runtime);
    MOD_LOG("Runtime opt-ins: hook=%s solo=%s dispatch=%s target_hint=%s hook_hint=%s",
            runtime_config.hook_enabled ? "yes" : "no",
            runtime_config.solo_confirmed ? "yes" : "no",
            runtime_config.dispatch_enabled ? "yes" : "no",
            runtime_config.target_hint.empty() ? "<none>" : runtime_config.target_hint.c_str(),
            runtime_config.hook_hint.empty() ? "<none>" : runtime_config.hook_hint.c_str());
    if (runtime_config.dispatch_enabled && !runtime_config.solo_confirmed) {
        MOD_LOG("WARNING: dispatch requested without VRC_OUTLAST_SOLO=1; dispatch stays disabled");
    }
    if (runtime_config.hook_enabled && adapter.ready()) {
        for (int attempt = 0; attempt < 120; ++attempt) {
            if (runtime.install(adapter, runtime_config)) break;
            Sleep(500);
        }
    } else if (!runtime_config.hook_enabled) {
        MOD_LOG("Runtime hook disabled; set VRC_OUTLAST_ENABLE_RUNTIME_HOOK=1 to enable");
    }

    InteractionEngine engine;
    unsigned long long frames = 0;
    bool logged_first = false;
    for (;;) {
        if (g_stop_requested.load()) break;
        vrc::VrcXRState state{};
        if (read_bridge_state(state)) {
            ++frames;
            if (state.magic != vrc::kVrcXRStateMagic ||
                state.version != vrc::kVrcXRStateVersion ||
                state.size_bytes != sizeof(vrc::VrcXRState)) {
                MOD_LOG("Bridge ABI mismatch; mod disabled");
                return 0;
            }
            if (!logged_first || (frames % 240) == 0) {
                logged_first = true;
                MOD_LOG("XR state seq=%u head=%d grips=%d/%d flags=%u scale=%.2f "
                        "ipd=%.4f period=%.1fms",
                        state.sequence, pose_usable(state.head) ? 1 : 0,
                        pose_usable(state.grip[0]) ? 1 : 0,
                        pose_usable(state.grip[1]) ? 1 : 0,
                        static_cast<unsigned int>(state.flags), state.world_scale,
                        state.ipd, state.display_period_s * 1000.0f);
            }
            engine.update(state, adapter);
            engine.pump(adapter);
            if (runtime_config.hook_enabled && adapter.ready() && !runtime.installed() &&
                (frames % 200) == 0) {
                (void)runtime.install(adapter, runtime_config);
            }
        }
        Sleep(10);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) vrc::u32 VRC_OutlastModVersion() {
    return 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_stop_requested.store(false);
        g_thread_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = CreateThread(nullptr, 0, mod_thread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop_requested.store(true);
        if (g_thread_done) {
            if (reserved == nullptr) WaitForSingleObject(g_thread_done, 3000);
            CloseHandle(g_thread_done);
            g_thread_done = nullptr;
        }
    }
    return TRUE;
}
