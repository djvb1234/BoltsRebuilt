// CPU-only tests of the original candidate/consensus cache policy. Resolver
// fixtures are deliberately independent of SDK sampler math: these tests prove
// exact-input reuse and candidate preservation, not a second sampler algorithm.
#include "../src/gpu/native/native_texture_binding_cache.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
struct Texture {
  uint32_t bindless_descriptor_index;
  uint32_t fetch_constant;
  uint32_t dimension;
  bool is_signed;
};
struct Sampler {
  uint32_t bindless_descriptor_index;
  uint32_t fetch_constant;
  uint32_t mag_filter;
  uint32_t min_filter;
  uint32_t mip_filter;
  uint32_t aniso_filter;
};
using Cache = nb::gpu::NativeTextureBindingCache<Texture, Sampler>;
size_t checks = 0;
void Check(bool value, const char* label) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL: " << label << '\n';
    std::exit(1);
  }
}
bool Same(const Texture& a, const Texture& b) {
  return a.bindless_descriptor_index == b.bindless_descriptor_index &&
         a.fetch_constant == b.fetch_constant && a.dimension == b.dimension &&
         a.is_signed == b.is_signed;
}
bool Same(const Sampler& a, const Sampler& b) {
  return a.bindless_descriptor_index == b.bindless_descriptor_index &&
         a.fetch_constant == b.fetch_constant && a.mag_filter == b.mag_filter &&
         a.min_filter == b.min_filter && a.mip_filter == b.mip_filter &&
         a.aniso_filter == b.aniso_filter;
}

void CandidateControls() {
  auto cache = std::make_unique<Cache>();
  // Interleaved fetches include unsigned/signed views, another dimension, and
  // exact duplicates. None may be discarded or reordered within a fetch.
  std::vector<Texture> textures = {
      {10, 3, 2, false}, {11, 0, 2, false}, {12, 3, 2, true},
      {13, 3, 4, false}, {12, 3, 2, true}, {14, 31, 3, false}};
  std::vector<Sampler> samplers = {
      {20, 3, 0, 1, 2, 3}, {21, 0, 3, 2, 1, 0},
      {22, 3, 1, 0, 2, 3}, {22, 3, 1, 0, 2, 3}, {23, 31, 1, 2, 0, 4}};
  auto* plan = cache->Prepare(0x1000, 1, textures, samplers);
  Check(plan != nullptr, "valid immutable plan");
  for (uint32_t fetch = 0; fetch < 32; ++fetch) {
    size_t count = 0;
    for (const auto& expected : textures) {
      if (expected.fetch_constant != fetch) continue;
      Check(count < plan->Textures(fetch).size(), "texture candidate present");
      Check(Same(plan->Textures(fetch)[count++], expected), "texture order/sign/dimension preserved");
    }
    Check(count == plan->Textures(fetch).size(), "no extra texture candidates");
    count = 0;
    for (const auto& expected : samplers) {
      if (expected.fetch_constant != fetch) continue;
      Check(count < plan->Samplers(fetch).size(), "sampler candidate present");
      Check(Same(plan->Samplers(fetch)[count++], expected), "all instruction override fields preserved");
    }
    Check(count == plan->Samplers(fetch).size(), "no extra sampler candidates");
  }
  Check(plan->Textures(32).empty() && plan->Samplers(UINT32_MAX).empty(), "invalid fetch spans empty");
  Check(cache->Prepare(0x1000, 1, textures, samplers) == plan, "same lifetime plan hit");
  Check(cache->stats().plan_hits == 1, "plan hit counted");
  const Texture original = textures[0];
  textures[0].dimension = 1;
  Check(Same(plan->Textures(3)[0], original), "plan owns immutable candidate snapshot");
  plan = cache->Prepare(0x1000, 2, textures, samplers);
  Check(Same(plan->Textures(3)[0], textures[0]), "new lifetime same-address ABA rebuilds");
  Check(cache->stats().resets == 2, "epoch reset counted");

  auto too_many_textures = std::vector<Texture>(Cache::kMaxTextures + 1, original);
  auto too_many_samplers = std::vector<Sampler>(Cache::kMaxSamplers + 1, samplers[0]);
  Check(!cache->Prepare(0x2000, 2, too_many_textures, samplers), "oversize texture fallback");
  Check(!cache->Prepare(0x2000, 2, textures, too_many_samplers), "oversize sampler fallback");
  auto bad_textures = textures;
  bad_textures[0].fetch_constant = 32;
  auto bad_samplers = samplers;
  bad_samplers[0].fetch_constant = UINT32_MAX;
  Check(!cache->Prepare(0x2000, 2, bad_textures, samplers), "unknown texture fetch fallback");
  Check(!cache->Prepare(0x2000, 2, textures, bad_samplers), "unknown sampler fetch fallback");
  Check(!cache->Prepare(0, 2, textures, samplers), "null shader fallback");
  too_many_textures.pop_back();
  too_many_samplers.pop_back();
  plan = cache->Prepare(0x2000, 2, too_many_textures, too_many_samplers);
  Check(plan && plan->Textures(3).size() == 255 && plan->Samplers(3).size() == 127,
        "exact SDK maxima accepted without truncation");
}

