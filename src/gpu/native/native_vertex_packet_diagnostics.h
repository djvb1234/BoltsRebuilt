// Original count-only observer for complete native VS constant packets.
// Owns ordinary RAM only: it never stores or reuses a GPU address.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace nb::gpu {
class NativeVertexPacketDiagnostics {
 public:
  static constexpr size_t kCapacity = 688 + 256 * 16;
  struct Result {
    bool observed = false;
    bool hit = false;
    bool new_pass_frame = false;
    uint64_t equal_bytes = 0;
  };

  // The caller proves this is the complete initialized packet used by a
  // successful draw. Length and every byte participate, including unused lanes.
  Result Observe(uint64_t frame, std::span<const uint8_t> bytes) noexcept {
    if (bytes.empty() || bytes.size() > bytes_.size()) {
      Invalidate();
      return {};
    }
    const bool hit = valid_ && frame == frame_ && bytes.size() == size_ &&
                     std::memcmp(bytes_.data(), bytes.data(), size_) == 0;
    const bool new_frame = !frame_seen_ || frame != frame_;
    if (!hit) std::memcpy(bytes_.data(), bytes.data(), bytes.size());
    size_ = bytes.size();
    frame_ = frame;
    frame_seen_ = true;
    valid_ = true;
    return {true, hit, new_frame, hit ? uint64_t(bytes.size()) : 0};
  }
  void Invalidate() noexcept { valid_ = false; }
  void Reset() noexcept { valid_ = false; frame_seen_ = false; }

  // A failed or unobservable Record must not leave the prior packet eligible
  // for a later comparison. Destruction handles every early return, including
  // exceptions, without adding another return-path checklist to Record.
  class Attempt {
   public:
    Attempt(NativeVertexPacketDiagnostics& owner, bool enabled, bool eligible,
            uint64_t& bypasses) noexcept
        : owner_(owner), enabled_(enabled), eligible_(eligible), bypasses_(bypasses) {
      if (!enabled || !eligible) owner_.Invalidate();
    }
    ~Attempt() {
      if (!observed_) {
        owner_.Invalidate();
        if (enabled_) ++bypasses_;
      }
    }
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;
    Result Complete(uint64_t frame, std::span<const uint8_t> bytes) noexcept {
      if (!enabled_ || !eligible_) return {};
      const Result result = owner_.Observe(frame, bytes);
      observed_ = result.observed;
      return result;
    }

   private:
    NativeVertexPacketDiagnostics& owner_;
    bool enabled_, eligible_, observed_ = false;
    uint64_t& bypasses_;
  };

 private:
  std::array<uint8_t, kCapacity> bytes_;  // Never clear/copy this while disabled.
  size_t size_ = 0;
  uint64_t frame_ = 0;
  bool valid_ = false, frame_seen_ = false;
};
}  // namespace nb::gpu
