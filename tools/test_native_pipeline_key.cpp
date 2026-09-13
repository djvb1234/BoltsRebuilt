// Original portable controls for the production native PSO key normalizer.
// No SDK, Windows, assets, compiler DLL or GPU is required. The independent
// projection below expands original guest inputs into every variable field of
// the unchanged GetPipeline descriptor; it does not reuse normalization masks.
#include "../src/gpu/native/native_pipeline_key.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
uint64_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
struct Key {
  uint32_t words[20]{};
  bool operator==(const Key&) const = default;
  bool operator<(const Key& rhs) const {
    return std::lexicographical_compare(std::begin(words), std::end(words),
                                        std::begin(rhs.words), std::end(rhs.words));
  }
};
Key Normalize(Key in) {
  nb::gpu::NormalizeNativePipelineKey(in.words);
  return in;
}
uint32_t Bits(uint32_t value, unsigned first, unsigned count) {
  return (value >> first) & ((uint32_t(1) << count) - 1);
}
// D3D12_BLEND values emitted by the current MapBlendFactor. These explicit
// tables also retain its alias/default behavior instead of treating guest
// factor numbers as descriptor values.
uint32_t BlendFactor(uint32_t input, bool alpha) {
  switch (input) {
    case 0: return 1;  // ZERO
    case 1: return 2;  // ONE
    case 4: return alpha ? 5 : 3;   // SRC_ALPHA / SRC_COLOR
    case 5: return alpha ? 6 : 4;
    case 6: return 5;
    case 7: return 6;
    case 8: return alpha ? 7 : 9;   // DEST_ALPHA / DEST_COLOR
    case 9: return alpha ? 8 : 10;
    case 10: return 7;
    case 11: return 8;
    case 12: case 14: return 14;   // BLEND_FACTOR
    case 13: case 15: return 15;
    case 16: return 11;            // SRC_ALPHA_SAT
    default: return 2;
  }
}
uint32_t BlendOp(uint32_t input) {
  switch (input) {
    case 1: return 2;  // SUBTRACT
    case 2: return 4;  // MIN
    case 3: return 5;  // MAX
    case 4: return 3;  // REV_SUBTRACT
    default: return 1;
  }
}
using Projection = std::vector<uint32_t>;
Projection Descriptor(const Key& in) {
  const auto& w = in.words;
  Projection p;
  p.reserve(62);
  const auto add = [&](uint32_t value) { p.push_back(value); };
  for (unsigned i = 0; i < 6; ++i) add(w[i]);  // RTV formats, DSV, sample count
  add(w[17]);                                 // sample mask is never elided
  add(Bits(w[16], 2, 1));                     // retained owner-local root variant
  add(Bits(w[16], 1, 1) && w[5] > 1);          // actual alpha-to-coverage BOOL
  for (unsigned i = 0; i < 4; ++i) {
    if (!w[i]) {
      // Exact fixed state explicitly assigned for an UNKNOWN RTV.
      for (uint32_t value : {0u, 2u, 1u, 1u, 2u, 1u, 1u, 0u}) add(value);
      continue;
    }
    const auto b = w[6 + i];
    const auto cs = Bits(b, 0, 5), co = Bits(b, 5, 3), cd = Bits(b, 8, 5);
    const auto as = Bits(b, 16, 5), ao = Bits(b, 21, 3), ad = Bits(b, 24, 5);
    add(!(cs == 1 && co == 0 && cd == 0 && as == 1 && ao == 0 && ad == 0));
    add(BlendFactor(cs, false)); add(BlendFactor(cd, false)); add(BlendOp(co));
    add(BlendFactor(as, true)); add(BlendFactor(ad, true)); add(BlendOp(ao));
    add(Bits(w[12], 4 * i, 4));
  }
  const auto front = Bits(w[11], 0, 1), back = Bits(w[11], 1, 1);
  add(front ? 2 : back ? 3 : 1);  // D3D12_CULL_MODE
  add(!Bits(w[11], 2, 1));
  add(w[14]); add(w[15]); add(Bits(w[16], 0, 1));
  const auto d = w[10];
  add(w[4] && Bits(d, 1, 1));
  add(Bits(d, 2, 1));         // retained even if DepthEnable is false / no DSV
  add(Bits(d, 4, 3) + 1);    // likewise retained DepthFunc
  add(w[4] && Bits(d, 0, 1));
  std::array<uint32_t, 10> stencil{};
  if (Bits(d, 0, 1)) {
    // Population uses ORIGINAL stencil_enable, before the absent-DSV override.
    stencil[0] = Bits(w[13], 8, 8);
    stencil[1] = Bits(w[13], 16, 8);
    stencil[2] = Bits(d, 11, 3) + 1;  // FailOp
    stencil[3] = Bits(d, 17, 3) + 1;  // DepthFailOp
    stencil[4] = Bits(d, 14, 3) + 1;  // PassOp
    stencil[5] = Bits(d, 8, 3) + 1;   // Func
    if (Bits(d, 7, 1)) {
      stencil[6] = Bits(d, 23, 3) + 1;
      stencil[7] = Bits(d, 29, 3) + 1;
      stencil[8] = Bits(d, 26, 3) + 1;
      stencil[9] = Bits(d, 20, 3) + 1;
    } else {
      for (unsigned i = 0; i < 4; ++i) stencil[6 + i] = stencil[2 + i];
    }
  }
  for (auto value : stencil) add(value);
  uint32_t targets = 0;
  for (unsigned i = 0; i < 4; ++i) if (w[i]) targets = i + 1;
  add(targets);
  add(front && back);  // original pre-creation cull refusal also has to match
  Check(p.size() == 62, "complete variable descriptor projection");
  return p;
}
Key Literal() {
  Key k;
  k.words[0] = 28;
  k.words[4] = 20;
  k.words[5] = 2;
  for (unsigned i = 6; i < 10; ++i) k.words[i] = 0x00010001u;
  k.words[10] = 0x00708767u;
  k.words[11] = 0x00018002u;
  k.words[12] = 15;
  k.words[13] = 0x00A6FF22u;
  k.words[16] = 5;
  k.words[17] = 0xFFFFFFFFu;
  return k;
}
void Equivalent(const Key& a, const Key& b, const char* message) {
  Check(Normalize(a) == Normalize(b), message);
  Check(Descriptor(a) == Descriptor(b), "equal normalized keys preserve original descriptor");
}
void Different(const Key& a, const Key& b, const char* message) {
  Check(!(Normalize(a) == Normalize(b)), message);
  Check(Descriptor(a) != Descriptor(b), "descriptor-changing control remains distinct");
}
void LiteralControls() {
  auto a = Literal();
  auto b = a;
  b.words[11] ^= 0xFFFFFFF8u;
  Equivalent(a, b, "setup fields outside cull/front-face are absent from desc");
  b = a; b.words[13] ^= 0xFF0000FFu;
  Equivalent(a, b, "dynamic stencil reference and padding do not select PSO");
  b = a; b.words[6] ^= 0xE000E000u;
  Equivalent(a, b, "blend padding is not read by identity or mapping");
  b = a; b.words[7] = 0xFFFFFFFFu; b.words[12] ^= 0xFFFFFFF0u;
  Equivalent(a, b, "UNKNOWN RTV blend and inactive color masks discarded");
  b = a; b.words[10] ^= 8;
  Equivalent(a, b, "depth padding never reaches descriptor");
  a.words[5] = 1; b = a; b.words[16] ^= 2;
  Equivalent(a, b, "single-sample alpha-to-coverage is explicitly false");
  a.words[5] = 2; b = a; b.words[16] ^= 2;
  Different(a, b, "MSAA alpha-to-coverage is retained");
  a = Literal(); a.words[10] &= ~1u; b = a;
  b.words[10] ^= 0xFFFFFF80u; b.words[13] = 0;
  Equivalent(a, b, "disabled stencil ignores both faces and masks");
  a = Literal(); a.words[10] &= ~0x80u; b = a; b.words[10] ^= 0xFFF00000u;
  Equivalent(a, b, "disabled backface copies exact front-face descriptor");
  a = Literal(); a.words[10] |= 0x80u; b = a; b.words[10] ^= 0x00100000u;
  Different(a, b, "enabled backface function remains distinct");
  a = Literal(); a.words[4] = 0; b = a; b.words[10] ^= 2;
  Equivalent(a, b, "UNKNOWN DSV forces DepthEnable false");
  for (uint32_t bit : {1u, 4u, 16u, 0x800u}) {
    b = a; b.words[10] ^= bit;
    Different(a, b, "UNKNOWN DSV retains stencil population, depth write/func and stencil ops");
  }
  b = a; b.words[13] ^= 0x100u;
  Different(a, b, "UNKNOWN DSV still retains populated stencil masks");
  a = Literal(); a.words[10] &= ~2u;
  for (uint32_t bit : {4u, 16u}) {
    b = a; b.words[10] ^= bit;
    Different(a, b, "disabled depth still retains write mask and function");
  }
  a = Literal();
  for (unsigned word : {0u, 4u, 5u, 14u, 15u, 17u}) {
    b = a; b.words[word] ^= 1;
    Different(a, b, "formats, samples, raw bias bits and sample mask are retained");
  }
  for (uint32_t bit : {1u, 4u}) {
    b = a; b.words[16] ^= bit;
    Different(a, b, "depth clip and retained root signature variant are distinct");
  }
  for (uint32_t face = 0; face < 8; ++face) {
    a = Literal(); a.words[11] = face | 0xFFFFFFF8u;
    const auto n = Normalize(a);
    Check(n.words[11] == face, "all cull/front-face bits retained including both-cull refusal");
    Check(Descriptor(a) == Descriptor(n), "both-cull refusal cannot become a usable draw");
  }
  for (uint32_t bits : {0u, 0x80000000u, 0x7FC12345u, 0x7FA54321u,
                        0xFFC56789u, 0x7F800000u, 0xFF800000u, 1u}) {
    a = Literal(); a.words[15] = bits;
    Check(Normalize(a).words[15] == bits, "signed zero, NaN payloads, infinity and subnormal bits retained");
  }
  a = Literal(); a.words[18] = 0x12345678; a.words[19] = 0xABCDEF01;
  const auto n = Normalize(a);
  Check(n.words[18] == a.words[18] && n.words[19] == a.words[19], "future reserved key words left intact");
}
uint32_t Random(uint64_t& state) {
  state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
  return uint32_t((state * 0x2545F4914F6CDD1Dull) >> 32);
}
Key RandomKey(uint64_t& random) {
  Key key;
  for (auto& word : key.words) word = Random(random);
  for (unsigned i = 0; i < 4; ++i) key.words[i] = Random(random) % 2 ? 0 : 28 + Random(random) % 4;
  key.words[4] = Random(random) % 2 ? 0 : 20;
  const uint32_t samples[] = {0, 1, 2, 4, 8};
  key.words[5] = samples[Random(random) % std::size(samples)];
  return key;
}
void ProjectionControls() {
  uint64_t random = 0xDC12123456ABCDEFu;
  for (unsigned i = 0; i < 50000; ++i) {
    const auto original = RandomKey(random);
    auto normalized = original;
    const bool changed = nb::gpu::NormalizeNativePipelineKey(normalized.words);
    Check(changed == !(normalized == original), "changed-key counter truthfully describes full key");
    Check(Descriptor(normalized) == Descriptor(original), "random original descriptor projection preserved");
    auto again = normalized;
    Check(!nb::gpu::NormalizeNativePipelineKey(again.words) && again == normalized, "normalization is idempotent");
    auto mutation = original;
    mutation.words[Random(random) % 20] ^= uint32_t(1) << (Random(random) % 32);
    if (Normalize(original) == Normalize(mutation)) {
      Check(Descriptor(original) == Descriptor(mutation), "equal randomized normalized keys imply exact descriptor");
    }
  }
  // Exhaust every single input bit in independent depth/stencil/RT scenarios.
  for (unsigned variant = 0; variant < 16; ++variant) {
    auto base = Literal();
    base.words[4] = variant & 1 ? 20 : 0;
    base.words[10] = (variant & 2 ? 1u : 0u) | (variant & 4 ? 0x80u : 0u) | 0x13579B76u;
    base.words[0] = variant & 8 ? 28 : 0;
    for (unsigned word = 0; word < 20; ++word) for (unsigned bit = 0; bit < 32; ++bit) {
      auto changed = base; changed.words[word] ^= uint32_t(1) << bit;
      Check(Descriptor(changed) == Descriptor(Normalize(changed)), "exhaustive single-bit projection preservation");
      if (Normalize(base) == Normalize(changed)) {
        Check(Descriptor(base) == Descriptor(changed), "discarded single input bit cannot change descriptor");
      }
    }
  }
}
void ToggleAndOwnerControls() {
  struct Entry { Projection projection; unsigned state; uint64_t identity; };
  std::array<std::map<Key, Entry>, 2> owners;
  uint64_t random = 0xED123456ABCDEF01u, next_identity = 1;
  std::array<Key, 64> inputs;
  for (auto& key : inputs) key = RandomKey(random);
  uint64_t memo_owner = 2, memo_identity = 0;
  Key memo_key;
  Projection memo_projection;
  for (unsigned event = 0; event < 16000; ++event) {
    const unsigned owner = Random(random) % 2;
    const bool enabled = Random(random) % 2 != 0;
    auto raw = inputs[Random(random) % inputs.size()];
    // Exercise raw and canonical representations of the same original desc
    // under both modes, while each map retains its shader/root-signature owner.
    if (Random(random) % 2) raw = Normalize(raw);
    if (Random(random) % 3 == 0) raw.words[11] ^= Random(random) & 0xFFFFFFF8u;
    const Key lookup = enabled ? Normalize(raw) : raw;
    const auto projection = Descriptor(raw);
    auto& map = owners[owner];
    // Reap before memo lookup, as production does. Pending entries become
    // either ready or known bad; no memo may publish their null/pending value.
    for (auto& [key, entry] : map) if (entry.state == 0) {
      // An ordinary driver-creation failure is cached as null too. Its exact
      // descriptor must stay attached to that result across mode switches.
      entry.state = entry.projection.back() || entry.identity % 17 == 0 ? 2u : 1u;
      (void)key;
    }
    if (memo_owner == owner && memo_identity && memo_key == lookup) {
      Check(memo_projection == projection, "mode toggles preserve ready memo descriptor/owner");
      continue;
    }
    auto it = map.find(lookup);
    if (it == map.end()) {
      const unsigned state = projection.back() ? 2u : 0u;
      map.emplace(lookup, Entry{projection, state, next_identity++});
      continue;
    }
    Check(it->second.projection == projection, "raw/canonical map collision preserves exact descriptor");
    if (it->second.state == 1) {
      Check(!projection.back(), "a both-cull refusal cannot retrieve a ready pipeline");
      memo_owner = owner; memo_key = lookup; memo_projection = projection;
      memo_identity = it->second.identity;
    } else {
      Check(it->second.state == 2, "known-bad cull/creation result remains a refusal");
    }
    if (event % 997 == 0) {
      owners[owner].clear();  // separate pass lifetime; invalidate its memo
      if (memo_owner == owner) memo_identity = 0;
    }
  }
}
}  // namespace

int main() {
  try {
    LiteralControls();
    ProjectionControls();
    ToggleAndOwnerControls();
    std::cout << "PASS native pipeline key: " << checks
              << " checks, 50000 random descriptors, 10240 single-bit cases, 16000 owner/toggle events\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native pipeline key after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}
