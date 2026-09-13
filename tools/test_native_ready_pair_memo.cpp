// Original CPU controls for exact keys, borrowed object identity and lifecycle.
// NativeShaderLibrary keeps its compile/filter/semantic checks outside this
// policy; those checks must still run after a successful memo lookup.
#include "../src/gpu/native/native_ready_pair_memo.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <tuple>
#include <utility>

namespace {
struct Pair {
  uint64_t vertex_hash;
  uint64_t pixel_hash;
  bool usable;
  bool enabled;
};
using Memo = nb::gpu::NativeReadyPairMemo<Pair>;
uint64_t checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL: " << reason << '\n';
    std::exit(1);
  }
}

void ExactKeysAndLiveObject() {
  Memo memo;
  Pair first{0x123456789ABCDEFull, 0xFEDCBA9876543210ull, true, true};
  Check(!memo.Find(first.vertex_hash, first.pixel_hash), "initial miss");
  memo.Publish(first.vertex_hash, first.pixel_hash, &first);
  Check(memo.Find(first.vertex_hash, first.pixel_hash) == &first, "exact full hashes hit");
  for (unsigned bit = 0; bit < 64; ++bit) {
    Check(!memo.Find(first.vertex_hash ^ (uint64_t(1) << bit), first.pixel_hash),
          "every VS hash bit matters");
    Check(!memo.Find(first.vertex_hash, first.pixel_hash ^ (uint64_t(1) << bit)),
          "every PS hash bit matters");
  }
  Check(memo.Find(first.vertex_hash, first.pixel_hash) == &first, "misses preserve last ready entry");
  // The memo does not freeze readiness or filtering. Production must inspect
  // the returned object again, even if an accepted pair later becomes unusable.
  first.usable = false;
  first.enabled = false;
  Pair* hit = memo.Find(first.vertex_hash, first.pixel_hash);
  Check(hit == &first && !hit->usable && !hit->enabled, "live usability and filter state remain visible");
  first.vertex_hash ^= 1;
  hit = memo.Find(first.vertex_hash ^ 1, first.pixel_hash);
  Check(hit == &first && hit->vertex_hash != (first.vertex_hash ^ 1),
        "live full-hash guard can reject changed semantic identity");
  memo.Publish(0, 0, nullptr);
  Check(memo.stats().publications == 1, "null publication is not a ready result");

  Pair depth{9, 0, true, true};
  memo.Publish(9, 0, &depth);
  Check(memo.Find(9, 0) == &depth, "PS zero is a real depth-only key");
  Check(!memo.Find(0, 9), "stage order matters");
  // These distinct tuples have identical production PairKey values. They must
  // still miss this memo so the existing map's full-hash collision guard runs.
  constexpr uint64_t folded_collision_vs = 9 ^ 0x9E3779B97F4A7C15ull;
  Check(!memo.Find(folded_collision_vs, 1), "folded-map collision cannot become memo hit");
  Check(memo.Find(9, 0) == &depth, "collision miss leaves accepted pair intact");
}

void LifetimeAndLibraryControls() {
  Memo first_library, second_library;
  Pair first{12, 34, true, true}, second{12, 34, true, true};
  first_library.Publish(12, 34, &first);
  second_library.Publish(12, 34, &second);
  Check(first_library.Find(12, 34) == &first && second_library.Find(12, 34) == &second,
        "effective sampler libraries retain separate Pair owners");
  first_library.Reset();  // before Load clears objects, including a failed Load
  first = {56, 78, true, true};
  Check(!first_library.Find(12, 34), "reset prevents same-address old-key reuse");
  Check(!first_library.Find(56, 78), "replacement object is absent until ready publication");
  first_library.Publish(56, 78, &first);
  Check(first_library.Find(56, 78) == &first, "new lifetime can publish same address");
  Check(second_library.Find(12, 34) == &second, "other library unaffected by reset");
  Check(first_library.stats().resets == 1, "reset count retained");
  Memo moved(std::move(first_library));
  Check(moved.Find(56, 78) == &first, "library move keeps heap-owned Pair reference");
  Check(!first_library.Find(56, 78), "moved-from library cannot borrow new owner's Pair");
  second_library = std::move(moved);
  Check(second_library.Find(56, 78) == &first, "move assignment replaces previous identity");
  Check(!second_library.Find(12, 34) && !moved.Find(56, 78), "move assignment forgets both stale entries");
  second_library.Reset();
  Check(!second_library.Find(56, 78), "final Load reset clears moved cache");
}

void IndependentSequenceOracle() {
  Memo memo;
  std::array<Pair, 19> pairs{};
  for (size_t i = 0; i < pairs.size(); ++i) {
    pairs[i] = {uint64_t(i * 73 + 1), uint64_t(i * 97), true, true};
  }
  // The oracle remembers the last accepted tuple as a value, not a hash or
  // pointer cache implementation. Misses and simulated pending results leave it.
  std::optional<std::tuple<uint64_t, uint64_t, size_t>> expected;
  std::mt19937 rng(0x792004);
  uint64_t publications = 0, resets = 0, hits = 0, misses = 0;
  for (unsigned event = 0; event < 16000; ++event) {
    const size_t index = rng() % pairs.size();
    Pair& pair = pairs[index];
    switch (rng() % 8) {
      case 0:
        memo.Reset(); expected.reset(); ++resets; break;
      case 1:
      case 2:
        memo.Publish(pair.vertex_hash, pair.pixel_hash, &pair);
        expected = std::make_tuple(pair.vertex_hash, pair.pixel_hash, index);
        ++publications;
        break;
      default: {
        Pair* wanted = nullptr;
        if (expected && std::get<0>(*expected) == pair.vertex_hash &&
            std::get<1>(*expected) == pair.pixel_hash) wanted = &pairs[std::get<2>(*expected)];
        Pair* actual = memo.Find(pair.vertex_hash, pair.pixel_hash);
        Check(actual == wanted, "sequence matches last accepted exact tuple oracle");
        if (wanted) ++hits; else ++misses;
        break;
      }
    }
  }
  const auto& stats = memo.stats();
  Check(stats.hits == hits && stats.misses == misses, "lookup counters match independent oracle");
  Check(stats.publications == publications && stats.resets == resets, "publication/reset counters match oracle");
}
}  // namespace

int main() {
  ExactKeysAndLiveObject();
  LifetimeAndLibraryControls();
  IndependentSequenceOracle();
  std::cout << "PASS: " << checks << " ready-pair memo checks\n";
}
