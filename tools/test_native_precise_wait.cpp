// Original portable controls for the production timer policy. No real sleeps.
// cl /nologo /EHsc /std:c++20 /O2 tools/test_native_precise_wait.cpp /Fe:<scratch.exe>
#include "../src/gpu/native/native_precise_wait_policy.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace {
uint64_t checks = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
struct Timer {
  uint64_t now = 1000000000, create_cost = 0, overshoot = 0;
  bool create_ok = true, clock_backwards = false;
  unsigned creates = 0, reads = 0, waits = 0, fail_at = 99;
  unsigned rollback_on_read = 0;
  uint64_t rollback_by = 0;
  std::array<int64_t, 8> due{};
  std::array<uint64_t, 8> elapsed{};
  Timer() { elapsed.fill(std::numeric_limits<uint64_t>::max()); }
  uint64_t NowNs() noexcept {
    ++reads;
    if (reads == rollback_on_read) return now - rollback_by;
    return clock_backwards && reads > 1 ? now - 1 : now;
  }
  bool EnsureTimer() noexcept { ++creates; now += create_cost; return create_ok; }
  bool WaitRelative(int64_t ticks) noexcept {
    Check(ticks < 0, "every due time is strictly relative");
    Check(waits < due.size(), "at most eight blocking attempts");
    const unsigned i = waits++;
    due[i] = ticks;
    if (i == fail_at) return false;
    now += elapsed[i] == std::numeric_limits<uint64_t>::max()
               ? uint64_t(-ticks) * 100 + overshoot : elapsed[i];
    return true;
  }
};

void LiteralCases() {
  Timer zero;
  Check(nb::gpu::NativeWaitAtLeast(zero, 0), "zero delay");
  Check(zero.creates == 0 && zero.waits == 0 && zero.reads == 0, "zero has no backend calls");
  for (uint32_t ms : {1u, 2u, 4u, 16777215u, 0xFFFFFFFFu}) {
    Timer timer;
    const uint64_t start = timer.now;
    Check(nb::gpu::NativeWaitAtLeast(timer, ms), "exact timer succeeds");
    Check(timer.waits == 1 && timer.due[0] == -int64_t(ms) * 10000,
          "literal ms to relative100ns conversion without overflow");
    Check(timer.now - start == uint64_t(ms) * 1000000, "literal whole-ms duration");
  }
  Timer early;
  early.elapsed[0] = 999950;
  Check(nb::gpu::NativeWaitAtLeast(early, 1), "early signal rearmed");
  Check(early.waits == 2 && early.due[0] == -10000 && early.due[1] == -1,
        "fifty remaining ns rounds UP to one tick");
  Check(early.now == 1001000050, "early signal never shortens requested wait");

  Timer create;
  create.create_cost = 100001;
  Check(nb::gpu::NativeWaitAtLeast(create, 1), "creation overhead counts elapsed");
  Check(create.due[0] == -9000 && create.now == 1001000001,
        "one monotonic start including setup");
  Timer late_create;
  late_create.create_cost = 1000001;
  Check(nb::gpu::NativeWaitAtLeast(late_create, 1) && !late_create.waits,
        "already elapsed request needs no additional wait");
  Timer late;
  late.overshoot = 300000;
  Check(nb::gpu::NativeWaitAtLeast(late, 1) && late.waits == 1,
        "late wake does not rearm full duration");
  Timer near_limit;
  near_limit.now = std::numeric_limits<uint64_t>::max() - 2000000;
  Check(nb::gpu::NativeWaitAtLeast(near_limit, 1), "elapsed arithmetic near clock limit");
}

