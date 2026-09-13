// Original portable policy for an opt-in, single-group performance-core trial.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace nb::gpu {
struct NativeCpuTopologyEntry {
  uint16_t group;
  uint16_t logical;
  uint8_t efficiency_class;
  bool reserved_for_other;
  uint8_t scheduling_class = 0;
};
enum class NativeCpuAffinitySelectionResult {
  kReady,
  kUnsupportedGroup,
  kInvalidTopology,
  kHomogeneousTopology,
  kEmptyAllowedMask,
  kNoAllowedPerformanceCore,
};
struct NativeCpuAffinitySelection {
  NativeCpuAffinitySelectionResult result = NativeCpuAffinitySelectionResult::kInvalidTopology;
  uint64_t allowed_mask = 0, performance_mask = 0;
  uint8_t efficiency_class = 0;
  uint64_t scheduling_mask = 0;
  uint8_t scheduling_class = 0;
};

inline NativeCpuAffinitySelection SelectNativePerformanceAffinity(
    std::span<const NativeCpuTopologyEntry> entries, uint16_t group,
    uint64_t original_mask, uint64_t process_mask, uint64_t system_mask) {
  NativeCpuAffinitySelection out;
  if (group != 0) { out.result = NativeCpuAffinitySelectionResult::kUnsupportedGroup; return out; }
  out.allowed_mask = original_mask & process_mask & system_mask;
  if (!out.allowed_mask) { out.result = NativeCpuAffinitySelectionResult::kEmptyAllowedMask; return out; }
  std::array<int16_t, 64> classes;
  std::array<uint8_t, 64> scheduling_classes{};
  classes.fill(-1);
  int highest = -1, lowest = 256;
  for (const auto& entry : entries) {
    if (entry.group != group) continue;
    if (entry.logical >= 64) return out;
    const uint64_t bit = uint64_t(1) << entry.logical;
    if (!(system_mask & bit)) continue;
    auto& previous = classes[entry.logical];
    if (previous >= 0 && (previous != entry.efficiency_class ||
                         scheduling_classes[entry.logical] != entry.scheduling_class)) return out;
    previous = entry.efficiency_class;
    scheduling_classes[entry.logical] = entry.scheduling_class;
    if (entry.efficiency_class > highest) highest = entry.efficiency_class;
    if (entry.efficiency_class < lowest) lowest = entry.efficiency_class;
  }
  if (highest < 0) return out;
  if (highest == lowest) { out.result = NativeCpuAffinitySelectionResult::kHomogeneousTopology; return out; }
  out.efficiency_class = uint8_t(highest);
  for (const auto& entry : entries) {
    if (entry.group == group && entry.logical < 64 && !entry.reserved_for_other &&
        entry.efficiency_class == highest) {
      out.performance_mask |= uint64_t(1) << entry.logical;
    }
  }
  out.performance_mask &= out.allowed_mask;
  // Optional experiment over the observed Windows scheduling-class byte.
  // Do not infer a CPU's frequency or benchmark speed from this metadata.
  // Selection stays inside the already permitted P-class intersection.
  for (const auto& entry : entries) {
    if (entry.group != group || entry.logical >= 64 || entry.reserved_for_other ||
        !(out.performance_mask & (uint64_t(1) << entry.logical))) continue;
    if (entry.scheduling_class > out.scheduling_class) {
      out.scheduling_class = entry.scheduling_class;
      out.scheduling_mask = 0;
    }
    if (entry.scheduling_class == out.scheduling_class)
      out.scheduling_mask |= uint64_t(1) << entry.logical;
  }
  out.result = out.performance_mask ? NativeCpuAffinitySelectionResult::kReady
                                    : NativeCpuAffinitySelectionResult::kNoAllowedPerformanceCore;
  return out;
}

enum class NativeCpuAffinityTransition { kNone, kEnable, kRestore };
inline NativeCpuAffinityTransition GetNativeCpuAffinityTransition(
    bool previous_request, bool request, uint32_t previous_mode = 0, uint32_t mode = 0) {
  if (!request) return previous_request ? NativeCpuAffinityTransition::kRestore
                                       : NativeCpuAffinityTransition::kNone;
  return !previous_request || previous_mode != mode ? NativeCpuAffinityTransition::kEnable
                                                    : NativeCpuAffinityTransition::kNone;
}

struct NativeCpuPlacement {
  bool valid = false, set_ideal = false;
  uint64_t mask = 0;
  uint8_t ideal_cpu = 0;
};
// Select only from the already validated original/process/system P-class
// intersection. Never relabel a homogeneous or E-only topology as a P core.
inline NativeCpuPlacement SelectNativeCpuPlacement(
    const NativeCpuAffinitySelection& selection, uint32_t mode) {
  NativeCpuPlacement out;
  if (mode > 7 || selection.result != NativeCpuAffinitySelectionResult::kReady ||
      !selection.performance_mask || (selection.performance_mask & ~selection.allowed_mask)) return out;
  const uint64_t selected = mode & 4 ? selection.scheduling_mask : selection.performance_mask;
  if (!selected || (selected & ~selection.performance_mask)) return out;
  out.valid = true;
  out.mask = selected;
  mode &= 3;
  if (!mode) return out;
  out.set_ideal = true;
  if (mode == 3) {
    for (uint32_t cpu = 0; cpu < 64; ++cpu) {
      if (out.mask & (uint64_t(1) << cpu)) { out.ideal_cpu = uint8_t(cpu); break; }
    }
  } else {
    for (uint32_t cpu = 64; cpu != 0; --cpu) {
      if (out.mask & (uint64_t(1) << (cpu - 1))) { out.ideal_cpu = uint8_t(cpu - 1); break; }
    }
  }
  if (mode >= 2) out.mask = uint64_t(1) << out.ideal_cpu;
  return out;
}
}  // namespace nb::gpu
