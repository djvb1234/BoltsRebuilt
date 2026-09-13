// Original exact-page classifier. Caller must hold the CPU authority guard.
#pragma once
#include "native_upload_shadow.h"
#include <chrono>
#include <cstring>

namespace nb::gpu {
inline uint64_t UploadClock(bool enabled) {
  return enabled ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()) : 0;
}
struct UploadClassification {
  uint32_t changed_mask = 0, matched_pages = 0;
  uint64_t snapshot_bytes = 0, compare_ns = 0, snapshot_ns = 0;
};
// At most one 64 KiB chunk, consisting of complete 4-64 KiB pages.
// Old mode receives an already captured scratch span. Direct mode captures
// only misses, at their original offsets; untouched scratch must not be read.
inline UploadClassification ClassifyUploadPages(const NativeUploadShadow& shadow,
    uint32_t address, std::span<const uint8_t> source, uint8_t* scratch,
    bool direct, bool clocks) {
  UploadClassification result;
  const uint32_t page_bytes = shadow.page_bytes();
  for (uint32_t page = 0; page < source.size() / page_bytes; ++page) {
    const size_t offset = size_t(page) * page_bytes;
    const auto bytes = source.subspan(offset, page_bytes);
    uint64_t begin = UploadClock(clocks);
    const bool match = shadow.Matches(address + uint32_t(offset), bytes);
    result.compare_ns += UploadClock(clocks) - begin;
    if (match) { ++result.matched_pages; continue; }
    result.changed_mask |= 1u << page;
    if (direct) {
      begin = UploadClock(clocks);
      std::memcpy(scratch + offset, bytes.data(), page_bytes);
      result.snapshot_ns += UploadClock(clocks) - begin;
      result.snapshot_bytes += page_bytes;
    }
  }
  return result;
}
}  // namespace nb::gpu
