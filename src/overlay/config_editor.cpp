#include "config_editor.h"
#include "core/config.h"
#include "core/logging.h"
#include "core/perf_monitor.h"
#include "render/stereo_renderer.h"
#include "render/latency_compensator.h"
#include "render/depth_reprojection.h"
#include "render/camera_rig.h"
#include "vr/tracking.h"
#include "input/input_proxy.h"
#include "input/input_mapper.h"

#include <imgui.h>
#include <format>
#include <vector>
#include <string>

namespace vrc {

ConfigEditor& ConfigEditor::instance() {
    static ConfigEditor editor;
    return editor;
}

void ConfigEditor::draw() {
    if (!open_) return;

    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("VR Game Converter - Configuration", &open_,
                      ImGuiWindowFlags_MenuBar))
    {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("ConfigTabs")) {
        if (ImGui::BeginTabItem("General")) {
            draw_general_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rendering")) {
            draw_rendering_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tracking")) {
            draw_tracking_tab();
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

    ImGui::Separator();

    if (ImGui::Button("Save Configuration")) {
        Config::instance().save(Config::instance().config_dir() / "config.json");
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Configuration")) {
        Config::instance().load(Config::instance().config_dir() / "config.json");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) {
        Config::instance().set_default_config();
    }

    ImGui::End();
}

void ConfigEditor::draw_general_tab() {
    auto& cfg = Config::instance().current_profile();
    auto& global = Config::instance();

    ImGui::Text("Active Profile: %s", cfg.name.c_str());

    char name_buf[128] = {};
    strncpy_s(name_buf, cfg.name.c_str(), sizeof(name_buf) - 1);
    if (ImGui::InputText("Profile Name", name_buf, sizeof(name_buf))) {
    }

    ImGui::SeparatorText("Eye Position");

    auto& rig = CameraRig::instance();

    f32 eye_sep = rig.eye_separation();
    if (ImGui::SliderFloat("Eye Separation (IPD)", &eye_sep, 0.040f, 0.090f, "%.4f m")) {
        rig.set_eye_separation(eye_sep);
        cfg.ipd = eye_sep;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##sep")) {
        rig.set_eye_separation(0.064f);
        cfg.ipd = 0.064f;
    }

    f32 conv = rig.convergence_distance();
    if (ImGui::SliderFloat("Convergence Distance", &conv, 0.5f, 20.0f, "%.1f m")) {
        rig.set_convergence_distance(conv);
        cfg.convergence_distance = conv;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##conv")) { rig.set_convergence_distance(5.0f); cfg.convergence_distance = 5.0f; }

    f32 eye_h = TrackingSystem::instance().eye_height();
    if (ImGui::SliderFloat("Eye Height", &eye_h, 0.0f, 2.5f, "%.2f m")) {
        TrackingSystem::instance().set_eye_height(eye_h);
        cfg.eye_height = eye_h;
    }

    ImGui::SeparatorText("World");

    f32 ws = rig.world_scale();
    if (ImGui::SliderFloat("World Scale", &ws, 0.25f, 4.0f, "%.2f")) {
        rig.set_world_scale(ws);
        cfg.world_scale = ws;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##ws")) { rig.set_world_scale(1.0f); cfg.world_scale = 1.0f; }

    ImGui::SeparatorText("Overlay");
    bool show_overlay = global.enable_overlay();
    if (ImGui::Checkbox("Show Config Overlay", &show_overlay)) {
        global.set_enable_overlay(show_overlay);
    }
    ImGui::Text("Toggle key: F2");
}

void ConfigEditor::draw_rendering_tab() {
    auto& cfg = Config::instance().current_profile();
    auto& renderer = StereoRenderer::instance();
    auto& latency = renderer.latency();
    auto& depth = renderer.depth();

    ImGui::SeparatorText("Render Settings");

    const char* modes[] = { "Reprojection (3D)", "Side-by-Side", "Over-Under",
                            "Interleaved", "Screen Capture" };
    int current_mode = static_cast<int>(renderer.mode());
    if (ImGui::Combo("Stereo Mode", &current_mode, modes, IM_ARRAYSIZE(modes))) {
        renderer.set_mode(static_cast<StereoRenderer::Mode>(current_mode));
    }

    ImGui::SliderFloat("Render Scale", &cfg.render_scale, 0.5f, 2.0f, "%.2f");

    bool dynamic_res = cfg.dynamic_resolution;
    if (ImGui::Checkbox("Dynamic Resolution", &dynamic_res)) {
        cfg.dynamic_resolution = dynamic_res;
        renderer.set_dynamic_resolution(dynamic_res);
    }

    bool foveated = cfg.foveated_rendering;
    if (ImGui::Checkbox("Foveated Rendering", &foveated)) {
        cfg.foveated_rendering = foveated;
    }

    float fov = cfg.fov_override;
    if (ImGui::SliderFloat("FOV Override", &fov, 0.0f, 140.0f, "%.0f")) {
        cfg.fov_override = fov;
    }

    bool vsync = renderer.vsync();
    if (ImGui::Checkbox("VSync", &vsync)) {
        renderer.set_vsync(vsync);
    }

    ImGui::Text("API: %s",
                Config::instance().detected_api() == GraphicsAPI::D3D11 ? "D3D11" :
                Config::instance().detected_api() == GraphicsAPI::D3D12 ? "D3D12" :
                Config::instance().detected_api() == GraphicsAPI::Vulkan ? "Vulkan" :
                Config::instance().detected_api() == GraphicsAPI::OpenGL ? "OpenGL" : "Unknown");

    ImGui::Text("Frames Rendered: %u", renderer.frame_count());

    ImGui::SeparatorText("Phase 2: Latency Compensation");

    bool latency_enabled = renderer.latency_compensation_enabled();
    if (ImGui::Checkbox("Latency Compensation (ATW)", &latency_enabled)) {
        renderer.set_latency_compensation_enabled(latency_enabled);
    }

    bool motion_smoothing = latency.motion_smoothing();
    if (ImGui::Checkbox("Motion Smoothing (Kalman)", &motion_smoothing)) {
        latency.set_motion_smoothing(motion_smoothing);
    }

    float pred_window = latency.prediction_window();
    if (ImGui::SliderFloat("Prediction Window (ms)", &pred_window, 5.0f, 60.0f, "%.1f")) {
        latency.set_prediction_window(pred_window);
    }

    f64 mtp = latency.current_motion_to_photon_ms();
    ImGui::Text("Motion-to-Photon: %.1f ms (smoothed)", mtp);

    if (mtp > 20.0) {
        ImGui::TextColored(ImVec4(1,0,0,1), "  Exceeds 20ms comfort threshold");
    }

    bool atw_needed = latency.needs_timewarp();
    ImGui::Text("ATW Status: %s", atw_needed ? "Active" : "Standby");

    ImGui::SeparatorText("Phase 2: Depth Reprojection");

    bool depth_avail = depth.is_initialized() && depth.depth_captured();
    ImGui::Text("Depth Buffer: %s", depth_avail ? "Captured" : "(unavailable)");
    ImGui::Text("Depth Reprojection: %s", depth.is_initialized() ? "Ready" : "Not initialized");
}

void ConfigEditor::draw_tracking_tab() {
    auto& tracking = TrackingSystem::instance();
    auto& cfg = Config::instance().current_profile();

    ImGui::SeparatorText("Head Tracking");

    bool tracking_enabled = cfg.enable_head_tracking;
    if (ImGui::Checkbox("Enable Head Tracking", &tracking_enabled)) {
        cfg.enable_head_tracking = tracking_enabled;
    }

    bool prediction = tracking.prediction_enabled();
    if (ImGui::Checkbox("Motion Prediction", &prediction)) {
        tracking.set_prediction_enabled(prediction);
    }

    ImGui::SliderFloat("IPD", &cfg.ipd, 0.054f, 0.074f, "%.4f m");
    tracking.set_ipd(cfg.ipd);

    ImGui::SeparatorText("Origin & Room-Scale");

    const char* origins[] = { "Local (Seated)", "Stage (Room-Scale)", "View (Head-Locked)" };
    int origin_idx = static_cast<int>(tracking.origin_mode());
    if (ImGui::Combo("Tracking Origin", &origin_idx, origins, IM_ARRAYSIZE(origins))) {
        tracking.set_origin_mode(static_cast<TrackingOrigin>(origin_idx));
    }

    f32 eye_h = tracking.eye_height();
    if (ImGui::SliderFloat("Eye Height (m)", &eye_h, 0.0f, 2.5f, "%.2f")) {
        tracking.set_eye_height(eye_h);
        cfg.eye_height = eye_h;
    }

    if (tracking.has_room_bounds()) {
        Vec2 bounds = tracking.room_bounds();
        ImGui::Text("Room Bounds: %.1f m x %.1f m", bounds.x, bounds.y);
    } else {
        ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "Room Bounds: Not set");
    }

    ImGui::SeparatorText("Tracking Status");
    ImGui::Text("Tracking: %s", tracking.is_tracking() ? "Active" : "Inactive");
    ImGui::Text("Confidence: %.2f", tracking.tracking_confidence());

    auto pose = tracking.get_head_pose();
    ImGui::Text("Position: (%.2f, %.2f, %.2f)",
                pose.position.x, pose.position.y, pose.position.z);
    ImGui::Text("Rotation: (%.2f, %.2f, %.2f, %.2f)",
                pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w);
    ImGui::Text("Timestamp: %.1f ms", pose.timestamp_ms);

    if (ImGui::Button("Recenter")) {
        tracking.recenter();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Origin")) {
        tracking.reset_origin();
    }
}

void ConfigEditor::draw_input_tab() {
    auto& mapper = InputMapper::instance();
    auto& proxy = InputProxy::instance();
    auto& cfg = Config::instance().current_profile();

    ImGui::SeparatorText("Input Mapping");
    ImGui::Text("Profile: %s", mapper.current_profile().name.c_str());

    ImGui::SliderFloat("Look Sensitivity", &mapper.current_profile().look_sensitivity,
                       0.1f, 5.0f, "%.1f");
    ImGui::SliderFloat("Movement Smoothing", &mapper.current_profile().movement_smoothing,
                       0.0f, 1.0f, "%.2f");

    bool head_aim = mapper.current_profile().head_aim;
    if (ImGui::Checkbox("Head Aim (look to aim)", &head_aim)) {
        mapper.current_profile().head_aim = head_aim;
    }

    bool snap_turn = mapper.current_profile().snap_turn;
    if (ImGui::Checkbox("Snap Turn", &snap_turn)) {
        mapper.current_profile().snap_turn = snap_turn;
    }

    float snap_angle = mapper.current_profile().snap_turn_angle;
    if (ImGui::SliderFloat("Snap Turn Angle", &snap_angle, 15.0f, 90.0f, "%.0f")) {
        mapper.current_profile().snap_turn_angle = snap_angle;
    }

    bool vignette = mapper.current_profile().vignette_on_move;
    if (ImGui::Checkbox("Vignette on Movement", &vignette)) {
        mapper.current_profile().vignette_on_move = vignette;
    }

    ImGui::SeparatorText("Active Input Actions");
    for (u32 i = 0; i < 20; i++) {
        InputAction action = static_cast<InputAction>(i);
        f32 val = proxy.get_action_value(action);
        if (val > 0.01f) {
            ImGui::Text("Action %u: %.2f", i, val);
        }
    }

    ImGui::SeparatorText("Profiles");
    auto profiles = mapper.available_profiles();
    for (auto& p : profiles) {
        if (ImGui::Button(p.c_str())) {
            mapper.load_profile(p);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

void ConfigEditor::draw_performance_tab() {
    auto& perf = PerfMonitor::instance();
    auto& latency = LatencyCompensator::instance();
    auto& cfg_latency = Config::instance().latency_stats();

    ImGui::SeparatorText("VR Performance Metrics");

    u32 fps = perf.fps();
    ImGui::Text("Frame Rate:    %u FPS", fps);

    ImGui::Text("CPU Work:       %.2f ms", perf.avg_cpu_ms());
    ImGui::Text("GPU Work:       %.2f ms", perf.avg_gpu_ms());
    ImGui::Text("Frame Time:     %.2f ms (avg)  %.2f min  %.2f max",
                perf.avg_frame_ms(), perf.min_frame_ms(), perf.max_frame_ms());
    ImGui::Text("99th Percentile: %.2f ms", perf.percentile_99_frame_ms());

    ImGui::SeparatorText("Motion-to-Photon Latency");

    f64 smoothed_mtp = latency.motion_to_photon_smoothed();
    ImGui::Text("Smoothed MTP:   %.1f ms", smoothed_mtp);
    ImGui::Text("Latest MTP:     %.1f ms", cfg_latency.total_motion_to_photon_ms);
    ImGui::Text("Present:        %.1f ms", cfg_latency.present_latency_ms);
    ImGui::Text("Render:         %.1f ms", cfg_latency.render_latency_ms);
    ImGui::Text("Tracking:       %.1f ms", cfg_latency.tracking_latency_ms);

    ImGui::SeparatorText("VR Comfort Checks");

    if (smoothed_mtp > 20.0) {
        ImGui::TextColored(ImVec4(1,0,0,1), "Motion-to-photon > 20ms (uncomfortable)");
    } else if (smoothed_mtp > 15.0) {
        ImGui::TextColored(ImVec4(1,1,0,1), "Motion-to-photon > 15ms (caution)");
    } else {
        ImGui::TextColored(ImVec4(0,1,0,1), "Motion-to-photon within comfort zone");
    }

    if (fps < 90) {
        ImGui::TextColored(ImVec4(1,0,0,1), "FPS below 90 (VR threshold)");
    } else {
        ImGui::TextColored(ImVec4(0,1,0,1), "FPS meets 90 VR threshold");
    }

    f32 dropped = perf.dropped_frames();
    ImGui::Text("Dropped Frames: %.1f%%", dropped * 100.0f);

    ImGui::Text("VR Comfort: %s", perf.is_comfortable() ? "Comfortable" : "Uncomfortable");

    // Frame time histogram (simplified bar display)
    ImGui::SeparatorText("Frame Time History");
    auto recent = perf.recent_frame_times(120);
    if (!recent.empty()) {
        f64 max_time = *std::max_element(recent.begin(), recent.end());
        f64 min_time = *std::min_element(recent.begin(), recent.end());
        f64 range = std::max(max_time - min_time, 1.0);

        // Draw mini bar chart using text
        ImGui::Text("  %5.1f ms  +" , max_time);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));

        char bars[121];
        u32 step = std::max(1u, static_cast<u32>(recent.size()) / 120);
        for (size_t i = 0; i < recent.size() && i < 120 * step; i += step) {
            f32 pct = static_cast<f32>((recent[i] - min_time) / range);
            u32 bar_len = static_cast<u32>(pct * 50.0f);
            bar_len = std::min(bar_len, 50u);
            memset(bars, '|', bar_len);
            bars[bar_len] = '\0';
            ImGui::Text("%s", bars);
        }
        ImGui::PopStyleColor();
        ImGui::Text("  %5.1f ms  -", min_time);
    }

    ImGui::SeparatorText("Recommendations");
    if (fps < 90) {
        ImGui::Text("- Enable Dynamic Resolution for GPU-bound scenarios");
        ImGui::Text("- Reduce Render Scale if FPS is below target");
    }
    if (smoothed_mtp > 20.0) {
        ImGui::Text("- Enable ATW / motion smoothing for latency reduction");
        ImGui::Text("- Disable VSync for lower motion-to-photon");
    }
    if (dropped > 0.05f) {
        ImGui::Text("- Consider lowering graphics settings to reduce dropped frames");
    }
}

} // namespace vrc
