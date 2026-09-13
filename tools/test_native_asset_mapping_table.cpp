// Original standalone CPU controls. Build/run separately; no SDK or game data.
#include "../src/gpu/native/native_asset_mapping_table.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Mapping {
  uint64_t version = 0;
  uint32_t asset_offset = 0;
  bool enabled = false;
};
using Small = nb::gpu::NativeAssetMappingTable<Mapping, 8, 3>;

void ExactKeysAndReplacement() {
  nb::gpu::NativeAssetMappingTable<Mapping, 64, 64> table;
  const std::array<uint64_t, 5> keys = {
      0, UINT64_MAX, (uint64_t(0x1000) << 32) | 64,
      (uint64_t(0x1000) << 32) | 68, (uint64_t(0x2000) << 32) | 64};
  for (size_t i = 0; i < keys.size(); ++i) {
    auto lookup = table.Find(keys[i]);
    Check(!lookup.value, "unseen exact key unexpectedly present");
    Check(table.Store(lookup, {7, uint32_t(i), i != 2}), "insert failed");
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    auto lookup = table.Find(keys[i]);
    Check(lookup.value && lookup.value->asset_offset == i, "base/length key aliases another key");
    Check(lookup.value->enabled == (i != 2), "negative mapping lost");
  }
  auto changed = table.Find(keys[2]);
  Check(table.Store(changed, {8, 99, true}), "same-key version replacement failed");
  auto replaced = table.Find(keys[2]);
  Check(replaced.value->version == 8 && replaced.value->asset_offset == 99,
        "updated invalidation version not retained");
  Check(table.size() == keys.size(), "replacement increased table size");
}

void CollisionWrapAndProbeRefusal() {
  Small table;
  std::vector<uint64_t> colliding;
  uint64_t independent = 0;
  // Observe initial buckets through the public empty-table lookup. These keys
  // force probing from the last cell through cells zero and one.
  for (uint64_t key = 1; colliding.size() < 4 || !independent; ++key) {
    const auto lookup = table.Find(key);
    if (lookup.slot == 7 && colliding.size() < 4) colliding.push_back(key);
    if (lookup.slot == 4) independent = key;
    Check(key < 10000, "could not form synthetic collision group");
  }
  for (size_t i = 0; i < 3; ++i) {
    auto lookup = table.Find(colliding[i]);
    Check(lookup.probes == i + 1, "linear-probe count or wrap is wrong");
    Check(table.Store(lookup, {1, uint32_t(i), true}), "collision insert failed");
  }
  auto refused = table.Find(colliding[3]);
  Check(!refused.value && refused.probes == 3, "probe budget was not bounded");
  Check(!table.Store(refused, {1, 3, true}), "exhausted lookup overwrote another mapping");
  for (size_t i = 0; i < 3; ++i) {
    auto kept = table.Find(colliding[i]);
    Check(kept.value && kept.value->asset_offset == i, "probe refusal discarded a valid mapping");
  }
  Check(table.Store(table.Find(independent), {1, 77, true}), "probe refusal disabled unrelated buckets");
  Check(table.size() == 4, "refused storage changed occupancy");
}

void FullCapacityRetainsMappings() {
  nb::gpu::NativeAssetMappingTable<Mapping, 8, 8> table;
  for (uint64_t key = 1; key <= 8; ++key)
    Check(table.Store(table.Find(key), {key, uint32_t(key), true}), "full-table fixture insert failed");
  auto ninth = table.Find(9);
  Check(!ninth.value && ninth.probes == 8, "full-table lookup did not terminate");
  Check(!table.Store(ninth, {9, 9, true}), "full table accepted an extra mapping");
  for (uint64_t key = 1; key <= 8; ++key)
    Check(table.Find(key).value->version == key, "full-table refusal evicted existing mapping");
}

void NewGenerationDropsPositiveAndNegative() {
  Small table;
  Check(table.Store(table.Find(10), {4, 20, true}), "positive insert failed");
  Check(table.Store(table.Find(11), {4, 0, false}), "negative insert failed");
  auto stale = table.Find(10);
  Check(!table.Clear(), "first generation reset unexpectedly wrapped");
  Check(!table.Find(10).value && !table.Find(11).value && !table.size(),
        "new frame or mode switch reused a previous validation");
  Check(!table.Store(stale, {5, 21, true}), "old lookup ticket published into a new generation");
  Check(table.Store(table.Find(10), {5, 21, true}), "fresh validation could not replace prior frame");
  Check(table.Find(10).value->version == 5, "new validation version not visible");
}

void GenerationWrapDropsAncientMappings() {
  nb::gpu::NativeAssetMappingTable<Mapping, 8, 8, uint8_t> table;
  Check(table.Store(table.Find(1), {1, 123, true}), "wrap fixture insert failed");
  uint32_t wraps = 0;
  for (uint32_t i = 0; i < 255; ++i) wraps += table.Clear() ? 1 : 0;
  Check(wraps == 1, "generation wrap count differs");
  Check(!table.Find(1).value, "wrapped generation resurrected ancient mapping");
  Check(table.Store(table.Find(1), {2, 456, true}), "post-wrap storage failed");
  Check(table.Find(1).value->version == 2, "post-wrap lookup returned ancient value");
}

void MoreThanLegacyLimitWithoutGlobalReset() {
  nb::gpu::NativeAssetMappingTable<Mapping, 65536> table;
  constexpr uint32_t count = 12000;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t key = (uint64_t(0x1000 + i * 32) << 32) | (64 + (i % 31) * 4);
    Check(table.Store(table.Find(key), {19, i, bool(i & 1)}), "production table fixture exceeded probe budget");
  }
  Check(table.size() == count, "production table cleared at the old8192 limit");
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t key = (uint64_t(0x1000 + i * 32) << 32) | (64 + (i % 31) * 4);
    auto value = table.Find(key).value;
    Check(value && value->asset_offset == i && value->enabled == bool(i & 1),
          "larger frame lost a positive or negative mapping");
  }
}
}

int main() {
  try {
    ExactKeysAndReplacement();
    CollisionWrapAndProbeRefusal();
    FullCapacityRetainsMappings();
    NewGenerationDropsPositiveAndNegative();
    GenerationWrapDropsAncientMappings();
    MoreThanLegacyLimitWithoutGlobalReset();
    std::cout << "PASS: 6 native asset mapping table controls\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
