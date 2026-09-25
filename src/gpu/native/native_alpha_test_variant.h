// Original, portable policy for the native pixel shader without its alpha-test discard.
//
// Every generated pixel shader ends with AlphaTest(oC0.w) (tools/ucode2hlsl.py). Its discard depends on
// a root constant, so the compiled shader can always discard, and a GPU then tests and writes depth and
// stencil after shading for every draw that writes them. The variant is the same generated body with
// only that statement removed, compiled with the same prelude and defines. A draw uses it only when the
// alpha-test word it uploads makes AlphaTest return without discarding, so the two shaders are
// equivalent at the HLSL level. They are separate programs compiled without IEEE strictness (float
// refactoring allowed), so identical bits are not guaranteed by construction; a DXBC diff and an on/off
// readback comparison on Windows confirm them. For a shader without guest kills or SV_Depth (the sidecar
// facts below) the variant has no discard, depth, UAV or coverage output left, so the driver may test
// depth and stencil before shading. Nothing forces that ([earlydepthstencil] would be wrong with alpha
// to coverage).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nb::gpu {

// The statement ucode2hlsl.py emits once, on its own line, before the outputs are copied out.
inline constexpr std::string_view kNativeAlphaTestStatement = "AlphaTest(oC0.w);";

// Replaces the generated AlphaTest statement with an empty statement, so the surrounding code parses
// exactly as before. False, with the body unchanged, unless the statement appears exactly once and
// alone on its line (indentation before it, a line end after it).
inline bool StripNativeAlphaTest(std::string& body) {
  const size_t at = body.find(kNativeAlphaTestStatement);
  if (at == std::string::npos ||
      body.find(kNativeAlphaTestStatement, at + 1) != std::string::npos) {
    return false;
  }
  size_t line = at;
  while (line != 0 && (body[line - 1] == ' ' || body[line - 1] == '\t')) --line;
  if (line != 0 && body[line - 1] != '\n') return false;
  size_t end = at + kNativeAlphaTestStatement.size();
  if (end < body.size() && body[end] == '\r') ++end;
  if (end < body.size() && body[end] != '\n') return false;
  body.replace(at, kNativeAlphaTestStatement.size(), ";");
  return true;
}

// The sidecar facts a pixel shader needs: kills=0 (a guest kill keeps its discard in the variant) and
// writes_depth=0 (SV_Depth also keeps depth late). A missing line or any other value is ineligible.
class NativeAlphaTestVariantSidecar {
 public:
  void Observe(std::string_view key, std::string_view value) {
    if (key == "kills") {
      no_kill_ = value == "0";
    } else if (key == "writes_depth") {
      no_depth_ = value == "0";
    }
  }
  bool eligible() const { return no_kill_ && no_depth_; }

 private:
  bool no_kill_ = false;
  bool no_depth_ = false;
};

// `alpha_test` is RootConstants::alpha_test[0] exactly as uploaded: RB_COLORCONTROL alpha_func (bits
// 0..2) | alpha_test_enable (bit 3), or 0 with nb_native_alpha_test off. prelude.hlsl AlphaTest returns
// when the enable bit is clear and keeps every fragment for func 7 (always, NaN included), so only an
// enabled test with another function can reach its discard.
constexpr bool NativeAlphaTestCanDiscard(uint32_t alpha_test) {
  return (alpha_test & 8u) != 0 && (alpha_test & 7u) != 7u;
}

// Whether early depth/stencil can save shading work for this pipeline state: a depth/stencil view is
// bound, and the draw writes depth or can write stencil (a draw that writes neither keeps early testing
// even with a discard). Alpha to coverage, which the pipeline enables only with MSAA, makes coverage
// depend on the shader output either way. `depthcontrol` and `stencil_ref_mask` are the
// NativeGeometryPass::GuestState words the pipeline is built from: RB_DEPTHCONTROL stencil_enable +0,
// z_enable +1, z_write_enable +2, backface_enable +7, 3-bit stencil ops at +11/+14/+17 (front) and
// +23/+26/+29 (back; the front ops apply to both faces without backface_enable), StencilOp::kKeep = 0;
// RB_STENCILREFMASK stencilwritemask +16.
constexpr bool NativeEarlyDepthStencilCanHelp(bool dsv_bound, uint32_t depthcontrol,
                                              uint32_t stencil_ref_mask, bool alpha_to_mask,
                                              uint32_t sample_count) {
  if (!dsv_bound || (alpha_to_mask && sample_count > 1)) return false;
  const bool depth_write = (depthcontrol & 0x6u) == 0x6u;
  const uint32_t stencil_ops = 0x000FF800u | ((depthcontrol & 0x80u) ? 0xFF800000u : 0u);
  const bool stencil_write = (depthcontrol & 0x1u) != 0 && (stencil_ref_mask & 0x00FF0000u) != 0 &&
                             (depthcontrol & stencil_ops) != 0;
  return depth_write || stencil_write;
}

// The per-draw choice, from the final root constants and pipeline state of the draw.
constexpr bool NativeAlphaTestVariantSelected(uint32_t alpha_test, bool dsv_bound, uint32_t depthcontrol,
                                              uint32_t stencil_ref_mask, bool alpha_to_mask,
                                              uint32_t sample_count) {
  return !NativeAlphaTestCanDiscard(alpha_test) &&
         NativeEarlyDepthStencilCanHelp(dsv_bound, depthcontrol, stencil_ref_mask, alpha_to_mask,
                                        sample_count);
}

// Scheduling. The variant's compiles and pipeline creations are optional work, since its draws already
// have the base form, so they never count against a base cap: a base compile or pipeline creation is
// admitted exactly when it would be with the variant disabled, and no draw is refused or left emulated
// for a slot the variant holds. Its bounded background work can still compete for CPU time.
constexpr bool NativeBaseSlotFree(size_t in_flight, size_t variant_in_flight, size_t cap) {
  return in_flight - variant_in_flight < cap;
}

// One variant compile at a time per library, started only while base compiles are below their cap.
constexpr bool NativeNoAlphaTestCompileAdmitted(size_t in_flight, size_t variant_in_flight, size_t cap) {
  return variant_in_flight == 0 && in_flight < cap;
}

// A pass queues a variant pipeline only while nothing of its own is pending (so none of its base
// pipelines is, and at most one variant is), and only while fewer than kNativeNoAlphaTestPipelinesInFlight
// variant creations are in flight across all passes, counted until the driver call returns rather than
// until the pass reaps the result (a pass that is never drawn again never reaps). Each costs about as
// much driver time as a base one, and warm-up would otherwise do nearly twice the work.
inline constexpr uint32_t kNativeNoAlphaTestPipelinesInFlight = 2;
constexpr bool NativeNoAlphaTestPipelineAdmitted(size_t pass_in_flight, uint32_t variant_in_flight) {
  return pass_in_flight == 0 && variant_in_flight < kNativeNoAlphaTestPipelinesInFlight;
}

}  // namespace nb::gpu
