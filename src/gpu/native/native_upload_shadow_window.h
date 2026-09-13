// Original nb same-run experiment schedule. Does not change the upload cache.
#pragma once
#include <cstdint>

namespace nb::gpu {
constexpr bool NativeUploadShadowWindowEnabled(bool requested, uint64_t frame,
                                               uint32_t first, uint32_t period,
                                               bool abba = false) {
  if (!requested || !period) return requested;
  if (frame < first) return false;
  const uint64_t block = (frame - first) / period;
  return abba ? ((block & 3u) == 1 || (block & 3u) == 2) : (block & 1u) != 0;
}
}  // namespace nb::gpu
