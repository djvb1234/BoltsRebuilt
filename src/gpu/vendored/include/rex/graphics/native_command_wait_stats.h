// Original optional command-thread wait diagnostics. No synchronization policy
// changes: clocks run only around actual empty-ring periods or blocked packets.
#pragma once
#include <chrono>
#include <cstdint>

namespace rex::graphics {
struct NativeCommandWaitStats {
  uint64_t idle_ns = 0, idle_periods = 0;
  uint64_t wait_reg_ns = 0, wait_reg_packets = 0, blocked_packets = 0;
  uint64_t sleep_calls = 0, yield_calls = 0;
  uint64_t requested_sleep_ns = 0, actual_sleep_ns = 0;
  uint64_t precise_calls = 0, precise_fallbacks = 0;
  uint64_t early_polls = 0, early_matches = 0, early_fallbacks = 0;
};
inline NativeCommandWaitStats& GetNativeCommandWaitStats() {
  static NativeCommandWaitStats stats;
  return stats;  // All accesses are on this plugin's command processor thread.
}
class NativeCommandWaitTimer {
 public:
  NativeCommandWaitTimer(bool enabled, uint64_t& total) : total_(enabled ? &total : nullptr) {}
  ~NativeCommandWaitTimer() { Stop(); }
  void Start() {
    if (total_ && !started_) { start_ = std::chrono::steady_clock::now(); started_ = true; }
  }
  void Stop() {
    if (!started_) return;
    *total_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start_).count());
    started_ = false;
  }
 private:
  uint64_t* total_;
  bool started_ = false;
  std::chrono::steady_clock::time_point start_{};
};
}  // namespace rex::graphics
