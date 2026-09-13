#include "nb_command_processor.h"
#include "native/nb_ultrawide_bridge.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/native_command_wait_stats.h>
#include <rex/graphics/d3d12/native_submission_stats.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "nb_cp_records.h"
#include "nb_graphics_system.h"
#include "native/offline_texture_cache.h"
#include "native/native_range_batch.h"
#include "native/native_range_diagnostics.h"
#include "native/native_shared_residency_mirror.h"
#include "native/native_invalidation_range.h"
#include "native/native_shader_cache.h"
#include "native/native_timing.h"
#include "native/native_constant_upload_diagnostics.h"
#include "native/native_frame_trace.h"
#include "native/native_upload_shadow.h"
#include "native/native_texture_descriptor_memo.h"
#include "native/native_texture_outdated_poll.h"
#include "native/native_wait_poll.h"
#include "native/native_translation_lookup_memo.h"

#if NB_HAS_RUNTIME_WATCH_CONTROL
#include "../runtime/watch_control.h"
#endif

REXCVAR_DECLARE(bool, nb_native_defer_guest_pipeline);
REXCVAR_DECLARE(bool, nb_native_skip_guest_pipeline);
REXCVAR_DECLARE(bool, nb_range_batch_optimize);
REXCVAR_DECLARE(bool, nb_range_batch_diagnostics);
REXCVAR_DECLARE(bool, nb_native_static_bindings);
REXCVAR_DECLARE(bool, nb_native_constant_reuse);
REXCVAR_DECLARE(bool, nb_native_unused_pixel_constants);
REXCVAR_DECLARE(bool, nb_native_texture_outdated_load_first);
REXCVAR_DECLARE(int32_t, nb_native_wait_spin_us);
REXCVAR_DECLARE(bool, nb_native_wait_target_diagnostics);
REXCVAR_DECLARE(bool, nb_native_register_fastpath);
REXCVAR_DECLARE(bool, nb_native_hardware_indices);
REXCVAR_DECLARE(bool, nb_native_minimal_diagnostics);
REXCVAR_DECLARE(bool, nb_shader_load_memo);
REXCVAR_DECLARE(bool, nb_native_avx2_shader_hash);
REXCVAR_DECLARE(bool, nb_native_async_replay);
REXCVAR_DECLARE(int32_t, nb_native_replay_chunk_draws);
REXCVAR_DECLARE(bool, nb_native_upload_shadow);
REXCVAR_DECLARE(bool, nb_native_pair_lookup_memo);
REXCVAR_DECLARE(bool, nb_native_precise_wait);
REXCVAR_DECLARE(bool, execute_unclipped_draw_vs_on_cpu);
REXCVAR_DECLARE(bool, nb_native_texture_descriptor_memo);
REXCVAR_DECLARE(bool, nb_native_translation_lookup_memo);
REXCVAR_DECLARE(bool, nb_native_stage_vertex_constants);
REXCVAR_DECLARE(bool, nb_native_vertex_constant_reuse);
REXCVAR_DECLARE(bool, nb_native_pipeline_key_normalize);
REXCVAR_DECLARE(bool, nb_native_shared_residency_mirror);
REXCVAR_DECLARE(bool, nb_native_empty_constant_layout);
REXCVAR_DECLARE(bool, nb_native_constant_binding_packet);
REXCVAR_DECLARE(bool, nb_native_constant_upload_diagnostics);
REXCVAR_DECLARE(bool, nb_native_double_pixel_packets);
REXCVAR_DECLARE(bool, nb_native_deferred_storage);
REXCVAR_DECLARE(bool, nb_native_submission_diagnostics);
REXCVAR_DECLARE(bool, nb_native_pipeline_binding_diagnostics);
REXCVAR_DECLARE(bool, nb_native_root_cbv);
REXCVAR_DECLARE(bool, nb_range_single_fastpath);
REXCVAR_DECLARE(bool, nb_native_pixel_binding_cache);
REXCVAR_DECLARE(bool, nb_native_early_wait_poll);
REXCVAR_DECLARE(bool, nb_native_narrow_invalidation);
REXCVAR_DECLARE(bool, nb_native_invalidation_diagnostics);
REXCVAR_DECLARE(int32_t, anisotropic_override);
REXCVAR_DEFINE_BOOL(nb_native_minimal_command_diagnostics, false, "nb",
                    "Omit per-command shader/draw/copy and generic CPU clocks; retain wall-frame and swap timing")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_thread_cpu_diagnostics, false, "nb",
                    "Report cumulative command-thread CPU time at performance log boundaries without per-draw clocks")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(nb_native_frame_trace_file, "", "nb",
                     "Write a bounded guest-swap frame trace CSV after its capture interval; empty disables it");
REXCVAR_DEFINE_DOUBLE(nb_native_frame_trace_start_seconds, 90.0, "nb",
                     "Start the frame trace this many steady-clock seconds after the first swap");
REXCVAR_DEFINE_DOUBLE(nb_native_frame_trace_duration_seconds, 100.0, "nb",
                     "Capture this many seconds of frame rows, retaining at most 60000 rows");
REXCVAR_DEFINE_BOOL(nb_native_texture_binding_cache, false, "nb",
                    "Reuse immutable shader texture candidates and exact-input SDK sampler parameters; resolve texture descriptors live")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_gpu_pcores, false, "nb",
                    "Restrict the GPU command thread to permitted performance cores on supported hybrid CPUs; diagnostic until measured")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(nb_native_gpu_placement, 0, "nb",
                    "GPU scheduling experiment: 0 P cores, 1 prefer highest, 2 pin highest, 3 pin lowest; 4..7 repeat over the highest observed scheduling-class subset")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_effective_samplers, true, "nb",
                    "Compile generated shaders for the single effective SDK sampler per slot; false is the equivalent diagnostic baseline");
REXCVAR_DEFINE_BOOL(nb_native_direct_guest_reads, false, "nb",
                    "Specialize shaders to the same guest-memory loads when geometry asset matching is inactive")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_constant_gpu_upload, false, "nb",
                    "Serve native per-draw constant slices from a bounded CPU-writable video-memory pool "
                    "(D3D12 GPU_UPLOAD); unsupported adapters, a full budget or any failure keep ordinary upload heaps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_watch_fastpath, false, "nb",
                    "Use the reviewed all-watched runtime shortcut when the project-owned runtime provides it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(nb_native_immutable_watch_fastpath, false, "nb",
                    "Skip effect-free notification setup for read-only or inaccessible coarse guest pages")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT64(nb_native_perf_benchmark_start, 0, "nb",
                     "First frame of the native performance comparison; prewarms both shader forms from frame 3600, 0 disables");
REXCVAR_DEFINE_STRING(nb_native_perf_benchmark_options, "1279,3327,5375,7423,7423,5375,3327,1279", "nb",
                      "Comma-separated performance bitmasks (2 to 32 windows); fixed at benchmark startup");

REXCVAR_DEFINE_BOOL(nb_native_demo, false, "nb",
                    "Take over the draws of the pixel shader named by nb_native_demo_ps with a native "
                    "fullscreen pattern drawn into the same render target (the proof-of-seam pass)");
REXCVAR_DEFINE_STRING(nb_native_demo_ps, "", "nb",
                      "ucode hash of the pixel shader to replace (16 hex digits, the ps_ucode field of the "
                      "CP records); empty disables the demo");
REXCVAR_DEFINE_BOOL(nb_native_bloom, false, "nb",
                    "Render the bloom composite (guest pixel shader D6B0AF19A166B630) natively: our HLSL, the "
                    "game's four bloom mips, scene texture and constants");

REXCVAR_DEFINE_BOOL(nb_native_bloom_blur, false, "nb",
                    "Render the bloom blur chain (guest pixel shader EDE50E25A39F4768, five mip levels per "
                    "frame) natively with the game's kernel offsets and weights");

REXCVAR_DEFINE_BOOL(nb_native_generic, false, "nb",
                    "Draw every (vertex, pixel) shader pair that has a generated shader in the native shader "
                    "library natively (tools/ucode2hlsl.py output)");
REXCVAR_DEFINE_STRING(nb_native_shader_dir, "", "nb",
                      "Directory of the generated native shaders (<vs>_<ps>.vs.hlsl/.ps.hlsl/.meta); empty = "
                      "native_shaders next to the executable");
REXCVAR_DEFINE_STRING(nb_native_shader_filter, "", "nb",
                      "Comma-separated substrings of <vs>_<ps> stems; only matching pairs draw natively (empty = all)");
REXCVAR_DEFINE_STRING(nb_native_asset_pack, "", "nb",
                      "Optional local immutable geometry pack; empty keeps guest shared-memory buffers");
REXCVAR_DEFINE_INT32(nb_native_asset_cache_mode, 0, "nb",
                     "Geometry matching: 0 off, 1 legacy, 2 optimized; F6 toggles off/optimized");
REXCVAR_DEFINE_INT64(nb_native_asset_benchmark_start, 0, "nb",
                     "First frame of a six-window legacy/optimized/off/off/optimized/legacy comparison; 0 disables");
REXCVAR_DEFINE_INT32(nb_native_asset_benchmark_frames, 300, "nb",
                     "Frames per geometry-cache benchmark window, including warmup");
REXCVAR_DEFINE_INT32(nb_native_asset_benchmark_warmup, 60, "nb",
                     "Warmup frames excluded from each geometry-cache benchmark sample");
REXCVAR_DEFINE_STRING(nb_screenshot_frames, "", "nb",
                      "Frame indices to announce and hold for the screenshot harness, e.g. \"1200,1800\"");
REXCVAR_DEFINE_INT32(nb_screenshot_hold_ms, 500, "nb",
                     "How long to hold an announced frame on screen so the harness can capture it");
// Same-run test of the SDK's execute_unclipped_draw_vs_on_cpu (draw_extent_estimator.cpp): at each swap,
// set it to whether the next frame lies in these windows. Empty leaves the cvar alone, so nb.toml or the
// command line decide.
REXCVAR_DEFINE_STRING(nb_vs_extent_frames, "", "nb",
                      "Same-run test: execute_unclipped_draw_vs_on_cpu is on only inside these frame windows, "
                      "first-last[,first-last...] in the frame trace's numbering; empty leaves it alone");
// Same-run test of chunked deferred replay (nb_native_replay_chunk_draws, docs/native_performance.md perf40).
// Inside these windows the next frame streams its replay to the worker in chunks. Outside them each
// submission replays whole (0). Empty leaves the cvar alone.
REXCVAR_DEFINE_STRING(nb_replay_chunk_frames, "", "nb",
                      "Same-run test: nb_native_replay_chunk_draws is nb_replay_chunk_frames_draws inside these frame "
                      "windows and 0 outside, first-last[,first-last...]; empty leaves it alone");
REXCVAR_DEFINE_INT32(nb_replay_chunk_frames_draws, 2048, "nb",
                     "Chunk size that nb_replay_chunk_frames sets inside its windows");
REXCVAR_DEFINE_STRING(nb_rdc_frames, "", "nb",
                      "Guest frame indices to hand RenderDoc through its in-application API, e.g. \"1800\"; needs the game launched under RenderDoc");
REXCVAR_DEFINE_INT64(nb_log_frame, 0, "nb",
                   "Log every native draw of this one guest frame in submission order, with its shader pair and depth state; 0 = off. For finding which pair draws a given surface");
REXCVAR_DEFINE_BOOL(nb_native_exp_bias, true, "nb",
                    "Apply the texture fetch constant's exponent bias (2^exp_adjust) to native samples, as the hardware and the SDK translator do; off reproduces the black skies and roofs that leaving it out caused");
REXCVAR_DEFINE_INT64(nb_trace_every, 0, "nb",
                     "Repeat the nb_native_trace_pair dump every N guest frames instead of only on the pair's first two draws, so state that changes during a run (time of day, a bool that flips) can be caught in the act; 0 = off");
REXCVAR_DEFINE_STRING(nb_native_trace_pair, "", "nb",
                      "Log the draw state a translated shader depends on for pairs whose stem contains this");
REXCVAR_DEFINE_BOOL(nb_native_alpha_test, true, "nb",
                    "Apply the guest's alpha test in native draws (off: bisecting aid for black surfaces)");
REXCVAR_DEFINE_BOOL(nb_native_vertex_kill, true, "nb",
                    "Apply the guest's vertex kill in native draws (off: bisecting aid)");
REXCVAR_DEFINE_BOOL(nb_native_stencil, true, "nb",
                    "Native geometry passes mirror the guest's stencil state (off: stencil disabled on native draws)");
REXCVAR_DEFINE_STRING(nb_native_shader_exclude, "", "nb",
                      "Comma-separated substrings of <vs>_<ps> stems whose pairs stay emulated (bisecting aid)");
REXCVAR_DEFINE_BOOL(nb_native_dof, false, "nb",
                    "Render the depth-of-field / fog composite (vs 67EBCAD85D59CEBF / ps 9DD9939DE449D096) natively");
REXCVAR_DEFINE_BOOL(nb_native_sprite, false, "nb",
                    "Draw the game's 2D sprites (vs E91536F60A69ABF3 / ps 54771EC87D07B28C) natively from the guest "
                    "vertex data: the first geometry pass");

namespace {
// App UI reads only this value; command-processor fields stay on their owner.
std::atomic<uint64_t> g_nb_completed_swaps{0};
}

extern "C" __declspec(dllexport) uint64_t NbGetCompletedSwapCount() {
  return g_nb_completed_swaps.load(std::memory_order_relaxed);
}

// The draw resolution scale in effect, for nb.exe's F10 overlay. Published at every swap; false before
// the first one.
namespace {
std::atomic<uint32_t> g_nb_draw_scale_x{0};
std::atomic<uint32_t> g_nb_draw_scale_y{0};
}  // namespace

extern "C" __declspec(dllexport) bool NbGetDrawResolutionScale(uint32_t* scale_x, uint32_t* scale_y) {
  const uint32_t x = g_nb_draw_scale_x.load(std::memory_order_relaxed);
  const uint32_t y = g_nb_draw_scale_y.load(std::memory_order_relaxed);
  if (!scale_x || !scale_y || !x || !y) return false;
  *scale_x = x;
  *scale_y = y;
  return true;
}

