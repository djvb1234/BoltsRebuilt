// nb - Banjo-Kazooie: Nuts & Bolts (PAL) on RexGlue
//
// App class: path configuration and the hooks the runtime exposes to us.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <toml++/toml.hpp>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

#include "audio/nb_audio_system.h"
#include "input/nb_scripted_input.h"
#include "ui/nb_camera_overlay.h"
#include "ui/nb_fps_overlay.h"
#include "ultrawide/nb_ultrawide.h"
#include "ultrawide/nb_ultrawide_math.h"

// Audio backend switch. "sdl" is SDL with a silent fallback when the machine has no playback device
// (without it the game dereferences a null pointer during boot). "nop" is the SDK's silent system, which
// refuses to create a driver at all; it exists to bisect the heap corruption that appears around the
// game's audio re-initialization (docs/known_issues.md).
REXCVAR_DEFINE_STRING(nb_audio, "sdl", "nb", "Audio backend: sdl (default, silent when no device) or nop");

// A scripted controller for reproducible runs: "<seconds>:<keys>,..." (see src/input/nb_scripted_input.h).
// When set, it is the only input device, so a run drives itself the same way every time.
REXCVAR_DEFINE_STRING(nb_input_script, "", "nb",
                      "Scripted controller schedule, e.g. \"34:A,34.2:,38:START,38.2:\" (empty = real input)");
REXCVAR_DEFINE_BOOL(nb_show_fps, true, "nb",
                   "Show completed game command-processor Swap cadence; F8 toggles the FPS overlay");

// A guest vblank rate for one launch, for play-testing another refresh rate without editing
// nb.toml. It has to be separate from video_mode_refresh_rate because ApplyConfiguredRefreshRate
// re-reads nb.toml over any command-line value (see there).
REXCVAR_DEFINE_DOUBLE(nb_refresh_rate_override, 0.0, "nb",
                      "Guest vblank rate in Hz for this launch only (24-480); 0 applies video_mode_refresh_rate from nb.toml")
    .range(0.0, 480.0);

class NbApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const auto key = e.virtual_key();
    if (key == rex::ui::VirtualKey::kF5 || key == rex::ui::VirtualKey::kF6 ||
        key == rex::ui::VirtualKey::kF8 || key == rex::ui::VirtualKey::kF9 ||
        key == rex::ui::VirtualKey::kF10) {
      // Keybind callbacks do not receive the event, so suppress held-key
      // repeats here. Only this app's focused window may queue debug toggles.
      if (e.prev_state() || !e.target() || !e.target()->HasFocus()) {
        e.set_handled(true);
        return;
      }
    }
    // ReXApp's private override dispatches to this public registry API.
    rex::ui::ProcessKeyEvent(e);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    fps_drawer_ = drawer;
    try {
      rex::ui::RegisterBind("bind_nb_fps", "F8", "Toggle game FPS overlay", [this] {
        const bool show = !fps_overlay_;
        if (show) fps_overlay_ = std::make_unique<nb::ui::NbFpsOverlay>(fps_drawer_);
        else fps_overlay_.reset();
        REXCVAR_SET(nb_show_fps, show);
        REXLOG_INFO("nb: F8 FPS overlay {} (completed command-processor Swap cadence)",
                    show ? "ON" : "OFF");
      });
      if (REXCVAR_GET(nb_show_fps)) {
        fps_overlay_ = std::make_unique<nb::ui::NbFpsOverlay>(drawer);
      }
      // Ultrawide tooling (docs/ultrawide.md): F10 shows what every live camera renders, F9 flips the
      // same run between the configured ultrawide aspect and the game's 16:9.
      rex::ui::RegisterBind("bind_nb_camera", "F10", "Toggle camera/FOV overlay", [this] {
        if (camera_overlay_) camera_overlay_.reset();
        else camera_overlay_ = std::make_unique<nb::ui::NbCameraOverlay>(fps_drawer_);
        REXLOG_INFO("nb: F10 camera overlay {}", camera_overlay_ ? "ON" : "OFF");
      });
      rex::ui::RegisterBind("bind_nb_ultrawide_ab", "F9", "Toggle ultrawide against 16:9 in the same run", [] {
        // Without a usable aspect there is nothing to flip. A suspension left from before nb_ultrawide was
        // turned off is cleared instead, so a later re-enable does not silently stay at 16:9.
        if (!nb::uw::PresenterAvailable() || !nb::uw::ReadConfig().aspect_valid) {
          if (nb::uw::Suspended()) {
            nb::uw::ToggleSuspended();
            REXLOG_INFO("nb: F9 cleared the pending ultrawide suspension (nb_ultrawide is off)");
          } else {
            REXLOG_INFO("nb: F9 ignored: nb_ultrawide is off or needs --gpu_plugin=nb");
          }
          return;
        }
        const bool suspended = nb::uw::ToggleSuspended();
        REXLOG_INFO("nb: F9 ultrawide {} from the next guest frame",
                    suspended ? "suspended (16:9 view)" : "resumed");
      });
      if (nb::uw::CameraOverlayAtStart()) {
        camera_overlay_ = std::make_unique<nb::ui::NbCameraOverlay>(drawer);
      }
    } catch (...) {
      rex::ui::UnregisterBind("bind_nb_fps");
      rex::ui::UnregisterBind("bind_nb_camera");
      rex::ui::UnregisterBind("bind_nb_ultrawide_ab");
      fps_overlay_.reset();
      camera_overlay_.reset();
      fps_drawer_ = nullptr;
      throw;
    }
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_nb_fps");
    rex::ui::UnregisterBind("bind_nb_camera");
    rex::ui::UnregisterBind("bind_nb_ultrawide_ab");
    fps_overlay_.reset();
    camera_overlay_.reset();
    fps_drawer_ = nullptr;
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    ApplyConfiguredRefreshRate();
    ConfigureUltrawide();
    if (REXCVAR_GET(nb_audio) == "nop") {
      REXLOG_WARN("nb: audio backend forced to nop (nb_audio=nop)");
      config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
    } else {
      config.audio_factory = REX_AUDIO_BACKEND(nb::audio::NbAudioSystem);
    }
    const std::string script = REXCVAR_GET(nb_input_script);
    if (!script.empty()) {
      REXLOG_WARN("nb: scripted controller in use; real controllers and the keyboard are ignored");
      config.input_factory = [script](bool) -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = std::make_unique<rex::input::InputSystem>(nullptr);
        auto driver = std::make_unique<nb::input::ScriptedInputDriver>(nullptr, 0, script);
        driver->Setup();
        input->AddDriver(std::move(driver));
        input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
        return input;
      };
    }
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NbApp>(new NbApp(ctx, "nb", PPCImageConfig));
  }


  // nb.toml's video_mode_refresh_rate never reaches the game through the SDK's loader. The SDK declares
  // that cvar with range(24, 240), and LoadConfig rejects anything outside it, 288 included, which
  // would leave the game at its stock 30 fps. REXCVAR_SET skips the range check. This runs before the
  // graphics system starts its vblank worker, which reads the rate once. The guest presents on every
  // second vblank, so its frame rate is half this value.
  static void ApplyConfiguredRefreshRate() {
    // nb_refresh_rate_override (playtest-native.ps1 -RefreshRate) wins for one launch. Command-line
    // values skip the cvar's declared range, so this clamp is the real guard. The kernel clamps the
    // guest video mode to the same 24..480 Hz (sdk-patches/0007).
    if (const double override_hz = REXCVAR_GET(nb_refresh_rate_override); override_hz > 0.0) {
      const double rate = std::clamp(override_hz, 24.0, 480.0);
      REXCVAR_SET(video_mode_refresh_rate, rate);
      REXLOG_INFO("nb: guest vblank set to {} Hz from nb_refresh_rate_override; nb.toml's video_mode_refresh_rate "
                  "ignored for this launch (the game presents every second one)", rate);
      return;
    }
    const std::filesystem::path config = std::filesystem::path(rex::filesystem::GetExecutablePath()).parent_path() / "nb.toml";
    if (!std::filesystem::exists(config)) return;
    try {
      auto parsed = toml::parse_file(config.string());
      if (auto rate = parsed["video_mode_refresh_rate"].value<double>(); rate && *rate > 0.0) {
        REXCVAR_SET(video_mode_refresh_rate, *rate);
        REXLOG_INFO("nb: guest vblank set to {} Hz from nb.toml (the game presents every second one)", *rate);
      }
    } catch (const toml::parse_error&) {
      // Reported by the SDK's own loader a moment later.
    }
  }

  // nb_ultrawide widens the view in guest hooks, but only rexgpu-nb can tell the presenter the frame is
  // 32:9. With the stock plugin the picture would stay 16:9, i.e. squashed, so the feature stays off there,
  // and says so. Runs before the plugin exists, so its render scale query already sees the result.
  static void ConfigureUltrawide() {
    const bool nb_plugin = rex::cvar::GetFlagByName("gpu_plugin") == "nb";
    nb::uw::SetPresenterAvailable(nb_plugin);
    const nb::uw::Config config = nb::uw::ReadConfig();
    if (!config.aspect_valid) return;
    if (!nb_plugin) {
      REXLOG_WARN("nb: nb_ultrawide {}:{} ignored: it needs --gpu_plugin=nb", config.aspect_w, config.aspect_h);
      return;
    }
    REXLOG_INFO("nb: ultrawide {}:{} configured: view {:.3f}x wider than 16:9, render scale {}x{} unless "
                "draw_resolution_scale_x/y are set (docs/ultrawide.md)",
                config.aspect_w, config.aspect_h, config.aspect / nb::uw::kBaseAspect, config.scale_x,
                config.scale_y);
  }

  // The SDK reads the game_data_root cvar before it loads <exe_dir>/nb.toml
  // (ReXApp::SetupEnvironment builds the PathConfig first, then calls LoadConfig),
  // so a game_data_root written in nb.toml never reaches the runtime. Resolve it
  // here from, in order: the command line (already in paths), the NB_GAME_ROOT
  // environment variable, nb.toml itself, and a `game` folder next to the exe.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;

    if (const char* env = std::getenv("NB_GAME_ROOT"); env && *env) {
      paths.game_data_root = env;
      return;
    }

    if (std::filesystem::exists(paths.config_path)) {
      try {
        auto config = toml::parse_file(paths.config_path.string());
        if (auto value = config["game_data_root"].value<std::string>(); value && !value->empty()) {
          paths.game_data_root = *value;
          return;
        }
      } catch (const toml::parse_error&) {
        // A broken nb.toml is reported by the SDK's own loader a moment later.
      }
    }

    auto sibling = paths.config_path.parent_path() / "game";
    if (std::filesystem::is_directory(sibling)) paths.game_data_root = sibling;
  }

 private:
  rex::ui::ImGuiDrawer* fps_drawer_ = nullptr;
  std::unique_ptr<nb::ui::NbFpsOverlay> fps_overlay_;
  std::unique_ptr<nb::ui::NbCameraOverlay> camera_overlay_;
};
