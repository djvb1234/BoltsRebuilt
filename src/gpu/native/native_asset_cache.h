// Original nb cache for locally extracted, byte-verified immutable guest buffers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <d3d12.h>

namespace rex::graphics {
class SharedMemory;
namespace d3d12 { class D3D12CommandProcessor; }
}

namespace nb::gpu {

class NativeAssetCache {
 public:
  // VS-only uint4. The shader must check the complete raw load against this
  // guest span, using subtraction after address >= guest_base, before rebasing.
  // Keep shared memory bound and resident for every load outside this span.
  struct Binding {
    uint32_t guest_base = 0;
    uint32_t byte_length = 0;
    uint32_t asset_offset = 0;
    uint32_t enabled = 0;
  };
  static_assert(sizeof(Binding) == 16);
  struct Stats {
    uint64_t assets = 0, arena_bytes = 0;
    uint64_t resolves = 0, hits = 0, cached_hits = 0;
    uint64_t misses = 0, snapshots = 0, snapshot_bytes = 0;
    uint64_t authority_refusals = 0, invalidation_races = 0, frame_resets = 0;
    uint64_t mode_switches = 0, legacy_capacity_resets = 0;
    uint64_t mapping_collision_steps = 0, mapping_probe_refusals = 0;
    uint64_t mapping_high_water = 0, mapping_max_probe = 0, mapping_generation_wraps = 0;
    uint64_t snapshot_growths = 0, snapshot_growth_bytes = 0;
  };

  NativeAssetCache();
  ~NativeAssetCache();
  NativeAssetCache(const NativeAssetCache&) = delete;
  NativeAssetCache& operator=(const NativeAssetCache&) = delete;

  // CPU/device setup only. Pack failure leaves the cache disabled. The file is
  // local user data and is never compiled into or copied into the repository.
  bool Initialize(ID3D12Device* device, rex::graphics::SharedMemory& shared_memory,
                  const std::filesystem::path& pack, std::string* error = nullptr);
  // Call with an open submission. The copy precedes all cached reads on the same
  // queue. Both default and staging resources live until Shutdown().
  bool EnsureUploaded(rex::graphics::d3d12::D3D12CommandProcessor& cp);
  // REQUIRED: successfully RequestRanges for this guest span first, including
  // rearming protection after frame resets. New ranges are CPU-authority checked
  // and byte-compared once per frame; same-frame writes invalidate cached hits.
  // legacy retains the previous unordered-map/resize path for same-build A/B.
  // Switching modes invalidates both mapping backends, even within one frame.
  Binding Resolve(uint32_t guest_base, uint32_t byte_length, uint64_t frame_index,
                  bool legacy = false);
  D3D12_GPU_VIRTUAL_ADDRESS gpu_address() const;
  bool initialized() const;
  const Stats& stats() const;
  // Caller must finish all GPU submissions first and destroy this cache before
  // SharedMemory. Unregisters the global watch before releasing its context.
  void Shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nb::gpu
