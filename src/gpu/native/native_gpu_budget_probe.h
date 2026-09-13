// Original opt-in, CP-owned diagnostics. GPU resources retire by the original
// direct-queue submission fence. No rendering, queue or guest timing changes.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rex::graphics::d3d12 { class DeferredCommandList; }
namespace nb::gpu {

class NativeGpuBudgetProbe final {
 public:
  using List = rex::graphics::d3d12::DeferredCommandList;
  enum class Publication { kPending, kWritten, kFailed };
  static std::unique_ptr<NativeGpuBudgetProbe> Create(
      ID3D12Device* device, ID3D12CommandQueue* queue, std::string path,
      uint32_t start_seconds, uint32_t frame_count, uint32_t group_period);
  ~NativeGpuBudgetProbe();
  NativeGpuBudgetProbe(const NativeGpuBudgetProbe&) = delete;
  NativeGpuBudgetProbe& operator=(const NativeGpuBudgetProbe&) = delete;

  // Called on the CP, not the replay worker. Time/window selection is cached
  // once per new swap frame, never parsed or clocked from a draw operation.
  // Also called before the CP's already-open-submission fast return, so a
  // failed swap cannot hide a frame boundary inside an existing submission.
  void ObserveFrame(uint64_t frame);
  void BeginSubmission(List& list, uint64_t frame, uint64_t resource_frame,
                       uint64_t submission);
  void EndSubmission(List& list, bool closes_frame, uint64_t transfers,
                     uint64_t tiles, uint64_t dumps);
  // Only four existing EDRAM leaf labels are recognized. Native/emulated draw
  // scopes and operation-kind changes NEVER allocate timestamp pairs.
  uint32_t EnterScope(List& list, const char* name, uint64_t detail0);
  void LeaveScope(List& list, uint32_t category);
  // op: 0 draw, 1 dispatch, 2 buffer copy, 3 texture copy, 4 clear, 5 resource copy.
  void Operation(List& list, uint32_t op, uint64_t copy_bytes);
  Publication TryFinish(uint64_t completed_submission);
  bool Finish(bool completion_proven, uint64_t completed_submission);
  bool finished() const { return finished_; }
  bool recording_active() const { return current_submission_ != kNoSlot; }
  bool capture_ended() const { return first_frame_ && observed_frame_ >= first_frame_ + frame_count_; }
  // Census every diagnostic frame, timestamp only the six selected frames.
  bool scopes_active() const { return current_submission_ != kNoSlot; }

 private:
  NativeGpuBudgetProbe() = default;
  static constexpr uint32_t kNoSlot = UINT32_MAX;
  static constexpr uint32_t kSubmissionCapacity = 8192;
  static constexpr uint32_t kGroupCapacity = 3072;
  static constexpr uint32_t kGroupsPerFrame = 512;
  enum Category : uint32_t {
    kOther, kTransfer, kStencil1, kStencil2, kStencil4, kDump, kResolveCopy
  };
  struct Submission {
    uint64_t frame, resource_frame, submission;
    uint32_t first_group;
    bool ended = false, closes_frame = false;
    uint64_t operations = 0, draws = 0, dispatches = 0, copy_bytes = 0;
    uint64_t transfers = 0, tiles = 0, dumps = 0;
    uint64_t scope_entries = 0, scope_operations = 0, scope_draws = 0;
    uint64_t scope_dispatches = 0, scope_copy_bytes = 0;
    uint64_t category_entries[kResolveCopy + 1] = {};
  };
  struct Group {
    uint64_t frame, submission;
    uint32_t category;
    uint64_t operations = 0, draws = 0, dispatches = 0, copy_bytes = 0;
    bool ended = false;
  };
  struct FormatCaps {
    DXGI_FORMAT format;
    bool support_valid = false, msaa_valid = false;
    uint32_t support1 = 0, support2 = 0, msaa2_levels = 0;
  };
  void EndGroup(List& list);
  void RetainInFlight();
  static const char* CategoryName(uint32_t category);
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> submission_queries_, group_queries_;
  Microsoft::WRL::ComPtr<ID3D12Resource> submission_readback_, group_readback_;
  std::vector<Submission> submissions_;
  std::vector<Group> groups_;
  std::vector<FormatCaps> formats_;
  D3D12_FEATURE_DATA_D3D12_OPTIONS options_{};
  bool options_valid_ = false;
  std::string path_;
  std::chrono::steady_clock::time_point first_frame_time_{};
  uint64_t observed_frame_ = UINT64_MAX, first_frame_ = 0, last_submission_ = 0;
  uint64_t gpu_frequency_ = 0, submission_overflow_ = 0, group_overflow_ = 0;
  uint64_t previous_transfers_ = 0, previous_tiles_ = 0, previous_dumps_ = 0;
  bool counter_reset_ = false;
  bool scope_error_ = false;
  bool alignment_error_ = false;
  uint32_t frame_closes_ = 0;
  uint32_t start_seconds_ = 0, frame_count_ = 0, group_period_ = 0;
  uint32_t current_submission_ = kNoSlot, current_group_ = kNoSlot;
  uint32_t groups_this_frame_ = 0, scope_ = kOther;
  bool frame_selected_ = false, groups_selected_ = false;
  bool groups_truncated_ = false, finished_ = false, completion_proven_ = false;
};
}  // namespace nb::gpu
