// Original CPU tests; no SDK, D3D12, texture assets, or driver required.
#include "../src/gpu/native/native_texture_descriptor_memo.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <random>

namespace {
using nb::gpu::NativeTextureDescriptorMemo;
using nb::gpu::NativeTextureDescriptorMemoStats;
uint64_t checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL: " << reason << '\n';
    std::exit(1);
  }
}

void ExactKeyAndFailureControls() {
  NativeTextureDescriptorMemo memo;
  NativeTextureDescriptorMemoStats stats;
  uint32_t output = 123;
  Check(!memo.Find(0, output, stats) && output == 123, "unpublished key zero misses without changing output");
  memo.Publish(0, 0, stats);
  Check(memo.Find(0, output, stats) && output == 0, "descriptor zero is a valid success");
  constexpr uint32_t key = 0xA5C37B19;
  memo.Publish(key, 98123, stats);
  for (unsigned bit = 0; bit < 32; ++bit) {
    output = 42;
    Check(!memo.Find(key ^ (uint32_t(1) << bit), output, stats) && output == 42,
          "every full descriptor-key bit participates");
  }
  Check(memo.Find(key, output, stats) && output == 98123, "misses retain previous successful identity");
  const auto publications = stats.publications;
  memo.Publish(key, UINT32_MAX, stats);
  memo.Publish(key ^ 1, UINT32_MAX, stats);
  Check(stats.publications == publications, "failed descriptors are never published");
  Check(memo.Find(key, output, stats) && output == 98123, "failed same-key publication cannot replace success");
  Check(!memo.Find(key ^ 1, output, stats), "failed other-key lookup cannot become hit");
  memo.Publish(UINT32_MAX, UINT32_MAX - 1, stats);
  Check(memo.Find(UINT32_MAX, output, stats) && output == UINT32_MAX - 1,
        "entire key and successful descriptor index ranges retained");
  memo.Reset();
  Check(!memo.Find(UINT32_MAX, output, stats), "reset discards released descriptor");
}

void OwnerLifetimeControls() {
  NativeTextureDescriptorMemoStats stats;
  std::array<NativeTextureDescriptorMemo, 2> textures;
  textures[0].Publish(0x1357, 10, stats);
  textures[1].Publish(0x1357, 20, stats);
  uint32_t index = 0;
  Check(textures[0].Find(0x1357, index, stats) && index == 10, "first live texture owns its descriptor");
  Check(textures[1].Find(0x1357, index, stats) && index == 20, "same key on another texture remains distinct");
  std::optional<NativeTextureDescriptorMemo> storage(std::in_place);
  storage->Publish(71, 5, stats);
  storage.reset();
  storage.emplace();
  Check(!storage->Find(71, index, stats), "destroyed texture cannot retain recycled descriptor identity");
  storage->Publish(71, 500, stats);
  Check(storage->Find(71, index, stats) && index == 500, "replacement texture publishes its new descriptor");
  Check(textures[0].Find(0x1357, index, stats) && index == 10, "other resource lifetime unaffected");
}

void AppendOnlyMapOracle() {
  struct Texture {
    NativeTextureDescriptorMemo memo;
    std::map<uint32_t, uint32_t> descriptors;
  };
  std::array<Texture, 7> textures;
  NativeTextureDescriptorMemoStats stats;
  std::mt19937 rng(0x4A82170);
  uint32_t next_index = 0;
  for (unsigned event = 0; event < 16000; ++event) {
    auto& texture = textures[rng() % textures.size()];
    // Include all sign/dimension combinations and arbitrary full swizzle bits.
    const uint32_t key = rng() % 37;
    const bool enabled = (rng() & 1u) != 0;
    if ((rng() % 31) == 0) {
      // Independent object destruction/recreation; previous indices may now be
      // returned to the descriptor allocator, so identity must start empty.
      texture.descriptors.clear();
      texture.memo.Reset();
      continue;
    }
    if ((rng() % 3) == 0) {
      const auto inserted = texture.descriptors.emplace(key, next_index++);
      if (enabled) texture.memo.Publish(key, inserted.first->second, stats);
      uint32_t cached = UINT32_MAX;
      if (enabled) {
        Check(texture.memo.Find(key, cached, stats) && cached == inserted.first->second,
              "duplicate insertion publishes map's actual retained value");
      }
      continue;
    }
    const auto expected = texture.descriptors.find(key);
    const uint32_t wanted = expected == texture.descriptors.end() ? UINT32_MAX : expected->second;
    uint32_t actual = UINT32_MAX;
    if (!enabled || !texture.memo.Find(key, actual, stats)) {
      actual = wanted;
      if (enabled) texture.memo.Publish(key, actual, stats);
    }
    Check(actual == wanted, "enabled/disabled result equals independent live-owner map");
  }
  Check(stats.hits != 0 && stats.misses != 0 && stats.publications != 0,
        "oracle exercised hits, misses and successful publications");
}
}  // namespace

int main() {
  ExactKeyAndFailureControls();
  OwnerLifetimeControls();
  AppendOnlyMapOracle();
  std::cout << "PASS: " << checks << " texture descriptor memo checks\n";
}
