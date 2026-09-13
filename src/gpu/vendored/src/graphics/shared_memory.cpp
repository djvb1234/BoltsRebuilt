#include <chrono>
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/thread/mutex.h>

#include "native/native_invalidation_range.h"
#include "native/native_range_batch.h"
#include "native/native_range_diagnostics.h"
#include "native/native_shared_residency_mirror.h"

REXCVAR_DEFINE_BOOL(nb_range_batch_optimize, true, "nb",
                    "Use stack storage for small shared-memory batches and merge overlapping upload pages");
REXCVAR_DEFINE_BOOL(nb_range_batch_diagnostics, false, "nb",
                    "Collect cumulative shared-memory range batching counters on the command processor thread");
REXCVAR_DEFINE_BOOL(nb_range_single_fastpath, false, "nb",
                    "Bypass batch storage for one shared-memory range while retaining allocation and validity checks");
REXCVAR_DEFINE_BOOL(nb_native_narrow_invalidation, false, "nb",
                    "Cap excess CPU write invalidation at 64KiB blocks, preserving actual writes and GPU authority")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_invalidation_diagnostics, false, "nb",
                    "Count CPU callback ranges and retained valid pages under the existing global lock");
REXCVAR_DEFINE_BOOL(nb_native_range_diagnostics, false, "nb",
                    "Profile shared-memory request outcomes and validity-scan lock intervals by explicit caller")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(nb_native_shared_residency_mirror, false, "nb",
                    "Reuse completed shared-memory residency checks with an atomically invalidated page mirror")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace nb::gpu {
namespace {
NativeRangeBatchStats range_batch_stats;
NativeInvalidationStats invalidation_stats;
thread_local NativeRangeDiagnosticsStats range_diagnostics_stats;
thread_local NativeRangeCaller range_diagnostics_caller = NativeRangeCaller::kOther;
thread_local uint64_t range_diagnostics_depth = 0;
thread_local NativeSharedResidencyStats shared_residency_stats;

uint64_t NativeRangeNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

enum class NativeRangeOutcome { kAborted, kEmpty, kInvalid, kAllocationFailure, kNoUpload, kUpload };

// Snapshot enablement once. Disabled calls do not read the clock or touch TLS
// counters. The observer also records early returns and exception unwinding.
class NativeRangeCallObservation {
 public:
  NativeRangeCallObservation(bool enabled, size_t input_count) noexcept : stats_(nullptr) {
    if (!enabled) return;
    stats_ = &range_diagnostics_stats.callers[size_t(range_diagnostics_caller)];
    start_ns_ = NativeRangeNowNs();
    top_level_ = range_diagnostics_depth++ == 0;
    ++stats_->calls;
    if (top_level_) ++stats_->top_level_calls;
    stats_->input_ranges += input_count;
  }
  ~NativeRangeCallObservation() {
    if (!stats_) return;
    const uint64_t elapsed = NativeRangeNowNs() - start_ns_;
    stats_->total_ns += elapsed;
    if (top_level_) stats_->top_level_ns += elapsed;
    switch (outcome_) {
      case NativeRangeOutcome::kAborted: ++stats_->aborted_calls; break;
      case NativeRangeOutcome::kEmpty: ++stats_->empty_calls; break;
      case NativeRangeOutcome::kInvalid: ++stats_->invalid_calls; break;
      case NativeRangeOutcome::kAllocationFailure: ++stats_->allocation_failures; break;
      case NativeRangeOutcome::kNoUpload:
        ++stats_->no_upload_calls;
        stats_->no_upload_ns += elapsed;
        break;
      case NativeRangeOutcome::kUpload: stats_->upload_ns += elapsed; break;
    }
    --range_diagnostics_depth;
  }
  NativeRangeCallObservation(const NativeRangeCallObservation&) = delete;
  NativeRangeCallObservation& operator=(const NativeRangeCallObservation&) = delete;
  NativeRangeCallerStats* stats() const noexcept { return stats_; }
  void Outcome(NativeRangeOutcome outcome) noexcept { if (stats_) outcome_ = outcome; }

 private:
  NativeRangeCallerStats* stats_;
  uint64_t start_ns_;
  bool top_level_;
  NativeRangeOutcome outcome_ = NativeRangeOutcome::kAborted;
};

class NativeRangeLockObservation {
 public:
  NativeRangeLockObservation(NativeRangeCallerStats* stats, uint64_t acquire_start_ns) noexcept
      : stats_(stats) {
    if (!stats_) return;
    acquired_ns_ = NativeRangeNowNs();
    ++stats_->lock_calls;
    stats_->lock_acquire_ns += acquired_ns_ - acquire_start_ns;
  }
  ~NativeRangeLockObservation() {
    if (stats_) stats_->lock_hold_ns += NativeRangeNowNs() - acquired_ns_;
  }
  NativeRangeLockObservation(const NativeRangeLockObservation&) = delete;
  NativeRangeLockObservation& operator=(const NativeRangeLockObservation&) = delete;

 private:
  NativeRangeCallerStats* stats_;
  uint64_t acquired_ns_;
};
}
const NativeRangeBatchStats& GetNativeRangeBatchStats() { return range_batch_stats; }
bool NativeRangeDiagnosticsEnabled() { return REXCVAR_GET(nb_native_range_diagnostics); }
const NativeRangeDiagnosticsStats& GetNativeRangeDiagnosticsStats() noexcept {
  return range_diagnostics_stats;
}
const NativeSharedResidencyStats& GetNativeSharedResidencyStats() noexcept {
  return shared_residency_stats;
}
NativeRangeCallerScope::NativeRangeCallerScope(NativeRangeCaller caller, bool enabled) noexcept
    : enabled_(enabled) {
  if (!enabled_) return;
  previous_ = range_diagnostics_caller;
  range_diagnostics_caller = size_t(caller) < size_t(NativeRangeCaller::kCount)
      ? caller : NativeRangeCaller::kOther;
}
NativeRangeCallerScope::~NativeRangeCallerScope() {
  if (enabled_) range_diagnostics_caller = previous_;
}
NativeInvalidationStats SnapshotNativeInvalidationStats() {
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  return invalidation_stats;
}
}  // namespace nb::gpu

namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  {
    auto global_lock = global_critical_region_.Acquire();
    native_residency_mirror_.reset();
    native_residency_owner_ = std::this_thread::get_id();
    native_residency_request_depth_ = 0;
    native_residency_unsupported_caller_.store(false, std::memory_order_seq_cst);
    native_residency_requested_ = false;
    native_residency_initialization_failed_ = false;
  }
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_.assign(num_system_page_flags_, 0);
  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (native_residency_mirror_) native_residency_mirror_->Reset();
  }
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  {
    auto global_lock = global_critical_region_.Acquire();
    // Retire the sidecar while callbacks are excluded, before backing storage
    // or page vectors are released. Repeated ShutdownCommon is harmless.
    native_residency_mirror_.reset();
  }
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  if (native_residency_mirror_) native_residency_mirror_->Reset();
  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));
  std::fill(system_page_flags_valid_and_gpu_written_.begin(),
            system_page_flags_valid_and_gpu_written_.end(), uint64_t(0));
  // nb: an explicit invalidation can occur within a frame. Notify both range
  // and global watches under the same lock/order as MemoryInvalidationCallback,
  // so consumers cannot retain a byte match across the following reupload.
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  if (native_residency_mirror_) native_residency_mirror_->Reset();
  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  system_page_flags_valid_ = system_page_flags_valid_and_gpu_written_;
}

void SharedMemory::ClearCache() {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (native_residency_mirror_) native_residency_mirror_->Reset();
  }
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

void SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;

  {
    auto global_lock = global_critical_region_.Acquire();

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;
      if (i == valid_block_first) {
        valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
      }
      if (i == valid_block_last && (valid_page_last & 63) != 63) {
        valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
      }
      // This precedes both CPU/GPU authority updates. In particular, do not
      // publish here: the CPU upload copy has not happened yet.
      if (native_residency_mirror_) native_residency_mirror_->ClearWord(i, valid_bits);
      system_page_flags_valid_[i] |= valid_bits;
      uint64_t& gpu_written = system_page_flags_valid_and_gpu_written_[i];
      gpu_written = written_by_gpu ? (gpu_written | valid_bits) : (gpu_written & ~valid_bits);
    }
  }

  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

