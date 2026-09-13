# Vendored RexGlue SDK graphics sources

This record accompanies the committed development snapshot used for the public
preview. Historical experiment names and document paths below refer to private
development notes; those notes and benchmark data are not part of this release.
The code changes and upstream revision are included for source comparison.

Upstream: rexglue-sdk tag `v0.10.0`, commit `f5337cdc947ff6d4c4196737e2c807a48f2a1fc2` (BSD-3-Clause,
Tom Clay and Xenia contributors; every file keeps its original license header). The plugin compiles
these copies instead of the same-named files under `${REXSDK_DIR}/src/graphics`, and
`src/gpu/CMakeLists.txt` puts `vendored/include` BEFORE the SDK include directory so the header here
wins. The stock `rexgpu-xenos` plugin never sees any of this, which is what keeps the A/B honest.

Rule: vendor a file only when it needs a change, keep the diff against upstream small and listed
here, and never vendor the headers `rexruntime.dll` also compiles (`rex/graphics/xenos.h`,
`register_file.h`, `pipeline/texture/info.h`, `video_mode_util.h`): those must stay identical in both
DLLs.

| File | Why it is here | Diffs applied |
|---|---|---|
| `include/rex/graphics/phase3_counters.h` | new: the named per-frame counter set (transfers, tiles, resolve dumps, tile passes, pipeline skips, waits) with a JSON-lines dump | swarm-3 T 0002/0004 as fixed in `C:/rex/dl/swarm23/check_T/fixed_series.diff` |
| `src/graphics/d3d12/render_target_cache.cpp` | step 1: `render_target_path_d3d12` defaults to `rtv` (ROV stays compiled); resolve-dump counter | T 0001, 0004 (fixed) |
| `src/graphics/d3d12/command_processor.cpp` | step 6: no record-time draw drop for pending pipelines, record-time skip counter, creation-wait timer, per-frame counter dump at the closing swap | T 0002, 0004 (fixed) |
| `include/rex/graphics/d3d12/command_processor.h`, `src/graphics/d3d12/command_processor.cpp` | native draw seam and accessors; batch native sampler descriptor lookup in the SDK heap | Native sampler batches retry every index on overflow; native and emulated requests share heap rotation, cache invalidation and submission-safe retirement; protected GPU-idle accessor lets extensions retire resources before SharedMemory teardown |
| `include/rex/graphics/d3d12/native_static_bindings.h` | original native root-binding state | Cache texture/sampler tables and guest/asset SRVs; optional independent pixel-CBV cache publishes only after an actual root write. Invalidate on ordinary deferred-list access, external roots, emulated bindings, heap rotation and new submissions; diagnostic switches retain unconditional writes |
| `include/rex/graphics/d3d12/pipeline_cache.h`, `src/graphics/d3d12/pipeline_cache.cpp`, command processor files above | native draws avoid discarded emulated pipeline work | Extract the existing synchronous shader translation and storage-queue preparation unchanged. Bindless, host-target, rasterizing, non-tessellated native draws retain translation/root validation, then configure an emulated PSO only on native refusal; other paths keep the original order. Independent switches control deferred binds and PSO preparation. First emulated fallback can create a previously unused PSO and still waits before execution |
| `src/graphics/d3d12/deferred_command_list.cpp` | step 6: Execute-time null-handle counter | T 0002 |
| `include/rex/graphics/d3d12/deferred_command_list.h`, source above | Optional reuse of constructed command storage | Retain vector high-water storage with a separate logical extent; replay only that extent. Every writer retains its owned argument snapshot, command order and live pipeline-handle resolution. Reset latches the diagnostic switch; copy/move and short/empty lists preserve logical bounds |
| `include/rex/graphics/d3d12/deferred_command_list.h`, source above and command processor | Optional compound native constant binding | One owned packet retains three full GPU addresses and the optional pixel-write flag; replay issues the same root0/root1/root4 CBV setter calls in order. Pixel memo publication follows successful append; draw commands and the root-constant variant are unchanged |
| `include/rex/graphics/d3d12/native_submission_stats.h`, command processor source | Optional host submission timing and barrier inventory | Time allocator/list reset, deferred replay, close and queue execution only when requested; count unchanged barrier types and shared-memory COPY transitions. Scoped count-only upload-phase attribution excludes nested/ambiguous native attempts and does not reorder commands. No per-draw clocks or observation-packet initialization when disabled |
| `src/graphics/d3d12/pipeline_cache.cpp` | step 6: shader translation stays synchronous on the CP thread while PSO creation is asynchronous, so a pending pipeline carries the real root signature | `fixed_step6_sync_translation.diff` (T review section 3) |
| `src/graphics/pipeline/render_target/cache.cpp` | step 2 preparation: JSON log of copying EDRAM ownership transfers inside a tile window (`nb_edram_transfer_log*`), transfer counters | T 0003 (fixed) |
| `src/graphics/command_processor.cpp` | tile-pass counter at the SET_BIN_SELECT writes | T 0004 |
| `src/graphics/command_processor.cpp` | sequential register write batching | Type-0 sequential writes use the existing ring/range writer (ALU/fetch/bool dirty unions, scalar fallback for all other ranges); repeated writes to one register keep the scalar path. Unknown-register metadata is only looked up when debug logging can emit it. `nb_native_register_fastpath=false` restores the diagnostic baseline |
| `src/graphics/d3d12/render_target_cache.cpp` | retain native roots across empty transfer checks | Under `nb_native_static_bindings`, no-clear calls with no transfers for any live destination perform the same temporary-descriptor cleanup and return before requesting raw command-list access; all actual transfer/clear work stays on the original path |
| `include/rex/graphics/shared_memory.h`, `src/graphics/shared_memory.cpp` | CPU-authoritative snapshots for opt-in local asset caches | `CopyCpuAuthoritativeRange` requires requested/valid pages and rejects GPU-written pages under the global critical region; `InvalidateAllPages` notifies full-range watches after clearing flags, including explicit same-frame invalidation |
| `src/graphics/shared_memory.cpp` | bounded native range batching | Original `native/native_range_batch.h` uses stack storage for at most 32 inputs and unions duplicate upload pages at disjoint byte-range boundaries. An optional single-range form owns one validated pair and avoids constructing the array; both feed the same allocation/validity/upload lambda. Bounds checks, sparse allocation, locked valid-page checks and upload sequencing remain; diagnostic switch preserves legacy allocation/append behavior |
| `src/graphics/shared_memory.cpp` | Optional cap on excess CPU invalidation | Original `native/native_invalidation_range.h` intersects the existing GPU-authority-guarded excess interval with 64KiB blocks containing the actual write. Actual pages, exact callbacks, frame clearing and GPU paths remain unchanged. Validity, watches and returned physical-unwatch endpoints use the same chosen range; omitted neighbors remain runtime-protected. Separate diagnostics use the existing global lock and return owned snapshots |
| `include/rex/graphics/d3d12/shared_memory.h`, `src/graphics/d3d12/shared_memory.cpp` | opt-in exact CPU upload byte reuse | `nb_native_upload_shadow=false` retains the original path. Enabled uploads still allocate first, mark/protect the exact returned range, and take bounded CPU-authoritative snapshots; exact unchanged pages skip WC writes and GPU copy commands. A bounded original page shadow publishes only the bytes actually queued, guarded against GPU/full-reset invalidation races. CPU page-state clearing and A/C/E alias protection remain unchanged; resource teardown unregisters the watch before releasing state |
| `src/graphics/d3d12/texture_cache.cpp` | Optional verified offline base-level BC upload data | Exact CPU-authoritative matches copy preconverted blocks through the SDK upload pool into its existing texture resource; misses retain SDK conversion, barriers, descriptors and watches |
| `src/graphics/pipeline/texture/cache.cpp` | Optional load-first invalidation poll | `nb_native_texture_outdated_load_first=false` retains the acquire exchange. Enabled calls acquire-load the existing flag and exchange only if true; release-producing watches and all binding/load work remain unchanged. CP-owned counters describe only enabled false loads, exchanges and true consumes. Original helper: `native/native_texture_outdated_poll.h`; no class/header ABI change |
| `include/rex/graphics/d3d12/texture_cache.h`, `src/graphics/d3d12/texture_cache.cpp` | Optional last SRV-map lookup per live texture | Exact full descriptor key and successful index only; live texture selection, descriptor creation and heap/resource lifetime remain unchanged |
| `include/rex/graphics/pipeline/shader/shader.h`, `src/graphics/pipeline/shader/shader.cpp` | Optional last translation lookup per owning shader | Exact 64-bit modification key; preserves creation and `is_new`; invalidates before deletion even while disabled; owner cannot be copied or moved; validity is always checked by original callers |
| `include/rex/graphics/d3d12/shader_load_memo.h`, pipeline cache files above | Optional bounded exact shader-load memo | Verify current guest bytes against an independent snapshot on every hit; publish only immutable successfully loaded shader bytes; reset before owners are deleted |
| `include/rex/graphics/native_command_wait_stats.h`, `src/graphics/command_processor.cpp` | Optional wait diagnostics and precise host wait | Distinguish empty-ring idle, whole blocked-packet elapsed and nested requested/actual sleep. Optional thread-local high-resolution timer preserves whole-ms requests with full-duration Sleep fallback. A separate default-off one-shot 250us first-retry diagnostic changes host polling cadence, keeps fresh predicate/endian/coherency checks, and falls back to the full original wait; Xenos interval fidelity is not established |
| `src/graphics/command_processor.cpp` | Optional bounded WAIT_REG_MEM re-read and blocked-wait target diagnostic | `nb_native_wait_spin_us=0` (default) keeps the original sleeps. When positive, a blocked packet with vsync on yields and re-reads the unchanged predicate, endian and coherency handling for at most that many microseconds before the original sleeps resume. A condition that becomes true inside the budget is seen within one yield; afterwards completion keeps the original bound of one sleep period after the condition holds, though not the original read phase. The Xenos CP re-polls in microseconds rather than sleeping a millisecond between reads. `nb_native_wait_target_diagnostics` records the polled register or memory word, compare function and mask of blocked packets, their blocked time, and whether and how soon after the vblank counter advanced they matched; it changes no behaviour. Original helper `native/native_wait_poll.h`; no class/header ABI change |
| `include/rex/graphics/d3d12/command_processor.h`, `src/graphics/d3d12/command_processor.cpp` | display-aspect seam for nb_ultrawide (docs/ultrawide.md) | `OverrideDisplayAspect(display_width, display_height)` is a protected virtual that does nothing by default. IssueSwap calls it after VdQueryVideoMode and before RefreshGuestOutput. NbCommandProcessor passes nb.exe's ultrawide aspect (for example 32:9), so the presenter fits the anamorphic guest output to the full screen. The guest-visible video mode is untouched |
| `src/graphics/pipeline/texture/cache.cpp` | ultrawide default render scale | `GetConfigDrawResolutionScale` uses nb.exe's nb_ultrawide_scale (through `native/nb_ultrawide_bridge.h`), but only when none of resolution_scale, draw_resolution_scale_x or draw_resolution_scale_y is set. No cvar is written. Otherwise the upstream logic is unchanged |
| `src/graphics/d3d12/render_target_cache.cpp` | diagnostic EDRAM-transfer frame windows (docs/ultrawide.md, Performance) | `nb_diag_skip_transfer_stencil_frames` and `nb_diag_skip_transfers_frames` hold "first-last" in the frame trace's numbering and are empty by default. Inside the window, `PerformTransfersAndResolveClears` skips either the stencil clear and eight stencil-bit passes of depth transfers, or every transfer draw, so one run measures their GPU cost against the frames around it. The output is wrong inside a window, so these are never for play. Since UW15 every window cvar of that function is evaluated once per frame, cached on the frame number: the function runs for every draw, and parsing a multi-range list per call cost the command thread milliseconds per frame, more outside a window than inside one. With every window empty the function is unchanged apart from those per-frame reads |
| `include/rex/graphics/util/draw_extent_estimator.h`, `src/graphics/util/draw_extent_estimator.cpp` | guards on the vertex extent estimate, which nb.toml turns on (`execute_unclipped_draw_vs_on_cpu`; docs/ultrawide.md, "The transfer fix") | Functions only, no data members, because the SDK's RenderTargetCache holds the estimator by value. `EstimateMaxY` and `EstimateVertexMaxY` take the render scale (default 1). Each guard can only enlarge a claim. At a render scale above 1 the bottom edge rounds up (the MSAA rule) instead of by the 1x .5 rule. The estimate falls back to the scissor for indices or vertex buffers the GPU wrote (`SharedMemory::IsRangeGpuWritten`), lines, rectangle lists with a primitive restart, and positions with w <= 0 or a non-finite result. A rectangle list also counts the highest of the three candidates for its implied fourth corner. The first fallback of each kind is logged with its vertex shader. At 1x, when no guard applies, the result is the SDK's |
| `include/rex/graphics/shared_memory.h`, `src/graphics/shared_memory.cpp` | GPU-written range query for the extent estimate | `IsRangeGpuWritten(start, length)` reads `system_page_flags_valid_and_gpu_written_` under the global lock. A range outside the buffer counts as written |
| `src/graphics/d3d12/render_target_cache.cpp` | registers that query | `Initialize` registers it through `SetDrawExtentGpuWrittenQuery` with the command processor's shared memory, and `Shutdown` withdraws it |
| `src/graphics/pipeline/render_target/cache.cpp` | render scale for the extent estimate | `Update` passes `draw_resolution_scale_y()` to `EstimateMaxY` |
| `src/graphics/d3d12/render_target_cache.cpp` | optional per-pixel stencil-bit transfer passes (docs/ultrawide-uw15-stencil-coverage.md) | `nb_transfer_stencil_coverage` (default false) and `nb_transfer_stencil_coverage_frames` (same-run windows, empty by default). Only for stencil-bit passes into a 2x or 4x destination: the transfer pixel shader drops SV_SampleIndex, emits the SDK's addressing, load and bit-test body once per sample of the pipeline's sample set with that sample's index as a literal, and writes the samples whose bit is set to SV_Coverage instead of discarding. Bit 31 of the transfer shader key marks the variant; only the copy passed to `GetOrCreateTransferPipelines` carries it, and a failed build falls back to the SDK passes. With both cvars off the generated shaders are the SDK's (two sample-index reads go through the equal `dest_sample`), plus cvar reads and window parses per call. Pass timing marks variant draws with bit 8 of the mode detail. `nb_transfer_stencil_coverage_verify_frames` adds a sample-exact check: after a checked draw's variant passes, occlusion queries count per bit the samples the SDK's shader would set (stencil ALWAYS), those that now have the bit (EQUAL) and every sample with the bit (no pixel shader, EQUAL); key bits 28-30 select those write-nothing pipelines. Draws inside an open guest occlusion query are skipped. One check per launch, up to 1,024 draws and at most 96 per shader key (so a long window reaches rare keys), logged with per-key draw counts after fence completion; its query heap and readback are released in `Shutdown` |
| `include/rex/graphics/d3d12/command_processor.h` | guest occlusion-query state for the UW15 check | `IsGuestOcclusionQueryActive()` returns `active_occlusion_query_.valid`; no behaviour change |
| `src/graphics/pipeline/texture/cache.cpp` | optional guest-layout extent memo for scaled-resolve checks (docs/ultrawide-uw16-texture-layout-memo.md) | At a render scale above 1x, `FindOrCreateTexture` computes a key's whole guest layout for every changed binding only to read its base and mip extents. With `nb_texture_layout_memo` (default off) or inside `nb_texture_layout_memo_frames`, those two extents come from a 1,024-entry direct-mapped memo whose key packs exactly the ten fields `TextureKey::GetGuestLayout` reads (60 bits plus a valid bit). `IsRangeScaledResolved` still runs on every call. `nb_texture_layout_memo_verify_frames` recomputes every hit and counts mismatches. Window cvars are evaluated once per frame. With every option off the function computes the layout as upstream |
| `src/graphics/pipeline/texture/cache.cpp` | optional generation-guarded memo of `FindOrCreateTexture`'s scaled-resolve decision (docs/ultrawide-uw17-scaled-resolve-memo.md) | `nb_scaled_resolve_memo` (default off), `nb_scaled_resolve_memo_frames` and `nb_scaled_resolve_memo_verify_frames`, evaluated once per frame. An atomic generation counter is bumped under the global critical region, before the bits change, whenever `MarkRangeAsResolved` sets a new scaled-resolve page bit, the global watch callback clears one, or the bitmap is zeroed. The decision for a key (keyed on its two extents and two pages, exactly the inputs of the two `IsRangeScaledResolved` calls) is reused only while that counter is unchanged, which skips the global lock and the bitmap scan. The verify window recomputes every hit and counts a difference as a mismatch only if the generation did not move meanwhile (otherwise as raced). With the option off, the decision is computed as upstream; the counter updates are the only other change |
| `include/rex/graphics/d3d12/command_processor.h`, `src/graphics/d3d12/command_processor.cpp`, `include/rex/graphics/d3d12/deferred_command_list.h`, `src/graphics/d3d12/deferred_command_list.cpp` | default-off EDRAM leaf GPU budget probe (NB_GPU_02; docs/native_performance.md) | The `nb_gpu_budget_*` cvars. `NativeGpuPassScope` also opens a budget scope while a captured submission is open; the probe accepts only the four existing EDRAM leaf labels. `BeginSubmission` reports the swap frame before any early return, then brackets each new submission. `EndSubmission` passes `is_closing_frame` and the Phase 3 transfer and dump counters after `SubmitBarriers`, and publishes once the original fence proves completion. `ShutdownContext` detaches the probe, then finishes it after the replay worker. The deferred list gains a recording-side probe pointer and count-only hooks on draws, dispatches, copies and clears. `SwapStorage` does not exchange that pointer; the move constructor transfers it. The probe is refused alongside legacy timing, submission diagnostics, replay chunks or the Phase 3 file. The async replay gate and all recorded commands are unchanged. With the cvar off only null-pointer checks remain |
Optional request diagnostics use original `native/native_range_diagnostics.h` and explicit
index/texture/native caller scopes. They time only the existing validity-scan lock and preserve
allocation, validity, uploads and return values. Outer-call time distinguishes recursive shadow
retries from inclusive totals. A separate count-only pipeline diagnostic records actual external
pointer changes before the unchanged `SetExternalPipeline` logic.

