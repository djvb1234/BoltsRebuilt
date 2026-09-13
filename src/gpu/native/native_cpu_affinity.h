// Original opt-in scheduling experiment for the calling GPU command thread.
// Update at frame boundaries only. No scheduling changes occur by default.
#pragma once
#include <cstdint>
#include <memory>

namespace nb::gpu {
class NativeCpuAffinity {
 public:
  struct Status {
    bool active = false;
    uint32_t thread_id = 0;
    uint16_t group = 0;
    uint8_t original_ideal_cpu = 0, performance_class = 0;
    uint64_t original_mask = 0, performance_mask = 0;
    uint64_t scheduling_mask = 0;
    uint8_t scheduling_class = 0;
    uint32_t placement_mode = 0;
    uint64_t applied_mask = 0;
    uint16_t current_ideal_group = 0, sampled_group = 0;
    uint8_t current_ideal_cpu = 0, sampled_cpu = 0;
    uint64_t processor_samples = 0, sampled_processor_changes = 0;
    uint64_t enables = 0, restores = 0, failures = 0;
    uint32_t windows_error = 0;
    const char* reason = "disabled";
  };
  NativeCpuAffinity();
  ~NativeCpuAffinity();
  NativeCpuAffinity(const NativeCpuAffinity&) = delete;
  NativeCpuAffinity& operator=(const NativeCpuAffinity&) = delete;

  // First enable captures this calling thread. Later transitions must come from
  // the same thread. Modes:0 P mask,1 P mask plus highest allowed ideal,2 pin
  // highest,3 pin lowest. A mode change restores the original state first.
  // Modes4..7 apply those policies to the highest observed scheduling-class
  // subset of permitted P cores; this is metadata to test, not a speed promise.
  // The owner must be the sole scheduling writer for this captured CP thread
  // through disable/Shutdown. Separate Windows mask/ideal APIs are not atomic.
  // Repeated requests latch failures and perform no scheduling writes. Active
  // frame boundaries sample the calling CPU; this is not a full migration trace.
  bool Update(bool enabled, uint32_t mode = 0);
  // May be called by the owner during shutdown, before the CP thread exits.
  // Returns false if a live thread's original state could not be restored.
  bool Shutdown() noexcept;
  const Status& status() const { return status_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Status status_;
  bool Fail(const char* reason, uint32_t error) noexcept;
  bool Enable(uint32_t mode);
  bool Restore() noexcept;
  void SampleProcessor() noexcept;
};
}  // namespace nb::gpu
