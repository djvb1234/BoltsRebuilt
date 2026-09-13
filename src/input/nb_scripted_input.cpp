// See nb_scripted_input.h.

#include "nb_scripted_input.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

namespace nb::input {

namespace {

using rex::input::DeviceId;

// One device, so its handle is a constant.
constexpr DeviceId kScriptedDevice = static_cast<DeviceId>(0x4E425343);  // 'NBSC'

std::string Upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return text;
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : text) {
    if (c == separator) {
      parts.push_back(current);
      current.clear();
    } else if (c != ' ') {
      current += c;
    }
  }
  parts.push_back(current);
  return parts;
}

bool ApplyKey(const std::string& key, ScriptedStep& step) {
  const size_t equals = key.find('=');
  if (equals != std::string::npos) {
    const std::string axis = Upper(key.substr(0, equals));
    const int value = std::atoi(key.c_str() + equals + 1);
    const int16_t clamped = static_cast<int16_t>(std::clamp(value, -32768, 32767));
    if (axis == "LX") step.thumb_lx = clamped;
    else if (axis == "LY") step.thumb_ly = clamped;
    else if (axis == "RX") step.thumb_rx = clamped;
    else if (axis == "RY") step.thumb_ry = clamped;
    else if (axis == "LT") step.left_trigger = static_cast<uint8_t>(std::clamp(value, 0, 255));
    else if (axis == "RT") step.right_trigger = static_cast<uint8_t>(std::clamp(value, 0, 255));
    else return false;
    return true;
  }
  static const struct {
    const char* name;
    uint16_t bit;
  } kButtons[] = {
      {"A", rex::input::X_INPUT_GAMEPAD_A},
      {"B", rex::input::X_INPUT_GAMEPAD_B},
      {"X", rex::input::X_INPUT_GAMEPAD_X},
      {"Y", rex::input::X_INPUT_GAMEPAD_Y},
      {"START", rex::input::X_INPUT_GAMEPAD_START},
      {"BACK", rex::input::X_INPUT_GAMEPAD_BACK},
      {"LB", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"RB", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"LS", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
      {"RS", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"UP", rex::input::X_INPUT_GAMEPAD_DPAD_UP},
      {"DOWN", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
      {"LEFT", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
      {"RIGHT", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  const std::string name = Upper(key);
  for (const auto& button : kButtons) {
    if (name == button.name) {
      step.buttons |= button.bit;
      return true;
    }
  }
  return false;
}

// Button bit index -> the VK_PAD_* the guest expects, in the order the SDK's own drivers report them.
constexpr rex::ui::VirtualKey kVirtualKeys[16] = {
    rex::ui::VirtualKey::kXInputPadDpadUp,      rex::ui::VirtualKey::kXInputPadDpadDown,
    rex::ui::VirtualKey::kXInputPadDpadLeft,    rex::ui::VirtualKey::kXInputPadDpadRight,
    rex::ui::VirtualKey::kXInputPadStart,       rex::ui::VirtualKey::kXInputPadBack,
    rex::ui::VirtualKey::kXInputPadLThumbPress, rex::ui::VirtualKey::kXInputPadRThumbPress,
    rex::ui::VirtualKey::kXInputPadLShoulder,   rex::ui::VirtualKey::kXInputPadRShoulder,
    rex::ui::VirtualKey::kNone,  // guide has no virtual key
    rex::ui::VirtualKey::kNone,
    rex::ui::VirtualKey::kXInputPadA,           rex::ui::VirtualKey::kXInputPadB,
    rex::ui::VirtualKey::kXInputPadX,           rex::ui::VirtualKey::kXInputPadY,
};

}  // namespace

void ScriptedInputDriver::QueueKeystrokes(uint16_t before, uint16_t after) {
  for (uint32_t bit = 0; bit < 16; ++bit) {
    const uint16_t mask = static_cast<uint16_t>(1u << bit);
    if (kVirtualKeys[bit] == rex::ui::VirtualKey::kNone || ((before ^ after) & mask) == 0) {
      continue;
    }
    const bool pressed = (after & mask) != 0;
    keystrokes_.emplace_back(kVirtualKeys[bit],
                             static_cast<uint16_t>(pressed ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN
                                                           : rex::input::X_INPUT_KEYSTROKE_KEYUP));
  }
}

bool ScriptedInputDriver::Parse(const std::string& script, std::vector<ScriptedStep>& out) {
  out.clear();
  for (const std::string& entry : Split(script, ',')) {
    if (entry.empty()) continue;
    const size_t colon = entry.find(':');
    if (colon == std::string::npos) {
      REXLOG_ERROR("nb: input script: '{}' is not <seconds>:<keys>", entry);
      return false;
    }
    ScriptedStep step;
    step.at_seconds = std::atof(entry.substr(0, colon).c_str());
    step.text = entry.substr(colon + 1);
    for (const std::string& key : Split(step.text, '+')) {
      if (key.empty()) continue;
      if (!ApplyKey(key, step)) {
        REXLOG_ERROR("nb: input script: unknown key '{}' in '{}'", key, entry);
        return false;
      }
    }
    out.push_back(step);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const ScriptedStep& a, const ScriptedStep& b) { return a.at_seconds < b.at_seconds; });
  return true;
}

ScriptedInputDriver::ScriptedInputDriver(rex::ui::Window* window, size_t window_z_order,
                                         const std::string& script)
    : InputDriver(window, window_z_order), start_(std::chrono::steady_clock::now()) {
  // The clock starts here as well as in Setup(): the SDK's own drivers are set up before being added,
  // and a driver added without that call would otherwise date every step to the epoch.
  Parse(script, steps_);
}

X_STATUS ScriptedInputDriver::Setup() {
  start_ = std::chrono::steady_clock::now();
  REXLOG_INFO("nb: scripted controller with {} steps", steps_.size());
  return X_STATUS_SUCCESS;
}

void ScriptedInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kScriptedDevice;
  info.name = "nb scripted controller";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT ScriptedInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t /*flags*/,
                                                         rex::input::X_INPUT_CAPABILITIES* out_caps) {
  if (id != kScriptedDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // gamepad
    out_caps->sub_type = 0x01;  // standard gamepad
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x8000u);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x8000u);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x8000u);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x8000u);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptedInputDriver::GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) {
  if (id != kScriptedDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!out_state) {
    return X_ERROR_SUCCESS;
  }
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  size_t step = SIZE_MAX;
  for (size_t i = 0; i < steps_.size() && steps_[i].at_seconds <= now; ++i) {
    step = i;
  }
  std::memset(out_state, 0, sizeof(*out_state));
  if (step != SIZE_MAX) {
    const ScriptedStep& current = steps_[step];
    out_state->gamepad.buttons = current.buttons;
    out_state->gamepad.thumb_lx = current.thumb_lx;
    out_state->gamepad.thumb_ly = current.thumb_ly;
    out_state->gamepad.thumb_rx = current.thumb_rx;
    out_state->gamepad.thumb_ry = current.thumb_ry;
    out_state->gamepad.left_trigger = current.left_trigger;
    out_state->gamepad.right_trigger = current.right_trigger;
  }
  if (step != last_step_) {
    last_step_ = step;
    ++packet_number_;  // a changed packet number is how the guest notices
    const uint16_t buttons = out_state->gamepad.buttons;
    QueueKeystrokes(last_buttons_, buttons);
    last_buttons_ = buttons;
    if (step != SIZE_MAX) {
      REXLOG_INFO("nb: scripted controller at {:.1f} s: '{}'", now, steps_[step].text);
    }
  }
  out_state->packet_number = packet_number_;
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptedInputDriver::SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* /*vibration*/) {
  return id == kScriptedDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptedInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t /*flags*/,
                                                rex::input::X_INPUT_KEYSTROKE* out_keystroke) {
  if (id != kScriptedDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // The menus poll this instead of the pad state, so a press has to arrive here too.
  if (keystrokes_.empty()) {
    return X_ERROR_EMPTY;
  }
  const auto event = keystrokes_.front();
  keystrokes_.pop_front();
  if (out_keystroke) {
    std::memset(out_keystroke, 0, sizeof(*out_keystroke));
    out_keystroke->virtual_key = static_cast<uint16_t>(event.first);
    out_keystroke->flags = event.second;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace nb::input
