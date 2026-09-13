// Original opt-in host submission diagnostics. These are CPU elapsed times,
// including any driver waits inside the calls, rather than GPU execution times.
#pragma once

#include <chrono>
#include <cstdint>

namespace rex::graphics::d3d12 {

struct NativeSubmissionCallStats {
  uint64_t calls = 0;
  uint64_t nanoseconds = 0;
  uint64_t failures = 0;
};

struct NativeSubmissionStats {
  NativeSubmissionCallStats allocator_reset;
  NativeSubmissionCallStats list_reset;
  NativeSubmissionCallStats replay;
  NativeSubmissionCallStats close;
  NativeSubmissionCallStats execute;
  // Exact host event waits only; excludes submitting pending lists and all
  // post-wait reclamation. May nest within IssueDraw/IssueCopy/IssueSwap clocks.
  NativeSubmissionCallStats fence_wait;
  NativeSubmissionCallStats queue_operation_wait;

  // Optional count-only external pipeline pointer changes, independent of the
  // more expensive per-submission timers. Includes texture/resolve pipelines.
  uint64_t external_pipeline_binds = 0;
  uint64_t external_pipeline_reuses = 0;

  // Successfully recorded nonempty barrier batches. These count commands
  // queued for later replay, not completed GPU barriers.
  uint64_t barrier_batches = 0;
  uint64_t barriers_total = 0;
  uint64_t max_barriers_per_batch = 0;
  uint64_t transition_barriers = 0;
  uint64_t aliasing_barriers = 0;
  uint64_t uav_barriers = 0;
  uint64_t unknown_barriers = 0;
  uint64_t shared_memory_transitions = 0;
  uint64_t shared_memory_to_copy_dest = 0;
  uint64_t shared_memory_from_copy_dest = 0;

  // Same-IssueDraw transition evidence, not a claim that reordering is safe or
  // that a GPU wait was removed. "Texture read" means RequestTextures queued a
  // shared-buffer transition into NON_PIXEL_SHADER_RESOURCE; unchanged read
  // state calls do not count. Draw counts below require native-hook success.
  uint64_t upload_order_native_attempts = 0;
  uint64_t upload_order_native_draws = 0;
  uint64_t upload_order_texture_read_draws = 0;
  uint64_t upload_order_copy_dest_draws = 0;
  uint64_t upload_order_candidate_draws = 0;
  uint64_t upload_order_late_copy_dest_transitions = 0;
  uint64_t upload_order_candidate_cycles = 0;  // late COPY_DEST followed by native shader-read transition
  uint64_t upload_order_refused_candidate_draws = 0;
  uint64_t upload_order_attribution_bypasses = 0;  // nested draw or possible duplicate texture request in native hook
};

inline NativeSubmissionStats& GetNativeSubmissionStats() {
  // Updates and snapshots belong to the command processor thread. A different
  // thread gets independent counters, so logging must run on the CP thread.
  static thread_local NativeSubmissionStats stats;
  return stats;
}

class NativeSubmissionTimer {
 public:
  explicit NativeSubmissionTimer(NativeSubmissionCallStats* stats) noexcept : stats_(stats) {
    if (stats_) {
      ++stats_->calls;
      start_ = std::chrono::steady_clock::now();
    }
  }
  ~NativeSubmissionTimer() noexcept {
    if (stats_) {
      stats_->nanoseconds += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start_).count());
    }
  }
  NativeSubmissionTimer(const NativeSubmissionTimer&) = delete;
  NativeSubmissionTimer& operator=(const NativeSubmissionTimer&) = delete;

  // Only HRESULT-returning calls report failures. The existing caller decides
  // what to do with the result; diagnostics never change failure handling.
  void RecordFailure(bool failed) noexcept {
    if (stats_ && failed) ++stats_->failures;
  }

 private:
  NativeSubmissionCallStats* stats_;
  std::chrono::steady_clock::time_point start_{};
};

}  // namespace rex::graphics::d3d12
