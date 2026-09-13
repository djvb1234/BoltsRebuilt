// One exact shader-pair lookup, owned by the library that owns the Pair objects.
// This cache does not decide readiness, filtering, or semantic hash agreement.
#pragma once

#include <cstdint>
#include <utility>

namespace nb::gpu {

struct NativeReadyPairMemoStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t publications = 0;
  uint64_t resets = 0;
};

template <typename Pair>
class NativeReadyPairMemo {
 public:
  NativeReadyPairMemo() = default;
  NativeReadyPairMemo(const NativeReadyPairMemo&) = delete;
  NativeReadyPairMemo& operator=(const NativeReadyPairMemo&) = delete;
  // Moving the owning library transfers its heap-owned Pair objects. Leave no
  // borrowed pointer in the moved-from library, which remains a valid object.
  NativeReadyPairMemo(NativeReadyPairMemo&& other) noexcept
      : entry_(std::exchange(other.entry_, {})), stats_(other.stats_) {}
  NativeReadyPairMemo& operator=(NativeReadyPairMemo&& other) noexcept {
    if (this != &other) {
      entry_ = std::exchange(other.entry_, {});
      stats_ = other.stats_;
    }
    return *this;
  }

  // The caller must process finished compiles before lookup, then recheck the
  // returned Pair's full shader hashes, usability, and current pair filters.
  Pair* Find(uint64_t vertex_hash, uint64_t pixel_hash) {
    if (entry_.pair && entry_.vertex_hash == vertex_hash && entry_.pixel_hash == pixel_hash) {
      ++stats_.hits;
      return entry_.pair;
    }
    ++stats_.misses;
    return nullptr;
  }

  // Publish only after the owning library has accepted a ready result.
  void Publish(uint64_t vertex_hash, uint64_t pixel_hash, Pair* pair) {
    if (!pair) return;
    entry_ = {vertex_hash, pixel_hash, pair};
    ++stats_.publications;
  }

  // Required before deleting/replacing Pair objects, including a failed Load.
  void Reset() {
    entry_ = {};
    ++stats_.resets;
  }
  const NativeReadyPairMemoStats& stats() const { return stats_; }

 private:
  struct Entry {
    uint64_t vertex_hash = 0;
    uint64_t pixel_hash = 0;
    Pair* pair = nullptr;
  };
  Entry entry_;
  NativeReadyPairMemoStats stats_;
};

}  // namespace nb::gpu
