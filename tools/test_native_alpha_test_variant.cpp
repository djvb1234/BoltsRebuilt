// Original portable controls for the native pixel shader without its alpha-test discard: the per-draw
// selection against a transcription of prelude.hlsl AlphaTest, the depth/stencil/coverage gating against
// an independent field decode, the generated-body edit, the sidecar eligibility, and the compile and
// pipeline admission rules, including a model showing the variant never refuses a base draw.
// No SDK, Windows, shader library, compiler DLL or GPU is required.
#include "../src/gpu/native/native_alpha_test_variant.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
uint64_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
uint32_t Bits(uint32_t value, unsigned first, unsigned count) {
  return (value >> first) & ((uint32_t(1) << count) - 1);
}

// src/gpu/native/shaders/prelude.hlsl AlphaTest, statement for statement: whether it discards.
// (tools/test_ucode2hlsl.py pins that function's text, so this copy cannot drift silently.)
bool PreludeAlphaTestDiscards(uint32_t alpha_test, float a, float ref_value) {
  if ((alpha_test & 8u) == 0u) return false;
  const uint32_t f = alpha_test & 7u;
  const bool keep = (f == 1u) ? (a < ref_value) : (f == 2u) ? (a == ref_value) : (f == 3u) ? (a <= ref_value) :
                    (f == 4u) ? (a > ref_value) : (f == 5u) ? (a != ref_value) : (f == 6u) ? (a >= ref_value) :
                    (f == 7u);
  return !keep;
}

void AlphaTestPatterns() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const float values[] = {-inf, -1.0f, -0.0f, 0.0f, std::numeric_limits<float>::denorm_min(), 0.5f,
                          1.0f, 2.0f, inf, nan};
  // RootConstants::alpha_test[0] is RB_COLORCONTROL & 0xF, or 0 with nb_native_alpha_test off.
  for (uint32_t pattern = 0; pattern < 16; ++pattern) {
    bool any_discard = false;
    for (float a : values) for (float ref_value : values) {
      any_discard |= PreludeAlphaTestDiscards(pattern, a, ref_value);
    }
    Check(nb::gpu::NativeAlphaTestCanDiscard(pattern) == any_discard,
          "the predicate matches exactly the patterns where AlphaTest can discard");
    if (!nb::gpu::NativeAlphaTestCanDiscard(pattern)) {
      // Selection needs no alpha or reference value: none can reach the discard.
      for (float a : values) for (float ref_value : values) {
        Check(!PreludeAlphaTestDiscards(pattern, a, ref_value), "a selected draw never discards");
      }
    }
  }
  Check(!nb::gpu::NativeAlphaTestCanDiscard(0), "nb_native_alpha_test off uploads 0: selectable");
  Check(nb::gpu::NativeAlphaTestCanDiscard(8), "enabled NEVER discards everything");
  Check(!nb::gpu::NativeAlphaTestCanDiscard(15), "enabled ALWAYS keeps everything, NaN included");
  for (uint32_t func = 0; func < 7; ++func) {
    Check(nb::gpu::NativeAlphaTestCanDiscard(8 | func), "every other enabled function keeps the discard");
    Check(!nb::gpu::NativeAlphaTestCanDiscard(func), "a disabled test of any function is selectable");
  }
}

// Independent decode of the pipeline descriptor fields GetPipeline builds from GuestState.
bool ReferenceCanHelp(bool dsv_bound, uint32_t depth, uint32_t ref_mask, bool alpha_to_mask, uint32_t samples) {
  if (!dsv_bound) return false;                       // DepthEnable and StencilEnable are forced off
  if (alpha_to_mask && samples > 1) return false;     // AlphaToCoverageEnable is set
  const bool z_enable = Bits(depth, 1, 1), z_write = Bits(depth, 2, 1);
  const bool stencil = Bits(depth, 0, 1), backface = Bits(depth, 7, 1);
  const uint32_t write_mask = Bits(ref_mask, 16, 8);
  const uint32_t front[3] = {Bits(depth, 11, 3), Bits(depth, 14, 3), Bits(depth, 17, 3)};
  const uint32_t back[3] = {Bits(depth, 23, 3), Bits(depth, 26, 3), Bits(depth, 29, 3)};
  bool op_writes = false;
  for (unsigned i = 0; i < 3; ++i) {
    op_writes |= front[i] != 0;                  // StencilOp::kKeep is 0
    op_writes |= backface && back[i] != 0;       // without backface_enable BackFace = FrontFace
  }
  return (z_enable && z_write) || (stencil && write_mask != 0 && op_writes);
}