void ExactKeyAndConsensusControls() {
  auto cache = std::make_unique<Cache>();
  const std::array<Sampler, 2> candidates = {{{0, 3, 0, 1, 2, 3}, {1, 3, 1, 0, 3, 2}}};
  auto* plan = cache->Prepare(0x1000, 1, {}, candidates);
  std::array<uint32_t, 6> words = {0x12345678u, 0x87654321u, 0, UINT32_MAX, 55, 66};
  uint32_t calls = 0;
  uint32_t expected_value = 0x2AAAAAu;
  auto same_result = [&](const Sampler&) { ++calls; return expected_value; };
  auto result = cache->ResolveSampler(*plan, 3, words, -1, same_result);
  Check(result.have_sampler && !result.conflict && result.value == expected_value, "different overrides may agree");
  Check(calls == 2, "initial consensus resolves all candidates");
  result = cache->ResolveSampler(*plan, 3, words, -1, same_result);
  Check(calls == 2 && result.value == expected_value, "exact hit invokes no SDK callback");

  // Every bit of all six DWORDs is part of the key, even if a particular SDK
  // sampler currently ignores that field. The resolver returns a new literal
  // each time so stale reuse cannot accidentally equal the expected result.
  for (size_t word = 0; word < words.size(); ++word) {
    for (uint32_t bit = 0; bit < 32; ++bit) {
      words[word] ^= uint32_t(1) << bit;
      ++expected_value;
      const auto before = calls;
      result = cache->ResolveSampler(*plan, 3, words, -1, same_result);
      Check(calls == before + 2 && result.value == expected_value && !result.conflict,
            "each fetch bit invalidates consensus");
      result = cache->ResolveSampler(*plan, 3, words, -1, same_result);
      Check(calls == before + 2 && result.value == expected_value, "mutated fetch second use hits");
      words[word] ^= uint32_t(1) << bit;
      ++expected_value;
      const auto restored = cache->ResolveSampler(*plan, 3, words, -1, same_result);
      Check(calls == before + 4 && restored.value == expected_value, "restored fetch differs from most recent input");
    }
  }
  for (const int32_t override_value : {-2, 0, 1, 2, 3, 4, 5, 6, INT32_MAX, INT32_MIN, -1}) {
    const auto before = calls;
    ++expected_value;
    result = cache->ResolveSampler(*plan, 3, words, override_value, same_result);
    Check(calls == before + 2 && result.value == expected_value, "full anisotropy override key");
  }
  // The two instruction candidates coincide for one live input and conflict for
  // another. A memo cannot permanently reduce this plan to the first candidate.
  words[0] = 0;
  auto diverging = [&](const Sampler& binding) {
    ++calls;
    return 17u + (words[0] ? binding.mag_filter : 0u);
  };
  result = cache->ResolveSampler(*plan, 3, words, -1, diverging);
  Check(result.have_sampler && !result.conflict && result.value == 17, "candidate coincidence");
  words[0] = 1;
  result = cache->ResolveSampler(*plan, 3, words, -1, diverging);
  Check(result.have_sampler && result.conflict, "later candidate divergence refuses");
  const auto before_conflict = calls;
  result = cache->ResolveSampler(*plan, 3, words, -1, diverging);
  Check(result.conflict && calls == before_conflict, "same conflicting input remains refusal on hit");
  words[0] = 0;
  result = cache->ResolveSampler(*plan, 3, words, -1, diverging);
  Check(result.have_sampler && !result.conflict, "restored effective agreement accepts");
  const auto before_empty = calls;
  result = cache->ResolveSampler(*plan, 2, words, -1, diverging);
  Check(!result.have_sampler && !result.conflict && calls == before_empty, "missing sampler remains missing");
  result = cache->ResolveSampler(*plan, 2, words, -1, diverging);
  Check(!result.have_sampler && calls == before_empty, "missing sampler memo remains refusal");
  Check(!cache->ResolveSampler(*plan, 32, words, -1, diverging).have_sampler, "unknown fetch sampler fallback");

  const std::array<Sampler, 1> replacement = {{{0, 3, 19, 18, 17, 16}}};
  plan = cache->Prepare(0x1000, 2, {}, replacement);
  const auto before_reset = calls;
  result = cache->ResolveSampler(*plan, 3, words, -1, [&](const Sampler& binding) {
    ++calls;
    Check(Same(binding, replacement[0]), "replacement instruction tuple after epoch");
    return 23u;
  });
  Check(calls == before_reset + 1 && result.value == 23, "epoch invalidates both plan and sampler memo");
}

