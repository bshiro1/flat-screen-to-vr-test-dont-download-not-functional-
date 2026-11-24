#include "xr_shared_state.h"
#include "core/config.h"
#include "core/logging.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vrc {

XRStateBridge& XRStateBridge::instance() {
    static XRStateBridge bridge;
    return bridge;
}

bool XRStateBridge::initialize(const std::string& name, u32 injection_method) {
    std::lock_guard<std::mutex> init_lock(init_mutex_);
    return initialize_locked(name, injection_method);
}

bool XRStateBridge::initialize_locked(const std::string& name, u32 injection_method) {
    shutdown_mappings();

    if (name.empty()) {
        Log::warn("XRStateBridge: empty shared-memory name; bridge disabled");
        return false;
    }
    if (name.size() >= sizeof(shared_name_)) {
        Log::warn("XRStateBridge: shared-memory name too long; bridge disabled");
        return false;
    }

    // Marshal UTF-8 config text correctly for the W APIs.
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       name.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) {
        Log::warn("XRStateBridge: invalid UTF-8 shared-memory name ({})",
                  GetLastError());
        return false;
    }
    std::wstring wide_name(static_cast<size_t>(wide_len), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.c_str(), -1,
                             wide_name.data(), wide_len)) {
        Log::warn("XRStateBridge: shared-memory name conversion failed ({})",
                  GetLastError());
        return false;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(VrcXRSharedMapping)),
        wide_name.c_str());
    if (!mapping) {
        Log::warn("XRStateBridge: CreateFileMappingW failed ({})", GetLastError());
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                               sizeof(VrcXRSharedMapping));
    if (!view) {
        Log::warn("XRStateBridge: MapViewOfFile failed ({})", GetLastError());
        CloseHandle(mapping);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        auto* header = static_cast<VrcXRSharedMapping*>(view);
        auto* seq = reinterpret_cast<volatile LONG*>(&header->sequence);

        // Reinitialization with the same name can happen while another process
        // is attached. Use the same odd/even write phase as publish() so it
        // never observes a zeroed or partially initialized state as valid.
        InterlockedIncrement(seq);
        MemoryBarrier();

        header->reserved0 = header->reserved1 = header->reserved2 = 0;
        std::memset(&header->state, 0, sizeof(header->state));
        header->state.magic = kVrcXRStateMagic;
        header->state.version = kVrcXRStateVersion;
        header->state.size_bytes = sizeof(VrcXRState);

        MemoryBarrier();
        InterlockedIncrement(seq);

        mapping_ = mapping;
        view_ = view;
        std::snprintf(shared_name_, sizeof(shared_name_), "%s", name.c_str());
        effective_injection_method_.store(injection_method, std::memory_order_release);
        state_sequence_.store(0, std::memory_order_release);
    }

    Log::info("XRStateBridge: published '{}' ({} bytes)", name.c_str(),
              static_cast<int>(sizeof(VrcXRState)));
    return true;
}

void XRStateBridge::shutdown_locked() {
    {
        // Drop any pending game-thread requests when the bridge goes away.
        std::lock_guard<std::mutex> haptic_lock(haptic_mutex_);
        haptic_queue_.clear();
    }
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    shared_name_[0] = '\0';
    effective_injection_method_.store(0, std::memory_order_release);
    state_sequence_.store(0, std::memory_order_release);
}

void XRStateBridge::shutdown_mappings() {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    shutdown_locked();
}

void XRStateBridge::shutdown() {
    std::lock_guard<std::mutex> init_lock(init_mutex_);
    shutdown_mappings();
}

bool XRStateBridge::is_initialized() const {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    return view_ != nullptr;
}

const char* XRStateBridge::shared_name() const {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    return view_ ? shared_name_ : "";
}

u32 XRStateBridge::copy_shared_name(char* dst, u32 dst_size) const {
    if (!dst || dst_size == 0) return 0;
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    if (!view_) return 0;

    size_t len = 0;
    while (len < sizeof(shared_name_) && shared_name_[len] != '\0') ++len;
    const size_t copy_len = len < static_cast<size_t>(dst_size) - 1
        ? len
        : static_cast<size_t>(dst_size) - 1;
    std::memcpy(dst, shared_name_, copy_len);
    dst[copy_len] = '\0';
    return static_cast<u32>(copy_len);
}

