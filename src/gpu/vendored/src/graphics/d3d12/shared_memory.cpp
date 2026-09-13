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
#include <bit>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include "native/native_upload_shadow.h"
#include "native/native_upload_shadow_compare.h"
#include "native/native_upload_shadow_window.h"

REXCVAR_DEFINE_BOOL(nb_native_upload_shadow_direct_compare, false, "nb",
                    "Experimental guarded direct comparison; capture only changed pages, requires upload shadow");
REXCVAR_DEFINE_BOOL(nb_native_upload_shadow_profile, false, "nb",
                    "Diagnostic upload clocks; never use for clean FPS measurements");
REXCVAR_DEFINE_BOOL(nb_native_upload_shadow_window_abba, false, "nb",
                    "Use off/on/on/off upload-shadow blocks instead of alternating blocks");
REXCVAR_DEFINE_BOOL(nb_native_upload_shadow, false, "nb",
                    "Compare complete CPU-authoritative upload pages with their last queued GPU bytes; unchanged pages skip WC writes and copies");
REXCVAR_DEFINE_UINT32(nb_native_upload_shadow_window_first, 6000, "nb",
                      "First backend frame of upload-shadow same-run windows");
REXCVAR_DEFINE_UINT32(nb_native_upload_shadow_window_period, 0, "nb",
                      "Diagnostic only: alternate off/on upload-shadow windows of this many backend frames; zero leaves the original option unchanged");

namespace nb::gpu {
namespace { NativeUploadShadowStats upload_shadow_stats; }
const NativeUploadShadowStats& GetNativeUploadShadowStats() { return upload_shadow_stats; }
}  // namespace nb::gpu

REXCVAR_DEFINE_BOOL(d3d12_tiled_shared_memory, true, "GPU/D3D12",
                    "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (REXCVAR_GET(d3d12_tiled_shared_memory) &&
      provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
      !provider.GetGraphicsAnalysis()) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  // No callback can reference the derived object or its arena after this point.
  if (upload_shadow_watch_) {
    UnregisterGlobalWatch(upload_shadow_watch_);
    upload_shadow_watch_ = nullptr;
  }
  upload_shadow_.reset();
  upload_shadow_scratch_.reset();
  upload_shadow_requested_ = false;
  upload_shadow_failed_ = false;
  upload_shadow_retrying_ = false;
  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  if (!command_processor_.FlushNativeReplay()) return false;
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  bool shadow_requested = REXCVAR_GET(nb_native_upload_shadow);
  if (shadow_requested) {
    shadow_requested = nb::gpu::NativeUploadShadowWindowEnabled(
        true, command_processor_.GetCurrentFrame(), REXCVAR_GET(nb_native_upload_shadow_window_first),
        REXCVAR_GET(nb_native_upload_shadow_window_period), REXCVAR_GET(nb_native_upload_shadow_window_abba));
  }
  if (!upload_shadow_retrying_ && UpdateUploadShadow(shadow_requested)) {
    ++nb::gpu::upload_shadow_stats.enabled_calls;
    return UploadRangesWithShadow(upload_page_ranges);
  }
  ++nb::gpu::upload_shadow_stats.disabled_calls;
  CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.SubmitBarriers();
  auto& command_list = command_processor_.GetDeferredCommandList();
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      const void* source = memory().TranslatePhysical(upload_range_start << page_size_log2());
      std::memcpy(upload_buffer_mapping, source, upload_buffer_size);
      ++nb::gpu::upload_shadow_stats.original_copy_commands;
      nb::gpu::upload_shadow_stats.original_copy_bytes += upload_buffer_size;
      {
        // Size histogram by whole pages; counting only, no behaviour change.
        const uint32_t copy_pages = std::max(uint32_t(1), uint32_t(upload_buffer_size >> page_size_log2()));
        const size_t bucket = std::min<size_t>(nb::gpu::kNativeUploadSizeBuckets - 1,
                                               size_t(std::bit_width(copy_pages)) - 1);
        ++nb::gpu::upload_shadow_stats.original_size_counts[bucket];
        nb::gpu::upload_shadow_stats.original_size_bytes[bucket] += upload_buffer_size;
      }
      command_list.D3DCopyBufferRegion(buffer_, upload_range_start << page_size_log2(),
                                       upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
  }
  return true;
}

