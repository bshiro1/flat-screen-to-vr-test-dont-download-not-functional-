#pragma once

#include "core/types.h"
#include <memory>
#include <vector>
#include <functional>

namespace vrc {

class HookManager {
public:
    static HookManager& instance();

    bool initialize();
    void shutdown();

    bool attach_to_process(const std::string& process_name);
    void detach();

    bool hook_present(GraphicsAPI api);
    bool unhook_present(GraphicsAPI api);

    bool hook_wndproc();
    bool unhook_wndproc();

    bool is_hooked() const { return hooked_; }
    GraphicsAPI detected_api() const { return detected_api_; }

    void set_on_present(OnPresentCallback callback) { on_present_ = callback; }
    void set_on_frame(OnFrameCallback callback) { on_frame_ = callback; }

    void fire_on_present(HookContext& ctx) { if (on_present_) on_present_(ctx); }
    void fire_on_frame(const FrameCapture& capture) { if (on_frame_) on_frame_(capture); }

    HookContext& context() { return ctx_; }

    // API-specific hook enable/disable
    bool hook_d3d11();
    bool hook_d3d12();
    bool hook_opengl();
    bool hook_vulkan();

private:
    HookManager() = default;

    bool detect_graphics_api();
    void* find_swap_chain();

    bool hooked_ = false;
    GraphicsAPI detected_api_ = GraphicsAPI::Unknown;
    HookContext ctx_;
    OnPresentCallback on_present_;
    OnFrameCallback on_frame_;
    std::string target_process_;
};

} // namespace vrc