bool CanHelp(bool dsv, uint32_t depth, uint32_t ref_mask, bool a2c, uint32_t samples) {
  return nb::gpu::NativeEarlyDepthStencilCanHelp(dsv, depth, ref_mask, a2c, samples);
}

void DepthStencilGating() {
  constexpr uint32_t kStencil = 1u, kZ = 2u, kZWrite = 4u, kBackface = 0x80u;
  constexpr uint32_t kWriteMask = 0x00FF0000u;
  Check(CanHelp(true, kZ | kZWrite, 0, false, 1), "depth test and write");
  Check(!CanHelp(true, kZ, kWriteMask, false, 1), "depth test without write keeps early testing");
  Check(!CanHelp(true, kZWrite, kWriteMask, false, 1), "depth write without depth enable writes nothing");
  Check(!CanHelp(false, kZ | kZWrite, kWriteMask, false, 1), "no depth/stencil view");
  Check(!CanHelp(true, kZ | kZWrite | 0x70u, 0, true, 2), "MSAA alpha to coverage");
  Check(!CanHelp(true, kZ | kZWrite, 0, true, 4), "MSAA alpha to coverage, 4 samples");
  Check(CanHelp(true, kZ | kZWrite, 0, true, 1), "single-sample alpha to coverage is disabled in the PSO");
  Check(CanHelp(true, kZ | kZWrite, 0, false, 8), "MSAA without alpha to coverage");
  for (unsigned shift : {11u, 14u, 17u}) {
    for (uint32_t op = 1; op < 8; ++op) {
      const uint32_t depth = kStencil | (op << shift);
      Check(CanHelp(true, depth, 0x00010000u, false, 1), "any writing front-face stencil op");
      Check(!CanHelp(true, depth, 0x0000FFFFu, false, 1), "a zero stencil write mask writes nothing");
      Check(!CanHelp(true, depth & ~kStencil, kWriteMask, false, 1), "disabled stencil writes nothing");
      Check(!CanHelp(false, depth, kWriteMask, false, 1), "stencil without a depth/stencil view");
    }
  }
  Check(!CanHelp(true, kStencil | kBackface, kWriteMask, false, 1), "all KEEP ops write nothing");
  for (unsigned shift : {23u, 26u, 29u}) {
    for (uint32_t op = 1; op < 8; ++op) {
      const uint32_t depth = kStencil | (op << shift);
      Check(!CanHelp(true, depth, kWriteMask, false, 1), "back-face ops are unused without backface_enable");
      Check(CanHelp(true, depth | kBackface, kWriteMask, false, 1), "back-face ops with backface_enable");
    }
  }
  // Stencil function, reference, read mask and depth function never decide it.
  Check(!CanHelp(true, kStencil | kZ | 0x700u | 0x70u | kBackface | (7u << 20), 0xFFFFFFFFu, false, 1),
        "functions, reference and read mask write nothing");
  // Randomized agreement with the independent decode, including padding bits.
  uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto random = [&] {
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    return uint32_t((state * 0x2545F4914F6CDD1Dull) >> 32);
  };
  const uint32_t samples[] = {0, 1, 2, 4, 8};
  for (unsigned i = 0; i < 400000; ++i) {
    uint32_t depth = random();
    // Bias toward the rarer all-KEEP and zero-mask cases.
    if (i % 4 == 0) depth &= ~0xFF8FF800u;
    uint32_t ref_mask = random();
    if (i % 3 == 0) ref_mask &= ~0x00FF0000u;
    const bool dsv = random() % 4 != 0, a2c = random() % 3 == 0;
    const uint32_t count = samples[random() % 5];
    Check(CanHelp(dsv, depth, ref_mask, a2c, count) == ReferenceCanHelp(dsv, depth, ref_mask, a2c, count),
          "gating agrees with the independent descriptor decode");
    for (uint32_t pattern = 0; pattern < 16; ++pattern) {
      Check(nb::gpu::NativeAlphaTestVariantSelected(pattern, dsv, depth, ref_mask, a2c, count) ==
                (!nb::gpu::NativeAlphaTestCanDiscard(pattern) &&
                 ReferenceCanHelp(dsv, depth, ref_mask, a2c, count)),
            "selection is exactly no possible discard and a possible early depth/stencil gain");
    }
  }
}

