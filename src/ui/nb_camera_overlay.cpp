#include "nb_camera_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "ultrawide/nb_ultrawide.h"
#include "ultrawide/nb_ultrawide_math.h"

namespace nb::ui {
namespace {

// cam+96 holds this sentinel for the default lens (the sub_822150D0 table at 0x82E5EE80), which gives 55 degrees.
constexpr float kDefaultLensFocal = 23.0546665f;
// A call site counts as live if it rebuilt its projection within the last second.
constexpr int64_t kLiveNs = 1'000'000'000;

const char* ModeName(int32_t mode) {
  switch (mode) {
    case 0: return "4:3";
    case 1: return "16:9";
    case 2: return "2.35:1";
    case 3: return "display";
    default: return "?";
  }
}

void DrawUltrawideLine(bool scale_known, uint32_t scale_x, uint32_t scale_y) {
  const nb::uw::Config config = nb::uw::ReadConfig();
  char scale_text[128];
  if (!scale_known) {
    std::snprintf(scale_text, sizeof(scale_text), "Render scale unknown.");
  } else if (config.aspect_valid && (scale_x != config.scale_x || scale_y != config.scale_y)) {
    std::snprintf(scale_text, sizeof(scale_text),
                  "Render scale %ux%u in effect (nb_ultrawide_scale asks %ux%u: set at start or overridden).",
                  scale_x, scale_y, config.scale_x, config.scale_y);
  } else {
    std::snprintf(scale_text, sizeof(scale_text), "Render scale %ux%u.", scale_x, scale_y);
  }
  if (!nb::uw::PresenterAvailable()) {
    ImGui::Text("Ultrawide: unavailable, it needs --gpu_plugin=nb. %s", scale_text);
    return;
  }
  if (config.aspect_invalid) {
    ImGui::Text("Ultrawide: '%s' not understood (off, or W:H / WxH wider than 16:9, at most 8:1); playing 16:9. %s",
                config.aspect_text.c_str(), scale_text);
    return;
  }
  if (!config.aspect_valid) {
    ImGui::Text("Ultrawide: off (16:9 as the game ships)%s. %s",
                nb::uw::Suspended() ? "; an F9 suspension is pending, F9 clears it" : "", scale_text);
    return;
  }
  const nb::uw::FrameState state = nb::uw::Current();
  const char* status = nb::uw::Suspended() ? "suspended with F9, showing 16:9" : state.active ? "on" : "starting";
  ImGui::Text("Ultrawide %u:%u (aspect %.3f, %.2fx the 16:9 view): %s. HUD %s. %s", config.aspect_w,
              config.aspect_h, config.aspect, config.aspect / nb::uw::kBaseAspect, status,
              state.hud_center ? "centred at 16:9" : "stretched", scale_text);
}

void DrawOverridesLine() {
  const double vfov = nb::uw::CamVfovOverride();
  const double scale = nb::uw::CamFovScale();
  const double cull = nb::uw::CamCullAspect();
  char vfov_text[32];
  char cull_text[32];
  if (vfov > 0.0) std::snprintf(vfov_text, sizeof(vfov_text), "%.1f deg", vfov);
  else std::snprintf(vfov_text, sizeof(vfov_text), "the game's");
  if (cull > 0.0) std::snprintf(cull_text, sizeof(cull_text), "%.3f", cull);
  else std::snprintf(cull_text, sizeof(cull_text), "follows the view");
  ImGui::Text("Debug overrides: vertical FOV %s, FOV scale %.3f, culling aspect %s", vfov_text, scale, cull_text);
}

void DrawCameras() {
  std::array<nb::uw::CameraSnapshot, 32> cameras;
  const size_t count = nb::uw::CopyCameras(cameras.data(), cameras.size());
  std::sort(cameras.begin(), cameras.begin() + count,
            [](const nb::uw::CameraSnapshot& a, const nb::uw::CameraSnapshot& b) { return a.calls > b.calls; });
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  const nb::uw::CameraSnapshot* busiest = nullptr;
  constexpr ImGuiTableFlags table_flags =
      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
  if (ImGui::BeginTable("##nb_cameras", 11, table_flags)) {
    for (const char* header : {"site", "mode", "vfov", "hfov", "aspect", "lens", "near", "far", "viewport", "ratio", "k"}) {
      ImGui::TableSetupColumn(header);
    }
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < count; ++i) {
      const nb::uw::CameraSnapshot& c = cameras[i];
      if (now - c.last_ns > kLiveNs) continue;
      if (!busiest) busiest = &c;
      // Ground truth is the matrix the game built: hfov = 2 atan(1/m00), vfov = 2 atan(1/m11).
      const double m00 = std::fabs(double(c.m00));
      const double m11 = std::fabs(double(c.m11));
      const double hfov = m00 > 0.0 ? nb::uw::RadiansToDegrees(2.0 * std::atan(1.0 / m00)) : 0.0;
      const double vfov = m11 > 0.0 ? nb::uw::RadiansToDegrees(2.0 * std::atan(1.0 / m11)) : 0.0;
      const double aspect = m00 > 0.0 ? m11 / m00 : 0.0;
      const bool default_lens = std::fabs(c.focal - kDefaultLensFocal) < 1e-3f;
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%08X", c.caller);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(ModeName(c.camera_mode));
      ImGui::TableNextColumn(); ImGui::Text("%.1f", vfov);
      ImGui::TableNextColumn(); ImGui::Text("%.1f", hfov);
      ImGui::TableNextColumn(); ImGui::Text("%.3f", aspect);
      ImGui::TableNextColumn(); ImGui::TextUnformatted(default_lens ? "default" : "custom");
      ImGui::TableNextColumn(); ImGui::Text("%.2f", c.near_z);
      ImGui::TableNextColumn(); ImGui::Text("%.0f", c.far_z);
      ImGui::TableNextColumn(); ImGui::Text("%u,%u %ux%u", c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3]);
      ImGui::TableNextColumn(); ImGui::Text("%.3f", c.cam_ratio);
      ImGui::TableNextColumn(); ImGui::Text("%.2f", c.k);
    }
    ImGui::EndTable();
  }
  if (!busiest) {
    ImGui::TextUnformatted("(no camera rebuilt its projection in the last second)");
    return;
  }
  const double v = busiest->vfov_deg;
  ImGui::Text("Busiest camera: vertical FOV %.1f deg. Horizontal FOV at 16:9 %.1f, 21:9 %.1f, 32:9 %.1f deg.", v,
              nb::uw::HorizontalFovDeg(v, 16.0 / 9.0), nb::uw::HorizontalFovDeg(v, 21.0 / 9.0),
              nb::uw::HorizontalFovDeg(v, 32.0 / 9.0));
}

}  // namespace

