// Original CPU-only controls against the production policy, without a game,
// graphics device, shader compiler or SDK-linked shader implementation.
// cl /nologo /EHsc /std:c++20 /W4 /WX tools\test_shader_load_memo.cpp /Fe:<scratch>\test_shader_load_memo.exe
#include "../src/gpu/vendored/include/rex/graphics/d3d12/shader_load_memo.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using Cache = rex::graphics::d3d12::ShaderLoadMemo;
namespace {
unsigned checks = 0;
void Check(bool okay, const char* label) {
  ++checks;
  if (!okay) { std::fprintf(stderr, "FAIL %u: %s\n", checks, label); std::exit(1); }
}
std::vector<uint32_t> HostWords(const uint8_t* raw, size_t bytes) {
  std::vector<uint32_t> result(bytes / 4);
  for (size_t i = 0; i < result.size(); ++i) {
    const size_t at = i * 4;
    result[i] = uint32_t(raw[at]) * 16777216u + uint32_t(raw[at + 1]) * 65536u +
                uint32_t(raw[at + 2]) * 256u + raw[at + 3];
  }
  return result;
}
void LiteralControls() {
  auto cache = std::make_unique<Cache>();  // keep the bounded 512 KiB payload off the Windows stack
  std::array<uint8_t, 8> live{{0x12, 0x34, 0x56, 0x78, 0xAA, 0xBB, 0xCC, 0xDD}};
  std::array<uint32_t, 2> immutable{{0x12345678, 0xAABBCCDD}};
  Check(cache->Find(live.data(), 0, 2) == 0, "cold miss");
  Check(cache->Publish(live.data(), 0, 2, immutable.data(), 100), "publish host-order shader bytes");
  Check(cache->Find(live.data(), 0, 2) == 100, "literal guest-endian bytes hit");
  Check(cache->stats().hits == 1 && cache->stats().misses == 1 &&
        cache->stats().bytes_compared == 8 && cache->stats().bytes_hash_avoided == 8, "lookup counters");
  for (size_t i = 0; i < live.size(); ++i) {
    live[i] ^= 1;
    Check(cache->Find(live.data(), 0, 2) == 0, "changed byte at same guest address misses");
    live[i] ^= 1;
    Check(cache->Find(live.data(), 0, 2) == 100, "restored exact bytes hit");
  }
  Check(cache->stats().content_misses == 8, "all mutations classified as content misses");
  immutable[0] ^= 0xFF;
  Check(cache->Find(live.data(), 0, 2) == 100, "stored bytes do not alias host Shader array");
  immutable[0] ^= 0xFF;
  auto alternate = live;
  Check(cache->Find(alternate.data(), 0, 2) == 0, "different source address is only a miss");
  Check(cache->Find(live.data(), 1, 2) == 0, "same bytes with different stage cannot hit");
  Check(cache->Find(live.data(), 0, 1) == 0, "same address with shorter count cannot hit");
  Check(cache->Publish(live.data(), 1, 2, immutable.data(), 200), "publish pixel-stage identity");
  Check(cache->Find(live.data(), 1, 2) == 200, "pixel stage returns its own identity");

  // Model a guest write between the original loader's read and publication.
  live[7] ^= 4;
  Check(cache->Publish(live.data(), 0, 2, immutable.data(), 100), "publish original immutable Shader after guest mutation");
  Check(cache->Find(live.data(), 0, 2) == 0, "later guest bytes cannot acquire old Shader identity");
  live[7] ^= 4;
  Check(cache->Find(live.data(), 0, 2) == 100, "only bytes represented by immutable Shader hit");
  cache->Reset();
  Check(cache->Find(live.data(), 0, 2) == 0, "shutdown/storage reload invalidates pointer before destruction");
  Check(cache->Publish(live.data(), 0, 2, immutable.data(), 300), "publish replacement owner at same guest address");
  Check(cache->Find(live.data(), 0, 2) == 300, "post-reset lookup cannot return deleted owner");
  Check(cache->stats().resets == 1, "cumulative stats survive reset");

  const auto* inaccessible = reinterpret_cast<const void*>(uintptr_t(1));
  const auto* inaccessible_words = reinterpret_cast<const uint32_t*>(uintptr_t(1));
  Check(cache->Find(inaccessible, 0, 0) == 0, "zero bytes bypass without source read");
  Check(cache->Find(inaccessible, 0, Cache::kMaxDwords + 1) == 0, "oversized input bypass without source read");
  Check(cache->Find(inaccessible, 0, UINT32_MAX) == 0, "huge count bypass before multiplication/read");
  Check(cache->Find(inaccessible, 2, 2) == 0, "unknown stage bypass without source read");
  Check(cache->Find(nullptr, 0, 2) == 0, "null source bypass");
  Check(!cache->Publish(inaccessible, 0, UINT32_MAX, inaccessible_words, 400), "oversized publication does not read source");
  Check(!cache->Publish(live.data(), 0, 2, nullptr, 400), "null immutable data not published");
  Check(!cache->Publish(live.data(), 0, 2, immutable.data(), 0), "failed Shader identity not published");
  Check(cache->Find(live.data(), 0, 2) == 300, "invalid publication leaves valid mapping intact");
  Check(cache->stats().bypasses == 5, "all bypass cases counted exactly once");
}
void CapacityAndCollisions() {
  auto cache = std::make_unique<Cache>();
  // 129 distinct valid addresses force at least 65 evictions, regardless of how
  // candidate slots are selected. A collision may miss; it must never misidentify.
  std::array<std::array<uint8_t, 4>, 129> input{};
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = {{uint8_t(i), 0x23, 0x45, 0x67}};
    const uint32_t word = uint32_t(i) * 16777216u + 0x234567u;
    Check(cache->Publish(input[i].data(), 0, 1, &word, i + 1), "collision fixture publication");
    Check(cache->Find(input[i].data(), 0, 1) == i + 1, "new publication immediately hits");
  }
  unsigned resident = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    const uintptr_t found = cache->Find(input[i].data(), 0, 1);
    Check(!found || found == i + 1, "direct-map collision cannot return another source's Shader");
    resident += found ? 1u : 0u;
  }
  Check(resident <= Cache::kEntries && cache->stats().evictions >= 65, "entry count remains bounded");
  cache->Reset();
  for (auto& bytes : input) Check(cache->Find(bytes.data(), 0, 1) == 0, "reset clears every possible slot");

  std::vector<uint8_t> storage(Cache::kMaxDwords * 4 + 1);
  uint8_t* unaligned = storage.data() + 1;
  for (size_t i = 0; i < Cache::kMaxDwords * 4; ++i) unaligned[i] = uint8_t(i * 17 + 31);
  const auto host = HostWords(unaligned, Cache::kMaxDwords * 4);
  Check(cache->Publish(unaligned, 1, Cache::kMaxDwords, host.data(), 999), "maximum-sized unaligned guest input");
  for (size_t i = 0; i < Cache::kMaxDwords * 4; ++i) {
    unaligned[i] ^= 1;
    Check(cache->Find(unaligned, 1, Cache::kMaxDwords) == 0, "every byte of maximum extent affects eligibility");
    unaligned[i] ^= 1;
  }
  Check(cache->Find(unaligned, 1, Cache::kMaxDwords) == 999, "maximum extent restored");
  Check(Cache::kPayloadBytes == 524288, "fixed independent byte storage bound");
}
uint32_t Random(uint32_t& state) {
  state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}
