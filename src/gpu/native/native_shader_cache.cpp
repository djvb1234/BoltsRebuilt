#include "native_shader_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace nb::gpu::shader_cache {
namespace {
constexpr size_t kHeaderBytes = 96;
constexpr uint32_t kFormatVersion = 1;
constexpr uint8_t kMagic[8] = {'N','B','H','L','S','L','0','1'};
std::atomic<uint64_t> hits{0}, misses{0}, rejected{0}, published{0}, write_failures{0};
std::atomic<uint64_t> temporary_sequence{0};
thread_local StoreFailure last_store_failure{};

uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t U64(const uint8_t* p) { return U32(p) | (uint64_t(U32(p + 4)) << 32); }
void Put32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}
void Put64(uint8_t* p, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(value >> (i * 8));
}
class Handle {
 public:
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~Handle() { Close(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
  HANDLE get() const { return value_; }
  bool Close() {
    if (!valid()) return true;
    const HANDLE value = std::exchange(value_, INVALID_HANDLE_VALUE);
    return CloseHandle(value) != FALSE;
  }
 private:
  HANDLE value_;
};
class Hash {
 public:
  Hash() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
      BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Hash() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  bool Add(std::span<const uint8_t> data) {
    if (!hash_) return false;
    while (!data.empty()) {
      const ULONG size = static_cast<ULONG>(std::min<size_t>(data.size(), 1u << 20));
      if (BCryptHashData(hash_, const_cast<PUCHAR>(data.data()), size, 0) < 0) return false;
      data = data.subspan(size);
    }
    return true;
  }
  bool Number(uint64_t value) {
    uint8_t bytes[8]; Put64(bytes, value); return Add(bytes);
  }
  bool String(std::string_view value) {
    return Number(value.size()) &&
           Add({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
  }
  bool Finish(Digest& value) {
    return hash_ && BCryptFinishHash(hash_, value.data(), static_cast<ULONG>(value.size()), 0) >= 0;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};
bool RegularDiskFile(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
         !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
bool ReadExact(HANDLE file, std::span<uint8_t> bytes) {
  while (!bytes.empty()) {
    DWORD done = 0;
    const DWORD wanted = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1u << 20));
    if (!ReadFile(file, bytes.data(), wanted, &done, nullptr) || !done) return false;
    bytes = bytes.subspan(done);
  }
  return true;
}
bool WriteExact(HANDLE file, std::span<const uint8_t> bytes) {
  while (!bytes.empty()) {
    DWORD done = 0;
    const DWORD wanted = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1u << 20));
    if (!WriteFile(file, bytes.data(), wanted, &done, nullptr) || !done) return false;
    bytes = bytes.subspan(done);
  }
  return true;
}
std::filesystem::path ModulePath(HMODULE module) {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) return {};
  return std::filesystem::path(std::wstring(path.data(), length));
}
bool HashCompiler(Digest& result) {
  const pD3DCompile compiler = CompilerEntryPoint();
  if (!compiler) return false;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(compiler), &module)) return false;
  const auto path = ModulePath(module);
  if (path.empty()) return false;
  // Opening without write/delete sharing prevents the image changing while hashed.
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  LARGE_INTEGER size{};
  if (!file.valid() || !RegularDiskFile(file.get()) || !GetFileSizeEx(file.get(), &size) ||
      size.QuadPart <= 0 || size.QuadPart > 256ll * 1024 * 1024) return false;
  Hash hash;
  std::array<uint8_t, 65536> bytes;
  uint64_t remaining = static_cast<uint64_t>(size.QuadPart);
  while (remaining) {
    const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, bytes.size()));
    if (!ReadExact(file.get(), {bytes.data(), count}) || !hash.Add({bytes.data(), count})) return false;
    remaining -= count;
  }
  return hash.Finish(result);
}
enum class ReadResult { kMiss, kInvalid, kHit };
ReadResult ReadEntry(const std::filesystem::path& path, const Digest& key,
                     std::string_view target, std::vector<uint8_t>& blob, DWORD* open_error = nullptr) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid()) {
    if (open_error) *open_error = GetLastError();
    return ReadResult::kMiss;
  }
  LARGE_INTEGER size{};
  std::array<uint8_t, kHeaderBytes> header{};
  if (!RegularDiskFile(file.get()) || !GetFileSizeEx(file.get(), &size) ||
      size.QuadPart < static_cast<int64_t>(kHeaderBytes) ||
      static_cast<uint64_t>(size.QuadPart) > kHeaderBytes + kMaxBlobBytes ||
      !ReadExact(file.get(), header)) return ReadResult::kInvalid;
  const uint64_t blob_size = U64(header.data() + 80);
  if (std::memcmp(header.data(), kMagic, sizeof(kMagic)) || U32(header.data() + 8) != kFormatVersion ||
      U32(header.data() + 12) != kHeaderBytes ||
      std::memcmp(header.data() + 16, key.data(), key.size()) || U64(header.data() + 88) != 0 ||
      !blob_size || blob_size > kMaxBlobBytes || blob_size + kHeaderBytes != static_cast<uint64_t>(size.QuadPart))
    return ReadResult::kInvalid;
  std::vector<uint8_t> candidate(static_cast<size_t>(blob_size));
  Digest digest{};
  if (!ReadExact(file.get(), candidate) || !Sha256(candidate, digest) ||
      std::memcmp(header.data() + 48, digest.data(), digest.size()) || !ValidContainer(candidate, target))
    return ReadResult::kInvalid;
  blob = std::move(candidate);
  return ReadResult::kHit;
}
}  // namespace

