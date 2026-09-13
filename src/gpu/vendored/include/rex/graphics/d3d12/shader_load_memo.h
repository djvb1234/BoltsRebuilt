// Original bounded policy for repeated guest shader loads. The owner controls
// Shader lifetimes; this helper retains independent bytes and opaque identities.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rex::graphics::d3d12 {

class ShaderLoadMemo {
 public:
  static constexpr size_t kEntries = 64;
  static constexpr uint32_t kMaxDwords = 2048;
  static constexpr size_t kPayloadBytes = kEntries * kMaxDwords * sizeof(uint32_t);
  static_assert(kPayloadBytes == 512 * 1024);

  struct Stats {
    uint64_t hits = 0, misses = 0, bypasses = 0, content_misses = 0;
    uint64_t bytes_compared = 0, bytes_hash_avoided = 0;  // requested comparison span, not CPU bus traffic
    uint64_t publications = 0, evictions = 0, resets = 0;
  };

  // Stage is Xenos ShaderType: 0 vertex, 1 pixel. Oversized or empty shaders
  // remain eligible for the original loader, but never touch memo byte storage.
  static bool Supports(const void* source, uint32_t stage, uint32_t dwords) {
    return source && stage <= 1 && dwords && dwords <= kMaxDwords;
  }

  uintptr_t Find(const void* live_source, uint32_t stage, uint32_t dwords) {
    if (!Supports(live_source, stage, dwords)) {
      ++stats_.bypasses;
      return 0;
    }
    const Entry& entry = entries_[Slot(live_source, stage, dwords)];
    if (entry.value && entry.source == live_source && entry.stage == stage && entry.dwords == dwords) {
      const size_t bytes = size_t(dwords) * sizeof(uint32_t);
      stats_.bytes_compared += bytes;
      // Address/count/stage only select a candidate. All current bytes must
      // match the immutable Shader snapshot before its identity can be reused.
      if (std::memcmp(live_source, entry.guest_words.data(), bytes) == 0) {
        ++stats_.hits;
        stats_.bytes_hash_avoided += bytes;
        return entry.value;
      }
      ++stats_.content_misses;
    }
    ++stats_.misses;
    return 0;
  }

  // The words MUST come from the returned Shader's immutable host-order ucode,
  // after its type/count have been checked. Do not copy the mutable guest input:
  // it could have changed since the original loader hashed or copied it.
  bool Publish(const void* source, uint32_t stage, uint32_t dwords,
               const uint32_t* shader_host_words, uintptr_t shader) {
    if (!Supports(source, stage, dwords) || !shader_host_words || !shader) return false;
    Entry& entry = entries_[Slot(source, stage, dwords)];
    if (entry.value) ++stats_.evictions;
    entry.value = 0;
    for (uint32_t i = 0; i < dwords; ++i) {
      uint32_t word = shader_host_words[i];
      if constexpr (std::endian::native == std::endian::little) {
        word = ((word & 0x000000FFu) << 24) | ((word & 0x0000FF00u) << 8) |
               ((word & 0x00FF0000u) >> 8) | ((word & 0xFF000000u) >> 24);
      }
      entry.guest_words[i] = word;
    }
    entry.source = source;
    entry.stage = stage;
    entry.dwords = dwords;
    entry.value = shader;
    ++stats_.publications;
    return true;
  }

  // The owner calls this BEFORE deleting shaders or their translations. Bytes
  // need not be erased; value is the validity flag and is checked before them.
  void Reset() {
    for (Entry& entry : entries_) entry.value = 0;
    ++stats_.resets;
  }
  const Stats& stats() const { return stats_; }

 private:
  static size_t Slot(const void* source, uint32_t stage, uint32_t dwords) {
    uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(source)) >> 2;
    key ^= uint64_t(dwords) * UINT64_C(0x9E3779B97F4A7C15);
    key ^= uint64_t(stage) << 32;
    key ^= key >> 33;
    key *= UINT64_C(0xFF51AFD7ED558CCD);
    key ^= key >> 33;
    return size_t(key) & (kEntries - 1);
  }
  struct Entry {
    std::array<uint32_t, kMaxDwords> guest_words;  // initialized only for a published entry's dwords
    const void* source;
    uint32_t stage, dwords;
    uintptr_t value = 0;
  };
  std::array<Entry, kEntries> entries_;
  Stats stats_{};
};

}  // namespace rex::graphics::d3d12
