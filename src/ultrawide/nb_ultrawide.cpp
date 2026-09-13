#include "ultrawide/nb_ultrawide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ultrawide/nb_ultrawide_math.h"

REXCVAR_DEFINE_STRING(nb_ultrawide, "off", "nb_ultrawide",
                      "Ultrawide display aspect: off, W:H (32:9, 21:9) or WxH (5120x1440). Hor+ view with the "
                      "HUD at 16:9 proportions in the middle; needs --gpu_plugin=nb and a fullscreen window "
                      "of that aspect (docs/ultrawide.md)");
REXCVAR_DEFINE_STRING(nb_ultrawide_hud, "center", "nb_ultrawide",
                      "HUD while ultrawide: center (16:9 proportions in the middle) or stretch (the unfixed "
                      "2:1 look, for comparison)")
    .allowed({"center", "stretch"});
REXCVAR_DEFINE_STRING(nb_ultrawide_scale, "auto", "nb_ultrawide",
                      "Render scale while ultrawide: auto (2x2 at 32:9, 1440 lines) or "
                      "SxT, e.g. 4x2 for a native 5120x1440 from the 1280x720 guest. Any explicit "
                      "draw_resolution_scale_x/y or resolution_scale, 1 included, wins. Takes effect at the "
                      "next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(nb_cam_vfov, 0.0, "nb_ultrawide",
                      "Debug: vertical FOV in degrees for every camera (0 = the game's own)")
    .range(0.0, 170.0);
REXCVAR_DEFINE_DOUBLE(nb_cam_fov_scale, 1.0, "nb_ultrawide",
                      "Debug: multiplies tan(vfov/2) for every camera (1 = the game's own)")
    .range(0.1, 4.0);
REXCVAR_DEFINE_DOUBLE(nb_cam_cull_aspect, 0.0, "nb_ultrawide",
                      "Debug: frustum-culling aspect (0 = follow the view; 1.7778 drops edge objects at 32:9, 7 "
                      "culls wider than the view)")
    .range(0.0, 20.0);
REXCVAR_DEFINE_BOOL(nb_camera_overlay, false, "nb_ultrawide", "Show the F10 camera/FOV overlay at start");

