// Original same-frame identity policy for complete native VS constant packets.
// Both packets are ordinary RAM. GPU addresses are opaque, frame-owned identities.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nb::gpu {
class NativeVertexConstantReuse {
 public:
  static constexpr size_t kCapacity = 688 + 256 * 16;

  // Reacquire before building each complete packet. Publishing swaps ownership;
  // the prior candidate becomes active and must not be modified afterwards.
  uint8_t* candidate_data() noexcept { return packets_[active_ ^ 1u].data(); }

  uint64_t Find(uint64_t frame, size_t size) const noexcept {
    return valid_ && size && size <= kCapacity && frame == frame_ && size == size_ &&
                   std::memcmp(packets_[active_].data(), packets_[active_ ^ 1u].data(), size) == 0
               ? address_ : 0;
  }

  // Call only after allocating a NEW slice, writing the complete initialized
  // candidate and establishing its current-frame GPU lifetime. Do not publish
  // a failed allocation or mutate the upload slice while it may be in flight.
  bool Publish(uint64_t frame, size_t size, uint64_t address) noexcept {
    if (!address || !size || size > kCapacity) return false;
    active_ ^= 1u;
    frame_ = frame;
    size_ = size;
    address_ = address;
    valid_ = true;
    return true;
  }
  void Reset() noexcept { valid_ = false; }

 private:
  std::array<std::array<uint8_t, kCapacity>, 2> packets_;
  uint32_t active_ = 0;
  uint64_t frame_ = 0, address_ = 0;
  size_t size_ = 0;
  bool valid_ = false;
};
}  // namespace nb::gpu
