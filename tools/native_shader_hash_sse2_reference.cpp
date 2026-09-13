// Independent translation unit using the unchanged SDK SSE2 algorithm.
// The upstream algorithm/license are in SDK thirdparty/xxHash/xxhash.h.
#include <cstddef>
#include <cstdint>
#define XXH_INLINE_ALL
#define XXH_VECTOR XXH_SSE2
#include <xxhash.h>
static_assert(XXH_VERSION_NUMBER == 803);
static_assert(XXH_VECTOR == XXH_SSE2);
#if defined(__AVX__) || defined(__AVX2__)
#error The reference must retain the baseline ISA.
#endif
extern "C" __declspec(noinline) uint64_t NativeShaderHashSse2Reference(
    const void* input, size_t length) noexcept {
  return XXH3_64bits(input, length);
}
