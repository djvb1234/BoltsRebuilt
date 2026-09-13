// Original, portable normalization of the native geometry pass's lookup key.
// This changes no guest state and no D3D12 descriptor. Equal normalized keys
// must produce exactly equal fields in the EXISTING GetPipeline descriptor
// builder (within the same pass / retained shader and root-signature owner).
#pragma once

#include <cstdint>

namespace nb::gpu {

// Layout is the original NativeGeometryPass::PipelineKey: RTVs[0..3], DSV[4],
// sample count[5], blend controls[6..9], depth[10], setup[11], color mask[12],
// stencil ref/masks[13], integer/float bias bits[14..15], clip/coverage/root-CBV
// flags[16], sample mask[17], reserved[18..19]. DXGI_FORMAT_UNKNOWN is zero.
// Return whether any input word changed. No allocation or floating-point work.
inline bool NormalizeNativePipelineKey(uint32_t (&words)[20]) noexcept {
  uint32_t changed = 0;
  const auto replace = [&](uint32_t index, uint32_t value) {
    changed |= words[index] ^ value;
    words[index] = value;
  };

  uint32_t color_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (words[i]) {
      // Only the six declared factor/op fields reach either BlendEnable's
      // identity check or MapBlendFactor/MapBlendOp. Preserve their raw enums.
      replace(6 + i, words[6 + i] & 0x1FFF1FFFu);
      color_mask |= 0xFu << (4 * i);
    } else {
      // The descriptor explicitly supplies fixed disabled state for UNKNOWN.
      replace(6 + i, 0);
    }
  }
  replace(12, words[12] & color_mask);

  const uint32_t original_depth = words[10];
  uint32_t depth = original_depth & ~0x8u;  // unused padding bit
  if (!(original_depth & 0x1u)) {
    // Without stencil, masks and both face-op descriptors remain zero. Keep
    // z_enable, z_write_enable and zfunc even when depth testing is disabled.
    depth &= 0x76u;
    replace(13, 0);
  } else {
    replace(13, words[13] & 0x00FFFF00u);  // reference is dynamic command state
    if (!(original_depth & 0x80u)) {
      // The builder copies FrontFace into BackFace when backface_enable is off.
      depth &= 0x000FFFFFu;
    }
  }
  if (!words[4]) {
    // UNKNOWN DSV overwrites DepthEnable, but DOES NOT clear depth write/func
    // or any stencil descriptors already populated above. Retain stencil's
    // original enable bit because it controls that descriptor population.
    depth &= ~0x2u;
  }
  replace(10, depth);

  // Polygon offset is already represented independently in words14/15. Only
  // cull_front, cull_back and face are read from setup in GetPipeline. Keeping
  // both cull bits also preserves its early "both faces culled" refusal.
  replace(11, words[11] & 0x7u);
  if (words[5] <= 1) {
    replace(16, words[16] & ~0x2u);  // AlphaToCoverageEnable is explicitly false
  }
  // Formats, sample mask/count, root variant, depth clip, raw float bits and
  // reserved words are intentionally left intact. No hardware-ignored-field
  // assumptions, enum remapping, cross-pass sharing or zero-depth elision.
  return changed != 0;
}

}  // namespace nb::gpu