namespace nb::gpu {

namespace {

struct AssetBenchmarkPhase {
  int window = -1;
  int mode = 0;
  int64_t options = -1;
  bool warmup = false;
  const char* name() const { return window < 0 ? "normal" : warmup ? "warmup" : "sample"; }
};

const std::vector<int64_t>& PerfBenchmarkOptions() {
  static const auto options = [] {
    std::vector<int64_t> result;
    const std::string text = REXCVAR_GET(nb_native_perf_benchmark_options);
    size_t start = 0;
    while (start < text.size()) {
      const size_t comma = text.find(',', start);
      const size_t end = comma == std::string::npos ? text.size() : comma;
      int64_t value = 0;
      const auto parsed = std::from_chars(text.data() + start, text.data() + end, value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end || value < 0 ||
          value > 281474976710655LL || result.size() == 32 || end + 1 == text.size()) {
        REXLOG_ERROR("rexgpu-nb: invalid performance benchmark option list");
        return std::vector<int64_t>{};
      }
      result.push_back(value);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (result.size() < 2) {
      REXLOG_ERROR("rexgpu-nb: performance benchmark needs 2 to 32 option windows");
      return std::vector<int64_t>{};
    }
    return result;
  }();
  return options;
}

AssetBenchmarkPhase AssetPhaseForFrame(uint64_t frame) {
  AssetBenchmarkPhase phase;
  phase.mode = std::clamp(REXCVAR_GET(nb_native_asset_cache_mode), 0, 2);
  const int64_t perf_start = REXCVAR_GET(nb_native_perf_benchmark_start);
  const int64_t start = perf_start > 0 ? perf_start : REXCVAR_GET(nb_native_asset_benchmark_start);
  const uint64_t length = uint64_t(std::clamp(REXCVAR_GET(nb_native_asset_benchmark_frames), 2, 36000));
  if (perf_start > 0) {
    phase.mode = 0;
    // Warm both compile/PSO caches before measuring. All guest state keeps advancing normally.
    const auto& options = PerfBenchmarkOptions();
    if (options.empty()) return phase;
    if (frame >= 3600 && frame < uint64_t(start)) {
      int64_t combined = 0;
      for (const int64_t option : options) combined |= option;
      phase.options = ((frame / length) & 1) ? combined : options.front();
    }
  }
  if (start <= 0 || frame < uint64_t(start)) return phase;
  const uint64_t offset = frame - uint64_t(start);
  constexpr int modes[] = {1, 2, 0, 0, 2, 1};
  // Bits: 1 avoids discarded binds, 2 batches ranges, 4 specializes samplers,
  // 8 caches static roots, 16 skips unused guest pipeline preparation,
  // 32 reuses identical constants and the last complete native PSO key,
  // 64 batches sequential guest register writes, 128 selects performance cores,
  // 256 uses hardware indices for eligible DMA triangle lists, 512 memoizes
  // exact shader-load bytes, 1024 omits detailed clocks/periodic draw logs,
  // 2048 reuses byte-verified uploads, 4096 caches immutable binding plans,
  // 8192 memoizes the last ready shader pair, 16384 omits per-command clocks,
  // 32768 uses precise host waits, 65536 memoizes each texture's last SRV lookup,
  // 131072 memoizes each shader's last exact translation-modification lookup,
  // 262144 assembles vertex constants in RAM before the mapped upload copy.
  // 524288 publishes an owned pixel-constant packet without a RAM copy.
  // 1048576 retains constructed deferred-command storage between submissions.
  // Bits 21..22 select the bounded performance-core placement experiment.
  // 8388608 uses an immutable root CBV for the unchanged b0 constant payload.
  // 16777216 specializes single-range residency without constructing a batch.
  // 33554432 omits repeated native pixel-CBV address writes.
  // 67108864 is placement bit2: test the scheduling-class P-core subset.
  // 134217728 permits one short first retry before normal WAIT_REG_MEM sleeps.
  // 268435456 specializes vertex loads when immutable geometry routing is inactive.
  // 536870912 caps excess CPU invalidation at 64KiB while preserving real writes.
  // 1073741824 reuses identical complete VS packets in immutable same-frame slices.
  // 2147483648 normalizes PSO keys only across identical descriptor settings.
  // 4294967296 reuses already-valid shared-memory residency with strict invalidation.
  // 8589934592 omits proven-unused trailing float constants from explicit empty layouts.
  // 17179869184 stores unchanged native CBV setters in one deferred packet.
  // 34359738368 serves native constant slices from CPU-writable video memory.
  // 17592186044416 re-reads blocked WAIT_REG_MEM predicates at yield cadence for up to 5 ms.
  if (offset / length >= (perf_start > 0 ? PerfBenchmarkOptions().size() : std::size(modes))) return phase;
  phase.window = int(offset / length);
  if (perf_start > 0) phase.options = PerfBenchmarkOptions()[phase.window];
  else phase.mode = modes[phase.window];
  phase.warmup = offset % length < uint64_t(std::clamp(REXCVAR_GET(nb_native_asset_benchmark_warmup), 0, int(length - 1)));
  return phase;
}

int64_t CurrentPerfOptions() {
#if NB_HAS_RUNTIME_WATCH_CONTROL
  const uint32_t watch_mode = nb::runtime::GetWatchFastpathMode();
#endif
  return (REXCVAR_GET(nb_native_defer_guest_pipeline) ? 1 : 0) |
         (REXCVAR_GET(nb_range_batch_optimize) ? 2 : 0) |
         (REXCVAR_GET(nb_native_effective_samplers) ? 4 : 0) |
         (REXCVAR_GET(nb_native_static_bindings) ? 8 : 0) |
         (REXCVAR_GET(nb_native_skip_guest_pipeline) ? 16 : 0) |
         (REXCVAR_GET(nb_native_constant_reuse) ? 32 : 0) |
         (REXCVAR_GET(nb_native_register_fastpath) ? 64 : 0) |
         (REXCVAR_GET(nb_native_gpu_pcores) ? 128 : 0) |
         (REXCVAR_GET(nb_native_hardware_indices) ? 256 : 0) |
         (REXCVAR_GET(nb_shader_load_memo) ? 512 : 0) |
         (REXCVAR_GET(nb_native_minimal_diagnostics) ? 1024 : 0) |
         (REXCVAR_GET(nb_native_upload_shadow) ? 2048 : 0) |
         (REXCVAR_GET(nb_native_texture_binding_cache) ? 4096 : 0) |
         (REXCVAR_GET(nb_native_pair_lookup_memo) ? 8192 : 0) |
         (REXCVAR_GET(nb_native_minimal_command_diagnostics) ? 16384 : 0) |
         (REXCVAR_GET(nb_native_precise_wait) ? 32768 : 0) |
         (REXCVAR_GET(nb_native_texture_descriptor_memo) ? 65536 : 0) |
         (REXCVAR_GET(nb_native_translation_lookup_memo) ? 131072 : 0) |
         (REXCVAR_GET(nb_native_stage_vertex_constants) ? 262144 : 0) |
         (REXCVAR_GET(nb_native_double_pixel_packets) ? 524288 : 0) |
         (REXCVAR_GET(nb_native_deferred_storage) ? 1048576 : 0) |
         ((std::clamp(REXCVAR_GET(nb_native_gpu_placement), 0, 7) & 3) << 21) |
         ((std::clamp(REXCVAR_GET(nb_native_gpu_placement), 0, 7) & 4) << 24) |
         (REXCVAR_GET(nb_native_root_cbv) ? 8388608 : 0) |
         (REXCVAR_GET(nb_range_single_fastpath) ? 16777216 : 0) |
         (REXCVAR_GET(nb_native_pixel_binding_cache) ? 33554432 : 0) |
         (REXCVAR_GET(nb_native_early_wait_poll) ? 134217728 : 0) |
         (REXCVAR_GET(nb_native_direct_guest_reads) ? 268435456 : 0) |
         (REXCVAR_GET(nb_native_narrow_invalidation) ? 536870912 : 0) |
         (REXCVAR_GET(nb_native_vertex_constant_reuse) ? 1073741824 : 0) |
         (REXCVAR_GET(nb_native_pipeline_key_normalize) ? 2147483648LL : 0) |
         (REXCVAR_GET(nb_native_shared_residency_mirror) ? 4294967296LL : 0) |
         (REXCVAR_GET(nb_native_empty_constant_layout) ? 8589934592LL : 0) |
         (REXCVAR_GET(nb_native_constant_binding_packet) ? 17179869184LL : 0) |
         (REXCVAR_GET(nb_native_constant_gpu_upload) ? 34359738368LL : 0) |
         (REXCVAR_GET(nb_native_avx2_shader_hash) ? 274877906944LL : 0) |
         (REXCVAR_GET(nb_native_async_replay) ? 549755813888LL : 0) |
         (REXCVAR_GET(nb_native_unused_pixel_constants) ? 4398046511104LL : 0) |
         (REXCVAR_GET(nb_native_texture_outdated_load_first) ? 8796093022208LL : 0) |
         (REXCVAR_GET(nb_native_wait_spin_us) > 0 ? 17592186044416LL : 0) |
         (REXCVAR_GET(nb_native_replay_chunk_draws) >= 2048 ? 2199023255552LL :
          REXCVAR_GET(nb_native_replay_chunk_draws) > 0 ? 1099511627776LL : 0)
#if NB_HAS_RUNTIME_WATCH_CONTROL
         | ((watch_mode & nb::runtime::kWatchAlreadyEnabled) ? 68719476736LL : 0)
         | ((watch_mode & nb::runtime::kWatchGuestImmutable) ? 137438953472LL : 0)
#endif
         ;
}
void ApplyPerfOptions(int64_t options) {
  REXCVAR_SET(nb_native_defer_guest_pipeline, (options & 1) != 0);
  REXCVAR_SET(nb_range_batch_optimize, (options & 2) != 0);
  REXCVAR_SET(nb_native_effective_samplers, (options & 4) != 0);
  REXCVAR_SET(nb_native_static_bindings, (options & 8) != 0);
  REXCVAR_SET(nb_native_skip_guest_pipeline, (options & 16) != 0);
  REXCVAR_SET(nb_native_constant_reuse, (options & 32) != 0);
  REXCVAR_SET(nb_native_register_fastpath, (options & 64) != 0);
  REXCVAR_SET(nb_native_gpu_pcores, (options & 128) != 0);
  REXCVAR_SET(nb_native_hardware_indices, (options & 256) != 0);
  REXCVAR_SET(nb_shader_load_memo, (options & 512) != 0);
  REXCVAR_SET(nb_native_minimal_diagnostics, (options & 1024) != 0);
  REXCVAR_SET(nb_native_upload_shadow, (options & 2048) != 0);
  REXCVAR_SET(nb_native_texture_binding_cache, (options & 4096) != 0);
  REXCVAR_SET(nb_native_pair_lookup_memo, (options & 8192) != 0);
  REXCVAR_SET(nb_native_minimal_command_diagnostics, (options & 16384) != 0);
  REXCVAR_SET(nb_native_precise_wait, (options & 32768) != 0);
  REXCVAR_SET(nb_native_texture_descriptor_memo, (options & 65536) != 0);
  REXCVAR_SET(nb_native_translation_lookup_memo, (options & 131072) != 0);
  REXCVAR_SET(nb_native_stage_vertex_constants, (options & 262144) != 0);
  REXCVAR_SET(nb_native_double_pixel_packets, (options & 524288) != 0);
  REXCVAR_SET(nb_native_deferred_storage, (options & 1048576) != 0);
  REXCVAR_SET(nb_native_gpu_placement, ((options >> 21) & 3) | ((options >> 24) & 4));
  REXCVAR_SET(nb_native_root_cbv, (options & 8388608) != 0);
  REXCVAR_SET(nb_range_single_fastpath, (options & 16777216) != 0);
  REXCVAR_SET(nb_native_pixel_binding_cache, (options & 33554432) != 0);
  REXCVAR_SET(nb_native_early_wait_poll, (options & 134217728) != 0);
  REXCVAR_SET(nb_native_direct_guest_reads, (options & 268435456) != 0);
  REXCVAR_SET(nb_native_narrow_invalidation, (options & 536870912) != 0);
  REXCVAR_SET(nb_native_vertex_constant_reuse, (options & 1073741824) != 0);
  REXCVAR_SET(nb_native_pipeline_key_normalize, (options & 2147483648LL) != 0);
  REXCVAR_SET(nb_native_shared_residency_mirror, (options & 4294967296LL) != 0);
  REXCVAR_SET(nb_native_empty_constant_layout, (options & 8589934592LL) != 0);
  REXCVAR_SET(nb_native_constant_binding_packet, (options & 17179869184LL) != 0);
  REXCVAR_SET(nb_native_constant_gpu_upload, (options & 34359738368LL) != 0);
  REXCVAR_SET(nb_native_avx2_shader_hash, (options & 274877906944LL) != 0);
  REXCVAR_SET(nb_native_async_replay, (options & 549755813888LL) != 0);
  REXCVAR_SET(nb_native_unused_pixel_constants, (options & 4398046511104LL) != 0);
  REXCVAR_SET(nb_native_texture_outdated_load_first, (options & 8796093022208LL) != 0);
  REXCVAR_SET(nb_native_wait_spin_us, (options & 17592186044416LL) ? 5000 : 0);
  REXCVAR_SET(nb_native_replay_chunk_draws, (options & 2199023255552LL) ? 2048 :
                                        (options & 1099511627776LL) ? 1024 : 0);
#if NB_HAS_RUNTIME_WATCH_CONTROL
  nb::runtime::SetWatchFastpathMode(
      ((options & 68719476736LL) ? nb::runtime::kWatchAlreadyEnabled : 0) |
      ((options & 137438953472LL) ? nb::runtime::kWatchGuestImmutable : 0));
#endif
}

uint32_t regs_index_offset(const rex::graphics::RegisterFile& regs) {
  return regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
}
}  // namespace

namespace {

// Pass 0: a procedural pattern that cannot be mistaken for emulated output.
// c[0] = (viewport width, viewport height, seconds since start, frame index).
constexpr const char kDemoPs[] = R"hlsl(
cbuffer NbPass : register(b0) { float4 c[5]; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
  float t = c[0].z;
  float stripes = step(0.5, frac((uv.x - uv.y) * 10.0 + t * 0.35));
  float3 orange = float3(0.96, 0.58, 0.12);
  float3 blue = float3(0.12, 0.38, 0.88);
  float3 col = lerp(orange, blue, saturate(uv.y * 1.2 - 0.1));
  col *= lerp(0.72, 1.0, stripes);
  float vignette = 1.0 - 0.45 * dot(uv - 0.5, uv - 0.5);
  return float4(col * vignette, 1.0);
}
)hlsl";

uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

}  // namespace

NbCommandProcessor::NbCommandProcessor(NbGraphicsSystem* graphics_system,
                                       rex::system::KernelState* kernel_state)
    : rex::graphics::d3d12::D3D12CommandProcessor(graphics_system, kernel_state) {
#if NB_HAS_RUNTIME_WATCH_CONTROL
  nb::runtime::SetWatchFastpathMode(
      (REXCVAR_GET(nb_native_watch_fastpath) ? nb::runtime::kWatchAlreadyEnabled : 0) |
      (REXCVAR_GET(nb_native_immutable_watch_fastpath) ? nb::runtime::kWatchGuestImmutable : 0));
  REXLOG_INFO("rexgpu-nb: runtime watch fastpath available {} initial mode {}",
              nb::runtime::WatchFastpathAvailable(), nb::runtime::GetWatchFastpathMode());
#endif
  perf_default_options_ = CurrentPerfOptions();
  const auto initial_phase = AssetPhaseForFrame(1);
  asset_cache_mode_ = initial_phase.mode;
  if (initial_phase.options >= 0) ApplyPerfOptions(initial_phase.options);

  try {
    const std::string trace_file = REXCVAR_GET(nb_native_frame_trace_file);
    if (!trace_file.empty()) {
      frame_trace_ = std::make_unique<NativeFrameTrace>(
          trace_file, REXCVAR_GET(nb_native_frame_trace_start_seconds),
          REXCVAR_GET(nb_native_frame_trace_duration_seconds));
    }
  } catch (...) {
    // Optional diagnostic allocation/configuration failure cannot fail rendering.
    frame_trace_.reset();
    REXLOG_WARN("rexgpu-nb: frame trace disabled: allocation or configuration failure");
  }

  // The SDK registry retains entries after unregistering and removes only the
  // first callback with a given name. A recreated CP therefore needs new names.
  static std::atomic<uint64_t> bind_generation{0};
  const uint64_t generation = bind_generation.fetch_add(1, std::memory_order_relaxed);
  const std::string suffix = generation ? "_" + std::to_string(generation) : "";
  native_toggle_bind_name_ = "bind_nb_native_toggle" + suffix;
  asset_toggle_bind_name_ = "bind_nb_asset_cache_toggle" + suffix;
  try {
    rex::ui::RegisterBind(native_toggle_bind_name_, "F5", "Toggle native geometry rendering", [this] {
      pending_debug_keys_.fetch_xor(kNativeTogglePending, std::memory_order_relaxed);
    });
    rex::ui::RegisterBind(asset_toggle_bind_name_, "F6", "Toggle native geometry cache", [this] {
      pending_debug_keys_.fetch_xor(kAssetTogglePending, std::memory_order_relaxed);
    });
  } catch (...) {
    // RegisterBind can publish its callback before a later allocation throws.
    // Clear both names even when the second registration did not return.
    rex::ui::UnregisterBind(native_toggle_bind_name_);
    rex::ui::UnregisterBind(asset_toggle_bind_name_);
    throw;
  }
}

// Bracket a whole guest frame with RenderDoc's in-application capture, rather than relying on F12 landing
// between the right two Presents. The guest's geometry, its shadow passes and the composite are submitted
// from the command processor thread across several command lists, and a Present-to-Present capture
// routinely misses all of them: the first attempt caught a frame with a single draw in it.
void NbCommandProcessor::UpdateRenderDocCapture(uint64_t frame) {
  const std::string list = REXCVAR_GET(nb_rdc_frames);
  if (list.empty()) {
    return;
  }
  if (!rdc_frames_parsed_) {
    rdc_frames_parsed_ = true;
    size_t start = 0;
    while (start < list.size()) {
      const size_t comma = list.find(',', start);
      const std::string item =
          list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!item.empty()) rdc_frames_.insert(std::strtoull(item.c_str(), nullptr, 10));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    // renderdoc.dll is already in the process when the game was launched through RenderDoc; we never
    // load it ourselves, so a normal run is unaffected.
    if (HMODULE module = GetModuleHandleA("renderdoc.dll")) {
      auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
      if (get_api && get_api(eRENDERDOC_API_Version_1_0_1, reinterpret_cast<void**>(&rdc_api_)) == 1) {
        REXLOG_INFO("rexgpu-nb: RenderDoc in-application capture armed for {} frames", rdc_frames_.size());
      } else {
        rdc_api_ = nullptr;
      }
    }
    if (!rdc_api_) {
      REXLOG_WARN("rexgpu-nb: nb_rdc_frames is set but RenderDoc is not attached to this process");
    }
  }
  if (!rdc_api_) {
    return;
  }
  if (rdc_capturing_) {
    rdc_api_->EndFrameCapture(nullptr, nullptr);
    rdc_capturing_ = false;
    // The capture was opened at the end of the previous frame, so the frame just finished is the one in it.
    REXLOG_INFO("rexgpu-nb: RenderDoc capture of frame {} finished", frame);
  }
  // The next guest frame is the one wanted, so open the capture now: IssueSwap runs after the frame it
  // ends, and everything submitted from here to the next swap belongs to frame + 1.
  if (rdc_frames_.count(frame + 1)) {
    rdc_api_->StartFrameCapture(nullptr, nullptr);
    rdc_capturing_ = true;
    REXLOG_INFO("rexgpu-nb: RenderDoc capture of frame {} started", frame + 1);
  }
}

// Created on the first native draw that asks for it, and retained afterwards: a benchmark window
// that turns the option off and on again reuses the same pages rather than paying for new ones, and
// the pool costs nothing while nothing requests a slice.
NativeConstantUploadPool* NbCommandProcessor::ConstantUploadPool() {
  if (!REXCVAR_GET(nb_native_constant_gpu_upload)) {
    return nullptr;
  }
  if (!constant_upload_pool_attempted_) {
    constant_upload_pool_attempted_ = true;
    if (ID3D12Device* device = GetD3D12Provider().GetDevice()) {
      constant_upload_pool_ = NativeConstantUploadPool::TryCreate(device);
      if (constant_upload_pool_) {
        REXLOG_INFO(
            "rexgpu-nb: native constant pool in CPU-writable video memory created; adapter GPU_UPLOAD "
            "support {}, page {} bytes, budget {} bytes",
            constant_upload_pool_->gpu_upload_supported() ? "present" : "absent",
            NativeConstantUploadPool::kDefaultPageBytes,
            NativeConstantUploadPool::kDefaultGpuByteBudget);
      } else {
        REXLOG_WARN("rexgpu-nb: native constant pool allocation failed; ordinary upload heaps retained");
      }
    } else {
      REXLOG_WARN("rexgpu-nb: no device for the native constant pool; ordinary upload heaps retained");
    }
  }
  return constant_upload_pool_.get();
}

NbCommandProcessor::~NbCommandProcessor() {
  // Dispatch holds the same registry mutex, so unregister also waits for an
  // in-flight callback before pending_debug_keys_ and the other members die.
  rex::ui::UnregisterBind(native_toggle_bind_name_);
  rex::ui::UnregisterBind(asset_toggle_bind_name_);
  REXLOG_INFO("rexgpu-nb: shutdown after {} frames, {} draws ({} native), {} resolves, {} shader loads",
              stats_.frames, stats_.draws, stats_.native_draws, stats_.copies, stats_.shaders_loaded);
}

void NbCommandProcessor::ShutdownContext() {
  if (!cpu_affinity_.Shutdown()) {
    REXLOG_WARN("rexgpu-nb: could not restore GPU thread scheduling during shutdown");
  }
  // Queued draws must finish before the arena and its SharedMemory watch leave.
  const bool native_resources_idle = AwaitNativeResourcesIdle();
  native_asset_cache_.Shutdown();
  // A failed drain is not permission to release buffers still referenced by the
  // GPU. Shutdown(false) retains their resource references even through pool
  // destruction; a healthy device and completed drain permit normal release.
  if (constant_upload_pool_) {
    ID3D12Device* device = GetD3D12Provider().GetDevice();
    const bool completion_proven = native_resources_idle && device &&
                                   SUCCEEDED(device->GetDeviceRemovedReason());
    constant_upload_pool_->Shutdown(completion_proven);
    if (!completion_proven) {
      REXLOG_WARN("rexgpu-nb: constant pool GPU completion unproven; retaining resource references");
    }
  }
  constant_upload_pool_.reset();
  rex::graphics::d3d12::D3D12CommandProcessor::ShutdownContext();
}


// F5/F6 arrive through the app's window listener, which remains active with a
// scripted controller. Consume their parity here to keep rendering changes and
// screenshot mode logs on the command thread at the existing swap boundary.
void NbCommandProcessor::PollDebugKeys() {
  const uint32_t pending = pending_debug_keys_.exchange(0, std::memory_order_relaxed);
  if (pending & kNativeTogglePending) {
    const bool enabled = !REXCVAR_GET(nb_native_generic);
    REXCVAR_SET(nb_native_generic, enabled);
    // WARN so it stands out in the log next to the frame number: that is how a screenshot gets matched
    // back to the mode it was taken in.
    REXLOG_WARN("rexgpu-nb: F5 at frame {}: native generic pass {}", stats_.frames,
                enabled ? "ON" : "OFF");
  }
  if (pending & kAssetTogglePending) {
    const int mode = REXCVAR_GET(nb_native_asset_cache_mode) == 0 ? 2 : 0;
    REXCVAR_SET(nb_native_asset_cache_mode, mode);
    REXLOG_WARN("rexgpu-nb: F6 at frame {}: geometry cache requested mode {} (scheduled benchmark takes priority)", stats_.frames, mode);
  }
}
// "first-last[,first-last...]"; an empty list contains no frame.
static bool NbFrameInWindowList(const std::string& windows, uint64_t frame) {
  const char* p = windows.c_str();
  while (*p) {
    unsigned long long first = 0, last = 0;
    int consumed = 0;
    if (std::sscanf(p, "%llu-%llu%n", &first, &last, &consumed) != 2 || consumed <= 0) return false;
    if (frame >= first && frame <= last) return true;
    p += consumed;
    while (*p == ',' || *p == ' ') ++p;
  }
  return false;
}

void NbCommandProcessor::OverrideDisplayAspect(uint32_t& display_width, uint32_t& display_height) {
  if (present_ultrawide_) {
    display_width = present_aspect_w_;
    display_height = present_aspect_h_;
  }
  if (display_width != logged_aspect_w_ || display_height != logged_aspect_h_) {
    logged_aspect_w_ = display_width;
    logged_aspect_h_ = display_height;
    REXLOG_INFO("rexgpu-nb: presenter aspect {}:{} from frame {}{}", display_width, display_height,
                stats_.frames + 1, present_ultrawide_ ? " (nb_ultrawide)" : "");
  }
}

void NbCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                   uint32_t frontbuffer_height) {
  // Exactly one take per swap packet, before the base class can return early: the aspect of the frame
  // this packet presents (OverrideDisplayAspect). The frontbuffer address lets nb.exe resynchronise its
  // queue after a lost packet.
  present_ultrawide_ =
      nb::gpu::UltrawideTakePresentAspect(frontbuffer_ptr, &present_aspect_w_, &present_aspect_h_) &&
      present_aspect_w_ && present_aspect_h_;
  g_nb_draw_scale_x.store(texture_cache().draw_resolution_scale_x(), std::memory_order_relaxed);
  g_nb_draw_scale_y.store(texture_cache().draw_resolution_scale_y(), std::memory_order_relaxed);
  const auto swap_start = std::chrono::steady_clock::now();
  D3D12CommandProcessor::IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
  const uint64_t swap_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - swap_start).count());
  issue_swap_ns_ += swap_ns;

  // Same contract as the SDK's own constant pool, which it reclaims with frame_completed_: a page
  // returns to the writable list only once the frame that last wrote it has completed on the GPU.
  // Once per swap is a coarser cadence than the SDK's, which only ever holds pages a little longer.
  if (constant_upload_pool_) {
    constant_upload_pool_->ReclaimCompleted(GetCompletedFrame());
  }

  const auto swap_time = std::chrono::steady_clock::now();
  uint64_t frame_ns = 0;
  if (previous_swap_time_ != std::chrono::steady_clock::time_point{}) {
    frame_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        swap_time - previous_swap_time_).count());
    frame_wall_ns_ += frame_ns;
    interval_timed_frames_++;
  }
  previous_swap_time_ = swap_time;
  interval_draws_ += draws_this_frame_;
  interval_native_draws_ += native_this_frame_;
  stats_.frames++;
  g_nb_completed_swaps.fetch_add(1, std::memory_order_relaxed);
  // Same-run test windows for the draw-extent estimate (nb_vs_extent_frames). The next frame decides.
  if (const std::string& vs_extent_windows = REXCVAR_GET(nb_vs_extent_frames); !vs_extent_windows.empty()) {
    const bool on = NbFrameInWindowList(vs_extent_windows, stats_.frames + 1);
    if (on != REXCVAR_GET(execute_unclipped_draw_vs_on_cpu)) {
      REXCVAR_SET(execute_unclipped_draw_vs_on_cpu, on);
      REXLOG_INFO("rexgpu-nb: execute_unclipped_draw_vs_on_cpu {} from frame {} (nb_vs_extent_frames {})",
                  on ? "on" : "off", stats_.frames + 1, vs_extent_windows);
    }
  }
  // Same for chunked replay (nb_replay_chunk_frames). The base swap above has already finished any chunk
  // session, so the next frame starts cleanly under the new size.
  if (const std::string& chunk_windows = REXCVAR_GET(nb_replay_chunk_frames); !chunk_windows.empty()) {
    const int32_t chunk_draws = NbFrameInWindowList(chunk_windows, stats_.frames + 1)
        ? REXCVAR_GET(nb_replay_chunk_frames_draws) : 0;
    if (chunk_draws != REXCVAR_GET(nb_native_replay_chunk_draws)) {
      REXCVAR_SET(nb_native_replay_chunk_draws, chunk_draws);
      REXLOG_INFO("rexgpu-nb: nb_native_replay_chunk_draws {} from frame {} (nb_replay_chunk_frames {})",
                  chunk_draws, stats_.frames + 1, chunk_windows);
    }
  }
  if (frame_trace_ && frame_trace_->enabled()) {
    NativeFrameTrace::FrameRow row;
    row.frame = stats_.frames;
    row.frame_ns = frame_ns;
    row.swap_ns = swap_ns;
    row.draws = draws_this_frame_;
    row.native_draws = native_this_frame_;
    row.shader_load_calls = shaders_this_frame_;
    static_assert(kRefusedCount == 8);
    for (uint32_t i = 0; i != kRefusedCount; ++i) {
      row.refusal[i] = generic_refusals_[i] - frame_trace_previous_refusals_[i];
    }
    const auto& uploads = GetNativeUploadShadowStats();
    row.shared_upload_bytes = uploads.original_copy_bytes;
    row.shared_upload_calls = uploads.original_copy_commands;
    const auto replay = NativeReplayCounts();
    row.async_queued = replay[0];
    row.async_completed = replay[1];
    const auto result = frame_trace_->Record(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(swap_time.time_since_epoch()).count()), row);
    if (result != NativeFrameTrace::Result::None) {
      if (result == NativeFrameTrace::Result::Saved) {
        REXLOG_INFO("rexgpu-nb: frame trace saved: status {}", frame_trace_->status());
      } else {
        REXLOG_WARN("rexgpu-nb: frame trace failed: status {}", frame_trace_->status());
      }
      frame_trace_.reset();
    }
  }
  const bool native_before_keys = REXCVAR_GET(nb_native_generic);
  PollDebugKeys();
  const uint64_t f = stats_.frames;
  const auto phase = AssetPhaseForFrame(f);
  const auto next_phase = AssetPhaseForFrame(f + 1);
  const bool phase_boundary = phase.window != next_phase.window || phase.warmup != next_phase.warmup || phase.options != next_phase.options;
  // A frame the harness wants: say so, then hold it on screen long enough to be captured.
  if (!screenshot_frames_parsed_) {
    screenshot_frames_parsed_ = true;
    const std::string list = REXCVAR_GET(nb_screenshot_frames);
    size_t start = 0;
    while (start < list.size()) {
      const size_t comma = list.find(',', start);
      const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!item.empty()) {
        screenshot_frames_.insert(std::strtoull(item.c_str(), nullptr, 10));
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (!screenshot_frames_.empty()) {
      REXLOG_INFO("rexgpu-nb: holding {} frames for the screenshot harness", screenshot_frames_.size());
    }
  }
  if (screenshot_frames_.count(f)) {
    REXLOG_INFO("rexgpu-nb: screenshot frame {}", f);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, REXCVAR_GET(nb_screenshot_hold_ms))));
  }
  UpdateRenderDocCapture(f);

  frames_since_log_++;
  if (phase_boundary || next_phase.mode != asset_cache_mode_ || native_before_keys != REXCVAR_GET(nb_native_generic) ||
      (phase.window < 0 && (f == 1 || f == 2 || f == 60 || (f % 600) == 0))) {
    REXLOG_INFO("rexgpu-nb: frame {}: {} draws ({} native), {} resolves, {} new shaders; frontbuffer {}x{} at 0x{:08X}; "
                "CPU per frame over the last {}: IssueDraw {:.2f} ms (generic pass {:.2f} ms)",
                f, draws_this_frame_, native_this_frame_, copies_this_frame_, shaders_this_frame_,
                frontbuffer_width, frontbuffer_height, frontbuffer_ptr, frames_since_log_,
                issue_draw_ns_ / 1.0e6 / double(frames_since_log_), generic_pass_ns_ / 1.0e6 / double(frames_since_log_));
    REXLOG_INFO("rexgpu-nb:   generic pass refusals over the last {} frames: not in library {}, disabled {}, "
                "not ready {}, unusable {}, prepare {}, texture unbound {}, sampler {}, record {}",
                frames_since_log_, generic_refusals_[kRefusedNotInLibrary], generic_refusals_[kRefusedDisabled],
                generic_refusals_[kRefusedNotReady], generic_refusals_[kRefusedUnusable],
                generic_refusals_[kRefusedPrepare], generic_refusals_[kRefusedTexture],
                generic_refusals_[kRefusedSampler], generic_refusals_[kRefusedRecord]);
    REXLOG_INFO("rexgpu-nb:   native draws using empty texture slots over the last {} frames: {}",
                frames_since_log_, native_empty_texture_draws_);
    REXLOG_INFO("rexgpu-nb:   native triangle-strip draws over the last {} frames: {}",
                frames_since_log_, native_triangle_strip_draws_);
    REXLOG_INFO("rexgpu-nb:   native draws using stacked texture fetches over the last {} frames: {}",
                frames_since_log_, native_stacked_texture_draws_);
    REXLOG_INFO("rexgpu-nb:   native cube texture draws {} / extra vertex stream draws {} over the last {} frames",
                native_cube_texture_draws_, native_extra_stream_draws_, frames_since_log_);
    if (native_asset_cache_.initialized()) {
      const auto& assets = native_asset_cache_.stats();
      REXLOG_INFO("rexgpu-nb:   native asset buffers cumulative: {} hits ({} cached), {} misses, {} snapshots / {} bytes, {} CPU-authority refusals, {} invalidation races",
                  assets.hits, assets.cached_hits, assets.misses, assets.snapshots, assets.snapshot_bytes,
                  assets.authority_refusals, assets.invalidation_races);
      REXLOG_INFO("rexgpu-nb:   asset mapping cumulative: {} mode switches, {} legacy cap resets, {} collisions, {} probe refusals, {} high water / {} max probe, {} generation wraps, {} snapshot growths / {} bytes",
                  assets.mode_switches, assets.legacy_capacity_resets, assets.mapping_collision_steps,
                  assets.mapping_probe_refusals, assets.mapping_high_water, assets.mapping_max_probe,
                  assets.mapping_generation_wraps, assets.snapshot_growths, assets.snapshot_growth_bytes);
    }
    const auto offline_textures = GetOfflineTextureCacheStats();
    REXLOG_INFO("rexgpu-nb:   offline texture uploads cumulative: {} matches / {} candidates, {} uploads / {} bytes, {} CPU-authority refusals, {} byte misses",
                offline_textures.matches, offline_textures.candidates, offline_textures.prepared_uploads,
                offline_textures.upload_bytes, offline_textures.cpu_authority_refusals, offline_textures.byte_misses);
    REXLOG_INFO("rexgpu-nb:   texture slot failures: no descriptor {}, unexpected null {}, mixed signs {}, "
                "no compatible binding {} (declared dims 1D {} / 2D {} / 3D {} / cube {})",
                texture_failures_[kTextureNoDescriptor], texture_failures_[kTextureUnexpectedNull],
                texture_failures_[kTextureMixedSigns], texture_failures_[kTextureNoBinding],
                texture_failure_dimensions_[0], texture_failure_dimensions_[1],
                texture_failure_dimensions_[2], texture_failure_dimensions_[3]);
    auto& timings = NativeGeometryPass::timings();
    const double frames = double(frames_since_log_);
    REXLOG_INFO("rexgpu-nb:   perf options {} window {} phase {}", CurrentPerfOptions(), phase.window, phase.name());
    REXLOG_INFO("rexgpu-nb:   fine-grained native clocks {}", REXCVAR_GET(nb_native_minimal_diagnostics) ? "disabled" : "enabled");
    REXLOG_INFO("rexgpu-nb:   per-command CPU clocks {} (wall-frame and swap clocks retained)",
                REXCVAR_GET(nb_native_minimal_command_diagnostics) ? "disabled" : "enabled");
    if (REXCVAR_GET(nb_native_thread_cpu_diagnostics) ||
        REXCVAR_GET(nb_native_constant_upload_diagnostics) ||
        REXCVAR_GET(nb_native_submission_diagnostics)) {
      FILETIME creation{}, exit{}, kernel{}, user{};
      const bool valid = GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) != 0;
      const auto ticks = [](FILETIME time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
      };
      REXLOG_INFO("rexgpu-nb:   command thread CPU cumulative: valid {} kernel {} ticks / user {} ticks (100 ns units)",
                  valid, ticks(kernel), ticks(user));
    }
    if (REXCVAR_GET(nb_native_constant_upload_diagnostics)) {
      const auto& preparation = GetNativeConstantPreparationStats();
      const auto log_preparation = [](const char* name, const auto& stage) {
        REXLOG_INFO("rexgpu-nb:   constant preparation {} cumulative: {} calls / {} ns / {} failures",
                    name, stage.calls, stage.nanoseconds, stage.failures);
      };
      log_preparation("allocation", preparation.allocation);
      log_preparation("ram_fill", preparation.ram_fill);
      log_preparation("mapped_fill", preparation.mapped_fill);
      log_preparation("memo", preparation.memo);
    }
    const auto& waits = rex::graphics::GetNativeCommandWaitStats();
    REXLOG_INFO("rexgpu-nb:   command waits cumulative: {} idle ns / {} idle periods, {} blocked packet ns / {} packets / {} blocked, {} sleeps / {} yields",
                waits.idle_ns, waits.idle_periods, waits.wait_reg_ns, waits.wait_reg_packets,
                waits.blocked_packets, waits.sleep_calls, waits.yield_calls);
    REXLOG_INFO("rexgpu-nb:   command sleeps cumulative: {} requested ns / {} actual ns, {} precise calls / {} fallbacks",
                waits.requested_sleep_ns, waits.actual_sleep_ns, waits.precise_calls, waits.precise_fallbacks);
    REXLOG_INFO("rexgpu-nb:   early command polls cumulative: {} attempts / {} next-read matches / {} fallbacks",
                waits.early_polls, waits.early_matches, waits.early_fallbacks);
    const auto& spins = nb::gpu::GetNativeWaitSpinStats();
    REXLOG_INFO("rexgpu-nb:   command wait spins cumulative: {} packets / {} yields / {} matched / {} budget exhausted",
                spins.spinning_packets, spins.yields, spins.matched_while_spinning, spins.budget_exhausted);
    if (REXCVAR_GET(nb_native_wait_target_diagnostics)) {
      const auto& targets = nb::gpu::GetNativeWaitTargetTable();
      for (const auto& target : targets) {
        if (!target.used) continue;
        REXLOG_INFO("rexgpu-nb:   wait target cumulative: {} 0x{:08X} fn {} mask 0x{:08X}: {} packets / {} ns / {} max ns / "
                    "{} vblank-advanced / {} timed advances / {} advance-to-match ns / {} within 100us; "
                    "last ref 0x{:08X} first 0x{:08X} match 0x{:08X}",
                    target.is_memory ? "memory" : "register", target.address, target.function, target.mask,
                    target.packets, target.blocked_ns, target.max_ns, target.vblank_advanced,
                    target.timed_advances, target.advance_to_match_ns, target.matched_within_100us,
                    target.last_ref, target.last_first_value, target.last_match_value);
      }
      REXLOG_INFO("rexgpu-nb:   wait targets untracked cumulative: {}", targets.untracked());
    }
    const auto& bindings = native_static_binding_stats();
    const auto& deferred = rex::graphics::d3d12::GetNativeDeferredStorageStats();
    REXLOG_INFO("rexgpu-nb:   deferred storage cumulative: {} enabled resets / {} commands / {} reused bytes / {} initialized bytes",
                deferred.enabled_resets, deferred.commands, deferred.reused_bytes, deferred.initialized_bytes);
    const auto replay_counts = NativeReplayCounts();
    REXLOG_INFO("rexgpu-nb:   async replay cumulative: {} queued / {} completed / {} failures",
                replay_counts[0], replay_counts[1], replay_counts[2]);
    const auto chunks = NativeReplayChunkCounts();
    REXLOG_INFO("rexgpu-nb:   replay chunks cumulative: {} sessions / {} queued chunks / {} completed chunks / {} finished sessions / {} failures",
                chunks[0], chunks[1], chunks[2], chunks[3], chunks[4]);

    if (constant_upload_pool_) {
      const auto& pool = constant_upload_pool_->stats();
      REXLOG_INFO("rexgpu-nb:   native constant pool cumulative: {} requests / {} bytes, {} unserved, "
                  "{} video pages / {} upload pages, {} video bytes held, {} creation failures / {} budget stops, {} reclaims",
                  pool.requests, pool.requested_bytes, pool.request_failures, pool.gpu_pages,
                  pool.upload_pages, pool.gpu_bytes, pool.creation_failures, pool.budget_stops,
                  pool.reclaims);
    }

    if (REXCVAR_GET(nb_native_submission_diagnostics)) {
      const auto& submissions = rex::graphics::d3d12::GetNativeSubmissionStats();
      const auto log_stage = [](const char* name, const auto& stage) {
        REXLOG_INFO("rexgpu-nb:   submission {} cumulative: {} calls / {} ns / {} failures",
                    name, stage.calls, stage.nanoseconds, stage.failures);
      };
      log_stage("allocator_reset", submissions.allocator_reset);
      log_stage("list_reset", submissions.list_reset);
      log_stage("replay", submissions.replay);
      log_stage("close", submissions.close);
      log_stage("execute", submissions.execute);
      log_stage("fence_wait", submissions.fence_wait);
      log_stage("queue_operation_wait", submissions.queue_operation_wait);
      REXLOG_INFO("rexgpu-nb:   submission barriers cumulative: {} batches / {} total / {} maximum batch, {} transition / {} aliasing / {} UAV / {} unknown, {} shared / {} to copy / {} from copy",
                  submissions.barrier_batches, submissions.barriers_total, submissions.max_barriers_per_batch,
                  submissions.transition_barriers, submissions.aliasing_barriers, submissions.uav_barriers,
                  submissions.unknown_barriers, submissions.shared_memory_transitions,
                  submissions.shared_memory_to_copy_dest, submissions.shared_memory_from_copy_dest);
      REXLOG_INFO("rexgpu-nb:   upload order cumulative: {} native attempts / {} successes, {} texture-read draws / {} native-copy draws / {} candidates, {} late-copy transitions / {} completed cycles / {} refused candidates / {} attribution bypasses",
                  submissions.upload_order_native_attempts, submissions.upload_order_native_draws,
                  submissions.upload_order_texture_read_draws, submissions.upload_order_copy_dest_draws,
                  submissions.upload_order_candidate_draws, submissions.upload_order_late_copy_dest_transitions,
                  submissions.upload_order_candidate_cycles, submissions.upload_order_refused_candidate_draws,
                  submissions.upload_order_attribution_bypasses);
    }
    REXLOG_INFO("rexgpu-nb:   static root bindings cumulative: {} hits / {} rebinds", bindings.hits, bindings.rebinds);
    REXLOG_INFO("rexgpu-nb:   pixel root bindings cumulative: {} hits / {} writes", bindings.pixel_hits, bindings.pixel_rebinds);
    const auto& texture_plans = native_texture_binding_cache_stats();
    const auto& translations = GetNativeTranslationLookupMemoStats();
    REXLOG_INFO("rexgpu-nb:   translation lookup memo cumulative: {} hits / {} misses / {} publications / {} invalidations",
                translations.hits, translations.misses, translations.publications, translations.invalidations);
    const auto& texture_descriptors = GetNativeTextureDescriptorMemoStats();
    REXLOG_INFO("rexgpu-nb:   texture descriptor memo cumulative: {} hits / {} misses / {} publications / {} failed lookups",
                texture_descriptors.hits, texture_descriptors.misses, texture_descriptors.publications,
                texture_descriptors.failed_lookups);
    const auto ready_pairs = native_pair_lookup_memo_stats();
    REXLOG_INFO("rexgpu-nb:   ready pair memo cumulative: {} hits / {} misses / {} publications / {} resets",
                ready_pairs.hits, ready_pairs.misses, ready_pairs.publications, ready_pairs.resets);
    REXLOG_INFO("rexgpu-nb:   texture binding plans cumulative: {} hits / {} misses / {} bypasses / {} resets, {} sampler hits / {} misses / {} resolutions",
                texture_plans.plan_hits, texture_plans.plan_misses, texture_plans.plan_bypasses, texture_plans.resets,
                texture_plans.sampler_hits, texture_plans.sampler_misses, texture_plans.sampler_resolutions);
    const auto& uploads = GetNativeUploadShadowStats();
    REXLOG_INFO("rexgpu-nb:   original shared uploads cumulative: {} copies / {} bytes",
                uploads.original_copy_commands, uploads.original_copy_bytes);
    const auto join_buckets = [](const uint64_t (&values)[kNativeUploadSizeBuckets]) {
      std::string text;
      for (size_t i = 0; i < kNativeUploadSizeBuckets; ++i) {
        if (i) text += ',';
        text += std::to_string(values[i]);
      }
      return text;
    };
    REXLOG_INFO("rexgpu-nb:   original shared upload sizes cumulative (pages 1,2,4,...,256,512+): counts {} / bytes {}",
                join_buckets(uploads.original_size_counts), join_buckets(uploads.original_size_bytes));
    REXLOG_INFO("rexgpu-nb:   upload shadow cumulative: {} enabled / {} disabled calls / {} init failures, {} reserved / {} snapshot bytes, {} matched / {} compared pages, {} copied / {} skipped bytes, {} copy commands / {} all-matched chunks, {} authority fallback bytes / {} publication rejects / {} evictions",
                uploads.enabled_calls, uploads.disabled_calls, uploads.initialization_failures,
                uploads.reserved_bytes, uploads.snapshot_bytes, uploads.matched_pages, uploads.compared_pages,
                uploads.copied_bytes, uploads.skipped_bytes, uploads.copy_commands, uploads.all_matched_chunks,
                uploads.authority_fallback_bytes, uploads.publication_rejects, uploads.evictions);
    REXLOG_INFO("rexgpu-nb:   upload shadow invalidation retries: {} / {} allocation failures",
                uploads.invalidation_retries, uploads.retry_allocation_failures);
    REXLOG_INFO("rexgpu-nb:   native guest pipeline skips cumulative: {}", native_guest_pipeline_skips());
    const auto& memo = native_shader_load_memo_stats();
    REXLOG_INFO("rexgpu-nb:   shader load memo cumulative: {} hits / {} misses / {} bypasses / {} content misses, {} bytes compared / {} bytes hash avoided, {} publications / {} evictions / {} resets",
                memo.hits, memo.misses, memo.bypasses, memo.content_misses, memo.bytes_compared, memo.bytes_hash_avoided,
                memo.publications, memo.evictions, memo.resets);
    const auto& indices = NativeGeometryPass::index_draw_stats();
    REXLOG_INFO("rexgpu-nb:   index draws cumulative: {} hardware / {} eligible manual, {} hardware indices / {} eligible manual indices",
                indices.hardware_draws, indices.manual_eligible_draws, indices.hardware_indices, indices.manual_eligible_indices);
    const auto& affinity = cpu_affinity_.status();
    REXLOG_INFO("rexgpu-nb:   command thread affinity: active {} thread {} group {} original {:X} performance {:X} class {} ideal {}, {} enables / {} restores / {} failures, error {} ({})",
                affinity.active, affinity.thread_id, affinity.group, affinity.original_mask, affinity.performance_mask,
                affinity.performance_class, affinity.original_ideal_cpu, affinity.enables, affinity.restores,
                affinity.failures, affinity.windows_error, affinity.reason);
    REXLOG_INFO("rexgpu-nb:   command thread placement: mode {} applied {:X} current ideal {}:{} sampled {}:{}, {} samples / {} observed changes",
                affinity.placement_mode, affinity.applied_mask, affinity.current_ideal_group, affinity.current_ideal_cpu,
                affinity.sampled_group, affinity.sampled_cpu, affinity.processor_samples, affinity.sampled_processor_changes);
    REXLOG_INFO("rexgpu-nb:   command thread scheduling class: {} subset {:X}", affinity.scheduling_class, affinity.scheduling_mask);
    const auto& reuse = NativeGeometryPass::constant_reuse_stats();
    const auto& binding_packets = native_constant_binding_packet_stats();
    REXLOG_INFO("rexgpu-nb:   constant binding packets cumulative: {} packets / {} pixel writes",
                binding_packets.packets, binding_packets.pixel_writes);
    REXLOG_INFO("rexgpu-nb:   empty constant layouts cumulative: {} VS packets / {} PS packets, {} logical bytes / {} reserved bytes omitted / {} proof bypasses",
                reuse.empty_vs_packets, reuse.empty_ps_packets,
                reuse.empty_constant_bytes_avoided, reuse.empty_constant_reserved_bytes_avoided,
                reuse.empty_layout_bypasses);
    const auto& pipeline_keys = NativeGeometryPass::pipeline_key_stats();
    REXLOG_INFO("rexgpu-nb:   normalized pipeline keys cumulative: {} lookups / {} changed, {} memo hits / {} map hits / {} pending / {} known bad / {} creation requests",
                pipeline_keys.normalized_lookups, pipeline_keys.changed_key_draws,
                pipeline_keys.ready_memo_hits, pipeline_keys.ready_map_hits,
                pipeline_keys.pending_hits, pipeline_keys.known_bad_hits, pipeline_keys.create_requests);
    if (REXCVAR_GET(nb_native_pipeline_binding_diagnostics)) {
      const auto& pipelines = rex::graphics::d3d12::GetNativeSubmissionStats();
      REXLOG_INFO("rexgpu-nb:   external pipelines cumulative: {} binds / {} pointer reuses",
                  pipelines.external_pipeline_binds, pipelines.external_pipeline_reuses);
    }
    REXLOG_INFO("rexgpu-nb:   constant reuse cumulative: {} PS hits / {} misses / {} bypasses, {} depth aliases, {} bytes / {} reserved bytes avoided, {} pipeline hits / {} misses",
                reuse.ps_hits, reuse.ps_misses, reuse.ps_bypasses, reuse.depth_only_aliases,
                reuse.ps_bytes_avoided, reuse.ps_reserved_bytes_avoided, reuse.pipeline_hits, reuse.pipeline_misses);
    REXLOG_INFO("rexgpu-nb:   unused pixel constants cumulative: {} packet builds / {} logical RAM bytes skipped / {} aliases",
                reuse.unused_ps_packet_builds_skipped, reuse.unused_ps_packet_bytes_skipped,
                reuse.unused_ps_aliases);
    const auto& texture_poll = GetNativeTextureOutdatedPollStats();
    REXLOG_INFO("rexgpu-nb:   texture outdated polling cumulative: {} false loads / {} exchanges / {} notifications consumed",
                texture_poll.false_load_skips, texture_poll.exchanges,
                texture_poll.consumed_notifications);
    REXLOG_INFO("rexgpu-nb:   vertex staging cumulative: {} uploads / {} bytes / {} bypasses",
                reuse.vs_staged_uploads, reuse.vs_staged_bytes, reuse.vs_stage_bypasses);
    REXLOG_INFO("rexgpu-nb:   vertex constant reuse cumulative: {} hits / {} misses / {} bypasses, {} bytes / {} reserved bytes avoided",
                reuse.vs_constant_hits, reuse.vs_constant_misses, reuse.vs_constant_bypasses,
                reuse.vs_constant_bytes_avoided, reuse.vs_constant_reserved_bytes_avoided);
    REXLOG_INFO("rexgpu-nb:   pixel packet swaps cumulative: {} publications / {} RAM copy bytes avoided",
                reuse.ps_packet_publications, reuse.ps_packet_copy_bytes_avoided);
    REXLOG_INFO("rexgpu-nb:   root CBV cumulative: {} draws / {} payload bytes",
                reuse.root_cbv_draws, reuse.root_cbv_bytes);
    REXLOG_INFO("rexgpu-nb:   direct guest reads cumulative: {} draws / {} asset-pointer refusals",
                reuse.direct_guest_draws, reuse.direct_guest_refusals);
    REXLOG_INFO("rexgpu-nb:   vertex packet probe cumulative: {} observations / {} exact matches / {} equal bytes / {} bypasses / {} per-pass frame transitions",
                reuse.vs_packet_observations, reuse.vs_packet_hits, reuse.vs_packet_equal_bytes,
                reuse.vs_packet_bypasses, reuse.vs_packet_pass_frames);
    if (REXCVAR_GET(nb_native_invalidation_diagnostics)) {
      // Snapshot under the global memory lock, then release it before logging.
      const auto invalidation = SnapshotNativeInvalidationStats();
      REXLOG_INFO("rexgpu-nb:   CPU invalidation cumulative: {} callbacks / {} exact / {} enabled / {} narrowed, {} requested / {} original / {} selected pages, {} retained / {} potential CPU pages",
                  invalidation.callbacks, invalidation.exact_callbacks, invalidation.enabled_callbacks,
                  invalidation.narrowed_callbacks, invalidation.requested_pages, invalidation.original_pages,
                  invalidation.selected_pages, invalidation.retained_cpu_pages, invalidation.potential_retained_cpu_pages);
    }
    const auto cache_stats = shader_cache::GetStats();
    REXLOG_INFO("rexgpu-nb:   shader disk cache cumulative: {} hits / {} misses / {} rejected / {} published / {} write failures",
                cache_stats.hits, cache_stats.misses, cache_stats.rejected, cache_stats.published, cache_stats.write_failures);
    if (REXCVAR_GET(nb_range_batch_diagnostics)) {
      const auto& ranges = GetNativeRangeBatchStats();
      REXLOG_INFO("rexgpu-nb:   range batches cumulative: {} calls, {} stack / {} heap, {} overlap pages saved, {} page visits, {} uploads / {} no-upload calls",
                  ranges.calls, ranges.stack_calls, ranges.heap_calls, ranges.overlap_pages_saved,
                  ranges.page_visits, ranges.upload_calls, ranges.no_upload_calls);
      REXLOG_INFO("rexgpu-nb:   single range fast calls cumulative: {}", ranges.single_fast_calls);
    }
    if (NativeRangeDiagnosticsEnabled()) {
      constexpr const char* caller_names[] = {"other", "index", "texture", "native"};
      const auto diagnostics = GetNativeRangeDiagnosticsStats();
      for (size_t i = 0; i < std::size(caller_names); ++i) {
        const auto& r = diagnostics.callers[i];
        REXLOG_INFO("rexgpu-nb:   range {} outcomes cumulative: {} calls / {} top level, {} inputs / {} normalized / {} requested bytes / {} page visits, {} empty / {} invalid / {} allocation failures / {} no-upload / {} upload / {} upload failures / {} aborted, {} upload ranges / {} upload pages",
                    caller_names[i], r.calls, r.top_level_calls, r.input_ranges, r.normalized_ranges,
                    r.requested_bytes, r.requested_page_visits, r.empty_calls, r.invalid_calls,
                    r.allocation_failures, r.no_upload_calls, r.upload_calls, r.upload_failures,
                    r.aborted_calls, r.upload_range_count, r.upload_page_count);
        REXLOG_INFO("rexgpu-nb:   range {} timing cumulative: {} total ns / {} top-level ns / {} no-upload ns / {} upload ns, {} lock calls / {} acquisition ns / {} held ns",
                    caller_names[i], r.total_ns, r.top_level_ns, r.no_upload_ns, r.upload_ns,
                    r.lock_calls, r.lock_acquire_ns, r.lock_hold_ns);
      }
    }
    const auto& mirror = GetNativeSharedResidencyStats();
    REXLOG_INFO("rexgpu-nb:   shared residency mirror cumulative: {} requests / {} hits / {} misses, {} hit ranges / {} bytes / {} pages, {} promoted pages, {} recursive / {} page-size / {} caller bypasses, {} initialization failures / {} mode resets",
                mirror.requests, mirror.hits, mirror.misses, mirror.hit_ranges, mirror.hit_bytes,
                mirror.hit_pages, mirror.promoted_pages, mirror.recursive_bypasses,
                mirror.unsupported_page_bypasses, mirror.unsupported_caller_bypasses,
                mirror.initialization_failures, mirror.mode_resets);
    REXLOG_INFO("rexgpu-nb:   command CPU per frame: shader loads {:.3f} ms, copies {:.3f} ms, swaps {:.3f} ms (nested draw/copy costs overlap)",
                shader_load_ns_ / 1.0e6 / frames, issue_copy_ns_ / 1.0e6 / frames, issue_swap_ns_ / 1.0e6 / frames);
    const auto snapshot_bytes = native_asset_cache_.stats().snapshot_bytes;
    REXLOG_INFO("rexgpu-nb: perf: mode {} window {} phase {} frames {} fps {:.3f} frame_ms {:.3f} native_pct {:.3f} draws_per_frame {:.1f} IssueDraw_ms {:.3f} generic_ms {:.3f} residency_ms {:.3f} assets_ms {:.3f} snapshot_bytes_per_frame {:.1f}",
                asset_cache_mode_, phase.window, phase.name(), frames_since_log_,
                frame_wall_ns_ ? double(interval_timed_frames_) * 1.0e9 / double(frame_wall_ns_) : 0.0,
                interval_timed_frames_ ? double(frame_wall_ns_) / 1.0e6 / double(interval_timed_frames_) : 0.0,
                interval_draws_ ? double(interval_native_draws_) * 100.0 / double(interval_draws_) : 0.0,
                double(interval_draws_) / frames, issue_draw_ns_ / 1.0e6 / frames, generic_pass_ns_ / 1.0e6 / frames,
                timings.residency_ns / 1.0e6 / frames, timings.asset_match_ns / 1.0e6 / frames,
                double(snapshot_bytes - interval_snapshot_bytes_) / frames);
    REXLOG_INFO("rexgpu-nb:   native draw CPU per frame: pipeline {:.2f} ms, constants {:.2f} ms, "
                "RequestRange {:.2f} ms, assets {:.2f} ms, UseForReading {:.2f} ms, record {:.2f} ms, texture request {:.2f} ms, texture bindings {:.2f} ms; residency {} hit / {} miss, {} invalidations folded",
                timings.pipeline_ns / 1.0e6 / frames,
                timings.constants_ns / 1.0e6 / frames,
                timings.residency_ns / 1.0e6 / frames,
                timings.asset_match_ns / 1.0e6 / frames,
                timings.use_reading_ns / 1.0e6 / frames,
                timings.record_ns / 1.0e6 / frames,
                timings.texture_request_ns / 1.0e6 / frames,
                timings.textures_ns / 1.0e6 / frames,
                timings.residency_hits / frames_since_log_, timings.residency_misses / frames_since_log_,
                timings.residency_resets / frames_since_log_);
    timings = NativeGeometryPass::Timings{};
    std::memset(generic_refusals_, 0, sizeof generic_refusals_);
    native_empty_texture_draws_ = 0;
    native_triangle_strip_draws_ = 0;
    native_stacked_texture_draws_ = 0;
    native_cube_texture_draws_ = 0;
    native_extra_stream_draws_ = 0;
    std::memset(texture_failures_, 0, sizeof texture_failures_);
    std::memset(texture_failure_dimensions_, 0, sizeof texture_failure_dimensions_);
    issue_draw_ns_ = 0;
    generic_pass_ns_ = 0;
    shader_load_ns_ = 0;
    issue_copy_ns_ = 0;
    issue_swap_ns_ = 0;
    frame_wall_ns_ = 0;
    interval_timed_frames_ = 0;
    interval_draws_ = 0;
    interval_native_draws_ = 0;
    interval_snapshot_bytes_ = snapshot_bytes;
    frames_since_log_ = 0;
  }
  if (frame_trace_) {
    for (uint32_t i = 0; i != kRefusedCount; ++i) {
      frame_trace_previous_refusals_[i] = generic_refusals_[i];
    }
  }
  // The just-completed frame uses the old mode; change only for the next frame.
  asset_cache_mode_ = next_phase.mode;
  if (REXCVAR_GET(nb_native_perf_benchmark_start) > 0) {
    const int64_t selected_options = next_phase.options >= 0 ? next_phase.options : perf_default_options_;
    ApplyPerfOptions(selected_options);
    if (next_phase.options != phase.options) {
      REXLOG_INFO("rexgpu-nb: applied performance options {} beginning frame {}", selected_options, f + 1);
    }
  }
  cpu_affinity_.Update(REXCVAR_GET(nb_native_gpu_pcores),
                       uint32_t(std::clamp(REXCVAR_GET(nb_native_gpu_placement), 0, 7)));
  if (phase.window >= 0 && next_phase.window < 0) {
    REXLOG_INFO("rexgpu-nb: {} benchmark complete", REXCVAR_GET(nb_native_perf_benchmark_start) > 0 ? "performance" : "asset");
  }
  // The draws of frame f were recorded under index f (stats_.frames + 1 at the time); flush them now.
  NbCpRecorder::Get().EndFrameAndFlush(f);
  draws_this_frame_ = 0;
  copies_this_frame_ = 0;
  shaders_this_frame_ = 0;
  native_this_frame_ = 0;
}

