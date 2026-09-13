// nb - a scripted controller, so a run can drive itself past the menus.
//
// The SDK has no input replay (plan Phase 1.6) and comparing the native renderer against the emulated
// one meant looking at whatever the title screen's camera happened to be doing. This driver reports one
// synthetic pad whose state comes from a schedule: "34:A,34.2:,37:A,37.2:" holds A from 34 to 34.2
// seconds, and so on. Times are seconds since the driver was set up, which is close enough to the
// process start for the screenshot harness to line up with.
//
// Entries are `<seconds>:<keys>`; keys are '+'-separated and case-insensitive: A B X Y START BACK
// LB RB LS RS UP DOWN LEFT RIGHT, plus LX=<n>, LY=<n>, RX=<n>, RY=<n> for the sticks (-32768..32767),
// and LT=<n>, RT=<n> for the analog triggers (0..255).
// An empty key list releases everything.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/ui/virtual_key.h>

namespace nb::input {

// The X_ERROR_* and X_STATUS_* constants are macros naming these types unqualified.
using rex::X_RESULT;
using rex::X_STATUS;

class ScriptedInputDriver : public rex::input::InputDriver {
 public:
  ScriptedInputDriver(rex::ui::Window* window, size_t window_z_order, const std::string& script);

  // Parses the schedule; returns false (and logs) if an entry is malformed.
  static bool Parse(const std::string& script, std::vector<struct ScriptedStep>& out);

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                      rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  // Queues the key-down and key-up events between two button states.
  void QueueKeystrokes(uint16_t before, uint16_t after);

  std::vector<struct ScriptedStep> steps_;
  std::chrono::steady_clock::time_point start_{};
  size_t last_step_ = SIZE_MAX;
  uint16_t last_buttons_ = 0;
  uint32_t packet_number_ = 1;
  std::deque<std::pair<rex::ui::VirtualKey, uint16_t>> keystrokes_;  // key, flags
};

struct ScriptedStep {
  double at_seconds = 0.0;
  uint16_t buttons = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  std::string text;  // for the log line when the step fires
};

}  // namespace nb::input