bool D3D12SharedMemory::UpdateUploadShadow(bool enabled) {
  if (enabled != upload_shadow_requested_) {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    upload_shadow_requested_ = enabled;
    upload_shadow_failed_ = false;
    if (upload_shadow_) upload_shadow_->Reset();
    if (REXCVAR_GET(nb_native_upload_shadow_window_period)) {
      const auto& stats = nb::gpu::upload_shadow_stats;
      REXGPU_INFO("nb upload-shadow window {} at backend frame {}: original_bytes {}, shadow_copied {}, shadow_skipped {}, authority_fallback {}, retries {}",
                  enabled ? "on" : "off", command_processor_.GetCurrentFrame(), stats.original_copy_bytes,
                  stats.copied_bytes, stats.skipped_bytes, stats.authority_fallback_bytes, stats.invalidation_retries);
      if (REXCVAR_GET(nb_native_upload_shadow_profile)) {
        REXGPU_INFO("nb upload-profile frame {}: lock_wait_ns {}, authority_ns {}, compare_ns {}, snapshot_ns {}, upload_ns {}, snapshot_bytes {}",
                    command_processor_.GetCurrentFrame(), stats.lock_wait_ns, stats.authority_ns,
                    stats.compare_ns, stats.snapshot_ns, stats.upload_ns, stats.snapshot_bytes);
      }
    }
  }
  if (!enabled || upload_shadow_failed_) return false;
  if (!upload_shadow_) {
    // Prepare all storage before changing the upload path. Failed preparation
    // does not alter validity/protection or require a renderer failure.
    try {
      auto candidate = std::make_unique<nb::gpu::NativeUploadShadow>();
      auto scratch = std::make_unique<uint8_t[]>(nb::gpu::NativeUploadShadow::kSnapshotBytes);
      if (!candidate->Initialize(1u << page_size_log2())) {
        upload_shadow_failed_ = true;
      } else {
        auto lock = rex::thread::global_critical_region::AcquireDirect();
        upload_shadow_ = std::move(candidate);
        upload_shadow_scratch_ = std::move(scratch);
        upload_shadow_watch_ = RegisterGlobalWatch(UploadShadowInvalidated, this);
      }
    } catch (...) {
      // RegisterGlobalWatch allocates before linking; a thrown allocation leaves
      // no registered callback. Existing handles, if any, stay owned until shutdown.
      upload_shadow_failed_ = true;
    }
    if (upload_shadow_failed_) {
      ++nb::gpu::upload_shadow_stats.initialization_failures;
      upload_shadow_.reset();
      upload_shadow_scratch_.reset();
      return false;
    }
  }
  return upload_shadow_->initialized();
}

void D3D12SharedMemory::UploadShadowInvalidated(
    const std::unique_lock<std::recursive_mutex>&, void* context,
    uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu) {
  auto& self = *static_cast<D3D12SharedMemory*>(context);
  if (!self.upload_shadow_ || !self.upload_shadow_requested_) return;
  // Full explicit invalidation is meaningful even when it is reported as CPU
  // invalidation. Ordinary CPU writes leave the queued GPU bytes unchanged.
  if (address_first == 0 && address_last >= kBufferSize - 1) {
    self.upload_shadow_->Reset();
  } else if (invalidated_by_gpu) {
    self.upload_shadow_->Invalidate(address_first, address_last);
  }
}

