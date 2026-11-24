#include "config_editor.h"
#include "imgui_overlay.h"
#include "core/config.h"
#include "core/logging.h"
#include "core/perf_monitor.h"
#include "input/input_proxy.h"
#include "input/input_mapper.h"

#include <imgui.h>
#include <algorithm>
#include <string>
#include <vector>

// UTF-8 degree sign; the default ImGui font covers Latin-1, so it renders.
#define VRC_DEG "\xC2\xB0"

namespace vrc {

namespace {

// ImGui::InputText operates on a mutable char buffer. Edit the std::string in
// place with a resize callback so long values (game_mod_path, bridge_name, FBT
// role, ...) are never truncated to a fixed-size buffer.
int input_text_resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

bool input_text_string(const char* label, std::string& value) {
    return ImGui::InputText(label, value.data(), value.size() + 1,
                            ImGuiInputTextFlags_CallbackResize,
                            input_text_resize_callback, &value);
}

// "(?)" marker with a hover tooltip, placed after the previous item.
void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Slider followed by a reset button. Returns true if the value changed.
bool slider_with_reset(const char* label, const char* reset_id, f32& value,
                       f32 min, f32 max, const char* fmt, f32 reset_value,
                       ImGuiSliderFlags flags = 0) {
    bool changed = ImGui::SliderFloat(label, &value, min, max, fmt, flags);
    ImGui::SameLine();
    if (ImGui::Button(reset_id)) {
        value = reset_value;
        changed = true;
    }
    return changed;
}

const char* api_name(GraphicsAPI api) {
    switch (api) {
        case GraphicsAPI::D3D11:  return "D3D11";
        case GraphicsAPI::D3D12:  return "D3D12 (overlay via D3D11On12)";
        case GraphicsAPI::Vulkan: return "Vulkan";
        case GraphicsAPI::OpenGL: return "OpenGL";
        default:                  return "Unknown";
    }
}

} // namespace

ConfigEditor& ConfigEditor::instance() {
    static ConfigEditor editor;
    return editor;
}

void ConfigEditor::draw() {
    if (!open_) return;

    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("VR Game Converter - Settings", &open_)) {
        ImGui::End();
        return;
    }

    // One snapshot per frame; written back once, and only if a control changed.
    ConfigProfile cfg = Config::instance().current_profile();
    bool changed = false;

    if (ImGui::BeginTabBar("ConfigTabs")) {
        if (ImGui::BeginTabItem("Display")) {
            changed |= draw_display_tab(cfg);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Depth 3D")) {
            changed |= draw_depth_tab(cfg);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Bridge & Mods")) {
            changed |= draw_bridge_tab(cfg);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Input")) {
            draw_input_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance")) {
            draw_performance_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (changed) Config::instance().set_current_profile(cfg);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f),
                       "Changes apply immediately. Save writes them to config.json.");

    auto& config = Config::instance();
    if (ImGui::Button("Save Configuration")) {
        config.save(config.config_dir() / "config.json");
        // Keep profiles\<name>.json in step with config.json, as the companion
        // app does.
        config.save_profile(config.current_profile().name);
        // Input settings live in their own profile file.
        InputMapper::instance().save_profile(InputMapper::instance().current_profile().name);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Configuration")) {
        // Also reloads the input profile the loaded VR profile names.
        load_failed_ = !config.load(config.config_dir() / "config.json");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) {
        config.set_default_config();
        load_failed_ = false;
    }
    if (load_failed_) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Load failed - check the log");
    }

    ImGui::End();
}