void Failures() {
  Timer unavailable;
  unavailable.create_ok = false;
  Check(!nb::gpu::NativeWaitAtLeast(unavailable, 1) && !unavailable.waits,
        "unsupported creation requests legacy fallback");
  Timer failed;
  failed.fail_at = 0;
  Check(!nb::gpu::NativeWaitAtLeast(failed, 1) && failed.waits == 1,
        "arm/wait failure requests legacy fallback");
  Timer failed_after_partial;
  failed_after_partial.elapsed[0] = 500000;
  failed_after_partial.fail_at = 1;
  Check(!nb::gpu::NativeWaitAtLeast(failed_after_partial, 1) &&
        failed_after_partial.waits == 2 && failed_after_partial.due[1] == -5000,
        "failure after a partial delay still requests caller fallback");
  Timer backwards;
  backwards.clock_backwards = true;
  Check(!nb::gpu::NativeWaitAtLeast(backwards, 1) && !backwards.waits,
        "broken monotonic clock fails closed");
  Timer stuck;
  stuck.elapsed.fill(0);
  Check(!nb::gpu::NativeWaitAtLeast(stuck, 1) && stuck.waits == 8,
        "repeated immediate signals cannot create an unbounded loop");
}

void RemainderOracle() {
  // Deliberately early first wake leaves each positive sub-ms remainder. The
  // independent oracle chooses the least integer tick whose duration covers it.
  for (uint64_t remaining = 1; remaining <= 100000; ++remaining) {
    Timer timer;
    timer.elapsed[0] = 1000000 - remaining;
    Check(nb::gpu::NativeWaitAtLeast(timer, 1), "remainder completes");
    uint64_t ticks = remaining / 100;
    if (ticks * 100 < remaining) ++ticks;
    Check(timer.waits == 2 && timer.due[1] == -int64_t(ticks),
          "independent quotient/remainder oracle");
    Check(timer.now >= 1001000000 && timer.now < 1001000100,
          "completion honors lower bound with less than one tick rounding");
  }
}

void NanosecondCases() {
  constexpr uint64_t short_ns = nb::gpu::NativeEarlyWaitPoll::kWaitNanoseconds;
  Timer zero;
  Check(nb::gpu::NativeWaitAtLeastNs(zero, 0) && !zero.creates && !zero.reads,
        "zero ns request has no timer or clock work");
  for (uint64_t request : {uint64_t(1), uint64_t(99), uint64_t(100), uint64_t(101),
                           short_ns, short_ns + 1, nb::gpu::kNativeWaitMaxNanoseconds}) {
    Timer timer;
    const uint64_t start = timer.now;
    Check(nb::gpu::NativeWaitAtLeastNs(timer, request), "bounded nanosecond timer succeeds");
    const uint64_t ticks = request / 100 + (request % 100 ? 1u : 0u);
    Check(timer.waits == 1 && timer.due[0] == -int64_t(ticks), "ns conversion rounds up independently");
    Check(timer.now - start >= request && timer.now - start - request < 100,
          "ns request meets lower bound with less than one tick rounding");
  }
  for (uint64_t oversized : {nb::gpu::kNativeWaitMaxNanoseconds + 1, UINT64_MAX}) {
    Timer timer;
    Check(!nb::gpu::NativeWaitAtLeastNs(timer, oversized) && !timer.creates && !timer.reads && !timer.waits,
          "oversized duration refuses before backend use");
    Check(nb::gpu::NativeWaitAtLeastNs(timer, short_ns), "refused input does not poison backend");
  }
  Timer partial_failure;
  partial_failure.elapsed[0] = 100000;
  partial_failure.fail_at = 1;
  Check(!nb::gpu::NativeWaitAtLeastNs(partial_failure, short_ns) && partial_failure.waits == 2 &&
            partial_failure.due[1] == -1500, "short timer partial failure returns fallback");
  Timer rollback;
  rollback.elapsed[0] = 100000;
  rollback.elapsed[1] = 10000;
  rollback.rollback_on_read = 4;
  rollback.rollback_by = 20000;  // Backwards from last read, still above start.
  Check(!nb::gpu::NativeWaitAtLeastNs(rollback, short_ns) && rollback.waits == 2,
        "clock rollback between partial waits fails closed");
  Timer overflow;
  overflow.now = UINT64_MAX - 100;
  Check(!nb::gpu::NativeWaitAtLeastNs(overflow, short_ns), "clock wrap fails closed");
  Timer unsupported;
  unsupported.create_ok = false;
  Check(!nb::gpu::NativeWaitAtLeastNs(unsupported, short_ns) && !unsupported.waits,
        "short timer creation failure returns fallback");
  Timer stuck;
  stuck.elapsed.fill(0);
  Check(!nb::gpu::NativeWaitAtLeastNs(stuck, short_ns) && stuck.waits == 8,
        "short timer early signals remain bounded");

  // Every possible remainder after an early wake of the 250us experiment.
  for (uint64_t remainder = 1; remainder <= short_ns; ++remainder) {
    Timer timer;
    timer.elapsed[0] = short_ns - remainder;
    Check(nb::gpu::NativeWaitAtLeastNs(timer, short_ns), "short remainder completes");
    const uint64_t ticks = remainder / 100 + (remainder % 100 ? 1u : 0u);
    Check(timer.waits == 2 && timer.due[1] == -int64_t(ticks), "short remainder oracle");
    Check(timer.now >= 1000000000 + short_ns && timer.now < 1000000100 + short_ns,
          "short remainder respects requested lower bound");
  }
}

