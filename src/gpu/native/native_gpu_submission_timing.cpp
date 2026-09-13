// Original submission timing implementation; no rendering state is changed.
#include "native_gpu_submission_timing.h"
#include <rex/graphics/d3d12/deferred_command_list.h>

#include <windows.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>

namespace nb::gpu {
namespace {
uint64_t CpuTicks() {
  LARGE_INTEGER value{};
  return QueryPerformanceCounter(&value) ? uint64_t(value.QuadPart) : 0;
}
}  // namespace

std::unique_ptr<NativeGpuSubmissionTiming> NativeGpuSubmissionTiming::Create(
    ID3D12Device* device, ID3D12CommandQueue* queue, std::string path,
    uint64_t first_frame, uint64_t frame_count) {
  if (!device || !queue || path.empty() || !frame_count ||
      first_frame > UINT64_MAX - frame_count) return nullptr;
  auto result = std::unique_ptr<NativeGpuSubmissionTiming>(new NativeGpuSubmissionTiming);
  result->device_ = device;
  result->path_ = std::move(path);
  result->first_frame_ = first_frame;
  result->frame_count_ = frame_count;
  LARGE_INTEGER cpu_frequency{};
  if (FAILED(queue->GetTimestampFrequency(&result->gpu_frequency_)) ||
      !result->gpu_frequency_ || !QueryPerformanceFrequency(&cpu_frequency) ||
      cpu_frequency.QuadPart <= 0) return nullptr;
  result->cpu_frequency_ = uint64_t(cpu_frequency.QuadPart);
  D3D12_QUERY_HEAP_DESC query_desc{};
  query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  query_desc.Count = kCapacity * 2;
  if (FAILED(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&result->queries_)))) return nullptr;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = kCapacity * 2 * sizeof(uint64_t);
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result->readback_)))) return nullptr;
  result->records_.reserve(kCapacity);
  return result;
}

NativeGpuSubmissionTiming::~NativeGpuSubmissionTiming() {
  if (!finished_ && (!records_.empty() || !passes_.empty())) RetainInFlightResources();
}

void NativeGpuSubmissionTiming::RetainInFlightResources() {
  // A failed drain must not turn diagnostics into an early-release hazard.
  // This path is used only during failed renderer teardown, never normal play.
  (void)queries_.Detach();
  (void)readback_.Detach();
  (void)pass_queries_.Detach();
  (void)pass_readback_.Detach();
}

bool NativeGpuSubmissionTiming::EnablePassTiming(uint32_t period, bool include_geometry) {
  if (!period) return true;
  D3D12_QUERY_HEAP_DESC queries{};
  queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  queries.Count = kPassCapacity * 2;
  if (FAILED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&pass_queries_)))) return false;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = kPassCapacity * 2 * sizeof(uint64_t);
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pass_readback_)))) return false;
  passes_.reserve(kPassCapacity);
  pass_period_ = period;
  pass_include_geometry_ = include_geometry;
  return true;
}

