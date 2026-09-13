// Original nb bounded range storage and upload-page union helpers.
// Byte validation/merge semantics follow ReXGlue SharedMemory (Xenia, BSD-3-Clause).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace nb::gpu {

enum class NativeSingleRangeKind { kNotSelected, kEmpty, kInvalid, kReady };

struct NativeSingleRangeSelection {
  using Range = std::pair<uint32_t, uint32_t>;
  NativeSingleRangeKind kind = NativeSingleRangeKind::kNotSelected;
  Range range{};
  std::span<const Range> ranges() const {
    return kind == NativeSingleRangeKind::kReady ? std::span<const Range>{&range, 1}
                                                : std::span<const Range>{};
  }
};

// Snapshot just the caller's one range before allocation, like the original
// batch path. No scratch array or heap storage is constructed. Empty ranges
// intentionally succeed even with an out-of-range start, matching Build below.
// The caller still performs the unchanged allocation and locked validity scan.
inline NativeSingleRangeSelection SelectNativeSingleRange(
    const NativeSingleRangeSelection::Range* input, size_t count,
    uint32_t byte_limit, bool enabled) {
  if (!enabled || !input || count != 1) return {};
  const auto [start, length] = *input;
  if (!length) return {NativeSingleRangeKind::kEmpty, {}};
  if (uint64_t(start) + length > byte_limit) {
    return {NativeSingleRangeKind::kInvalid, {}};
  }
  return {NativeSingleRangeKind::kReady, {start, length}};
}

class NativeRangeBatch {
 public:
  using Range = std::pair<uint32_t, uint32_t>;  // start, non-inclusive length
  static constexpr size_t kStackCapacity = 32;

  // The caller still performs sparse allocation and checks current valid bits
  // under the SDK lock. This only validates and merges requested byte ranges.
  bool Build(const Range* input, size_t count, uint32_t byte_limit, bool optimize) {
    size_ = 0;
    heap_.clear();
    on_stack_ = optimize && count <= kStackCapacity;
    if (!input || !count) return true;
    if (!on_stack_) heap_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto [start, length] = input[i];
      if (!length) continue;
      if (start > byte_limit || length > byte_limit - start) {
        size_ = 0;
        return false;
      }
      if (on_stack_) stack_[size_] = input[i];
      else heap_.push_back(input[i]);
      ++size_;
    }
    if (size_ < 2) return true;
    Range* data = on_stack_ ? stack_.data() : heap_.data();
    std::sort(data, data + size_, [](const Range& a, const Range& b) { return a.first < b.first; });
    size_t output = 0;
    for (size_t i = 1; i < size_; ++i) {
      Range& previous = data[output];
      const Range next = data[i];
      const uint64_t previous_end = uint64_t(previous.first) + previous.second;
      if (next.first <= previous_end) {
        const uint64_t end = std::max(previous_end, uint64_t(next.first) + next.second);
        previous.second = static_cast<uint32_t>(end - previous.first);
      } else {
        data[++output] = next;
      }
    }
    size_ = output + 1;
    return true;
  }

  std::span<const Range> ranges() const {
    return {on_stack_ ? stack_.data() : heap_.data(), size_};
  }
  bool on_stack() const { return on_stack_; }

 private:
  std::array<Range, kStackCapacity> stack_;
  std::vector<Range> heap_;
  size_t size_ = 0;
  bool on_stack_ = false;
};

// Input page ranges come from the SDK's sorted, merged byte ranges. Starts are
// monotonic, but disjoint byte ranges can still name the same boundary page.
// Return the count of duplicate pages omitted. Legacy mode merges adjacency
// only, retaining the previous SDK append behavior for same-run comparison.
inline uint32_t AppendUploadPageRange(std::vector<NativeRangeBatch::Range>& output,
                                     uint32_t start, uint32_t count, bool optimize) {
  if (!count) return 0;
  if (!output.empty()) {
    auto& last = output.back();
    const uint64_t last_end = uint64_t(last.first) + last.second;
    const uint64_t end = uint64_t(start) + count;
    if (start == last_end || (optimize && start >= last.first && start < last_end)) {
      const uint32_t overlap = start < last_end
          ? static_cast<uint32_t>(std::min(last_end, end) - start) : 0;
      last.second = static_cast<uint32_t>(std::max(last_end, end) - last.first);
      return overlap;
    }
  }
  output.emplace_back(start, count);
  return 0;
}

// Written and read on the command processor thread only. Counters are collected
// only when nb_range_batch_diagnostics is enabled; no per-call clocks/logging.
struct NativeRangeBatchStats {
  uint64_t calls = 0, optimized_calls = 0, legacy_calls = 0;
  // Selected single-range calls bypass both stack and heap batch construction.
  uint64_t single_fast_calls = 0;
  uint64_t stack_calls = 0, heap_calls = 0, invalid_requests = 0;
  uint64_t input_ranges = 0, merged_ranges = 0, page_visits = 0;
  uint64_t overlap_events = 0, overlap_pages_saved = 0;
  uint64_t upload_calls = 0, no_upload_calls = 0, upload_pages = 0;
};
const NativeRangeBatchStats& GetNativeRangeBatchStats();

}  // namespace nb::gpu
