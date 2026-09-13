// Original bounded controls for the actual production proof helper. No SDK,
// game data, compiler-generated model, OS protection, locks, or GPU operations.
#include "../src/runtime/watch_fastpath.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
struct Block {
  uint64_t notify_on_invalidation = 0;
  uint64_t unrelated = 0x98ABCDEF01234567;
};
struct Context {
  uint32_t first = 0, last = 0, system_size = 4096, system_count = 1;
  uint32_t shift = 12, offset = 0;
  size_t guest_count = 1;
};
uint64_t checks = 0, accepted = 0, rejected = 0;
void Check(bool ok, const char* text) {
  ++checks;
  if (!ok) throw std::runtime_error(text);
}
template<class Flags> bool Actual(const Context& c, const Flags& flags) {
  return nb::runtime::AllNotificationPagesWatched(c.first,c.last,c.system_size,
      c.system_count,c.shift,c.offset,c.guest_count,flags);
}
// Independent scalar oracle: inspect EACH page's bounds, product, guest index
// and single bit. No endpoint proof or first/last word-mask implementation.
// With reject_wrap=false, match the SDK uint32 multiplication exactly; accepted
// production results must also be safe under that original arithmetic.
template<class Flags> bool Oracle(const Context& c, const Flags& flags,
                                 bool reject_wrap = true) {
  if (c.first > c.last || !c.system_size || c.shift >= 32) return false;
  for (uint64_t page=c.first; page<=c.last; ++page) {
    if (page >= c.system_count || page / 64 >= flags.size()) return false;
    const uint64_t product = page * uint64_t(c.system_size);
    if (reject_wrap && product > UINT32_MAX) return false;
    const uint32_t original_product = uint32_t(product);
    uint32_t guest = 0;
    if (original_product >= c.offset) guest = original_product - c.offset;
    guest /= uint32_t(1) << c.shift;
    if (size_t(guest) >= c.guest_count) return false;
    if (((flags[size_t(page / 64)].notify_on_invalidation >> (page % 64)) & 1) == 0)
      return false;
  }
  return true;
}
template<class Flags> void Verify(const Context& c, const Flags& flags, bool expected) {
  const bool actual = Actual(c,flags);
  Check(actual == expected,"literal expectation");
  Check(actual == Oracle(c,flags),"independent per-page oracle");
  Check(!actual || Oracle(c,flags,false),"accepted original SDK page lookup unsafe");
  if (actual) ++accepted; else ++rejected;
}
std::vector<Block> Watched(const Context& c) {
  std::vector<Block> flags((size_t(c.system_count)+63)/64);
  for (uint64_t p=c.first;p<=c.last;++p)
    flags[size_t(p/64)].notify_on_invalidation |= uint64_t(1) << (p%64);
  return flags;
}
void LiteralMasksAndMutation() {
  for (auto span : {std::array<uint32_t,2>{0,0},{63,63},{64,64},
                   {0,63},{1,64},{63,126},{7,130}}) {
    Context c{span[0],span[1],4096,span[1]+2,12,0,size_t(span[1])+2};
    auto flags = Watched(c);
    // All out-of-range bits remain zero, including partial first/last words.
    const auto before = flags;
    Verify(c,flags,true);
    for (uint32_t p=c.first;p<=c.last;++p) {
      flags[p/64].notify_on_invalidation &= ~(uint64_t(1) << (p%64));
      Verify(c,flags,false);
      flags[p/64].notify_on_invalidation |= uint64_t(1) << (p%64);
      Verify(c,flags,true);
    }
    for(size_t i=0;i<flags.size();++i) {
      Check(flags[i].notify_on_invalidation == before[i].notify_on_invalidation,
            "helper changed notification flags");
      Check(flags[i].unrelated == before[i].unrelated,"helper changed unrelated flags");
    }
  }
}
// Virtual all-watched vector allows huge size_t/count boundaries without huge
// allocations. Read tracking only observes helper accesses in the controls.
struct VirtualFlags {
  size_t count;
  mutable size_t reads = 0;
  size_t size() const noexcept { return count; }
  Block operator[](size_t index) const noexcept {
    if (index >= count) std::abort();
    ++reads;
    return {UINT64_MAX,0x98ABCDEF01234567};
  }
};
void GuardBounds() {
  const Context base{63,64,4096,65,12,0,65};
  for (unsigned variant=0;variant!=11;++variant) {
    Context c=base;
    VirtualFlags flags{2};
    switch(variant) {
      case 0:c.first=65;break;
      case 1:c.system_size=0;break;
      case 2:c.system_count=0;break;
      case 3:c.system_count=64;break;
      case 4:c.shift=32;break;
      case 5:c.shift=UINT32_MAX;break;
      case 6:c.guest_count=0;break;
      case 7:c.guest_count=64;break;
      case 8:flags.count=0;break;
      case 9:flags.count=1;break;
      case 10:c.system_size=UINT32_MAX;break;
    }
    Check(!Actual(c,flags),"malformed bounds accepted");
    Check(flags.reads==0,"rejected bounds accessed a flag word");
    Verify(c,flags,false);
  }
  for(uint32_t offset:{0u,4095u,4096u,4097u,UINT32_MAX}) {
    Context c{0,3,4096,4,12,offset,4};
    auto flags=Watched(c);
    Verify(c,flags,true);
  }
  // Repeated guest indices (host pages smaller than guest pages), with offsets
  // immediately below, on, and above the last host page's start.
  for(uint32_t offset:{12287u,12288u,12289u}) {
    Context c{0,3,4096,4,14,offset,1};
    Verify(c,Watched(c),true);
  }
  Context high{UINT32_MAX-1,UINT32_MAX-1,1,UINT32_MAX,0,0,size_t(UINT32_MAX)};
  VirtualFlags flags{(size_t(UINT32_MAX)/64)+1};
  Verify(high,flags,true);
  high.first=high.last=UINT32_MAX;
  Check(!Actual(high,flags),"UINT32_MAX last can fit no uint32 count");
  if constexpr (SIZE_MAX > UINT32_MAX) {
    Context c{};
    c.guest_count = size_t(UINT32_MAX)+1;
    VirtualFlags oversized{size_t(UINT32_MAX)+1};
    Verify(c,oversized,true); // Neither size_t bound may be narrowed to uint32.
  }
}
void WrapCounterexample() {
  Context c{0xFFFFF,0x100000,4096,0x100001,12,0,0x2000};
  VirtualFlags flags{(size_t(c.system_count)+63)/64};
  Verify(c,flags,false);
  Check(!Oracle(c,flags,false),"cross-wrap failed to expose earlier original OOB");
  // Deliberately broken endpoint-only guard; the wrapped last product is zero
  // and hides the earlier invalid page. Require that the production rejects it.
  const uint32_t broken_last = c.last * c.system_size;
  Check((broken_last >> c.shift) < c.guest_count && !Actual(c,flags),
        "overflow mutant was not distinguished");
  c.first=c.last;
  Verify(c,flags,false);
  Check(Oracle(c,flags,false),"single wrap should be conservative fallback only");
}
void ModeWrapper() {
  Context c{};
  for(bool enabled:{false,true}) for(bool notify:{false,true}) for(bool provider:{false,true}) {
    VirtualFlags flags{1};
    // The integration owns these gates; the helper intentionally has no modes.
    const bool selected = enabled && notify && !provider && Actual(c,flags);
    Check(selected == (enabled && notify && !provider),"wrapper mode selection");
    Check(flags.reads == (selected?1u:0u),"disabled/provider path read flags");
  }
}
void Randomized() {
  uint32_t state=0x53C0DE;
  const auto random=[&state]() { state^=state<<13;state^=state>>17;state^=state<<5;return state; };
  constexpr uint32_t sizes[]={1,4096,8192,65536};
  constexpr uint32_t shifts[]={0,12,14,24,31};
  for(unsigned i=0;i!=256;++i) {
    Context c;
    c.first=random()%512;c.last=c.first+random()%128;
    c.system_count=c.last+1+random()%64;c.system_size=sizes[random()%4];
    c.shift=shifts[random()%5];c.offset=random()%65537;
    const uint64_t last_byte=uint64_t(c.last)*c.system_size;
    c.guest_count=size_t((last_byte>c.offset?last_byte-c.offset:0)>>c.shift)+1;
    auto flags=Watched(c);
    Verify(c,flags,true);
    const uint32_t missing=c.first+random()%(c.last-c.first+1);
    flags[missing/64].notify_on_invalidation &= ~(uint64_t(1)<<(missing%64));
    Verify(c,flags,false);
  }
}

