// hooks_m1.h - Milestone-1 policy hooks for nb.exe (RexGlue SDK v0.10.0).
//
// Adopt as src/hooks/hooks_m1.h. Hook inventory:
//   1. sub_8222A990 (tile-count lookup, 0x8222A990)      whole-function hook, force the result to 1
//   2. sub_823ED2A8 +0x29C / +0x2EC / +0x43C             three mid-asm hooks, zero r6 (MultiSample)
//   3. rex_D3DDevice_BeginTiling / rex_D3DDevice_EndTiling  log-only wrappers
//   4. rex_D3DDevice_Swap                                 ONE strong wrapper in swap_hook.cpp; this
//                                                         TU only contributes nb_m1_swap_enter()
//
// Binding model (verified against the SDK sources and by check_N/linktest):
//   - DEFINE_REX_FUNC(name) emits a strong body `__imp__name` and a weak alias `name`
//     (resources/templates/codegen/pch_h.inja:81-85). nb_init.cpp's PPCFuncMappings[] and
//     nb_register.cpp's SetFunction() both take `&name`, so a strong `name` defined in any
//     nb.exe object replaces every guest call, direct or through the table. The original stays
//     reachable as `__imp__name` (REX_EXTERN, include/rex/ppc/func.h:46).
//   - The generated objects link straight into nb.exe (OBJECT library nb_recomp), and hook
//     TUs are listed in NB_SOURCES, so no link anchor is needed.
//   - Generated symbol names come from config/*.toml: 0x822788A0 = rex_D3DDevice_BeginTiling,
//     0x82212328 = rex_D3DDevice_EndTiling, 0x82287590 = rex_D3DDevice_Swap (config/gpu_funcs.toml
//     lines 22, 23, 95); 0x8222A990 is unnamed, hence sub_8222A990 (nb_register.cpp:1007).
//     Never re-pin these names from config/hooks.toml: [functions] entries are merged with
//     insert_or_assign (src/codegen/config.cpp:216) and hooks.toml is the LAST include in
//     nb_manifest.toml, so a pin there would rename the generated symbols.
//   - Mid-asm hooks: the codegen emits `extern void name(PPCRegister& r6);` and calls
//     `name(ctx.r6);` immediately BEFORE the instruction at `address`
//     (src/codegen/function_graph.cpp:413-448 and 577-587; r6 is an argument register and is
//     never localized, src/codegen/builders/context.cpp:51-59). Definitions below must keep
//     C++ linkage and PPCRegister& parameters.

#pragma once

#include <rex/cvar.h>
#include <rex/ppc/context.h>

// ---------------------------------------------------------------------------
// Cvars (defined in hooks_m1.cpp, category "nb" like nb_audio in src/nb_app.h).
// All default to false: every hook is then a pure call-through.
//
// nb_m1_force_single_tile and nb_m1_disable_scene_msaa must be set BEFORE the
// scene surfaces are created (config/nb.toml, read at boot). sub_823ED2A8 sizes
// the scene surfaces to one tile band (R+164 = band height) using the same
// lookup, so toggling the tile count at runtime leaves band-sized surfaces
// behind until the game re-creates them.
// ---------------------------------------------------------------------------

// Force the tile count returned by sub_8222A990 to 1.
REXCVAR_DECLARE(bool, nb_m1_force_single_tile);
// Zero r6 (MultiSample) at the three scene-surface sites in sub_823ED2A8.
REXCVAR_DECLARE(bool, nb_m1_disable_scene_msaa);
// Log rex_D3DDevice_BeginTiling / rex_D3DDevice_EndTiling arguments and rects.
REXCVAR_DECLARE(bool, nb_m1_log_tiling);
// Log the rex_D3DDevice_Swap call rate once per second.
REXCVAR_DECLARE(bool, nb_m1_log_swap);

// ---------------------------------------------------------------------------
// Mid-asm hook entry points (config/hooks.toml [[midasm_hook]] entries).
// ---------------------------------------------------------------------------

// 0x823ED544: before `bl sub_8231A808` (XGSurfaceSize) called with the DEPTH format
// 0x1A220197; its result is stored to r1+112 = D3DSURFACE_PARAMETERS.Base of the color
// surface created at 0x823ED594, i.e. the color surface is placed after the depth surface
// in EDRAM. Zeroing r6 here keeps that base equal to the 1x depth footprint.
void nb_msaa_off_color_size(PPCRegister& r6);
// 0x823ED594: before `bl rex_D3DDevice_CreateSurface`, color scene surface (0x18280186).
// r6 = MultiSample. reNut's disable_msaa_color site.
void nb_msaa_off_color_surface(PPCRegister& r6);
// 0x823ED6E4: before `bl rex_D3DDevice_CreateSurface`, depth scene surface (0x1A220197).
// r6 = MultiSample. reNut's disable_msaa_depth site.
void nb_msaa_off_depth_surface(PPCRegister& r6);

// ---------------------------------------------------------------------------
// Swap observer, called by the single rex_D3DDevice_Swap wrapper in swap_hook.cpp
// before the original body runs.
// ---------------------------------------------------------------------------
void nb_m1_swap_enter(PPCContext& ctx);
