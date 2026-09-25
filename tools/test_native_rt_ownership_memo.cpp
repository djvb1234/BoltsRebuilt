// Original CPU controls for the render target claim memo used by the vendored
// RenderTargetCache::Update. A tile-level model of the cache (ChangeOwnership
// with EDRAM wrapping, Update's non-overlapping and often empty ranges, claims
// from outside Update, ClearCache deleting targets that own no tile, the
// interlock barrier and DestroyAllRenderTargets) runs with and without the memo
// in lockstep and must agree exactly. The SDK integration (the generation bump
// sites, D3D12 behaviour) is reviewed separately.
#include "../src/gpu/native/native_rt_ownership_memo.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace {
struct Key {
  uint32_t key = 0;
};
struct Target {
  uint32_t key;
};
using Memo = nb::gpu::NativeRtOwnershipMemo<Key, Target>;
constinit Memo constant_initialized_memo;  // as the static in Update needs
constexpr uint32_t kSlots = nb::gpu::kNativeRtOwnershipSlots;
constexpr uint32_t kTiles = 2048;
static_assert(kSlots == 5);

uint64_t checks = 0;
void Check(bool condition, const char* why) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << why << '\n';
    std::exit(1);
  }
}

// RenderTargetKey layout: base 11 bits, pitch 8, msaa 2, depth 1, format 4.
uint32_t MakeKey(uint32_t base, uint32_t pitch, uint32_t msaa, uint32_t depth, uint32_t format) {
  return base | (pitch << 11) | (msaa << 19) | (depth << 21) | (format << 22);
}

void DirectControls() {
  Memo memo;
  int owner_a = 0, owner_b = 0;
  Target t0{1}, t2{2}, poison{3};
  Key keys[kSlots];
  keys[0].key = MakeKey(0, 80, 0, 1, 0);
  keys[2].key = MakeKey(600, 80, 0, 0, 6);
  keys[1].key = 0xDEADBEEF;  // unused slots are never read
  keys[3].key = 0x12345678;
  const uint32_t used = 0b101;
  uint32_t lengths[kSlots] = {600, 400, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};  // past count: unread
  Target* targets[kSlots] = {&t0, &poison, &t2, &poison, &poison};

  Check(!memo.Matches(&owner_a, 0, 0, keys, lengths, 0), "a new memo never matches, even all-zero input");
  Check(!constant_initialized_memo.Matches(&owner_a, 0, 0, keys, lengths, 0),
        "a constant-initialized memo never matches");
  memo.Publish(&owner_a, 7, used, keys, lengths, 2, targets);
  Check(memo.Matches(&owner_a, 7, used, keys, lengths, 2), "exact claim matches");
  Target* out[kSlots] = {&poison, &poison, &poison, &poison, &poison};
  memo.CopyTargets(out);
  Check(out[0] == &t0 && out[2] == &t2, "used slots return the published targets");
  Check(!out[1] && !out[3] && !out[4], "unused slots return nullptr, never an unread input");

  Check(!memo.Matches(&owner_b, 7, used, keys, lengths, 2), "another cache never matches");
  Check(!memo.Matches(&owner_a, 8, used, keys, lengths, 2), "a later generation never matches");
  Check(!memo.Matches(&owner_a, 6, used, keys, lengths, 2), "an earlier generation never matches");
  Check(!memo.Matches(&owner_a, 7, used | 0b10, keys, lengths, 3), "an added slot misses");
  Check(!memo.Matches(&owner_a, 7, 0b001, keys, lengths, 1), "a removed slot misses");
  Check(!memo.Matches(&owner_a, 7, 0b110, keys, lengths, 2), "same count, other slots miss");
  Check(!memo.Matches(&owner_a, 7, used, keys, lengths, 1), "a different count misses");
  Key zero_slot[kSlots];
  std::copy(keys, keys + kSlots, zero_slot);
  zero_slot[1].key = 0;
  Check(!memo.Matches(&owner_a, 7, 0b011, zero_slot, lengths, 2),
        "the used-slot mask is exact, even where a key equals an unused slot's");

  for (uint32_t slot : {0u, 2u}) {
    for (uint32_t bit = 0; bit < 32; ++bit) {
      Key changed[kSlots];
      std::copy(keys, keys + kSlots, changed);
      changed[slot].key ^= uint32_t(1) << bit;
      Check(!memo.Matches(&owner_a, 7, used, changed, lengths, 2), "any key bit of a used slot misses");
    }
  }
  Key unused_changed[kSlots];
  std::copy(keys, keys + kSlots, unused_changed);
  unused_changed[1].key = 1;
  unused_changed[4].key = 2;
  Check(memo.Matches(&owner_a, 7, used, unused_changed, lengths, 2), "unused slot contents are ignored");

  uint32_t shorter[kSlots] = {0, 399, 7, 7, 7};
  Check(memo.Matches(&owner_a, 7, used, keys, shorter, 2), "no longer ranges are already owned");
  uint32_t longer0[kSlots] = {601, 400};
  uint32_t longer1[kSlots] = {600, 401};
  Check(!memo.Matches(&owner_a, 7, used, keys, longer0, 2), "a longer first range misses");
  Check(!memo.Matches(&owner_a, 7, used, keys, longer1, 2), "a longer second range misses");
  uint32_t swapped[kSlots] = {400, 600};
  Check(!memo.Matches(&owner_a, 7, used, keys, swapped, 2), "lengths compare by sorted position");

  memo.Publish(&owner_b, 9, 0b1, keys, lengths, 1, targets);
  Check(!memo.Matches(&owner_a, 7, used, keys, lengths, 2), "publication replaces the old claim");
  Check(memo.Matches(&owner_b, 9, 0b1, keys, lengths, 1), "the new claim matches");
}

