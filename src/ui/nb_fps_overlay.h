// App-owned FPS display. The metric is completed NbCP IssueSwap cadence,
// not monitor refresh, UI redraw rate, or GPU fence completion.
#pragma once

#include <chrono>
#include <cstdint>

#include <rex/ui/imgui_dialog.h>

namespace nb::ui {

class NbFpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit NbFpsOverlay(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  using Clock = std::chrono::steady_clock;
  using SwapCountGetter = uint64_t (*)();
  const bool nb_backend_;
  SwapCountGetter get_swap_count_ = nullptr;
  Clock::time_point sample_start_{};
  Clock::time_point last_change_{};
  uint64_t sample_count_ = 0;
  uint64_t observed_count_ = 0;
  double fps_ = -1.0;
  bool initialized_ = false;
};

}  // namespace nb::ui
