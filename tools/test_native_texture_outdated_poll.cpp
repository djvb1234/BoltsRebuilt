// Original portable controls for the actual atomic consume helper. This proves
// event/visibility behavior, not game performance or Windows texture watches.
#include "../src/gpu/native/native_texture_outdated_poll.h"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using nb::gpu::NativePollTextureOutdated;
using nb::gpu::NativeTextureOutdatedPollStats;
using nb::gpu::detail::NativePollTextureOutdatedObserved;
using Stage = nb::gpu::detail::NativeTexturePollStage;
uint64_t checks = 0;
void Check(bool value, const char* why) {
  ++checks;
  if (!value) throw std::runtime_error(why);
}

void LiteralAndToggleControls() {
  std::atomic<bool> flag{false};
  NativeTextureOutdatedPollStats stats;
  unsigned hooks = 0;
  auto observer = [&](Stage, bool) noexcept { ++hooks; };
  Check(!NativePollTextureOutdatedObserved(flag, false, stats, observer), "off false");
  flag.store(true, std::memory_order_release);
  Check(NativePollTextureOutdatedObserved(flag, false, stats, observer), "off consumes true");
  Check(hooks == 0 && stats.false_load_skips == 0 && stats.exchanges == 0 &&
            stats.consumed_notifications == 0, "off touches neither hooks nor stats");
  Check(!flag.load() && !NativePollTextureOutdated(flag, true, stats), "on false skips");
  flag.store(true, std::memory_order_release);
  flag.store(true, std::memory_order_release);
  Check(NativePollTextureOutdated(flag, true, stats), "coalesced true consumed once");
  Check(!NativePollTextureOutdated(flag, true, stats), "consumed flag remains false");
  Check(stats.false_load_skips == 2 && stats.exchanges == 1 &&
            stats.consumed_notifications == 1, "enabled counters describe polls");
  flag.store(true, std::memory_order_release);
  Check(NativePollTextureOutdated(flag, false, stats), "toggle off consumes pending event");
  Check(stats.exchanges == 1 && stats.consumed_notifications == 1, "off is uncounted");
  flag.store(true, std::memory_order_release);
  Check(NativePollTextureOutdated(flag, true, stats), "toggle on consumes fresh event");
}

void InterleavingControls() {
  std::atomic<bool> flag{false};
  NativeTextureOutdatedPollStats stats;
  Check(!NativePollTextureOutdatedObserved(flag, true, stats,
      [&](Stage stage, bool value) noexcept {
        if (stage == Stage::kAfterLoad && !value) flag.store(true, std::memory_order_release);
      }), "producer after false load is deferred, not consumed");
  Check(flag.load() && stats.exchanges == 0, "false fast path preserves racing notification");
  Check(NativePollTextureOutdated(flag, true, stats), "next call consumes deferred notification");

  uint64_t payload = 10;
  flag.store(true, std::memory_order_release);
  Check(NativePollTextureOutdatedObserved(flag, true, stats,
      [&](Stage stage, bool value) noexcept {
        if (stage == Stage::kAfterLoad && value) {
          payload = 20;
          flag.store(true, std::memory_order_release);
        }
      }), "producer between true load and exchange is consumed");
  Check(payload == 20 && !flag.load(), "one consume covers preceding coalesced events");

  flag.store(true, std::memory_order_release);
  Check(NativePollTextureOutdatedObserved(flag, true, stats,
      [&](Stage stage, bool value) noexcept {
        if (stage == Stage::kAfterExchange && value) {
          payload = 30;
          flag.store(true, std::memory_order_release);
        }
      }), "original event consumed before later producer");
  Check(payload == 30 && flag.load(), "post-exchange notification remains pending");
  Check(NativePollTextureOutdated(flag, true, stats), "post-exchange event consumed next time");

  // Production has one consumer. Still require the result/counter to reflect
  // the exchange result, rather than mistakenly returning the earlier load.
  flag.store(true, std::memory_order_release);
  const uint64_t old_consumed = stats.consumed_notifications;
  const uint64_t old_exchanges = stats.exchanges;
  Check(!NativePollTextureOutdatedObserved(flag, true, stats,
      [&](Stage stage, bool) noexcept {
        if (stage == Stage::kAfterLoad) flag.exchange(false, std::memory_order_acquire);
      }), "exchange false cannot become a consume");
  Check(stats.exchanges == old_exchanges + 1 &&
            stats.consumed_notifications == old_consumed, "attempt and consume are distinct");
}

void ThreadedVisibilityControls(bool load_first) {
  constexpr uint32_t kRounds = 4096;
  std::atomic<bool> flag{false}, stop{false};
  std::atomic<uint32_t> acknowledged{0};
  std::array<uint64_t, 4> payload{};  // Non-atomic: publication must protect this.
  NativeTextureOutdatedPollStats stats;
  std::thread producer([&] {
    for (uint32_t n = 1; n <= kRounds; ++n) {
      while (acknowledged.load(std::memory_order_acquire) != n - 1) {
        if (stop.load(std::memory_order_relaxed)) return;
        std::this_thread::yield();
      }
      if (stop.load(std::memory_order_relaxed)) return;
      for (size_t lane = 0; lane < payload.size(); ++lane) {
        payload[lane] = (uint64_t(n) << 32) ^ (UINT64_C(0xF1357AEA2E62A9C5) + lane);
      }
      flag.store(true, std::memory_order_release);
    }
  });
  try {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (uint32_t n = 1; n <= kRounds; ++n) {
      while (!NativePollTextureOutdated(flag, load_first, stats)) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("producer timeout");
        std::this_thread::yield();
      }
      for (size_t lane = 0; lane < payload.size(); ++lane) {
        Check(payload[lane] == ((uint64_t(n) << 32) ^
                  (UINT64_C(0xF1357AEA2E62A9C5) + lane)), "acquire observes published payload");
      }
      // Prevent the next non-atomic write until every current payload read ends.
      acknowledged.store(n, std::memory_order_release);
    }
    producer.join();
  } catch (...) {
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    throw;
  }
  Check(!flag.load(), "all notified rounds consumed");
  Check(stats.exchanges == (load_first ? kRounds : 0) &&
            stats.consumed_notifications == (load_first ? kRounds : 0),
        "one acknowledged notification per round; off counters untouched");
}
}  // namespace

int main() {
  try {
    LiteralAndToggleControls();
    InterleavingControls();
    ThreadedVisibilityControls(false);
    ThreadedVisibilityControls(true);
    std::cout << "PASS " << checks << " checks / 4 groups\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
