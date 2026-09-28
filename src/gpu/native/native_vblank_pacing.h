// Original guest-vblank pacing helpers for the SDK's "GPU VSync" thread
// (vendored src/graphics/graphics_system.cpp). Portable and free of Windows.
//
// The SDK thread marks every vblank whose due guest tick has passed, then
// sleeps 1 ms and looks again. A vblank is therefore marked at the first loop
// wakeup after it falls due, up to a full host sleep period late, and anything
// the guest gates on it (an interrupt-driven swap, a WAIT_REG_MEM on a counter
// the interrupt writes) inherits that delay. The optional precise pacing sleeps
// only until the next vblank is due, never longer than the original 1 ms.
#pragma once

#include <atomic>
#include <cstdint>

namespace nb::gpu {

// The original loop's sleep, and the longest the precise pacing ever waits.
inline constexpr uint64_t kNativeVblankMaxWaitNs = 1000000;

// Host nanoseconds covered by a span of guest ticks, rounded up. Guest ticks
// advance at frequency * scalar per host second (rex::chrono::Clock). An
// unusable frequency or scalar, or a span too large to represent, saturates.
inline uint64_t NativeGuestTicksToNs(uint64_t ticks, uint64_t frequency, double scalar) noexcept {
  if (!ticks) return 0;
  if (!frequency || !(scalar > 0.0)) return UINT64_MAX;
  const double ns = double(ticks) * 1e9 / (double(frequency) * scalar);
  if (!(ns < 18446744073709549568.0)) return UINT64_MAX;  // largest double below 2^64
  const uint64_t whole = uint64_t(ns);
  return whole + (double(whole) < ns ? 1 : 0);
}

// How long the VSync thread may wait, observed at guest tick `now`, before the
// vblank due at guest tick `due`. Zero when it is already due (mark it without
// sleeping). Never more than the original 1 ms, so the pacing can only wake the
// thread earlier than the original loop would have, and a scalar or rate change
// is picked up within the same bound.
inline uint64_t NativeVblankWaitNs(uint64_t now, uint64_t due, uint64_t frequency,
                                   double scalar) noexcept {
  if (due <= now) return 0;
  const uint64_t ns = NativeGuestTicksToNs(due - now, frequency, scalar);
  return ns < kNativeVblankMaxWaitNs ? ns : kNativeVblankMaxWaitNs;
}

// Always-on counters. Written only by the VSync thread; the command thread
// reads them for the periodic log line, so relaxed atomics are enough.
struct NativeVblankStats {
  std::atomic<uint64_t> marked{0};         // vblanks marked
  std::atomic<uint64_t> late_ns{0};        // summed delay from due tick to marking
  std::atomic<uint64_t> max_late_ns{0};    // largest single delay
  std::atomic<uint64_t> late_over_1ms{0};  // vblanks marked more than 1 ms after due
  std::atomic<uint64_t> precise_waits{0};  // precise timer waits taken
  std::atomic<uint64_t> precise_fallbacks{0};  // timer failures that fell back to Sleep(1)

  void RecordMarked(uint64_t late) noexcept {
    marked.fetch_add(1, std::memory_order_relaxed);
    const uint64_t sum = late_ns.load(std::memory_order_relaxed);
    late_ns.store(sum + late < sum ? UINT64_MAX : sum + late, std::memory_order_relaxed);
    if (late > max_late_ns.load(std::memory_order_relaxed))
      max_late_ns.store(late, std::memory_order_relaxed);
    if (late > 1000000) late_over_1ms.fetch_add(1, std::memory_order_relaxed);
  }
};
inline NativeVblankStats& GetNativeVblankStats() noexcept {
  static NativeVblankStats stats;
  return stats;
}

}  // namespace nb::gpu