rex::graphics::Shader* NbCommandProcessor::LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                                      uint32_t guest_address,
                                                      const uint32_t* host_address,
                                                      uint32_t dword_count) {
  stats_.shaders_loaded++;
  shaders_this_frame_++;
  const bool timed = !REXCVAR_GET(nb_native_minimal_command_diagnostics);
  const auto start = NativeTimingStart(timed);
  auto* result = D3D12CommandProcessor::LoadShader(shader_type, guest_address, host_address, dword_count);
  shader_load_ns_ += NativeTimingElapsed(timed, start);
  return result;
}

bool NbCommandProcessor::IssueDraw(rex::graphics::xenos::PrimitiveType primitive_type,
                                   uint32_t index_count, IndexBufferInfo* index_buffer_info,
                                   bool major_mode_explicit) {
  stats_.draws++;
  draws_this_frame_++;
  // Frame inventory: snapshot the state this draw consumes before forwarding it. register_file_
  // and active_*_shader() are CommandProcessor members visible to the subclass.
  auto& recorder = NbCpRecorder::Get();
  if (recorder.WantsFrame(stats_.frames + 1)) {
    recorder.RecordDraw(stats_.frames + 1, primitive_type, index_count,
                        index_buffer_info ? index_buffer_info->guest_base : 0,
                        index_buffer_info ? index_buffer_info->count : 0,
                        index_buffer_info ? static_cast<uint32_t>(index_buffer_info->format) : 0,
                        major_mode_explicit, register_file_, active_vertex_shader(), active_pixel_shader());
  }
  const bool timed = !REXCVAR_GET(nb_native_minimal_command_diagnostics);
  const auto t0 = NativeTimingStart(timed);
  const bool result = D3D12CommandProcessor::IssueDraw(primitive_type, index_count, index_buffer_info, major_mode_explicit);
  issue_draw_ns_ += NativeTimingElapsed(timed, t0);
  return result && ReplayNativeDrawBoundary();
}

