// Original CPU controls for the production owner-local translation memo policy.
// The SDK integration's is_new writes and deletion order are reviewed separately.
#include "../src/gpu/native/native_translation_lookup_memo.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace {
struct Translation {
  uint64_t identity;
  bool ready = false;
  bool valid = false;
};
using Memo = nb::gpu::NativeTranslationLookupMemo<Translation>;
using Stats = nb::gpu::NativeTranslationLookupMemoStats;
static_assert(!std::is_copy_constructible_v<Memo> && !std::is_move_constructible_v<Memo>);
static_assert(!std::is_copy_assignable_v<Memo> && !std::is_move_assignable_v<Memo>);
uint64_t checks = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}

void ExactAndLiveIdentity() {
  Memo memo;
  Stats stats;
  Translation first{1}, second{2};
  constexpr uint64_t key = 0x0123456789ABCDEFull;
  Check(!memo.Find(key, stats), "new owner has no accepted lookup");
  memo.Publish(key, &first, stats);
  Check(memo.Find(key, stats) == &first, "exact modification lookup");
  for (unsigned bit = 0; bit < 64; ++bit) {
    Check(!memo.Find(key ^ (uint64_t(1) << bit), stats), "every modification bit matters");
  }
  Check(memo.Find(key, stats) == &first, "miss preserves last successful entry");
  memo.Publish(key + 1, nullptr, stats);
  Check(memo.Find(key, stats) == &first && stats.publications == 1,
        "null result never replaces successful identity");
  Check(!first.ready && !first.valid, "untranslated identity is a legitimate map result");
  first.ready = first.valid = true;
  Check(memo.Find(key, stats)->ready && memo.Find(key, stats)->valid,
        "readiness is read from the live object, not cached");
  first.valid = false;
  Check(!memo.Find(key, stats)->valid, "later invalidity remains visible");
  Check(!memo.Invalidate(key + 1) && memo.Find(key, stats) == &first,
        "destroying another key preserves this object");
  Check(memo.Invalidate(key) && !memo.Find(key, stats), "matching deletion forgets identity");
  Check(!memo.Invalidate(key), "repeat deletion has no retained identity");
  memo.Publish(0, &first, stats);
  Check(memo.Find(0, stats) == &first, "zero is a real modification key");
  memo.Publish(UINT64_MAX, &second, stats);
  Check(memo.Find(UINT64_MAX, stats) == &second, "maximum key has no sentinel meaning");
}

void OwnershipAndAddressReuse() {
  Memo first_owner, second_owner;
  Stats stats;
  Translation first{1}, second{2};
  first_owner.Publish(7, &first, stats);
  second_owner.Publish(7, &second, stats);
  Check(first_owner.Find(7, stats) == &first && second_owner.Find(7, stats) == &second,
        "equal modification in different Shader owners cannot alias");

  alignas(Translation) std::array<unsigned char, sizeof(Translation)> storage{};
  auto* reused = std::construct_at(reinterpret_cast<Translation*>(storage.data()), Translation{3});
  first_owner.Publish(11, reused, stats);
  // Invalidating remains mandatory with the optimization switched off.
  Check(first_owner.Invalidate(11), "off-mode deletion invalidates before destruction");
  std::destroy_at(reused);
  reused = std::construct_at(reinterpret_cast<Translation*>(storage.data()), Translation{4});
  Check(!first_owner.Find(11, stats), "same-address replacement cannot revive old lookup");
  first_owner.Publish(11, reused, stats);
  Check(first_owner.Find(11, stats)->identity == 4, "new lifetime publishes explicitly");
  first_owner.Reset();  // Shader destructor clears before deleting translations.
  Check(!first_owner.Find(11, stats), "owner teardown removes all borrowed identity");
  std::destroy_at(reused);
  Check(second_owner.Find(7, stats) == &second, "other live owner remains intact");

  std::unordered_map<uint64_t, std::unique_ptr<Translation>> map;
  map.emplace(42, std::make_unique<Translation>(Translation{42}));
  Translation* stable = map.at(42).get();
  first_owner.Publish(42, stable, stats);
  for (uint64_t i = 100; i < 2000; ++i) map.emplace(i, std::make_unique<Translation>(Translation{i}));
  Check(first_owner.Find(42, stats) == stable && map.at(42).get() == stable,
        "owner map growth does not relocate heap-owned translations");
  first_owner.Reset();
}

void SequenceAgainstValueOracle() {
  Memo memo;
  Stats stats;
  std::mt19937_64 random(0x2A19072026ull);
  std::array<uint64_t, 37> keys{};
  for (auto& key : keys) key = random();
  // Expected memo identity is represented by a key and monotonically assigned
  // lifetime number; it never stores the borrowed pointer under test.
  std::optional<std::pair<uint64_t, uint64_t>> expected;
  std::unordered_map<uint64_t, std::unique_ptr<Translation>> live;
  uint64_t next_identity = 1;
  bool enabled = false;
  for (unsigned event = 0; event < 24000; ++event) {
    const uint64_t key = keys[random() % keys.size()];
    switch (random() % 6) {
      case 0: enabled = !enabled; break;
      case 1: {
        const bool was_cached = expected && expected->first == key;
        Check(memo.Invalidate(key) == was_cached, "deletion invalidates exactly the cached lifetime");
        if (was_cached) expected.reset();
        live.erase(key);  // after invalidation, independent of enabled state
        break;
      }
      case 2:
        memo.Reset(); expected.reset(); live.clear(); break;
      default: {
        Translation* hit = enabled ? memo.Find(key, stats) : nullptr;
        if (enabled) {
          const bool expected_hit = expected && expected->first == key;
          Check(bool(hit) == expected_hit, "lookup presence matches value oracle");
          if (hit) Check(hit->identity == expected->second, "hit retains exact live lifetime");
        }
        auto it = live.find(key);
        if (it == live.end()) {
          it = live.emplace(key, std::make_unique<Translation>(Translation{next_identity++})).first;
        }
        if (hit) Check(hit == it->second.get(), "accepted hit agrees with authoritative map");
        else if (enabled) {
          memo.Publish(key, it->second.get(), stats);
          expected = std::make_pair(key, it->second->identity);
        }
        break;
      }
    }
  }
  memo.Reset();
  Check(stats.hits > 0 && stats.misses > 0 && stats.publications > 0,
        "random controls exercise hits, misses and publication");
}
}  // namespace

int main() {
  ExactAndLiveIdentity();
  OwnershipAndAddressReuse();
  SequenceAgainstValueOracle();
  std::cout << "PASS " << checks << " translation lookup memo checks\n";
}
