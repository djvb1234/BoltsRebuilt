// Original bounded wait policy, independent of Windows and the guest clock.
#pragma once
#include <cstdint>

namespace nb::gpu {
inline constexpr uint64_t kNativeWaitMaxNanoseconds = uint64_t(UINT32_MAX) * 1000000;

// Backend owns a private synchronous timer. NowNs is monotonic; WaitRelative
// arms a one-shot negative 100 ns due time and blocks until signaled. Failures
// return false so the caller can perform the unchanged full-duration Sleep.
template <typename Backend>
bool NativeWaitAtLeastNs(Backend& backend, uint64_t requested_ns) noexcept {
  if (!requested_ns) return true;
  // Keep the duration domain bounded by the existing millisecond API. Reject
  // oversized calls before touching the backend or doing clock arithmetic.
  if (requested_ns > kNativeWaitMaxNanoseconds) return false;
  const uint64_t start = backend.NowNs();
  if (!backend.EnsureTimer()) return false;
  uint64_t previous = start;
  // Every retry blocks on a newly armed timer; there is no polling/spin wait.
  // Defensively bound unexpected repeated early signals before falling back.
  for (unsigned attempt = 0; attempt <= 8; ++attempt) {
    const uint64_t now = backend.NowNs();
    if (now < previous) return false;
    previous = now;
    const uint64_t elapsed = now - start;
    if (elapsed >= requested_ns) return true;
    if (attempt == 8) return false;
    const uint64_t remaining_ns = requested_ns - elapsed;
    // Round up, never down or to zero (zero would mean an absolute due time).
    const int64_t ticks = int64_t(remaining_ns / 100 + (remaining_ns % 100 != 0));
    if (!backend.WaitRelative(-ticks)) return false;
  }
  return false;
}

template <typename Backend>
bool NativeWaitAtLeast(Backend& backend, uint32_t milliseconds) noexcept {
  return NativeWaitAtLeastNs(backend, uint64_t(milliseconds) * 1000000);
}

// Diagnostic timing experiment, not a claim about Xenos hardware cycles. This
// state tracks reads, not elapsed time: only the next normal predicate read
// following a successful short wait may count as an early match.
class NativeEarlyWaitPoll {
 public:
  static constexpr uint64_t kWaitNanoseconds = 250000;
  struct Read {
    bool first_failure;
    bool early_match;
  };
  Read ObservePredicate(bool matched) noexcept {
    const Read result{!matched && !failed_once_, matched && short_wait_pending_};
    failed_once_ |= !matched;
    short_wait_pending_ = false;
    return result;
  }
  static bool ShouldWaitEarly(const Read& read, uint32_t interval,
                              uint32_t function, bool vsync, bool enabled) noexcept {
    return enabled && read.first_failure && vsync && interval / 0x100 == 1 &&
           (function & 7) != 0;
  }
  void DidShortWait(bool succeeded) noexcept { short_wait_pending_ = succeeded; }

 private:
  bool failed_once_ = false;
  bool short_wait_pending_ = false;
};
}  // namespace nb::gpu
