// Original portable metadata and bytecode-extent policy controls. Synthetic only.
#include "../src/gpu/native/native_constant_layout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Run { uint16_t first, count; };
using Layout = nb::gpu::NativeConstantLayoutMetadata<Run>;
using Match = nb::gpu::NativeConstantBindingMatch;
uint64_t checks = 0;
void Check(bool okay, const char* message) {
  ++checks;
  if (!okay) throw std::runtime_error(message);
}
void PresenceAndBounds() {
  Layout missing;
  Check(missing.valid() && !missing.has_packed_layout && missing.upper_bound == 256,
        "absent metadata is legacy");
  Check(missing.SetUpperBound("0") && missing.valid() && !missing.has_packed_layout,
        "consts zero alone does not promise empty packed layout");
  Check(nb::gpu::NativeConstantRegisters(missing.packed_registers) == 256,
        "missing cruns retains256 even with consts zero");
  Layout empty;
  Check(empty.SetRuns("") && empty.valid() && empty.has_packed_layout && empty.runs.empty(),
        "explicit cruns promise suffices when optional consts is absent");
  Check(empty.upper_bound == 256 && empty.packed_registers == 0,
        "default legacy upper bound must not erase explicit presence");
  Check(empty.SetUpperBound("0") && empty.valid(), "explicit zero fields agree");
  Check(!empty.SetRuns("") && !empty.valid(), "duplicate empty line refused");
  Layout contradictory;
  Check(contradictory.SetRuns("") && contradictory.SetUpperBound("1") && !contradictory.valid(),
        "positive declared reads contradict explicit empty");
  Layout no_pack;
  Check(no_pack.SetUpperBound("0") && no_pack.SetRuns("0-255") && no_pack.valid(),
        "NO_PACK full layout is valid even without collected float reads");
  Check(no_pack.packed_registers == 256 && no_pack.runs.size() == 1 &&
        no_pack.runs[0].first == 0 && no_pack.runs[0].count == 256, "full run literal");
  Layout ordered;
  Check(ordered.SetRuns("255-255,2-4,8-8,0-0") && ordered.valid(), "packed order preserved");
  Check(ordered.packed_registers == 6 && ordered.runs[0].first == 255 &&
        ordered.runs[1].count == 3 && ordered.runs[3].first == 0, "literal run mapping");
  const std::array<std::string, 16> bad_runs = {"0", "-1-2", "0-", "0--1", "1-0",
      "0-256", "65536-65536", "0-0,", ",0-0", "0-0,,1-1", "0-0x", " 0-0",
      "0-255,0-0", "4294967296-4294967296", "0-0-0", "0-0\n"};
  for (const auto& text : bad_runs) {
    Layout invalid;
    Check(!invalid.SetRuns(text) && !invalid.valid(), "malformed/bounded run rejected");
    Check(invalid.runs.empty() && invalid.packed_registers == 0,
          "failed parsing publishes no partial layout");
  }
  for (const std::string text : {"", "+0", "-0", "0x0", "0 ", "1x", "257", "4294967296"}) {
    Layout invalid;
    Check(!invalid.SetUpperBound(text) && !invalid.valid(), "bad decimal upper bound rejected");
  }
  Layout duplicate;
  Check(duplicate.SetUpperBound("256") && !duplicate.SetUpperBound("256") && !duplicate.valid(),
        "duplicate upper bound rejected");
}
void IndependentRunEvents() {
  uint32_t rng = 0x92A6B135;
  for (uint32_t event = 0; event < 6000; ++event) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    std::vector<Run> expected;
    std::string text;
    uint32_t next = rng % 17, total = 0;
    for (uint32_t i = 0; i < 1 + rng % 8 && next < 256; ++i) {
      const uint32_t count = std::min(1 + ((rng >> (i * 3)) & 15), 256 - next);
      if (!text.empty()) text += ',';
      text += std::to_string(next) + '-' + std::to_string(next + count - 1);
      expected.push_back(Run{uint16_t(next), uint16_t(count)});
      total += count;
      next += count + 1 + ((rng >> (i * 2)) & 7);
    }
    Layout actual;
    Check(actual.SetRuns(text) && actual.valid(), "generated independent interval string parses");
    Check(actual.has_packed_layout && actual.packed_registers == total &&
          actual.runs.size() == expected.size(), "independent interval total/count");
    for (size_t i = 0; i < expected.size(); ++i)
      Check(actual.runs[i].first == expected[i].first && actual.runs[i].count == expected[i].count,
            "independent interval values/order");
  }
  for (uint32_t packed = 0; packed <= 256; ++packed) {
    Check(nb::gpu::NativeConstantRegisters(packed, false) == (packed == 0 ? 256u : packed),
          "legacy extent literal");
    Check(nb::gpu::NativeConstantRegisters(packed, true) == packed, "explicit extent literal");
  }
}
// Independently flattened reflection fixtures. Production reads actual D3D
// reflection; this controls its two pure range policies, including both blobs.
struct Variable { bool used; uint32_t offset, bytes; };
struct Binding {
  uint32_t space, first, count;
  bool readable = true;
  std::vector<Variable> variables;
};
bool ProjectedFits(const std::vector<Binding>& bindings, uint32_t target, uint32_t header) {
  bool found = false;
  for (const auto& b : bindings) {
    if (!b.readable) return false;
    const Match m = nb::gpu::NativeConstantHeaderBindingMatch(b.space, b.first, b.count, target);
    if (m == Match::kIgnored) continue;
    if (m == Match::kAmbiguous || found) return false;
    found = true;
    for (const auto& v : b.variables)
      if (!nb::gpu::NativeConstantVariableFitsHeader(v.used, v.offset, v.bytes, header)) return false;
  }
  return true;
}
void ReflectionExtentControls() {
  for (const uint32_t header : {192u, 688u}) {
    const std::vector<Binding> only_header{{0, 1, 1, true, {{true, 0, header}, {false, header, 4096}}}};
    Check(ProjectedFits(only_header, 1, header), "unused trailing declared array allowed");
    auto tail = only_header; tail[0].variables[1].used = true;
    Check(!ProjectedFits(tail, 1, header), "actual tail read refuses shorter packet");
    Check(ProjectedFits({}, 1, header), "absent target has no reads");
    Check(!ProjectedFits({{0, 1, 1, false, {}}}, 1, header), "reflection failure refuses");
    Check(!ProjectedFits({only_header[0], only_header[0]}, 1, header), "duplicate binding ambiguous");
    Check(!ProjectedFits({{0, 0, 3, true, {}}}, 1, header), "CBV array covering target ambiguous");
    Check(!ProjectedFits({{0, 99, 0, true, {}}}, 1, header), "unknown array conservative");
    Check(ProjectedFits({{3, 1, 1, true, {{true, 0, UINT32_MAX}}}}, 1, header), "different register space irrelevant");
    Check(!nb::gpu::NativeConstantVariableFitsHeader(true, UINT32_MAX, 2, header), "overflow-safe used offset");
    Check(!nb::gpu::NativeConstantVariableFitsHeader(true, header, 0, header), "unknown used zero-size extent refuses");
    // Both linked blobs must pass, including a PS reading vertex b1 or VS reading pixel b2.
    Check(!(ProjectedFits(only_header, 1, header) && ProjectedFits(tail, 1, header)),
          "second linked shader tail read disqualifies target");
    for (uint32_t offset = 0; offset <= header + 16; ++offset) {
      for (uint32_t size : {1u, 4u, 16u, 4096u, UINT32_MAX}) {
        const bool oracle = uint64_t(offset) + size <= header;
        Check(nb::gpu::NativeConstantVariableFitsHeader(true, offset, size, header) == oracle,
              "used interval against independent64-bit oracle");
        Check(nb::gpu::NativeConstantVariableFitsHeader(false, offset, size, header),
              "unused variable never adds an actual read");
      }
    }
  }
  for (uint32_t target : {1u, 2u}) for (uint32_t first = 0; first < 6; ++first)
    for (uint32_t count = 0; count < 6; ++count) {
      Match expected = count == 0 ? Match::kAmbiguous : Match::kIgnored;
      for (uint32_t index = first; index < first + count; ++index)
        if (index == target) expected = count == 1 ? Match::kExact : Match::kAmbiguous;
      Check(nb::gpu::NativeConstantHeaderBindingMatch(0, first, count, target) == expected,
            "binding matches independent register enumeration");
    }
}
}  // namespace
int main() {
  try {
    PresenceAndBounds(); IndependentRunEvents(); ReflectionExtentControls();
    std::cout << "PASS " << checks << " native constant-layout controls\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n'; return 1;
  }
}