namespace {

constexpr uint64_t kPresent16x9 = (uint64_t(9) << 32) | 16;  // PackPresent(false, 16, 9)

std::atomic<bool> g_presenter_available{true};
std::atomic<bool> g_suspended{false};
std::atomic<uint64_t> g_swaps{0};

// Latched once per guest frame. k, kh and the HUD mode are read only on the guest thread's own frame. The
// presenter's triple is one packed word, because rexgpu-nb's command-processor thread reads it.
std::atomic<double> g_k{1.0};
std::atomic<double> g_kh{1.0};
std::atomic<bool> g_hud_center{true};
std::atomic<uint64_t> g_present{kPresent16x9};

// The presenter state of each emitted swap packet, oldest first, tagged with the frontbuffer address the
// packet carries. It is pushed at the guest Swap with the state that frame was rendered with, and taken by
// rexgpu-nb when its command processor reaches that packet. The D3D worker thread also calls Swap, hence
// the mutex.
struct PresentEntry {
  uint32_t frontbuffer = 0;
  uint64_t word = kPresent16x9;
};
std::mutex g_present_mutex;
std::array<PresentEntry, 16> g_present_queue{};
size_t g_present_head = 0;
size_t g_present_count = 0;
uint64_t g_present_last_pushed = kPresent16x9;  // what a re-present of the last image repeats
uint64_t g_present_last = kPresent16x9;         // the last state taken
bool g_present_taken = false;
uint64_t g_present_resyncs = 0;                 // entries dropped because their packet never arrived

// Only touched where the state is latched: on the guest threads at Swap, and in OnPreSetup before that.
std::mutex g_latch_mutex;
bool g_logged = false;
nb::uw::FrameState g_last_logged;
std::string g_last_text;

std::mutex g_camera_mutex;
std::array<nb::uw::CameraSnapshot, 32> g_cameras;
size_t g_camera_count = 0;

struct Computed {
  nb::uw::FrameState state;
  std::string text;
  bool invalid = false;
};

// String cvars are read through the registry. GetFlagByName copies them under the registry mutex that the
// F4 settings overlay and the console also write under, so a live edit cannot race the guest thread.
Computed Compute() {
  Computed c;
  c.text = rex::cvar::GetFlagByName("nb_ultrawide");
  nb::uw::Aspect aspect;
  const bool parsed = nb::uw::ParseAspect(c.text, &aspect);
  c.invalid = !parsed && !nb::uw::IsOffToken(c.text);
  if (parsed && g_presenter_available.load(std::memory_order_relaxed) &&
      !g_suspended.load(std::memory_order_relaxed)) {
    c.state.active = true;
    c.state.k = aspect.ratio / nb::uw::kBaseAspect;
    c.state.kh = 1.0 / c.state.k;
    c.state.aspect_w = aspect.w;
    c.state.aspect_h = aspect.h;
  }
  c.state.hud_center = nb::uw::detail::Normalize(rex::cvar::GetFlagByName("nb_ultrawide_hud")) != "stretch";
  return c;
}

void Latch() {
  const Computed c = Compute();
  const nb::uw::FrameState& s = c.state;
  g_k.store(s.k, std::memory_order_relaxed);
  g_kh.store(s.kh, std::memory_order_relaxed);
  g_hud_center.store(s.hud_center, std::memory_order_relaxed);
  g_present.store(nb::uw::PackPresent(s.active, s.aspect_w, s.aspect_h), std::memory_order_relaxed);

  std::lock_guard lock(g_latch_mutex);
  if (!g_logged || c.text != g_last_text) {
    if (c.invalid) {
      REXLOG_WARN("nb: nb_ultrawide '{}' not understood (off, or W:H / WxH wider than 16:9 and at most 8:1); "
                  "playing 16:9", c.text);
    }
    g_last_text = c.text;
  }
  if (g_logged && s.active == g_last_logged.active && s.aspect_w == g_last_logged.aspect_w &&
      s.aspect_h == g_last_logged.aspect_h && s.hud_center == g_last_logged.hud_center) {
    return;
  }
  g_logged = true;
  g_last_logged = s;
  if (s.active) {
    REXLOG_INFO("nb: ultrawide on at guest swap {}: display aspect {}:{}, view {:.3f}x wider than 16:9, HUD {}",
                g_swaps.load(std::memory_order_relaxed), s.aspect_w, s.aspect_h, s.k,
                s.hud_center ? "centred at 16:9" : "stretched");
  } else {
    REXLOG_INFO("nb: ultrawide off at guest swap {}{}", g_swaps.load(std::memory_order_relaxed),
                g_suspended.load(std::memory_order_relaxed) ? " (suspended with F9: 16:9 view)" : "");
  }
}

int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Under g_present_mutex.
void PushPresent(uint32_t frontbuffer, uint64_t word) {
  if (g_present_count == g_present_queue.size()) {
    // The command processor is 16 swaps behind. Drop the oldest entry rather than grow; the take
    // resynchronises on the frontbuffer tags.
    g_present_head = (g_present_head + 1) % g_present_queue.size();
    --g_present_count;
  }
  g_present_queue[(g_present_head + g_present_count) % g_present_queue.size()] = {frontbuffer, word};
  ++g_present_count;
  g_present_last_pushed = word;
}

}  // namespace

