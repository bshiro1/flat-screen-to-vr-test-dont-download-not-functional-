#pragma once

#include "core/types.h"

namespace vrc {

// Settings window drawn inside the ImGui overlay. Edits a snapshot of the
// active ConfigProfile and writes it back with Config::set_current_profile()
// when a control changed; the renderer and input code read that profile every
// frame, so changes apply immediately (except where a label says otherwise).
class ConfigEditor {
public:
    static ConfigEditor& instance();

    void draw();

    void set_open(bool open) { open_ = open; }
    bool is_open() const { return open_; }
    void toggle() { open_ = !open_; }

private:
    ConfigEditor() = default;
    bool open_ = true;
    bool load_failed_ = false;

    // Each returns true if it changed `cfg`.
    bool draw_display_tab(ConfigProfile& cfg);
    bool draw_depth_tab(ConfigProfile& cfg);
    bool draw_bridge_tab(ConfigProfile& cfg);
    void draw_input_tab();
    void draw_performance_tab();
};

} // namespace vrc