The optional shared residency mirror promotes only preexisting valid bits under the
original scan lock after all backing allocations succeed. CPU/GPU authority changes
clear atomic mirror bits before modifying authoritative flags. Frame, mode and lifetime
resets clear the mirror; recursive and unexpected callers cannot use it. The sole CP
owner permits conservative multiword hit checks without reading SDK validity vectors
outside their original lock. Newly uploaded pages are never published immediately.

The GPU target hashes the sorted vendored-header paths and contents into all compile commands.
Adding, removing or editing an override rebuilds every plugin object/PCH, avoiding stale class layouts
after include resolution or ABI changes. This does not alter the stock plugin or SDK sources.

Original `native/native_gpu_submission_timing.*` adds an opt-in bounded timestamp
capture around each actual backend direct-list replay. Unique query/readback slots
are never reused, and readback requires a healthy device and the captured
submission fence's completion. Publication happens after the requested range
ends, with no added GPU wait, because the game can bypass shutdown teardown.
The existing shutdown drain is also a supported completion path. CPU and GPU clock domains
remain separate. The diagnostic changes no draw, state, barrier or submission
ordering; it excludes presenter lists and is not a pure GPU-busy measurement.

The generic texture-cache copy was taken from the local source SDK's
`src/graphics/pipeline/texture/cache.cpp`, SHA256
`D365A4D8D7F2361244DDD6DB02984E1A003CF323B40CC90C63C36AF7A1D7D26E`.
Its original license header and all code outside the added flag/counter interface
and one invalidation-consume condition are preserved. The load-first path never
acknowledges a false observation; a producer after that load remains pending.
Counters coalesce notifications exactly as the original boolean flag does and
must not be interpreted as a count of memory writes.

