// Original portable controls for the production constant-packing/reuse policy.
// Build/run is coordinated by the root task; no SDK, assets or GPU required.
#include "../src/gpu/native/native_constant_reuse.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using Packet = nb::gpu::NativePixelConstantPacket;
using Cache = nb::gpu::NativePixelConstantReuse;
struct Run { uint16_t first, count; };
struct Input {
  std::array<uint32_t, 8> bools{};
  std::array<uint32_t, 20> swizzles{}, samplers{};
  std::array<uint32_t, 1024> floats{};
};
uint64_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
Input MakeInput() {
  Input in;
  for (size_t i = 0; i < in.bools.size(); ++i) in.bools[i] = 0xA0123400u + uint32_t(i);
  for (size_t i = 0; i < 20; ++i) {
    in.swizzles[i] = 0xFFF12300u + uint32_t(i);
    in.samplers[i] = uint32_t(i * 0x10001u);
  }
  for (size_t i = 0; i < in.floats.size(); ++i) in.floats[i] = 0x12345600u + uint32_t(i);
  // Preserve signaling/quiet NaN payloads, infinities and both signs of zero.
  const uint32_t special[] = {0x7FC12345u, 0x7FA54321u, 0xFFC45678u, 0x80000000u,
                              0u, 0x7F800000u, 0xFF800000u, 0x00000001u};
  std::copy(std::begin(special), std::end(special), in.floats.begin());
  return in;
}
// Independent byte-by-byte oracle. It never uses memcpy or the production run
// copy helper. Both this app and its Windows GPU ABI are little endian.
std::vector<uint8_t> Oracle(const Input& in, const std::vector<Run>& runs, uint32_t packed,
                            bool null_source = false, bool null_bools = false) {
  std::vector<uint8_t> out;
  const auto append = [&](uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) out.push_back(uint8_t(word >> (byte * 8)));
  };
  for (uint32_t word : in.bools) append(null_bools ? 0u : word);
  for (uint32_t word : in.swizzles) append(word);
  for (uint32_t word : in.samplers) append(word);
  const uint32_t count = packed ? packed : 256;
  if (null_source) for (uint32_t i = 0; i < count * 4; ++i) append(0);
  else if (runs.empty()) for (uint32_t i = 0; i < count * 4; ++i) append(in.floats[i]);
  else for (auto run : runs) for (uint32_t i = 0; i < run.count; ++i)
    for (uint32_t component = 0; component < 4; ++component)
      append(in.floats[(uint32_t(run.first) + i) * 4 + component]);
  return out;
}
bool Build(Packet& packet, const Input& input, const std::vector<Run>& runs, uint32_t packed,
           bool null_source = false, bool null_bools = false) {
  const auto floats = std::bit_cast<std::array<float, 1024>>(input.floats);
  return packet.Build(null_bools ? nullptr : input.bools.data(), input.swizzles.data(),
      input.samplers.data(), null_source ? nullptr : floats.data(), runs.data(), runs.size(), packed);
}
bool BuildCandidate(Cache& cache, const Input& input, const std::vector<Run>& runs, uint32_t packed,
                    bool null_source = false, bool null_bools = false) {
  const auto floats = std::bit_cast<std::array<float, 1024>>(input.floats);
  return cache.BuildCandidate(null_bools ? nullptr : input.bools.data(), input.swizzles.data(),
      input.samplers.data(), null_source ? nullptr : floats.data(), runs.data(), runs.size(), packed);
}
bool Equal(std::span<const uint8_t> a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
  return true;
}
void PacketControls() {
  Input in = MakeInput(); Packet packet;
  Check(Build(packet, in, {}, 0), "legacy packet builds");
  Check(packet.bytes().size() == 4288, "legacy zero count means256");
  Check(Equal(packet.bytes(), Oracle(in, {}, 0)), "full packet literal byte oracle");
  Check(Build(packet, in, {}, 1, true), "unused zero-read placeholder builds");
  Check(packet.bytes().size() == 208 && Equal(packet.bytes(), Oracle(in, {}, 1, true)),
        "one-register zero placeholder retains192-byte header");
  Check(Build(packet, in, {{0, 1}}, 1), "actual guest c0 is one register");
  Check(Equal(packet.bytes(), Oracle(in, {{0, 1}}, 1)), "actual c0 preserves NaN payloads");
  const std::vector<Run> sparse = {{255, 1}, {2, 3}, {8, 1}, {0, 1}};
  Check(Build(packet, in, sparse, 6), "ordered sparse runs build");
  Check(Equal(packet.bytes(), Oracle(in, sparse, 6)), "ordered sparse oracle");
  Check(Build(packet, in, {{0, 256}}, 256, false, true), "relative full256 plus null bools");
  Check(Equal(packet.bytes(), Oracle(in, {{0, 256}}, 256, false, true)), "null bool oracle");
  Check(Build(packet, in, {{5, 2}}, 2, true, true), "null float source builds");
  Check(Equal(packet.bytes(), Oracle(in, {{5, 2}}, 2, true, true)), "null float source oracle");
}
void ExplicitEmptyControls() {
  Input in = MakeInput(); Packet packet; Cache cache;
  auto expected = Oracle(in, {}, 0);
  expected.resize(Packet::kHeaderBytes);
  const auto* inaccessible = reinterpret_cast<const float*>(uintptr_t(1));
  const auto* inaccessible_runs = reinterpret_cast<const Run*>(uintptr_t(1));
  const auto build_empty = [&](Packet& target, const Input& input) {
    return target.Build(input.bools.data(), input.swizzles.data(), input.samplers.data(),
                        inaccessible, inaccessible_runs, 0, 0, true);
  };
  Check(build_empty(packet, in), "explicit empty never dereferences float or run sentinel");
  Check(packet.bytes().size() == 192 && Equal(packet.bytes(), expected),
        "header-only PS literal byte oracle");
  Check(cache.Publish(11, packet.bytes(), 0x1234567800), "empty immutable slice publishes");
  in.floats.fill(0xFFFFFFFFu);
  Check(build_empty(packet, in) && cache.Find(11, packet.bytes()) == 0x1234567800,
        "unread float changes do not alter empty packet identity");
  for (size_t byte = 0; byte < Packet::kHeaderBytes; ++byte) {
    Input changed = in;
    uint32_t* word = byte < 32 ? &changed.bools[byte / 4] :
        byte < 112 ? &changed.swizzles[(byte - 32) / 4] : &changed.samplers[(byte - 112) / 4];
    *word ^= 1u << ((byte % 4) * 8);
    Check(build_empty(packet, changed) && !cache.Find(11, packet.bytes()),
          "every live header byte still participates in identity");
  }
  Check(build_empty(packet, in) && !cache.Find(12, packet.bytes()), "empty identity cannot cross frame");
  Check(Build(packet, in, {}, 0), "disabled flag still builds complete legacy payload");
  Check(packet.bytes().size() == 4288 && !cache.Find(11, packet.bytes()),
        "same-frame legacy toggle cannot reuse shorter allocation");
  Check(cache.Publish(11, packet.bytes(), 0x8765432100), "legacy immutable slice publishes");
  Check(build_empty(packet, in) && !cache.Find(11, packet.bytes()),
        "same-frame empty toggle compares complete length");
  Check(cache.BuildCandidate(in.bools.data(), in.swizzles.data(), in.samplers.data(),
                            inaccessible, inaccessible_runs, 0, 0, true), "double empty candidate builds");
  Check(Equal(cache.candidate_bytes(), expected), "double empty candidate literal bytes");
  Check(!cache.PublishCandidate(11, 0), "empty failed allocation never publishes");
  Check(cache.PublishCandidate(11, 0xABCDEF0000), "empty double candidate publishes after upload");
  Check(cache.Find(11, packet.bytes()) == 0xABCDEF0000, "published empty double candidate identity");
  Check(!packet.Build(in.bools.data(), in.swizzles.data(), in.samplers.data(), nullptr,
                      inaccessible_runs, 1, 0, true), "empty contradictory run count rejected before reads");
  Check(packet.bytes().empty(), "bad explicit layout exposes no partial packet");
  Check(!packet.Build(in.bools.data(), in.swizzles.data(), in.samplers.data(), nullptr,
                      static_cast<const Run*>(nullptr), 0, 1, true), "empty contradictory packed count refused");
  cache.Reset();
  Check(build_empty(packet, in) && !cache.Find(11, packet.bytes()), "reset retires empty identity");
}
void BoundsControls() {
  Input in = MakeInput(); Packet packet;
  Check(!Build(packet, in, {}, 257), "oversize bypass");
  Check(packet.bytes().empty(), "failed build exposes no partial packet");
  Check(!Build(packet, in, {{255, 2}}, 2), "source end overrun refused");
  Check(!Build(packet, in, {{257, 0}}, 1), "out-of-range empty run refused");
  Check(!Build(packet, in, {{0, 1}}, 2), "incomplete packet refused");
  Check(!Build(packet, in, {{0, 2}}, 1), "excess run bytes refused");
  Check(!Build(packet, in, {{0, 1}}, 0), "legacy256 inconsistent run total bypass");
  Check(Build(packet, in, {{0, 1}, {256, 0}}, 1), "legal one-past zero-length run");
  const auto floats = std::bit_cast<std::array<float, 1024>>(in.floats);
  Check(packet.Build(in.bools.data(), in.swizzles.data(), in.samplers.data(), floats.data(),
                     static_cast<const Run*>(nullptr), 99, 1), "null runs retains original contiguous path");
  Check(Equal(packet.bytes(), Oracle(in, {}, 1)), "contiguous fallback oracle");
}
void EveryByteControls() {
  Input in = MakeInput(); Packet packet; Cache cache;
  Check(Build(packet, in, {}, 0), "full comparison packet");
  Check(cache.Find(42, packet.bytes()) == 0, "unpublished cache misses");
  Check(cache.Publish(42, packet.bytes(), 0x123400u), "publish succeeds");
  Check(cache.Find(42, packet.bytes()) == 0x123400u, "exact bytes hit");
  std::vector<uint8_t> mutated(packet.bytes().begin(), packet.bytes().end());
  for (size_t i = 0; i < mutated.size(); ++i) {
    mutated[i] ^= 1;
    Check(cache.Find(42, mutated) == 0, "every header/float byte independently invalidates");
    mutated[i] ^= 1;
  }
  Check(cache.Find(42, mutated) == 0x123400u, "mutation never changed cached shadow");
  mutated.pop_back();
  Check(cache.Find(42, mutated) == 0, "length mismatch misses");
}
void LifetimeControls() {
  Input in = MakeInput(); Packet packet; Cache cache;
  Check(Build(packet, in, {{3, 2}}, 2), "lifetime packet");
  const std::vector<uint8_t> old(packet.bytes().begin(), packet.bytes().end());
  Check(cache.Publish(8, old, 0x1000), "lifetime publication");
  Check(cache.Find(9, old) == 0, "frame expiration");
  Check(!cache.Publish(9, old, 0), "failed allocator cannot publish");
  Check(cache.Find(8, old) == 0x1000, "failed publication preserves prior valid mapping");
  Check(cache.Find(9, old) == 0, "failed publication did not retag old memory");
  in.samplers[0] = 0x00020002;
  Check(Build(packet, in, {{3, 2}}, 2), "sampler rollover packet");
  Check(cache.Find(8, packet.bytes()) == 0, "changed descriptor index misses");
  Check(cache.Publish(8, packet.bytes(), 0x2000), "new immutable slice");
  Check(cache.Find(8, old) == 0, "prior content is not falsely reused");
  Check(old == Oracle(MakeInput(), {{3, 2}}, 2), "old published upload model unchanged");
  cache.Reset();
  Check(cache.Find(8, packet.bytes()) == 0, "diagnostic mode reset");
  Check(cache.Publish(UINT64_MAX, packet.bytes(), 0x3000), "last frame value");
  Check(cache.Find(0, packet.bytes()) == 0, "frame number wrap cannot reuse prior frame");
}
void UnusedSourceControls() {
  Input in = MakeInput(); Packet packet; Cache cache;
  const std::vector<Run> runs = {{4, 2}};
  Check(Build(packet, in, runs, 2), "sparse cache initial");
  Check(cache.Publish(1, packet.bytes(), 0x1000), "sparse publication");
  in.floats[0] ^= 1;
  Check(Build(packet, in, runs, 2), "unused guest register mutation");
  Check(cache.Find(1, packet.bytes()) == 0x1000, "unused source changes may hit");
  Check(Build(packet, in, {{5, 2}}, 2), "different layout same size");
  Check(cache.Find(1, packet.bytes()) == 0, "different used bytes miss regardless of layout pointer");
}
void PipelineControls() {
  nb::gpu::NativeReadyPipelineReuse cache;
  uint32_t words[20]{};
  for (uint32_t i = 0; i < 20; ++i) words[i] = i * 0x10001u;
  words[15] = 0x7FC12345u; words[17] = 0xA5A5A5A5u;
  Check(cache.Find(words) == 0, "unpublished ready pipeline misses");
  cache.Publish(words, 0);
  Check(cache.Find(words) == 0, "pending/null pipeline never cached");
  cache.Publish(words, 0x1000);
  Check(cache.Find(words) == 0x1000, "full pipeline exact hit");
  for (size_t word = 0; word < 20; ++word) for (uint32_t bit = 0; bit < 32; ++bit) {
    words[word] ^= uint32_t(1) << bit;
    Check(cache.Find(words) == 0, "all 640 pipeline key bits matter, including sample mask");
    words[word] ^= uint32_t(1) << bit;
  }
  cache.Publish(words, 0);
  Check(cache.Find(words) == 0x1000, "null result does not replace ready value");
  cache.Reset();
  Check(cache.Find(words) == 0, "pipeline mode transition reset");
}
void DepthOnlyControls() {
  using nb::gpu::NativeBindingCoversPixelConstants;
  using nb::gpu::NativeDepthOnlyPixelAddress;
  Check(NativeDepthOnlyPixelAddress(true, true, 0x1000) == 0x1000, "proven unused b2 aliases b1");
  Check(NativeDepthOnlyPixelAddress(false, true, 0x1000) == 0, "legacy depth path still allocates");
  Check(NativeDepthOnlyPixelAddress(true, false, 0x1000) == 0, "PS/VS read or failed reflection refuses alias");
  Check(NativeDepthOnlyPixelAddress(true, true, 0) == 0, "failed vertex upload cannot alias");
  Check(NativeBindingCoversPixelConstants(0, 2, 1), "direct b2 binding");
  Check(NativeBindingCoversPixelConstants(0, 1, 2), "cbuffer array includes b2");
  Check(NativeBindingCoversPixelConstants(0, 0, UINT32_MAX), "unbounded cbuffer range");
  Check(NativeBindingCoversPixelConstants(0, 1, 0), "unknown count fails closed");
  Check(!NativeBindingCoversPixelConstants(1, 2, 1), "different register space");
  Check(!NativeBindingCoversPixelConstants(0, 1, 1), "b1 alone is safe");
  Check(!NativeBindingCoversPixelConstants(0, 3, UINT32_MAX), "higher start never wraps to b2");
}
void EventControls() {
  Input in = MakeInput(); Packet packet; Cache cache;
  std::vector<uint8_t> expected_bytes;
  uint64_t frame = 1, expected_frame = 0, expected_address = 0, next_address = 0x1000;
  bool expected_valid = false;
  std::map<uint64_t, std::vector<uint8_t>> uploads;
  uint32_t random = 0x19482971;
  const auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
  for (uint32_t event = 0; event < 2000; ++event) {
    const uint32_t action = next() % 9;
    if (action == 0) ++frame;
    if (action == 1) { cache.Reset(); expected_valid = false; }
    if (action == 2) in.bools[next() % 8] ^= next();
    if (action == 3) in.swizzles[next() % 20] ^= next();
    if (action == 4) in.samplers[next() % 20] ^= next();
    if (action == 5) in.floats[next() % 1024] ^= next();
    const std::vector<Run> runs = action == 6 ? std::vector<Run>{{0, 4}, {251, 5}}
                                              : std::vector<Run>{{3, 4}, {250, 5}};
    const auto reference = Oracle(in, runs, 9);
    Check(Build(packet, in, runs, 9), "random packet builds");
    Check(Equal(packet.bytes(), reference), "random packet independent byte oracle");
    const uint64_t expected = expected_valid && expected_frame == frame && expected_bytes == reference
                                  ? expected_address : 0;
    const uint64_t actual = cache.Find(frame, packet.bytes());
    Check(actual == expected, "random reuse versus independent last-upload model");
    if (actual) Check(uploads.at(actual) == reference, "hit addresses correct immutable upload content");
    else if (event % 17 == 0) Check(!cache.Publish(frame, packet.bytes(), 0), "random allocation failure");
    else {
      next_address += 0x100;
      uploads.emplace(next_address, reference);
      Check(cache.Publish(frame, packet.bytes(), next_address), "random publication");
      expected_valid = true; expected_bytes = reference; expected_frame = frame; expected_address = next_address;
    }
  }
}
void DoublePacketControls() {
  Input in = MakeInput(); Cache cache;
  const auto original = Oracle(in, {}, 0);
  Check(!cache.PublishCandidate(1, 0x1000), "cannot publish an unbuilt candidate");
  Check(BuildCandidate(cache, in, {}, 0), "owned legacy256 candidate builds");
  Check(Equal(cache.candidate_bytes(), original), "owned candidate independent full-byte oracle");
  const uint8_t* first_slot = cache.candidate_bytes().data();
  Check(cache.Find(1, original) == 0, "candidate build alone never publishes");
  Check(cache.PublishCandidate(1, 0x1000), "first candidate publication");
  Check(cache.candidate_bytes().empty(), "publication consumes pending candidate");
  Check(!cache.PublishCandidate(1, 0x2000), "candidate cannot publish twice");
  Check(cache.Find(1, original) == 0x1000, "owned published identity");

  // Every byte of the complete header and float payload still participates in
  // the exact comparison, while rebuilding the inactive slot leaves the
  // active CPU snapshot and its immutable GPU-address model unchanged.
  for (size_t byte = 0; byte < original.size(); ++byte) {
    const size_t word_index = byte / 4;
    uint32_t& word = word_index < 8 ? in.bools[word_index] :
                     word_index < 28 ? in.swizzles[word_index - 8] :
                     word_index < 48 ? in.samplers[word_index - 28] : in.floats[word_index - 48];
    const uint32_t bit = 1u << ((byte % 4) * 8);
    word ^= bit;
    Check(BuildCandidate(cache, in, {}, 0), "mutated owned packet builds");
    Check(cache.candidate_bytes().data() != first_slot, "candidate storage differs from active slot");
    Check(cache.Find(1, cache.candidate_bytes()) == 0, "every owned candidate byte invalidates");
    Check(cache.Find(1, original) == 0x1000, "candidate mutation never overwrites published bytes");
    word ^= bit;
  }
  in.samplers[0] ^= 0x00010001;
  Check(BuildCandidate(cache, in, {{255, 1}, {0, 1}}, 2), "shorter sparse owned packet");
  const auto shorter = Oracle(in, {{255, 1}, {0, 1}}, 2);
  Check(Equal(cache.candidate_bytes(), shorter), "short packet excludes retained tail");
  Check(!cache.PublishCandidate(2, 0), "allocation failure leaves candidate pending");
  Check(Equal(cache.candidate_bytes(), shorter), "allocation failure does not mutate candidate");
  Check(cache.Find(1, original) == 0x1000 && cache.Find(2, original) == 0,
        "failed publication preserves old frame without retagging");
  Check(cache.PublishCandidate(2, 0x2000), "successful retry flips candidate ownership");
  Check(cache.Find(2, shorter) == 0x2000, "new packet identity and size published");
  Check(cache.Find(2, original) == 0, "old content never aliases new shorter upload");

  Check(!BuildCandidate(cache, in, {{0, 1}}, 2), "incomplete owned candidate fails");
  Check(cache.candidate_bytes().empty() && !cache.PublishCandidate(3, 0x3000),
        "failed build cannot expose or publish old candidate bytes");
  Check(cache.Find(2, shorter) == 0x2000, "failed build preserves active mapping");
  Check(!BuildCandidate(cache, in, {}, 257), "owned packet bounded to256 registers");
  Check(BuildCandidate(cache, in, {}, 1), "next candidate uses prior active storage");
  Check(cache.candidate_bytes().data() == first_slot, "publication changed indices instead of copying bytes");

  // Both diagnostic paths share the same exact cache and frame lifetime. A
  // successful legacy publication supersedes any pending optimized candidate.
  Check(cache.Publish(3, original, 0x3000), "legacy path can replace owned active slot");
  Check(!cache.PublishCandidate(3, 0x4000), "legacy publication invalidates stale candidate");
  Check(BuildCandidate(cache, MakeInput(), {}, 0), "optimized rebuild after legacy publication");
  Check(cache.Find(3, cache.candidate_bytes()) == 0x3000, "mode toggle retains exact live mapping");
  cache.Reset();
  Check(cache.Find(3, original) == 0 && !cache.PublishCandidate(3, 0x4000),
        "reset invalidates active and pending identities");
  const Run* inaccessible_runs = reinterpret_cast<const Run*>(uintptr_t(1));
  Check(cache.BuildCandidate<Run>(nullptr, in.swizzles.data(), in.samplers.data(), nullptr,
                                 inaccessible_runs, 99, 0), "null source ignores inaccessible run metadata");
  Check(Equal(cache.candidate_bytes(), Oracle(in, {}, 0, true, true)), "null source and bools retain all header bytes");
  Check(cache.PublishCandidate(UINT64_MAX, 0x5000), "owned final frame value");
  Check(cache.Find(0, Oracle(in, {}, 0, true, true)) == 0, "owned frame wrap cannot reuse reclaimed upload");
}
void DoublePacketEventControls() {
  Input in = MakeInput(); Packet legacy; Cache cache;
  std::vector<uint8_t> expected_bytes;
  uint64_t frame = 1, expected_frame = 0, expected_address = 0, next_address = 0x1000;
  bool expected_valid = false;
  std::map<uint64_t, std::vector<uint8_t>> uploads;
  uint32_t random = 0x723AF08B;
  const auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
  for (uint32_t event = 0; event < 4000; ++event) {
    const uint32_t action = next() % 12;
    if (action == 0) ++frame;
    if (action == 1) { cache.Reset(); expected_valid = false; }
    if (action == 2) in.bools[next() % 8] ^= next();
    if (action == 3) in.swizzles[next() % 20] ^= next();
    if (action == 4) in.samplers[next() % 20] ^= next();
    if (action == 5) in.floats[next() % 1024] ^= next();
    const bool owned = event % 3 != 0;  // switch implementations within frames
    const bool null_source = action == 6, null_bools = action == 7;
    const std::vector<Run> runs = action == 8 ? std::vector<Run>{} : std::vector<Run>{{0, 4}, {251, 5}};
    const uint32_t packed = action == 8 ? 0u : 9u;
    const auto reference = Oracle(in, runs, packed, null_source, null_bools);
    if (action == 9) {
      // Record's incomplete-metadata bypass resets reuse before the original
      // upload path. No partially built packet becomes a reusable identity.
      Check(!BuildCandidate(cache, in, {{0, 1}}, 2), "event invalid candidate");
      cache.Reset(); expected_valid = false;
      continue;
    }
    if (action == 10) {
      // Reuse-disabled draw resets both modes, independently of the packet flag.
      cache.Reset(); expected_valid = false;
      Check(cache.Find(frame, reference) == 0, "event reuse disabled");
      continue;
    }
    Check(owned ? BuildCandidate(cache, in, runs, packed, null_source, null_bools)
                : Build(legacy, in, runs, packed, null_source, null_bools), "mixed-mode packet builds");
    const auto bytes = owned ? cache.candidate_bytes() : legacy.bytes();
    Check(Equal(bytes, reference), "mixed-mode literal byte oracle");
    const uint64_t expected = expected_valid && expected_frame == frame && expected_bytes == reference
                                  ? expected_address : 0;
    const uint64_t actual = cache.Find(frame, bytes);
    Check(actual == expected, "mixed-mode lookup versus independent upload model");
    if (actual) Check(uploads.at(actual) == reference, "mixed-mode hit preserves immutable GPU bytes");
    else if (event % 17 == 0) {
      Check(!(owned ? cache.PublishCandidate(frame, 0) : cache.Publish(frame, bytes, 0)),
            "mixed-mode failed allocation cannot publish");
      Check(cache.Find(frame, bytes) == 0, "failed allocation cannot become a hit");
    } else {
      next_address += 0x2000;
      uploads.emplace(next_address, reference);  // model upload BEFORE publication
      Check(owned ? cache.PublishCandidate(frame, next_address) : cache.Publish(frame, bytes, next_address),
            "mixed-mode successful upload publication");
      expected_valid = true; expected_bytes = reference; expected_frame = frame; expected_address = next_address;
    }
  }
}
}  // namespace

int main() {
  try {
    static_assert(std::endian::native == std::endian::little, "Windows GPU ABI test oracle");
    PacketControls(); ExplicitEmptyControls(); BoundsControls(); EveryByteControls(); LifetimeControls();
    UnusedSourceControls(); PipelineControls(); DepthOnlyControls(); EventControls();
    DoublePacketControls(); DoublePacketEventControls();
    std::cout << "PASS: 11 native constant reuse controls, " << checks
              << " checks (including 6000 independent upload events)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