bool Sha256(std::span<const uint8_t> bytes, Digest& result) noexcept {
  Hash hash; return hash.Add(bytes) && hash.Finish(result);
}
bool MakeKey(const Request& request, const Digest& compiler_identity, Digest& result) noexcept {
  Hash hash;
  if (!hash.String("nb-native-shader-cache") || !hash.Number(kFormatVersion) ||
      !hash.Add(compiler_identity) || !hash.String(request.source_name) || !hash.String(request.source) ||
      !hash.String(request.entry_point) || !hash.String(request.target) ||
      !hash.Number(request.flags1) || !hash.Number(request.flags2) || !hash.Number(request.macros.size())) return false;
  for (const auto& macro : request.macros)
    if (!hash.String(macro.name) || !hash.Number(macro.has_definition) || !hash.String(macro.value)) return false;
  return hash.Finish(result);
}
pD3DCompile CompilerEntryPoint() noexcept {
  static const pD3DCompile compiler = []() -> pD3DCompile {
    const HMODULE module = GetModuleHandleW(D3DCOMPILER_DLL_W);
    if (!module) return nullptr;
    return std::bit_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile"));
  }();
  return compiler;
}
bool CompilerIdentity(Digest& result) noexcept {
  struct Identity { Digest digest{}; bool valid = false; };
  static const Identity identity = [] {
    Identity value;
    try { value.valid = HashCompiler(value.digest); } catch (...) {}
    return value;
  }();
  result = identity.digest; return identity.valid;
}
std::filesystem::path DefaultDirectory() noexcept {
  try {
    auto executable = ModulePath(nullptr);
    return executable.empty() ? std::filesystem::path{} : executable.parent_path() / L"native_shader_cache";
  } catch (...) { return {}; }
}
std::filesystem::path EntryPath(const std::filesystem::path& directory, const Digest& key) {
  constexpr char hex[] = "0123456789abcdef";
  std::string name; name.reserve(70);
  for (uint8_t byte : key) { name += hex[byte >> 4]; name += hex[byte & 15]; }
  return directory / (name + ".nbcso");
}
bool ValidContainer(std::span<const uint8_t> blob, std::string_view target) noexcept {
  if (blob.size() < 32 || blob.size() > kMaxBlobBytes || std::memcmp(blob.data(), "DXBC", 4) ||
      U32(blob.data() + 20) != 1 || U32(blob.data() + 24) != blob.size()) return false;
  const uint32_t chunks = U32(blob.data() + 28);
  if (!chunks || chunks > (blob.size() - 32) / 4) return false;
  uint32_t expected_stage;
  if (target == "vs_5_1") expected_stage = 1;
  else if (target == "ps_5_1") expected_stage = 0;
  else return false;
  bool have_code = false;
  const size_t directory_end = 32 + size_t(chunks) * 4;
  for (uint32_t i = 0; i < chunks; ++i) {
    const uint32_t offset = U32(blob.data() + 32 + size_t(i) * 4);
    if (offset < directory_end || offset > blob.size() - 8) return false;
    const uint32_t length = U32(blob.data() + offset + 4);
    if (length > blob.size() - offset - 8) return false;
    const uint8_t* tag = blob.data() + offset;
    if (!std::memcmp(tag, "SHEX", 4) || !std::memcmp(tag, "SHDR", 4)) {
      if (have_code || length < 8 || length % 4) return false;
      const uint32_t version = U32(tag + 8);
      if ((version >> 16) != expected_stage || (version & 0xFFFF) != 0x51 ||
          U32(tag + 12) != length / 4) return false;
      have_code = true;
    }
  }
  return have_code;
}
bool Load(const std::filesystem::path& directory, const Digest& key,
          std::string_view target, std::vector<uint8_t>& blob) noexcept {
  blob.clear();
  try {
    if (!directory.empty()) {
      const auto result = ReadEntry(EntryPath(directory, key), key, target, blob);
      if (result == ReadResult::kHit) { ++hits; return true; }
      if (result == ReadResult::kInvalid) ++rejected;
    }
  } catch (...) {}
  ++misses; return false;
}
bool Store(const std::filesystem::path& directory, const Digest& key,
           std::string_view target, std::span<const uint8_t> blob) noexcept {
  last_store_failure = {};
  std::filesystem::path temporary;
  bool own_temporary = false;
  try {
    Digest digest{};
    if (directory.empty() || !ValidContainer(blob, target) || !Sha256(blob, digest)) {
      last_store_failure = {"validate_input", 0}; ++write_failures; return false;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      last_store_failure = {"create_directories", static_cast<uint32_t>(error.value())};
      ++write_failures; return false;
    }
    const auto destination = EntryPath(directory, key);
    // Most competing compiles arrive after a winner is already present. A read
    // avoids opening another temporary file or contending on a doomed rename.
    std::vector<uint8_t> existing;
    if (ReadEntry(destination, key, target, existing) == ReadResult::kHit) {
      if (existing.size() == blob.size() && std::equal(existing.begin(), existing.end(), blob.begin())) return true;
      last_store_failure = {"different_existing_bytes", 0}; ++write_failures; return false;
    }
    std::array<uint8_t, kHeaderBytes> header{};
    std::copy(std::begin(kMagic), std::end(kMagic), header.begin());
    Put32(header.data() + 8, kFormatVersion); Put32(header.data() + 12, static_cast<uint32_t>(kHeaderBytes));
    std::copy(key.begin(), key.end(), header.begin() + 16);
    std::copy(digest.begin(), digest.end(), header.begin() + 48);
    Put64(header.data() + 80, blob.size());
    // CREATE_NEW is the ownership claim. PID reuse or a stale temp file cannot
    // cause a writer to truncate another writer's data.
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      temporary = destination;
      temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                   std::to_wstring(temporary_sequence.fetch_add(1)) + L".tmp";
      Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
      if (!file.valid()) {
        const DWORD create_error = GetLastError();
        last_store_failure = {"create_temporary", create_error};
        if (create_error == ERROR_FILE_EXISTS || create_error == ERROR_ALREADY_EXISTS) continue;
        break;
      }
      own_temporary = true;
      const bool complete = WriteExact(file.get(), header) && WriteExact(file.get(), blob) && FlushFileBuffers(file.get());
      if (!complete) last_store_failure = {"write_or_flush_temporary", GetLastError()};
      const bool closed = file.Close();
      if (!closed && complete) last_store_failure = {"close_temporary", GetLastError()};
      if (complete && closed && MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        own_temporary = false; last_store_failure = {}; ++published; return true;
      }
      // A competing writer may already have published this exact request. Never
      // replace its bytes; compare through the normal validation path.
      bool identical = false;
      if (complete && closed) {
        last_store_failure = {"publish", GetLastError()};
        DWORD read_error = 0;
        const auto result = ReadEntry(destination, key, target, existing, &read_error);
        identical = result == ReadResult::kHit && existing.size() == blob.size() &&
                    std::equal(existing.begin(), existing.end(), blob.begin());
        if (result == ReadResult::kMiss) last_store_failure = {"read_publish_winner", read_error};
        else if (result == ReadResult::kInvalid) last_store_failure = {"invalid_publish_winner", 0};
        else if (!identical) last_store_failure = {"different_existing_bytes", 0};
      }
      DeleteFileW(temporary.c_str()); own_temporary = false;
      if (identical) { last_store_failure = {}; return true; }
      break;
    }
  } catch (...) { last_store_failure = {"exception", 0}; }
  if (own_temporary) DeleteFileW(temporary.c_str());
  ++write_failures; return false;
}
StoreFailure LastStoreFailure() noexcept { return last_store_failure; }
Stats GetStats() noexcept {
  return {hits.load(), misses.load(), rejected.load(), published.load(), write_failures.load()};
}
}  // namespace nb::gpu::shader_cache