Original `native/native_gpu_upload_pool.*` adds an opt-in native-only constant pool
derived from `rex::ui::GraphicsUploadBufferPool` — not from `D3D12UploadBufferPool`,
whose page type is private and whose `Request` downcasts to it. Its pages are committed
`D3D12_HEAP_TYPE_GPU_UPLOAD` buffers, so the same per-draw bytes live in CPU-writable
video memory instead of host memory. Page ownership, the single open page, retirement of
a full page and reuse only after the tagged index completes are the base class's
unchanged behaviour, so in-flight constants stay immutable. Slices are tagged with
`GetCurrentFrame()` and reclaimed with `GetCompletedFrame()`, matching the SDK's own
`Reclaim(frame_completed_)`. An adapter without `D3D12_OPTIONS16::GPUUploadHeapSupported`,
a reached video-memory budget, a creation or mapping failure, or any request the pool
cannot serve fall back to an ordinary upload page or to the command processor's own pool,
so optional failure falls through to the original SDK allocation. Host allocation
exceptions are contained, failed rollover latches further optional requests off,
and unproven shutdown retains resource references. No SDK source is edited.

The native resource-idle accessor additionally requires no open submission or
unproven direct queue operations. In the existing out-of-submission wait path,
Signal and event registration now check HRESULTs separately; a successful event
wait and completed target fence are required before clearing pending state.
Optional host submission diagnostics time only actual event waits. Original
`native/native_constant_upload_diagnostics.h` separates native allocation, RAM
assembly, mapped fills and memo CPU elapsed, with no clocks while disabled.

