// Original optional consume of the existing texture-invalidation flag.
// There is one command-thread consumer; producers release-store true after
// invalidating texture state. False loads never acknowledge a producer.
#pragma once

#include <atomic>
#include <cstdint>

namespace nb::gpu {

struct NativeTextureOutdatedPollStats {
  uint64_t false_load_skips = 0;
  uint64_t exchanges = 0;
  uint64_t consumed_notifications = 0;
};

// One plugin definition, written/read only by the command thread. These count
// enabled calls only; consumed_notifications counts true exchanges, not writes
// by producers (several invalidations may coalesce into one true flag).
const NativeTextureOutdatedPollStats& GetNativeTextureOutdatedPollStats() noexcept;

namespace detail {
enum class NativeTexturePollStage { kAfterLoad, kAfterExchange };
struct NativeTexturePollNoObserver {
  void operator()(NativeTexturePollStage, bool) const noexcept {}
};

// The no-op observer is eliminated in production. Controls instantiate this
// same body with deterministic intervening producer events, without timing races.
template <typename Observer>
inline bool NativePollTextureOutdatedObserved(
    std::atomic<bool>& flag, bool load_first,
    NativeTextureOutdatedPollStats& stats, Observer observer) noexcept {
  if (!load_first) return flag.exchange(false, std::memory_order_acquire);

  const bool pending = flag.load(std::memory_order_acquire);
  observer(NativeTexturePollStage::kAfterLoad, pending);
  if (!pending) {
    ++stats.false_load_skips;
    return false;
  }
  ++stats.exchanges;
  const bool consumed = flag.exchange(false, std::memory_order_acquire);
  observer(NativeTexturePollStage::kAfterExchange, consumed);
  if (consumed) ++stats.consumed_notifications;
  return consumed;
}
}  // namespace detail

inline bool NativePollTextureOutdated(std::atomic<bool>& flag, bool load_first,
                                    NativeTextureOutdatedPollStats& stats) noexcept {
  return detail::NativePollTextureOutdatedObserved(
      flag, load_first, stats, detail::NativeTexturePollNoObserver{});
}

}  // namespace nb::gpu
