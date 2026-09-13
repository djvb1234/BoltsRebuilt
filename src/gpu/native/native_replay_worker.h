// Original optional CPU replay worker. This does not change GPU queue ordering,
// fence values, resource states, draw payloads, or guest timing.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <d3d12.h>
#include <wrl/client.h>
#include <rex/graphics/d3d12/deferred_command_list.h>

namespace nb::gpu {

class NativeReplayWorker final {
 public:
  using CommandProcessor = rex::graphics::d3d12::D3D12CommandProcessor;
  using Stream = rex::graphics::d3d12::DeferredCommandList;
  // Construction/resource/thread failure disables this optional path before
  // any caller stream is touched. No global thread scheduling changes.
  static std::unique_ptr<NativeReplayWorker> TryCreate(
      ID3D12Device* device, const CommandProcessor& cp) noexcept;
  ~NativeReplayWorker();
  NativeReplayWorker(const NativeReplayWorker&) = delete;
  NativeReplayWorker& operator=(const NativeReplayWorker&) = delete;

  // CP-only, after Wait and successful source.ResolvePipelineHandlesForReplay.
  // Source must share this worker's CP owner. Allocator must already be GPU-idle.
  // False leaves source untouched. True consumes it and transfers exclusive
  // storage ownership until Wait; reset the returned scratch before recording.
  // Allocator/queue/fence are AddRef'd for the CPU job. The CP's existing fence
  // lifecycle must retain ALL referenced resources until actual GPU completion.
  bool Submit(Stream& source, ID3D12CommandAllocator* allocator,
              ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) noexcept;
  // A chunk session records ONE command list and signals ONE reserved fence.
  // Begin resets the allocator/list; only Finish closes, executes and signals.
  // Every source must be preflighted on the CP. Once Begin succeeds, the caller
  // must finish this session or handle failure; it cannot replay a tail elsewhere.
  bool BeginChunks(Stream& source, ID3D12CommandAllocator* allocator,
                   ID3D12CommandQueue* queue, ID3D12Fence* fence,
                   uint64_t value) noexcept;
  // Reaps a completed CPU slot but never waits for an executing chunk. Readiness
  // alone does not imply a session exists; Begin/Append enforce that separately.
  bool ReadyForChunk() noexcept;
  bool TryAppendChunk(Stream& source) noexcept;
  // Called after Wait. Accepts even an empty preflighted final tail. The session's
  // original allocator, queue, fence and value remain unchanged.
  bool FinishChunks(Stream& source) noexcept;
  // Joins only the current CPU replay/queue-submission job, NOT the GPU fence.
  // Intermediate chunk waits retain the session and do NOT submit it. Callers
  // needing GPU completion must Finish first. Failure never fabricates a fence.
  HRESULT Wait() noexcept;
  HRESULT Result() const noexcept { return result_.load(std::memory_order_acquire); }

  struct JobResult {
    uint64_t fence_value = 0;
    HRESULT allocator_reset = E_PENDING, list_reset = E_PENDING;
    HRESULT replay = E_PENDING, close = E_PENDING, signal = E_PENDING;
    HRESULT result = S_OK;
    bool queue_execute_attempted = false;
  };
  struct Stats {
    uint64_t submitted = 0, completed = 0, failed = 0;
    uint64_t chunk_sessions = 0, chunks_submitted = 0, chunks_completed = 0;
    uint64_t chunk_sessions_completed = 0, chunks_failed = 0;
    JobResult last;
  };
  // CP-only. Original job counts/last describe Whole or Finish only. Chunk counts
  // include Begin, Append and Finish. Completion is published by Wait or a reap.
  const Stats& stats() const noexcept { return stats_; }

 private:
  explicit NativeReplayWorker(const CommandProcessor& cp);
  bool Initialize(ID3D12Device* device);
  void Run() noexcept;
  HRESULT ExecuteJob() noexcept;
  void ReapCompletedLocked() noexcept;
  bool Start(Stream& source, ID3D12CommandAllocator* allocator,
             ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value,
             bool chunks) noexcept;
  enum class State { kIdle, kQueued, kRunning, kComplete };
  enum class Operation { kWhole, kBegin, kAppend, kFinish };
  // Preconstructed before thread launch. Swapped only while worker is idle.
  Stream stream_;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> initial_allocator_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> list_1_;
  struct Job {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    JobResult report;
    Operation operation = Operation::kWhole;
  } job_;
  Stream::ReplayState replay_state_;
  std::mutex mutex_;
  std::condition_variable condition_;
  State state_ = State::kIdle;
  bool stop_ = false;
  bool session_active_ = false;  // CP-only, protected by mutex_ with slot state
  std::atomic<HRESULT> result_{S_OK};
  Stats stats_;  // owning CP only
  // Declared last, launched only after every earlier member/resource is ready.
  std::thread thread_;
};

}  // namespace nb::gpu
