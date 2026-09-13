// Original bounded, opt-in backend direct-queue timing. No game data.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rex::graphics::d3d12 { class DeferredCommandList; }
namespace nb::gpu {

// Each submission owns two unique query indices and a unique readback range.
// Nothing is reused or read until the caller proves the captured queue work is complete.
// These are bottom-of-pipe elapsed intervals, not GPU busy time or Present time.
class NativeGpuSubmissionTiming {
 public:
  static constexpr uint32_t kCapacity = 8192;
  static constexpr uint32_t kNoSlot = UINT32_MAX;
  static std::unique_ptr<NativeGpuSubmissionTiming> Create(
      ID3D12Device* device, ID3D12CommandQueue* queue, std::string path,
      uint64_t first_frame, uint64_t frame_count);
  ~NativeGpuSubmissionTiming();
  NativeGpuSubmissionTiming(const NativeGpuSubmissionTiming&) = delete;
  NativeGpuSubmissionTiming& operator=(const NativeGpuSubmissionTiming&) = delete;

  uint32_t Begin(ID3D12GraphicsCommandList* list, uint64_t guest_frame,
                 uint64_t resource_frame, uint64_t submission, bool frame_open, bool closes_frame);
  void End(ID3D12GraphicsCommandList* list, uint32_t slot);
  void BeforeExecute(uint32_t slot);
  void AfterExecute(uint32_t slot);
  void Submitted(uint32_t slot, bool close_succeeded, bool signal_succeeded);
  void FailedReset(uint64_t guest_frame);
  // Sparse pass diagnostics use unique queries in the original deferred order.
  // Names must be static literals. Period zero allocates/records nothing.
  bool EnablePassTiming(uint32_t period, bool include_geometry = false);
  uint32_t BeginPass(rex::graphics::d3d12::DeferredCommandList& list,
      uint64_t frame, uint64_t submission, const char* name,
      uint64_t detail0, uint64_t detail1, uint64_t detail2);
  void EndPass(rex::graphics::d3d12::DeferredCommandList& list, uint32_t slot);
  void SubmittedPasses(uint64_t submission, bool valid);
  enum class Publication { kPending, kWritten, kFailed };
  bool finished() const { return finished_; }
  bool NeedsCompletionCheck(uint64_t guest_frame) const;
  Publication TryFinishAfterCapture(uint64_t guest_frame, uint64_t completed_submission);
  // Call once, after the captured submissions finish. Failure never fabricates timing.
  // If GPU completion is unproven, retain COM resources until process teardown.
  bool Finish(bool capture_gpu_complete, uint64_t completed_submission);

 private:
  NativeGpuSubmissionTiming() = default;
  struct Record {
    uint64_t guest_frame = 0, resource_frame = 0, submission = 0;
    uint64_t record_begin = 0, record_end = 0, execute_begin = 0, execute_end = 0;
    uint64_t gpu_begin = 0, gpu_end = 0;
    bool frame_open = false, closes_frame = false, submitted = false;
  };
  void RetainInFlightResources();
  static constexpr uint32_t kPassCapacity = 65536;
  struct PassRecord {
    uint64_t frame, submission, detail0, detail1, detail2;
    const char* name;
    bool ended = false, submitted = false;
  };
  bool FinishPasses(bool completion_proven, uint64_t completed_submission);
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> pass_queries_;
  Microsoft::WRL::ComPtr<ID3D12Resource> pass_readback_;
  std::vector<PassRecord> passes_;
  uint32_t pass_period_ = 0, pass_submitted_count_ = 0;
  bool pass_include_geometry_ = false;
  uint64_t pass_overflow_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  std::vector<Record> records_;
  std::string path_;
  uint64_t first_frame_ = 0, frame_count_ = 0;
  uint64_t gpu_frequency_ = 0, cpu_frequency_ = 0, overflow_ = 0;
  uint64_t reset_failures_ = 0;
  bool invalid_ = false, finished_ = false;
};

}  // namespace nb::gpu