bool NbCommandProcessor::IssueCopy() {
  stats_.copies++;
  copies_this_frame_++;
  auto& recorder = NbCpRecorder::Get();
  if (recorder.WantsFrame(stats_.frames + 1)) {
    recorder.RecordCopy(stats_.frames + 1, register_file_, active_vertex_shader(), active_pixel_shader());
  }
  const bool timed = !REXCVAR_GET(nb_native_minimal_command_diagnostics);
  const auto start = NativeTimingStart(timed);
  const bool result = D3D12CommandProcessor::IssueCopy();
  issue_copy_ns_ += NativeTimingElapsed(timed, start);
  return result;
}

float NbCommandProcessor::PixelConstant(uint32_t index, uint32_t component) const {
  // SQ_PS_CONST is 0x000FF100 in this game (base 256, 255 constants), the layout the SDK asserts.
  const uint32_t reg = rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X + 4 * index + component;
  float f;
  std::memcpy(&f, &register_file_->values[reg], sizeof f);
  return f;
}

// Every caller runs inside TryNativeDraw, after IssueDraw requested the same
// vertex/pixel used-texture mask. Guest fetch registers cannot change within the
// seam, so the current active bindings are already available. Keep the second
// request only for the diagnostic comparison with the previous behavior.
void NbCommandProcessor::RequestDrawTextures(const NativeDrawContext& context) {
  if (REXCVAR_GET(nb_native_defer_guest_pipeline)) return;
  NativeRangeCallerScope origin(NativeRangeCaller::kTextureRequest, NativeRangeDiagnosticsEnabled());
  const bool detailed = !REXCVAR_GET(nb_native_minimal_diagnostics);
  const auto start = NativeTimingStart(detailed);
  uint32_t used_texture_mask = 0;
  if (context.vertex_shader) used_texture_mask |= context.vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (context.pixel_shader) used_texture_mask |= context.pixel_shader->GetUsedTextureMaskAfterTranslation();
  texture_cache().RequestTextures(used_texture_mask);
  NativeGeometryPass::timings().texture_request_ns +=
      NativeTimingElapsed(detailed, start);
}

