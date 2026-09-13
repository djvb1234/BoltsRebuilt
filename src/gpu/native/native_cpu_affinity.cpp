// Original Windows implementation. Only explicit enable/disable transitions
// alter the GPU CP thread; other threads, process priority and power policy stay
// outside this helper's scope. EfficiencyClass is discovered from Windows.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_cpu_affinity.h"
#include "native_cpu_affinity_policy.h"

#include <cstring>
#include <vector>

namespace nb::gpu {
struct NativeCpuAffinity::Impl {
  HANDLE thread = nullptr;
  DWORD thread_id = 0;
  GROUP_AFFINITY original_affinity{};
  PROCESSOR_NUMBER original_ideal{};
  bool last_requested = false, last_result = true;
  uint32_t last_mode = 0;
  ~Impl() { if (thread) CloseHandle(thread); }
};

NativeCpuAffinity::NativeCpuAffinity() = default;
NativeCpuAffinity::~NativeCpuAffinity() { Shutdown(); }

bool NativeCpuAffinity::Fail(const char* reason, uint32_t error) noexcept {
  status_.reason = reason;
  status_.windows_error = error;
  ++status_.failures;
  return false;
}

bool NativeCpuAffinity::Update(bool enabled, uint32_t mode) {
  if (!impl_) {
    if (!enabled) return true; // default-off path does not even query the OS
    try {
      impl_ = std::make_unique<Impl>();
    } catch (...) {
      return Fail("CPU affinity state allocation failed", ERROR_NOT_ENOUGH_MEMORY);
    }
  }
  const auto transition = GetNativeCpuAffinityTransition(impl_->last_requested, enabled, impl_->last_mode, mode);
  if (transition == NativeCpuAffinityTransition::kNone) {
    SampleProcessor();
    return impl_->last_result;
  }
  // State changes are requested on the CP thread; Shutdown may restore from its
  // owner after the worker stops. Retain a real handle to avoid TID reuse.
  if (impl_->thread && GetCurrentThreadId() != impl_->thread_id) {
    // Do not consume the transition: the CP thread can still make it later.
    return Fail("affinity transition requested from another thread", ERROR_INVALID_THREAD_ID);
  }
  impl_->last_requested = enabled;
  impl_->last_mode = mode;
  if (transition == NativeCpuAffinityTransition::kRestore) {
    impl_->last_result = Restore();
    return impl_->last_result;
  }
  try {
    impl_->last_result = Enable(mode);
  } catch (...) {
    // Allocations/topology reads precede the mutation. If an unexpected later
    // exception occurs, restore before returning control to the renderer.
    Restore();
    impl_->last_result = Fail("CPU topology preparation failed", ERROR_NOT_ENOUGH_MEMORY);
  }
  SampleProcessor();
  return impl_->last_result;
}

void NativeCpuAffinity::SampleProcessor() noexcept {
  if (!impl_ || !status_.active || !impl_->last_result || GetCurrentThreadId() != impl_->thread_id) return;
  PROCESSOR_NUMBER processor{};
  GetCurrentProcessorNumberEx(&processor);
  if (status_.processor_samples && (processor.Group != status_.sampled_group || processor.Number != status_.sampled_cpu))
    ++status_.sampled_processor_changes;
  status_.sampled_group = processor.Group;
  status_.sampled_cpu = processor.Number;
  ++status_.processor_samples;
}

bool NativeCpuAffinity::Enable(uint32_t mode) {
  if (status_.active && !Restore()) return false;
  if (mode > 7) return Fail("invalid command-thread placement mode", ERROR_INVALID_PARAMETER);
  if (GetActiveProcessorGroupCount() != 1) {
    return Fail("performance affinity trial supports one processor group only", ERROR_NOT_SUPPORTED);
  }
  if (!impl_->thread) {
    impl_->thread_id = GetCurrentThreadId();
    impl_->thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE,
                               FALSE, impl_->thread_id);
    if (!impl_->thread) return Fail("could not open calling command thread", GetLastError());
    status_.thread_id = impl_->thread_id;
  }
  if (GetProcessIdOfThread(impl_->thread) != GetCurrentProcessId() ||
      WaitForSingleObject(impl_->thread, 0) != WAIT_TIMEOUT) {
    return Fail("captured command thread is no longer live in this process", ERROR_INVALID_HANDLE);
  }
  GROUP_AFFINITY original{};
  PROCESSOR_NUMBER ideal{};
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (!GetThreadGroupAffinity(impl_->thread, &original) ||
      !GetThreadIdealProcessorEx(impl_->thread, &ideal) ||
      !GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
    return Fail("could not capture original command-thread scheduling state", GetLastError());
  }
  if (original.Group != 0 || ideal.Group != 0) {
    return Fail("command thread is outside supported group zero", ERROR_NOT_SUPPORTED);
  }

