// Original public-API runtime parity control. No game data, GPU, write faults,
// private heap inspection, Memory::Reset, or whole-arena clearing.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>
#include "../src/runtime/watch_control.h"

namespace {
using Memory = rex::memory::Memory;
using Heap = rex::memory::BaseHeap;
constexpr uint32_t kHeaps[] = {0xA0000000,0xC0000000,0xE0000000};
constexpr uint32_t kRW = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
uint64_t checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
struct Event {
  uint32_t phase; int64_t relative_start; uint32_t length; bool exact;
  bool operator==(const Event&) const = default;
};
struct Protection {
  uint32_t phase, sample; DWORD state, protection;
  bool operator==(const Protection&) const = default;
};
struct Result {
  std::array<Event,128> events{}; size_t event_count = 0;
  std::array<Protection,64> protections{}; size_t protection_count = 0;
  std::array<bool,32> triggers{}; size_t trigger_count = 0;
  uint32_t phase = 0, origin = 0; bool overflow = false;
};
std::pair<uint32_t,uint32_t> Callback(void* context, uint32_t start,
                                    uint32_t length, bool exact) {
  // Called under the runtime global lock: no allocation, logging, or throwing.
  auto& r = *static_cast<Result*>(context);
  if (r.event_count == r.events.size()) r.overflow = true;
  else r.events[r.event_count++] = {r.phase,int64_t(start)-r.origin,length,exact};
  return {start,length}; // Do not request excess unwatching.
}
void Phase(Result& r, uint32_t origin) { ++r.phase; r.origin = origin; }
bool Trigger(Memory& memory, Result& r, uint32_t address, uint32_t length) {
  bool value = memory.TriggerPhysicalMemoryCallbacks(
      rex::thread::global_critical_region::AcquireDirect(),address,length,true,true,true);
  Check(!r.overflow && r.trigger_count < r.triggers.size(),"callback trace capacity");
  r.triggers[r.trigger_count++] = value;
  return value;
}
void Snapshot(Memory& memory, Result& r, uint32_t address, uint32_t step,
              DWORD expected = 0) {
  for (uint32_t sample = 0; sample != 2; ++sample) {
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(memory.TranslateVirtual(address+sample*step),&info,sizeof(info))
          == sizeof(info),"VirtualQuery failed");
    Check(r.protection_count < r.protections.size(),"protection trace capacity");
    r.protections[r.protection_count++] = {r.phase,sample,info.State,info.Protect};
    Check(info.State == MEM_COMMIT,"test mapping not committed");
    if (expected) Check((info.Protect & 0xFF) == expected,"unexpected host protection");
  }
}
// Memory::Enable... visits all three aliases. Clipped edge probes can arm an
// existing initialization allocation in another alias, so clear every touched
// intersection using public mapping APIs before the next case/mode.
void ClearAliases(Memory& memory, Result& r, uint32_t start, uint32_t length) {
  const uint64_t end = uint64_t(start)+length;
  for (uint32_t base : kHeaps) {
    auto* heap = memory.LookupHeap(base);
    const uint32_t low = memory.GetPhysicalAddress(heap->heap_base());
    const uint32_t high = memory.GetPhysicalAddress(heap->heap_base()+heap->heap_size()-1);
    const uint64_t first = std::max(uint64_t(start),uint64_t(low));
    const uint64_t stop = std::min(end,uint64_t(high)+1);
    if (first < stop) Trigger(memory,r,heap->heap_base()+uint32_t(first-low),uint32_t(stop-first));
  }
}
struct Cleanup {
  Memory& memory; Heap* heap; uint32_t address; void* callback;
  ~Cleanup() {
    // Result (callback storage) outlives this object. Release first so a watched
    // allocation is invalidated while its callback is still registered.
    if (address) heap->Release(address);
    if (callback) memory.UnregisterPhysicalMemoryInvalidationCallback(callback);
  }
};
Result Case(Memory& memory, uint32_t heap_base, uint32_t mode, uint32_t page) {
  nb::runtime::SetWatchFastpathMode(mode);
  Check(nb::runtime::GetWatchFastpathMode() == mode,"runtime mode did not switch");
  auto* heap = memory.LookupHeap(heap_base);
  Check(heap && heap->heap_type() == rex::memory::HeapType::kGuestPhysical,"physical heap lookup");
  const uint32_t bytes = std::max(heap->page_size(),page*2);
  uint32_t address = 0;
  Check(heap->Alloc(bytes,std::max(heap->page_size(),page),
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      kRW,false,&address),"isolated physical allocation failed");
  Result r;
  Cleanup cleanup{memory,heap,address,nullptr};
  cleanup.callback = memory.RegisterPhysicalMemoryInvalidationCallback(Callback,&r);
  Check(cleanup.callback != nullptr,"callback registration failed");
  const uint32_t physical = memory.GetPhysicalAddress(address);
  Check(physical != UINT32_MAX,"allocated physical mapping unavailable");
  const uint32_t span = page+1;
  Phase(r,physical); Snapshot(memory,r,address,page,PAGE_READWRITE);
  for (unsigned i=0;i!=2;++i) memory.EnablePhysicalMemoryAccessCallbacks(physical,span,true,false);
  Check(r.event_count == 0,"enable unexpectedly invoked callback");
  Snapshot(memory,r,address,page,PAGE_READONLY);
  Phase(r,physical);
  Check(Trigger(memory,r,address,span),"watched explicit trigger returned false");
  Check(r.event_count == 1 && r.events[0].exact,"explicit callback event missing");
  Snapshot(memory,r,address,page,PAGE_READWRITE);
  Check(!Trigger(memory,r,address,span),"unwatched repeat triggered callback");
  Phase(r,physical);
  memory.EnablePhysicalMemoryAccessCallbacks(physical,span,true,false);
  memory.EnablePhysicalMemoryAccessCallbacks(physical,span,true,false);
  Snapshot(memory,r,address,page,PAGE_READONLY);
  Check(Trigger(memory,r,address,span),"re-enabled explicit trigger failed");
  for (uint32_t access : {uint32_t(rex::memory::kMemoryProtectRead),uint32_t(0)}) {
    Phase(r,physical);
    Check(heap->Protect(address,bytes,access,nullptr),"guest protect failed");
    const size_t events_before = r.event_count;
    memory.EnablePhysicalMemoryAccessCallbacks(physical,span,true,false);
    memory.EnablePhysicalMemoryAccessCallbacks(physical,span,true,false);
    Snapshot(memory,r,address,page,access ? PAGE_READONLY : PAGE_NOACCESS);
    Check(!Trigger(memory,r,address,span),"immutable/inaccessible page became watched");
    Check(r.event_count == events_before,"unexpected immutable page callback");
    Check(heap->Protect(address,bytes,kRW,nullptr),"restore guest writable failed");
    Snapshot(memory,r,address,page,PAGE_READWRITE);
  }
  const uint32_t low = memory.GetPhysicalAddress(heap->heap_base());
  const uint32_t high = memory.GetPhysicalAddress(heap->heap_base()+heap->heap_size()-1);
  const std::array<uint32_t,2> clipped[] = {{physical,0},{low ? low-1 : 0,low ? 2u : 0u},{high-1,8}};
  for (const auto& range : clipped) {
    Phase(r,range[0]);
    memory.EnablePhysicalMemoryAccessCallbacks(range[0],range[1],true,false);
    memory.EnablePhysicalMemoryAccessCallbacks(range[0],range[1],true,false);
    Snapshot(memory,r,address,page);
    ClearAliases(memory,r,range[0],range[1]);
    Snapshot(memory,r,address,page,PAGE_READWRITE);
  }
  Phase(r,physical);
  memory.EnablePhysicalMemoryAccessCallbacks(physical,span,true,false);
  Snapshot(memory,r,address,page,PAGE_READONLY);
  Check(heap->Release(address),"watched allocation release failed");
  cleanup.address = 0;
  Check(!r.overflow,"callback trace overflow");
  memory.UnregisterPhysicalMemoryInvalidationCallback(cleanup.callback);
  cleanup.callback = nullptr;
  std::cout << "CASE heap=0x" << std::hex << heap_base << std::dec
            << " mode=" << mode << " events=" << r.event_count
            << " protections=" << r.protection_count << '\n';
  return r;
}
void Compare(const Result& a, const Result& b) {
  Check(a.event_count == b.event_count,"callback event count differs");
  Check(a.protection_count == b.protection_count,"protection sample count differs");
  Check(a.trigger_count == b.trigger_count,"trigger count differs");
  for(size_t i=0;i<a.event_count;++i) Check(a.events[i] == b.events[i],"callback trace differs");
  for(size_t i=0;i<a.protection_count;++i) Check(a.protections[i] == b.protections[i],"host protection differs");
  for(size_t i=0;i<a.trigger_count;++i) Check(a.triggers[i] == b.triggers[i],"trigger return differs");
}
struct ResetMode { ~ResetMode() { nb::runtime::SetWatchFastpathMode(0); } };
}
int main() {
  try {
    ResetMode reset;
    Check(nb::runtime::WatchFastpathAvailable(),"requires override-enabled runtime build");
    wchar_t runtime_path[32768]{};
    const auto runtime_module = GetModuleHandleW(L"rexruntime.dll");
    const DWORD path_length = GetModuleFileNameW(runtime_module,runtime_path,32768);
    Check(runtime_module && path_length && path_length < 32768,"runtime DLL identity unavailable");
    std::wcout << L"RUNTIME " << runtime_path << L'\n';
    nb::runtime::SetWatchFastpathMode(0);
    rex::InitLogging();
    Memory memory;
    Check(memory.Initialize(),"Memory.Initialize failed");
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    Check(info.dwPageSize == 4096,"fixture currently validates Windows 4KiB host pages");
    for(uint32_t heap : kHeaps) {
      const auto off = Case(memory,heap,0,info.dwPageSize);
      for (uint32_t mode = 1; mode <= 3; ++mode) {
        const auto on = Case(memory,heap,mode,info.dwPageSize);
        Compare(off,on);
      }
    }
    std::cout << "PASS runtime_watch: " << checks << " checks, three aliases / four modes\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr << "FAIL runtime_watch: " << e.what() << '\n'; return 1;
  }
}
