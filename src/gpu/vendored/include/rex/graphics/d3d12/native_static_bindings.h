// Original native geometry binding cache. It contains no D3D12 objects so its
// invalidation rules can be checked independently of the graphics runtime.
#pragma once

#include <array>
#include <cstdint>

namespace rex::graphics::d3d12 {

class NativeStaticBindings {
 public:
  enum : uint32_t {
    kTextureTable = 1u << 0,
    kSamplerTable = 1u << 1,
    kGuestMemory = 1u << 2,
    kAssetMemory = 1u << 3,
  };
  struct Stats {
    // Counts individual root bindings, not draws. No atomics are needed: all
    // updates and snapshots belong to the command processor thread.
    uint64_t hits = 0;
    uint64_t rebinds = 0;
    uint64_t pixel_hits = 0;
    uint64_t pixel_rebinds = 0;
  };

  void InvalidateAll() { valid_ = 0; pixel_valid_ = false; }
  void InvalidateDescriptorTables() { valid_ &= ~(kTextureTable | kSamplerTable); }

  // Values are texture table, sampler table, guest SRV and asset SRV, in that
  // order. An unused sampler table is neither written nor marked valid. The
  // caller must record every returned binding before another cache operation.
  uint32_t Update(uintptr_t signature, const std::array<uint64_t, 4>& values,
                  bool sampler_used, bool enabled) {
    if (signature_ != signature) InvalidateAll();
    signature_ = signature;
    const uint32_t requested = kTextureTable | kGuestMemory | kAssetMemory |
                               (sampler_used ? kSamplerTable : 0u);
    uint32_t rebind = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bit = 1u << i;
      if (!(requested & bit)) continue;
      if (enabled && (valid_ & bit) && values_[i] == values[i]) {
        ++stats_.hits;
      } else {
        rebind |= bit;
        ++stats_.rebinds;
      }
      values_[i] = values[i];
    }
    // Disabled draws always write all requested values. Discarding validity
    // also makes a later diagnostic toggle independent of this mode's history.
    valid_ = enabled ? (valid_ | requested) : 0;
    return rebind;
  }

  // Root 4 has independent policy from the four static roots. Its immutable
  // upload allocation is identified by the full GPU address, never by reading
  // the mapped buffer. A query does not publish a miss: the caller must first
  // append the actual root write, then call DidWritePixelConstantBuffer.
  bool NeedsPixelConstantBufferWrite(uintptr_t signature, uint64_t address,
                                     bool enabled) {
    if (signature_ != signature) InvalidateAll();
    signature_ = signature;
    if (!enabled) pixel_valid_ = false;
    if (pixel_valid_ && pixel_address_ == address) {
      ++stats_.pixel_hits;
      return false;
    }
    return true;
  }

  void DidWritePixelConstantBuffer(uint64_t address, bool enabled) {
    pixel_address_ = address;
    pixel_valid_ = enabled;
    ++stats_.pixel_rebinds;
  }

  const Stats& stats() const { return stats_; }

 private:
  uintptr_t signature_ = 0;
  std::array<uint64_t, 4> values_{};
  uint32_t valid_ = 0;
  uint64_t pixel_address_ = 0;
  bool pixel_valid_ = false;
  Stats stats_{};
};

}  // namespace rex::graphics::d3d12