uint32_t NbCommandProcessor::GuestTextureIndex(const rex::graphics::d3d12::D3D12Shader& shader,
                                               uint32_t fetch_constant, bool allow_empty, uint32_t dimension,
                                               const TextureBindingCache::Plan* plan) {
  const bool trace = !REXCVAR_GET(nb_native_trace_pair).empty();
  // The translator emits a binding for each (fetch constant, dimension, is_signed) and lets the
  // emulated shader pick between the pair at runtime from the swizzled signs. The texture cache builds
  // only the descriptor those signs call for - unsigned when some component is unsigned, signed when
  // some is signed - so the other half of the pair legitimately resolves to the null texture. Taking
  // the first binding that matched the fetch constant and refusing on null therefore threw away three
  // thousand draws a frame on textures that were bound perfectly well.
  //
  // Our HLSL samples one view, so this takes whichever of the pair the cache actually built. For a
  // fetch whose components are all signed or all unsigned that is unambiguous: exactly one exists, and
  // it is the one the guest shader would have selected. Mixed signs need the per-component blend of
  // both views that only the emulated shader does, so those still refuse.
  const uint8_t signs = texture_cache().GetActiveTextureSwizzledSigns(fetch_constant);
  const bool mixed_signs = rex::graphics::texture_util::IsAnySignSigned(signs) &&
                           rex::graphics::texture_util::IsAnySignNotSigned(signs);
  bool matched_binding = false;
  bool had_descriptor = false;
  const auto expected_dimension = dimension == 4 ? rex::graphics::xenos::FetchOpDimension::kCube
                                                 : rex::graphics::xenos::FetchOpDimension::k2D;
  const uint32_t expected_null = static_cast<uint32_t>(dimension == 4 ? SystemBindlessView::kNullTextureCube
                                                                    : SystemBindlessView::kNullTexture2DArray);
  const auto bindings = plan ? plan->Textures(fetch_constant)
                            : std::span<const rex::graphics::d3d12::D3D12Shader::TextureBinding>(
                                  shader.GetTextureBindingsAfterTranslation());
  for (const auto& binding : bindings) {
    if (binding.fetch_constant != fetch_constant) continue;
    if (binding.dimension != expected_dimension) continue;
    matched_binding = true;
    if (mixed_signs) continue;
    const uint32_t heap_index = texture_cache().GetActiveTextureBindlessSRVIndex(binding);
    if (heap_index == UINT32_MAX) continue;
    had_descriptor = true;
    // kNullTexture2DArray sits at kUnboundedSRVsStart. A null view for a populated fetch remains a
    // failure: silently using it can turn shadow atlases into fully shadowed surfaces.
    const uint32_t shader_index =
        heap_index - static_cast<uint32_t>(SystemBindlessView::kUnboundedSRVsStart);
    if (heap_index != expected_null) {
      return shader_index;
    }
    if (allow_empty) {
      const auto fetch = register_file_->GetTextureFetch(fetch_constant);
      if (((fetch.base_address | fetch.mip_address) & 0x1FFFFu) == 0) {
        // Shader metadata includes fetches in runtime branches that may never execute. An entirely
        // absent base/mip data range is explicitly empty in the SDK, even if other fetch fields are
        // nonzero or gpu_allow_invalid_fetch_constants is enabled. Invalid bindings have unsigned
        // signs and an all-zero component mapping. Match that zero instead of refusing the draw.
        static uint32_t empty_fetches_logged = 0;
        if (empty_fetches_logged++ < 8) {
          REXLOG_INFO("rexgpu-nb: empty texture tf{} uses SDK zero: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                      fetch_constant, fetch.dword_0, fetch.dword_1, fetch.dword_2,
                      fetch.dword_3, fetch.dword_4, fetch.dword_5);
        }
        return 0;
      }
    }
  }
  if (!matched_binding) {
    ++texture_failures_[kTextureNoBinding];
    for (const auto& binding : shader.GetTextureBindingsAfterTranslation()) {
      if (binding.fetch_constant == fetch_constant) {
        ++texture_failure_dimensions_[static_cast<uint32_t>(binding.dimension) & 3];
      }
    }
    if (trace) {
      REXLOG_WARN("rexgpu-nb: tf{} has no expected native dimension {} binding", fetch_constant, dimension);
    }
    return UINT32_MAX;
  }
  if (mixed_signs) {
    ++texture_failures_[kTextureMixedSigns];
  } else if (had_descriptor) {
    ++texture_failures_[kTextureUnexpectedNull];
  } else {
    ++texture_failures_[kTextureNoDescriptor];
  }
  if (trace) {
    // Only the failures are worth logging: doing it for every binding buries everything else, at
    // 35,000 lines a rotation.
    const auto fetch = register_file_->GetTextureFetch(fetch_constant);
    REXLOG_INFO("rexgpu-nb: tf{} unbound; fetch type {} dim {} base 0x{:X} fmt {} signs 0x{:02X}",
                fetch_constant, static_cast<uint32_t>(fetch.type),
                static_cast<uint32_t>(fetch.dimension), uint32_t(fetch.base_address),
                static_cast<uint32_t>(fetch.format), signs);
  }
  return UINT32_MAX;
}