NbCameraOverlay::NbCameraOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

void NbCameraOverlay::OnDraw(ImGuiIO& io) {
#ifdef _WIN32
  // UI-thread lookup only, like the FPS overlay. NbApp destroys this dialog before the plugin unloads.
  if (!get_draw_scale_) {
    if (HMODULE module = GetModuleHandleW(L"rexgpu-nb.dll")) {
      get_draw_scale_ = reinterpret_cast<DrawScaleGetter>(
          reinterpret_cast<void*>(GetProcAddress(module, "NbGetDrawResolutionScale")));
    }
  }
#endif
  uint32_t scale_x = 0, scale_y = 0;
  const bool scale_known = get_draw_scale_ && get_draw_scale_(&scale_x, &scale_y);

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 12.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.7f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##nb_camera_overlay", nullptr, flags)) {
    DrawUltrawideLine(scale_known, scale_x, scale_y);
    DrawOverridesLine();
    ImGui::Separator();
    DrawCameras();
    ImGui::Separator();
    ImGui::TextUnformatted(
        "F10 hide  |  F9 ultrawide against 16:9 (same run)  |  nb_cam_vfov, nb_cam_fov_scale, nb_cam_cull_aspect: "
        "F4 settings or the console");
  }
  ImGui::End();
}

}  // namespace nb::ui
