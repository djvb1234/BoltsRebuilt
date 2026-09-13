// Original command-thread WAIT_REG_MEM helpers: an optional bounded spin that
// re-reads the unchanged predicate instead of sleeping, and a small table of
// blocked-wait targets for diagnostics. Everything here runs on the command
// thread only; nothing is shared with memory-watch or vblank threads.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nb::gpu {

// Spin budget for one WAIT_REG_MEM packet. The original loop sleeps
// wait / 0x100 ms (at least 1 ms) between predicate reads, so a packet whose
// condition becomes true just after a read still sleeps out the rest of that
// millisecond, and the host timer adds a few hundred microseconds more. The
// Xenos command processor re-polls in microseconds. While the budget lasts the
// caller yields and re-reads instead of sleeping, so a condition that becomes
// true inside the budget is seen within one yield. Once it is spent, the
// original sleeps resume: completion is then at most one sleep period after
// the condition holds, the same bound the original loop has, although those
// later reads are not phase-aligned with the original loop's and a particular
// packet can finish up to one sleep period later than it would have. A long
// guest wait costs at most budget_ns of spinning.
class NativeWaitSpinBudget {
 public:
  explicit NativeWaitSpinBudget(uint64_t budget_ns) noexcept : budget_ns_(budget_ns) {}

  // Called after a failed predicate read, with the monotonic time of that read.
  bool ShouldSpin(uint64_t now_ns) noexcept {
    if (!budget_ns_ || exhausted_) return false;
    if (!started_) {
      started_ = true;
      start_ns_ = now_ns;
      return true;
    }
    // A clock that goes backwards cannot bound the spin, so stop spinning.
    if (now_ns < start_ns_ || now_ns - start_ns_ >= budget_ns_) {
      exhausted_ = true;
      return false;
    }
    return true;
  }
  bool started() const noexcept { return started_; }
  bool exhausted() const noexcept { return exhausted_; }

 private:
  uint64_t budget_ns_;
  uint64_t start_ns_ = 0;
  bool started_ = false;
  bool exhausted_ = false;
};

// Always-on counters, so a benchmark window can prove the spin actually ran
// (or did not). Counted per packet, not per predicate read.
struct NativeWaitSpinStats {
  uint64_t spinning_packets = 0;        // packets that spun at least once
  uint64_t yields = 0;                  // yields issued in place of sleeps
  uint64_t matched_while_spinning = 0;  // predicate became true inside the budget
  uint64_t budget_exhausted = 0;        // fell back to the original sleeps
};
inline NativeWaitSpinStats& GetNativeWaitSpinStats() noexcept {
  static NativeWaitSpinStats stats;
  return stats;
}

// One blocked-wait target: the polled register or memory word, the compare
// function and the mask. The reference value is not part of the key because
// counters such as a vblank count change it every frame.
struct NativeWaitTarget {
  bool used = false;
  bool is_memory = false;
  uint32_t address = 0;   // poll_reg_addr as encoded: register index, or memory address with endian bits
  uint32_t function = 0;  // wait_info & 7
  uint32_t mask = 0;
  uint64_t packets = 0;
  uint64_t blocked_ns = 0;
  uint64_t max_ns = 0;
  uint64_t vblank_advanced = 0;      // the vblank counter moved between first failure and match
  uint64_t timed_advances = 0;       // packets whose first vblank advance was observed while spinning
  uint64_t advance_to_match_ns = 0;  // summed time from that observation to the match
  uint64_t matched_within_100us = 0;
  uint32_t last_ref = 0, last_first_value = 0, last_match_value = 0;
};

struct NativeWaitSample {
  bool is_memory;
  uint32_t address, function, mask, ref;
  uint32_t first_value, match_value;
  uint64_t blocked_ns;
  bool vblank_advanced;
  bool advance_timed;             // an advance was seen by a read made while spinning
  uint64_t advance_to_match_ns;   // meaningful only when advance_timed
};

class NativeWaitTargetTable {
 public:
  static constexpr size_t kCapacity = 8;

  void Record(const NativeWaitSample& s) noexcept {
    NativeWaitTarget* slot = nullptr;
    for (NativeWaitTarget& entry : entries_) {
      if (entry.used && entry.is_memory == s.is_memory && entry.address == s.address &&
          entry.function == s.function && entry.mask == s.mask) {
        slot = &entry;
        break;
      }
      if (!entry.used && !slot) slot = &entry;
    }
    // The first loop may have picked a free slot before finding a later match;
    // matches are always inserted before free slots, so a free slot means new.
    if (!slot) {
      ++untracked_;
      return;
    }
    if (!slot->used) {
      slot->used = true;
      slot->is_memory = s.is_memory;
      slot->address = s.address;
      slot->function = s.function;
      slot->mask = s.mask;
    }
    ++slot->packets;
    slot->blocked_ns += s.blocked_ns;
    if (s.blocked_ns > slot->max_ns) slot->max_ns = s.blocked_ns;
    if (s.vblank_advanced) ++slot->vblank_advanced;
    if (s.advance_timed) {
      ++slot->timed_advances;
      slot->advance_to_match_ns += s.advance_to_match_ns;
      if (s.advance_to_match_ns <= 100000) ++slot->matched_within_100us;
    }
    slot->last_ref = s.ref;
    slot->last_first_value = s.first_value;
    slot->last_match_value = s.match_value;
  }
  const NativeWaitTarget* begin() const noexcept { return entries_; }
  const NativeWaitTarget* end() const noexcept { return entries_ + kCapacity; }
  uint64_t untracked() const noexcept { return untracked_; }

 private:
  NativeWaitTarget entries_[kCapacity];
  uint64_t untracked_ = 0;
};
inline NativeWaitTargetTable& GetNativeWaitTargetTable() noexcept {
  static NativeWaitTargetTable table;
  return table;
}

}  // namespace nb::gpu
