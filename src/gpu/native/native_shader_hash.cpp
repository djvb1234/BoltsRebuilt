// Original thread-safe, baseline-ISA dispatch. The hash body is SDK xxHash.
#include "native_shader_hash.h"
#include "native_shader_hash_policy.h"
#include <rex/hash.h>

#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#endif

namespace nb::gpu {
namespace {
#if defined(_M_X64) || defined(__x86_64__)
// Clang requires the intrinsic's target feature. Only this guarded leaf may
// execute XGETBV; the caller and all other baseline code retain x86-64-v2.
#if defined(__clang__)
__attribute__((target("xsave"), noinline))
#else
__declspec(noinline)
#endif
uint64_t ReadXcr0() noexcept { return _xgetbv(0); }

bool DetectAvx2() noexcept {
  NativeShaderHashCpuFeatures features;
  int registers[4]{};
  __cpuidex(registers, 0, 0);
  features.max_basic_leaf = static_cast<uint32_t>(registers[0]);
  if (features.max_basic_leaf < 1) return false;
  __cpuidex(registers, 1, 0);
  features.leaf1_ecx = static_cast<uint32_t>(registers[2]);
  if (!NativeShaderHashMayReadXcr0(features)) return false;
  __cpuidex(registers, 7, 0);
  features.leaf7_ebx = static_cast<uint32_t>(registers[1]);
  if (!(features.leaf7_ebx & (1u << 5))) return false;
  features.xcr0 = ReadXcr0();
  return NativeShaderHashCpuSupportsAvx2(features);
}
#else
bool DetectAvx2() noexcept { return false; }
#endif
}  // namespace

bool NativeShaderHashAvx2Available() noexcept {
  static const bool available = DetectAvx2();
  return available;
}

uint64_t NativeShaderHash(const void* input, size_t length, bool enabled) noexcept {
  if (NativeShaderHashWantsAvx2(enabled, length) && NativeShaderHashAvx2Available()) {
    return detail::NativeShaderHashAvx2(input, length);
  }
  return XXH3_64bits(input, length);
}
}  // namespace nb::gpu
