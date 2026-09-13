#include "native_gpu_budget_probe.h"
#include <rex/graphics/d3d12/deferred_command_list.h>

#include <cstdio>
#include <cstring>
#include <utility>

namespace nb::gpu {
namespace {
bool MakeQueries(ID3D12Device* device, uint32_t count,
                 Microsoft::WRL::ComPtr<ID3D12QueryHeap>& queries,
                 Microsoft::WRL::ComPtr<ID3D12Resource>& readback) {
  D3D12_QUERY_HEAP_DESC q{};
  q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  q.Count = count;
  if (FAILED(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)))) return false;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = uint64_t(count) * sizeof(uint64_t);
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
      &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
}
}  // namespace

std::unique_ptr<NativeGpuBudgetProbe> NativeGpuBudgetProbe::Create(
    ID3D12Device* device, ID3D12CommandQueue* queue, std::string path,
    uint32_t start_seconds, uint32_t frame_count, uint32_t group_period) {
  if (!device || !queue || path.empty() || !frame_count || frame_count > 2400 ||
      (group_period && (frame_count - 1) / group_period + 1 > 6))
    return nullptr;
  auto p = std::unique_ptr<NativeGpuBudgetProbe>(new NativeGpuBudgetProbe);
  p->device_ = device;
  p->path_ = std::move(path);
  p->start_seconds_ = start_seconds;
  p->frame_count_ = frame_count;
  p->group_period_ = group_period;
  if (FAILED(queue->GetTimestampFrequency(&p->gpu_frequency_)) || !p->gpu_frequency_ ||
      !MakeQueries(device, kSubmissionCapacity * 2,
                   p->submission_queries_, p->submission_readback_)) return nullptr;
  if (group_period && !MakeQueries(device, kGroupCapacity * 2,
                                  p->group_queries_, p->group_readback_)) return nullptr;
  p->submissions_.reserve(kSubmissionCapacity);
  if (group_period) p->groups_.reserve(kGroupCapacity);
  p->options_valid_ = SUCCEEDED(device->CheckFeatureSupport(
      D3D12_FEATURE_D3D12_OPTIONS, &p->options_, sizeof(p->options_)));
  const DXGI_FORMAT formats[] = {
      DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
      DXGI_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_UINT,
      DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_UINT,
      DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_UINT,
      DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
      DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
  p->formats_.reserve(sizeof(formats) / sizeof(formats[0]));
  for (DXGI_FORMAT format : formats) {
    FormatCaps caps{format};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    caps.support_valid = SUCCEEDED(device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    if (caps.support_valid) {
      caps.support1 = uint32_t(support.Support1);
      caps.support2 = uint32_t(support.Support2);
    }
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa{};
    msaa.Format = format; msaa.SampleCount = 2;
    caps.msaa_valid = SUCCEEDED(device->CheckFeatureSupport(
        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa, sizeof(msaa)));
    if (caps.msaa_valid) caps.msaa2_levels = msaa.NumQualityLevels;
    p->formats_.push_back(caps);
  }
  return p;
}

NativeGpuBudgetProbe::~NativeGpuBudgetProbe() {
  if (!completion_proven_ && !submissions_.empty()) RetainInFlight();
}
void NativeGpuBudgetProbe::RetainInFlight() {
  // Failure must not release query objects still referenced by deferred or GPU work.
  (void)submission_queries_.Detach(); (void)submission_readback_.Detach();
  (void)group_queries_.Detach(); (void)group_readback_.Detach();
}
void NativeGpuBudgetProbe::ObserveFrame(uint64_t frame) {
  if (frame == observed_frame_) return;
  const auto now = std::chrono::steady_clock::now();
  if (observed_frame_ == UINT64_MAX) first_frame_time_ = now;
  if (!first_frame_ && frame &&
      now - first_frame_time_ >= std::chrono::seconds(start_seconds_)) first_frame_ = frame;
  // Include the boundary entering the capture and the one leaving its last
  // frame. The pre-capture close anchors the cumulative counter deltas.
  if (first_frame_ && observed_frame_ != UINT64_MAX &&
      observed_frame_ < first_frame_ + frame_count_) {
    alignment_error_ |= frame != observed_frame_ + 1 || frame_closes_ != 1 ||
                        current_submission_ != kNoSlot;
  }
  observed_frame_ = frame;
  frame_closes_ = 0;
  frame_selected_ = !finished_ && first_frame_ && frame >= first_frame_ &&
                    frame - first_frame_ < frame_count_;
  groups_selected_ = frame_selected_ && group_period_ &&
                     (frame - first_frame_) % group_period_ == 0;
  groups_this_frame_ = 0;
  groups_truncated_ = false;
}
void NativeGpuBudgetProbe::BeginSubmission(List& list, uint64_t frame,
    uint64_t resource_frame, uint64_t submission) {
  ObserveFrame(frame);
  current_submission_ = current_group_ = kNoSlot;
  if (!frame_selected_) return;
  if (submissions_.size() == kSubmissionCapacity) { ++submission_overflow_; return; }
  current_submission_ = uint32_t(submissions_.size());
  submissions_.push_back({frame, resource_frame, submission, uint32_t(groups_.size())});
  last_submission_ = submission;
  list.D3DEndQuery(submission_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                   current_submission_ * 2);
}
void NativeGpuBudgetProbe::EndGroup(List& list) {
  if (current_group_ == kNoSlot) return;
  list.D3DEndQuery(group_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                   current_group_ * 2 + 1);
  groups_[current_group_].ended = true;
  current_group_ = kNoSlot;
}
void NativeGpuBudgetProbe::EndSubmission(List& list, bool closes_frame,
    uint64_t transfers, uint64_t tiles, uint64_t dumps) {
  // With nb_phase3_counters_file empty, these source counters are cumulative.
  // Snapshot every closed frame, including frames before diagnostics arm.
  uint64_t delta_transfers = 0, delta_tiles = 0, delta_dumps = 0;
  if (closes_frame) {
    ++frame_closes_;
    if (frame_selected_ && frame_closes_ != 1) alignment_error_ = true;
    const bool monotonic = transfers >= previous_transfers_ &&
        tiles >= previous_tiles_ && dumps >= previous_dumps_;
    counter_reset_ |= !monotonic;
    if (monotonic) {
      delta_transfers = transfers - previous_transfers_;
      delta_tiles = tiles - previous_tiles_;
      delta_dumps = dumps - previous_dumps_;
    }
    previous_transfers_ = transfers; previous_tiles_ = tiles; previous_dumps_ = dumps;
  }
  if (current_submission_ == kNoSlot) return;
  if (scope_ != kOther) scope_error_ = true;
  EndGroup(list);
  scope_ = kOther;
  auto& s = submissions_[current_submission_];
  s.closes_frame = closes_frame;
  if (closes_frame) {
    s.transfers = delta_transfers; s.tiles = delta_tiles; s.dumps = delta_dumps;
  }
  list.D3DEndQuery(submission_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                   current_submission_ * 2 + 1);
  // Resolve only initialized, ended query pairs. One batch per heap/submission;
  // never ResolveQueryData after each draw. Query-copy tails lie AFTER the bracket.
  list.D3DResolveQueryData(submission_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
      current_submission_ * 2, 2, submission_readback_.Get(),
      uint64_t(current_submission_) * 2 * sizeof(uint64_t));
  const uint32_t count = uint32_t(groups_.size()) - s.first_group;
  if (count) list.D3DResolveQueryData(group_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
      s.first_group * 2, count * 2, group_readback_.Get(),
      uint64_t(s.first_group) * 2 * sizeof(uint64_t));
  s.ended = true;
  current_submission_ = kNoSlot;
}
uint32_t NativeGpuBudgetProbe::EnterScope(List& list, const char* name, uint64_t detail0) {
  if (current_submission_ == kNoSlot) return kNoSlot;
  uint32_t category = kOther;
  if (!std::strcmp(name, "transfer_color_depth")) category = kTransfer;
  else if (!std::strcmp(name, "transfer_stencil_bits")) {
    const uint32_t log2_samples = uint32_t((detail0 >> 32) & 255);
    if (log2_samples > 2) { scope_error_ = true; return kNoSlot; }
    category = log2_samples == 0 ? kStencil1 : log2_samples == 1 ? kStencil2 : kStencil4;
  } else if (!std::strcmp(name, "resolve_dump")) category = kDump;
  else if (!std::strcmp(name, "resolve_copy")) category = kResolveCopy;
  else return kNoSlot;
  // These existing leaves do not nest or span submissions. Reject violations;
  // never silently close/reclassify a parent and pretend complete attribution.
  if (scope_ != kOther) { scope_error_ = true; return kNoSlot; }
  scope_ = category;
  auto& s = submissions_[current_submission_];
  ++s.scope_entries;
  ++s.category_entries[category];
  if (groups_selected_ && !groups_truncated_) {
    if (groups_this_frame_ == kGroupsPerFrame || groups_.size() == kGroupCapacity) {
      ++group_overflow_; groups_truncated_ = true;
    } else {
      current_group_ = uint32_t(groups_.size());
      groups_.push_back({s.frame, s.submission, category});
      ++groups_this_frame_;
      list.D3DEndQuery(group_queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                      current_group_ * 2);
    }
  }
  return category;
}
void NativeGpuBudgetProbe::LeaveScope(List& list, uint32_t category) {
  if (scope_ != category || current_submission_ == kNoSlot) scope_error_ = true;
  EndGroup(list);
  scope_ = kOther;
}
void NativeGpuBudgetProbe::Operation(List& list, uint32_t op, uint64_t copy_bytes) {
  (void)list;  // Counting only: NO timestamp or grouping decision at operations.
  if (current_submission_ == kNoSlot) return;
  auto& s = submissions_[current_submission_];
  ++s.operations; s.draws += uint64_t(op == 0); s.dispatches += uint64_t(op == 1);
  s.copy_bytes += copy_bytes;
  if (scope_ != kOther) {
    ++s.scope_operations; s.scope_draws += uint64_t(op == 0);
    s.scope_dispatches += uint64_t(op == 1); s.scope_copy_bytes += copy_bytes;
  }
  if (current_group_ == kNoSlot) return;
  auto& g = groups_[current_group_];
  ++g.operations; g.draws += uint64_t(op == 0); g.dispatches += uint64_t(op == 1);
  g.copy_bytes += copy_bytes;
}
const char* NativeGpuBudgetProbe::CategoryName(uint32_t c) {
  static const char* const names[] = {"residual", "transfer_color_depth",
      "stencil_1x", "stencil_2x", "stencil_4x", "resolve_dump", "resolve_copy"};
  return c < sizeof(names) / sizeof(names[0]) ? names[c] : "invalid_category";
}
NativeGpuBudgetProbe::Publication NativeGpuBudgetProbe::TryFinish(uint64_t completed) {
  if (finished_ || !first_frame_ || observed_frame_ < first_frame_ + frame_count_)
    return Publication::kPending;
  if (completed != UINT64_MAX && completed < last_submission_) return Publication::kPending;
  return Finish(completed != UINT64_MAX, completed) ? Publication::kWritten : Publication::kFailed;
}
bool NativeGpuBudgetProbe::Finish(bool completion, uint64_t completed) {
  if (finished_) return false;
  finished_ = true;
  completion_proven_ = completion && completed != UINT64_MAX &&
      completed >= last_submission_ && device_ && SUCCEEDED(device_->GetDeviceRemovedReason());
  bool all_ended = true;
  for (const auto& s : submissions_) all_ended &= s.ended;
  for (const auto& g : groups_) all_ended &= g.ended;
  const bool capture_complete = first_frame_ && observed_frame_ >= first_frame_ + frame_count_;
  bool valid = completion_proven_ && all_ended && capture_complete &&
               !submission_overflow_ && !alignment_error_ && !submissions_.empty();
  uint64_t* submission_ticks = nullptr;
  uint64_t* group_ticks = nullptr;
  const D3D12_RANGE no_write{0, 0};
  if (completion_proven_ && all_ended && !submissions_.empty()) {
    D3D12_RANGE range{0, submissions_.size() * 2 * sizeof(uint64_t)};
    if (FAILED(submission_readback_->Map(0, &range,
        reinterpret_cast<void**>(&submission_ticks))) || !submission_ticks) {
      valid = false; submission_ticks = nullptr;
    }
    if (!groups_.empty()) {
      range.End = groups_.size() * 2 * sizeof(uint64_t);
      if (FAILED(group_readback_->Map(0, &range,
          reinterpret_cast<void**>(&group_ticks))) || !group_ticks) {
        valid = false; group_ticks = nullptr;
      }
    }
  } else valid = false;
  if (submission_ticks) for (size_t i = 0; i < submissions_.size(); ++i)
    valid &= submission_ticks[2*i] != 0 && submission_ticks[2*i+1] >= submission_ticks[2*i];
  if (group_ticks) for (size_t i = 0; i < groups_.size(); ++i)
    valid &= group_ticks[2*i] != 0 && group_ticks[2*i+1] >= group_ticks[2*i];
  std::FILE* file = std::fopen(path_.c_str(), "wb");
  bool written = false;
  if (file) {
    std::fprintf(file, "{\"schema\":\"nb_gpu_budget_v2\",\"valid\":%s,"
        "\"capture_gpu_complete\":%s,\"capture_complete\":%s,\"gpu_frequency\":%llu,"
        "\"first_frame\":%llu,\"frame_count\":%u,\"start_seconds\":%u,\"group_period\":%u,"
        "\"submission_overflow\":%llu,\"group_overflow\":%llu,"
        "\"group_limit_per_frame\":%u,\"group_capacity\":%u,\"scope_error\":%s,"
        "\"alignment_error\":%s,"
        "\"grouping\":\"edram_leaf_scopes_v2\",\"query_copy_tails_outside_brackets\":true,"
        "\"counter_reset_detected\":%s,\"counters_are_frame_deltas\":true,"
        "\"options_valid\":%s,\"ps_stencil_ref\":%u,\"typed_uav_additional\":%u,"
        "\"output_merger_logic_op\":%u,\"formats\":[",
        valid ? "true" : "false", completion_proven_ ? "true" : "false",
        capture_complete ? "true" : "false", (unsigned long long)gpu_frequency_,
        (unsigned long long)first_frame_, frame_count_, start_seconds_, group_period_,
        (unsigned long long)submission_overflow_, (unsigned long long)group_overflow_,
        kGroupsPerFrame, kGroupCapacity, scope_error_ ? "true" : "false",
        alignment_error_ ? "true" : "false", counter_reset_ ? "true" : "false", options_valid_ ? "true" : "false",
        unsigned(options_.PSSpecifiedStencilRefSupported), unsigned(options_.TypedUAVLoadAdditionalFormats),
        unsigned(options_.OutputMergerLogicOp));
    for (size_t i = 0; i < formats_.size(); ++i) {
      const auto& f = formats_[i];
      std::fprintf(file, "%s{\"dxgi_format\":%u,\"support_valid\":%s,\"support1\":%u,"
          "\"support2\":%u,\"msaa2_valid\":%s,\"msaa2_levels\":%u}",
          i ? "," : "", unsigned(f.format), f.support_valid ? "true" : "false",
          f.support1, f.support2, f.msaa_valid ? "true" : "false", f.msaa2_levels);
    }
    std::fputs("],\"records\":[", file);
    for (size_t i = 0; i < submissions_.size(); ++i) {
      const auto& s = submissions_[i];
      std::fprintf(file, "%s{\"guest_frame\":%llu,\"resource_frame\":%llu,\"submission\":%llu,"
          "\"closes_frame\":%s,\"submitted\":%s,\"gpu_begin\":%llu,\"gpu_end\":%llu,"
          "\"operations\":%llu,\"draws\":%llu,\"dispatches\":%llu,\"copy_bytes\":%llu,"
          "\"transfers\":%llu,\"transfer_tiles\":%llu,\"resolve_dumps\":%llu,"
          "\"scope_entries\":%llu,\"scope_operations\":%llu,\"scope_draws\":%llu,"
          "\"scope_dispatches\":%llu,\"scope_copy_bytes\":%llu,\"category_entries\":{",
          i ? "," : "", (unsigned long long)s.frame, (unsigned long long)s.resource_frame,
          (unsigned long long)s.submission, s.closes_frame ? "true" : "false",
          completion_proven_ && s.ended ? "true" : "false",
          (unsigned long long)(submission_ticks ? submission_ticks[2*i] : 0),
          (unsigned long long)(submission_ticks ? submission_ticks[2*i+1] : 0),
          (unsigned long long)s.operations, (unsigned long long)s.draws,
          (unsigned long long)s.dispatches, (unsigned long long)s.copy_bytes,
          (unsigned long long)s.transfers, (unsigned long long)s.tiles,
          (unsigned long long)s.dumps, (unsigned long long)s.scope_entries,
          (unsigned long long)s.scope_operations, (unsigned long long)s.scope_draws,
          (unsigned long long)s.scope_dispatches, (unsigned long long)s.scope_copy_bytes);
      for (uint32_t c = kTransfer; c <= kResolveCopy; ++c) {
        std::fprintf(file, "%s\"%s\":%llu", c == kTransfer ? "" : ",",
            CategoryName(c), (unsigned long long)s.category_entries[c]);
      }
      std::fputs("}}", file);
    }
    std::fputs("]}", file);
    written = !std::ferror(file); written &= std::fclose(file) == 0;
  }
  if (group_period_) {
    std::FILE* output = std::fopen((path_ + ".groups.csv").c_str(), "wb");
    bool groups_written = false;
    if (output) {
      std::fprintf(output, "# valid=%u,complete_attribution=%u,gpu_frequency=%llu,period=%u,overflow=%llu\n",
          unsigned(valid), unsigned(valid && !group_overflow_ && !scope_error_),
          (unsigned long long)gpu_frequency_, group_period_, (unsigned long long)group_overflow_);
      std::fputs("frame,submission,category,operations,draws,dispatches,copy_bytes,gpu_begin,gpu_end\n", output);
      for (size_t i = 0; i < groups_.size(); ++i) {
        const auto& g = groups_[i];
        std::fprintf(output, "%llu,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (unsigned long long)g.frame, (unsigned long long)g.submission, CategoryName(g.category),
            (unsigned long long)g.operations, (unsigned long long)g.draws,
            (unsigned long long)g.dispatches, (unsigned long long)g.copy_bytes,
            (unsigned long long)(group_ticks ? group_ticks[2*i] : 0),
            (unsigned long long)(group_ticks ? group_ticks[2*i+1] : 0));
      }
      groups_written = !std::ferror(output); groups_written &= std::fclose(output) == 0;
    }
    written &= groups_written;
  }
  if (submission_ticks) submission_readback_->Unmap(0, &no_write);
  if (group_ticks) group_readback_->Unmap(0, &no_write);
  if (!completion_proven_ || !all_ended) {
    completion_proven_ = false;
    RetainInFlight();
  }
  return valid && written && !group_overflow_ && !scope_error_;
}
}  // namespace nb::gpu
