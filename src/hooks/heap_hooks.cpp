// Guest-side heap hooks (config/hooks.toml documents each site).

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "generated/default/nb_init.h"

#include "pool_trace_hooks.h"

namespace {

inline uint32_t LoadU32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, sizeof v);
  return __builtin_bswap32(v);
}

inline uint16_t LoadU16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, base + addr, sizeof v);
  return __builtin_bswap16(v);
}

// One XDK HEAP_ENTRY header, 16 bytes before the user pointer: Size and PrevSize in 16-byte units,
// SegmentIndex, flags (0x1 busy, 0x2 extra, 0x4 fill pattern, 0x8 virtual, 0x10 last), unused bytes,
// then the free-list links at +8 / +12 (only meaningful when the block is free).
struct HeapEntry {
  uint16_t size = 0, prev_size = 0;
  uint8_t segment = 0, flags = 0, unused = 0, pad = 0;
  uint32_t flink = 0, blink = 0;
};

HeapEntry ReadEntry(const uint8_t* base, uint32_t header) {
  HeapEntry e;
  e.size = LoadU16(base, header);
  e.prev_size = LoadU16(base, header + 2);
  e.segment = base[header + 4];
  e.flags = base[header + 5];
  e.unused = base[header + 6];
  e.pad = base[header + 7];
  e.flink = LoadU32(base, header + 8);
  e.blink = LoadU32(base, header + 12);
  return e;
}

// The heap arena lives in the 64 KB-page virtual range; anything else is not a block header we can read.
bool InArena(uint32_t addr) { return addr >= 0x40000000u && addr < 0x7F000000u; }

}  // namespace

// meInternalFree: the owner probe returned -1, so no game heap owns the pointer. The game would
// dereference a null heap descriptor here. Returning true takes the unlock-and-return path.
// Seen about once per four to eight boots around the audio re-init; the block was freed once already
// (double free), so skipping the second free is what a tolerant heap would have done. The dump of the
// header and its two neighbours is the "skip-site forensics" from docs/known_issues.md: it tells which
// object class the pointer belonged to (48-byte pool object, level table, bitmap) and whether the
// neighbour the coalescer would read is already damaged.
bool nb_meInternalFree_skip_unowned(PPCRegister& r3, PPCRegister& r29) {
  if (r3.u32 != 0xFFFFFFFFu) {
    return false;
  }
  static std::atomic<uint32_t> count{0};
  const uint32_t n = ++count;
  if (n <= 16 || (n % 256) == 0) {
    const uint32_t ptr = r29.u32;
    const auto* ctx = rex::runtime::ThreadState::Get()->context();
    const uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
    REXLOG_WARN("nb: meInternalFree(0x{:08X}) found no owning heap; skipping the free [{}] host tid {:#x} guest tid "
                "{:#x} lr 0x{:08X}{}",
                ptr, n, rex::thread::current_thread_id(), rex::system::XThread::GetCurrentThreadId(),
                static_cast<uint32_t>(ctx->lr), nb::hooks::PoolWasTornDown(ptr) ? " (a torn-down pool object)" : "");
    if (InArena(ptr) && ptr >= 16) {
      const uint32_t header = ptr - 16;
      const HeapEntry e = ReadEntry(base, header);
      REXLOG_WARN("nb:   header 0x{:08X}: size={} prev={} seg={} flags=0x{:02X} unused={} flink=0x{:08X} blink=0x{:08X}",
                  header, e.size, e.prev_size, e.segment, e.flags, e.unused, e.flink, e.blink);
      if (e.prev_size != 0 && e.prev_size < 61440) {
        const uint32_t prev = header - e.prev_size * 16u;
        if (InArena(prev)) {
          const HeapEntry p = ReadEntry(base, prev);
          REXLOG_WARN("nb:   prev   0x{:08X}: size={} prev={} flags=0x{:02X} flink=0x{:08X} blink=0x{:08X}", prev, p.size,
                      p.prev_size, p.flags, p.flink, p.blink);
        }
      }
      if (e.size != 0 && e.size < 61440 && !(e.flags & 0x10)) {
        const uint32_t next = header + e.size * 16u;
        if (InArena(next)) {
          const HeapEntry q = ReadEntry(base, next);
          REXLOG_WARN("nb:   next   0x{:08X}: size={} prev={} flags=0x{:02X} flink=0x{:08X} blink=0x{:08X}{}", next,
                      q.size, q.prev_size, q.flags, q.flink, q.blink,
                      (!(q.flags & 1) && (q.blink == 0 || q.flink == 0)) ? "  <- the coalescer would fault here" : "");
        }
      }
    }
  }
  return true;
}
