// ultrawide_hooks.cpp - guest hooks for Hor+ ultrawide, the centred HUD and the camera/FOV debug tools
// (docs/ultrawide.md; the state lives in src/ultrawide/nb_ultrawide.cpp). With nb_ultrawide off and the
// nb_cam_* cvars at their defaults, every hook is a call-through, and the camera snapshot only reads guest
// memory.
//
// Guest facts, cited to generated/default of the 2026-09-10 codegen:
//
// - sub_822ECCD0 (nb_recomp.133.cpp:2766) returns the split-screen factor in f1.
//   - It is the shared numerator of both aspect getters: sub_82239C40 for the camera
//     (nb_recomp.137.cpp:745) and sub_82236148 for the display. Nothing else calls it.
//   - Scaling it widens the projection, the culling frustum and every getter user together.
//     meSetViewport still sees equal camera and display aspects, so it adds no letterbox.
//   - The getters reuse f0, f13 and r7 after the call (nb_recomp.137.cpp:801-870), so only f1 may change.
//
// - rex_XuiRenderSetViewTransform (0x82205C88, nb_recomp.167.cpp:235) tail-calls the XUI device's
//   SetViewTransform.
//   - sub_82208338 calls it at lr 0x8220849C, passing the XUI view diag(rtW/1280, rtH/720) in r4, which
//     points at its r1+128 (nb_recomp.37.cpp:434-481).
//   - It then renders its scene lists and restores the saved view from r1+192 at lr 0x822084D0.
//   - Inside that pass, RenderTopmostObjects (combo-box drop-down lists) sets the topmost element's view
//     (lr 0x829D396C) and then the pass view it saved on entry (lr 0x829D39F0). Both were read back from
//     the device, so they are the squeezed bytes; the XUI setter and getter copy them float by float, so
//     they compare bit-equal.
//   - XUI's render-to-texture runs in a separate pre-render pass and is left alone.
//
// - The glyph renderer sub_82325608 scissors only the inline button icons a string embeds (characters
//   U+E000-U+E00B), in its second loop. Ordinary glyphs are not scissored (nb_recomp.76.cpp:4532-4633).
//   - The icon scissor is the text's bounds (the box RECT unioned with the ordinary glyphs' extents) plus
//     the matrix translation (m30 of world x view x K in r23), with no scale.
//   - It hands the rectangle, at its r1+240, to rex_D3DDevice_SetScissorRect at lr 0x82325C18, and restores
//     the scissor after each icon (0x82325C28).
//
// - rex_meCalculateProjection_822EC7B8 (nb_recomp.157.cpp:2765), with me = 0x82FADB80.
//   - It returns early unless [me+628] != 0 and [me+620] == 0.
//   - Otherwise it reads camera = [me+416] (near at +100, far at +104) and builds the projection at me+64
//     from the aspect and the vertical FOV at me+612, in degrees.
//   - It then sets the viewport at me+588 (a D3DVIEWPORT9, nb_recomp.198.cpp:2832-2879) and builds the
//     frustum with sub_8224F3D0(f1 = vfov, f2 = aspect) at 0x822EC928.
//
// - sub_822150D0 (nb_recomp.22.cpp:435) turns a lens into the vertical FOV in degrees and stores it through
//   r4. The function keeps r4 in r31, because its atan helper clobbers r4 itself.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/system/xmemory.h>

#include "ultrawide/nb_ultrawide.h"
#include "ultrawide/nb_ultrawide_math.h"

REX_EXTERN(__imp__sub_822ECCD0);
REX_EXTERN(__imp__rex_XuiRenderSetViewTransform);
REX_EXTERN(__imp__rex_D3DDevice_SetScissorRect);
REX_EXTERN(__imp__rex_meCalculateProjection_822EC7B8);
REX_EXTERN(__imp__sub_822150D0);
REX_EXTERN(__imp__sub_8224F3D0);