  // CPU sets are a separate soft scheduling preference. Keep this experiment
  // scoped to the measured default-empty case, rather than overriding someone
  // else's selection with a conflicting hard-affinity mask.
  ULONG selected_count = 0;
  SetLastError(ERROR_SUCCESS);
  const BOOL selected_ok = GetThreadSelectedCpuSets(impl_->thread, nullptr, 0, &selected_count);
  const DWORD selected_error = GetLastError();
  if (!selected_ok && !(selected_count && selected_error == ERROR_INSUFFICIENT_BUFFER)) {
    return Fail("could not query command-thread selected CPU sets", selected_error);
  }
  if (selected_count) return Fail("command thread already selects explicit CPU sets", ERROR_NOT_SUPPORTED);
  ULONG default_count = 0;
  SetLastError(ERROR_SUCCESS);
  const BOOL default_ok = GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &default_count);
  const DWORD default_error = GetLastError();
  if (!default_ok && !(default_count && default_error == ERROR_INSUFFICIENT_BUFFER)) {
    return Fail("could not query process default CPU sets", default_error);
  }
  if (default_count) return Fail("process already selects default CPU sets", ERROR_NOT_SUPPORTED);

  ULONG bytes = 0;
  SetLastError(ERROR_SUCCESS);
  GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
  if (!bytes || bytes > 1024 * 1024) {
    return Fail("CPU-set topology size is unavailable or excessive", GetLastError());
  }
  std::vector<uint8_t> storage(bytes);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()),
                                  bytes, &bytes, GetCurrentProcess(), 0)) {
    return Fail("CPU-set topology query failed", GetLastError());
  }
  if (bytes > storage.size()) return Fail("CPU topology changed during query", ERROR_INVALID_DATA);
  std::vector<NativeCpuTopologyEntry> entries;
  entries.reserve(64);
  size_t offset = 0;
  while (offset < bytes) {
    if (bytes - offset < sizeof(DWORD) * 2) return Fail("truncated CPU-set record", ERROR_INVALID_DATA);
    DWORD size = 0;
    CPU_SET_INFORMATION_TYPE type{};
    std::memcpy(&size, storage.data() + offset, sizeof(size));
    std::memcpy(&type, storage.data() + offset + sizeof(DWORD), sizeof(type));
    if (size < sizeof(DWORD) * 2 || size > bytes - offset)
      return Fail("invalid CPU-set record extent", ERROR_INVALID_DATA);
    if (type == CpuSetInformation) {
      if (size < sizeof(SYSTEM_CPU_SET_INFORMATION))
        return Fail("CPU-set record is too short", ERROR_INVALID_DATA);
      SYSTEM_CPU_SET_INFORMATION info{};
      std::memcpy(&info, storage.data() + offset, sizeof(info));
      const bool reserved_elsewhere = info.CpuSet.Allocated && !info.CpuSet.AllocatedToTargetProcess;
      entries.push_back({info.CpuSet.Group, info.CpuSet.LogicalProcessorIndex,
                         info.CpuSet.EfficiencyClass, reserved_elsewhere, info.CpuSet.SchedulingClass});
    }
    offset += size;
  }
  const auto selection = SelectNativePerformanceAffinity(entries, original.Group, original.Mask,
                                                         process_mask, system_mask);
  if (selection.result != NativeCpuAffinitySelectionResult::kReady) {
    const char* reason = "invalid CPU topology for performance affinity";
    switch (selection.result) {
      case NativeCpuAffinitySelectionResult::kHomogeneousTopology:
        reason = "Windows exposes no distinct performance-core class"; break;
      case NativeCpuAffinitySelectionResult::kEmptyAllowedMask:
        reason = "original/process/system affinity intersection is empty"; break;
      case NativeCpuAffinitySelectionResult::kNoAllowedPerformanceCore:
        reason = "original affinity permits no available performance-class CPU"; break;
      case NativeCpuAffinitySelectionResult::kUnsupportedGroup:
        reason = "performance affinity requires group zero"; break;
      default: break;
    }
    return Fail(reason, ERROR_NOT_SUPPORTED);
  }
  const auto placement = SelectNativeCpuPlacement(selection, mode);
  if (!placement.valid) return Fail("invalid performance-core placement", ERROR_INVALID_DATA);
  impl_->original_affinity = original;
  impl_->original_ideal = ideal;
  status_.group = original.Group;
  status_.original_ideal_cpu = ideal.Number;
  status_.original_mask = original.Mask;
  status_.performance_mask = selection.performance_mask;
  status_.performance_class = selection.efficiency_class;
  status_.scheduling_mask = selection.scheduling_mask;
  status_.scheduling_class = selection.scheduling_class;

  // No allocations or string formatting follow scheduling mutations. Preserve
  // the actual previous mask returned by Windows, then
  // abandon the trial if another scheduling writer raced our original read.
  const DWORD_PTR previous = SetThreadAffinityMask(impl_->thread, DWORD_PTR(placement.mask));
  if (!previous) return Fail("SetThreadAffinityMask failed", GetLastError());
  impl_->original_affinity.Mask = previous;
  status_.original_mask = previous;
  status_.active = true;
  if (previous != original.Mask) {
    Restore();
    return Fail("command-thread affinity changed during preparation", ERROR_RETRY);
  }
  if (placement.set_ideal) {
    // Owner-controlled CP scheduling: the captured pre-mask ideal is the
    // restoration baseline. Windows may adjust ideal state when affinity is
    // narrowed, so that intermediate ideal is not a replacement baseline.
    PROCESSOR_NUMBER requested_ideal{};
    requested_ideal.Group = original.Group;
    requested_ideal.Number = placement.ideal_cpu;
    if (!SetThreadIdealProcessorEx(impl_->thread, &requested_ideal, nullptr)) {
      const DWORD error = GetLastError();
      Restore();
      return Fail("SetThreadIdealProcessorEx failed", error);
    }
  }
  GROUP_AFFINITY applied{};
  PROCESSOR_NUMBER applied_ideal{};
  if (!GetThreadGroupAffinity(impl_->thread, &applied) ||
      !GetThreadIdealProcessorEx(impl_->thread, &applied_ideal) || applied.Group != 0 ||
      uint64_t(applied.Mask) != placement.mask || applied_ideal.Group != original.Group ||
      (placement.set_ideal && applied_ideal.Number != placement.ideal_cpu)) {
    const DWORD error = GetLastError();
    Restore();
    return Fail("performance placement readback did not match request", error ? error : ERROR_INVALID_DATA);
  }
  status_.placement_mode = mode;
  status_.applied_mask = applied.Mask;
  status_.current_ideal_group = applied_ideal.Group;
  status_.current_ideal_cpu = applied_ideal.Number;
  ++status_.enables;
  status_.reason = "performance-core placement applied";
  status_.windows_error = ERROR_SUCCESS;
  return true;
}

