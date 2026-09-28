// Original count-only inventory of the places where the command processor
// thread ends a submission or blocks on the GPU. Every figure is updated on the
// command processor thread, and the clocks run only around waits that already
// block, so nothing here is read per draw.
#pragma once

#include <chrono>
#include <cstdint>

namespace rex::graphics::d3d12 {

struct NativeGpuWaitStats {
  // ExecuteCommandLists submissions by the reason EndSubmission ran.
  uint64_t submissions_swap = 0;
  uint64_t submissions_primary_buffer_end = 0;
  uint64_t submissions_occlusion_query = 0;
  uint64_t submissions_other = 0;

  // Blocking fence waits. "Frame latency" is BeginSubmission awaiting the frame
  // kQueueFrames back; "drain" is any other wait, such as an occlusion query
  // result, a readback or an AwaitAllQueueOperationsCompletion.
  uint64_t frame_latency_waits = 0;
  uint64_t frame_latency_wait_ns = 0;
  uint64_t drain_waits = 0;
  uint64_t drain_wait_ns = 0;

  // Pipeline creation awaited by PipelineCache::EndSubmission, per submission
  // that actually waited (the Phase 3 swap_wait_time_us figure, always counted).
  uint64_t pipeline_waits = 0;
  uint64_t pipeline_wait_ns = 0;

  // Guest occlusion queries (EVENT_WRITE_ZPD on the host query path).
  uint64_t occlusion_begins = 0;
  // Ends answered before the packet returned: submit, drain, write.
  uint64_t occlusion_sync_ends = 0;
  uint64_t occlusion_sync_ns = 0;
  // nb_occlusion_query_deferred: ends left for the fence to answer, results
  // written when their submission completed, and results that had to be
  // awaited (reused guest address, reused host slot, idle command processor,
  // shutdown or switching host queries off).
  uint64_t occlusion_deferred_ends = 0;
  uint64_t occlusion_deferred_results = 0;
  uint64_t occlusion_deferred_forced = 0;
};

inline NativeGpuWaitStats& GetNativeGpuWaitStats() {
  // Command processor thread only, like GetNativeSubmissionStats.
  static thread_local NativeGpuWaitStats stats;
  return stats;
}

inline uint64_t NativeGpuWaitNowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace rex::graphics::d3d12
