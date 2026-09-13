#include "native_replay_worker.h"

#include <new>
#include <system_error>

namespace nb::gpu {

NativeReplayWorker::NativeReplayWorker(const CommandProcessor& cp) : stream_(cp) {}

std::unique_ptr<NativeReplayWorker> NativeReplayWorker::TryCreate(
    ID3D12Device* device, const CommandProcessor& cp) noexcept {
  if (!device) return nullptr;
  try {
    auto worker = std::unique_ptr<NativeReplayWorker>(new NativeReplayWorker(cp));
    if (!worker->Initialize(device)) return nullptr;
    worker->thread_ = std::thread(&NativeReplayWorker::Run, worker.get());
    return worker;
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (const std::system_error&) {
    return nullptr;
  }
}

bool NativeReplayWorker::Initialize(ID3D12Device* device) {
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&initial_allocator_))) || !initial_allocator_) return false;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      initial_allocator_.Get(), nullptr, IID_PPV_ARGS(&list_))) || !list_) return false;
  if (FAILED(list_->Close())) return false;
  // If list1 is unavailable, retain the original CP path, including its sample
  // position support decision, rather than silently omit an encoded operation.
  if (FAILED(list_.As(&list_1_)) || !list_1_) return false;
  return true;
}

NativeReplayWorker::~NativeReplayWorker() {
  // The owning CP must still prove GPU retirement before releasing its resource
  // pools. Joining here proves only that this worker will make no more API calls.
  Wait();
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  condition_.notify_one();
  if (thread_.joinable()) thread_.join();
}

bool NativeReplayWorker::Submit(Stream& source, ID3D12CommandAllocator* allocator,
                                ID3D12CommandQueue* queue, ID3D12Fence* fence,
                                uint64_t value) noexcept {
  return Start(source, allocator, queue, fence, value, false);
}

bool NativeReplayWorker::BeginChunks(Stream& source,
                                     ID3D12CommandAllocator* allocator,
                                     ID3D12CommandQueue* queue,
                                     ID3D12Fence* fence, uint64_t value) noexcept {
  return Start(source, allocator, queue, fence, value, true);
}

bool NativeReplayWorker::Start(Stream& source, ID3D12CommandAllocator* allocator,
                               ID3D12CommandQueue* queue, ID3D12Fence* fence,
                               uint64_t value, bool chunks) noexcept {
  if (!allocator || !queue || !fence || !value || value == UINT64_MAX ||
      !source.pipeline_handles_resolved_for_replay()) return false;
  {
    std::lock_guard lock(mutex_);
    if (stop_ || !thread_.joinable() || state_ != State::kIdle ||
        session_active_ || FAILED(Result())) return false;
    // No allocation or fallible setup after consuming source. ComPtr assignment
    // only retains existing objects; this fixed slot is never a growing queue.
    job_.allocator = allocator;
    job_.queue = queue;
    job_.fence = fence;
    job_.report = JobResult{};
    job_.report.fence_value = value;
    job_.operation = chunks ? Operation::kBegin : Operation::kWhole;
    stream_.SwapStorage(source);
    session_active_ = chunks;
    state_ = State::kQueued;
    if (chunks) {
      ++stats_.chunk_sessions;
      ++stats_.chunks_submitted;
    } else {
      ++stats_.submitted;
    }
  }
  condition_.notify_one();
  return true;
}

bool NativeReplayWorker::ReadyForChunk() noexcept {
  std::lock_guard lock(mutex_);
  ReapCompletedLocked();
  return !stop_ && thread_.joinable() && state_ == State::kIdle &&
         SUCCEEDED(Result());
}

bool NativeReplayWorker::TryAppendChunk(Stream& source) noexcept {
  if (!source.pipeline_handles_resolved_for_replay()) return false;
  {
    std::lock_guard lock(mutex_);
    ReapCompletedLocked();
    if (stop_ || !thread_.joinable() || state_ != State::kIdle ||
        !session_active_ || FAILED(Result())) return false;
    job_.operation = Operation::kAppend;
    stream_.SwapStorage(source);
    state_ = State::kQueued;
    ++stats_.chunks_submitted;
  }
  condition_.notify_one();
  return true;
}

