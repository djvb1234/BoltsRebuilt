// Original bounded byte shadow for the last CPU upload queued to shared memory.
// Synchronization and D3D12 command ordering belong to the caller.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace nb::gpu {

class NativeUploadShadow {
 public:
  static constexpr uint32_t kPhysicalBytes = 1u << 29;
  static constexpr size_t kDefaultCapacityBytes = 64u << 20;
  static constexpr size_t kSnapshotBytes = 64u << 10;

  // Only full host pages, 4 KiB through 64 KiB, are cached. Failure disables it.
  bool Initialize(uint32_t page_bytes,
                  size_t capacity_bytes = kDefaultCapacityBytes) noexcept;
  void Reset() noexcept;
  // Inclusive byte endpoints. Even a partial GPU write destroys the identity
  // of every intersecting page. Every call also invalidates publication tokens.
  void Invalidate(uint32_t first, uint32_t last) noexcept;
  uint64_t epoch() const { return epoch_; }
  bool initialized() const { return page_bytes_ != 0; }
  uint32_t page_bytes() const { return page_bytes_; }

  bool Matches(uint32_t address, std::span<const uint8_t> bytes) const noexcept;
  // Call only after queueing a copy of these exact captured bytes. A GPU write
  // or full reset between snapshot and publication rejects the old token.
  bool Publish(uint32_t address, std::span<const uint8_t> bytes,
               uint64_t captured_epoch) noexcept;
  uint64_t evictions() const { return evictions_; }

 private:
  static constexpr uint32_t kAbsent = UINT32_MAX;
  struct Slot { uint32_t page = kAbsent; bool valid = false; };
  bool GetPage(uint32_t address, size_t size, uint32_t& page) const noexcept;
  void AdvanceEpoch() noexcept;
  uint32_t page_bytes_ = 0;
  uint32_t next_slot_ = 0;
  uint64_t epoch_ = 1, evictions_ = 0;
  std::vector<uint32_t> page_to_slot_;
  std::vector<Slot> slots_;
  std::unique_ptr<uint8_t[]> bytes_;
};

// Power-of-two page buckets for original upload copies: 1, 2-3, ..., 512+ pages.
inline constexpr size_t kNativeUploadSizeBuckets = 10;

// Written on the command processor thread. Counts are cumulative and clocks
// are intentionally absent from the per-page comparison path.
struct NativeUploadShadowStats {
  uint64_t enabled_calls = 0, disabled_calls = 0, initialization_failures = 0;
  uint64_t reserved_bytes = 0, snapshot_bytes = 0, compared_pages = 0;
  uint64_t matched_pages = 0, copied_bytes = 0, skipped_bytes = 0;
  uint64_t copy_commands = 0, all_matched_chunks = 0;
  uint64_t authority_fallback_bytes = 0, publication_rejects = 0, evictions = 0;
  uint64_t invalidation_retries = 0, retry_allocation_failures = 0;
  uint64_t original_copy_commands = 0, original_copy_bytes = 0;
  uint64_t lock_wait_ns = 0, authority_ns = 0, compare_ns = 0;
  uint64_t snapshot_ns = 0, upload_ns = 0;
  uint64_t original_size_counts[kNativeUploadSizeBuckets] = {};
  uint64_t original_size_bytes[kNativeUploadSizeBuckets] = {};
};
// Implemented by the D3D12 integration; read on the command processor thread.
const NativeUploadShadowStats& GetNativeUploadShadowStats();

}  // namespace nb::gpu
