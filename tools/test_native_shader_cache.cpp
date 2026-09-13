// Original CPU-only controls. No D3DCompile, GPU device, game, or SDK data used.
// From a developer shell (root owns the build lock):
// cl /nologo /EHsc /std:c++20 /W4 /WX tools\test_native_shader_cache.cpp src\gpu\native\native_shader_cache.cpp /Fe:<scratch>\test_native_shader_cache.exe
// Run with an existing scratch parent directory as the sole argument. Artifacts
// are retained in an atomically claimed unique child; no recursive cleanup.
#include "../src/gpu/native/native_shader_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace cache = nb::gpu::shader_cache;
namespace {
unsigned checks = 0;
void Check(bool value, const char* label) {
  ++checks;
  if (!value) throw std::runtime_error(label);
}
std::span<const uint8_t> Bytes(std::string_view value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
std::string Hex(const cache::Digest& digest) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (uint8_t byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
void Put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (i * 8));
}
// Literal structural fixture, deliberately not executable shader instructions.
// The cache validates container bounds/profile, not the complete shader ISA.
std::vector<uint8_t> Fixture(bool vertex = false) {
  std::vector<uint8_t> value(52);
  std::copy_n("DXBC", 4, value.begin());
  Put32(value, 20, 1); Put32(value, 24, 52); Put32(value, 28, 1); Put32(value, 32, 36);
  std::copy_n("SHEX", 4, value.begin() + 36); Put32(value, 40, 8);
  Put32(value, 44, vertex ? 0x10051 : 0x51); Put32(value, 48, 2);
  return value;
}
std::vector<uint8_t> Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("test could not read artifact");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file) throw std::runtime_error("test could not write artifact");
}
cache::Digest Key(const cache::Request& request, const cache::Digest& compiler) {
  cache::Digest result{};
  Check(cache::MakeKey(request, compiler, result), "key calculation"); return result;
}
void TestHashesAndKeys() {
  cache::Digest digest{};
  Check(cache::Sha256({}, digest) && Hex(digest) ==
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256 empty known vector");
  Check(cache::Sha256(Bytes("abc"), digest) && Hex(digest) ==
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 abc known vector");
  const std::string million(1000000, 'a');
  Check(cache::Sha256(Bytes(million), digest) && Hex(digest) ==
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA256 million-a known vector");
  cache::Digest compiler{};
  const std::array<cache::Macro, 2> macros{{{"A", "1"}, {"B", "2"}}};
  const cache::Request original{"shader", "source", "main", "ps_5_1", macros, 0x100, 0};
  const auto key = Key(original, compiler);
  Check(Key(original, compiler) == key, "identical input key stable");
  auto request = original; request.source_name = "shader2";
  Check(Key(request, compiler) != key, "source name affects key");
  request = original; request.source = "source\r\n";
  Check(Key(request, compiler) != key, "exact source bytes affect key");
  const char embedded[] = {'s','o','u','r','c','e',0,'x'};
  request.source = {embedded, sizeof(embedded)};
  Check(Key(request, compiler) != key, "embedded null source bytes are included");
  request = original; request.entry_point = "other";
  Check(Key(request, compiler) != key, "entrypoint affects key");
  request = original; request.target = "vs_5_1";
  Check(Key(request, compiler) != key, "profile affects key");
  request = original; request.flags1 ^= 1;
  Check(Key(request, compiler) != key, "flags1 affect key");
  request = original; request.flags2 = 1;
  Check(Key(request, compiler) != key, "flags2 affect key");
  auto changed = macros; std::swap(changed[0], changed[1]); request = original; request.macros = changed;
  Check(Key(request, compiler) != key, "macro order affects key");
  changed = macros; changed[0].name = "C";
  Check(Key(request, compiler) != key, "macro name affects key");
  changed = macros; changed[0].value = "3";
  Check(Key(request, compiler) != key, "macro value affects key");
  changed = macros; changed[0].has_definition = false;
  Check(Key(request, compiler) != key, "null definition differs from present definition");
  changed = macros; changed[0].value = ""; const auto empty_key = Key(request, compiler);
  changed[0].has_definition = false;
  Check(Key(request, compiler) != empty_key, "null definition differs from empty definition");
  request = original; request.macros = {};
  Check(Key(request, compiler) != key, "macro absence affects key");
  compiler[31] = 1;
  Check(Key(original, compiler) != key, "compiler image identity affects key");
  // Keep a real static import from D3DCOMPILER_DLL_W in this CPU-only test.
  // Linking an otherwise unused import library alone does not load its DLL.
  ID3DBlob* blob = nullptr;
  Check(SUCCEEDED(D3DCreateBlob(1, &blob)) && blob, "compiler DLL blob allocation import");
  blob->Release();
  const HMODULE module = GetModuleHandleW(D3DCOMPILER_DLL_W);
  const auto expected = module ? std::bit_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
  Check(expected && cache::CompilerEntryPoint() == expected, "resolver returns the loaded DLL export");
  HMODULE owner = nullptr;
  Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(expected), &owner) && owner != GetModuleHandleW(nullptr),
        "resolved compiler belongs to a DLL, not the executable import thunk");
  cache::Digest actual{}, actual_again{};
  Check(cache::CompilerIdentity(actual) && cache::CompilerIdentity(actual_again) && actual == actual_again,
        "loaded D3DCompile module identity is available and stable");
}
void TestContainers() {
  const auto pixel = Fixture(); const auto vertex = Fixture(true);
  Check(cache::ValidContainer(pixel, "ps_5_1"), "literal pixel container");
  Check(cache::ValidContainer(vertex, "vs_5_1"), "literal vertex container");
  Check(!cache::ValidContainer(pixel, "vs_5_1"), "wrong stage refused");
  Check(!cache::ValidContainer(pixel, "ps_5_0"), "unsupported profile refused");
  for (size_t length = 0; length < pixel.size(); ++length)
    Check(!cache::ValidContainer({pixel.data(), length}, "ps_5_1"), "every container truncation refused");
  auto bad = pixel; bad[0] ^= 1;
  Check(!cache::ValidContainer(bad, "ps_5_1"), "bad DXBC magic refused");
  bad = pixel; Put32(bad, 28, UINT32_MAX);
  Check(!cache::ValidContainer(bad, "ps_5_1"), "chunk table overflow refused");
  bad = pixel; Put32(bad, 32, 0);
  Check(!cache::ValidContainer(bad, "ps_5_1"), "chunk overlaps directory refused");
  bad = pixel; Put32(bad, 32, UINT32_MAX);
  Check(!cache::ValidContainer(bad, "ps_5_1"), "chunk offset overflow refused");
  bad = pixel; Put32(bad, 40, UINT32_MAX);
  Check(!cache::ValidContainer(bad, "ps_5_1"), "chunk size overflow refused");
  bad = pixel; Put32(bad, 44, 0x50);
  Check(!cache::ValidContainer(bad, "ps_5_1"), "wrong shader model refused");
  bad = pixel; Put32(bad, 48, 3);
  Check(!cache::ValidContainer(bad, "ps_5_1"), "code dword count mismatch refused");
}
void TestFiles(const std::filesystem::path& root) {
  const auto directory = root / "files";
  cache::Digest key{}; key[0] = 42;
  const auto fixture = Fixture();
  std::vector<uint8_t> loaded{1, 2, 3};
  Check(!cache::Load(directory, key, "ps_5_1", loaded) && loaded.empty(), "missing file miss clears output");
  Check(cache::Store(directory, key, "ps_5_1", fixture), "first publish");
  Check(cache::Load(directory, key, "ps_5_1", loaded) && loaded == fixture, "published bytes round trip");
  const auto path = cache::EntryPath(directory, key);
  const auto saved = Read(path);
  Check(cache::Store(directory, key, "ps_5_1", fixture) && Read(path) == saved, "identical publication preserves winner");
  Check(!cache::Load(directory, key, "vs_5_1", loaded), "disk hit rejects wrong shader stage");
  for (size_t length = 0; length < saved.size(); ++length) {
    Write(path, {saved.data(), length});
    loaded = {1};
    Check(!cache::Load(directory, key, "ps_5_1", loaded) && loaded.empty(), "every entry truncation refused");
  }
  for (size_t byte = 0; byte < saved.size(); ++byte) {
    auto corrupt = saved; corrupt[byte] ^= 1; Write(path, corrupt);
    Check(!cache::Load(directory, key, "ps_5_1", loaded), "every single-byte entry corruption refused");
  }
  auto corrupt = saved; corrupt.push_back(0); Write(path, corrupt);
  Check(!cache::Load(directory, key, "ps_5_1", loaded), "trailing entry bytes refused");
  Check(!cache::Store(directory, key, "ps_5_1", fixture) && Read(path) == corrupt,
        "corrupt existing entry not replaced");
  Write(path, saved);
  cache::Digest wrong_key = key; wrong_key[1] = 1;
  Write(cache::EntryPath(directory, wrong_key), saved);
  Check(!cache::Load(directory, wrong_key, "ps_5_1", loaded), "filename cannot substitute for embedded request key");
  wrong_key[1] = 2;
  const auto collision = cache::EntryPath(directory, wrong_key);
  Check(std::filesystem::create_directory(collision), "test directory collision created");
  Check(!cache::Store(directory, wrong_key, "ps_5_1", fixture) && std::filesystem::is_directory(collision),
        "directory collision cannot be replaced");
  const auto blocker = root / "not_a_directory"; Write(blocker, fixture);
  Check(!cache::Store(blocker / "child", key, "ps_5_1", fixture), "unwritable destination is ordinary failure");
  Check(!cache::Store(directory, key, "vs_5_1", fixture), "store refuses wrong stage");
}
void TestParallel(const std::filesystem::path& root) {
  const auto directory = root / "parallel";
  cache::Digest key{}; key[0] = 99;
  const auto fixture = Fixture();
  std::atomic<bool> go{false}, stop{false}, invalid_bytes{false};
  std::atomic<unsigned> observed{0}, stored{0}, failed_stores{0};
  const auto rejected_before = cache::GetStats().rejected;
  std::vector<std::thread> readers, writers;
  for (unsigned n = 0; n < 4; ++n) readers.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    while (!stop.load()) {
      std::vector<uint8_t> value;
      if (cache::Load(directory, key, "ps_5_1", value)) {
        ++observed;
        if (value != fixture) invalid_bytes = true;
      } else if (!value.empty()) invalid_bytes = true;
      std::this_thread::yield();
    }
  });
  for (unsigned n = 0; n < 8; ++n) writers.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 8; ++i) {
      if (cache::Store(directory, key, "ps_5_1", fixture)) ++stored;
      else if (failed_stores.fetch_add(1) == 0) {
        const auto failure = cache::LastStoreFailure();
        std::printf("parallel first Store refusal: %s, system_error=%u\n", failure.operation, failure.system_error);
      }
    }
  });
  go = true;
  for (auto& writer : writers) writer.join();
  stop = true;
  for (auto& reader : readers) reader.join();
  std::printf("parallel: successful_stores=%u refused_stores=%u reader_hits=%u invalid_reader_bytes=%u\n",
              stored.load(), failed_stores.load(), observed.load(), unsigned(invalid_bytes.load()));
  // Optional cache writes may refuse transient Windows sharing errors. They may
  // never return wrong or partial data, and at least one writer must publish.
  Check(!invalid_bytes, "parallel readers never receive wrong bytes");
  Check(stored > 0, "at least one parallel writer succeeds");
  std::vector<uint8_t> final;
  Check(cache::Load(directory, key, "ps_5_1", final) && final == fixture,
        "parallel writers leave an exact fully validated final entry");
  Check(observed > 0, "readers observed published entry");
  Check(cache::GetStats().rejected == rejected_before, "readers never observed partial publication");
  unsigned files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    ++files; Check(entry.path().extension() == ".nbcso", "owned temporary files cleaned");
  }
  Check(files == 1, "parallel publication leaves one immutable entry");
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2 || !std::filesystem::is_directory(argv[1]))
      throw std::runtime_error("usage: test_native_shader_cache.exe <existing scratch parent>");
    std::filesystem::path root;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      const auto candidate = std::filesystem::path(argv[1]) /
          (L"shader_cache_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(attempt));
      if (std::filesystem::create_directory(candidate)) { root = candidate; break; }
    }
    if (root.empty()) throw std::runtime_error("could not claim unique test directory");
    TestHashesAndKeys(); TestContainers(); TestFiles(root); TestParallel(root);
    const auto stats = cache::GetStats();
    std::printf("PASS %u checks; hits=%llu rejected=%llu published=%llu; retained %ls\n", checks,
                static_cast<unsigned long long>(stats.hits), static_cast<unsigned long long>(stats.rejected),
                static_cast<unsigned long long>(stats.published), root.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what()); return 1;
  }
}
