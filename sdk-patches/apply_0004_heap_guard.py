"""Apply sdk-patches/0004 (rexcrt heap free-validation guard) to an SDK checkout.

Why: twice in ~18 boots the game died inside o1heapFree (o1heap.c ~500, a write to 0x3C)
while freeing a block whose o1heap FragmentHeader held garbage, reached from the game's
meInternalFree through the rexcrt RtlFreeHeap hook. o1heap validates nothing in release
builds, so a foreign pointer, a double free or an underflow into the 16 bytes before a block
corrupts its bins and crashes later. This guard stamps every live allocation, checks the stamp
and o1heap's own header before freeing, and turns a bad free into a rate-limited warning with
the guest return address instead of a crash.

Usage: python apply_0004_heap_guard.py <sdk_root>
Idempotent: refuses to run twice.
"""
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1])
path = sdk / "src" / "kernel" / "crt" / "heap.cpp"
s = path.read_text(encoding="utf-8")
if "kLiveMagic" in s:
    print("already applied"); sys.exit(0)


def rep(old, new):
    global s
    assert s.count(old) == 1, (s.count(old), old[:80])
    s = s.replace(old, new)


rep("#include <o1heap.h>\n",
    "#include <o1heap.h>\n"
    "\n"
    "#include <rex/system/thread_state.h>\n")

rep("#ifndef HEAP_ZERO_MEMORY\nconstexpr uint32_t HEAP_ZERO_MEMORY = 0x00000008;\n#endif\n\n}  // namespace\n",
    '''#ifndef HEAP_ZERO_MEMORY
constexpr uint32_t HEAP_ZERO_MEMORY = 0x00000008;
#endif

// nb patch 0004: free-validation guard.
// Every live allocation carries kLiveMagic in SizeHeader::reserved; Free() flips it to
// kFreedMagic. o1heap validates nothing in release builds, so a foreign pointer, a double
// free or an underflow into the sixteen bytes before a block corrupts its bins and crashes
// later (seen as a write to 0x3C in o1heapFree, reached from the game's meInternalFree).
// A free that fails the checks is logged with the guest return address and skipped.
constexpr uint64_t kLiveMagic = 0x2150414548584552ull;   // "REXHEAP!"
constexpr uint64_t kFreedMagic = 0x2144454552465845ull;  // "EXFREED!"

// o1heap's private FragmentHeader (o1heap.c): {Fragment* next; uintptr_t prev_used;},
// O1HEAP_ALIGNMENT bytes immediately before the payload o1heap hands out.
struct O1FragmentHeaderView {
  uintptr_t next;
  uintptr_t prev_used;
};

bool FragmentHeaderLooksSane(const void* payload, uintptr_t arena_lo, uintptr_t arena_hi) {
  auto* fh = reinterpret_cast<const O1FragmentHeaderView*>(
      static_cast<const uint8_t*>(payload) - O1HEAP_ALIGNMENT);
  auto in_arena = [&](uintptr_t p) {
    return p == 0 || (p >= arena_lo && p < arena_hi && (p % O1HEAP_ALIGNMENT) == 0);
  };
  if ((fh->prev_used & 1u) == 0) {
    return false;  // o1heap already considers this fragment free
  }
  return in_arena(fh->next) && in_arena(fh->prev_used & ~uintptr_t{1});
}

uint32_t GuestReturnAddress() {
  auto* ts = rex::runtime::ThreadState::Get();
  return ts ? static_cast<uint32_t>(ts->context()->lr) : 0;
}

void ReportRejected(const char* op, uint32_t guest_addr, uint64_t requested_size, uint64_t reserved) {
  static std::atomic<uint32_t> count{0};
  const uint32_t n = ++count;
  if (n > 32 && (n % 1024) != 0) {
    return;
  }
  const char* verdict = reserved == kFreedMagic ? "double free" : "not a live allocation";
  REXKRNL_WARN("rexcrt_{}: rejected 0x{:08X} ({}; header size={} tag=0x{:016X}) from guest lr 0x{:08X} [{}]",
               op, guest_addr, verdict, requested_size, reserved, GuestReturnAddress(), n);
}

}  // namespace
''')

rep("  void* real_host = GuestToHost(guest_addr - kHeaderSize);\n  o1heapFree(segment->heap, real_host);\n}\n",
    '''  void* real_host = GuestToHost(guest_addr - kHeaderSize);
  auto* hdr = static_cast<SizeHeader*>(real_host);
  const auto arena_lo = reinterpret_cast<uintptr_t>(GuestToHost(segment->guest_base));
  const auto arena_hi = reinterpret_cast<uintptr_t>(GuestToHost(segment->guest_end));
  if (hdr->reserved != kLiveMagic || !FragmentHeaderLooksSane(real_host, arena_lo, arena_hi)) {
    ReportRejected("RtlFreeHeap", guest_addr, hdr->requested_size, hdr->reserved);
    return;
  }
  hdr->reserved = kFreedMagic;
  o1heapFree(segment->heap, real_host);
}
''')

rep("  auto* hdr = static_cast<SizeHeader*>(GuestToHost(guest_addr - kHeaderSize));\n  return static_cast<uint32_t>(hdr->requested_size);\n}\n",
    '''  auto* hdr = static_cast<SizeHeader*>(GuestToHost(guest_addr - kHeaderSize));
  if (hdr->reserved != kLiveMagic) {
    ReportRejected("RtlSizeHeap", guest_addr, hdr->requested_size, hdr->reserved);
    return ~0u;
  }
  return static_cast<uint32_t>(hdr->requested_size);
}
''')

rep("  auto* old_hdr = static_cast<SizeHeader*>(real_host);\n  uint32_t old_size = static_cast<uint32_t>(old_hdr->requested_size);\n",
    '''  auto* old_hdr = static_cast<SizeHeader*>(real_host);
  if (old_hdr->reserved != kLiveMagic) {
    ReportRejected("RtlReAllocateHeap", guest_addr, old_hdr->requested_size, old_hdr->reserved);
    return AllocLocked(new_size, zero_new);
  }
  uint32_t old_size = static_cast<uint32_t>(old_hdr->requested_size);
''')

rep("    o1heapFree(segment->heap, real_host);\n    return new_guest;\n",
    "    old_hdr->reserved = kFreedMagic;\n    o1heapFree(segment->heap, real_host);\n    return new_guest;\n")

rep("  auto* new_hdr = static_cast<SizeHeader*>(new_ptr);\n  new_hdr->requested_size = new_size;\n",
    "  auto* new_hdr = static_cast<SizeHeader*>(new_ptr);\n  new_hdr->requested_size = new_size;\n  new_hdr->reserved = kLiveMagic;\n")

rep("    hdr->requested_size = size;\n    hdr->reserved = 0;\n",
    "    hdr->requested_size = size;\n    hdr->reserved = kLiveMagic;\n")

if "#include <atomic>" not in s:
    rep("#include <o1heap.h>\n", "#include <atomic>\n\n#include <o1heap.h>\n")

path.write_text(s, encoding="utf-8", newline="\n")
print("patched", path)
