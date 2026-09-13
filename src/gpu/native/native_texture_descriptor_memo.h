// Original one-entry lookup policy for one live texture's append-only SRV map.
// The owning texture destroys this identity before its descriptors can be reused.
#pragma once

#include <cstdint>

namespace nb::gpu {

struct NativeTextureDescriptorMemoStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t publications = 0;
  uint64_t failed_lookups = 0;
};

class NativeTextureDescriptorMemo {
 public:
  bool Find(uint32_t key, uint32_t& index, NativeTextureDescriptorMemoStats& stats) const {
    if (index_ != UINT32_MAX && key_ == key) {
      index = index_;
      ++stats.hits;
      return true;
    }
    ++stats.misses;
    return false;
  }
  // Publish the value actually stored by the map, including an existing value
  // when emplace rejects a duplicate key. A failed lookup/creation is not cached.
  void Publish(uint32_t key, uint32_t index, NativeTextureDescriptorMemoStats& stats) {
    if (index == UINT32_MAX) return;
    key_ = key;
    index_ = index;
    ++stats.publications;
  }
  void Reset() { index_ = UINT32_MAX; }

 private:
  uint32_t key_ = 0;
  uint32_t index_ = UINT32_MAX;
};

// Cumulative diagnostic counters, written/read on the GPU command thread.
const NativeTextureDescriptorMemoStats& GetNativeTextureDescriptorMemoStats();

}  // namespace nb::gpu
