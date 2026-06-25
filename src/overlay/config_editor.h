#pragma once

#include "core/types.h"

namespace vrc {

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

    void draw_general_tab();
    void draw_rendering_tab();
    void draw_tracking_tab();
    void draw_input_tab();
    void draw_performance_tab();
};

} // namespace vrc