bool ConfigEditor::draw_display_tab(ConfigProfile& cfg) {
    const ConfigProfile def;
    bool changed = false;

    ImGui::Text("Active Profile: %s", cfg.name.c_str());

    ImGui::SeparatorText("Render Mode");
    const char* render_modes[] = { "Screen (virtual flat screen)",
                                   "Depth 3D (experimental, D3D11 only)" };
    int mode_idx = std::clamp(static_cast<int>(cfg.render_mode), 0,
                              static_cast<int>(IM_ARRAYSIZE(render_modes)) - 1);
    if (ImGui::Combo("Render Mode", &mode_idx, render_modes, IM_ARRAYSIZE(render_modes))) {
        cfg.render_mode = static_cast<RenderMode>(mode_idx);
        changed = true;
    }
    if (cfg.render_mode == RenderMode::Depth3D) {
        ImGui::TextWrapped("Depth 3D reprojects the game image using its depth buffer. "
                           "It falls back to Screen until a usable depth buffer is "
                           "captured. Settings are on the Depth 3D tab.");
    } else {
        ImGui::TextWrapped("The game is shown on a flat screen in VR. Your headset "
                           "renders it per eye, so it always has correct stereo.");
    }

    ImGui::SeparatorText("Virtual Screen");
    if (cfg.render_mode == RenderMode::Depth3D) {
        ImGui::TextDisabled("In Depth 3D, distance and size place the image plane.");
    }
    changed |= slider_with_reset("Screen Distance (m)", "Reset##dist",
                                 cfg.convergence_distance, 0.5f, 20.0f, "%.1f m",
                                 def.convergence_distance);
    changed |= slider_with_reset("Screen Size (FOV" VRC_DEG ", 0 = default 60" VRC_DEG ")",
                                 "Reset##fov", cfg.fov_override, 0.0f, 170.0f,
                                 "%.0f" VRC_DEG, def.fov_override);
    help_marker("Horizontal angle the screen covers in your view. 0 uses the "
                "default of 60" VRC_DEG ".");

    const char* scale_label = cfg.render_mode == RenderMode::Depth3D ? "World Scale"
                                                                     : "Screen Scale";
    changed |= slider_with_reset(scale_label, "Reset##scale", cfg.world_scale,
                                 0.25f, 4.0f, "%.2fx", def.world_scale);
    help_marker("Screen mode: multiplies the screen size. Depth 3D: scales the game "
                "world relative to your head movement. Also sent to game mods "
                "through the 6DoF bridge as the world scale.");

    ImGui::BeginDisabled(cfg.render_mode != RenderMode::Screen);
    changed |= ImGui::Checkbox("World-locked screen", &cfg.enable_head_tracking);
    ImGui::SetItemTooltip("On: the screen stays fixed in the room.\n"
                          "Off: the screen follows your head.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Resolution");
    changed |= slider_with_reset("Render Scale (applies next launch)", "Reset##rs",
                                 cfg.render_scale, 0.25f, 2.0f, "%.2fx", def.render_scale);
    help_marker("Multiplies the VR swapchain resolution. Swapchains are created when "
                "the VR session starts, so a change takes effect on the next launch.");

    ImGui::SeparatorText("Overlay");
    bool show_overlay = ImGuiOverlay::instance().is_visible();
    if (ImGui::Checkbox("Show Config Overlay", &show_overlay)) {
        ImGuiOverlay::instance().set_visible(show_overlay);
    }
    ImGui::TextDisabled("Toggle key: F2");
    ImGui::Text("Graphics API: %s", api_name(Config::instance().detected_api()));

    return changed;
}

