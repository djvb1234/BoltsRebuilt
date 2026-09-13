// Original local cache for successful D3DCompile results. No game-derived data is
// part of this source. Cache failures always leave normal compilation available.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcompiler.h>

namespace nb::gpu::shader_cache {

using Digest = std::array<uint8_t, 32>;
struct Macro { std::string_view name, value; bool has_definition = true; };
struct Request {
  std::string_view source_name, source, entry_point, target;
  std::span<const Macro> macros;
  uint32_t flags1 = 0, flags2 = 0;
};
struct Stats {
  uint64_t hits = 0, misses = 0, rejected = 0, published = 0, write_failures = 0;
};
struct StoreFailure {
  const char* operation = "none";
  uint32_t system_error = 0;
};

constexpr uint64_t kMaxBlobBytes = 64u * 1024u * 1024u;
bool Sha256(std::span<const uint8_t> bytes, Digest& result) noexcept;
// Hashes exact, length-delimited input bytes, including macro order and absence.
bool MakeKey(const Request& request, const Digest& compiler_identity, Digest& result) noexcept;
// Resolve only the already loaded D3DCOMPILER_DLL_W export, avoiding the
// executable import thunk produced by the SDK's non-dllimport declaration.
// Caching and compilation must use this same resolved function.
pD3DCompile CompilerEntryPoint() noexcept;
// Identifies the module containing CompilerEntryPoint(), including forwarding.
// Hashes its complete on-disk image once per process; failure disables caching.
bool CompilerIdentity(Digest& result) noexcept;
std::filesystem::path DefaultDirectory() noexcept;
std::filesystem::path EntryPath(const std::filesystem::path& directory, const Digest& key);
// Checks DXBC extents and the SHEX/SHDR stage/model token. This is container
// validation, not a complete instruction validator or a trust boundary.
bool ValidContainer(std::span<const uint8_t> blob, std::string_view target) noexcept;
bool Load(const std::filesystem::path& directory, const Digest& key,
          std::string_view target, std::vector<uint8_t>& blob) noexcept;
// Publishes only complete immutable entries, without replacing an existing file.
// A corrupt existing entry remains a miss; no cache file is deleted or replaced.
bool Store(const std::filesystem::path& directory, const Digest& key,
           std::string_view target, std::span<const uint8_t> blob) noexcept;
// Diagnostic for the calling thread's last Store only; cleanup preserves it.
StoreFailure LastStoreFailure() noexcept;
Stats GetStats() noexcept;

}  // namespace nb::gpu::shader_cache