nb::gpu::NativeSharedResidencyMirror* SharedMemory::PrepareNativeResidencyRequest(
    bool requested, bool owning_caller, bool outer_request) {
  auto& stats = nb::gpu::shared_residency_stats;
  if (requested) ++stats.requests;
  if (!owning_caller) {
    // The original upload vector is CP-owned. Do not extend mirror guarantees
    // to an unexpected caller or mutate the CP's depth/mode fields from it.
    auto global_lock = global_critical_region_.Acquire();
    native_residency_unsupported_caller_.store(true, std::memory_order_seq_cst);
    if (native_residency_mirror_) native_residency_mirror_->Disable();
    if (requested) ++stats.unsupported_caller_bypasses;
    return nullptr;
  }
  if (!outer_request) {
    if (requested) ++stats.recursive_bypasses;
    return nullptr;
  }
  if (requested != native_residency_requested_) {
    auto global_lock = global_critical_region_.Acquire();
    native_residency_requested_ = requested;
    native_residency_initialization_failed_ = false;
    if (native_residency_mirror_) native_residency_mirror_->Reset();
    ++stats.mode_resets;
  }
  if (!requested) return nullptr;
  if (native_residency_unsupported_caller_.load(std::memory_order_seq_cst)) {
    ++stats.unsupported_caller_bypasses;
    return nullptr;
  }
  if (page_size_log2_ != nb::gpu::NativeSharedResidencyMirror::kPageLog2) {
    ++stats.unsupported_page_bypasses;
    return nullptr;
  }
  if (!native_residency_mirror_ && !native_residency_initialization_failed_) {
    try {
      // Allocate ordinary RAM before publication, without holding the global
      // lock. Allocation failure only disables this optional optimization.
      auto candidate = std::make_unique<nb::gpu::NativeSharedResidencyMirror>();
      auto global_lock = global_critical_region_.Acquire();
      // An unexpected caller may have arrived while allocation was outside the
      // lock and no published pointer existed yet. Its lifetime latch wins.
      if (native_residency_unsupported_caller_.load(std::memory_order_seq_cst)) {
        ++stats.unsupported_caller_bypasses;
        return nullptr;
      }
      native_residency_mirror_ = std::move(candidate);
    } catch (...) {
      native_residency_initialization_failed_ = true;
      ++stats.initialization_failures;
    }
  }
  if (!native_residency_mirror_) return nullptr;
  if (!native_residency_mirror_->enabled()) {
    ++stats.unsupported_caller_bypasses;
    return nullptr;
  }
  return native_residency_mirror_.get();
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  nb::gpu::NativeRangeCallObservation range_observation(
      nb::gpu::NativeRangeDiagnosticsEnabled(), count);
  nb::gpu::NativeRangeCallerStats* range_profile = range_observation.stats();
  if (ranges == nullptr || !count) {
    range_observation.Outcome(nb::gpu::NativeRangeOutcome::kEmpty);
    return true;
  }

  const bool residency_owner = std::this_thread::get_id() == native_residency_owner_;
  nb::gpu::NativeSharedResidencyRequestScope residency_scope(
      residency_owner ? &native_residency_request_depth_ : nullptr);
  nb::gpu::NativeSharedResidencyMirror* residency_mirror = PrepareNativeResidencyRequest(
      REXCVAR_GET(nb_native_shared_residency_mirror), residency_owner, residency_scope.outer());

  const bool optimize = REXCVAR_GET(nb_range_batch_optimize);
  nb::gpu::NativeRangeBatchStats* diagnostics = REXCVAR_GET(nb_range_batch_diagnostics)
      ? &nb::gpu::range_batch_stats : nullptr;
  if (diagnostics) {
    ++diagnostics->calls;
    if (optimize) ++diagnostics->optimized_calls;
    else ++diagnostics->legacy_calls;
    diagnostics->input_ranges += count;
  }
  SCOPE_profile_cpu_f("gpu");
  // Both preparation paths use this exact allocation/validity/upload sequence.
  // In particular, the fast path does not pre-scan validity or acquire an extra
  // lock on a miss, and it retains current CPU/GPU authority on every request.
  const auto request_merged_ranges =
      [&](std::span<const std::pair<uint32_t, uint32_t>> merged_ranges) {
    if (diagnostics) diagnostics->merged_ranges += merged_ranges.size();
    if (range_profile) {
      range_profile->normalized_ranges += merged_ranges.size();
      for (const auto& range : merged_ranges) range_profile->requested_bytes += range.second;
    }
    // Some texture or buffer is empty, for example - safe to draw in this case.
    if (merged_ranges.empty()) {
      range_observation.Outcome(nb::gpu::NativeRangeOutcome::kEmpty);
      return true;
    }

    if (residency_mirror) {
      auto& stats = nb::gpu::shared_residency_stats;
      if (residency_mirror->Contains(merged_ranges)) {
        ++stats.hits;
        stats.hit_ranges += merged_ranges.size();
        for (const auto& range : merged_ranges) {
          stats.hit_bytes += range.second;
          stats.hit_pages += ((range.first + range.second - 1) >> page_size_log2_) -
                             (range.first >> page_size_log2_) + 1;
        }
        if (diagnostics) ++diagnostics->no_upload_calls;
        range_observation.Outcome(nb::gpu::NativeRangeOutcome::kNoUpload);
        COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
        COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count", uint32_t(merged_ranges.size()));
        COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count", uint32_t(0));
        return true;
      }
      ++stats.misses;
    }

    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
        range_observation.Outcome(nb::gpu::NativeRangeOutcome::kAllocationFailure);
        return false;
      }
    }

    upload_ranges_.clear();
    auto append_upload_range = [this, optimize, diagnostics](uint32_t page_start, uint32_t page_count) {
      const uint32_t overlap = nb::gpu::AppendUploadPageRange(upload_ranges_, page_start, page_count, optimize);
      if (diagnostics && overlap) {
        ++diagnostics->overlap_events;
        diagnostics->overlap_pages_saved += overlap;
      }
    };
    {
      const uint64_t lock_start_ns = range_profile ? nb::gpu::NativeRangeNowNs() : 0;
      auto global_lock = global_critical_region_.Acquire();
      // Declared after global_lock, so the hold timer ends before its unlock,
      // including when range-vector growth throws during the scan.
      nb::gpu::NativeRangeLockObservation lock_observation(range_profile, lock_start_ns);
      for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
        uint32_t page_first = range.first >> page_size_log2_;
        uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
        if (diagnostics) diagnostics->page_visits += page_last - page_first + 1;
        if (range_profile) range_profile->requested_page_visits += page_last - page_first + 1;
        uint32_t block_first = page_first >> 6;
        uint32_t block_last = page_last >> 6;
        uint32_t range_start = UINT32_MAX;
        for (uint32_t i = block_first; i <= block_last; ++i) {
          uint64_t block_valid = system_page_flags_valid_[i];
          const uint64_t original_valid = block_valid;
          uint64_t requested_mask = UINT64_MAX;
          // Consider pages in the block outside the requested range valid.
          if (i == block_first) {
            uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
            block_valid |= block_before;
            requested_mask &= ~block_before;
          }
          if (i == block_last && (page_last & 63) != 63) {
            uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
            block_valid |= ~block_inside;
            requested_mask &= block_inside;
          }

          // Only preexisting valid bits, under the original scan lock and after
          // ALL requested backing allocations succeeded. Missing/uploaded bits
          // deliberately remain uncached until another ordinary request.
          if (residency_mirror) {
            nb::gpu::shared_residency_stats.promoted_pages +=
                residency_mirror->PromoteValidWord(i, original_valid, requested_mask);
          }

          while (true) {
            uint32_t block_page;
            if (range_start == UINT32_MAX) {
              // Check if need to open a new range.
              if (!rex::bit_scan_forward(~block_valid, &block_page)) {
                break;
              }
              range_start = (i << 6) + block_page;
            } else {
              // Check if need to close the range.
              // Ignore the valid pages before the beginning of the range.
              uint64_t block_valid_from_start = block_valid;
              if (i == (range_start >> 6)) {
                block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
              }
              if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
                break;
              }
              append_upload_range(range_start, (i << 6) + block_page - range_start);
              // In the next iteration within this block, consider this range
              // valid since it has been queued for upload.
              block_valid |= (uint64_t(1) << block_page) - 1;
              range_start = UINT32_MAX;
            }
          }
        }
        if (range_start != UINT32_MAX) {
          append_upload_range(range_start, page_last + 1 - range_start);
        }
      }
    }

    COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
    COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                      uint32_t(merged_ranges.size()));
    COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                      uint32_t(upload_ranges_.size()));

    if (upload_ranges_.empty()) {
      if (diagnostics) ++diagnostics->no_upload_calls;
      range_observation.Outcome(nb::gpu::NativeRangeOutcome::kNoUpload);
      return true;
    }

    if (diagnostics) {
      ++diagnostics->upload_calls;
      for (const auto& upload : upload_ranges_) diagnostics->upload_pages += upload.second;
    }

    if (range_profile) {
      ++range_profile->upload_calls;
      range_profile->upload_range_count += upload_ranges_.size();
      for (const auto& upload : upload_ranges_) range_profile->upload_page_count += upload.second;
    }
    const bool uploaded = UploadRanges(upload_ranges_);
    if (range_profile && !uploaded) ++range_profile->upload_failures;
    range_observation.Outcome(nb::gpu::NativeRangeOutcome::kUpload);
    return uploaded;
  };

  const auto single = nb::gpu::SelectNativeSingleRange(
      ranges, count, kBufferSize, REXCVAR_GET(nb_range_single_fastpath));
  if (single.kind != nb::gpu::NativeSingleRangeKind::kNotSelected) {
    if (diagnostics) ++diagnostics->single_fast_calls;
    if (single.kind == nb::gpu::NativeSingleRangeKind::kInvalid) {
      if (diagnostics) ++diagnostics->invalid_requests;
      range_observation.Outcome(nb::gpu::NativeRangeOutcome::kInvalid);
      return false;
    }
    return request_merged_ranges(single.ranges());
  }

  // Keep this construction after the selected-path return: std::pair default
  // construction initializes the entire bounded array even for one input.
  nb::gpu::NativeRangeBatch batch;
  if (!batch.Build(ranges, count, kBufferSize, optimize)) {
    if (diagnostics) ++diagnostics->invalid_requests;
    range_observation.Outcome(nb::gpu::NativeRangeOutcome::kInvalid);
    return false;
  }
  if (diagnostics) {
    if (batch.on_stack()) ++diagnostics->stack_calls;
    else ++diagnostics->heap_calls;
  }
  return request_merged_ranges(batch.ranges());
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