bool ConfigEditor::draw_depth_tab(ConfigProfile& cfg) {
    const ConfigProfile def;
    bool changed = false;

    if (cfg.render_mode != RenderMode::Depth3D) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "Used only when Render Mode is Depth 3D (Display tab).");
    }

    ImGui::SeparatorText("Depth Reprojection");
    changed |= ImGui::Checkbox("Volumetric Parallax", &cfg.volumetric_parallax);
    help_marker("Reproject each pixel using the game's depth buffer so near and far "
                "objects separate as you move your head. When off, or when no depth "
                "buffer has been captured, the image is shown flat.");

    changed |= slider_with_reset("Parallax Strength", "Reset##ps", cfg.parallax_strength,
                                 0.0f, 3.0f, "%.2f", def.parallax_strength);
    help_marker("0 = flat, 1 = physically scaled, above 1 exaggerates depth.");

    ImGui::SeparatorText("Game Camera");
    ImGui::TextWrapped("These must match the game's camera, otherwise depth looks "
                       "stretched or squashed.");
    changed |= slider_with_reset("Game FOV (" VRC_DEG ")", "Reset##gfov",
                                 cfg.depth_fov_degrees, 30.0f, 150.0f,
                                 "%.0f" VRC_DEG, def.depth_fov_degrees);
    help_marker("Set this to the field of view configured in the game's own graphics "
                "settings. It is the game camera's FOV, not the headset's.");

    changed |= slider_with_reset("Near Plane (m)", "Reset##near", cfg.depth_near_plane,
                                 0.001f, 10.0f, "%.3f m", def.depth_near_plane,
                                 ImGuiSliderFlags_Logarithmic);
    changed |= slider_with_reset("Far Plane (m)", "Reset##far", cfg.depth_far_plane,
                                 10.0f, 100000.0f, "%.0f m", def.depth_far_plane,
                                 ImGuiSliderFlags_Logarithmic);
    help_marker("The game camera's near/far clip distances. The far plane must be "
                "greater than the near plane.");

    changed |= ImGui::Checkbox("Reversed-Z depth (Unity/Unreal)", &cfg.depth_reversed_z);
    help_marker("Unity (D3D11 and newer) and Unreal store depth reversed (near = 1, "
                "far = 0). Turn this off if near objects appear to be the farthest away.");

    ImGui::SeparatorText("Eyes");
    changed |= ImGui::Checkbox("Swap Eyes", &cfg.swap_eyes);
    help_marker("Sends the left-eye image to the right eye and vice versa. Use the "
                "debug tint below to check.");
    changed |= ImGui::Checkbox("Show Eye Debug Tint", &cfg.show_eye_debug);
    help_marker("Tints the top-right corner of each eye's image: red = image rendered "
                "for the left eye, blue = image rendered for the right eye.");
    if (cfg.show_eye_debug) {
        ImGui::TextWrapped("Close one eye at a time. Red should be in your left eye. "
                           "If red appears in your right eye, enable Swap Eyes.");
    }

    return changed;
}

