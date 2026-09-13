// Original bounded cap for excess CPU write invalidation. No SDK or OS state.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace nb::gpu {
struct NativeInvalidationRange {
  uint32_t first, last;  // Inclusive host page indices.
  bool operator==(const NativeInvalidationRange&) const = default;
};

// The existing GPU-written-neighbor exclusion has already produced original.
// Intersect only its excess with the 64KiB blocks containing the actual write.
// Never remove an actual requested page or add a page excluded by the SDK.
// Disabled/exact requests retain the original range byte-for-byte. Unusual
// malformed inputs also retain that path rather than inventing new authority.
inline NativeInvalidationRange CapNativeInvalidationRange(
    NativeInvalidationRange requested, NativeInvalidationRange original,
    uint32_t host_page_size_log2, bool exact, bool enabled) noexcept {
  if (!enabled || exact || host_page_size_log2 >= 32 ||
      requested.first > requested.last || original.first > requested.first ||
      original.last < requested.last) return original;
  const uint32_t block_log2 = host_page_size_log2 < 16 ? 16 - host_page_size_log2 : 0;
  const uint32_t block_mask = (uint32_t(1) << block_log2) - 1;
  return {std::max(original.first, requested.first & ~block_mask),
          std::min(original.last, requested.last | block_mask)};
}

inline uint64_t NativeInvalidationPageCount(NativeInvalidationRange range) noexcept {
  return range.first <= range.last ? uint64_t(range.last) - range.first + 1 : 0;
}

inline uint64_t NativeInvalidationPageMask(NativeInvalidationRange range,
                                          uint32_t block) noexcept {
  if (range.first > range.last || block < (range.first >> 6) || block > (range.last >> 6)) return 0;
  uint64_t mask = UINT64_MAX;
  if (block == (range.first >> 6)) mask &= UINT64_MAX << (range.first & 63);
  if (block == (range.last >> 6)) mask &= UINT64_MAX >> (63 - (range.last & 63));
  return mask;
}

inline uint32_t NativeRetainedCpuPages(NativeInvalidationRange original,
                                      NativeInvalidationRange selected, uint32_t block,
                                      uint64_t valid, uint64_t gpu_written) noexcept {
  return uint32_t(std::popcount(valid & ~gpu_written & NativeInvalidationPageMask(original, block) &
                               ~NativeInvalidationPageMask(selected, block)));
}

struct NativeInvalidationStats {
  // Callback threads update these only under the SDK global critical region.
  // Counts include repeated invalidation, not unique pages or measured savings.
  uint64_t callbacks = 0, exact_callbacks = 0, enabled_callbacks = 0, narrowed_callbacks = 0;
  uint64_t requested_pages = 0, original_pages = 0, selected_pages = 0;
  uint64_t retained_cpu_pages = 0, potential_retained_cpu_pages = 0;
};

// Returns an owned snapshot under that same global lock; never a live reference.
// Call before taking logger/other locks, following the SDK global-lock order.
NativeInvalidationStats SnapshotNativeInvalidationStats();
}  // namespace nb::gpu