bool NativeCpuAffinity::Restore() noexcept {
  if (!impl_ || !status_.active) return true;
  if (!impl_->thread || WaitForSingleObject(impl_->thread, 0) == WAIT_OBJECT_0) {
    status_.active = false;
    status_.reason = "command thread exited; scheduling state no longer exists";
    status_.windows_error = ERROR_SUCCESS;
    return true;
  }
  if (!SetThreadAffinityMask(impl_->thread, impl_->original_affinity.Mask)) {
    return Fail("could not restore original command-thread affinity", GetLastError());
  }
  if (!SetThreadIdealProcessorEx(impl_->thread, &impl_->original_ideal, nullptr)) {
    // Affinity is already restored, but keep active=true to retry ideal restore
    // during Shutdown rather than claiming the entire baseline was restored.
    return Fail("affinity restored but original ideal processor restore failed", GetLastError());
  }
  GROUP_AFFINITY restored{};
  PROCESSOR_NUMBER ideal{};
  if (!GetThreadGroupAffinity(impl_->thread, &restored) ||
      !GetThreadIdealProcessorEx(impl_->thread, &ideal) ||
      restored.Group != impl_->original_affinity.Group ||
      restored.Mask != impl_->original_affinity.Mask ||
      ideal.Group != impl_->original_ideal.Group || ideal.Number != impl_->original_ideal.Number) {
    return Fail("original scheduling state readback did not match", ERROR_INVALID_DATA);
  }
  status_.active = false;
  status_.applied_mask = restored.Mask;
  status_.current_ideal_group = ideal.Group;
  status_.current_ideal_cpu = ideal.Number;
  ++status_.restores;
  status_.reason = "original affinity and ideal processor restored";
  status_.windows_error = ERROR_SUCCESS;
  return true;
}

bool NativeCpuAffinity::Shutdown() noexcept {
  if (!impl_) return true;
  impl_->last_requested = false;
  impl_->last_result = Restore();
  return impl_->last_result;
}
}  // namespace nb::gpu