void NbCommandProcessor::LogNativeDraw(const char* pass, const NativeDrawContext& context) {
  stats_.native_draws++;
  native_this_frame_++;
  // One frame logged draw by draw, in order, is how a surface gets traced back to its shader pair: the
  // skydome and the other big early draws are identifiable by position in the frame plus their depth
  // state, which no periodic sample can show.
  const int64_t log_frame = REXCVAR_GET(nb_log_frame);
  if (log_frame > 0 && static_cast<uint64_t>(log_frame) == stats_.frames + 1) {
    rex::graphics::reg::RB_DEPTHCONTROL depth;
    depth.value = register_file_->Get<uint32_t>(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
    REXLOG_INFO("rexgpu-nb: frameseq {} pair {:016X}_{:016X} prim {} x{} zenable {} zwrite {} zfunc {} rtv0 {} dsv {}",
                native_this_frame_, context.vertex_shader->ucode_data_hash(),
                context.pixel_shader ? context.pixel_shader->ucode_data_hash() : 0ull,
                static_cast<uint32_t>(context.guest_primitive_type), context.index_count,
                uint32_t(depth.z_enable), uint32_t(depth.z_write_enable), uint32_t(depth.zfunc),
                int(context.rtv_formats[0]), int(context.dsv_format));
  }
  if (stats_.native_draws <= 3 ||
      (!REXCVAR_GET(nb_native_minimal_diagnostics) && (stats_.native_draws % 600) == 0)) {
    REXLOG_INFO("rexgpu-nb: native {} draw #{} in frame {}: replaced guest {} prim {} x{} (vs {:016X}, indx_offset {}) on rtv "
                "[{},{},{},{}] dsv {} x{}",
                pass, stats_.native_draws, stats_.frames + 1, context.indexed ? "indexed" : "non-indexed",
                static_cast<uint32_t>(context.guest_primitive_type), context.index_count,
                context.vertex_shader->ucode_data_hash(), regs_index_offset(*register_file_), int(context.rtv_formats[0]),
                int(context.rtv_formats[1]), int(context.rtv_formats[2]), int(context.rtv_formats[3]),
                int(context.dsv_format), context.sample_count);
  }
}

bool NbCommandProcessor::TryNativeDraw(const NativeDrawContext& context) {
  if (!context.pixel_shader) {
    // Depth-only draws (shadow casters): only the generic pass handles them, with a vertex-only entry.
    return REXCVAR_GET(nb_native_generic) && TryGenericPass(context);
  }
  if (REXCVAR_GET(nb_native_bloom) && TryBloomCompositePass(context)) {
    return true;
  }
  if (REXCVAR_GET(nb_native_bloom_blur) && TryBloomBlurPass(context)) {
    return true;
  }
  if (REXCVAR_GET(nb_native_sprite) && TrySpritePass(context)) {
    return true;
  }
  if (REXCVAR_GET(nb_native_dof) && TryDofFogPass(context)) {
    return true;
  }
  if (REXCVAR_GET(nb_native_generic) && TryGenericPass(context)) {
    return true;
  }
  if (REXCVAR_GET(nb_native_demo) && TryDemoPass(context)) {
    return true;
  }
  return false;
}

bool NbCommandProcessor::TryDemoPass(const NativeDrawContext& context) {
  if (!demo_ps_hash_parsed_) {
    demo_ps_hash_parsed_ = true;
    const std::string text = REXCVAR_GET(nb_native_demo_ps);
    demo_ps_hash_ = text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 16);
    REXLOG_INFO("rexgpu-nb: native demo armed for pixel shader {:016X}", demo_ps_hash_);
  }
  if (demo_ps_hash_ == 0 || context.pixel_shader->ucode_data_hash() != demo_ps_hash_) {
    return false;
  }
  if (!demo_pass_init_attempted_) {
    demo_pass_init_attempted_ = true;
    demo_pass_.Initialize(GetD3D12Provider().GetDevice(), "demo", kDemoPs, NativeFullscreenPass::Options{});
  }
  if (!demo_pass_.initialized()) {
    return false;
  }
  uint32_t constants[NativeFullscreenPass::kConstantDwords] = {};
  const auto& vp = *context.viewport_info;
  constants[0] = FloatBits(static_cast<float>(vp.xy_extent[0]));
  constants[1] = FloatBits(static_cast<float>(vp.xy_extent[1]));
  constants[2] = FloatBits(std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time_).count());
  constants[3] = FloatBits(static_cast<float>(stats_.frames + 1));
  if (!demo_pass_.Record(*this, context, constants)) {
    return false;
  }
  LogNativeDraw("demo", context);
  return true;
}

bool NbCommandProcessor::TryBloomCompositePass(const NativeDrawContext& context) {
  // Game-specific transcriptions are excluded from the source release.
  // The generic native path or SDK fallback handles this draw.
  return false;
}

bool NbCommandProcessor::TryBloomBlurPass(const NativeDrawContext& context) {
  // Game-specific transcriptions are excluded from the source release.
  // The generic native path or SDK fallback handles this draw.
  return false;
}