struct ImmutableProbe {
  const std::vector<uint32_t>& protections;
  size_t calls = 0, previous = 0;
  bool bad_index = false, bad_order = false;
  bool operator()(size_t guest) noexcept {
    if (calls && guest <= previous) bad_order = true;
    previous = guest; ++calls;
    if (guest >= protections.size()) { bad_index = true; return false; }
    // Literal protection truth table: Read=1, Write=2, WriteCombine=8.
    const uint32_t p = protections[guest];
    return !(p & 1) || !(p & 10);
  }
};
template<class Flags> bool ImmutableActual(const Context& c, const Flags& flags,
                                          ImmutableProbe& probe) {
  return nb::runtime::AllGuestPagesImmutable(c.first,c.last,c.system_size,
      c.system_count,c.shift,c.offset,c.guest_count,flags,probe);
}
struct ScalarEffect { bool valid = false, all_immutable = true; size_t changes = 0, distinct = 0; };
// Independent original-loop effect oracle: visit EVERY host page and compute
// whether it would change a notification bit / start a Protect range. The
// immutable shortcut must have zero effects for arbitrary watch-bit patterns.
template<class Flags> ScalarEffect ScalarNoEffect(const Context& c, const Flags& flags,
    const std::vector<uint32_t>& protections, bool reject_wrap = true) {
  ScalarEffect out;
  if (c.first > c.last || !c.system_size || c.shift >= 32) return out;
  std::vector<bool> visited(protections.size());
  for (uint64_t page=c.first;page<=c.last;++page) {
    if (page >= c.system_count || page/64 >= flags.size()) return {};
    const uint64_t wide = page * uint64_t(c.system_size);
    if (reject_wrap && wide > UINT32_MAX) return {};
    uint32_t byte = uint32_t(wide);
    byte = byte < c.offset ? 0 : byte-c.offset;
    const size_t guest = byte / (uint32_t(1) << c.shift);
    if (guest >= c.guest_count || guest >= protections.size()) return {};
    if (!visited[guest]) { visited[guest]=true; ++out.distinct; }
    const uint32_t p = protections[guest];
    bool writable = false;
    if (p & 1) writable = (p & 2) != 0 || (p & 8) != 0;
    if (writable) {
      out.all_immutable = false;
      if (!((flags[size_t(page/64)].notify_on_invalidation >> (page%64)) & 1))
        ++out.changes;
    }
  }
  out.valid = true;
  return out;
}
template<class Flags> void VerifyImmutable(const Context& c, const Flags& flags,
    const std::vector<uint32_t>& protections, bool expected) {
  ImmutableProbe probe{protections};
  const bool actual = ImmutableActual(c,flags,probe);
  const auto scalar = ScalarNoEffect(c,flags,protections);
  const bool coarse = c.shift < 32 && (uint64_t(1)<<c.shift) > c.system_size;
  Check(actual == expected,"immutable literal expectation");
  Check(actual == (coarse && scalar.valid && scalar.all_immutable),"immutable scalar oracle");
  Check(!probe.bad_index && !probe.bad_order,"predicate duplicate/OOB index");
  if (actual) {
    const auto original = ScalarNoEffect(c,flags,protections,false);
    Check(original.valid && original.changes == 0,"accepted shortcut hid SDK effects");
    Check(probe.calls == scalar.distinct,"did not visit each distinct guest once");
  }
}
void ImmutableControls() {
  Context c{0,63,4096,64,16,0,4};
  std::vector<uint32_t> protections{0,1,2,8};
  std::vector<Block> flags(1);
  VerifyImmutable(c,flags,protections,true); // Unallocated / readonly mixed, no watch bits.
  for (uint32_t raw=0;raw!=32;++raw) {
    protections.assign(4,raw);
    VerifyImmutable(c,flags,protections,!(raw&1) || !(raw&10));
  }
  for (uint64_t bits:{uint64_t(0),UINT64_MAX,uint64_t(0x123456789ABCDEF0)}) {
    flags[0].notify_on_invalidation=bits;
    protections={0,1,4,16};
    VerifyImmutable(c,flags,protections,true);
    for (size_t guest=0;guest!=4;++guest) {
      const auto saved=protections[guest]; protections[guest]=9; // Read + WriteCombine.
      VerifyImmutable(c,flags,protections,false); protections[guest]=saved;
    }
    Check(flags[0].notify_on_invalidation == bits,"immutable helper changed flags");
  }
  // Repeated indices, E-style offset and saturating prefix; non-power-of-two
  // host size is included to test the distinct-index argument itself.
  for (uint32_t host:{1u,3000u,4096u}) for(uint32_t offset:{0u,4095u,4096u,4097u,UINT32_MAX}) {
    Context x{0,63,host,64,16,offset,4};
    VerifyImmutable(x,flags,std::vector<uint32_t>(4,1),true);
  }
  Context large_guest{0,4095,4096,4096,24,0,1};
  VirtualFlags zero_read_flags{64};
  VerifyImmutable(large_guest,zero_read_flags,std::vector<uint32_t>(1,0),true);
  Check(zero_read_flags.reads == 0,"immutable proof read flag words");
  Context cross_word{63,64,4096,65,16,0,5};
  VerifyImmutable(cross_word,VirtualFlags{2},std::vector<uint32_t>{3,3,3,0,1},true);
  // Every malformed endpoint rejects before the predicate or flag-word access.
  for(unsigned variant=0;variant!=12;++variant) {
    Context x{63,64,4096,65,16,0,5}; VirtualFlags bounded{2};
    switch(variant) {
      case 0:x.first=65;break; case 1:x.system_size=0;break;
      case 2:x.system_count=64;break; case 3:x.shift=32;break;
      case 4:x.shift=UINT32_MAX;break; case 5:x.guest_count=0;break;
      case 6:x.guest_count=4;break; case 7:bounded.count=1;break;
      case 8:x.system_size=UINT32_MAX;break; case 9:x.shift=12;break;
      case 10:x.shift=11;break; case 11:bounded.count=0;break;
    }
    ImmutableProbe probe{protections};
    Check(!ImmutableActual(x,bounded,probe),"immutable invalid guard accepted");
    Check(probe.calls == 0 && bounded.reads == 0,"immutable invalid guard observed state");
  }
  Context wrap{0xFFFFF,0x100000,4096,0x100001,16,0,1};
  VirtualFlags wide_flags{(size_t(wrap.system_count)+63)/64};
  const std::vector<uint32_t> one{0};
  VerifyImmutable(wrap,wide_flags,one,false);
  Check(!ScalarNoEffect(wrap,wide_flags,one,false).valid,"immutable wrap did not expose old OOB");
  Check(((wrap.last*wrap.system_size)>>wrap.shift) < wrap.guest_count,
        "immutable endpoint-only negative control not exercised");
  wrap.first=wrap.last;
  VerifyImmutable(wrap,wide_flags,one,false);
  Check(ScalarNoEffect(wrap,wide_flags,one,false).valid,"single wrap should only fall back");
  if constexpr (SIZE_MAX > UINT32_MAX) {
    Context x{0,0,1,1,31,0,size_t(UINT32_MAX)+1};
    VirtualFlags huge{size_t(UINT32_MAX)+1};
    VerifyImmutable(x,huge,one,true);
  }
  Context high{UINT32_MAX-1,UINT32_MAX-1,1,UINT32_MAX,31,0,2};
  VirtualFlags high_flags{(size_t(UINT32_MAX)/64)+1};
  const std::vector<uint32_t> high_values{3,1};
  VerifyImmutable(high,high_flags,high_values,true);
  high.first=high.last=UINT32_MAX;
  ImmutableProbe high_probe{high_values};
  Check(!ImmutableActual(high,high_flags,high_probe) && high_probe.calls == 0,
        "immutable maximum last cannot fit uint32 system count");
  for(bool enabled:{false,true}) for(bool notify:{false,true}) for(bool provider:{false,true}) {
    ImmutableProbe probe{one};
    Context x{0,0,4096,1,16,0,1};
    const bool selected=enabled && notify && !provider && ImmutableActual(x,flags,probe);
    Check(selected == (enabled && notify && !provider),"immutable wrapper mode");
    Check(probe.calls == (selected?1u:0u),"immutable disabled/provider predicate read");
  }
  uint32_t state=0x1A11CE;
  const auto random=[&state]() { state^=state<<13;state^=state>>17;state^=state<<5;return state; };
  constexpr uint32_t immutable[]={0,1,2,4,5,8,10,16,17,UINT32_MAX-1};
  for(unsigned fixture=0;fixture!=256;++fixture) {
    Context x{random()%256,0,4096,512,16,random()%8193,33};
    x.last=x.first+random()%128;
    auto random_flags=std::vector<Block>(8);
    for(auto& b:random_flags) b.notify_on_invalidation=(uint64_t(random())<<32)|random();
    std::vector<uint32_t> values(33);
    for(auto& v:values) v=immutable[random()%10];
    VerifyImmutable(x,random_flags,values,true);
    const uint32_t page=x.first+random()%(x.last-x.first+1);
    const uint32_t byte=page*4096;
    values[(byte>x.offset?byte-x.offset:0)/65536]=3;
    VerifyImmutable(x,random_flags,values,false);
  }
}
}
int main() {
  try {
    LiteralMasksAndMutation();GuardBounds();WrapCounterexample();ModeWrapper();Randomized();
    ImmutableControls();
    Check(accepted>=256 && rejected>=256,"random accepted/rejected coverage missing");
    std::cout<<"PASS watch_fastpath: "<<checks<<" checks, "<<accepted
             <<" accepted / "<<rejected<<" rejected cases\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"FAIL watch_fastpath: "<<e.what()<<"\n";return 1;
  }
}
