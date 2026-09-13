// Original Windows CPU-only controls and bounded microbenchmark. Links the
// ACTUAL production dispatch/AVX2 files and a separate unchanged SSE2 reference.
// No runtime DLL, game bytes, GPU, process attachment or mutable input races.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include "native_shader_hash.h"
#include "native_shader_hash_policy.h"

extern "C" uint64_t NativeShaderHashSse2Reference(const void*, size_t) noexcept;
namespace {
uint64_t checks = 0;
volatile uint64_t benchmark_sink = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) throw std::runtime_error(what);
}
uint64_t Next(uint64_t& state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
void PolicyControls() {
  using namespace nb::gpu;
  NativeShaderHashCpuFeatures full{7, (1u << 26) | (1u << 27) | (1u << 28),
                                  1u << 5, 6};
  Check(NativeShaderHashCpuSupportsAvx2(full), "complete CPU/OS features");
  Check(NativeShaderHashMayReadXcr0(full), "XGETBV admitted only after prerequisites");
  for (uint32_t leaf = 0; leaf < 7; ++leaf) {
    auto f = full; f.max_basic_leaf = leaf;
    Check(!NativeShaderHashMayReadXcr0(f) && !NativeShaderHashCpuSupportsAvx2(f),
          "missing CPUID leaf refuses without XGETBV");
  }
  for (unsigned bit : {26u, 27u, 28u}) {
    auto f = full; f.leaf1_ecx &= ~(1u << bit);
    Check(!NativeShaderHashMayReadXcr0(f) && !NativeShaderHashCpuSupportsAvx2(f),
          "missing XSAVE/OSXSAVE/AVX refuses before XGETBV");
  }
  auto f = full; f.leaf7_ebx = 0;
  Check(!NativeShaderHashCpuSupportsAvx2(f), "missing AVX2 refuses");
  for (uint64_t xcr0 = 0; xcr0 < 16; ++xcr0) {
    f = full; f.xcr0 = xcr0;
    Check(NativeShaderHashCpuSupportsAvx2(f) == ((xcr0 & 6) == 6),
          "both XMM and YMM OS state required");
  }
  for (size_t length : {size_t{0}, size_t{1}, size_t{240}, size_t{241}, size_t{4096}, SIZE_MAX}) {
    Check(!NativeShaderHashWantsAvx2(false, length), "disabled always baseline");
    Check(NativeShaderHashWantsAvx2(true, length) == (length > 240), "long-only admission");
  }
}
void ConcurrentFirstCall() {
  // Must run before ANY availability query or enabled production hash in main.
  std::array<uint8_t, 4096> data{};
  uint64_t state = 0x7C15E947AD9235B1ull;
  for (auto& byte : data) byte = static_cast<uint8_t>(Next(state));
  const uint64_t expected = NativeShaderHashSse2Reference(data.data(), data.size());
  std::atomic<unsigned> failures{0};
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false}, cancel{false};
  std::array<bool, 8> available{};
  std::array<std::thread, 8> workers;
  try {
    for (size_t i = 0; i < workers.size(); ++i) {
      workers[i] = std::thread([&, i] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (cancel.load(std::memory_order_acquire)) return;
        for (unsigned repeat = 0; repeat < 64; ++repeat) {
          if (nb::gpu::NativeShaderHash(data.data(), data.size(), true) != expected)
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        available[i] = nb::gpu::NativeShaderHashAvx2Available();
      });
    }
  } catch (...) {
    cancel.store(true, std::memory_order_release);
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    throw;
  }
  while (ready.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();
  Check(failures.load() == 0, "concurrent first-call hashes");
  for (bool a : available) Check(a == available[0], "single immutable CPU decision");
}
void Compare(const void* data, size_t length, bool avx2) {
  const uint64_t expected = NativeShaderHashSse2Reference(data, length);
  Check(nb::gpu::NativeShaderHash(data, length, false) == expected, "disabled exact reference");
  Check(nb::gpu::NativeShaderHash(data, length, true) == expected, "enabled exact reference");
  if (avx2) {
    Check(nb::gpu::detail::NativeShaderHashAvx2(data, length) == expected,
          "actual AVX2 body exact reference");
  }
}
void ByteControls(bool avx2) {
  constexpr size_t maximum = 65537;
  std::vector<uint8_t> storage(maximum + 128);
  const uintptr_t aligned = (reinterpret_cast<uintptr_t>(storage.data()) + 63) & ~uintptr_t{63};
  auto* data = reinterpret_cast<uint8_t*>(aligned);
  for (unsigned pattern = 0; pattern < 4; ++pattern) {
    uint64_t state = 0x184D934ACA170057ull;
    for (size_t i = 0; i < maximum + 64; ++i) {
      data[i] = pattern == 0 ? uint8_t{0} : pattern == 1 ? uint8_t{255} :
          pattern == 2 ? static_cast<uint8_t>(i * 29 + (i >> 7)) :
                         static_cast<uint8_t>(Next(state));
    }
    for (size_t offset = 0; offset < 64; ++offset) {
      for (size_t length = 0; length <= 512; ++length) Compare(data + offset, length, avx2);
      for (size_t boundary : {size_t{1024}, size_t{2048}, size_t{4096}, size_t{8192}, size_t{16384}, size_t{65536}}) {
        for (size_t length : {boundary - 1, boundary, boundary + 1}) Compare(data + offset, length, avx2);
      }
    }
  }
  // Each new byte sequence is compared independently; no assertion that hashes
  // can never collide. Includes mutations on both sides of stripes/blocks.
  for (size_t offset : {size_t{0}, size_t{1}, size_t{13}, size_t{63}}) {
    for (size_t position = 0; position < 4096; ++position) {
      data[offset + position] ^= uint8_t{0x80};
      Compare(data + offset, 4096, avx2);
      data[offset + position] ^= uint8_t{0x80};
    }
  }
  Compare(nullptr, 0, avx2);
}
struct GuardedBytes {
  uint8_t* allocation = nullptr;
  uint8_t* begin = nullptr;
  size_t capacity = 0;
  GuardedBytes() {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    capacity = ((65537 + page - 1) / page) * page;
    allocation = static_cast<uint8_t*>(VirtualAlloc(nullptr, capacity + 2 * page,
                                                   MEM_RESERVE, PAGE_NOACCESS));
    if (!allocation) throw std::runtime_error("guard reserve");
    begin = static_cast<uint8_t*>(VirtualAlloc(allocation + page, capacity,
                                              MEM_COMMIT, PAGE_READWRITE));
    if (!begin) { VirtualFree(allocation, 0, MEM_RELEASE); allocation = nullptr;
      throw std::runtime_error("guard commit"); }
    uint64_t state = 0x7FADD391468AB901ull;
    for (size_t i = 0; i < capacity; ++i) begin[i] = static_cast<uint8_t>(Next(state));
  }
  ~GuardedBytes() { if (allocation) VirtualFree(allocation, 0, MEM_RELEASE); }
  GuardedBytes(const GuardedBytes&) = delete;
  GuardedBytes& operator=(const GuardedBytes&) = delete;
};
void GuardControls(bool avx2) {
  GuardedBytes bytes;
  std::vector<size_t> lengths;
  for (size_t length = 0; length <= 513; ++length) lengths.push_back(length);
  for (size_t boundary : {size_t{1024}, size_t{4096}, size_t{8192}, size_t{65536}})
    for (size_t length : {boundary - 1, boundary, boundary + 1}) lengths.push_back(length);
  for (size_t length : lengths) {
    Compare(bytes.begin, length, avx2);
    // With length=0 this is a deliberately inaccessible pointer: no dereference.
    Compare(bytes.begin + bytes.capacity - length, length, avx2);
  }
}
using Hash = uint64_t (*)(const void*, size_t) noexcept;
uint64_t EnabledHash(const void* input, size_t length) noexcept {
  return nb::gpu::NativeShaderHash(input, length, true);
}
struct Measurement { double ns; uint64_t checksum; };
__declspec(noinline) Measurement Measure(Hash hash, const std::vector<uint8_t>& bytes,
                                       size_t length, size_t iterations, size_t seed) {
  uint64_t checksum = 0;
  const size_t offset_mask = bytes.size() / 2 - 1;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iterations; ++i) {
    const size_t offset = (i * 8191 + seed * 1031) & offset_mask;
    checksum += hash(bytes.data() + offset, length);
  }
  const auto end = std::chrono::steady_clock::now();
  benchmark_sink = checksum;
  return {std::chrono::duration<double, std::nano>(end - start).count(), checksum};
}
void Benchmark() {
  std::vector<uint8_t> bytes(8 * 1024 * 1024);
  uint64_t state = 0x1F12B37592008731ull;
  for (auto& byte : bytes) byte = static_cast<uint8_t>(Next(state));
  std::puts("MICROBENCH full-call cost, unchanged SDK SSE2 vs production guarded enabled; rotating synthetic bytes; ABBA; no FPS claim");
  std::puts("bytes,iterations_per_batch,legacy_ns_per_call,enabled_ns_per_call,enabled_over_legacy");
  for (size_t length : {size_t{240}, size_t{256}, size_t{512}, size_t{1024}, size_t{2048}, size_t{4096}, size_t{8192}}) {
    const size_t iterations = (32 * 1024 * 1024) / length;
    const auto warm0 = Measure(NativeShaderHashSse2Reference, bytes, length, iterations, 0);
    const auto warm1 = Measure(EnabledHash, bytes, length, iterations, 0);
    Check(warm0.checksum == warm1.checksum, "warmup checksum");
    double elapsed[2]{};
    for (size_t round = 0; round < 3; ++round) {
      uint64_t expected = 0;
      for (size_t step = 0; step < 4; ++step) {
        const bool optimized = step == 1 || step == 2;
        const auto measurement = Measure(optimized ? EnabledHash : NativeShaderHashSse2Reference,
                                         bytes, length, iterations, round + 1);
        if (!step) expected = measurement.checksum;
        Check(measurement.checksum == expected, "ABBA preserved live checksum");
        elapsed[optimized ? 1 : 0] += measurement.ns;
      }
    }
    std::printf("%zu,%zu,%.3f,%.3f,%.4f\n", length, iterations,
                elapsed[0] / (iterations * 6), elapsed[1] / (iterations * 6),
                elapsed[1] / elapsed[0]);
  }
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--no-benchmark") != 0))
      throw std::runtime_error("optional argument: --no-benchmark");
    PolicyControls();
    ConcurrentFirstCall();
    const bool avx2 = nb::gpu::NativeShaderHashAvx2Available();
    std::printf("CPU_OS_AVX2=%u (unsupported hardware exercises fallback only)\n", unsigned(avx2));
    ByteControls(avx2);
    GuardControls(avx2);
    if (argc == 1) Benchmark();
    std::printf("PASS %llu exact-hash/policy/publication checks; guard pages intact\n",
                static_cast<unsigned long long>(checks));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %llu checks: %s\n",
                 static_cast<unsigned long long>(checks), error.what());
    return 1;
  }
}
