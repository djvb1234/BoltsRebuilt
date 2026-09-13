// Original nb conservative mirror of completed shared-memory residency checks.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace nb::gpu {

struct NativeSharedResidencyStats {
  uint64_t requests = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t hit_ranges = 0;
  uint64_t hit_bytes = 0;
  uint64_t hit_pages = 0;
  uint64_t promoted_pages = 0;
  uint64_t recursive_bypasses = 0;
  uint64_t unsupported_page_bypasses = 0;
  uint64_t unsupported_caller_bypasses = 0;
  uint64_t initialization_failures = 0;
  uint64_t mode_resets = 0;
};

// CP-thread cumulative counters. Invalidation callbacks do not modify them.
const NativeSharedResidencyStats& GetNativeSharedResidencyStats() noexcept;

// A bit means that a PREEXISTING valid page was observed under the SDK's global
// lock, after every requested backing allocation succeeded. It never means
// merely that MakeRangeValid ran: that happens before the CPU upload copy.
//
// Contract:
// * Only the owning CP thread promotes, during a nonrecursive RequestRanges.
// * Every authority invalidation clears these bits BEFORE changing live state.
// * Resource/frame/mode/lifetime resets clear all bits directly.
// * Recursive requests neither read this mirror nor promote into it.
// During a fast check no thread can promote. All-hit reads across multiple
// words therefore have a linearization point at the first read: bits observed
// in later words were already set then, since concurrent operations only clear.
// The authoritative validity vectors are never read outside their SDK lock.
class NativeSharedResidencyMirror {
 public:
  using Range = std::pair<uint32_t, uint32_t>;
  static constexpr uint32_t kPageLog2 = 12;
  static constexpr uint32_t kPageBytes = 1u << kPageLog2;
  static constexpr uint32_t kBufferBytes = 1u << 29;
  static constexpr uint32_t kPageCount = kBufferBytes >> kPageLog2;
  static constexpr uint32_t kWordCount = kPageCount / 64;

  NativeSharedResidencyMirror() noexcept = default;
  NativeSharedResidencyMirror(const NativeSharedResidencyMirror&) = delete;
  NativeSharedResidencyMirror& operator=(const NativeSharedResidencyMirror&) = delete;

  bool enabled() const noexcept { return !disabled_.load(std::memory_order_seq_cst); }

  bool Contains(std::span<const Range> ranges) const noexcept {
    if (ranges.empty() || !enabled()) return false;
    for (const auto& range : ranges) {
      if (!range.second || range.first >= kBufferBytes ||
          range.second > kBufferBytes - range.first) return false;
    }
    for (const auto& range : ranges) {
      const uint32_t first = range.first >> kPageLog2;
      const uint32_t last = (range.first + range.second - 1) >> kPageLog2;
      for (uint32_t word = first / 64; word <= last / 64; ++word) {
        const uint64_t mask = PageMask(word, first, last);
        if ((words_[word].load(std::memory_order_seq_cst) & mask) != mask) return false;
      }
    }
    return true;
  }

  // Must be called under the authoritative scan lock, after sparse allocation,
  // using the original valid word and only this request's actual page mask.
  uint32_t PromoteValidWord(uint32_t word, uint64_t live_valid,
                            uint64_t requested_mask) noexcept {
    if (word >= kWordCount || !enabled()) return 0;
    const uint64_t bits = live_valid & requested_mask;
    if (!bits) return 0;
    const uint64_t previous = words_[word].fetch_or(bits, std::memory_order_seq_cst);
    return uint32_t(std::popcount(bits & ~previous));
  }

  void ClearWord(uint32_t word, uint64_t mask) noexcept {
    if (word < kWordCount) words_[word].fetch_and(~mask, std::memory_order_seq_cst);
  }

  void ClearPages(uint32_t first, uint32_t last) noexcept {
    if (first > last || first >= kPageCount) return;
    if (last >= kPageCount) last = kPageCount - 1;
    for (uint32_t word = first / 64; word <= last / 64; ++word) {
      ClearWord(word, PageMask(word, first, last));
    }
  }

  void Reset() noexcept {
    for (auto& word : words_) word.store(0, std::memory_order_seq_cst);
  }

  // An unsupported caller permanently disables this owner until destruction.
  // Ordinary resets/mode toggles must not accidentally re-enable that owner.
  void Disable() noexcept {
    disabled_.store(true, std::memory_order_seq_cst);
    Reset();
  }

 private:
  static uint64_t PageMask(uint32_t word, uint32_t first, uint32_t last) noexcept {
    uint64_t mask = UINT64_MAX;
    if (word == first / 64) mask &= UINT64_MAX << (first & 63);
    if (word == last / 64) mask &= UINT64_MAX >> (63 - (last & 63));
    return mask;
  }

  std::array<std::atomic<uint64_t>, kWordCount> words_{};
  std::atomic<bool> disabled_{false};
};

// Used even while disabled so a hot-reload during UploadRanges cannot make its
// recursive shadow retry look like an outer, completed request.
class NativeSharedResidencyRequestScope {
 public:
  explicit NativeSharedResidencyRequestScope(uint32_t* owner_depth) noexcept : depth_(owner_depth) {
    if (depth_) ++*depth_;
  }
  ~NativeSharedResidencyRequestScope() { if (depth_) --*depth_; }
  bool outer() const noexcept { return depth_ && *depth_ == 1; }
  NativeSharedResidencyRequestScope(const NativeSharedResidencyRequestScope&) = delete;
  NativeSharedResidencyRequestScope& operator=(const NativeSharedResidencyRequestScope&) = delete;

 private:
  uint32_t* depth_;
};

}  // namespace nb::gpu