void XRStateBridge::publish(const VrcXRState& state) {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto* header = static_cast<VrcXRSharedMapping*>(view_);
    if (!header) return;

    auto* seq = reinterpret_cast<volatile LONG*>(&header->sequence);

    // Odd sequence = writer active. Readers retry until an even, stable value.
    InterlockedIncrement(seq);
    MemoryBarrier();

    std::memcpy(&header->state, &state, sizeof(VrcXRState));

    MemoryBarrier();
    InterlockedIncrement(seq);
    state_sequence_.store(state.sequence, std::memory_order_release);
}

bool XRStateBridge::read(VrcXRState& out) const {
    std::lock_guard<std::mutex> lock(mapping_mutex_);
    auto* header = static_cast<const VrcXRSharedMapping*>(view_);
    if (!header) return false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        u32 before = header->sequence;
        if (before & 1u) continue;

        MemoryBarrier();
        std::memcpy(&out, &header->state, sizeof(VrcXRState));
        MemoryBarrier();

        u32 after = header->sequence;
        if (before == after && !(after & 1u)) {
            return out.magic == kVrcXRStateMagic &&
                   out.version == kVrcXRStateVersion &&
                   out.size_bytes == sizeof(VrcXRState);
        }
    }
    return false;
}

bool XRStateBridge::copy_state(VrcXRState& out, u32 out_size) const {
    // Never touch a caller buffer whose declared size is smaller than ours.
    if (out_size < sizeof(VrcXRState)) return false;

    VrcXRState snapshot{};
    if (!read(snapshot)) return false;
    out = snapshot;
    return true;
}

bool XRStateBridge::request_haptic(bool left, f32 amplitude, f32 duration_s) {
    if (!is_initialized()) return false;
    if (!std::isfinite(amplitude) || !std::isfinite(duration_s)) return false;
    if (amplitude <= 0.0f || duration_s <= 0.0f) return false;

    HapticRequest request;
    request.left = left;
    request.amplitude = amplitude < 0.0f ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);
    request.duration_s = duration_s < 0.0f ? 0.0f : (duration_s > 2.0f ? 2.0f : duration_s);

    std::lock_guard<std::mutex> lock(haptic_mutex_);
    // Bounded queue: a runaway game-side mod must not grow memory unbounded.
    if (haptic_queue_.size() >= 32) haptic_queue_.erase(haptic_queue_.begin());
    haptic_queue_.push_back(request);
    return true;
}

} // namespace vrc

VRC_BRIDGE_API vrc::u32 VRC_GetBridgeApiVersion() {
    return 1;
}

VRC_BRIDGE_API vrc::u32 VRC_GetInjectionMethod() {
    return vrc::XRStateBridge::instance().injection_method();
}

VRC_BRIDGE_API bool VRC_GetXRState(vrc::VrcXRState* out) {
    return out ? vrc::XRStateBridge::instance().read(*out) : false;
}

VRC_BRIDGE_API bool VRC_CopyXRState(vrc::VrcXRState* out, vrc::u32 out_size) {
    return out && vrc::XRStateBridge::instance().copy_state(*out, out_size);
}

VRC_BRIDGE_API const char* VRC_GetXRSharedMemoryName() {
    return vrc::XRStateBridge::instance().shared_name();
}

VRC_BRIDGE_API vrc::u32 VRC_CopyXRSharedMemoryName(char* dst, vrc::u32 dst_size) {
    return vrc::XRStateBridge::instance().copy_shared_name(dst, dst_size);
}

VRC_BRIDGE_API vrc::u32 VRC_GetXRStateSequence() {
    return vrc::XRStateBridge::instance().sequence_for_readers();
}

VRC_BRIDGE_API bool VRC_RequestHaptic(bool left, vrc::f32 amplitude,
                                      vrc::f32 duration_s) {
    return vrc::XRStateBridge::instance().request_haptic(left, amplitude, duration_s);
}
