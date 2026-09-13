/**
 ******************************************************************************
 * phase3_counters.h - Nuts & Bolts de-emulation Phase 3 named counter set.
 ******************************************************************************
 * Added by the rexgpu-nb plugin's vendored D3D12 backend. Not part of the
 * upstream RexGlue SDK (v0.10.0, BSD-3); the surrounding files retain the
 * SDK's license and notices.
 *
 * The set is the one named in kimi_swarm_digest section 2 item 6, modelled on
 * the SDK's own direct_resolve_attempt/success/fallback_count_ members
 * (include/rex/graphics/d3d12/render_target_cache.h:732-734).
 ******************************************************************************
 */

#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace rex::graphics {

// Per-frame counters for the Phase 3 A/B harness. They accumulate on the
// command processor thread and are reset after each JSON-lines dump at swap.
// Every wired site runs on that one thread (record time in
// D3D12CommandProcessor::IssueDraw, deferred-list Execute inside
// D3D12CommandProcessor::EndSubmission, RenderTargetCache::ChangeOwnership
// and D3D12RenderTargetCache::DumpRenderTargets during draws and copies, the
// bin-select packet cases in CommandProcessor::ExecutePacketType3), so plain
// uint64_t without atomics is sufficient.
struct Phase3Counters {
  // Phase 3 step 2. Copying EDRAM ownership transfers recorded by
  // RenderTargetCache::ChangeOwnership (append and extend sites).
  uint64_t transfer_count = 0;
  // Sum of (end_tiles - start_tiles) over those transfers, in 80x16-sample
  // EDRAM tiles.
  uint64_t transfer_tiles = 0;
  // D3D12RenderTargetCache::DumpRenderTargets calls that actually dumped
  // rectangles (the EDRAM dump half of every host-RT resolve).
  uint64_t resolve_dump_count = 0;
  // Tiling passes entered: PM4_SET_BIN_SELECT* writes while binning state
  // differs from the non-tiling defaults. Step 5 measures its saving against
  // this.
  uint64_t tile_pass_count = 0;
  // Draws executed in more than one tile pass. Wired by Phase 3 step 5; stays
  // zero until then.
  uint64_t duplicated_draw_across_passes_count = 0;
  // Phase 3 step 6. Draws that reached the pipeline bind while the D3D12
  // pipeline was still being created at record time; before step 6 these
  // draws were dropped (the removed early return in IssueDraw). Now they are
  // recorded and bound at Execute time.
  uint64_t record_time_pipeline_skip_count = 0;
  // kSetPipelineStateHandle commands whose handle still resolved to nullptr
  // at Execute time. PipelineCache::EndSubmission awaits all queued creations
  // before the deferred list executes, so a null here means pipeline creation
  // failed, not that it was still in flight.
  uint64_t execute_time_null_handle_count = 0;
  // Phase 3 step 3 resolve/swap alias registry. Wired by step 3; stays zero
  // until then.
  uint64_t alias_hit_count = 0;
  uint64_t alias_miss_count = 0;
  // Texture cache reloads from ranges covered by recorded resolve
  // destinations. Wired by step 3; stays zero until then.
  uint64_t texture_reload_from_resolved_range_count = 0;
  // readback_resolve path uses. Wired by step 3; stays zero until then.
  uint64_t readback_resolve_use_count = 0;
  // Wall time in microseconds spent inside PipelineCache::EndSubmission (the
  // pipeline creation await) from D3D12CommandProcessor::EndSubmission. This
  // is the residual step-6 stall.
  uint64_t swap_wait_time_us = 0;

  void Reset() { *this = Phase3Counters{}; }

  // One JSON object per line, stable key order matching digest item 6. All
  // values are numeric, so no escaping is needed.
  void WriteJsonLine(std::FILE* out, uint32_t frame) const {
    std::fprintf(out,
                 "{\"frame\":%" PRIu32 ",\"transfers\":%" PRIu64
                 ",\"transfer_tiles\":%" PRIu64 ",\"resolve_dumps\":%" PRIu64
                 ",\"tile_passes\":%" PRIu64
                 ",\"duplicated_draws_across_passes\":%" PRIu64
                 ",\"record_time_pipeline_skips\":%" PRIu64
                 ",\"execute_time_null_handle_skips\":%" PRIu64
                 ",\"alias_hits\":%" PRIu64 ",\"alias_misses\":%" PRIu64
                 ",\"texture_reloads_from_resolved_ranges\":%" PRIu64
                 ",\"readback_resolve_uses\":%" PRIu64
                 ",\"swap_wait_time_us\":%" PRIu64 "}\n",
                 frame, transfer_count, transfer_tiles, resolve_dump_count, tile_pass_count,
                 duplicated_draw_across_passes_count, record_time_pipeline_skip_count,
                 execute_time_null_handle_count, alias_hit_count, alias_miss_count,
                 texture_reload_from_resolved_range_count, readback_resolve_use_count,
                 swap_wait_time_us);
  }
};

// The single instance. Only safe because all writers are on the command
// processor thread (see the struct comment).
inline Phase3Counters& GetPhase3Counters() {
  static Phase3Counters counters;
  return counters;
}

}  // namespace rex::graphics
