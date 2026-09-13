#include "nb_fps_overlay.h"

#include <imgui.h>
#include <rex/cvar.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nb::ui {

NbFpsOverlay::NbFpsOverlay(rex::ui::ImGuiDrawer* drawer)
    : ImGuiDialog(drawer), nb_backend_(rex::cvar::GetFlagByName("gpu_plugin") == "nb") {}

void NbFpsOverlay::OnDraw(ImGuiIO& io) {
#ifdef _WIN32
  // UI-thread lookup only. NbApp destroys this dialog before the SDK destroys
  // its drawer/runtime and unloads the plugin; no CP object escapes the DLL.
  if (nb_backend_ && !get_swap_count_) {
    if (HMODULE module = GetModuleHandleW(L"rexgpu-nb.dll")) {
      get_swap_count_ = reinterpret_cast<SwapCountGetter>(
          GetProcAddress(module, "NbGetCompletedSwapCount"));
    }
  }
#endif
  if (get_swap_count_) {
    const auto now = Clock::now();
    const uint64_t count = get_swap_count_();
    if (!initialized_) {
      sample_start_ = last_change_ = now;
      sample_count_ = observed_count_ = count;
      initialized_ = true;
    }
    if (count != observed_count_) {
      observed_count_ = count;
      last_change_ = now;
    }
    const double seconds = std::chrono::duration<double>(now - sample_start_).count();
    if (seconds >= 0.5) {
      fps_ = count ? double(count - sample_count_) / seconds : -1.0;
      sample_start_ = now;
      sample_count_ = count;
    }
    // Clear a stale high reading even if the UI keeps painting during a game
    // stall. No timestamps, timers, or extra work are added to individual draws.
    if (count && std::chrono::duration<double>(now - last_change_).count() >= 0.5) {
      fps_ = 0.0;
    }
  }

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f),
                          ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##nb_game_fps", nullptr, flags)) {
    if (!nb_backend_ || !get_swap_count_) ImGui::TextUnformatted("Game FPS: n/a");
    else if (fps_ < 0.0) ImGui::TextUnformatted("Game FPS: --");
    else ImGui::Text("Game FPS: %.1f", fps_);
  }
  ImGui::End();
}

}  // namespace nb::ui