namespace {

constexpr uint32_t kMe = 0x82FADB80;
constexpr uint32_t kXuiViewCallReturn = 0x8220849C;
constexpr uint32_t kXuiViewRestoreReturn = 0x822084D0;
constexpr uint32_t kGlyphScissorCallReturn = 0x82325C18;

// The squeezed XUI pass on this thread runs from the squeeze call (lr 0x8220849C) to the pass's restore
// (lr 0x822084D0). t_hud_squeezed says whether the view set last is the squeezed one: the squeeze call sets
// it, and so does any view inside the pass that is byte-identical to it (RenderTopmostObjects replays it).
// It gates the icon-scissor correction, together with the squeeze that pass used.
thread_local bool t_hud_pass = false;
thread_local bool t_hud_squeezed = false;
thread_local double t_hud_kh = 1.0;
thread_local uint8_t t_squeezed_view[64] = {};

// Guest memory is big-endian.
uint8_t* Guest(uint8_t* base, uint32_t address) { return rex::memory::GuestPtr<uint8_t*>(base, address); }
uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Guest(base, address), sizeof(value));
  return std::byteswap(value);
}
void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t swapped = std::byteswap(value);
  std::memcpy(Guest(base, address), &swapped, sizeof(swapped));
}
float LoadF32(uint8_t* base, uint32_t address) { return std::bit_cast<float>(LoadU32(base, address)); }
void StoreF32(uint8_t* base, uint32_t address, float value) { StoreU32(base, address, std::bit_cast<uint32_t>(value)); }

}  // namespace

// Hor+: scale both aspect getters by k. The original runs first, because it also stores R+256 = 1.
REX_HOOK_RAW(sub_822ECCD0) {
  __imp__sub_822ECCD0(ctx, base);
  const double k = nb::uw::CurrentK();
  if (k != 1.0) {
    ctx.f1.f64 = double(float(ctx.f1.f64 * k));
  }
}

// Centred HUD: squeeze the one XUI view transform the game sets for its HUD, menu and loading passes, so
// the 1280-wide canvas lands in the middle 1/k of the frame at 16:9 proportions.
REX_HOOK_RAW(rex_XuiRenderSetViewTransform) {
  if (ctx.lr == kXuiViewCallReturn) {
    // A new pass. Nothing counts as squeezed until this call applies the squeeze.
    t_hud_pass = t_hud_squeezed = false;
    const nb::uw::FrameState state = nb::uw::Current();
    if (ctx.r4.u32 != 0 && state.active && state.hud_center && state.kh != 1.0) {
      const uint32_t matrix = ctx.r4.u32;
      uint8_t saved[64];
      std::memcpy(saved, Guest(base, matrix), sizeof(saved));
      float row0[4], row3[4];
      for (uint32_t j = 0; j < 4; ++j) {
        row0[j] = LoadF32(base, matrix + 4 * j);
        row3[j] = LoadF32(base, matrix + 48 + 4 * j);
      }
      nb::uw::SqueezeViewRows(row0, row3, state.kh);
      for (uint32_t j = 0; j < 4; ++j) {
        StoreF32(base, matrix + 4 * j, row0[j]);
        StoreF32(base, matrix + 48 + 4 * j, row3[j]);
      }
      __imp__rex_XuiRenderSetViewTransform(ctx, base);
      // The device has copied the view. Keep its bytes, to recognise it when the pass sets it again, then
      // put the caller's stack back exactly as it was.
      std::memcpy(t_squeezed_view, Guest(base, matrix), sizeof(t_squeezed_view));
      std::memcpy(Guest(base, matrix), saved, sizeof(saved));
      t_hud_pass = t_hud_squeezed = true;
      t_hud_kh = state.kh;
      return;
    }
    __imp__rex_XuiRenderSetViewTransform(ctx, base);
    return;
  }
  // Any other view call. The pass's restore ends the pass. Inside it, the correction follows the view that
  // is actually set, so the topmost element's view and the pass view RenderTopmostObjects restores keep it.
  if (ctx.lr == kXuiViewRestoreReturn) t_hud_pass = false;
  t_hud_squeezed = t_hud_pass && ctx.r4.u32 != 0 &&
                   std::memcmp(Guest(base, ctx.r4.u32), t_squeezed_view, sizeof(t_squeezed_view)) == 0;
  __imp__rex_XuiRenderSetViewTransform(ctx, base);
}