// Tile-level model of the cache: EDRAM owners, render targets created on first
// use, and ChangeOwnership with its wrap to the next addressing period. As in
// the vendored cache, every call that can change the owners or delete a target
// bumps the generation.
struct Model {
  std::array<uint32_t, kTiles> owner{};
  std::map<uint32_t, std::unique_ptr<Target>> targets;
  uint64_t generation = 0;
  Memo memo;
  uint64_t hits = 0;

  Target* GetOrCreate(uint32_t key) {
    auto& target = targets[key];
    if (!target) target = std::make_unique<Target>(Target{key});
    return target.get();
  }
  // Appends (tile, source) for each copying transfer, like ChangeOwnership.
  void Claim(uint32_t key, uint32_t length, std::vector<std::pair<uint32_t, uint32_t>>* transfers) {
    ++generation;
    const uint32_t start = key & (kTiles - 1);
    const uint32_t end = start + length;
    auto extent = [&](uint32_t first, uint32_t last) {
      for (uint32_t tile = first; tile < last; ++tile) {
        if (owner[tile] == key) continue;
        if (owner[tile] && transfers) transfers->emplace_back(tile, owner[tile]);
        owner[tile] = key;
      }
    };
    extent(start, std::min(end, kTiles));
    if (end > kTiles) extent(0, std::min(end & (kTiles - 1), start));
  }
  // ClearCache: deletes every target that owns no tile and keeps the owners.
  void ClearCache() {
    ++generation;
    std::vector<uint32_t> owned(owner.begin(), owner.end());
    std::sort(owned.begin(), owned.end());
    for (auto it = targets.begin(); it != targets.end();) {
      if (std::binary_search(owned.begin(), owned.end(), it->first)) {
        ++it;
      } else {
        it = targets.erase(it);
      }
    }
  }
  // The full-EDRAM interlock barrier empties the owners; DestroyAllRenderTargets
  // also deletes every target.
  void Reset(bool destroy_targets) {
    ++generation;
    owner.fill(0);
    if (destroy_targets) targets.clear();
  }
  // The claim part of Update: slots sorted by base, then targets, then claims.
  void Update(bool use_memo, uint32_t used, const Key* keys, const uint32_t* lengths_sorted,
              const uint32_t* slots_sorted, uint32_t count, Target** rts,
              std::vector<std::pair<uint32_t, uint32_t>>* transfers) {
    for (uint32_t i = 0; i < kSlots; ++i) transfers[i].clear();
    const bool unchanged =
        use_memo && memo.Matches(this, generation, used, keys, lengths_sorted, count);
    if (unchanged) {
      ++hits;
      memo.CopyTargets(rts);
      return;
    }
    for (uint32_t i = 0; i < count; ++i) rts[slots_sorted[i]] = GetOrCreate(keys[slots_sorted[i]].key);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t slot = slots_sorted[i];
      Claim(keys[slot].key, lengths_sorted[i], &transfers[slot]);
    }
    if (use_memo) memo.Publish(this, generation, used, keys, lengths_sorted, count, rts);
  }
};

struct Binding {
  uint32_t used = 0;
  Key keys[kSlots];
  std::vector<std::pair<uint32_t, uint32_t>> sorted;  // (base, slot)
  std::vector<uint32_t> max_lengths;                  // Update's non-overlap clamp
  std::vector<uint32_t> usual_lengths;                // what most of its draws claim, often 0
};

Binding RandomBinding(std::mt19937& rng) {
  Binding b;
  std::uniform_int_distribution<uint32_t> tile(0, kTiles - 1);
  const uint32_t pitch = 1 + rng() % 4;
  while (!b.used) b.used = rng() & 0b11111;
  std::vector<uint32_t> bases;
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    if (!((b.used >> slot) & 1)) continue;
    uint32_t base;
    do base = tile(rng); while (std::find(bases.begin(), bases.end(), base) != bases.end());
    bases.push_back(base);
    b.keys[slot].key = MakeKey(base, pitch, rng() % 3, slot == 0, rng() % 16);
    b.sorted.emplace_back(base, slot);
  }
  std::sort(b.sorted.begin(), b.sorted.end());
  for (size_t i = 0; i < b.sorted.size(); ++i) {
    const uint32_t next = i + 1 < b.sorted.size() ? b.sorted[i + 1].first : kTiles + b.sorted[0].first;
    b.max_lengths.push_back(next - b.sorted[i].first);
    // EstimateMaxY can give 0: the target is then bound but owns nothing, so ClearCache deletes it.
    b.usual_lengths.push_back(rng() % 3 == 0 ? 0 : rng() % (b.max_lengths.back() + 1));
  }
  return b;
}