bool NbCommandProcessor::PrepareGeometry(const NativeDrawContext& context, NativeGeometryPass::RootConstants& root,
                                         NativeGeometryPass::GuestState& state, NativeGeometryPass::DrawArgs& args,
                                         const std::vector<NativeShaderLibrary::Stream>* streams) {
  using namespace rex::graphics;
  // A refused draw stays emulated; the first few refusals say why so a silent pass can be diagnosed.
  auto refuse = [&](const char* why) {
    static std::unordered_map<std::string, uint32_t> logged;
    if (logged[why]++ < 4 ||
        (REXCVAR_GET(nb_log_frame) > 0 && uint64_t(REXCVAR_GET(nb_log_frame)) == stats_.frames + 1)) {
      REXLOG_WARN("rexgpu-nb: native geometry refused: {} (vs {:016X} ps {:016X}, prim {}, {} x{}, ib 0x{:08X})", why,
                  context.vertex_shader->ucode_data_hash(),
                  context.pixel_shader ? context.pixel_shader->ucode_data_hash() : 0,
                  static_cast<uint32_t>(context.guest_primitive_type), context.indexed ? "indexed" : "non-indexed",
                  context.index_count, context.index_guest_base);
    }
    return false;
  };
  root = NativeGeometryPass::RootConstants{};
  args = NativeGeometryPass::DrawArgs{};
  // Streams: a generated pass names its vertex fetch constants and the strides its vfetch instructions
  // carry; the hand-written passes take the guest vertex shader's first binding.
  NativeShaderLibrary::Stream first_binding{};
  const NativeShaderLibrary::Stream* stream_list = nullptr;
  size_t stream_count = 0;
  if (streams) {
    stream_list = streams->data();
    stream_count = streams->size();
  } else {
    const auto& bindings = context.vertex_shader->vertex_bindings();
    if (bindings.empty()) {
      return refuse("vertex shader has no vertex bindings");
    }
    first_binding.fetch_constant = bindings[0].fetch_constant;
    first_binding.stride_dwords = bindings[0].stride_words;
    stream_list = &first_binding;
    stream_count = 1;
  }
  if (stream_count > NativeGeometryPass::kMaxVertexStreams) {
    return refuse("vertex stream count exceeds native limit");
  }
  // Zero streams is fine: particles build everything from constants and the vertex index.
  for (size_t i = 0; i < stream_count; ++i) {
    if (stream_list[i].fetch_constant >= 96 || stream_list[i].stride_dwords > 255) {
      return refuse("vertex stream descriptor exceeds Xenos bounds");
    }
    const auto fetch = register_file_->GetVertexFetch(stream_list[i].fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || fetch.address == 0) {
      return refuse("vertex fetch constant not a vertex buffer");
    }
    if (i == 0) {
      root.fetch_base_bytes = fetch.address << 2;
      root.fetch_stride_dwords = stream_list[i].stride_dwords;
      root.fetch_endian = static_cast<uint32_t>(fetch.endian);
    } else if (i == 1) {
      root.fetch1_base_bytes = fetch.address << 2;
      root.fetch1_stride_dwords = stream_list[i].stride_dwords;
      root.fetch1_endian = static_cast<uint32_t>(fetch.endian);
    } else {
      args.extra_vertex_streams[i - 2][0] = fetch.address << 2;
      args.extra_vertex_streams[i - 2][1] = stream_list[i].stride_dwords;
      args.extra_vertex_streams[i - 2][2] = static_cast<uint32_t>(fetch.endian);
    }
    args.fetch_size_bytes[i] = fetch.size << 2;
  }
  args.vertex_stream_count = static_cast<uint32_t>(stream_count);
  root.index_offset = regs_index_offset(*register_file_);
  root.vertex_index_min = register_file_->Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  root.vertex_index_max = register_file_->Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  root.index_base_bytes = 0xFFFFFFFFu;
  if (context.indexed) {
    if (context.index_guest_base == 0) {
      return refuse("indexed draw without a guest index buffer");
    }
    root.index_base_bytes = context.index_guest_base;
    root.index_format = context.index_format == xenos::IndexFormat::kInt32 ? 1u : 0u;
    root.index_endian = static_cast<uint32_t>(context.index_endian);
    const uint64_t index_bytes = uint64_t(context.index_count) * (root.index_format ? 4u : 2u);
    if (index_bytes > UINT32_MAX) return refuse("guest index extent exceeds native bounds");
    args.index_size_bytes = static_cast<uint32_t>(index_bytes);
  }
  switch (context.guest_primitive_type) {
    case xenos::PrimitiveType::kTriangleList:
      args.hardware_index_eligible = context.hardware_index_eligible;
      args.use_hardware_indices = context.hardware_index_eligible && context.use_hardware_indices;
      root.primitive_mode = args.use_hardware_indices ? 4u : 0u;
      args.host_vertex_count = context.index_count;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      // Indexed strips need primitive-reset handling. The measured non-indexed strips can expand
      // entirely in VertexIndex, without an intermediate index buffer or a geometry shader.
      if (context.indexed) return refuse("indexed triangle strip");
      root.primitive_mode = 3;
      args.host_vertex_count = context.index_count >= 3 ? (context.index_count - 2) * 3 : 0;
      break;
    case xenos::PrimitiveType::kQuadList:
      if (context.indexed) {
        return refuse("indexed quad list");
      }
      root.primitive_mode = 1;
      args.host_vertex_count = (context.index_count / 4) * 6;
      break;
    case xenos::PrimitiveType::kPointList: {
      // Point sprites: six host vertices a point; sizes as the SDK computes them for its system constants.
      root.primitive_mode = 2;
      args.host_vertex_count = context.index_count * 6;
      const auto& vp_extent = context.viewport_info->xy_extent;
      const float to_ndc_x = float(context.draw_resolution_scale_x) / float(std::max(vp_extent[0], 1u));
      const float to_ndc_y = float(context.draw_resolution_scale_y) / float(std::max(vp_extent[1], 1u));
      const auto point_size = register_file_->Get<reg::PA_SU_POINT_SIZE>();
      const float diameter_x = float(point_size.width) * (2.0f / 16.0f);
      const float diameter_y = float(point_size.height) * (2.0f / 16.0f);
      std::memcpy(&root.pass_params[0], &to_ndc_x, 4);
      std::memcpy(&root.pass_params[1], &to_ndc_y, 4);
      std::memcpy(&root.pass_params[2], &diameter_x, 4);
      std::memcpy(&root.pass_params[3], &diameter_y, 4);
      root.alpha_test[3] = (*register_file_)[XE_GPU_REG_PA_SU_POINT_MINMAX];
      break;
    }
    default:
      return refuse("unsupported primitive type");
  }
  const auto& vp = *context.viewport_info;
  for (int i = 0; i < 3; ++i) {
    root.ndc_scale[i] = vp.ndc_scale[i];
    root.ndc_offset[i] = vp.ndc_offset[i];
  }
  const auto& regs = *register_file_;
  state = NativeGeometryPass::GuestState{};
  state.blendcontrol[0] = regs[XE_GPU_REG_RB_BLENDCONTROL0];
  state.blendcontrol[1] = regs[XE_GPU_REG_RB_BLENDCONTROL1];
  state.blendcontrol[2] = regs[XE_GPU_REG_RB_BLENDCONTROL2];
  state.blendcontrol[3] = regs[XE_GPU_REG_RB_BLENDCONTROL3];
  state.su_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  state.color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  // Depth/stencil and polygon offset exactly as the SDK's pipeline cache derives them for its own
  // pipelines (a native shadow caster without the guest's depth bias is acne everywhere).
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  if (!REXCVAR_GET(nb_native_stencil)) {
    depth_control.stencil_enable = 0;
  }
  state.depthcontrol = depth_control.value;
  const auto su_mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const bool stencil_backface = depth_control.stencil_enable && depth_control.backface_enable;
  state.stencil_ref_mask = regs[(stencil_backface && su_mode.cull_front) ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                                                          : XE_GPU_REG_RB_STENCILREFMASK];
  float polygon_offset_scale = 0.0f, polygon_offset = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(regs, true, polygon_offset_scale, polygon_offset);
  state.depth_bias =
      draw_util::GetD3D10IntegerPolygonOffset(regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
  state.depth_bias_slope = polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit *
                           float(std::max(context.draw_resolution_scale_x, context.draw_resolution_scale_y));
  state.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  state.alpha_to_mask = regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable != 0;
  // Alpha test (prelude.hlsl AlphaTest): RB_COLORCONTROL low nibble is func (3 bits) + enable.
  root.alpha_test[0] = REXCVAR_GET(nb_native_alpha_test) ? (regs[XE_GPU_REG_RB_COLORCONTROL] & 0xFu) : 0u;
  root.alpha_test[1] = regs[XE_GPU_REG_RB_ALPHA_REF];
  // Vertex kill mode for shaders writing oPts.z: 1 = any killed vertex kills the primitive.
  root.alpha_test[2] =
      REXCVAR_GET(nb_native_vertex_kill) ? regs.Get<reg::PA_CL_CLIP_CNTL>().vtx_kill_or : 0u;
  // The interpolator register the rasterizer's generated parameters replace, if any (prelude
  // ParamGenOverride); the same rule as Shader::GetInterpolatorInputMask.
  root.param_gen = regs.Get<reg::SQ_PROGRAM_CNTL>().param_gen
                       ? (0x100u | (regs.Get<reg::SQ_CONTEXT_MISC>().param_gen_pos & 0xFFu))
                       : 0u;
  // The draw resolution scale rides in bits 16..19 and 20..23 as scale - 1. Those bits are zero at 1x, so
  // unscaled play sees the same constant as before. The prelude's ParamGenOverride and TexSize use it to
  // undo the scale the way the SDK's translator does.
  root.param_gen |= ((std::min<uint32_t>(context.draw_resolution_scale_x, 16u) - 1u) << 16) |
                    ((std::min<uint32_t>(context.draw_resolution_scale_y, 16u) - 1u) << 20);
  // Bit 24 mirrors a non-default draw_resolution_scaled_texture_offsets = false: fetch offsets then count
  // guest texels on scaled textures (prelude TexOffsetSize). It is only set at a scale, so 1x keeps its
  // constant.
  if ((context.draw_resolution_scale_x > 1 || context.draw_resolution_scale_y > 1) &&
      !REXCVAR_GET(draw_resolution_scaled_texture_offsets)) {
    root.param_gen |= 1u << 24;
  }
  args.vs_constants = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  args.ps_constants = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X]);
  args.bool_constants = &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
  return true;
}

bool NbCommandProcessor::TrySpritePass(const NativeDrawContext& context) {
  // Game-specific transcriptions are excluded from the source release.
  // The generic native path or SDK fallback handles this draw.
  return false;
}

bool NbCommandProcessor::TryDofFogPass(const NativeDrawContext& context) {
  // Game-specific transcriptions are excluded from the source release.
  // The generic native path or SDK fallback handles this draw.
  return false;
}

namespace {

// nb_native_shader_dir, or native_shaders next to the executable.
std::string ResolveShaderDirectory(const std::string& configured) {
  if (!configured.empty()) {
    return configured;
  }
  wchar_t exe_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return "native_shaders";
  }
  return (std::filesystem::path(exe_path).parent_path() / "native_shaders").string();
}

}  // namespace

bool NbCommandProcessor::TryGenericPass(const NativeDrawContext& context) {
  const bool timed = !REXCVAR_GET(nb_native_minimal_command_diagnostics);
  const auto t0 = NativeTimingStart(timed);
  const bool result = TryGenericPassImpl(context);
  generic_pass_ns_ += NativeTimingElapsed(timed, t0);
  return result;
}