bool SharedMemory::CopyCpuAuthoritativeRange(uint32_t start, uint32_t length,
                                           void* destination) {
  if (!destination) return false;
  return VisitCpuAuthoritativeRange(start, length, destination,
      [](void* context, const uint8_t* source, uint32_t bytes) {
        std::memcpy(context, source, bytes);
      });
}

bool SharedMemory::VisitCpuAuthoritativeRange(uint32_t start, uint32_t length,
    void* context, CpuReadVisitor visitor, CpuReadTiming* timing) {
  if (!visitor || !length || start >= kBufferSize || length > kBufferSize - start) {
    return false;
  }
  const auto ticks = [timing]() -> uint64_t {
    return timing ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) : 0;
  };
  const uint64_t wait_start = ticks();
  auto global_lock = global_critical_region_.Acquire();
  const uint64_t authority_start = ticks();
  if (timing) timing->lock_wait_ns += authority_start - wait_start;
  struct AuthorityTimer {
    CpuReadTiming* timing;
    uint64_t begin;
    bool ended = false;
    void End() {
      if (timing && !ended) {
        timing->authority_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) - begin;
        ended = true;
      }
    }
    ~AuthorityTimer() { End(); }
  } authority_timer{timing, authority_start};
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  if ((page_last >> 6) >= system_page_flags_valid_and_gpu_written_.size()) {
    return false;
  }
  for (uint32_t block = page_first >> 6; block <= (page_last >> 6); ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == (page_first >> 6)) mask &= UINT64_MAX << (page_first & 63);
    if (block == (page_last >> 6)) mask &= UINT64_MAX >> (63 - (page_last & 63));
    if ((system_page_flags_valid_[block] & mask) != mask ||
        (system_page_flags_valid_and_gpu_written_[block] & mask)) return false;
  }
  authority_timer.End();
  visitor(context, reinterpret_cast<const uint8_t*>(memory().TranslatePhysical(start)), length);
  return true;
}