void EarlyPollStateControls() {
  using Early = nb::gpu::NativeEarlyWaitPoll;
  for (uint32_t interval : {0u, 255u, 256u, 257u, 511u, 512u, UINT32_MAX}) {
    for (uint32_t function = 0; function < 8; ++function) {
      for (bool vsync : {false, true}) {
        for (bool enabled : {false, true}) {
          Early state;
          const auto first = state.ObservePredicate(false);
          const bool expected = enabled && vsync && interval >= 256 && interval < 512 && function != 0;
          Check(Early::ShouldWaitEarly(first, interval, function, vsync, enabled) == expected,
                "first-failed eligibility differs from literal interval bounds");
          Check(!first.early_match && first.first_failure, "first failed predicate observation");
          const auto second = state.ObservePredicate(false);
          Check(!Early::ShouldWaitEarly(second, 256, 3, true, true),
                "later flags/interval changes cannot recover consumed first failure");
        }
      }
    }
  }
  Early already_matched;
  const auto matched = already_matched.ObservePredicate(true);
  Check(!matched.first_failure && !matched.early_match &&
            !Early::ShouldWaitEarly(matched, 256, 7, true, true),
        "already matched/Always predicate never schedules a short wait");
  for (bool success : {false, true}) {
    for (bool next_matches : {false, true}) {
      Early state;
      const auto first = state.ObservePredicate(false);
      Check(Early::ShouldWaitEarly(first, 256, 3, true, true), "short wait fixture eligible");
      state.DidShortWait(success);
      const auto next = state.ObservePredicate(next_matches);
      Check(next.early_match == (success && next_matches) && !next.first_failure,
            "early match only after immediately preceding successful short wait");
      Check(!Early::ShouldWaitEarly(next, 256, 3, true, true), "only one short wait per packet");
      const auto later = state.ObservePredicate(true);
      Check(!later.early_match && !later.first_failure, "later full-wait match cannot count as early");
    }
  }
  // Mixed read histories: the independent expected match is tied to read index
  // one only. Subsequent outcomes never create another early opportunity.
  for (uint32_t bits = 0; bits < 256; ++bits) {
    for (bool succeeded : {false, true}) {
      Early state;
      const auto first = state.ObservePredicate(false);
      Check(first.first_failure, "fresh packet resets first-failure state");
      state.DidShortWait(succeeded);
      for (unsigned read = 0; read < 8; ++read) {
        const bool matches = (bits & (1u << read)) != 0;
        const auto observed = state.ObservePredicate(matches);
        Check(observed.early_match == (read == 0 && succeeded && matches),
              "early attribution matches independent read-index oracle");
        Check(!observed.first_failure && !Early::ShouldWaitEarly(observed, 256, 3, true, true),
              "mixed outcomes do not create additional short waits");
      }
    }
  }
}
}  // namespace

int main() {
  LiteralCases();
  Failures();
  RemainderOracle();
  NanosecondCases();
  EarlyPollStateControls();
  std::printf("PASS %llu checks (ms/ns literals, failures, 350000 remainder controls, early-poll state)\n",
              static_cast<unsigned long long>(checks));
}
