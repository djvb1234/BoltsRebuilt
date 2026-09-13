// Original CPU/OS admission policy for the optional exact XXH3 AVX2 wrapper.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nb::gpu {
inline constexpr size_t kNativeShaderHashLongMinimum = 241;
struct NativeShaderHashCpuFeatures {
  uint32_t max_basic_leaf = 0;
  uint32_t leaf1_ecx = 0;
  uint32_t leaf7_ebx = 0;
  uint64_t xcr0 = 0;
};
// XGETBV itself must not execute until XSAVE, OSXSAVE and AVX are established.
constexpr bool NativeShaderHashMayReadXcr0(
    const NativeShaderHashCpuFeatures& features) noexcept {
  constexpr uint32_t required = (1u << 26) | (1u << 27) | (1u << 28);
  return features.max_basic_leaf >= 7 &&
         (features.leaf1_ecx & required) == required;
}
constexpr bool NativeShaderHashCpuSupportsAvx2(
    const NativeShaderHashCpuFeatures& features) noexcept {
  return NativeShaderHashMayReadXcr0(features) &&
         (features.leaf7_ebx & (1u << 5)) != 0 &&
         (features.xcr0 & uint64_t{6}) == 6;
}
constexpr bool NativeShaderHashWantsAvx2(bool enabled, size_t length) noexcept {
  return enabled && length >= kNativeShaderHashLongMinimum;
}
}  // namespace nb::gpu
