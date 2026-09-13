// Original project-local dispatch around the unchanged SDK xxHash algorithm.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nb::gpu {
// Initialized once with C++ thread-safe publication; no mutable dispatch table.
bool NativeShaderHashAvx2Available() noexcept;
// Reads the full current input, with the same zero-seed XXH3_64bits result.
// Unsupported CPUs, disabled calls and lengths <=240 retain baseline XXH3.
uint64_t NativeShaderHash(const void* input, size_t length, bool enabled) noexcept;
namespace detail {
// Baseline callers MUST establish NativeShaderHashAvx2Available() first.
// Kept in a separate no-PCH, no-LTO object compiled only with AVX2 enabled.
uint64_t NativeShaderHashAvx2(const void* input, size_t length) noexcept;
}  // namespace detail
}  // namespace nb::gpu