namespace nb::uw {

void OnGuestSwapEnter(bool emits_packet, uint32_t frontbuffer) {
  if (!emits_packet) return;
  const uint64_t word = g_present.load(std::memory_order_relaxed);
  std::lock_guard lock(g_present_mutex);
  PushPresent(frontbuffer, word);
}

void OnGuestRepresent(bool emits_packet, uint32_t frontbuffer) {
  if (!emits_packet) return;
  std::lock_guard lock(g_present_mutex);
  PushPresent(frontbuffer, g_present_last_pushed);
}

void OnGuestSwap() {
  g_swaps.fetch_add(1, std::memory_order_relaxed);
  Latch();
}

FrameState Current() {
  FrameState s;
  s.active = UnpackPresent(g_present.load(std::memory_order_relaxed), &s.aspect_w, &s.aspect_h);
  s.k = g_k.load(std::memory_order_relaxed);
  s.kh = g_kh.load(std::memory_order_relaxed);
  s.hud_center = g_hud_center.load(std::memory_order_relaxed);
  return s;
}

double CurrentK() { return g_k.load(std::memory_order_relaxed); }

uint64_t GuestSwapCount() { return g_swaps.load(std::memory_order_relaxed); }

void SetPresenterAvailable(bool available) {
  g_presenter_available.store(available, std::memory_order_relaxed);
  // Latch now, so the first frame already renders with the configured aspect.
  Latch();
}

bool PresenterAvailable() { return g_presenter_available.load(std::memory_order_relaxed); }

bool ToggleSuspended() {
  // Only the UI thread's F9 bind writes this.
  const bool suspended = !g_suspended.load(std::memory_order_relaxed);
  g_suspended.store(suspended, std::memory_order_relaxed);
  return suspended;
}

bool Suspended() { return g_suspended.load(std::memory_order_relaxed); }

// Command-line values skip the cvars' declared ranges, so the hooks get sanitised values.
double CamVfovOverride() {
  const double v = REXCVAR_GET(nb_cam_vfov);
  return std::isfinite(v) ? std::clamp(v, 0.0, 170.0) : 0.0;
}
double CamFovScale() {
  const double v = REXCVAR_GET(nb_cam_fov_scale);
  return std::isfinite(v) ? std::clamp(v, 0.1, 4.0) : 1.0;
}
double CamCullAspect() {
  const double v = REXCVAR_GET(nb_cam_cull_aspect);
  return std::isfinite(v) ? std::clamp(v, 0.0, 20.0) : 0.0;
}
bool CameraOverlayAtStart() { return REXCVAR_GET(nb_camera_overlay); }

Config ReadConfig() {
  Config c;
  c.aspect_text = rex::cvar::GetFlagByName("nb_ultrawide");
  Aspect aspect;
  c.aspect_valid = ParseAspect(c.aspect_text, &aspect);
  c.aspect_invalid = !c.aspect_valid && !IsOffToken(c.aspect_text);
  if (c.aspect_valid) {
    c.aspect = aspect.ratio;
    c.aspect_w = aspect.w;
    c.aspect_h = aspect.h;
  }
  const double k = c.aspect_valid ? aspect.ratio / kBaseAspect : 1.0;
  c.scale_valid = ParseScale(rex::cvar::GetFlagByName("nb_ultrawide_scale"), k, &c.scale_x, &c.scale_y);
  if (!c.scale_valid) ParseScale("auto", k, &c.scale_x, &c.scale_y);
  return c;
}

void RecordCamera(const CameraSnapshot& snapshot) {
  const int64_t now = SteadyNowNs();
  std::lock_guard lock(g_camera_mutex);
  for (size_t i = 0; i < g_camera_count; ++i) {
    if (g_cameras[i].caller == snapshot.caller) {
      const uint64_t calls = g_cameras[i].calls + 1;
      g_cameras[i] = snapshot;
      g_cameras[i].calls = calls;
      g_cameras[i].last_ns = now;
      return;
    }
  }
  if (g_camera_count < g_cameras.size()) {
    g_cameras[g_camera_count] = snapshot;
    g_cameras[g_camera_count].calls = 1;
    g_cameras[g_camera_count].last_ns = now;
    ++g_camera_count;
  }
}

size_t CopyCameras(CameraSnapshot* out, size_t capacity) {
  std::lock_guard lock(g_camera_mutex);
  const size_t n = std::min(capacity, g_camera_count);
  std::copy_n(g_cameras.begin(), n, out);
  return n;
}

}  // namespace nb::uw

