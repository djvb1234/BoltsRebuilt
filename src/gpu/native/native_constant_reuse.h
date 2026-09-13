// Original, portable exact-byte policies for native pixel constant uploads and
// repeated ready pipeline lookups. No GPU or guest-memory ownership lives here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "native_constant_layout.h"

namespace nb::gpu {

class NativePixelConstantPacket {
 public:
  static constexpr size_t kBoolBytes = 32;
  static constexpr size_t kSlotBytes = 80;
  static constexpr size_t kHeaderBytes = kBoolBytes + 2 * kSlotBytes;
  static constexpr size_t kCapacity = kHeaderBytes + 256 * 16;

  // packed == 0 retains the handwritten/legacy256 contract unless the caller
  // supplies the separately proven explicit-empty layout.
  // A run has first/count fields in float4 registers. Refuse caching any layout
  // that cannot prove every byte in this bounded packet was initialized.
  template <typename Run>
  bool Build(const uint32_t* bools, const uint32_t* swizzles, const uint32_t* samplers,
             const float* source, const Run* runs, size_t run_count, uint32_t packed,
             bool explicit_empty = false) {
    size_ = 0;
    if (explicit_empty && (packed || run_count)) return false;
    const size_t registers = NativeConstantRegisters(packed, explicit_empty);
    if (registers > 256 || !swizzles || !samplers) return false;
    if (source && runs && run_count) {
      size_t total = 0;
      for (size_t i = 0; i < run_count; ++i) {
        const size_t first = runs[i].first, count = runs[i].count;
        if (first > 256 || count > 256 - first || total > registers ||
            count > registers - total) return false;
        total += count;
      }
      if (total != registers) return false;
    }
    if (bools) std::memcpy(bytes_.data(), bools, kBoolBytes);
    else std::memset(bytes_.data(), 0, kBoolBytes);
    std::memcpy(bytes_.data() + kBoolBytes, swizzles, kSlotBytes);
    std::memcpy(bytes_.data() + kBoolBytes + kSlotBytes, samplers, kSlotBytes);
    auto* destination = bytes_.data() + kHeaderBytes;
    const size_t float_bytes = registers * 16;
    if (!float_bytes) { /* Explicit-empty metadata: never touch the float source. */ }
    else if (!source) std::memset(destination, 0, float_bytes);
    else if (!runs || !run_count) std::memcpy(destination, source, float_bytes);
    else {
      for (size_t i = 0; i < run_count; ++i) {
        const size_t bytes = size_t(runs[i].count) * 16;
        std::memcpy(destination, reinterpret_cast<const uint8_t*>(source) + size_t(runs[i].first) * 16, bytes);
        destination += bytes;
      }
    }
    size_ = kHeaderBytes + float_bytes;
    return true;
  }
  std::span<const uint8_t> bytes() const { return {bytes_.data(), size_}; }

 private:
  friend class NativePixelConstantReuse;
  bool Assign(std::span<const uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > bytes_.size()) return false;
    if (bytes.data() != bytes_.data()) std::memcpy(bytes_.data(), bytes.data(), bytes.size());
    size_ = bytes.size();
    return true;
  }
  // Filled only through Build or validated cache publication. Avoid zero-filling
  // 4 KiB of packet storage on every draw.
  std::array<uint8_t, kCapacity> bytes_;
  size_t size_ = 0;
};

class NativePixelConstantReuse {
 public:
  uint64_t Find(uint64_t frame, std::span<const uint8_t> bytes) const {
    const auto active = packets_[active_].bytes();
    return valid_ && frame == frame_ && bytes.size() == active.size() &&
                   std::memcmp(active.data(), bytes.data(), active.size()) == 0
               ? address_ : 0;
  }
  // Call only after allocating and fully writing a NEW immutable upload slice.
  // Failure leaves the old exact mapping intact; no failed address is published.
  bool Publish(uint64_t frame, std::span<const uint8_t> bytes, uint64_t address) {
    if (!address || !packets_[active_].Assign(bytes)) return false;
    frame_ = frame; address_ = address; valid_ = true; candidate_valid_ = false;
    return true;
  }

  // Build into owned ordinary RAM distinct from the last published snapshot.
  // Rebuilding or failing a candidate never changes the active upload identity.
  template <typename Run>
  bool BuildCandidate(const uint32_t* bools, const uint32_t* swizzles, const uint32_t* samplers,
                      const float* source, const Run* runs, size_t run_count, uint32_t packed,
                      bool explicit_empty = false) {
    candidate_valid_ = packets_[active_ ^ 1u].Build(
        bools, swizzles, samplers, source, runs, run_count, packed, explicit_empty);
    return candidate_valid_;
  }
  std::span<const uint8_t> candidate_bytes() const {
    return candidate_valid_ ? packets_[active_ ^ 1u].bytes() : std::span<const uint8_t>{};
  }
  // Call only AFTER copying candidate_bytes() to a newly allocated immutable
  // upload slice. Flip ownership, never copy the packet back into another RAM
  // snapshot. A zero address retains both the old mapping and pending candidate.
  bool PublishCandidate(uint64_t frame, uint64_t address) {
    if (!address || !candidate_valid_) return false;
    active_ ^= 1u;
    frame_ = frame; address_ = address; valid_ = true; candidate_valid_ = false;
    return true;
  }
  void Reset() { valid_ = false; candidate_valid_ = false; }

 private:
  std::array<NativePixelConstantPacket, 2> packets_;
  uint32_t active_ = 0;
  uint64_t frame_ = 0, address_ = 0;
  bool valid_ = false, candidate_valid_ = false;
};

class NativeReadyPipelineReuse {
 public:
  static constexpr size_t kWords = 20;
  uintptr_t Find(const uint32_t (&words)[kWords]) const {
    return value_ && std::memcmp(words_.data(), words, sizeof(words)) == 0 ? value_ : 0;
  }
  // Pending/null results must never become memoized misses.
  void Publish(const uint32_t (&words)[kWords], uintptr_t value) {
    if (!value) return;
    std::memcpy(words_.data(), words, sizeof(words)); value_ = value;
  }
  void Reset() { value_ = 0; }

 private:
  std::array<uint32_t, kWords> words_;
  uintptr_t value_ = 0;
};

inline bool NativeBindingCoversPixelConstants(uint32_t space, uint32_t first, uint32_t count) {
  return space == 0 && (count == 0 || (first <= 2 && count > 2 - first));
}

// The caller proves no PS and no reflected VS b2 binding. Reuse the valid b1
// address to satisfy root binding without allocating an ignored pixel packet.
inline uint64_t NativeDepthOnlyPixelAddress(bool enabled, bool pixel_buffer_unused,
                                          uint64_t vertex_address) {
  return enabled && pixel_buffer_unused ? vertex_address : 0;
}

}  // namespace nb::gpu