bool SharedMemory::IsRangeGpuWritten(uint32_t start, uint32_t length) {
  if (!length) {
    return false;
  }
  // Nothing is tracked outside the buffer, so the answer there has to be the cautious one.
  if (start >= kBufferSize || length > kBufferSize - start) {
    return true;
  }
  auto global_lock = global_critical_region_.Acquire();
  const uint32_t page_first = start >> page_size_log2_;
  const uint32_t page_last = (start + length - 1) >> page_size_log2_;
  if ((page_last >> 6) >= system_page_flags_valid_and_gpu_written_.size()) {
    return true;
  }
  for (uint32_t block = page_first >> 6; block <= (page_last >> 6); ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == (page_first >> 6)) mask &= UINT64_MAX << (page_first & 63);
    if (block == (page_last >> 6)) mask &= UINT64_MAX >> (63 - (page_last & 63));
    if (system_page_flags_valid_and_gpu_written_[block] & mask) {
      return true;
    }
  }
  return false;
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  const nb::gpu::NativeInvalidationRange requested_range{page_first, page_last};
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start = system_page_flags_valid_and_gpu_written_[block_first];
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end = system_page_flags_valid_and_gpu_written_[block_last];
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
  }

  const nb::gpu::NativeInvalidationRange original_range{page_first, page_last};
  const bool narrow = REXCVAR_GET(nb_native_narrow_invalidation);
  const auto selected_range = nb::gpu::CapNativeInvalidationRange(
      requested_range, original_range, page_size_log2_, exact_range, narrow);
  // These SAME endpoints control page validity, all watch callbacks and the
  // returned physical unwatch extent. Omitted neighbors remain write-watched
  // by the runtime, so their later CPU writes still trigger invalidation.
  page_first = selected_range.first;
  page_last = selected_range.last;
  nb::gpu::NativeInvalidationStats* diagnostics = REXCVAR_GET(nb_native_invalidation_diagnostics)
      ? &nb::gpu::invalidation_stats : nullptr;
  nb::gpu::NativeInvalidationRange potential_range = original_range;
  if (diagnostics) {
    ++diagnostics->callbacks;
    diagnostics->exact_callbacks += exact_range;
    diagnostics->enabled_callbacks += narrow;
    diagnostics->narrowed_callbacks += selected_range != original_range;
    diagnostics->requested_pages += nb::gpu::NativeInvalidationPageCount(requested_range);
    diagnostics->original_pages += nb::gpu::NativeInvalidationPageCount(original_range);
    diagnostics->selected_pages += nb::gpu::NativeInvalidationPageCount(selected_range);
    potential_range = nb::gpu::CapNativeInvalidationRange(
        requested_range, original_range, page_size_log2_, exact_range, true);
  }

  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    if (diagnostics && potential_range != original_range) {
      const uint64_t valid = system_page_flags_valid_[i];
      const uint64_t gpu_written = system_page_flags_valid_and_gpu_written_[i];
      const uint32_t retained = nb::gpu::NativeRetainedCpuPages(
          original_range, potential_range, i, valid, gpu_written);
      diagnostics->potential_retained_cpu_pages += retained;
      if (narrow) diagnostics->retained_cpu_pages += retained;
    }
    // Direct invalidation is required here: FireWatches below occurs after the
    // authoritative flags change and would be too late for a lock-free hit.
    if (native_residency_mirror_) native_residency_mirror_->ClearWord(i, invalidate_bits);
    system_page_flags_valid_[i] &= ~invalidate_bits;
    system_page_flags_valid_and_gpu_written_[i] &= ~invalidate_bits;
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
