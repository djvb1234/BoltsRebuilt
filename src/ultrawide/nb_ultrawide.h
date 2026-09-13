// nb_ultrawide: Hor+ ultrawide for the recompiled game (docs/ultrawide.md). One setting, nb_ultrawide,
// drives four places that must agree:
// - the camera aspect hook and the XUI view squeeze (src/hooks/ultrawide_hooks.cpp);
// - the display aspect rexgpu-nb hands the presenter;
// - the render scale rexgpu-nb picks at startup.
// The plugin reaches the last two through the functions nb_ultrawide.cpp exports. With nb_ultrawide = "off",
// every one of them is a pass-through.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nb::uw {

// What the hooks apply to the current guest frame. The state is latched at the guest Swap
// (swap_hook.cpp), so the projection and the HUD change together at a frame boundary. The presenter's
// aspect travels with each swap packet (OnGuestSwapEnter), so the presenter shows each frame at the aspect
// it was rendered with.
struct FrameState {
  bool active = false;     // a wider-than-16:9 aspect is configured, usable and not suspended (F9)
  double k = 1.0;          // target aspect / (16/9): how much wider than the game's 16:9 the view is
  double kh = 1.0;         // 1 / k: the horizontal squeeze that keeps the HUD at 16:9 proportions
  bool hud_center = true;  // nb_ultrawide_hud: "center", or "stretch" to show the unfixed HUD
  uint32_t aspect_w = 16;  // display aspect for the presenter while active
  uint32_t aspect_h = 9;
};

// Guest-thread entry points, called from swap_hook.cpp around the original Swap. emits_packet says whether
// this Swap emits a swap packet (VdSwap runs and accepts the frontbuffer); frontbuffer is the physical
// address that packet carries, the frontbuffer_ptr rexgpu-nb's IssueSwap receives.
// OnGuestSwapEnter (before it): queue the presenter state this frame was rendered with.
void OnGuestSwapEnter(bool emits_packet, uint32_t frontbuffer);
// OnGuestSwap (after it): latch the next frame's state.
void OnGuestSwap();
// Instead of both, for a Swap that presents the previous image again (the D3D worker while the title has
// released the device, and two terminal loops): queue the state that image's own packet carried and latch
// nothing. The held image keeps its aspect, and an F9 press in that window applies from the title's next
// own frame.
void OnGuestRepresent(bool emits_packet, uint32_t frontbuffer);
FrameState Current();  // the latched state
double CurrentK();     // the latched k alone, for the hot aspect hook
uint64_t GuestSwapCount();

// Called from NbApp::OnPreSetup. The aspect needs rexgpu-nb's presenter seam, so with another GPU plugin
// the feature stays off rather than squashing the picture.
void SetPresenterAvailable(bool available);
bool PresenterAvailable();
// F9: a same-run A/B against 16:9. Takes effect at the next guest Swap. Returns the new suspended state.
bool ToggleSuspended();
bool Suspended();

// Debug camera controls (the nb_cam_* cvars), sanitised: a non-finite or out-of-range value means the
// game's own value (0 for the FOV, 1 for the scale).
double CamVfovOverride();
double CamFovScale();
double CamCullAspect();
bool CameraOverlayAtStart();

// The configured settings, for logs and the overlay.
struct Config {
  std::string aspect_text;     // nb_ultrawide as set
  bool aspect_valid = false;   // it parses to a wider-than-16:9 aspect
  bool aspect_invalid = false; // it is neither off nor a usable aspect (the log warns)
  double aspect = 16.0 / 9.0;
  uint32_t aspect_w = 16, aspect_h = 9;
  uint32_t scale_x = 1, scale_y = 1;  // the render scale nb_ultrawide_scale asks for
  bool scale_valid = true;            // false: nb_ultrawide_scale was not understood, so auto is used
};
Config ReadConfig();

// Camera snapshots from the meCalculateProjection hook, one per call site, for the F10 overlay.
struct CameraSnapshot {
  uint32_t caller = 0;       // guest return address: which meCalculateProjection call site
  uint64_t calls = 0;        // rebuilds seen from this site
  uint64_t last_swap = 0;    // guest swap count at the last rebuild
  int64_t last_ns = 0;       // host steady-clock time of the last rebuild, stamped by RecordCamera
  uint32_t camera = 0;       // [me+416]
  int32_t camera_mode = 0;   // cam+108: 0 = 4:3, 1 = 16:9, 2 = 2.35:1, 3 = follow the display mode me+616
  int32_t display_mode = 0;  // me+616
  float focal = 0.0f;        // cam+96; 23.0546665 marks the default lens
  float near_z = 0.0f;       // cam+100
  float far_z = 0.0f;        // cam+104
  float vfov_deg = 0.0f;     // me+612: the vertical FOV the projection used
  float m00 = 0.0f;          // the projection actually built at me+64
  float m11 = 0.0f;
  uint32_t viewport[4] = {};  // me+588: D3DVIEWPORT9 X, Y, Width, Height after meSetViewport
  float cam_ratio = 0.0f;     // cam+172: camera aspect / display aspect (1 = no letterbox)
  double k = 1.0;             // the latched k when it was built
};
void RecordCamera(const CameraSnapshot& snapshot);
size_t CopyCameras(CameraSnapshot* out, size_t capacity);

}  // namespace nb::uw