bool NbCommandProcessor::TryGenericPassImpl(const NativeDrawContext& context) {
  using rex::graphics::xenos::ClampMode;
  if (!bindless_resources_used()) {
    return false;
  }
  if (!native_asset_cache_attempted_) {
    native_asset_cache_attempted_ = true;
    const auto configured = REXCVAR_GET(nb_native_asset_pack);
    if (!configured.empty()) {
      std::string error;
      if (native_asset_cache_.Initialize(GetD3D12Provider().GetDevice(), shared_memory(), configured, &error)) {
        const auto& assets = native_asset_cache_.stats();
        REXLOG_INFO("rexgpu-nb: native asset pack ready: {} buffers, {} bytes", assets.assets, assets.arena_bytes);
      } else {
        REXLOG_WARN("rexgpu-nb: native asset pack disabled: {}; shared-memory buffers retained", error);
      }
    }
  }
  // Snapshot eligibility once for both library identity and DrawArgs. The pass
  // additionally refuses any non-null asset pointer with specialized bytecode.
  NativeAssetCache* draw_asset_cache =
      native_asset_cache_.initialized() && asset_cache_mode_ != 0 ? &native_asset_cache_ : nullptr;
  const bool direct_guest_reads = REXCVAR_GET(nb_native_direct_guest_reads) && !draw_asset_cache;
  const bool effective_samplers = REXCVAR_GET(nb_native_effective_samplers);
  const uint32_t library_index = (effective_samplers ? 1u : 0u) | (direct_guest_reads ? 2u : 0u);
  auto& library = shader_libraries_[library_index];
  if (!shader_libraries_loaded_[library_index]) {
    shader_libraries_loaded_[library_index] = true;
    library.Load(ResolveShaderDirectory(REXCVAR_GET(nb_native_shader_dir)),
                 REXCVAR_GET(nb_native_shader_filter), REXCVAR_GET(nb_native_shader_exclude),
                 effective_samplers, direct_guest_reads);
  }
  NativeShaderLibrary::Pair* pair = nullptr;
  const uint64_t ps_hash = context.pixel_shader ? context.pixel_shader->ucode_data_hash() : 0;
  switch (library.FindPair(context.vertex_shader->ucode_data_hash(), ps_hash,
                                   GetD3D12Provider().GetDevice(), &pair)) {
    case NativeShaderLibrary::Lookup::kReady:
      break;
    case NativeShaderLibrary::Lookup::kUnknownShader:
      generic_refusals_[kRefusedNotInLibrary]++;
      if (REXCVAR_GET(nb_log_frame) > 0 && uint64_t(REXCVAR_GET(nb_log_frame)) == stats_.frames + 1) {
        REXLOG_INFO("rexgpu-nb: native library missing at frame {}: vs {:016X} ps {:016X}, prim {} x{}",
                    stats_.frames + 1, context.vertex_shader->ucode_data_hash(),
                    context.pixel_shader ? context.pixel_shader->ucode_data_hash() : 0,
                    static_cast<uint32_t>(context.guest_primitive_type), context.index_count);
      }
      return false;
    case NativeShaderLibrary::Lookup::kDisabled:
      generic_refusals_[kRefusedDisabled]++;
      return false;
    case NativeShaderLibrary::Lookup::kNotReady:
      generic_refusals_[kRefusedNotReady]++;
      return false;
    case NativeShaderLibrary::Lookup::kUnusable:
      generic_refusals_[kRefusedUnusable]++;
      return false;
  }

  NativeGeometryPass::RootConstants root;
  NativeGeometryPass::GuestState state;
  NativeGeometryPass::DrawArgs args;
  if (!PrepareGeometry(context, root, state, args, &pair->vs->streams)) {
    pair->skips++;
    generic_refusals_[kRefusedPrepare]++;
    return false;
  }
  args.asset_cache = draw_asset_cache;
  args.legacy_asset_cache = asset_cache_mode_ == 1;
  args.constant_pool = ConstantUploadPool();

  // Textures: the pixel shader's fetch constants occupy root-constant slots 0..15, the vertex shader's
  // 16..19, in the order the translated shader assigned them. Each slot also carries the sampler the
  // fetch constant asks for (bit 1 point) and its component swizzle.
  // Ask the texture cache to load this draw's textures before resolving any of them. The SDK does this
  // at command_processor.cpp:2404-2410 and the cache's own header is explicit that it matters:
  // "ActiveTexture" means as of the latest RequestTextures call. Without it a native draw binds whatever
  // some earlier emulated draw happened to request, so a texture no emulated draw ever used is never
  // uploaded and samples as zero everywhere - a real descriptor pointing at an empty texture, which is
  // indistinguishable from a correctly bound black one until you read the texture back.
  //
  // That is what made whole surfaces black, and why it got worse as coverage rose: at 54% native there
  // were enough emulated draws to load most textures incidentally, and at 91% there were not.
  RequestDrawTextures(context);
  bool textures_bound = true;
  bool uses_empty_texture = false;
  bool uses_stacked_texture = false;
  bool uses_cube_texture = false;
  GenericRefusal binding_refusal = kRefusedTexture;
  // Every draw resolves each of its fetch constants through the texture cache, which for a town frame
  // is tens of thousands of lookups. Timed separately so it can be compared against residency.
  auto bind_textures = [&](const rex::graphics::d3d12::D3D12Shader* shader,
                           const NativeShaderLibrary::StageShader& stage, uint32_t slot_base) {
    const bool detailed = !REXCVAR_GET(nb_native_minimal_diagnostics);
    const auto textures_start = NativeTimingStart(detailed);
    TextureBindingCache::Plan* binding_plan = nullptr;
    if (shader && !stage.texture_fetch_constants.empty() &&
        REXCVAR_GET(nb_native_texture_binding_cache)) {
      static_assert(TextureBindingCache::kMaxTextures ==
                    rex::graphics::d3d12::D3D12Shader::kMaxTextureBindings);
      static_assert(TextureBindingCache::kMaxSamplers ==
                    rex::graphics::d3d12::D3D12Shader::kMaxSamplerBindings);
      // The native seam follows successful synchronous shader translation.
      // Bindings are immutable from then on. PipelineCache resets this lifetime
      // counter before either Shader deletion path, even when LoadShader memo
      // lookups are disabled, preventing stale plans after pointer reuse.
      binding_plan = texture_binding_cache_.Prepare(
          reinterpret_cast<uintptr_t>(shader), native_shader_load_memo_stats().resets,
          shader->GetTextureBindingsAfterTranslation(), shader->GetSamplerBindingsAfterTranslation());
    }
    for (size_t i = 0; i < stage.texture_fetch_constants.size() && textures_bound; ++i) {
      const uint32_t slot = slot_base + static_cast<uint32_t>(i);
      const uint32_t fetch_constant = stage.texture_fetch_constants[i];
      const bool stacked_fetch = stage.texture_dimensions[i] == 3;
      const bool cube_fetch = stage.texture_dimensions[i] == 4;
      const auto fetch = register_file_->GetTextureFetch(fetch_constant);
      const bool empty_fetch = ((fetch.base_address | fetch.mip_address) & 0x1FFFFu) == 0;
      if (cube_fetch && !empty_fetch && fetch.dimension != rex::graphics::xenos::DataDimension::kCube) {
        textures_bound = false;
        return;
      }
      if (stacked_fetch &&
          (fetch.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked ||
           fetch.vol_mag_filter != fetch.vol_min_filter)) {
        // tfetch3D also addresses true volumes and can select layer filters using Z gradients.
        // This native subset handles 2D arrays with one uniform layer filter only.
        if (pair->skips++ < 2) {
          REXLOG_WARN("rexgpu-nb: native {}: 3D fetch tf{} kept emulated (dimension {}, volume filters {}/{})",
                      pair->stem, fetch_constant, uint32_t(fetch.dimension),
                      uint32_t(fetch.vol_mag_filter), uint32_t(fetch.vol_min_filter));
        }
        textures_bound = false;
        return;
      }
      const uint32_t index = shader ? GuestTextureIndex(*shader, fetch_constant, true,
                                                       stage.texture_dimensions[i], binding_plan)
                                    : UINT32_MAX;
      if (index == UINT32_MAX) {
        if (pair->skips++ < 2) {
          REXLOG_WARN("rexgpu-nb: native {}: {} fetch constant {} not bound; emulated draw kept", pair->stem,
                      slot_base ? "vertex" : "pixel", fetch_constant);
        }
        textures_bound = false;
        return;
      }
      root.texture_index[slot] = index;
      uses_stacked_texture |= stacked_fetch;
      uses_cube_texture |= cube_fetch;
      if (index == 0) {
        uses_empty_texture = true;
        continue;  // Tex2D returns zero without touching a sampler for this slot.
      }
      // The SDK already canonicalizes instruction min/mag/mip/aniso overrides, including explicit
      // LOD's anisotropy restriction. A slot can use that sampler directly when every instruction
      // for the fetch constant resolves to the same descriptor. Different raw overrides may still
      // resolve identically for this draw; conflicting effective descriptors need separate slots.
      rex::graphics::d3d12::D3D12TextureCache::SamplerParameters sampler;
      bool have_sampler = false;
      bool sampler_conflict = false;
      if (binding_plan) {
        const std::array<uint32_t, 6> fetch_words = {
            fetch.dword_0, fetch.dword_1, fetch.dword_2,
            fetch.dword_3, fetch.dword_4, fetch.dword_5};
        const auto consensus = texture_binding_cache_.ResolveSampler(
            *binding_plan, fetch_constant, fetch_words, REXCVAR_GET(anisotropic_override),
            [&](const auto& candidate) { return texture_cache().GetSamplerParameters(candidate).value; });
        sampler.value = consensus.value;
        have_sampler = consensus.have_sampler;
        sampler_conflict = consensus.conflict;
      } else {
        for (const auto& candidate : shader->GetSamplerBindingsAfterTranslation()) {
          if (candidate.fetch_constant != fetch_constant) continue;
          const auto resolved = texture_cache().GetSamplerParameters(candidate);
          if (have_sampler && resolved != sampler) {
            sampler_conflict = true;
            break;
          }
          sampler = resolved;
          have_sampler = true;
        }
      }
      if (!have_sampler || sampler_conflict) {
        if (pair->skips++ < 2) {
          REXLOG_WARN("rexgpu-nb: native {}: tf{} has {} SDK sampler bindings; emulated draw kept",
                      pair->stem, fetch_constant, sampler_conflict ? "conflicting" : "no");
        }
        textures_bound = false;
        binding_refusal = kRefusedSampler;
        return;
      }
      const uint32_t sampler_slot = args.sampler_slot_count++;
      args.sampler_slots[sampler_slot] = slot;
      // Record resolves this effective sampler once and duplicates its index for the
      // legacy point/linear selectors, preserving old generated bodies without a second lookup.
      args.sampler_parameters[sampler_slot] = sampler;
      // Leave sampler_sel clear: computed-LOD fetches retain gradients even for base-map filtering.
      // The SDK descriptor clamps MinLOD/MaxLOD while preserving minification vs. magnification.
      // The fetch constant also carries a signed power-of-two bias the hardware applies to every
      // component of every sample from this texture (xenos.h: exp_adjust, word 3 bits 13..18), and the
      // SDK's own translator multiplies by 2^bias at the tail of each texture fetch. Ours never did: for
      // a vertex fetch the bias is an instruction operand and shows up in the ucode disassembly, so
      // ucode2hlsl.py handles it, but for a texture fetch it lives here in the fetch constant and never
      // reaches the translator at all. It rides in the spare top bits of the swizzle word rather than
      // costing another root constant - the swizzle itself is 12 bits, and the texture cache bakes it
      // into the descriptor anyway.
      const uint32_t exp_bits =
          REXCVAR_GET(nb_native_exp_bias) ? (static_cast<uint32_t>(fetch.exp_adjust) & 0x3Fu) : 0u;
      // The SRV already applies swizzling. For 3D instructions, use its otherwise-unused low bits
      // for raw stack depth and the common volume filter. Keep the same exponent/LOD fields for both.
      const uint32_t low_bits = stacked_fetch
          ? uint32_t(fetch.size_2d.stack_depth) | (uint32_t(fetch.vol_mag_filter) << 6)
          : uint32_t(fetch.swizzle);
      const uint32_t lod_bits = static_cast<uint32_t>(fetch.lod_bias) & 0x3FFu;
      // Bit 28 marks a resolution-scaled resolve, which holds scale x the guest's texels. With it the
      // prelude's TexSize reports the guest size for unnormalized coordinates, which is what the SDK's
      // translator effectively does.
      const uint32_t scaled_bit =
          texture_cache().IsActiveTextureResolutionScaled(fetch_constant) ? (1u << 28) : 0u;
      args.texture_swizzle[slot] = low_bits | (exp_bits << 12) | (lod_bits << 18) | scaled_bit;
    }
    NativeGeometryPass::timings().textures_ns += NativeTimingElapsed(detailed, textures_start);
  };
  if (pair->ps) {
    bind_textures(context.pixel_shader, *pair->ps, 0);
  }
  bind_textures(context.vertex_shader, *pair->vs,
                NativeGeometryPass::kVertexTextureSlotBase);
  if (!textures_bound) {
    generic_refusals_[binding_refusal]++;
    return false;
  }

  // One-shot diagnostic for a pair named by nb_native_trace_pair: the state a translated shader depends
  // on but cannot see (param_gen, and each texture slot's addressing).
  if (!REXCVAR_GET(nb_native_trace_pair).empty() &&
      pair->stem.find(REXCVAR_GET(nb_native_trace_pair)) != std::string::npos &&
      (pair->draws < 2 || (REXCVAR_GET(nb_trace_every) > 0 &&
                           (stats_.frames + 1) % uint64_t(REXCVAR_GET(nb_trace_every)) == 0))) {
    const auto program_cntl = register_file_->Get<rex::graphics::reg::SQ_PROGRAM_CNTL>();
    const auto context_misc = register_file_->Get<rex::graphics::reg::SQ_CONTEXT_MISC>();
    REXLOG_INFO("rexgpu-nb: trace {} frame {}: param_gen {} pos {}, vs_export_count {}, ps_num_reg {}, prim {} x{}",
                pair->stem, stats_.frames + 1, uint32_t(program_cntl.param_gen), uint32_t(context_misc.param_gen_pos),
                uint32_t(program_cntl.vs_export_count), uint32_t(program_cntl.ps_num_reg),
                static_cast<uint32_t>(context.guest_primitive_type), context.index_count);
    if (pair->ps) {
      for (size_t i = 0; i < pair->ps->texture_fetch_constants.size(); ++i) {
        const auto fetch = register_file_->GetTextureFetch(pair->ps->texture_fetch_constants[i]);
        REXLOG_INFO("rexgpu-nb: trace {}:   ps tf{} clamp {}/{} filter {}/{} swizzle 0x{:03X} size {}x{} "
                    "base 0x{:X} fmt {} type {} signs 0x{:02X} endian {}",
                    pair->stem, pair->ps->texture_fetch_constants[i], uint32_t(fetch.clamp_x),
                    uint32_t(fetch.clamp_y), uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter),
                    uint32_t(fetch.swizzle), uint32_t(fetch.size_2d.width) + 1,
                    uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.base_address),
                    uint32_t(fetch.format), uint32_t(fetch.type),
                    texture_cache().GetActiveTextureSwizzledSigns(pair->ps->texture_fetch_constants[i]),
                    uint32_t(fetch.endianness));
        uint32_t base_page, mip_page, mip_min, mip_max;
        rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
            fetch, nullptr, nullptr, nullptr, &base_page, &mip_page, &mip_min, &mip_max);
        REXLOG_INFO("rexgpu-nb: trace {}:   ps tf{} mip_filter {} mip_min {} mip_max {} lod_bias {} "
                    "normalized_mips {}..{} pages 0x{:X}/0x{:X}",
                    pair->stem, pair->ps->texture_fetch_constants[i],
                    uint32_t(fetch.mip_filter), uint32_t(fetch.mip_min_level),
                    uint32_t(fetch.mip_max_level), int32_t(fetch.lod_bias),
                    mip_min, mip_max, base_page, mip_page);
      }
      // The bool constants the shader branches on, and the pixel constants the guest actually gives it -
      // read through the same runs the upload uses, so a packing mistake shows up here too.
      const uint32_t* bools = &register_file_->values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
      REXLOG_INFO("rexgpu-nb: trace {}:   bools {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  pair->stem, bools[0], bools[1], bools[2], bools[3], bools[4], bools[5], bools[6],
                  bools[7]);
      const float* ps_c =
          reinterpret_cast<const float*>(&register_file_->values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_256_X]);
      uint32_t packed = 0;
      // A shader with only a handful of constants gets all of them; the curated list below is for the
      // big material shaders, where dumping 80 float4s per draw buries everything else.
      const bool dump_all = pair->ps->packed_constants <= 16;
      for (const auto& run : pair->ps->constant_runs) {
        for (uint32_t k = 0; k < run.count; ++k, ++packed) {
          const float* v = ps_c + (size_t(run.first) + k) * 4;
          // c8/c10/c12 are the shared scalars, 21/35/37/45 the tone map and fade, and 58..77 the six
          // lights: position with 1/range in w, then colour.
          // 22 and 41/42 are the sun (colour, direction, eye position) and 20 the PCF tap weight, which
          // together decide whether the directional term survives at all.
          if (dump_all || packed == 0 || packed == 8 || packed == 10 || packed == 12 || packed == 20 ||
              packed == 21 || packed == 22 || packed == 35 || packed == 37 || packed == 41 ||
              packed == 42 || packed == 45 || (packed >= 58 && packed <= 65)) {
            REXLOG_INFO("rexgpu-nb: trace {}:   ps_c[{}] = guest c{} = {} {} {} {}", pair->stem, packed,
                        run.first + k, v[0], v[1], v[2], v[3]);
          }
        }
      }
    }
    // The vertex side too: a texture-coordinate matrix lives in vertex constants, so a surface can go
    // black from a zeroed UV row without anything on the pixel side looking wrong.
    {
      const float* vs_c =
          reinterpret_cast<const float*>(&register_file_->values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X]);
      uint32_t vpacked = 0;
      const bool vdump_all = pair->vs->packed_constants <= 20;
      for (const auto& run : pair->vs->constant_runs) {
        for (uint32_t k = 0; k < run.count; ++k, ++vpacked) {
          const float* v = vs_c + (size_t(run.first) + k) * 4;
          if (vdump_all || vpacked <= 8) {
            REXLOG_INFO("rexgpu-nb: trace {}:   vs_c[{}] = guest c{} = {} {} {} {}", pair->stem, vpacked,
                        run.first + k, v[0], v[1], v[2], v[3]);
          }
        }
      }
    }
  }
  args.vs_constant_runs = pair->vs->constant_runs.data();
  args.vs_constant_run_count = pair->vs->constant_runs.size();
  args.vs_constant_count = pair->vs->packed_constants;
  args.vs_has_packed_layout = pair->vs->has_packed_layout;
  if (pair->ps) {
    args.ps_constant_runs = pair->ps->constant_runs.data();
    args.ps_constant_run_count = pair->ps->constant_runs.size();
    args.ps_constant_count = pair->ps->packed_constants;
    args.ps_has_packed_layout = pair->ps->has_packed_layout;
  } else {
    args.ps_constants = nullptr;
    args.ps_constant_count = 1;
  }
  if (!pair->pass.Record(*this, context, state, root, args)) {
    pair->skips++;
    generic_refusals_[kRefusedRecord]++;
    return false;
  }
  pair->draws++;
  if (uses_empty_texture) ++native_empty_texture_draws_;
  if (root.primitive_mode == 3) ++native_triangle_strip_draws_;
  if (uses_stacked_texture) ++native_stacked_texture_draws_;
  if (uses_cube_texture) ++native_cube_texture_draws_;
  if (pair->vs->streams.size() > 2) ++native_extra_stream_draws_;
  LogNativeDraw(pair->stem.c_str(), context);
  return true;
}

}  // namespace nb::gpu