void LockstepAgainstModel() {
  std::mt19937 rng(0x5EED1234u);
  uint64_t updates = 0, hit_updates = 0, transfer_updates = 0, cache_clears = 0, cleared_targets = 0;
  for (int run = 0; run < 40; ++run) {
    Model slow, fast;
    std::vector<Binding> pool;
    for (int i = 0; i < 6; ++i) pool.push_back(RandomBinding(rng));
    size_t current = 0;
    for (int step = 0; step < 3000; ++step) {
      const uint32_t event = rng() % 100;
      if (event < 3) {
        // A resolve clear: the target is created, then any range claimed (often none),
        // with a separate transfer list.
        const Binding& other = pool[rng() % pool.size()];
        const uint32_t key =
            event == 0 ? MakeKey(rng() % kTiles, 1, 0, 0, 0) : other.keys[other.sorted[0].second].key;
        const uint32_t length = rng() % 4 == 0 ? 0 : rng() % (kTiles + 1);
        slow.GetOrCreate(key);
        fast.GetOrCreate(key);
        slow.Claim(key, length, nullptr);
        fast.Claim(key, length, nullptr);
        continue;
      }
      if (event < 5) {
        const size_t targets_before = fast.targets.size();
        slow.ClearCache();
        fast.ClearCache();
        ++cache_clears;
        cleared_targets += targets_before - fast.targets.size();
        continue;
      }
      if (event < 6) {
        const bool destroy_targets = rng() % 4 == 0;
        slow.Reset(destroy_targets);
        fast.Reset(destroy_targets);
        continue;
      }
      // Draws mostly repeat the current binding and its usual extent, like a pass of a frame.
      if (event < 12) current = rng() % pool.size();
      const Binding& b = pool[current];
      const uint32_t count = uint32_t(b.sorted.size());
      uint32_t lengths[kSlots] = {}, slots[kSlots] = {};
      const uint32_t extent = rng() % 8;
      for (uint32_t i = 0; i < count; ++i) {
        slots[i] = b.sorted[i].second;
        const uint32_t max_length = b.max_lengths[i];
        lengths[i] = extent == 0   ? max_length
                     : extent == 1 ? rng() % (max_length + 1)
                                   : b.usual_lengths[i];
      }
      Target* slow_rts[kSlots] = {};
      Target* fast_rts[kSlots] = {};
      std::vector<std::pair<uint32_t, uint32_t>> slow_transfers[kSlots], fast_transfers[kSlots];
      const uint64_t hits_before = fast.hits;
      slow.Update(false, b.used, b.keys, lengths, slots, count, slow_rts, slow_transfers);
      fast.Update(true, b.used, b.keys, lengths, slots, count, fast_rts, fast_transfers);
      ++updates;
      hit_updates += fast.hits != hits_before;
      bool any_transfer = false;
      for (uint32_t slot = 0; slot < kSlots; ++slot) {
        if (!((b.used >> slot) & 1)) continue;
        // Liveness first: a deleted target must be caught before anything reads it.
        const auto live = fast.targets.find(b.keys[slot].key);
        Check(live != fast.targets.end() && live->second.get() == fast_rts[slot],
              "the fast path binds the live target of each key");
        Check(slow_rts[slot] && slow_rts[slot]->key == b.keys[slot].key,
              "the slow path binds the target of each key");
        Check(slow_transfers[slot] == fast_transfers[slot], "the fast path skips no transfer");
        any_transfer |= !slow_transfers[slot].empty();
      }
      transfer_updates += any_transfer;
      if (fast.hits != hits_before) Check(!any_transfer, "a hit is only taken when nothing transfers");
      Check(slow.owner == fast.owner, "the ownership map is identical");
      Check(std::equal(slow.targets.begin(), slow.targets.end(), fast.targets.begin(), fast.targets.end(),
                       [](const auto& a, const auto& b) { return a.first == b.first; }),
            "the render target set is identical");
    }
  }
  Check(hit_updates > updates / 4, "the controls take the fast path often");
  Check(transfer_updates > updates / 20, "the controls also exercise real transfers");
  Check(cleared_targets > cache_clears, "cache clears often delete targets that own nothing");
  std::cout << updates << " model updates, " << hit_updates << " fast-path hits, " << transfer_updates
            << " with transfers, " << cleared_targets << " targets deleted by " << cache_clears
            << " cache clears\n";
}
}  // namespace

int main() {
  DirectControls();
  LockstepAgainstModel();
  std::cout << "PASS " << checks << " render target claim memo checks\n";
}
