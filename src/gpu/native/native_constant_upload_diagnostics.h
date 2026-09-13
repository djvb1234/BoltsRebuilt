// Original optional CPU attribution for native constant preparation. These
// measure host elapsed time, not GPU execution or write-combining drain time.
#pragma once
#include <rex/graphics/d3d12/native_submission_stats.h>

namespace nb::gpu {
struct NativeConstantPreparationStats {
  rex::graphics::d3d12::NativeSubmissionCallStats allocation;
  rex::graphics::d3d12::NativeSubmissionCallStats ram_fill;
  rex::graphics::d3d12::NativeSubmissionCallStats mapped_fill;
  rex::graphics::d3d12::NativeSubmissionCallStats memo;
};
inline NativeConstantPreparationStats& GetNativeConstantPreparationStats() {
  static thread_local NativeConstantPreparationStats stats;
  return stats;
}
using NativeConstantTimer = rex::graphics::d3d12::NativeSubmissionTimer;
}  // namespace nb::gpu
