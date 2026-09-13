// Original isolated wrapper. Algorithm and BSD notices remain unchanged in
// the externally included, licensed SDK thirdparty/xxHash/xxhash.h (0.8.3).
// Never use the baseline PCH here: it may have instantiated SSE2 XXH3 already.
#include "native_shader_hash.h"
#define XXH_INLINE_ALL
#define XXH_VECTOR XXH_AVX2
#include <xxhash.h>

static_assert(XXH_VERSION_NUMBER == 803, "Revalidate exact hashes after an SDK xxHash update");
static_assert(XXH_VECTOR == XXH_AVX2, "This isolated object must use the AVX2 body");
#if !defined(__AVX2__)
#error Compile ONLY this source with AVX2 enabled.
#endif

namespace nb::gpu::detail {
__declspec(noinline) uint64_t NativeShaderHashAvx2(const void* input,
                                                size_t length) noexcept {
  return XXH3_64bits(input, length);
}
}  // namespace nb::gpu::detail
