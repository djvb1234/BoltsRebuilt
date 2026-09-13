// hooks_m1.cpp - Milestone-1 policy hooks for nb.exe (adopt as src/hooks/hooks_m1.cpp).
//
// Every hook is gated by a cvar and, with the cvar off, is a pure call-through to the
// original recompiled body (__imp__<name>). Guest facts below are cited to
// generated/default/nb_recomp.N.cpp line numbers of the 2026-09-06 codegen output.
//
// SDK declarations relied on (RexGlue SDK v0.10.0):
//   include/rex/hook.h:49            REX_HOOK_RAW(name) -> extern "C" REX_FUNC(name)
//   include/rex/ppc/func.h:45-46     REX_FUNC / REX_EXTERN
//   include/rex/cvar.h:340,343,356   REXCVAR_DECLARE / REXCVAR_GET / REXCVAR_DEFINE_BOOL
//   include/rex/logging/macros.h:51-56  REXLOG_INFO (core category, like heap_hooks.cpp)
//   include/rex/system/xmemory.h:56-58  rex::memory::GuestPtr<T>(base, guest_addr)
//   include/rex/ppc/context.h:43-55,213 rex::ppc::Register (.u32/.s32/.u64) = PPCRegister

#include "hooks_m1.h"

#include <bit>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(nb_m1_force_single_tile, false, "nb",
                    "Force the predicated-tiling tile count (sub_8222A990 result) to 1; set at boot "
                    "and pair with nb_m1_disable_scene_msaa (a single MSAA tile overflows EDRAM)");
REXCVAR_DEFINE_BOOL(nb_m1_disable_scene_msaa, false, "nb",
                    "Zero the MultiSample argument at the three scene-surface sites in sub_823ED2A8 "
                    "(0x823ED544, 0x823ED594, 0x823ED6E4); set at boot");
REXCVAR_DEFINE_BOOL(nb_m1_log_tiling, false, "nb",
                    "Log rex_D3DDevice_BeginTiling / rex_D3DDevice_EndTiling arguments and tile rects");
REXCVAR_DEFINE_BOOL(nb_m1_log_swap, false, "nb", "Log the rex_D3DDevice_Swap call rate once per second");

