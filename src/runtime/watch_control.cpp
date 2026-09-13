#include "watch_control.h"

#include <atomic>

#ifndef NB_RUNTIME_WATCH_FASTPATH
#define NB_RUNTIME_WATCH_FASTPATH 0
#endif

namespace nb::runtime {
namespace {
std::atomic<uint32_t> watch_fastpath_mode{0};
}

bool WatchFastpathAvailable() noexcept {
  return NB_RUNTIME_WATCH_FASTPATH != 0;
}

bool WatchFastpathEnabled() noexcept {
  return (GetWatchFastpathMode() & kWatchAlreadyEnabled) != 0;
}

void SetWatchFastpathEnabled(bool enabled) noexcept {
  // This flag publishes no data. Both paths acquire the original memory lock
  // and have identical state transitions. A concurrent toggle may select either.
  if (enabled && WatchFastpathAvailable()) {
    watch_fastpath_mode.fetch_or(kWatchAlreadyEnabled, std::memory_order_relaxed);
  } else {
    watch_fastpath_mode.fetch_and(~kWatchAlreadyEnabled, std::memory_order_relaxed);
  }
}

uint32_t GetWatchFastpathMode() noexcept {
  return watch_fastpath_mode.load(std::memory_order_relaxed);
}

void SetWatchFastpathMode(uint32_t mode) noexcept {
  watch_fastpath_mode.store(WatchFastpathAvailable()
                               ? mode & (kWatchAlreadyEnabled | kWatchGuestImmutable)
                               : 0,
                           std::memory_order_relaxed);
}

}  // namespace nb::runtime
