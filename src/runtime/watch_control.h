#pragma once

#include <cstdint>

namespace nb::runtime {

inline constexpr uint32_t kWatchAlreadyEnabled = 1;
inline constexpr uint32_t kWatchGuestImmutable = 2;

// These functions are supplied by the project-owned rexruntime build in both
// configurations, so toggling the source override does not change its imports.
bool WatchFastpathAvailable() noexcept;
bool WatchFastpathEnabled() noexcept;
void SetWatchFastpathEnabled(bool enabled) noexcept;
uint32_t GetWatchFastpathMode() noexcept;
void SetWatchFastpathMode(uint32_t mode) noexcept;

}  // namespace nb::runtime