bool ConfigEditor::draw_bridge_tab(ConfigProfile& cfg) {
    const ConfigProfile def;
    bool changed = false;

    ImGui::SeparatorText("Input Injection");
    const char* injection_methods[] = { "Legacy SendInput", "6DoF Bridge" };
    int injection_idx = std::clamp(static_cast<int>(cfg.injection_method), 0,
                                   static_cast<int>(IM_ARRAYSIZE(injection_methods)) - 1);
    if (ImGui::Combo("Injection Method (applies next launch)", &injection_idx,
                     injection_methods, IM_ARRAYSIZE(injection_methods))) {
        cfg.injection_method = static_cast<InjectionMethod>(injection_idx);
        changed = true;
    }
    help_marker("Legacy SendInput turns VR controller input into keyboard/mouse input. "
                "6DoF Bridge also publishes head, controller and tracker poses to "
                "game-side mods through shared memory. The bridge is created at startup.");

    changed |= ImGui::Checkbox("Send Legacy Input via Bridge", &cfg.bridge_send_legacy_input);
    help_marker("With the 6DoF Bridge selected, also send keyboard/mouse input to the game.");

    changed |= input_text_string("Bridge Name (applies next launch)", cfg.bridge_name);

    changed |= slider_with_reset("IPD for game mods (bridge)", "Reset##ipd", cfg.ipd,
                                 0.050f, 0.080f, "%.4f m", def.ipd);
    help_marker("Sent to game-side mods through the bridge when the headset does not "
                "report its IPD. Rendering always uses the headset's own IPD.");

    ImGui::SeparatorText("Game-side Mod");
    changed |= ImGui::Checkbox("Inject game-side mod on launch", &cfg.game_mod_enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(companion Play in VR only)");
    changed |= input_text_string("Game Mod DLL", cfg.game_mod_path);
    ImGui::TextDisabled("Blank uses the bundled vrc_big_walk_mod.dll if it was built.");

    ImGui::SeparatorText("Full Body Tracking");
    changed |= ImGui::Checkbox("Enable Full Body Tracking", &cfg.fbt_enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(requires the 6DoF bridge)");
    if (cfg.fbt_enabled) {
        ImGui::Indent();
        ImGui::TextDisabled("OpenXR tracker role paths.");
        changed |= input_text_string("Hips Role", cfg.fbt_hips_role);
        changed |= input_text_string("Left Foot Role", cfg.fbt_left_foot_role);
        changed |= input_text_string("Right Foot Role", cfg.fbt_right_foot_role);
        changed |= input_text_string("Chest Role", cfg.fbt_chest_role);
        changed |= input_text_string("Left Shoulder Role", cfg.fbt_left_shoulder_role);
        changed |= input_text_string("Right Shoulder Role", cfg.fbt_right_shoulder_role);
        ImGui::Unindent();
    }

    return changed;
}

void ConfigEditor::draw_input_tab() {
    auto& mapper = InputMapper::instance();
    auto& proxy = InputProxy::instance();
    // Edit a copy and write it back through set_profile(), which also updates
    // the stored profile that Save serialises.
    auto profile = mapper.current_profile();
    bool dirty = false;

    ImGui::SeparatorText("Input Mapping");
    ImGui::Text("Profile: %s", profile.name.c_str());

    dirty |= ImGui::SliderFloat("Look Sensitivity", &profile.look_sensitivity,
                                0.1f, 5.0f, "%.1f");
    dirty |= ImGui::SliderFloat("Movement Smoothing", &profile.movement_smoothing,
                                0.0f, 1.0f, "%.2f");

    dirty |= ImGui::Checkbox("Head Aim (look to aim)", &profile.head_aim);
    if (profile.head_aim) {
        ImGui::Indent();
        dirty |= ImGui::Checkbox("Invert Look X", &profile.invert_x);
        ImGui::SameLine();
        dirty |= ImGui::Checkbox("Invert Look Y", &profile.invert_y);
        ImGui::Unindent();
    }

    dirty |= ImGui::Checkbox("Snap Turn", &profile.snap_turn);
    dirty |= ImGui::SliderFloat("Snap Turn Angle", &profile.snap_turn_angle,
                                15.0f, 90.0f, "%.0f");
    dirty |= ImGui::Checkbox("Vignette on Movement", &profile.vignette_on_move);

    ImGui::SeparatorText("Active Input Actions");
    for (u32 i = 0; i < 20; i++) {
        f32 val = proxy.get_action_value(static_cast<InputAction>(i));
        if (val > 0.01f) {
            ImGui::Text("Action %u: %.2f", i, val);
        }
    }

    ImGui::SeparatorText("Profiles");
    for (auto& p : mapper.available_profiles()) {
        if (ImGui::Button(p.c_str())) {
            mapper.load_profile(p);
            // A different profile is now active; don't overwrite it with the
            // copy edited before the click.
            dirty = false;
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    if (dirty) mapper.set_profile(profile);
}

void ConfigEditor::draw_performance_tab() {
    auto& perf = PerfMonitor::instance();

    ImGui::SeparatorText("Game Frame Timing");
    const u32 fps = perf.fps();
    ImGui::Text("Frame Rate:      %u FPS", fps);
    ImGui::Text("Frame Time:      %.2f ms avg   %.2f min   %.2f max",
                perf.avg_frame_ms(), perf.min_frame_ms(), perf.max_frame_ms());
    ImGui::Text("99th Percentile: %.2f ms", perf.percentile_99_frame_ms());
    ImGui::Text("CPU Work:        %.2f ms", perf.avg_cpu_ms());
    ImGui::Text("GPU Work:        %.2f ms", perf.avg_gpu_ms());
    const f32 dropped = perf.dropped_frames();
    ImGui::Text("Frames over 11 ms: %.1f%%", dropped * 100.0f);

    if (fps < 90) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Below 90 FPS");
    } else {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "90 FPS or more");
    }

    ImGui::SeparatorText("Frame Time History");
    auto recent = perf.recent_frame_times(120);
    if (!recent.empty()) {
        std::vector<float> samples;
        samples.reserve(recent.size());
        for (f64 ms : recent) samples.push_back(static_cast<float>(ms));
        const float max_ms = *std::max_element(samples.begin(), samples.end());
        ImGui::PlotLines("##frametimes", samples.data(), static_cast<int>(samples.size()),
                         0, nullptr, 0.0f, std::max(max_ms * 1.1f, 1.0f),
                         ImVec2(-1.0f, 80.0f));
        ImGui::TextDisabled("Last %zu frames, 0 to %.1f ms", samples.size(), max_ms * 1.1f);
    }

    if (fps > 0 && fps < 90) {
        ImGui::SeparatorText("Tips");
        ImGui::TextWrapped("Lower Render Scale (Display tab, applies next launch) or the "
                           "game's own graphics settings to raise the frame rate.");
    }
}

} // namespace vrc