uint32_t NativeGpuSubmissionTiming::BeginPass(rex::graphics::d3d12::DeferredCommandList& list,
    uint64_t frame, uint64_t submission, const char* name,
    uint64_t detail0, uint64_t detail1, uint64_t detail2) {
  if (!pass_period_ || finished_ || invalid_ || frame < first_frame_ ||
      frame - first_frame_ >= frame_count_ || (frame - first_frame_) % pass_period_) return kNoSlot;
  if (!pass_include_geometry_ && (!std::strcmp(name, "native_geometry") ||
      !std::strcmp(name, "native_hook_inclusive") || !std::strcmp(name, "emulated_draw_region"))) return kNoSlot;
  if (passes_.size() == kPassCapacity) { ++pass_overflow_; return kNoSlot; }
  const uint32_t slot = uint32_t(passes_.size());
  passes_.push_back({frame, submission, detail0, detail1, detail2, name});
  list.D3DEndQuery(pass_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
  return slot;
}
void NativeGpuSubmissionTiming::EndPass(rex::graphics::d3d12::DeferredCommandList& list, uint32_t slot) {
  if (slot == kNoSlot) return;
  list.D3DEndQuery(pass_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
  list.D3DResolveQueryData(pass_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                         pass_readback_.Get(), uint64_t(slot) * 2 * sizeof(uint64_t));
  passes_[slot].ended = true;
}
void NativeGpuSubmissionTiming::SubmittedPasses(uint64_t submission, bool valid) {
  while (pass_submitted_count_ < passes_.size() &&
         passes_[pass_submitted_count_].submission <= submission) {
    auto& pass = passes_[pass_submitted_count_++];
    pass.submitted = valid && pass.ended && pass.submission == submission;
  }
}
bool NativeGpuSubmissionTiming::FinishPasses(bool completion_proven, uint64_t completed_submission) {
  if (!pass_period_) return true;
  bool valid = completion_proven && !invalid_ && !pass_overflow_ && !passes_.empty() &&
      completed_submission >= passes_.back().submission;
  uint64_t* ticks = nullptr;
  D3D12_RANGE range{0, passes_.size() * 2 * sizeof(uint64_t)};
  if (valid && (FAILED(pass_readback_->Map(0, &range, reinterpret_cast<void**>(&ticks))) || !ticks)) valid = false;
  if (ticks) {
    for (size_t i = 0; i < passes_.size(); ++i)
      valid &= passes_[i].submitted && ticks[i * 2] != 0 && ticks[i * 2 + 1] >= ticks[i * 2];
  }
  std::FILE* file = std::fopen((path_ + ".passes.csv").c_str(), "wb");
  bool written = false;
  if (file) {
    std::fprintf(file, "# valid=%u,gpu_frequency=%llu,period=%u,overflow=%llu\n",
        unsigned(valid), (unsigned long long)gpu_frequency_, pass_period_, (unsigned long long)pass_overflow_);
    std::fputs("frame,submission,pass,detail0,detail1,detail2,gpu_begin,gpu_end\n", file);
    for (size_t i = 0; i < passes_.size(); ++i) {
      const auto& p = passes_[i];
      std::fprintf(file, "%llu,%llu,%s,%llu,%llu,%llu,%llu,%llu\n",
          (unsigned long long)p.frame, (unsigned long long)p.submission, p.name,
          (unsigned long long)p.detail0, (unsigned long long)p.detail1, (unsigned long long)p.detail2,
          (unsigned long long)(ticks ? ticks[i * 2] : 0), (unsigned long long)(ticks ? ticks[i * 2 + 1] : 0));
    }
    written = !std::ferror(file);
    written &= std::fclose(file) == 0;
  }
  if (ticks) { const D3D12_RANGE no_write{0, 0}; pass_readback_->Unmap(0, &no_write); }
  return valid && written;
}

uint32_t NativeGpuSubmissionTiming::Begin(ID3D12GraphicsCommandList* list,
    uint64_t guest_frame, uint64_t resource_frame, uint64_t submission, bool frame_open, bool closes_frame) {
  if (finished_ || invalid_ || !list || guest_frame < first_frame_ ||
      guest_frame - first_frame_ >= frame_count_) return kNoSlot;
  if (records_.size() == kCapacity) { ++overflow_; return kNoSlot; }
  const uint32_t slot = uint32_t(records_.size());
  Record record{};
  record.guest_frame = guest_frame;
  record.resource_frame = resource_frame;
  record.submission = submission;
  record.frame_open = frame_open;
  record.closes_frame = closes_frame;
  list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
  record.record_begin = CpuTicks();
  records_.push_back(record);  // Capacity was allocated before capture.
  return slot;
}

void NativeGpuSubmissionTiming::End(ID3D12GraphicsCommandList* list, uint32_t slot) {
  if (slot == kNoSlot) return;
  records_[slot].record_end = CpuTicks();
  list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
  list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2,
                        readback_.Get(), uint64_t(slot) * 2 * sizeof(uint64_t));
}
void NativeGpuSubmissionTiming::BeforeExecute(uint32_t slot) {
  if (slot != kNoSlot) records_[slot].execute_begin = CpuTicks();
}
void NativeGpuSubmissionTiming::AfterExecute(uint32_t slot) {
  if (slot != kNoSlot) records_[slot].execute_end = CpuTicks();
}
void NativeGpuSubmissionTiming::Submitted(uint32_t slot, bool close_succeeded, bool signal_succeeded) {
  if (slot == kNoSlot) return;
  records_[slot].submitted = close_succeeded && signal_succeeded;
  invalid_ |= !records_[slot].submitted;
}
void NativeGpuSubmissionTiming::FailedReset(uint64_t guest_frame) {
  if (!finished_ && guest_frame >= first_frame_ && guest_frame - first_frame_ < frame_count_) {
    ++reset_failures_;
    invalid_ = true;
  }
}

bool NativeGpuSubmissionTiming::NeedsCompletionCheck(uint64_t guest_frame) const {
  return !finished_ && guest_frame >= first_frame_ && guest_frame - first_frame_ >= frame_count_;
}
NativeGpuSubmissionTiming::Publication NativeGpuSubmissionTiming::TryFinishAfterCapture(
    uint64_t guest_frame, uint64_t completed_submission) {
  if (!NeedsCompletionCheck(guest_frame)) return Publication::kPending;
  if (completed_submission != UINT64_MAX && !records_.empty() &&
      completed_submission < records_.back().submission) return Publication::kPending;
  if (completed_submission != UINT64_MAX && !passes_.empty() &&
      completed_submission < passes_.back().submission) return Publication::kPending;
  return Finish(completed_submission != UINT64_MAX, completed_submission)
      ? Publication::kWritten : Publication::kFailed;
}

bool NativeGpuSubmissionTiming::Finish(bool capture_gpu_complete, uint64_t completed_submission) {
  if (finished_) return false;
  finished_ = true;
  const bool completion_proven = capture_gpu_complete && completed_submission != UINT64_MAX &&
      (records_.empty() || completed_submission >= records_.back().submission) &&
      (passes_.empty() || completed_submission >= passes_.back().submission) &&
      device_ && SUCCEEDED(device_->GetDeviceRemovedReason());
  bool valid = completion_proven && !invalid_;
  const bool passes_valid = FinishPasses(completion_proven, completed_submission);
  if (!completion_proven && (!records_.empty() || !passes_.empty())) RetainInFlightResources();
  if (valid && !records_.empty()) {
    void* mapped = nullptr;
    const D3D12_RANGE range{0, records_.size() * 2 * sizeof(uint64_t)};
    if (FAILED(readback_->Map(0, &range, &mapped)) || !mapped) {
      valid = false;
    } else {
      for (size_t i = 0; i < records_.size(); ++i) {
        auto& record = records_[i];
        uint64_t ticks[2];
        std::memcpy(ticks, static_cast<const uint8_t*>(mapped) + i * sizeof(ticks), sizeof(ticks));
        record.gpu_begin = ticks[0]; record.gpu_end = ticks[1];
        if (!record.submitted || !record.record_begin ||
            record.record_end < record.record_begin || record.execute_begin < record.record_end ||
            record.execute_end < record.execute_begin || ticks[1] < ticks[0]) valid = false;
      }
      const D3D12_RANGE no_write{0, 0};
      readback_->Unmap(0, &no_write);
    }
  }
  valid &= !overflow_ && !records_.empty();
  // Write only after capture and its fence completion; no per-frame IO. The
  // normal game exit can bypass renderer destruction, so publication need not
  // wait for shutdown. Later submissions cannot touch any of these slots.
  std::FILE* file = std::fopen(path_.c_str(), "wb");
  if (!file) return false;
  std::fprintf(file, "{\"valid\":%s,\"capture_gpu_complete\":%s,\"overflow\":%" PRIu64
      ",\"reset_failures\":%" PRIu64 ",\"gpu_frequency\":%" PRIu64 ",\"cpu_frequency\":%" PRIu64
      ",\"first_frame\":%" PRIu64 ",\"frame_count\":%" PRIu64 ",\"records\":[\n",
      valid ? "true" : "false", completion_proven ? "true" : "false", overflow_,
      reset_failures_, gpu_frequency_, cpu_frequency_, first_frame_, frame_count_);
  for (size_t i = 0; i < records_.size(); ++i) {
    const auto& r = records_[i];
    std::fprintf(file, "%s{\"guest_frame\":%" PRIu64 ",\"resource_frame\":%" PRIu64
        ",\"submission\":%" PRIu64 ",\"frame_open\":%s,\"closes_frame\":%s,\"submitted\":%s,"
        "\"record_begin\":%" PRIu64 ",\"record_end\":%" PRIu64
        ",\"execute_begin\":%" PRIu64 ",\"execute_end\":%" PRIu64
        ",\"gpu_begin\":%" PRIu64 ",\"gpu_end\":%" PRIu64 "}",
        i ? ",\n" : "", r.guest_frame, r.resource_frame, r.submission,
        r.frame_open ? "true" : "false", r.closes_frame ? "true" : "false", r.submitted ? "true" : "false",
        r.record_begin, r.record_end, r.execute_begin, r.execute_end, r.gpu_begin, r.gpu_end);
  }
  std::fputs("\n]}\n", file);
  const bool write_ok = !std::ferror(file);
  const bool close_ok = std::fclose(file) == 0;
  return valid && passes_valid && !overflow_ && !records_.empty() && write_ok && close_ok;
}

}  // namespace nb::gpu
