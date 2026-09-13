// Original read-only proof for the SDK PhysicalHeap watch-enable loop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nb::runtime {

// Caller retains the original clipping, assertions, notifications-only mode
// gate, and global critical-region lock. Use the actual flag vector for EACH
// physical alias. The lock and initialized heap lifetime must cover this call.
// FlagBlocks provides size() and const operator[](size_t), with a uint64_t
// notify_on_invalidation member; neither operation may throw or mutate state.
// False means run the original loop, including its OOB logging/assertions.
// This neither changes flags/protection nor proves a previous Protect succeeded.
template <class FlagBlocks>
[[nodiscard]] bool AllNotificationPagesWatched(
    uint32_t first, uint32_t last, uint32_t system_page_size,
    uint32_t system_page_count, uint32_t guest_page_shift,
    uint32_t host_address_offset, size_t guest_page_table_size,
    const FlagBlocks& flag_blocks) noexcept {
  if (first > last || !system_page_size || last >= system_page_count ||
      guest_page_shift >= 32) {
    return false;
  }

  // The original loop multiplies in uint32_t. Widen BEFORE multiplication to
  // exclude wrap; then multiplication, saturating subtraction, and shifting are
  // monotone over the whole interval, so its last guest index bounds every one.
  const uint64_t last_offset_wide = uint64_t(last) * system_page_size;
  if (last_offset_wide > UINT32_MAX) return false;
  const uint32_t last_offset = uint32_t(last_offset_wide);
  const uint32_t last_guest =
      (last_offset > host_address_offset ? last_offset - host_address_offset : 0)
      >> guest_page_shift;
  if (size_t(last_guest) >= guest_page_table_size) return false;

  const size_t first_block = size_t(first >> 6);
  const size_t last_block = size_t(last >> 6);
  if (last_block >= flag_blocks.size()) return false;
  for (size_t block = first_block;; ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == first_block) mask &= UINT64_MAX << (first & 63);
    if (block == last_block) mask &= UINT64_MAX >> (63 - (last & 63));
    if ((flag_blocks[block].notify_on_invalidation & mask) != mask) return false;
    if (block == last_block) return true;
    // last_block <= UINT32_MAX/64, so increment cannot overflow size_t.
  }
}

// Separate no-effect proof for notifications-only calls on coarse guest pages.
// Retain the same caller lock, clipping, assertions, and mode gate as above.
// is_immutable(index) must be a pure noexcept bool predicate over the current
// guest page table: true iff ToPageAccess(current_protect) != kReadWrite.
// Validate all original flag/table indices before invoking that predicate, so
// malformed input falls through to the original diagnostics rather than hiding
// them. No flag word is read or changed here, even if its watch bit is stale.
template <class FlagBlocks, class GuestPageImmutable>
[[nodiscard]] bool AllGuestPagesImmutable(
    uint32_t first, uint32_t last, uint32_t system_page_size,
    uint32_t system_page_count, uint32_t guest_page_shift,
    uint32_t host_address_offset, size_t guest_page_table_size,
    const FlagBlocks& flag_blocks, GuestPageImmutable&& is_immutable) noexcept {
  static_assert(std::is_nothrow_invocable_r_v<bool, GuestPageImmutable&, size_t>,
                "Guest-page predicate must return bool without throwing");
  if (first > last || !system_page_size || last >= system_page_count ||
      guest_page_shift >= 32 ||
      (uint64_t(1) << guest_page_shift) <= system_page_size) {
    return false;
  }
  const uint64_t last_offset_wide = uint64_t(last) * system_page_size;
  if (last_offset_wide > UINT32_MAX) return false;
  const uint32_t last_offset = uint32_t(last_offset_wide);
  const uint32_t last_guest =
      (last_offset > host_address_offset ? last_offset - host_address_offset : 0)
      >> guest_page_shift;
  if (size_t(last_guest) >= guest_page_table_size ||
      size_t(last >> 6) >= flag_blocks.size()) {
    return false;
  }
  // The widened last-product proof makes the original uint32 multiplication
  // nonwrapping for every earlier page. With host bytes < guest bytes, saturated
  // subtraction followed by division can advance the guest index by at most
  // one per host page; this interval contains exactly the distinct indices.
  const uint32_t first_offset = uint32_t(uint64_t(first) * system_page_size);
  const uint32_t first_guest =
      (first_offset > host_address_offset ? first_offset - host_address_offset : 0)
      >> guest_page_shift;
  for (size_t guest = first_guest;; ++guest) {
    if (!is_immutable(guest)) return false;
    if (guest == size_t(last_guest)) return true;
    // last_guest <= UINT32_MAX and is reached before any increment past it.
  }
}

}  // namespace nb::runtime
