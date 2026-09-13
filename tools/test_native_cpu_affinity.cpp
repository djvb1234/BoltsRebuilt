// Original portable controls. Define NB_NATIVE_CPU_AFFINITY_WINDOWS_TEST and
// link native_cpu_affinity.cpp for explicit own-thread Windows integration tests.
#include "../src/gpu/native/native_cpu_affinity_policy.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#if defined(NB_NATIVE_CPU_AFFINITY_WINDOWS_TEST)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../src/gpu/native/native_cpu_affinity.h"
#endif

namespace {
using Entry = nb::gpu::NativeCpuTopologyEntry;
using Result = nb::gpu::NativeCpuAffinitySelectionResult;
using nb::gpu::SelectNativePerformanceAffinity;
uint64_t checks = 0;
void Check(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
std::vector<Entry> Machine() {
  std::vector<Entry> values;
  // Captured OS topology: eight noncontiguous performance cores, not CPUs0..7.
  for (uint16_t cpu = 0; cpu < 24; ++cpu) {
    const bool performance = cpu == 0 || cpu == 1 || (cpu >= 10 && cpu <= 13) || cpu == 22 || cpu == 23;
    values.push_back({0, cpu, uint8_t(performance), false});
  }
  return values;
}
void LiteralTopology() {
  const auto values = Machine();
  auto selection = SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF);
  Check(selection.result == Result::kReady, "heterogeneous topology accepted");
  Check(selection.performance_mask == 0xC03C03, "exact observed P-core mask");
  Check(selection.efficiency_class == 1, "higher Windows efficiency class is performance class");
  selection = SelectNativePerformanceAffinity(values, 0, 0xC00000, 0xFFFFFF, 0xFFFFFF);
  Check(selection.performance_mask == 0xC00000, "original affinity never widened");
  selection = SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0x3C00, 0xFFFFFF);
  Check(selection.performance_mask == 0x3C00, "process affinity intersection");
  selection = SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0x3FFF);
  Check(selection.performance_mask == 0x3C03, "offline system CPUs excluded");
}
void RefusalControls() {
  auto values = Machine();
  Check(SelectNativePerformanceAffinity(values, 1, 1, 1, 1).result == Result::kUnsupportedGroup,
        "other groups refused");
  Check(SelectNativePerformanceAffinity(values, 0, 0, 0xFFFFFF, 0xFFFFFF).result == Result::kEmptyAllowedMask,
        "empty allowed mask refused");
  Check(SelectNativePerformanceAffinity(values, 0, 0x3FC, 0xFFFFFF, 0xFFFFFF).result == Result::kNoAllowedPerformanceCore,
        "E-only original mask does not relabel E cores as P");
  for (auto& value : values) value.efficiency_class = 0;
  Check(SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF).result == Result::kHomogeneousTopology,
        "homogeneous CPU stays unchanged");
  Check(SelectNativePerformanceAffinity({}, 0, 1, 1, 1).result == Result::kInvalidTopology,
        "missing topology refused");
}
void ReservationControls() {
  auto values = Machine(); values[1].reserved_for_other = true;
  auto selection = SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF);
  Check(selection.performance_mask == 0xC03C01, "reserved CPU excluded");
  for (auto& value : values) if (value.efficiency_class) value.reserved_for_other = true;
  Check(SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF).result == Result::kNoAllowedPerformanceCore,
        "all performance cores reserved refuses E substitution");
}
void BoundsAndClasses() {
  std::vector<Entry> values = {{0, 0, 1, false}, {0, 63, 9, false}};
  auto selection = SelectNativePerformanceAffinity(values, 0, UINT64_MAX, UINT64_MAX, UINT64_MAX);
  Check(selection.performance_mask == (uint64_t(1) << 63), "bit63 without shift overflow");
  Check(selection.efficiency_class == 9, "nonbinary efficiency classes supported");
  values.push_back({0, 64, 9, false});
  Check(SelectNativePerformanceAffinity(values, 0, UINT64_MAX, UINT64_MAX, UINT64_MAX).result == Result::kInvalidTopology,
        "invalid logical index refused");
  values = Machine(); values.push_back({0, 1, 0, false});
  Check(SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF).result == Result::kInvalidTopology,
        "conflicting duplicate CPU class refused");
  values = Machine(); values.push_back(values[1]); values.push_back({1, 1, 99, false});
  Check(SelectNativePerformanceAffinity(values, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF).performance_mask == 0xC03C03,
        "consistent duplicates and unrelated groups do not perturb selection");
}
void AllSubsetControls() {
  const auto values = Machine();
  // Every subset of a mixed8-CPU window, checked against a literal mask oracle.
  for (uint64_t mask = 1; mask < 256; ++mask) {
    const auto selection = SelectNativePerformanceAffinity(values, 0, mask, 0xFFFFFF, 0xFFFFFF);
    const uint64_t expected = mask & 0xC03C03;
    Check(selection.performance_mask == expected, "subset intersection byte oracle");
    Check(selection.result == (expected ? Result::kReady : Result::kNoAllowedPerformanceCore),
          "subset outcome oracle");
  }
}
void TransitionControls() {
  using nb::gpu::GetNativeCpuAffinityTransition;
  using Transition = nb::gpu::NativeCpuAffinityTransition;
  Check(GetNativeCpuAffinityTransition(false, false) == Transition::kNone, "default-off performs no OS operation");
  Check(GetNativeCpuAffinityTransition(false, true) == Transition::kEnable, "explicit enable transition");
  Check(GetNativeCpuAffinityTransition(true, true) == Transition::kNone, "steady enable performs no OS operation");
  Check(GetNativeCpuAffinityTransition(true, false) == Transition::kRestore, "disable restores captured baseline");
  bool previous = false; unsigned enables = 0, restores = 0;
  for (unsigned frame = 0; frame < 800; ++frame) {
    const bool request = frame >= 200 && frame < 600;
    const auto transition = GetNativeCpuAffinityTransition(previous, request);
    enables += transition == Transition::kEnable; restores += transition == Transition::kRestore;
    previous = request;
  }
  Check(enables == 1 && restores == 1, "800 frame ABBA sequence changes scheduling exactly twice");
}
void PlacementControls() {
  using nb::gpu::SelectNativeCpuPlacement;
  const auto selection = SelectNativePerformanceAffinity(Machine(), 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF);
  const uint64_t expected_masks[] = {0xC03C03, 0xC03C03, 0x800000, 1};
  const uint8_t expected_ideals[] = {0, 23, 23, 0};
  for (uint32_t mode = 0; mode < 4; ++mode) {
    const auto placement = SelectNativeCpuPlacement(selection, mode);
    Check(placement.valid && placement.mask == expected_masks[mode], "four literal placement masks");
    Check(placement.set_ideal == (mode != 0), "mode zero leaves ideal selection to Windows");
    Check(placement.ideal_cpu == expected_ideals[mode], "four literal ideal selections");
  }
  Check(!SelectNativeCpuPlacement(selection, 8).valid, "unsupported placement mode refused");
  Check(!SelectNativeCpuPlacement(selection, UINT32_MAX).valid, "negative int conversion is refused");
  auto invalid = selection;
  invalid.performance_mask = 0;
  Check(!SelectNativeCpuPlacement(invalid, 0).valid, "empty performance mask cannot enable");
  invalid = selection; invalid.allowed_mask = 1;
  Check(!SelectNativeCpuPlacement(invalid, 2).valid, "placement cannot bypass validated allowed intersection");
  for (auto reason : {Result::kInvalidTopology, Result::kHomogeneousTopology, Result::kUnsupportedGroup,
                     Result::kNoAllowedPerformanceCore, Result::kEmptyAllowedMask}) {
    invalid = selection; invalid.result = reason;
    for (uint32_t mode = 0; mode < 4; ++mode)
      Check(!SelectNativeCpuPlacement(invalid, mode).valid, "every placement preserves topology refusal");
  }
  const std::vector<Entry> high = {{0, 0, 0, false}, {0, 63, 9, false}};
  const auto high_selection = SelectNativePerformanceAffinity(high, 0, UINT64_MAX, UINT64_MAX, UINT64_MAX);
  for (uint32_t mode = 0; mode < 4; ++mode) {
    const auto placement = SelectNativeCpuPlacement(high_selection, mode);
    Check(placement.valid && placement.mask == (uint64_t(1) << 63), "single available P core at bit63");
    if (mode) Check(placement.ideal_cpu == 63, "both placement directions handle bit63");
  }
}
void PlacementSubsetControls() {
  using nb::gpu::SelectNativeCpuPlacement;
  const uint8_t logical[] = {0, 1, 10, 11, 12, 13, 22, 23};
  for (uint32_t subset = 1; subset < 256; ++subset) {
    uint64_t permitted = 0;
    std::vector<uint8_t> expected;
    for (uint32_t index = 0; index < 8; ++index) {
      if (subset & (1u << index)) {
        permitted |= uint64_t(1) << logical[index];
        expected.push_back(logical[index]);
      }
    }
    // E cores remain permitted too, proving selection uses the validated
    // performance intersection rather than the first/last allowed CPU.
    const auto selection = SelectNativePerformanceAffinity(Machine(), 0, permitted | 0x3FC,
                                                           0xFFFFFF, 0xFFFFFF);
    for (uint32_t mode = 0; mode < 4; ++mode) {
      const auto placement = SelectNativeCpuPlacement(selection, mode);
      Check(placement.valid, "every nonempty allowed P subset supports every mode");
      const uint8_t selected_cpu = mode == 3 ? expected.front() : expected.back();
      Check(placement.mask == (mode < 2 ? permitted : uint64_t(1) << selected_cpu),
            "placement matches independent ordered-CPU oracle");
      if (mode) Check(placement.ideal_cpu == selected_cpu, "preferred core never escapes allowed P subset");
    }
  }
  auto reserved = Machine(); reserved[23].reserved_for_other = true; reserved[0].reserved_for_other = true;
  const auto selection = SelectNativePerformanceAffinity(reserved, 0, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF);
  Check(SelectNativeCpuPlacement(selection, 1).ideal_cpu == 22, "highest reserved P core is skipped");
  Check(SelectNativeCpuPlacement(selection, 3).ideal_cpu == 1, "lowest reserved P core is skipped");
}
void PlacementTransitionControls() {
  using nb::gpu::GetNativeCpuAffinityTransition;
  using Transition = nb::gpu::NativeCpuAffinityTransition;
  for (uint32_t before = 0; before < 4; ++before) for (uint32_t after = 0; after < 4; ++after) {
    Check(GetNativeCpuAffinityTransition(false, false, before, after) == Transition::kNone,
          "placement changes while disabled never schedule");
    Check(GetNativeCpuAffinityTransition(false, true, before, after) == Transition::kEnable,
          "enable accepts each placement mode");
    Check(GetNativeCpuAffinityTransition(true, false, before, after) == Transition::kRestore,
          "disable restores each placement mode");
    Check(GetNativeCpuAffinityTransition(true, true, before, after) ==
              (before == after ? Transition::kNone : Transition::kEnable),
          "enabled mode change enters restore-before-enable path");
  }
  Check(GetNativeCpuAffinityTransition(true, true, 9, 9) == Transition::kNone,
        "failed invalid mode remains latched for identical request");
  Check(GetNativeCpuAffinityTransition(true, true, 9, 0) == Transition::kEnable,
        "new valid mode can recover from latched failed request");
  bool enabled = false; uint32_t previous_mode = 0;
  uint32_t enables = 0, final_restores = 0;
  const uint32_t modes[] = {0, 1, 2, 3, 3, 2, 1, 0};
  for (uint32_t mode : modes) {
    for (unsigned frame = 0; frame < 300; ++frame) {
      const auto transition = GetNativeCpuAffinityTransition(enabled, true, previous_mode, mode);
      enables += transition == Transition::kEnable;
      enabled = true; previous_mode = mode;
    }
  }
  final_restores += GetNativeCpuAffinityTransition(enabled, false, previous_mode, 0) == Transition::kRestore;
  Check(enables == 7 && final_restores == 1, "2400-frame mirrored trial applies only seven transitions then restores");
}
void SchedulingClassControls() {
  auto entries = Machine();
  for (auto& e : entries) e.scheduling_class = e.efficiency_class ? 1 : 99;
  entries[1].scheduling_class = entries[10].scheduling_class = 2;
  const uint8_t logical[] = {0,1,10,11,12,13,22,23};
  for (uint32_t subset=1; subset<256; ++subset) {
    uint64_t permitted=0;
    std::vector<uint8_t> chosen;
    for (uint32_t i=0;i<8;++i) if(subset&(1u<<i)) permitted|=uint64_t(1)<<logical[i];
    const bool has_class_two=(permitted&0x402)!=0;
    uint64_t expected=0;
    for (auto cpu:logical) if((permitted&(uint64_t(1)<<cpu)) &&
        (!has_class_two || cpu==1 || cpu==10)) {chosen.push_back(cpu);expected|=uint64_t(1)<<cpu;}
    const auto s=SelectNativePerformanceAffinity(entries,0,permitted|0x3FC,0xFFFFFF,0xFFFFFF);
    Check(s.performance_mask==permitted && s.scheduling_mask==expected,
          "scheduling subset never includes high-class E cores or escapes permissions");
    Check(s.scheduling_class==(has_class_two?2:1),"scheduling class derived only from permitted P cores");
    for(uint32_t mode=4;mode<8;++mode) {
      const auto p=nb::gpu::SelectNativeCpuPlacement(s,mode);
      const auto cpu=mode==7?chosen.front():chosen.back();
      Check(p.valid && p.mask==(mode<6?expected:uint64_t(1)<<cpu),"class mode mask matches ordered CPU oracle");
      Check(p.set_ideal==(mode!=4),"class mask mode retains normal ideal policy");
      if(mode!=4)Check(p.ideal_cpu==cpu,"class ideal stays in selected subset");
    }
  }
  entries[1].reserved_for_other=entries[10].reserved_for_other=true;
  const auto reserved=SelectNativePerformanceAffinity(entries,0,0xFFFFFF,0xFFFFFF,0xFFFFFF);
  Check(reserved.scheduling_class==1 && !(reserved.scheduling_mask&0x402),"reserved high-class cores excluded");
  auto repeated=entries;repeated.push_back(entries[0]);repeated.back().scheduling_class=7;
  Check(SelectNativePerformanceAffinity(repeated,0,0xFFFFFF,0xFFFFFF,0xFFFFFF).result==Result::kInvalidTopology,
        "conflicting scheduling class for one CPU is rejected");
  auto corrupt=reserved;corrupt.scheduling_mask|=uint64_t(1)<<9;
  Check(!nb::gpu::SelectNativeCpuPlacement(corrupt,4).valid,"bad scheduling subset cannot escape P mask");
  corrupt=reserved;corrupt.scheduling_mask=0;
  Check(!nb::gpu::SelectNativeCpuPlacement(corrupt,6).valid,"empty scheduling subset is refused");
  for(uint32_t mode=8;mode<16;++mode)
    Check(!nb::gpu::SelectNativeCpuPlacement(reserved,mode).valid,"unsupported extended placement refused");
}
#if defined(NB_NATIVE_CPU_AFFINITY_WINDOWS_TEST)
// Explicit optional integration control. Only this executable's calling thread
// is changed; the production helper's destructor restores it on every exit.
bool WindowsPlacementControls() {
  GROUP_AFFINITY original{};
  PROCESSOR_NUMBER original_ideal{};
  Check(GetThreadGroupAffinity(GetCurrentThread(), &original) != FALSE, "Windows original group query");
  Check(GetThreadIdealProcessorEx(GetCurrentThread(), &original_ideal) != FALSE, "Windows original ideal query");
  const auto original_restored = [&]() {
    GROUP_AFFINITY actual{}; PROCESSOR_NUMBER ideal{};
    return GetThreadGroupAffinity(GetCurrentThread(), &actual) &&
        GetThreadIdealProcessorEx(GetCurrentThread(), &ideal) && actual.Group == original.Group &&
        actual.Mask == original.Mask && ideal.Group == original_ideal.Group && ideal.Number == original_ideal.Number;
  };
  nb::gpu::NativeCpuAffinity affinity;
  Check(affinity.Update(false, 3) && original_restored(), "Windows disabled mode never changes baseline");
  if (!affinity.Update(true, 0)) {
    const uint32_t error = affinity.status().windows_error;
    Check(affinity.Shutdown() && original_restored(), "unsupported Windows trial leaves exact baseline");
    Check(error == ERROR_NOT_SUPPORTED, "Windows setup failed unexpectedly");
    std::cout << "SKIP: Windows placement trial unavailable on this topology/CPU-set configuration\n";
    return false;
  }
  const uint64_t performance_mask = affinity.status().performance_mask;
  Check(performance_mask != 0, "Windows trial exposes a nonempty validated performance mask");
  const uint64_t scheduling_mask = affinity.status().scheduling_mask;
  Check(scheduling_mask && !(scheduling_mask & ~performance_mask), "Windows class mask is a permitted P subset");
  uint64_t prior_enables = affinity.status().enables, prior_restores = affinity.status().restores;
  uint32_t previous_mode = 0;
  const uint32_t modes[] = {0,1,2,3,4,5,6,7,7,6,5,4,3,2,1,0};
  for (uint32_t mode : modes) {
    Check(affinity.Update(true, mode), "Windows placement transition succeeds");
    GROUP_AFFINITY actual{}; PROCESSOR_NUMBER ideal{};
    Check(GetThreadGroupAffinity(GetCurrentThread(), &actual) != FALSE &&
          GetThreadIdealProcessorEx(GetCurrentThread(), &ideal) != FALSE, "Windows applied state queries");
    const uint64_t selected_mask=mode&4?scheduling_mask:performance_mask;
    uint32_t low=0,high=63;
    while(!(selected_mask&(uint64_t(1)<<low)))++low;
    while(!(selected_mask&(uint64_t(1)<<high)))--high;
    const uint32_t direction=mode&3;
    const uint64_t expected_mask = direction < 2 ? selected_mask : uint64_t(1) << (direction == 3 ? low : high);
    Check(actual.Group == original.Group && actual.Mask == expected_mask && !(actual.Mask & ~original.Mask),
          "Windows actual mask matches exact permitted placement");
    if (direction) Check(ideal.Group == original.Group && ideal.Number == (direction == 3 ? low : high),
                    "Windows actual preferred core matches placement");
    const auto& status = affinity.status();
    Check(status.original_mask == original.Mask && status.original_ideal_cpu == original_ideal.Number,
          "mode changes preserve original baseline instead of recapturing narrowed state");
    Check(status.applied_mask == actual.Mask && status.current_ideal_group == ideal.Group &&
          status.current_ideal_cpu == ideal.Number && status.placement_mode == mode,
          "reported applied mask and ideal match OS state");
    Check(status.sampled_group == actual.Group && status.sampled_cpu < 64 &&
          (actual.Mask & (uint64_t(1) << status.sampled_cpu)), "sampled CP CPU lies in applied mask");
    const uint64_t changes = mode != previous_mode;
    Check(status.enables == prior_enables + changes && status.restores == prior_restores + changes,
          "each mode change restores then applies exactly once");
    prior_enables = status.enables; prior_restores = status.restores; previous_mode = mode;
  }
  Check(!affinity.Update(true, 8) && !affinity.status().active && original_restored(),
        "invalid mode restores exact baseline before refusal");
  const uint64_t failures = affinity.status().failures;
  Check(!affinity.Update(true, 8) && affinity.status().failures == failures && original_restored(),
        "repeated invalid mode stays latched without mutation");
  Check(affinity.Update(true, 2), "valid placement recovers after latched failure");
  Check(affinity.Update(false, 3) && original_restored(), "disable restores exact group mask and ideal");
  Check(affinity.Update(true, 3), "last placement enabled for shutdown control");
  Check(affinity.Shutdown() && original_restored(), "shutdown restores exact group mask and ideal");
  std::cout << "PASS: Windows placement transitions, readback, failure latching and exact restoration\n";
  return true;
}
#endif
}  // namespace
int main() {
  try {
    LiteralTopology(); RefusalControls(); ReservationControls(); BoundsAndClasses(); AllSubsetControls(); TransitionControls();
    PlacementControls(); PlacementSubsetControls(); PlacementTransitionControls(); SchedulingClassControls();
#if defined(NB_NATIVE_CPU_AFFINITY_WINDOWS_TEST)
    const bool windows_ran = WindowsPlacementControls();
    std::cout << "PASS: 10 portable CPU-affinity controls; Windows " << (windows_ran ? "passed" : "skipped")
              << ", " << checks << " checks\n";
#else
    std::cout << "PASS: 10 native CPU-affinity controls, " << checks << " checks; no OS mutations\n";
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