// The tail tools/ucode2hlsl.py translate_ps emits for a single-target shader.
const std::string kGenerated =
    "struct PsIn { float4 pos : SV_Position; float2 ptcoord : POINTCOORD; bool front : SV_IsFrontFace; };\n"
    "float4 main(PsIn i) : SV_Target0 {\n"
    "  float ps = 0.0;\n"
    "  bool p0 = false;\n"
    "  float4 oC0 = 0.0;\n"
    "  float4 v1 = (ps_c[0] + ps_c[1]);\n"
    "  oC0 = v1;\n"
    "  AlphaTest(oC0.w);\n"
    "  return oC0;\n"
    "}\n";

void Unchanged(const std::string& body, const char* message) {
  std::string edited = body;
  Check(!nb::gpu::StripNativeAlphaTest(edited), message);
  Check(edited == body, "a refused body is left unchanged");
}

void BodyEdit() {
  std::string body = kGenerated;
  Check(nb::gpu::StripNativeAlphaTest(body), "the generated statement is found exactly once");
  std::string expected = kGenerated;
  expected.replace(expected.find("AlphaTest(oC0.w);"), 17, ";");
  Check(body == expected, "only the statement changes, to an empty statement");
  Check(body.find("AlphaTest") == std::string::npos && body.find("discard") == std::string::npos,
        "no alpha test or discard left in the body");
  Check(!nb::gpu::StripNativeAlphaTest(body), "an edited body has no statement left");

  std::string crlf = "  oC0 = v1;\r\n  AlphaTest(oC0.w);\r\n  return oC0;\r\n";
  Check(nb::gpu::StripNativeAlphaTest(crlf), "CRLF line ends");
  Check(crlf == "  oC0 = v1;\r\n  ;\r\n  return oC0;\r\n", "CRLF kept");
  std::string edges = "AlphaTest(oC0.w);";
  Check(nb::gpu::StripNativeAlphaTest(edges) && edges == ";", "statement at both ends of the body");
  std::string tabbed = "{\n\tAlphaTest(oC0.w);\n}";
  Check(nb::gpu::StripNativeAlphaTest(tabbed) && tabbed == "{\n\t;\n}", "tab indentation");

  Unchanged("", "empty body");
  Unchanged("  oC0 = v1;\n  return oC0;\n", "no statement");
  Unchanged("  AlphaTest(oC1.w);\n", "a different operand is not the generated statement");
  Unchanged("  AlphaTest(oC0.w)\n", "no semicolon");
  Unchanged("  AlphaTest(oC0.w);\n  AlphaTest(oC0.w);\n", "two statements");
  Unchanged("  AlphaTest(oC0.w);\n  // AlphaTest(oC0.w);\n", "a second, commented copy");
  Unchanged(kGenerated + "  AlphaTest(oC0.w);\n", "generated body plus another statement");
  Unchanged("  AlphaTest(oC0.w);AlphaTest(oC0.w);\n", "adjacent copies");
  Unchanged("  if (p0) AlphaTest(oC0.w);\n", "not alone on its line (before)");
  Unchanged("  AlphaTest(oC0.w); oC0 = 0.0;\n", "not alone on its line (after)");
  Unchanged("  // AlphaTest(oC0.w);\n", "only in a comment");
  Unchanged("  MyAlphaTest(oC0.w);\n", "a longer identifier");
  Unchanged("  AlphaTest(oC0.w);\r  x;\n", "a bare carriage return is not a line end");
}