bool D3D12SharedMemory::UploadRangesWithShadow(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  auto& stats = nb::gpu::upload_shadow_stats;
  const uint32_t page_bytes = 1u << page_size_log2();
  const bool direct_compare = REXCVAR_GET(nb_native_upload_shadow_direct_compare);
  const bool clocks = REXCVAR_GET(nb_native_upload_shadow_profile);
  uint64_t request_epoch;
  {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    request_epoch = upload_shadow_->epoch();
  }
  bool copying = false;
  DeferredCommandList* command_list = nullptr;
  auto begin_copy = [&]() {
    if (!copying) {
      CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
      command_processor_.SubmitBarriers();
      command_list = &command_processor_.GetDeferredCommandList();
      copying = true;
    }
  };
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      // Preserve the SDK's allocation amount/order, including partial pool
      // returns. A failed allocation must never leave uncopied pages valid.
      uint8_t* upload_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (!upload_mapping) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        return false;
      }
      stats.reserved_bytes += upload_buffer_size;
      const uint32_t allocation_address = upload_range_start << page_size_log2();
      MakeRangeValid(allocation_address, uint32_t(upload_buffer_size), false);
      for (size_t offset = 0; offset < upload_buffer_size;) {
        const size_t chunk_bytes = std::min(nb::gpu::NativeUploadShadow::kSnapshotBytes,
                                            upload_buffer_size - offset);
        const uint32_t address = allocation_address + uint32_t(offset);
        const uint32_t page_count = uint32_t(chunk_bytes / page_bytes);
        uint32_t changed_mask = 0;
        uint64_t captured_epoch = 0;
        bool captured;
        CpuReadTiming read_timing;
        auto inspect = [&](const uint8_t* source, uint32_t bytes) {
          // Invoked inside the authority guard. No source pointer escapes.
          captured_epoch = upload_shadow_->epoch();
          if (!direct_compare) {
            const uint64_t before = nb::gpu::UploadClock(clocks);
            std::memcpy(upload_shadow_scratch_.get(), source, bytes);
            stats.snapshot_ns += nb::gpu::UploadClock(clocks) - before;
            stats.snapshot_bytes += bytes;
            source = upload_shadow_scratch_.get();
          }
          const auto result = nb::gpu::ClassifyUploadPages(*upload_shadow_, address,
              {source, bytes}, upload_shadow_scratch_.get(), direct_compare, clocks);
          changed_mask = result.changed_mask;
          stats.compared_pages += page_count;
          stats.matched_pages += result.matched_pages;
          stats.skipped_bytes += uint64_t(result.matched_pages) * page_bytes;
          stats.snapshot_bytes += result.snapshot_bytes;
          stats.snapshot_ns += result.snapshot_ns;
          stats.compare_ns += result.compare_ns;
        };
        auto visit = [&]() {
          return VisitCpuAuthoritativeRange(address, uint32_t(chunk_bytes), &inspect,
              [](void* context, const uint8_t* source, uint32_t bytes) {
                (*static_cast<decltype(inspect)*>(context))(source, bytes);
              }, clocks ? &read_timing : nullptr);
        };
        if (direct_compare) {
          captured = visit();
        } else {
          // Preserve the old recursive acquisition for the diagnostic baseline.
          const uint64_t wait_start = nb::gpu::UploadClock(clocks);
          auto lock = rex::thread::global_critical_region::AcquireDirect();
          stats.lock_wait_ns += nb::gpu::UploadClock(clocks) - wait_start;
          captured = visit();
        }
        stats.lock_wait_ns += read_timing.lock_wait_ns;
        stats.authority_ns += read_timing.authority_ns;
        if (!captured) {
          auto lock = rex::thread::global_critical_region::AcquireDirect();
          upload_shadow_->Invalidate(address, address + uint32_t(chunk_bytes) - 1);
        }
        if (!captured) {
          // Same guest -> WC source and copy extent as the SDK. Never publish a
          // guessed identity for bytes whose CPU authority could not be captured.
          const uint64_t upload_start = nb::gpu::UploadClock(clocks);
          begin_copy();
          std::memcpy(upload_mapping + offset, memory().TranslatePhysical(address), chunk_bytes);
          command_list->D3DCopyBufferRegion(buffer_, address, upload_buffer,
                                           UINT64(upload_buffer_offset + offset), UINT64(chunk_bytes));
          stats.upload_ns += nb::gpu::UploadClock(clocks) - upload_start;
          stats.authority_fallback_bytes += chunk_bytes;
          stats.copied_bytes += chunk_bytes;
          ++stats.copy_commands;
        } else {

          if (!changed_mask) ++stats.all_matched_chunks;
          for (uint32_t page = 0; page < page_count;) {
            if (!(changed_mask & (1u << page))) { ++page; continue; }
            const uint32_t first = page;
            do { ++page; } while (page < page_count && (changed_mask & (1u << page)));
            const size_t run_offset = size_t(first) * page_bytes;
            const size_t run_bytes = size_t(page - first) * page_bytes;
            const uint64_t upload_start = nb::gpu::UploadClock(clocks);
            begin_copy();
            std::memcpy(upload_mapping + offset + run_offset,
                        upload_shadow_scratch_.get() + run_offset, run_bytes);
            command_list->D3DCopyBufferRegion(buffer_, address + uint32_t(run_offset), upload_buffer,
                                             UINT64(upload_buffer_offset + offset + run_offset), UINT64(run_bytes));
            stats.upload_ns += nb::gpu::UploadClock(clocks) - upload_start;
            stats.copied_bytes += run_bytes;
            ++stats.copy_commands;
            // This publishes the exact scratch bytes supplied to the recorded
            // copy. Any intervening GPU/full reset changes the token and rejects
            // publication, even if the callback came from another CPU thread.
            auto lock = rex::thread::global_critical_region::AcquireDirect();
            const uint64_t evictions_before = upload_shadow_->evictions();
            for (uint32_t publish_page = first; publish_page < page; ++publish_page) {
              if (!upload_shadow_->Publish(address + publish_page * page_bytes,
                      {upload_shadow_scratch_.get() + size_t(publish_page) * page_bytes, page_bytes},
                      captured_epoch)) ++stats.publication_rejects;
            }
            stats.evictions += upload_shadow_->evictions() - evictions_before;
          }
        }
        offset += chunk_bytes;
      }
      const uint32_t upload_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_pages;
      upload_range_length -= upload_pages;
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> retry_ranges;
  {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    if (upload_shadow_->epoch() == request_epoch) {
      // This locked cutoff is the same as an ordinary valid-page RequestRanges
      // check. All-match leaves the previous state and pending UAV flag intact.
      return true;
    }
    // A GPU/full-reset callback after classification can invalidate a skipped
    // page. Re-evaluate live SDK authority, rather than copying an old snapshot
    // over newer GPU data. Snapshot-refusal invalidation also takes this safe
    // path. Own the spans before recursion reuses SharedMemory::upload_ranges_.
    try {
      retry_ranges.reserve(upload_page_ranges.size());
      for (const auto& range : upload_page_ranges) {
        retry_ranges.emplace_back(range.first << page_size_log2(),
                                   range.second << page_size_log2());
      }
    } catch (...) {
      upload_shadow_->Reset();
      ++stats.retry_allocation_failures;
      return false;
    }
    // The ordinary retry may queue different CPU bytes; do not retain prior
    // identities for any retried page, including those that were matched here.
    for (const auto& range : retry_ranges) {
      upload_shadow_->Invalidate(range.first, range.first + range.second - 1);
    }
  }
  ++stats.invalidation_retries;
  struct ScopedBypass {
    bool& flag;
    bool previous;
    explicit ScopedBypass(bool& value) : flag(value), previous(value) { flag = true; }
    ~ScopedBypass() { flag = previous; }
  } bypass(upload_shadow_retrying_);
  // No accesses to upload_page_ranges after entering the recursive request.
  // The bypass also suppresses mode updates, so this cannot recurse into shadow.
  return RequestRanges(retry_ranges.data(), retry_ranges.size());
}

}  // namespace rex::graphics::d3d12
