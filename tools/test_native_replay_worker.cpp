// Original bounded GPU control for ACTUAL NativeReplayWorker and deferred
// storage/preflight/replay. Synthetic bytes only, isolated CP resolver stub.
// This does not prove production CP cache/watch/presentation integration.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <immintrin.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <rex/graphics/flags.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include "native_replay_worker.h"
using Microsoft::WRL::ComPtr;
using Stream = rex::graphics::d3d12::DeferredCommandList;
using Worker = nb::gpu::NativeReplayWorker;
namespace {
constexpr size_t kBytes = 65536;
constexpr std::array<uint64_t, 3> kFenceValues{11, 29, 47};
unsigned checks = 0;
void Require(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
void Check(HRESULT hr, const char* message) {
  if (FAILED(hr)) { std::fprintf(stderr, "%s HRESULT=%08X\n", message, unsigned(hr));
    throw std::runtime_error(message); }
}
[[noreturn]] void Fatal(const char* message) noexcept {
  std::fprintf(stderr, "FAIL: %s; terminate without releasing potentially in-flight objects\n", message);
  std::fflush(stderr);
  TerminateProcess(GetCurrentProcess(), 3);
  std::abort();
}
struct Event {
  HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Event() { if (!value) throw std::runtime_error("event creation"); }
  ~Event() { CloseHandle(value); }
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;
};
// Actual COM allocator interface; fails before the worker can pass it to D3D12.
class FailingAllocator final : public ID3D12CommandAllocator {
 public:
  std::atomic<ULONG> refs{1};
  std::atomic<unsigned> resets{0};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
        iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12CommandAllocator)) {
      *result = static_cast<ID3D12CommandAllocator*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** result) override {
    if (result) *result = nullptr; return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override { ++resets; return E_FAIL; }
};
uint8_t Literal(unsigned seed, size_t offset) {
  uint32_t value = uint32_t(offset) * 0x7FEB352Du + seed * 0x9E3779B9u;
  value ^= value >> 15; value *= 0x846CA68Bu; value ^= value >> 16;
  return uint8_t(value);
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type,
                            D3D12_RESOURCE_STATES state, size_t bytes = kBytes) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> result;
  Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                        nullptr, IID_PPV_ARGS(&result)), "buffer creation");
  return result;
}
void Barrier(Stream& stream, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  stream.D3DResourceBarrier(1, &barrier);
}
class Probe {
 public:
  explicit Probe(bool warp) : stream_(cp_, 8) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer(); debug_enabled_ = true;
    }
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    if (warp) {
      ComPtr<IDXGIAdapter> adapter;
      Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
      Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device_)), "WARP device");
      std::puts("adapter=WARP functional_only=true");
    } else {
      for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(i, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        Check(result, "adapter enumeration");
        DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc), "adapter description");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)))) {
          std::printf("adapter_vendor=%04X device=%04X\n", desc.VendorId, desc.DeviceId); break;
        }
      }
      Require(bool(device_), "hardware adapter unavailable; use explicit --warp");
    }
    if (debug_enabled_) device_.As(&info_);
    std::printf("debug_enabled=%u info_queue=%u\n", unsigned(debug_enabled_), unsigned(bool(info_)));
    if (!info_) std::puts("debug unavailable: no debug-clean claim");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Check(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)), "queue creation");
    Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "completion fence");
    Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_)), "GPU gate fence");
    Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&failure_fence_)), "failure-control fence");
    Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&proof_fence_)), "chunk queue-drain fence");
    for (auto& allocator : allocators_)
      Check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator)), "job allocator");
    upload_ = Buffer(device_.Get(), D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ, kBytes * 3);
    a_ = Buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    b_ = Buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    for (auto& readback : readbacks_)
      readback = Buffer(device_.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    // All source bytes are immutable until the final GPU fence. No WC reads.
    std::vector<uint8_t> source(kBytes * 3);
    for (size_t i = 0; i < source.size(); ++i) source[i] = Literal(unsigned(i / kBytes) + 1, i % kBytes);
    const D3D12_RANGE no_read{0, 0}; void* mapped = nullptr;
    Check(upload_->Map(0, &no_read, &mapped), "upload map");
    std::memcpy(mapped, source.data(), source.size()); _mm_sfence();
    const D3D12_RANGE written{0, source.size()}; upload_->Unmap(0, &written);
    Require(!Worker::TryCreate(nullptr, cp_), "null creation must refuse");
    worker_ = Worker::TryCreate(device_.Get(), cp_);
    Require(bool(worker_), "actual worker creation");
  }
  ~Probe() {
    if (unsafe_) Fatal("GPU completion not proven at fixture destruction");
    // Worker dies before any queue/allocator/resources, after explicit GPU idle.
    worker_.reset();
  }
  void Run() {
    Require(SUCCEEDED(worker_->Wait()) && worker_->stats().completed == 0, "initial idle Wait");
    Require(!Submit(0), "unresolved empty source refused");
    nb_native_deferred_storage = false; stream_.Reset();
    RecordFirst();
    Require(!Submit(0), "unresolved recorded source refused");
    Require(stream_.ResolvePipelineHandlesForReplay(), "job1 preflight");
    Require(!worker_->Submit(stream_, allocators_[0].Get(), queue_.Get(), fence_.Get(), UINT64_MAX),
            "reserved fence value refused without consuming source");
    Require(stream_.pipeline_handles_resolved_for_replay(), "refusal retains source marker");
    // Queue wait is legal and prevents all three jobs reaching the GPU. CPU
    // replay/submission must complete independently. The CPU later opens it.
    unsafe_ = true;
    if (FAILED(queue_->Wait(gate_.Get(), 1))) Fatal("queue gate failed");
    Require(Submit(0), "job1 submission");
    // Returned storage belongs to CP. Reset and re-record it while job1 owns
    // another vector. Distinct dependent copies expose accidental aliasing.
    nb_native_deferred_storage = true; stream_.Reset();
    RecordSecond();
    Require(stream_.ResolvePipelineHandlesForReplay(), "job2 preflight");
    Require(!Submit(1), "occupied slot refuses until explicit Wait, even if CPU finished");
    Require(stream_.pipeline_handles_resolved_for_replay(), "occupied refusal retains source");
    CpuWait(0);
    Require(Submit(1), "same refused job2 remains intact and submits after Wait");
    nb_native_deferred_storage = false; stream_.Reset();
    RecordThird();
    Require(stream_.ResolvePipelineHandlesForReplay(), "job3 preflight");
    Require(!Submit(2), "second occupied-slot refusal");
    CpuWait(1);
    Require(Submit(2), "job3 submission");
    stream_.Reset();
    // This different, never-submitted command must not overwrite job3 storage.
    stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), kBytes, 17);
    CpuWait(2);
    if (FAILED(gate_->Signal(1))) Fatal("CPU release of GPU gate failed");
    if (FAILED(fence_->SetEventOnCompletion(kFenceValues.back(), gpu_done_.value))) Fatal("GPU completion arm failed");
    if (WaitForSingleObject(gpu_done_.value, 30000) != WAIT_OBJECT_0) Fatal("GPU completion timeout");
    const uint64_t completed = fence_->GetCompletedValue();
    if (completed != kFenceValues.back() || FAILED(device_->GetDeviceRemovedReason())) Fatal("GPU completion/device state invalid");
    unsafe_ = false;
    CheckReadbacks();
    ValidateDebug();
    Require(worker_->stats().submitted == 3 && worker_->stats().completed == 3 &&
            worker_->stats().failed == 0, "exact final job stats");
    Check(worker_->Wait(), "idle repeated Wait");
    Require(worker_->stats().completed == 3, "repeated Wait does not double count");
    ChunkControls();
    worker_.reset();
    FailureControl();
    worker_.reset();
    ChunkFailureControl();
    std::printf("PASS ordered_jobs=3 unsplit_controls=1 chunk_sessions=2 chunk_jobs=7 allocator_failure_controls=2 literal_bytes=%zu checks=%u CPU_Wait_is_not_GPU_completion=true\n", kBytes * 12, checks);
  }
 private:
  bool Submit(size_t job) {
    return worker_->Submit(stream_, allocators_[job].Get(), queue_.Get(), fence_.Get(), kFenceValues[job]);
  }
  void CpuWait(size_t job) {
    if (!ResetEvent(cpu_done_.value)) Fatal("CPU event reset failed");
    std::atomic<HRESULT> result{E_PENDING};
    std::thread waiter([&] {
      result.store(worker_->Wait(), std::memory_order_release);
      if (!SetEvent(cpu_done_.value)) Fatal("CPU waiter event failed");
    });
    if (WaitForSingleObject(cpu_done_.value, 5000) != WAIT_OBJECT_0) {
      gate_->Signal(1); Fatal("CPU Wait did not return while GPU was gated");
    }
    waiter.join();
    Check(result.load(std::memory_order_acquire), "CPU replay completion");
    Require(fence_->GetCompletedValue() == 0, "CPU Wait must not imply gated GPU completion");
    const auto& stats = worker_->stats();
    Require(stats.completed == job + 1 && stats.last.fence_value == kFenceValues[job], "exact completed job/fence ID");
    Require(stats.last.allocator_reset == S_OK && stats.last.list_reset == S_OK &&
            stats.last.replay == S_OK && stats.last.close == S_OK && stats.last.signal == S_OK &&
            stats.last.queue_execute_attempted, "all actual API stages completed");
    Check(worker_->Wait(), "repeated CPU Wait");
    Require(worker_->stats().completed == job + 1, "same job counted once");
  }
  void RecordFirst() {
    stream_.D3DCopyBufferRegion(a_.Get(), 0, upload_.Get(), 0, kBytes);
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stream_.D3DCopyResource(readbacks_[0].Get(), a_.Get());
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  }
  void RecordSecond() {
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stream_.D3DCopyResource(b_.Get(), a_.Get());
    stream_.D3DCopyBufferRegion(b_.Get(), 257, upload_.Get(), kBytes + 13, 8191);
    Barrier(stream_, b_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stream_.D3DCopyResource(readbacks_[1].Get(), b_.Get());
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(stream_, b_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  }
  void RecordThird() {
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(stream_, b_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stream_.D3DCopyResource(a_.Get(), b_.Get());
    stream_.D3DCopyBufferRegion(a_.Get(), 32769, upload_.Get(), 2 * kBytes + 29, 4097);
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stream_.D3DCopyResource(readbacks_[2].Get(), a_.Get());
    Barrier(stream_, a_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(stream_, b_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  }
  void BoundedCpuWait() {
    if (!ResetEvent(cpu_done_.value)) Fatal("chunk CPU event reset failed");
    std::atomic<HRESULT> result{E_PENDING};
    std::thread waiter([&] {
      result.store(worker_->Wait(), std::memory_order_release);
      if (!SetEvent(cpu_done_.value)) Fatal("chunk CPU waiter event failed");
    });
    if (WaitForSingleObject(cpu_done_.value, 5000) != WAIT_OBJECT_0)
      Fatal("chunk CPU Wait deadline");
    waiter.join();
    Check(result.load(std::memory_order_acquire), "chunk CPU recording completion");
  }
  void GpuWait(uint64_t value) {
    if (!ResetEvent(gpu_done_.value) ||
        FAILED(fence_->SetEventOnCompletion(value, gpu_done_.value)) ||
        WaitForSingleObject(gpu_done_.value, 30000) != WAIT_OBJECT_0 ||
        fence_->GetCompletedValue() != value ||
        FAILED(device_->GetDeviceRemovedReason())) Fatal("chunk GPU completion proof failed");
  }
  void ProveUnsubmitted(uint64_t submission_value, uint64_t proof_value) {
    // Wait has joined the CPU chunk. A separate queue signal proves any earlier
    // accidental Execute/Signal has retired before inspecting the reserved fence.
    if (FAILED(queue_->Signal(proof_fence_.Get(), proof_value)) ||
        !ResetEvent(gpu_done_.value) ||
        FAILED(proof_fence_->SetEventOnCompletion(proof_value, gpu_done_.value)) ||
        WaitForSingleObject(gpu_done_.value, 30000) != WAIT_OBJECT_0 ||
        proof_fence_->GetCompletedValue() != proof_value ||
        FAILED(device_->GetDeviceRemovedReason())) Fatal("intermediate queue-drain proof failed");
    Require(fence_->GetCompletedValue() == submission_value,
            "intermediate chunk must not execute/signal a submission");
  }
  void FreshTargets() {
    // Called only after completion of ALL earlier GPU work has been proven.
    stream_.Reset();
    a_ = Buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    b_ = Buffer(device_.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    for (auto& readback : readbacks_)
      readback = Buffer(device_.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  std::vector<uint8_t> ReadbackBytes() {
    std::vector<uint8_t> bytes(kBytes * 3);
    for (size_t job = 0; job < 3; ++job) {
      void* mapped = nullptr; const D3D12_RANGE read{0, kBytes};
      Check(readbacks_[job]->Map(0, &read, &mapped), "comparison readback map");
      std::memcpy(bytes.data() + kBytes * job, mapped, kBytes);
      const D3D12_RANGE no_write{0, 0}; readbacks_[job]->Unmap(0, &no_write);
    }
    return bytes;
  }
  void ChunkControls() {
    // Establish an actual single-list unsplit control, independently checked
    // against literal bytes. Both split sessions must equal this entire output.
    FreshTargets(); RecordFirst(); RecordSecond(); RecordThird();
    Require(stream_.ResolvePipelineHandlesForReplay(), "unsplit preflight");
    unsafe_ = true;
    Require(worker_->Submit(stream_, allocators_[0].Get(), queue_.Get(), fence_.Get(), 61),
            "unsplit control accepted");
    BoundedCpuWait(); GpuWait(61); unsafe_ = false;
    CheckReadbacks();
    const auto expected = ReadbackBytes();
    uint64_t previous_fence = 61, proof_value = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
      nb_native_deferred_storage = mode != 0;
      FreshTargets();
      stream_.D3DSetPipelineState(nullptr);  // Copies execute even with no PSO.
      RecordFirst();
      const size_t original_size = stream_.UsedBytes();
      Require(!worker_->BeginChunks(stream_, allocators_[0].Get(), queue_.Get(), fence_.Get(), 79),
              "unpreflighted Begin refuses before consuming bytes");
      Require(stream_.UsedBytes() == original_size, "Begin refusal preserves bytes");
      Require(stream_.ResolvePipelineHandlesForReplay(), "chunk Begin preflight");
      Stream copy(stream_);
      stream_.Reset();
      stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), kBytes, 17);
      const auto before = worker_->stats();
      const uint64_t value = mode ? 97 : 79;
      Require(worker_->ReadyForChunk(), "healthy empty slot ready");
      Require(!worker_->TryAppendChunk(copy) && copy.UsedBytes() == original_size,
              "Append without session preserves source");
      unsafe_ = true;
      Require(worker_->BeginChunks(copy, allocators_[0].Get(), queue_.Get(), fence_.Get(), value),
              "Begin consumes independent copied stream");
      copy.Reset();  // Does not overwrite storage now exclusively owned by worker.
      BoundedCpuWait(); ProveUnsubmitted(previous_fence, ++proof_value);
      Require(worker_->stats().submitted == before.submitted &&
              worker_->stats().completed == before.completed &&
              worker_->stats().chunks_completed == before.chunks_completed + 1,
              "Begin completes a chunk, not a submission");
      stream_.Reset(); RecordSecond();
      Require(stream_.ResolvePipelineHandlesForReplay(), "Append preflight");
      const size_t second_size = stream_.UsedBytes();
      Require(!worker_->Submit(stream_, allocators_[1].Get(), queue_.Get(), fence_.Get(), value) &&
              !worker_->BeginChunks(stream_, allocators_[1].Get(), queue_.Get(), fence_.Get(), value) &&
              stream_.UsedBytes() == second_size && stream_.pipeline_handles_resolved_for_replay(),
              "active session refuses new whole/session jobs without mutation");
      Require(worker_->TryAppendChunk(stream_), "Append accepted after intermediate Wait");
      stream_.Reset(); RecordThird();
      // Exercise nonblocking completed-slot reaping, with a bounded sleep only in
      // this fixture. Production merely skips an optional chunk when not ready.
      const ULONGLONG deadline = GetTickCount64() + 5000;
      while (!worker_->ReadyForChunk() && GetTickCount64() < deadline) Sleep(1);
      Require(worker_->ReadyForChunk(), "Append becomes ready without Wait");
      ProveUnsubmitted(previous_fence, ++proof_value);
      Require(worker_->stats().chunks_completed == before.chunks_completed + 2,
              "Ready reaps completed Append exactly once");
      Require(!worker_->FinishChunks(stream_), "unpreflighted Finish leaves tail intact");
      Require(stream_.ResolvePipelineHandlesForReplay(), "final copy tail preflight");
      if (mode) {
        Require(worker_->TryAppendChunk(stream_), "third nonfinal chunk accepted");
        stream_.Reset();
        BoundedCpuWait(); ProveUnsubmitted(previous_fence, ++proof_value);
        Require(stream_.UsedBytes() == 0 && stream_.ResolvePipelineHandlesForReplay(),
                "empty final tail has valid preflight");
      }
      Require(worker_->FinishChunks(stream_), "Finish accepts original-session tail");
      stream_.Reset();
      stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), kBytes, 17);
      BoundedCpuWait(); GpuWait(value); unsafe_ = false;
      const auto after = worker_->stats();
      const uint64_t chunks = mode ? 4 : 3;
      Require(after.submitted == before.submitted + 1 && after.completed == before.completed + 1 &&
              after.failed == 0 && after.chunk_sessions == before.chunk_sessions + 1 &&
              after.chunks_submitted == before.chunks_submitted + chunks &&
              after.chunks_completed == before.chunks_completed + chunks &&
              after.chunk_sessions_completed == before.chunk_sessions_completed + 1 &&
              after.chunks_failed == 0, "one final job per complete chunk session");
      Require(after.last.fence_value == value && after.last.allocator_reset == S_OK &&
              after.last.list_reset == S_OK && after.last.replay == S_OK &&
              after.last.close == S_OK && after.last.signal == S_OK && after.last.queue_execute_attempted,
              "final report retains Begin reset and Finish execution proof");
      Require(worker_->Wait() == S_OK && worker_->stats().chunks_completed == after.chunks_completed,
              "repeated final Wait cannot replay or recount");
      CheckReadbacks();
      Require(ReadbackBytes() == expected, "chunked list equals actual unsplit output");
      previous_fence = value;
    }
    ValidateDebug();
  }
  void ChunkFailureControl() {
    worker_ = Worker::TryCreate(device_.Get(), cp_);
    Require(bool(worker_), "chunk failure worker creation");
    ComPtr<FailingAllocator> allocator; allocator.Attach(new FailingAllocator);
    stream_.Reset();
    stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), 0, 17);
    Require(stream_.ResolvePipelineHandlesForReplay(), "failed Begin preflight");
    unsafe_ = true;
    Require(worker_->BeginChunks(stream_, allocator.Get(), queue_.Get(), failure_fence_.Get(), 31),
            "failed Begin accepted before asynchronous allocator reset");
    Require(worker_->Wait() == E_FAIL && worker_->Result() == E_FAIL,
            "failed Begin is sticky and joined");
    const auto& stats = worker_->stats();
    Require(stats.chunk_sessions == 1 && stats.chunks_submitted == 1 &&
            stats.chunks_completed == 1 && stats.chunks_failed == 1 &&
            stats.submitted == 0 && stats.completed == 0 && stats.failed == 0 &&
            stats.chunk_sessions_completed == 0, "failed Begin never counts a final job");
    Require(allocator->resets.load() == 1 && allocator->refs.load() == 2,
            "failed intermediate session retains allocator until teardown");
    stream_.Reset();
    stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), kBytes, 17);
    Require(stream_.ResolvePipelineHandlesForReplay(), "sticky-refusal stream preflight");
    const size_t bytes = stream_.UsedBytes();
    Require(!worker_->ReadyForChunk() && !worker_->TryAppendChunk(stream_) &&
            !worker_->FinishChunks(stream_) &&
            !worker_->Submit(stream_, allocators_[0].Get(), queue_.Get(), failure_fence_.Get(), 37) &&
            !worker_->BeginChunks(stream_, allocators_[0].Get(), queue_.Get(), failure_fence_.Get(), 37) &&
            stream_.UsedBytes() == bytes && stream_.pipeline_handles_resolved_for_replay(),
            "every sticky-failure refusal leaves source untouched");
    Require(worker_->Wait() == E_FAIL && worker_->stats().chunks_completed == 1,
            "failed chunk counted once");
    ProveUnsubmitted(97, 6);
    Require(failure_fence_->GetCompletedValue() == 0, "failed Begin cannot signal reserved fence");
    unsafe_ = false;
    worker_.reset();
    Require(allocator->refs.load() == 1, "failed session strong refs released on worker teardown");
  }
  void FailureControl() {
    worker_ = Worker::TryCreate(device_.Get(), cp_);
    auto& failing = worker_;
    Require(bool(failing), "failure worker creation");
    ComPtr<FailingAllocator> allocator; allocator.Attach(new FailingAllocator);
    stream_.Reset();
    stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), 0, 17);
    Require(stream_.ResolvePipelineHandlesForReplay(), "failure job preflight");
    unsafe_ = true;
    Require(failing->Submit(stream_, allocator.Get(), queue_.Get(), failure_fence_.Get(), 19),
            "failing allocator job accepted");
    Require(allocator->refs.load() == 2, "job retains allocator until Wait");
    Require(failing->Wait() == E_FAIL, "allocator failure published by Wait");
    const auto& result = failing->stats().last;
    Require(result.allocator_reset == E_FAIL && result.list_reset == E_PENDING &&
            result.replay == E_PENDING && result.close == E_PENDING && result.signal == E_PENDING &&
            !result.queue_execute_attempted, "failure stops before any GPU execution or signal");
    unsafe_ = false;
    Require(allocator->resets.load() == 1 && allocator->refs.load() == 1, "exact reset/strong-ref release");
    Require(failure_fence_->GetCompletedValue() == 0 && failing->Result() == E_FAIL, "no fabricated GPU fence; sticky failure");
    stream_.Reset();
    stream_.D3DCopyBufferRegion(readbacks_[0].Get(), 0, upload_.Get(), kBytes, 17);
    Require(stream_.ResolvePipelineHandlesForReplay(), "refused later stream preflight");
    Require(!failing->Submit(stream_, allocators_[0].Get(), queue_.Get(), failure_fence_.Get(), 23) &&
            stream_.pipeline_handles_resolved_for_replay(), "sticky failure leaves later source untouched");
    Require(failing->Wait() == E_FAIL && failing->stats().submitted == 1 &&
            failing->stats().completed == 1 && failing->stats().failed == 1,
            "repeated failed Wait does not double-count");
  }
  void CheckReadbacks() {
    for (size_t job = 0; job < 3; ++job) {
      void* mapped = nullptr; const D3D12_RANGE read{0, kBytes};
      Check(readbacks_[job]->Map(0, &read, &mapped), "readback map");
      size_t mismatches = 0;
      for (size_t i = 0; i < kBytes; ++i) {
        uint8_t expected = Literal(1, i);
        if (job >= 1 && i >= 257 && i < 257 + 8191) expected = Literal(2, i - 257 + 13);
        if (job >= 2 && i >= 32769 && i < 32769 + 4097) expected = Literal(3, i - 32769 + 29);
        if (static_cast<const uint8_t*>(mapped)[i] != expected) ++mismatches;
      }
      const D3D12_RANGE no_write{0, 0}; readbacks_[job]->Unmap(0, &no_write);
      Require(mismatches == 0, "ordered literal GPU bytes or untouched guard bytes differ");
    }
  }
  void ValidateDebug() {
    if (!info_) return;
    bool clean = true;
    for (UINT64 i = 0; i < info_->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
      SIZE_T bytes = 0; Check(info_->GetMessage(i, nullptr, &bytes), "debug message size");
      std::vector<uint8_t> storage(bytes); auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      Check(info_->GetMessage(i, message, &bytes), "debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::fprintf(stderr, "%s\n", message->pDescription); clean = false; }
    }
    Require(clean, "D3D12 debug error");
  }
  rex::graphics::d3d12::D3D12CommandProcessor cp_;
  Stream stream_;
  Event gpu_done_, cpu_done_;  // RAII; both exist before any queue operation.
  bool debug_enabled_ = false, unsafe_ = false;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12InfoQueue> info_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12Fence> fence_, gate_, failure_fence_, proof_fence_;
  std::array<ComPtr<ID3D12CommandAllocator>, 3> allocators_;
  ComPtr<ID3D12Resource> upload_, a_, b_;
  std::array<ComPtr<ID3D12Resource>, 3> readbacks_;
  std::unique_ptr<Worker> worker_;
};
}  // namespace
int main(int argc, char** argv) {
  try {
    Require(argc == 1 || (argc == 2 && std::string(argv[1]) == "--warp"), "optional --warp only");
    Probe probe(argc == 2); probe.Run(); return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
  }
}