// Inline button icons inside the squeezed HUD. Their scissor is the text's bounds plus the element origin,
// with no scale, so under the squeeze it would be 1/kh too wide: icons in a box starting at 0 would escape
// clipping on the right, and icons near the left of an inset box would be cut. The fix scales the
// horizontal extent around the element origin by the same kh. It uses kh rather than the matrix's m00,
// which also carries the element's own scale, so every element keeps exactly the clip it has at 16:9, only
// squeezed. Ordinary glyphs are not scissored.
REX_HOOK_RAW(rex_D3DDevice_SetScissorRect) {
  if (t_hud_squeezed && ctx.lr == kGlyphScissorCallReturn && ctx.r4.u32 != 0 && ctx.r23.u32 != 0) {
    const double origin = LoadF32(base, ctx.r23.u32 + 48);
    if (std::isfinite(origin)) {
      const uint32_t rect = ctx.r4.u32;
      const double left = static_cast<int32_t>(LoadU32(base, rect + 0));
      const double right = static_cast<int32_t>(LoadU32(base, rect + 8));
      StoreU32(base, rect + 0,
               static_cast<uint32_t>(static_cast<int32_t>(std::floor(nb::uw::SqueezeFromOrigin(left, origin, t_hud_kh)))));
      StoreU32(base, rect + 8,
               static_cast<uint32_t>(static_cast<int32_t>(std::ceil(nb::uw::SqueezeFromOrigin(right, origin, t_hud_kh)))));
    }
  }
  __imp__rex_D3DDevice_SetScissorRect(ctx, base);
}

// Camera snapshot for the F10 overlay. It only reads, after the original has run, and only when the
// original actually rebuilt the projection (so the camera pointer it dereferenced is valid).
REX_HOOK_RAW(rex_meCalculateProjection_822EC7B8) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const bool rebuilds = LoadU32(base, kMe + 628) != 0 && LoadU32(base, kMe + 620) == 0;
  __imp__rex_meCalculateProjection_822EC7B8(ctx, base);
  if (!rebuilds) return;
  nb::uw::CameraSnapshot s;
  s.caller = caller;
  s.last_swap = nb::uw::GuestSwapCount();
  s.camera = LoadU32(base, kMe + 416);
  if (s.camera) {
    s.focal = LoadF32(base, s.camera + 96);
    s.near_z = LoadF32(base, s.camera + 100);
    s.far_z = LoadF32(base, s.camera + 104);
    s.camera_mode = static_cast<int32_t>(LoadU32(base, s.camera + 108));
    s.cam_ratio = LoadF32(base, s.camera + 172);
  }
  s.display_mode = static_cast<int32_t>(LoadU32(base, kMe + 616));
  s.vfov_deg = LoadF32(base, kMe + 612);
  s.m00 = LoadF32(base, kMe + 64);
  s.m11 = LoadF32(base, kMe + 84);
  for (uint32_t i = 0; i < 4; ++i) {
    s.viewport[i] = LoadU32(base, kMe + 588 + 4 * i);
  }
  s.k = nb::uw::CurrentK();
  nb::uw::RecordCamera(s);
}

// Debug FOV: rewrite the vertical FOV the lens function produced. That one value feeds the projection,
// the frustum, the pixel focal, LOD and the per-player snapshot consistently. The camera is recalculated
// every frame, so a change shows on the next frame.
REX_HOOK_RAW(sub_822150D0) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_822150D0(ctx, base);
  const double vfov_override = nb::uw::CamVfovOverride();
  const double tan_scale = nb::uw::CamFovScale();
  if (out == 0 || (vfov_override <= 0.0 && tan_scale == 1.0)) return;
  const double game_vfov = LoadF32(base, out);
  const double wanted =
      vfov_override > 0.0 ? vfov_override : nb::uw::ScaledVerticalFovDeg(game_vfov, tan_scale);
  if (!std::isfinite(wanted)) return;
  StoreF32(base, out, static_cast<float>(std::clamp(wanted, 1.0, 170.0)));
}

// Debug culling: replace the frustum aspect (f2) that meCalculateProjection, the only caller, passes.
REX_HOOK_RAW(sub_8224F3D0) {
  const double cull_aspect = nb::uw::CamCullAspect();
  if (cull_aspect > 0.0) {
    ctx.f2.f64 = double(float(cull_aspect));
  }
  __imp__sub_8224F3D0(ctx, base);
}