void CollisionAndLiveDescriptorControls() {
  auto cache = std::make_unique<Cache>();
  const std::array<uint32_t, 6> words{};
  // More identities than capacity forces collisions without knowing the slot
  // formula. The callback oracle uses each actual owner's independent literal.
  for (uint32_t round = 0; round < 4; ++round) {
    for (uint32_t n = 0; n < 129; ++n) {
      const uint32_t owner = (round & 1u) ? 128u - n : n;
      const std::array<Texture, 2> textures = {{{owner * 2, 0, 2, false}, {owner * 2 + 1, 0, 2, true}}};
      const std::array<Sampler, 1> samplers = {{{owner, 0, owner + 7, 1, 2, 3}}};
      auto* plan = cache->Prepare(uintptr_t(0x1000) + uintptr_t(owner) * 256, 1, textures, samplers);
      Check(plan && Same(plan->Textures(0)[0], textures[0]) && Same(plan->Samplers(0)[0], samplers[0]),
            "collision cannot reuse another shader's candidate plan");
      const auto result = cache->ResolveSampler(*plan, 0, words, 0, [&](const Sampler& binding) {
        Check(Same(binding, samplers[0]), "collision resolution uses actual instruction candidate");
        return owner + 1234u;
      });
      Check(result.have_sampler && !result.conflict && result.value == owner + 1234,
            "collision cannot reuse another shader's sampler consensus");
    }
  }
  Check(cache->stats().plan_misses >= 129, "bounded plan table necessarily evicted");
  // The policy only returns immutable candidate records. Resolve a simulated
  // live descriptor after every lookup to exercise eviction/recreation, typed
  // null, and unsigned/signed choices while the fetch and plan remain unchanged.
  const std::array<Texture, 2> textures = {{{10, 3, 2, false}, {11, 3, 2, true}}};
  auto* plan = cache->Prepare(0x1000, 2, textures, {});
  const std::array<uint32_t, 5> live_views = {100, 501, 0, UINT32_MAX, 207};
  for (uint32_t view : live_views) {
    for (bool is_signed : {false, true}) {
      uint32_t observed = UINT32_MAX;
      for (const auto& candidate : plan->Textures(3)) {
        if (candidate.dimension == 2 && candidate.is_signed == is_signed) observed = view;
      }
      Check(observed == view, "immutable plan leaves current SRV and typed-null resolution live");
    }
  }
}
}  // namespace

int main() {
  CandidateControls();
  ExactKeyAndConsensusControls();
  CollisionAndLiveDescriptorControls();
  std::cout << "PASS: " << checks << " native texture binding cache policy checks\n";
}