Regenerate the diff against upstream with:

```bash
for f in $(cd src/gpu/vendored && find src include -type f); do diff -u C:/rex/dl/sdk_pristine/$f src/gpu/vendored/$f; done > docs/vendored_upstream.diff
```

(`C:/rex/dl/sdk_pristine` is `git -C C:/rex/sdk archive HEAD` extracted; recreate it after an SDK bump.)

The D3D12 shared-memory copy also supports default-off same-run upload-shadow
windows (`nb_native_upload_shadow_window_first/period`), implemented by original
`native/native_upload_shadow_window.h`. Period zero preserves the existing
boolean option. Diagnostic transitions use the existing cache reset and log
cumulative counters. Byte comparisons, snapshots, authority, invalidations,
copy ordering and retries are unchanged. The 32:9 retest was slower; see
`docs/ultrawide-upload-shadow.md`. No normal-play option was enabled.

UW13 adds a synchronous guarded CPU read visitor to SharedMemory and a default-off
D3D12 upload-shadow direct comparison option. Only changed pages are snapshotted;
allocation, authority, invalidation and retry ordering are preserved. Optional
CPU clocks and ABBA windows are diagnostics, not normal-play defaults.
The existing bounded GPU submission capture now optionally records sparse,
unique-query pass intervals and resolve metadata, with the same fence-proven
readback and failed-drain resource retention. See docs/ultrawide-uw13-experiments.md.

Original `native/native_gpu_budget_probe.*` (NB_GPU_02) is a default-off companion to that capture that
keeps async replay on. It writes timestamps only at submission begin and end, and at the entry and
exit of accepted EDRAM leaves in at most six sampled frames. Operations only count. Query slots are
unique and never reused. Each submission resolves once per heap, after its bracket, only the pairs it
ended. The readback is mapped only after the original submission fence passes the last captured
submission on a healthy device; otherwise the query objects are deliberately retained. Frame-boundary
checks and a per-category census invalidate the output instead of reclassifying work. The boundary
checks require one close per frame, consecutive IDs and no live captured submission. Intervals are
elapsed time, not GPU busy time.
