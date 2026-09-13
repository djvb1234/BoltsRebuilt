// Original bounded table for CPU-thread asset lookups; no SDK or GPU dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

namespace nb::gpu {

// A frame reset changes one generation number rather than freeing map nodes.
// Values from earlier generations are never returned. Probe exhaustion refuses
// storage without evicting any current mapping: the caller keeps its freshly
// validated result and simply validates again on a later lookup.
template <typename Value, size_t Capacity, size_t ProbeLimit = 64,
          typename Generation = uint32_t>
class NativeAssetMappingTable {
  static_assert(Capacity && !(Capacity & (Capacity - 1)));
  static_assert(Capacity <= UINT32_MAX);
  static_assert(ProbeLimit && ProbeLimit <= Capacity);
  static_assert(std::is_unsigned_v<Generation>);
  static_assert(std::numeric_limits<Generation>::digits > 1);
  struct Cell {
    uint64_t key = 0;
    Value value{};
    Generation generation = 0;
  };

 public:
  struct Lookup {
    const Value* value = nullptr;
    uint64_t key = 0;
    uint32_t slot = static_cast<uint32_t>(Capacity);
    uint32_t probes = 0;
    Generation generation = 0;
  };
  NativeAssetMappingTable() : cells_(std::make_unique<Cell[]>(Capacity)) {}

  Lookup Find(uint64_t key) const {
    Lookup result;
    result.key = key;
    result.generation = generation_;
    size_t slot = Bucket(key);
    for (size_t step = 0; step < ProbeLimit; ++step, slot = (slot + 1) & (Capacity - 1)) {
      const Cell& cell = cells_[slot];
      ++result.probes;
      if (cell.generation != generation_) {
        result.slot = static_cast<uint32_t>(slot);
        return result;
      }
      if (cell.key == key) {
        result.slot = static_cast<uint32_t>(slot);
        result.value = &cell.value;
        return result;
      }
    }
    return result;
  }

  bool Store(const Lookup& lookup, const Value& value) {
    if (lookup.slot >= Capacity || lookup.generation != generation_) return false;
    Cell& cell = cells_[lookup.slot];
    if (cell.generation == generation_ && cell.key != lookup.key) return false;
    if (cell.generation != generation_) ++size_;
    cell.key = lookup.key;
    cell.value = value;
    cell.generation = generation_;
    return true;
  }

  // Returns true only when the generation wraps and the tag array is swept.
  bool Clear() {
    size_ = 0;
    generation_ = static_cast<Generation>(generation_ + 1);
    if (generation_) return false;
    for (size_t i = 0; i < Capacity; ++i) cells_[i].generation = 0;
    generation_ = 1;
    return true;
  }
  size_t size() const { return size_; }

 private:
  static size_t Bucket(uint64_t key) {
    // Avalanche both halves: neighboring physical addresses and common buffer
    // lengths must not turn linear probing into an aligned-address cluster.
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ull;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebull;
    key ^= key >> 31;
    return static_cast<size_t>(key) & (Capacity - 1);
  }
  std::unique_ptr<Cell[]> cells_;
  Generation generation_ = 1;
  size_t size_ = 0;
};

}  // namespace nb::gpu
