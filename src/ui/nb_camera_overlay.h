// F10 camera/FOV overlay for ultrawide work (docs/ultrawide.md, "FOV tooling"). For every live camera it
// shows what is actually rendered: vertical and horizontal FOV, aspect, lens and viewport, read back from
// the projection the game built. It also shows the ultrawide state, the render scale in effect and the
// debug overrides.
#pragma once

#include <cstdint>

#include <rex/ui/imgui_dialog.h>

namespace nb::ui {

class NbCameraOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit NbCameraOverlay(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // rexgpu-nb's NbGetDrawResolutionScale, resolved on the UI thread once the plugin is loaded.
  using DrawScaleGetter = bool (*)(uint32_t*, uint32_t*);
  DrawScaleGetter get_draw_scale_ = nullptr;
};

}  // namespace nb::ui