bool NativeReplayWorker::FinishChunks(Stream& source) noexcept {
  if (!source.pipeline_handles_resolved_for_replay()) return false;
  {
    std::lock_guard lock(mutex_);
    if (stop_ || !thread_.joinable() || state_ != State::kIdle ||
        !session_active_ || FAILED(Result())) return false;
    job_.operation = Operation::kFinish;
    stream_.SwapStorage(source);
    state_ = State::kQueued;
    ++stats_.chunks_submitted;
    ++stats_.submitted;
  }
  condition_.notify_one();
  return true;
}

void NativeReplayWorker::ReapCompletedLocked() noexcept {
  if (state_ != State::kComplete) return;
  const bool final = job_.operation == Operation::kWhole ||
                     job_.operation == Operation::kFinish;
  const bool failed = FAILED(job_.report.result);
  if (job_.operation != Operation::kWhole) {
    ++stats_.chunks_completed;
    if (failed) ++stats_.chunks_failed;
  }
  if (final) {
    stats_.last = job_.report;
    ++stats_.completed;
    if (failed) ++stats_.failed;
    if (job_.operation == Operation::kFinish && !failed) {
      ++stats_.chunk_sessions_completed;
    }
    job_.allocator.Reset();
    job_.queue.Reset();
    job_.fence.Reset();
    session_active_ = false;
  }
  // An intermediate failure remains a latched, unsubmitted session. The caller
  // handles device failure; retaining its references here never invents a fence.
  state_ = State::kIdle;
}

HRESULT NativeReplayWorker::Wait() noexcept {
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [this] { return state_ == State::kIdle || state_ == State::kComplete; });
  ReapCompletedLocked();
  return Result();
}

void NativeReplayWorker::Run() noexcept {
  // Diagnostic identity only; leave affinity, priority and scheduling unchanged.
  (void)SetThreadDescription(GetCurrentThread(), L"Native D3D12 replay");
  std::unique_lock lock(mutex_);
  while (true) {
    condition_.wait(lock, [this] { return stop_ || state_ == State::kQueued; });
    if (stop_) return;
    state_ = State::kRunning;
    lock.unlock();
    const HRESULT result = ExecuteJob();
    // Sticky failure is available to the CP before it waits for a missing GPU
    // fence. Never attempt later jobs or signal a replacement completion value.
    if (FAILED(result)) result_.store(result, std::memory_order_release);
    lock.lock();
    job_.report.result = result;
    state_ = State::kComplete;
    condition_.notify_all();
  }
}

HRESULT NativeReplayWorker::ExecuteJob() noexcept {
  auto& report = job_.report;
  try {
    if (job_.operation == Operation::kWhole || job_.operation == Operation::kBegin) {
      report.allocator_reset = job_.allocator->Reset();
      if (FAILED(report.allocator_reset)) return report.allocator_reset;
      report.list_reset = list_->Reset(job_.allocator.Get(), nullptr);
      if (FAILED(report.list_reset)) return report.list_reset;
      replay_state_ = {};
    }
    // Preflight changed every pipeline handle to a direct pointer on the CP;
    // Execute therefore touches only owned stream bytes and D3D12 interfaces.
    stream_.Execute(list_.Get(), list_1_.Get(),
                    job_.operation == Operation::kWhole ? nullptr : &replay_state_);
    report.replay = S_OK;
    if (job_.operation == Operation::kBegin || job_.operation == Operation::kAppend) {
      return S_OK;
    }
    report.close = list_->Close();
    if (FAILED(report.close)) return report.close;
    ID3D12CommandList* lists[] = {list_.Get()};
    report.queue_execute_attempted = true;
    job_.queue->ExecuteCommandLists(1, lists);
    report.signal = job_.queue->Signal(job_.fence.Get(), report.fence_value);
    return report.signal;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_UNEXPECTED;
  }
}

}  // namespace nb::gpu