namespace {

// Guest memory is big-endian. Same arithmetic as REX_LOAD_U32 / REX_STORE_U32 in the
// generated code (pch_h.inja:135-144), volatile included.
int32_t GuestLoadS32(uint8_t* base, uint32_t guest_addr) {
  const volatile int32_t* p = rex::memory::GuestPtr<const volatile int32_t*>(base, guest_addr);
  return std::byteswap(static_cast<int32_t>(*p));
}

// D3DRECT {x1, y1, x2, y2}, 16-byte stride. Layout verified from the BeginTiling body
// (nb_recomp.58.cpp:1296-1400: it copies the four dwords at +0/+4/+8/+12 into device+12752
// and keeps the running maxima of +8 and +12 as the tiled extent at device+13180/13184) and
// from the builder loop in sub_823ED2A8 (nb_recomp.96.cpp:6120-6165: rect i =
// {0, y_off, width, y_off + band_height}), i.e. the tiles are horizontal bands.
struct GuestRect {
  int32_t x1, y1, x2, y2;
};

GuestRect GuestLoadRect(uint8_t* base, uint32_t rect_addr) {
  return {GuestLoadS32(base, rect_addr + 0), GuestLoadS32(base, rect_addr + 4),
          GuestLoadS32(base, rect_addr + 8), GuestLoadS32(base, rect_addr + 12)};
}

void LogTilingRects(uint8_t* base, uint32_t rect_array, uint32_t count) {
  // Cap the dump so a corrupt count cannot flood the log.
  const uint32_t shown = count < 4 ? count : 4;
  for (uint32_t i = 0; i < shown; ++i) {
    const GuestRect r = GuestLoadRect(base, rect_array + i * 16);
    REXLOG_INFO("nb_m1:   rect[{}] = ({}, {}) - ({}, {})", i, r.x1, r.y1, r.x2, r.y2);
  }
  if (count > shown) {
    REXLOG_INFO("nb_m1:   ... {} more rects not shown", count - shown);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Hook 1: sub_8222A990, the tile-count lookup (0x8222A990, nb_recomp.24.cpp:614-687).
//
// Why the callee and not its callers: the function is a leaf that reads two globals
// (resMode = R+180 = 0x82F9EBE4, msaaMode = R+232 = 0x82F9EC18, R = 0x82F9EB30) and returns
// 1, 2 or 4 in r3; r3 on entry is never read. It has 17 call sites in 12 functions
// (sub_82278C08, sub_8233D558, sub_82359B50, sub_8233FA48, sub_823BA3A8, sub_8222ADA0,
// sub_82362400, sub_8233EEE0, sub_8233E1C0, sub_823DBEB8, sub_8233C668, sub_823ED2A8); one
// hook covers all of them.
//
// The stored count: sub_82278C08 (nb_recomp.101.cpp:1404-1409) stores the result at
// R-4 = 0x82F9EB2C right after the call and calls BeginTiling only when it is > 1;
// EndTiling's single caller sub_8227A9F0 (nb_recomp.131.cpp:1363-1367) reads the same word
// and skips EndTiling when it is <= 1. That store is the only writer of 0x82F9EB2C in the
// image, so forcing the return value here also forces the stored count.
//
// MSAA pairing: sub_823ED2A8 sizes the scene surfaces per tile band (R+164 = band height,
// nb_recomp.96.cpp:6166-6178) and the game runs 2 bands of 1280x384 at 2x (768 EDRAM tiles
// each for color and depth; the glow scratch sits right behind them at 1536). One 1280x720
// band at 2x needs 1440 + 1440 = 2880 tiles; the SDK's EDRAM is 2048 tiles and wraps
// (src/graphics/pipeline/render_target/cache.cpp:199, xenos::kEdramTileCount), so the depth
// surface would alias the color surface. Ship nb_m1_force_single_tile only together with
// nb_m1_disable_scene_msaa (1x: 720 + 720 = 1440 tiles).
// ---------------------------------------------------------------------------

REX_EXTERN(__imp__sub_8222A990);

REX_HOOK_RAW(sub_8222A990) {
  __imp__sub_8222A990(ctx, base);
  if (!REXCVAR_GET(nb_m1_force_single_tile)) {
    return;
  }
  const uint32_t original_count = ctx.r3.u32;
  if (original_count <= 1) {
    return;
  }
  ctx.r3.u64 = 1;
  // Log only on change; the lookup runs several times per frame. Benign race: a lost
  // update here costs one duplicate log line, nothing else.
  static uint32_t last_logged = 0;
  if (last_logged != original_count) {
    last_logged = original_count;
    REXLOG_INFO("nb_m1: tile count forced {} -> 1 (nb_m1_force_single_tile)", original_count);
  }
}

// ---------------------------------------------------------------------------
// Hook 2: three mid-asm hooks inside sub_823ED2A8 (nb_recomp.96.cpp:5905-6600).
//
// Each runs immediately before the `bl` at its address, after r6 was selected from
// msaaMode/resMode (ladders at nb_recomp.96.cpp:6244-6262, 6296-6312, 6480-6500:
// msaaMode 2 -> r6 = 2; msaaMode 1 -> r6 = 1, or 2 when resMode == 0; else r6 = 0) and
// before anything else touches r6. The two later CreateSurface calls at 0x823ED5EC and
// 0x823ED708 already pass `li r6,0` (nb_recomp.96.cpp:6365, 6520) and are not hooked.
// ---------------------------------------------------------------------------

void nb_msaa_off_color_size(PPCRegister& r6) {
  if (!REXCVAR_GET(nb_m1_disable_scene_msaa)) {
    return;
  }
  // Runs only while the scene surfaces are (re)created, so log-once is enough.
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("nb_m1: MSAA off at 0x823ED544 (XGSurfaceSize of the depth footprint that becomes "
                "the color surface EDRAM base), r6 was {}", r6.u32);
  }
  r6.u64 = 0;
}

void nb_msaa_off_color_surface(PPCRegister& r6) {
  if (!REXCVAR_GET(nb_m1_disable_scene_msaa)) {
    return;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("nb_m1: MSAA off at 0x823ED594 (color CreateSurface), r6 was {}", r6.u32);
  }
  r6.u64 = 0;
}

void nb_msaa_off_depth_surface(PPCRegister& r6) {
  if (!REXCVAR_GET(nb_m1_disable_scene_msaa)) {
    return;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("nb_m1: MSAA off at 0x823ED6E4 (depth CreateSurface), r6 was {}", r6.u32);
  }
  r6.u64 = 0;
}

// ---------------------------------------------------------------------------
// Hook 3: rex_D3DDevice_BeginTiling (0x822788A0, nb_recomp.58.cpp:1296) and
// rex_D3DDevice_EndTiling (0x82212328, nb_recomp.82.cpp:309) log-only wrappers.
//
// Registers the bodies actually read:
//   BeginTiling: r3 = device (stored as r31), r5 = tile count (stored to device+12748),
//     r6 = rect array (16-byte D3DRECTs, copied to device+12752), r7 = 16-byte clear
//     vector (kept in r29, lvx128 later). r4, r8, r9 and f1 are overwritten before any
//     read, so the caller's r4 = 0 / r9 = 0 / f1 are not consumed.
//   EndTiling: r3 = device, r6 = destination texture/header (kept in r27, null-checked),
//     r7 = clear vector (r26). The caller's r4 = 0x300 and r5 = 0 are never read; the
//     body sets its own `li r4,768` for the resolve it issues.
//   The single callers are sub_82278C08 (nb_recomp.101.cpp:1451-1453, r6 = 0x82F9EB50)
//   and sub_8227A9F0 (nb_recomp.131.cpp:1397-1399, r6 = 0x82F83268).
//
// No tile-collapse at BeginTiling: the scene surfaces are created band-sized
// (1280x384 for 2 bands), so a single union rect (0,0)-(1280,720) would overrun the
// color surface into the depth surface's EDRAM range. The only sound single-tile
// lever is hook 1 at boot, which also makes sub_823ED2A8 create full-frame surfaces.
// ---------------------------------------------------------------------------

REX_EXTERN(__imp__rex_D3DDevice_BeginTiling);
REX_EXTERN(__imp__rex_D3DDevice_EndTiling);

REX_HOOK_RAW(rex_D3DDevice_BeginTiling) {
  if (REXCVAR_GET(nb_m1_log_tiling)) {
    REXLOG_INFO("nb_m1: BeginTiling device={:#x} r4={} tiles={} rects={:#x} clear={:#x}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    if (ctx.r6.u32 != 0 && ctx.r5.u32 > 0) {
      LogTilingRects(base, ctx.r6.u32, ctx.r5.u32);
    }
  }
  __imp__rex_D3DDevice_BeginTiling(ctx, base);
}

REX_HOOK_RAW(rex_D3DDevice_EndTiling) {
  if (REXCVAR_GET(nb_m1_log_tiling)) {
    REXLOG_INFO("nb_m1: EndTiling device={:#x} r4={:#x} r5={} dest={:#x} clear={:#x}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  }
  __imp__rex_D3DDevice_EndTiling(ctx, base);
}

// ---------------------------------------------------------------------------
// Hook 4: Swap observer. The strong rex_D3DDevice_Swap lives in swap_hook.cpp (one per
// executable); this only counts calls. Arguments (nb_recomp.131.cpp:1505-1530): r3 = device
// (r31), r4 = front-buffer texture (r28), r5 = optional pointer (r24, null-checked later).
// ---------------------------------------------------------------------------

void nb_m1_swap_enter(PPCContext& ctx) {
  if (!REXCVAR_GET(nb_m1_log_swap)) {
    return;
  }
  using clock = std::chrono::steady_clock;
  static uint64_t frame_count = 0;
  static clock::time_point window_start = clock::now();
  ++frame_count;
  const auto now = clock::now();
  const double elapsed = std::chrono::duration<double>(now - window_start).count();
  if (elapsed >= 1.0) {
    REXLOG_INFO("nb_m1: Swap device={:#x} frontbuffer={:#x}, {:.1f} swaps/s over {} frames",
                ctx.r3.u32, ctx.r4.u32, frame_count / elapsed, frame_count);
    frame_count = 0;
    window_start = now;
  }
}