// rexgpu-nb resolves these from nb.exe with GetProcAddress (src/gpu/native/nb_ultrawide_bridge.cpp). An
// older nb.exe without them simply leaves the plugin at 16:9.

// rexgpu-nb calls this once per swap packet, at the top of its IssueSwap, with the frontbuffer address the
// packet carries. It returns the presenter state of the frame that packet presents.
// - Entries before the first one with that address belong to packets that never reached the command
//   processor, so they are dropped. The title alternates two frontbuffers, so a lost packet shows up as a
//   mismatch at the very next one.
// - With no matching entry the head is taken, as a plain queue would.
// - With nothing queued (a packet the guest hook did not see) it repeats the last state taken, or the
//   latched one before the first.
// The 2 in the name marks the signature change: an older plugin finds no symbol and stays at 16:9 instead
// of calling it with the wrong arguments.
extern "C" __declspec(dllexport) bool NbUltrawideTakePresentAspect2(uint32_t frontbuffer, uint32_t* width,
                                                                     uint32_t* height) {
  if (!width || !height) return false;
  uint64_t word;
  size_t dropped = 0;
  uint64_t resyncs = 0;
  {
    std::lock_guard lock(g_present_mutex);
    if (g_present_count) {
      for (size_t i = 0; i < g_present_count; ++i) {
        if (g_present_queue[(g_present_head + i) % g_present_queue.size()].frontbuffer == frontbuffer) {
          dropped = i;
          break;
        }
      }
      g_present_head = (g_present_head + dropped) % g_present_queue.size();
      g_present_count -= dropped;
      word = g_present_queue[g_present_head].word;
      g_present_head = (g_present_head + 1) % g_present_queue.size();
      --g_present_count;
      g_present_last = word;
      g_present_taken = true;
      g_present_resyncs += dropped;
      resyncs = g_present_resyncs;
    } else {
      word = g_present_taken ? g_present_last : g_present.load(std::memory_order_relaxed);
    }
  }
  if (dropped && (resyncs <= 16 || resyncs % 1024 < dropped)) {
    REXLOG_WARN("nb: ultrawide presenter queue resynchronised at frontbuffer 0x{:08X}: dropped {} entries whose "
                "swap packet never reached the command processor ({} so far)",
                frontbuffer, dropped, resyncs);
  }
  return nb::uw::UnpackPresent(word, width, height);
}

// Asked once, when the plugin sizes its render targets, and only when no draw resolution scale is set at
// all. Suspension (F9) is a runtime A/B and does not change the scale: the presenter fits either state from
// the same scaled render.
extern "C" __declspec(dllexport) bool NbUltrawideDefaultScale(uint32_t* scale_x, uint32_t* scale_y) {
  if (!scale_x || !scale_y || !g_presenter_available.load(std::memory_order_relaxed)) return false;
  const nb::uw::Config c = nb::uw::ReadConfig();
  if (!c.aspect_valid) return false;
  if (!c.scale_valid) {
    REXLOG_WARN("nb: nb_ultrawide_scale '{}' not understood (auto or SxT with 1..7); using auto",
                rex::cvar::GetFlagByName("nb_ultrawide_scale"));
  }
  *scale_x = c.scale_x;
  *scale_y = c.scale_y;
  REXLOG_INFO("nb: ultrawide render scale {}x{} requested for display aspect {}:{}", c.scale_x, c.scale_y,
              c.aspect_w, c.aspect_h);
  return c.scale_x > 1 || c.scale_y > 1;
}
