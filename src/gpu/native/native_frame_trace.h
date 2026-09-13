// Original bounded diagnostic of guest swap cadence and CP frame counters.
// now_ns is an absolute steady-clock tick supplied by the CP, never GPU time.
// The first Record establishes time zero. frame_ns/swap_ns come from the caller;
// an initial frame_ns of zero means no previous interval, not a zero-cost frame.
// Rows are selected by the ending swap tick; its interval may start before capture.
// Saving happens on the first Record at/after the interval end, not destruction:
// the game may hard-exit. This helper never waits for a GPU or changes rendering.
#pragma once

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace nb::gpu {

class NativeFrameTrace final {
 public:
  struct FrameRow {
    uint64_t frame = 0, frame_ns = 0, swap_ns = 0;
    uint64_t draws = 0, native_draws = 0, shader_load_calls = 0;
    uint64_t refusal[8]{};
    uint64_t shared_upload_bytes = 0, shared_upload_calls = 0;
    uint64_t async_queued = 0, async_completed = 0;
  };
  enum class Result { None, Saved, Failed };
  static constexpr size_t kMaxRows = 60000;

  NativeFrameTrace(std::string_view path, double start_seconds,
                   double duration_seconds) noexcept {
    if (path.empty()) return;
    enabled_ = true;
    status_ = "pending";
    if (path.size() > 32768) { FailInitialization("invalid_path"); return; }
    const double start_ns = start_seconds * 1.0e9;
    const double duration_ns = duration_seconds * 1.0e9;
    const double maximum = static_cast<double>(std::numeric_limits<uint64_t>::max());
    if (!std::isfinite(start_ns) || !std::isfinite(duration_ns) || start_ns < 0 ||
        duration_ns < 1 || start_ns >= maximum || duration_ns >= maximum) {
      FailInitialization("invalid_interval");
      return;
    }
    start_ns_ = static_cast<uint64_t>(start_ns);
    const auto duration = static_cast<uint64_t>(duration_ns);
    if (start_ns_ > std::numeric_limits<uint64_t>::max() - duration) {
      FailInitialization("invalid_interval");
      return;
    }
    end_ns_ = start_ns_ + duration;
    try {
      path_.assign(path);
      rows_.reserve(kMaxRows);
    } catch (...) {
      FailInitialization("allocation_failed");
    }
  }

  NativeFrameTrace(const NativeFrameTrace&) = delete;
  NativeFrameTrace& operator=(const NativeFrameTrace&) = delete;
  bool enabled() const noexcept { return enabled_; }
  bool overflowed() const noexcept { return dropped_rows_ != 0; }
  const char* status() const noexcept { return status_; }

  Result Record(uint64_t now_ns, const FrameRow& row) noexcept {
    if (!enabled_) return Result::None;
    if (initialization_failed_) { enabled_ = false; return Result::Failed; }
    if (!have_baseline_) {
      first_ns_ = last_ns_ = now_ns;
      have_baseline_ = true;
    }
    if (now_ns < last_ns_) {
      status_ = "non_monotonic_clock";
      enabled_ = false;
      return Result::Failed;
    }
    last_ns_ = now_ns;
    const uint64_t elapsed = now_ns - first_ns_;
    if (elapsed >= end_ns_) return Write();
    if (elapsed >= start_ns_) {
      if (rows_.size() < kMaxRows) {
        // Capacity was reserved once; Entry has no allocation or throwing copy.
        rows_.push_back(Entry{elapsed, row});
      } else {
        ++dropped_rows_;
      }
    }
    return Result::None;
  }

 private:
  struct Entry { uint64_t elapsed_ns; FrameRow row; };
  static_assert(kMaxRows * sizeof(Entry) <= 12 * 1024 * 1024);
  void FailInitialization(const char* status) noexcept {
    status_ = status;
    initialization_failed_ = true;  // Report Failed once from the first Record.
  }
  Result Write() noexcept {
    enabled_ = false;  // Exactly one attempt, including open/write failures.
    status_ = overflowed() ? "capacity_overflow" : "complete";
    try {
      std::ofstream output(path_, std::ios::out | std::ios::trunc);
      if (!output.is_open()) { status_ = "open_failed"; return Result::Failed; }
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output << "# native_frame_trace status=" << status_ << " rows=" << rows_.size()
             << " dropped_rows=" << dropped_rows_ << " start_ns=" << start_ns_
             << " end_ns=" << end_ns_ << '\n';
      output << "elapsed_ns,frame,frame_ns,swap_ns,draws,native_draws,shader_load_calls";
      for (unsigned i = 0; i != 8; ++i) output << ",refusal_" << i;
      output << ",shared_upload_bytes,shared_upload_calls,async_queued,async_completed\n";
      for (const auto& entry : rows_) {
        const auto& r = entry.row;
        output << entry.elapsed_ns << ',' << r.frame << ',' << r.frame_ns << ',' << r.swap_ns
               << ',' << r.draws << ',' << r.native_draws << ',' << r.shader_load_calls;
        for (uint64_t refusal : r.refusal) output << ',' << refusal;
        output << ',' << r.shared_upload_bytes << ',' << r.shared_upload_calls
               << ',' << r.async_queued << ',' << r.async_completed << '\n';
      }
      output << "# end native_frame_trace\n";  // Readers can reject an incomplete write.
      output.close();
      return Result::Saved;
    } catch (...) {
      status_ = "write_failed";
      return Result::Failed;
    }
  }
  std::string path_;
  std::vector<Entry> rows_;
  uint64_t start_ns_ = 0, end_ns_ = 0, first_ns_ = 0, last_ns_ = 0, dropped_rows_ = 0;
  const char* status_ = "disabled";
  bool enabled_ = false, initialization_failed_ = false, have_baseline_ = false;
};

}  // namespace nb::gpu