void Sidecar() {
  using Sidecar = nb::gpu::NativeAlphaTestVariantSidecar;
  const auto eligible = [](std::initializer_list<std::pair<const char*, const char*>> lines) {
    Sidecar sidecar;
    for (const auto& [key, value] : lines) sidecar.Observe(key, value);
    return sidecar.eligible();
  };
  Check(eligible({{"hash", "1"}, {"writes_depth", "0"}, {"kills", "0"}, {"colors", "1"}}), "kills=0, writes_depth=0");
  Check(!eligible({}), "an empty sidecar");
  Check(!eligible({{"writes_depth", "0"}}), "a missing kills= counts as a kill");
  Check(!eligible({{"kills", "0"}}), "a missing writes_depth= counts as a depth write");
  Check(!eligible({{"writes_depth", "0"}, {"kills", "1"}}), "a guest kill");
  Check(!eligible({{"writes_depth", "1"}, {"kills", "0"}}), "a depth write");
  Check(!eligible({{"writes_depth", "0"}, {"kill", "0"}}), "the vertex kill= key is a different fact");
  Check(!eligible({{"writes_depth", "0"}, {"kills", ""}}), "an empty value");
  Check(!eligible({{"writes_depth", "0"}, {"kills", "0 "}}), "an unparsed value");
  Check(!eligible({{"writes_depth", "00"}, {"kills", "0"}}), "only the exact generated value");
  Check(!eligible({{"writes_depth", "0"}, {"kills", "0"}, {"kills", "1"}}), "the last line wins, as in ParseMeta");
  Check(eligible({{"kills", "1"}, {"writes_depth", "0"}, {"kills", "0"}}), "the last line wins either way");
}
void AdmissionRules() {
  using nb::gpu::NativeBaseSlotFree;
  using nb::gpu::NativeNoAlphaTestCompileAdmitted;
  using nb::gpu::NativeNoAlphaTestPipelineAdmitted;
  constexpr uint32_t kLimit = nb::gpu::kNativeNoAlphaTestPipelinesInFlight;
  Check(kLimit >= 1 && kLimit <= 2, "a small process-wide variant pipeline limit");
  for (size_t cap : {1u, 2u, 4u, 12u}) {
    for (size_t base = 0; base <= cap + 1; ++base) {
      for (size_t variant = 0; variant <= 2; ++variant) {
        Check(NativeBaseSlotFree(base + variant, variant, cap) == (base < cap),
              "a base request sees only base work, whatever the variant has in flight");
        Check(NativeNoAlphaTestCompileAdmitted(base + variant, variant, cap) == (variant == 0 && base < cap),
              "one variant compile, only while base compiles are below the cap");
      }
    }
  }
  for (size_t pass = 0; pass < 4; ++pass) {
    for (uint32_t all = 0; all < 4; ++all) {
      Check(NativeNoAlphaTestPipelineAdmitted(pass, all) == (pass == 0 && all < kLimit),
            "a variant pipeline only from an idle pass, within the process-wide limit");
    }
  }
}

// NativeGeometryPass::Record's pipeline scheduling, modelled: each creation takes a fixed time, a draw
// names a pass, a state key and whether it selected the variant, and all passes share the process-wide
// variant limit. A pass reaps its own results only when it is drawn; the process-wide count drops when
// the creation finishes, as the worker uncounts itself. Outcome 0 is a refused (emulated) draw, 1 the
// base pipeline, 2 the variant.
struct PipelineModel {
  struct Pass {
    std::map<std::pair<uint32_t, bool>, uint64_t> pending;  // (key, variant) -> completion time
    std::set<std::pair<uint32_t, bool>> ready;
    uint32_t variant_pending = 0;
  };
  std::vector<Pass> passes;
  std::multiset<uint64_t> variant_running;  // completion times of variant creations in flight
  uint32_t variant_unreaped = 0;
  size_t cap;
  uint64_t duration;
  bool enabled;
  // Earlier rules, kept as negative controls: variants counted in the base cap, and the process-wide
  // variant count held until the owning pass reaps.
  bool count_variants_in_base_cap = false;
  bool count_variants_until_reaped = false;

  PipelineModel(size_t pass_count, size_t base_cap, uint64_t creation_time, bool variant_enabled)
      : passes(pass_count), cap(base_cap), duration(creation_time), enabled(variant_enabled) {}

  uint32_t VariantsInFlight(uint64_t now) {
    if (count_variants_until_reaped) return variant_unreaped;
    variant_running.erase(variant_running.begin(), variant_running.upper_bound(now));
    return uint32_t(variant_running.size());
  }

  int Draw(size_t index, uint32_t key, bool selected, uint64_t now) {
    Pass& pass = passes[index];
    for (auto it = pass.pending.begin(); it != pass.pending.end();) {  // ReapPipelines
      if (it->second > now) { ++it; continue; }
      if (it->first.second) { --pass.variant_pending; --variant_unreaped; }
      pass.ready.insert(it->first);
      it = pass.pending.erase(it);
    }
    const bool want = enabled && selected;
    if (want && pass.ready.count({key, true})) return 2;  // FindNoAlphaTestPipeline: ready
    const bool absent = want && !pass.pending.count({key, true});
    if (!pass.ready.count({key, false})) {  // GetPipeline: the base pipeline, as without the variant
      if (!pass.pending.count({key, false})) {
        const bool free = count_variants_in_base_cap
                              ? pass.pending.size() < cap
                              : nb::gpu::NativeBaseSlotFree(pass.pending.size(), pass.variant_pending, cap);
        if (free) pass.pending[{key, false}] = now + duration;
      }
      return 0;
    }
    if (absent && nb::gpu::NativeNoAlphaTestPipelineAdmitted(pass.pending.size(), VariantsInFlight(now))) {
      pass.pending[{key, true}] = now + duration;
      variant_running.insert(now + duration);
      ++pass.variant_pending;
      ++variant_unreaped;
    }
    return 1;
  }
};

