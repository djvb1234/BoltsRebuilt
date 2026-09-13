#include "native_upload_shadow.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace nb::gpu {

bool NativeUploadShadow::Initialize(uint32_t page_bytes, size_t capacity_bytes) noexcept {
  const bool exhausted_epoch = epoch_ == std::numeric_limits<uint64_t>::max();
  AdvanceEpoch();
  page_bytes_ = 0;
  bytes_.reset();
  page_to_slot_.clear();
  slots_.clear();
  if (exhausted_epoch || page_bytes < 4096 || (page_bytes & (page_bytes - 1)) ||
      page_bytes > kSnapshotBytes || kSnapshotBytes % page_bytes ||
      kPhysicalBytes % page_bytes || capacity_bytes < page_bytes ||
      capacity_bytes % page_bytes || capacity_bytes > kPhysicalBytes) return false;
  try {
    // Deliberately uninitialized: no read occurs before a complete publication.
    bytes_.reset(new uint8_t[capacity_bytes]);
    page_to_slot_.assign(kPhysicalBytes / page_bytes, kAbsent);
    slots_.resize(capacity_bytes / page_bytes);
  } catch (...) {
    bytes_.reset();
    page_to_slot_.clear();
    slots_.clear();
    return false;
  }
  page_bytes_ = page_bytes;
  next_slot_ = 0;
  evictions_ = 0;
  return true;
}

void NativeUploadShadow::AdvanceEpoch() noexcept {
  if (epoch_ == std::numeric_limits<uint64_t>::max()) {
    // Fail closed instead of allowing an old publication token to wrap around.
    page_bytes_ = 0;
  } else {
    ++epoch_;
  }
}

void NativeUploadShadow::Reset() noexcept {
  AdvanceEpoch();
  std::fill(page_to_slot_.begin(), page_to_slot_.end(), kAbsent);
  for (auto& slot : slots_) slot = {};
  next_slot_ = 0;
}

void NativeUploadShadow::Invalidate(uint32_t first, uint32_t last) noexcept {
  AdvanceEpoch();
  if (!initialized() || first > last || first >= kPhysicalBytes) return;
  last = std::min(last, kPhysicalBytes - 1);
  const uint32_t page_last = last / page_bytes_;
  for (uint32_t page = first / page_bytes_; page <= page_last; ++page) {
    const uint32_t slot = page_to_slot_[page];
    if (slot != kAbsent && slots_[slot].page == page) slots_[slot].valid = false;
  }
}

bool NativeUploadShadow::GetPage(uint32_t address, size_t size, uint32_t& page) const noexcept {
  if (!initialized() || size != page_bytes_ || address % page_bytes_ ||
      address >= kPhysicalBytes || size > kPhysicalBytes - address) return false;
  page = address / page_bytes_;
  return true;
}

bool NativeUploadShadow::Matches(uint32_t address, std::span<const uint8_t> bytes) const noexcept {
  uint32_t page;
  if (!GetPage(address, bytes.size(), page)) return false;
  const uint32_t slot = page_to_slot_[page];
  return slot != kAbsent && slots_[slot].valid && slots_[slot].page == page &&
         std::memcmp(bytes.data(), bytes_.get() + size_t(slot) * page_bytes_, page_bytes_) == 0;
}

bool NativeUploadShadow::Publish(uint32_t address, std::span<const uint8_t> bytes,
                                 uint64_t captured_epoch) noexcept {
  uint32_t page;
  if (captured_epoch != epoch_ || !GetPage(address, bytes.size(), page)) return false;
  uint32_t slot = page_to_slot_[page];
  if (slot == kAbsent || slots_[slot].page != page) {
    slot = next_slot_;
    if (++next_slot_ == slots_.size()) next_slot_ = 0;
    const uint32_t previous_page = slots_[slot].page;
    if (previous_page != kAbsent && page_to_slot_[previous_page] == slot) {
      page_to_slot_[previous_page] = kAbsent;
      ++evictions_;
    }
    page_to_slot_[page] = slot;
    slots_[slot].page = page;
  }
  slots_[slot].valid = false;
  std::memcpy(bytes_.get() + size_t(slot) * page_bytes_, bytes.data(), page_bytes_);
  slots_[slot].valid = true;
  return true;
}

}  // namespace nb::gpu
