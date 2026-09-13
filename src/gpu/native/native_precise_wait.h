// Original opt-in host timer for the calling command processor thread.
#pragma once
#include <cstdint>

namespace nb::gpu {
// A successful call has elapsed at least the requested whole milliseconds.
// False means unavailable/API failure: the caller MUST use its original Sleep
// with the FULL requested duration. That fallback retains legacy OS semantics.
// One lazy private handle per calling thread; it closes when that thread exits.
// No guest clock, polling predicate, process timer resolution or affinity change.
bool NativePreciseSleep(uint32_t milliseconds) noexcept;
// Same private timer and lower-bound/fallback contract for a bounded ns request
// (at most UINT32_MAX milliseconds). No fallback sleep is performed internally.
bool NativePreciseSleepNs(uint64_t nanoseconds) noexcept;
}  // namespace nb::gpu