struct ModelRun {
  std::vector<int> outcomes;
  uint64_t variant_draws = 0;
};

ModelRun RunPipelineModel(uint64_t seed, size_t cap, bool enabled, bool control = false) {
  PipelineModel model(3, cap, 300, enabled);
  model.count_variants_in_base_cap = control;
  uint64_t state = seed;
  const auto random = [&] {
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    return uint32_t((state * 0x2545F4914F6CDD1Dull) >> 32);
  };
  ModelRun run;
  for (uint64_t now = 0; now < 20000; ++now) {
    const size_t pass = random() % 3;
    // Bursts of new state keys, as warm-up and scene changes bring them, between quieter stretches that
    // redraw what is already known.
    const bool burst = (now / 2500) % 2 == 0;
    const uint32_t key = burst && random() % 4 == 0 ? 24 + random() % (1 + now / 50) : random() % 24;
    const bool selected = random() % 3 != 0;
    const int outcome = model.Draw(pass, key, selected, now);
    run.outcomes.push_back(outcome);
    run.variant_draws += outcome == 2;
    Check(outcome != 2 || selected, "only a selecting draw gets the variant");
    for (const auto& modelled : model.passes) {
      Check(modelled.variant_pending <= 1, "at most one variant creation pending per pass");
      Check(modelled.pending.size() - modelled.variant_pending <= cap, "base creations stay within the cap");
    }
    Check(model.VariantsInFlight(now) <= nb::gpu::kNativeNoAlphaTestPipelinesInFlight,
          "variant creations stay within the process-wide limit");
  }
  return run;
}

void PipelineScheduling() {
  bool control_differs = false;
  for (size_t cap : {1u, 2u, 4u, 12u}) {
    for (uint64_t seed : {0x9E3779B97F4A7C15ull, 0xDC12123456ABCDEFull, 0x0123456789ABCDEFull}) {
      const ModelRun off = RunPipelineModel(seed, cap, false);
      const ModelRun on = RunPipelineModel(seed, cap, true);
      Check(off.variant_draws == 0, "no variant with the switch off");
      Check(on.variant_draws > 0, "the variant does get used");
      for (size_t i = 0; i < off.outcomes.size(); ++i) {
        Check((off.outcomes[i] == 0) == (on.outcomes[i] == 0),
              "enabling the variant never refuses a draw the base path would record, nor the reverse");
      }
      const ModelRun control = RunPipelineModel(seed, cap, true, true);
      for (size_t i = 0; i < off.outcomes.size(); ++i) {
        control_differs |= (off.outcomes[i] == 0) != (control.outcomes[i] == 0);
      }
    }
  }
  Check(control_differs, "the model detects variants that take base slots");
}

// Passes 0 and 1 warm up and then leave the frame for good (a level exit or a cutscene) while their
// variant creations are in flight; only pass 2 is drawn afterwards. Returns pass 2's variant draws.
uint64_t IdlePassVariantDraws(uint64_t scene_end, bool count_until_reaped) {
  PipelineModel model(3, 4, 300, true);
  model.count_variants_until_reaped = count_until_reaped;
  uint64_t variant_draws = 0;
  for (uint64_t now = 0; now < 20000; ++now) {
    const bool old_scene = now < scene_end;
    const size_t pass = old_scene ? size_t(now % 2) : 2;
    const uint32_t key = old_scene ? uint32_t(now / 2 % 3) : uint32_t(now / 7 % 40);
    const int outcome = model.Draw(pass, key, true, now);
    if (!old_scene) variant_draws += outcome == 2;
  }
  return variant_draws;
}

void IdlePasses() {
  for (uint64_t scene_end : {350u, 450u, 590u, 700u}) {
    Check(IdlePassVariantDraws(scene_end, false) > 0, "passes that stop drawing do not hold the variant limit");
    Check(IdlePassVariantDraws(scene_end, true) == 0,
          "control: a count held until the reap starves every other pass");
  }
}
}  // namespace

int main() {
  try {
    AlphaTestPatterns();
    DepthStencilGating();
    BodyEdit();
    Sidecar();
    AdmissionRules();
    PipelineScheduling();
    IdlePasses();
    std::cout << "PASS native alpha test variant: " << checks << " checks\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native alpha test variant after " << checks << " checks: " << e.what() << '\n';
    return 1;
  }
}
