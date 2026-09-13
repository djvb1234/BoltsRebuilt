// Original nb diagnostics for shared-memory residency requests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nb::gpu {

// Attribution is an explicit dynamic scope, not a guessed stack symbol. A
// nested texture request overrides a surrounding native hook and restores it.
enum class NativeRangeCaller : size_t {
  kOther,
  kIndexProcessing,
  kTextureRequest,
  kNativeHook,
  kCount,
};

struct NativeRangeCallerStats {
  uint64_t calls = 0;
  uint64_t top_level_calls = 0;
  uint64_t input_ranges = 0;
  uint64_t normalized_ranges = 0;
  uint64_t requested_bytes = 0;
  uint64_t requested_page_visits = 0;
  uint64_t empty_calls = 0;
  uint64_t invalid_calls = 0;
  uint64_t allocation_failures = 0;
  uint64_t no_upload_calls = 0;
  uint64_t upload_calls = 0;
  uint64_t upload_failures = 0;
  uint64_t aborted_calls = 0;
  // Planned upload spans, not actual CopyBufferRegion bytes. Upload shadow
  // retries or failures can change the work performed by UploadRanges.
  uint64_t upload_range_count = 0;
  uint64_t upload_page_count = 0;
  // total_ns is inclusive of recursive RequestRanges (shadow retries).
  // top_level_ns counts only outer calls and avoids that overlap.
  uint64_t total_ns = 0;
  uint64_t top_level_ns = 0;
  uint64_t no_upload_ns = 0;
  uint64_t upload_ns = 0;
  // Only the original validity-scan lock is timed here. Acquisition includes
  // uncontended API overhead; it is not proof of contention. Hold time ends
  // immediately before unlocking and therefore excludes the unlock API.
  uint64_t lock_calls = 0;
  uint64_t lock_acquire_ns = 0;
  uint64_t lock_hold_ns = 0;
};

struct NativeRangeDiagnosticsStats {
  std::array<NativeRangeCallerStats, size_t(NativeRangeCaller::kCount)> callers{};
};

// These diagnostics and caller scopes belong to the calling CP thread. Read
// the cumulative counters on that same thread; no cross-thread snapshot or
// global-lock acquisition is added to the profiling path.
bool NativeRangeDiagnosticsEnabled();
const NativeRangeDiagnosticsStats& GetNativeRangeDiagnosticsStats() noexcept;

class NativeRangeCallerScope {
 public:
  NativeRangeCallerScope(NativeRangeCaller caller, bool enabled) noexcept;
  ~NativeRangeCallerScope();
  NativeRangeCallerScope(const NativeRangeCallerScope&) = delete;
  NativeRangeCallerScope& operator=(const NativeRangeCallerScope&) = delete;

 private:
  bool enabled_;
  NativeRangeCaller previous_ = NativeRangeCaller::kOther;
};

}  // namespace nb::gpu
