// Standalone controls for src/gpu/native/native_vblank_pacing.h. No game data, no GPU.
// Build: clang++ -std=c++20 -O2 -Wall -Wextra -Werror -DNOMINMAX -Isrc/gpu/native
//        tools/test_native_vblank_pacing.cpp -o control.exe
#include "native_vblank_pacing.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
int g_checks = 0;
int g_failures = 0;
void Check(bool ok, const char* what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL line %d: %s\n", line, what);
  }
}
#define CHECK(x) Check((x), #x, __LINE__)

using nb::gpu::kNativeVblankMaxWaitNs;
using nb::gpu::NativeGuestTicksToNs;
using nb::gpu::NativeVblankWaitNs;

constexpr uint64_t kQpc = 10000000;  // a common Windows QPC rate, 100 ns ticks
constexpr uint64_t kOdd = 3579545;   // a legacy ACPI PM timer rate, not a divisor of 1e9

void TicksToNs() {
  CHECK(NativeGuestTicksToNs(0, kQpc, 1.0) == 0);
  CHECK(NativeGuestTicksToNs(1, kQpc, 1.0) == 100);
  CHECK(NativeGuestTicksToNs(20833, kQpc, 1.0) == 2083300);
  // Rounds up, never down: 1 tick at 3579545 Hz is 279.36... ns.
  CHECK(NativeGuestTicksToNs(1, kOdd, 1.0) == 280);
  // Guest time running twice as fast covers half the host time.
  CHECK(NativeGuestTicksToNs(20000, kQpc, 2.0) == 1000000);
  CHECK(NativeGuestTicksToNs(20000, kQpc, 0.5) == 4000000);
  // Unusable inputs saturate rather than divide by zero.
  CHECK(NativeGuestTicksToNs(1, 0, 1.0) == UINT64_MAX);
  CHECK(NativeGuestTicksToNs(1, kQpc, 0.0) == UINT64_MAX);
  CHECK(NativeGuestTicksToNs(1, kQpc, -1.0) == UINT64_MAX);
  CHECK(NativeGuestTicksToNs(UINT64_MAX, 1, 1.0) == UINT64_MAX);
}

void WaitBounds() {
  // Already due, or overdue: mark without sleeping.
  CHECK(NativeVblankWaitNs(100, 100, kQpc, 1.0) == 0);
  CHECK(NativeVblankWaitNs(200, 100, kQpc, 1.0) == 0);
  // Due within the original sleep: wait exactly until then.
  CHECK(NativeVblankWaitNs(0, 1, kQpc, 1.0) == 100);
  CHECK(NativeVblankWaitNs(1000, 5000, kQpc, 1.0) == 400000);
  CHECK(NativeVblankWaitNs(0, 9999, kQpc, 1.0) == 999900);
  // Never longer than the original 1 ms sleep, whatever the gap or inputs.
  CHECK(NativeVblankWaitNs(0, 10000, kQpc, 1.0) == kNativeVblankMaxWaitNs);
  CHECK(NativeVblankWaitNs(0, 20833, kQpc, 1.0) == kNativeVblankMaxWaitNs);
  CHECK(NativeVblankWaitNs(0, UINT64_MAX, 1, 1.0) == kNativeVblankMaxWaitNs);
  CHECK(NativeVblankWaitNs(0, 1, 0, 1.0) == kNativeVblankMaxWaitNs);
  CHECK(NativeVblankWaitNs(0, 1, kQpc, 0.0) == kNativeVblankMaxWaitNs);
}

// Replays the vendored loop against an idealised host whose timer wakes
// exactly when asked and whose Sleep(1) wakes after sleep_ns. Checks every
// vblank is marked in order, none early, and the precise pacing is never later.
struct Result {
  uint64_t marked = 0, max_late_ns = 0, late_ns = 0;
};
Result Simulate(bool precise, uint64_t sleep_ns, uint64_t interval_ticks, uint64_t end_ns) {
  Result r;
  uint64_t now_ns = 12345;  // arbitrary phase against the vblank grid
  const auto ticks = [&] { return now_ns / 100; };  // kQpc
  uint64_t last = ticks();
  while (now_ns < end_ns) {
    const uint64_t current = ticks();
    while (current - last >= interval_ticks) {
      const uint64_t late = NativeGuestTicksToNs(current - last - interval_ticks, kQpc, 1.0);
      ++r.marked;
      r.late_ns += late;
      if (late > r.max_late_ns) r.max_late_ns = late;
      last += interval_ticks;
    }
    if (precise) {
      const uint64_t wait = NativeVblankWaitNs(ticks(), last + interval_ticks, kQpc, 1.0);
      CHECK(wait <= kNativeVblankMaxWaitNs);
      now_ns += wait;
      continue;
    }
    now_ns += sleep_ns;
  }
  return r;
}

void LoopReplay() {
  const uint64_t interval = 20833;  // 480 Hz at 10 MHz
  const uint64_t end = 1000000000;  // one host second
  for (uint64_t sleep_ns : {1000000ull, 1500000ull, 2000000ull}) {
    const Result original = Simulate(false, sleep_ns, interval, end);
    const Result precise = Simulate(true, sleep_ns, interval, end);
    // Same vblank count within the last partial interval.
    CHECK(original.marked + 1 >= precise.marked && precise.marked + 1 >= original.marked);
    CHECK(precise.marked >= 479 && precise.marked <= 481);
    // The precise pacing marks each vblank within one guest tick of its due time.
    CHECK(precise.max_late_ns <= 100);
    CHECK(precise.late_ns <= original.late_ns);
    // The original loop is late by up to a whole sleep period.
    CHECK(original.max_late_ns > 500000);
    CHECK(original.max_late_ns <= sleep_ns);
    std::printf("sleep %llu ns: original mean late %.0f ns, max %llu; precise mean %.0f ns, max %llu\n",
                (unsigned long long)sleep_ns, double(original.late_ns) / double(original.marked),
                (unsigned long long)original.max_late_ns, double(precise.late_ns) / double(precise.marked),
                (unsigned long long)precise.max_late_ns);
  }
}

void Stats() {
  nb::gpu::NativeVblankStats stats;
  stats.RecordMarked(0);
  stats.RecordMarked(1500000);
  stats.RecordMarked(700000);
  CHECK(stats.marked.load() == 3);
  CHECK(stats.late_ns.load() == 2200000);
  CHECK(stats.max_late_ns.load() == 1500000);
  CHECK(stats.late_over_1ms.load() == 1);
  stats.RecordMarked(UINT64_MAX);
  CHECK(stats.late_ns.load() == UINT64_MAX);  // saturates, never wraps
  CHECK(&nb::gpu::GetNativeVblankStats() == &nb::gpu::GetNativeVblankStats());
}
}  // namespace

int main() {
  TicksToNs();
  WaitBounds();
  LoopReplay();
  Stats();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
