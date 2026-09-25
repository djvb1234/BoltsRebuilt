// Original memo of the last render target claim in RenderTargetCache::Update
// (vendored pipeline/render_target/cache.cpp). After a successful claim every
// used slot's render target owns the EDRAM range it claimed: Update clamps the
// ranges so they never overlap, so no later claim of the same Update takes one
// back. Until the ownership map or the render target set changes, which bumps
// the cache's generation, claiming the same keys again with ranges no longer
// than before finds every tile already owned: GetOrCreateRenderTarget would
// return the remembered targets and ChangeOwnership would change nothing and
// append no transfer. Command processor thread only.
#pragma once

#include <cstdint>

namespace nb::gpu {

// Slot 0 is depth / stencil and 1-4 are color, as in RenderTargetCache::Update.
inline constexpr uint32_t kNativeRtOwnershipSlots = 5;

// Key needs a uint32_t `key` holding all of its fields (RenderTargetKey's union).
template <typename Key, typename Target>
class NativeRtOwnershipMemo {
 public:
  // constinit-capable, so a function-local static needs no initialization guard.
  constexpr NativeRtOwnershipMemo() = default;

  // keys are indexed by slot and read only for used slots; lengths_tiles are
  // indexed by position in the EDRAM-base order the claims are made in, which
  // equal used slots and keys make identical.
  bool Matches(const void* owner, uint64_t generation, uint32_t used_bits, const Key* keys,
               const uint32_t* lengths_tiles, uint32_t count) const {
    if (!owner_ || owner != owner_ || generation != generation_ || used_bits != used_bits_ ||
        count != count_) {
      return false;
    }
    for (uint32_t i = 0; i < kNativeRtOwnershipSlots; ++i) {
      if (((used_bits >> i) & 1) && keys[i].key != keys_[i]) {
        return false;
      }
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (lengths_tiles[i] > lengths_tiles_[i]) {
        return false;
      }
    }
    return true;
  }

  // The targets of the published claim, nullptr in unused slots.
  void CopyTargets(Target** targets_out) const {
    for (uint32_t i = 0; i < kNativeRtOwnershipSlots; ++i) {
      targets_out[i] = targets_[i];
    }
  }

  // Call only after a claim succeeded, with the generation read after its last
  // ownership change. targets are read only for used slots.
  void Publish(const void* owner, uint64_t generation, uint32_t used_bits, const Key* keys,
               const uint32_t* lengths_tiles, uint32_t count, Target* const* targets) {
    owner_ = owner;
    generation_ = generation;
    used_bits_ = used_bits;
    count_ = count;
    for (uint32_t i = 0; i < kNativeRtOwnershipSlots; ++i) {
      const bool used = (used_bits >> i) & 1;
      keys_[i] = used ? keys[i].key : 0;
      targets_[i] = used ? targets[i] : nullptr;
      lengths_tiles_[i] = i < count ? lengths_tiles[i] : 0;
    }
  }

 private:
  const void* owner_ = nullptr;
  uint64_t generation_ = 0;
  uint32_t used_bits_ = 0;
  uint32_t count_ = 0;
  uint32_t keys_[kNativeRtOwnershipSlots] = {};
  uint32_t lengths_tiles_[kNativeRtOwnershipSlots] = {};
  Target* targets_[kNativeRtOwnershipSlots] = {};
};

}  // namespace nb::gpu