void IndependentOwnerOracle() {
  auto cache = std::make_unique<Cache>();
  std::array<std::array<uint8_t, 32>, 96> inputs{};
  // The oracle owns a permanent identity for each exact stage+length+byte
  // sequence. It does not know the memo's slot hash or eviction strategy.
  std::map<std::string, uintptr_t> owners;
  uintptr_t next_owner = 1000;
  uint32_t random = 0x68251A39;
  for (unsigned event = 0; event < 12000; ++event) {
    const uint32_t choice = Random(random);
    auto& live = inputs[choice % inputs.size()];
    const uint32_t count = 1 + ((choice >> 8) % 8), stage = (choice >> 16) & 1;
    if ((choice & 7) == 0) live[(choice >> 17) % (count * 4)] ^= uint8_t(1u << ((choice >> 23) & 7));
    if (event % 127 == 0) { cache->Reset(); owners.clear(); }
    std::string key; key.push_back(char(stage)); key.push_back(char(count));
    key.append(reinterpret_cast<const char*>(live.data()), count * 4);
    const auto owner = owners.try_emplace(key, next_owner);
    if (owner.second) ++next_owner;
    const uintptr_t expected = owner.first->second;
    const uintptr_t found = cache->Find(live.data(), stage, count);
    Check(!found || found == expected, "hit always names current exact-byte owner across writes and resets");
    if (!found) {
      const auto host = HostWords(live.data(), count * 4);
      Check(cache->Publish(live.data(), stage, count, host.data(), expected), "oracle miss publication");
    }
    Check(cache->Find(live.data(), stage, count) == expected, "post-publication exact match");
  }
}
}  // namespace
int main() {
  LiteralControls(); CapacityAndCollisions(); IndependentOwnerOracle();
  std::printf("PASS %u shader load memo controls; exact live bytes, bounded collisions, immutable publication, owner resets\n", checks);
  return 0;
}
