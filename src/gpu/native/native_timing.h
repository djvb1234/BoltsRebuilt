// Original optional fine-grained draw diagnostics. Coarse frame/draw clocks are
// separate so a profiling-off comparison still measures actual elapsed time.
#pragma once
#include <chrono>
#include <cstdint>

namespace nb::gpu {
inline std::chrono::steady_clock::time_point NativeTimingStart(bool enabled) {
  return enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
}
inline uint64_t NativeTimingElapsed(bool enabled, std::chrono::steady_clock::time_point start) {
  return enabled ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count()) : 0;
}
}  // namespace nb::gpu
