# BoltsRebuilt

A static recompilation of **Banjo-Kazooie: Nuts & Bolts** for Windows, built on
[RexGlue](https://github.com/rexglue/rexglue-sdk) with a custom native Direct3D 12
renderer, shader translation tools, and ultrawide support.

**Source-only developer preview.** To play, you need the supported PAL base-game
extraction and must build locally. This repository contains no game executable,
assets, captured shaders, or generated game C++.

**[Build and play](docs/BUILDING.md)** · **[Report a playtest issue](https://github.com/djvb1234/BoltsRebuilt/issues/new/choose)** · **[Source provenance](docs/PROVENANCE.md)**

## What is implemented

- A custom GPU plugin that replaces supported guest draws with native D3D12 draws.
- A shader-to-HLSL translator, runtime shader library, background compilation,
  and pipeline caching. Shader inputs come from each tester's own local game.
- Native geometry drawing with guest vertex/index data, up to sixteen vertex
  streams, stencil state, and supported cube-texture fetches.
- Command recording/replay, constant upload, shared-memory, texture lookup,
  render-target transfer and resolve optimizations, with diagnostic controls.
- 21:9 and 32:9 camera support: wider horizontal field of view, matching culling,
  a centered HUD, and configurable internal rendering up to 5120x1440.
- Scripted controller input, frame/camera overlays, CPU/GPU probes, and focused
  renderer correctness tests.

The renderer uses RexGlue's Xenia-derived command-stream and resource
infrastructure. Unsupported shaders or rendering state retain per-draw fallback.
Coverage and performance vary by scene; completing and validating the whole game
remain ongoing work. The internal application/plugin names `nb` and `rexgpu-nb`
are retained for compatibility.

## Try it

Install the Windows C++/Clang build tools listed in the
[build guide](docs/BUILDING.md), then:

```powershell
git clone https://github.com/djvb1234/BoltsRebuilt.git
cd BoltsRebuilt
& .\tools\build-local.ps1 -GameRoot 'D:\Games\NutsAndBolts'
& .\tools\play.ps1 -CaptureShaders
# Close the game after visiting the areas you want to test.
& .\tools\prepare-shaders.ps1
& .\tools\play.ps1
```

The helper checks your executable's identity, downloads a pinned SDK, applies
the included source patches, and generates/builds game code on your machine.
Captures, generated files, builds, saves and game paths stay in ignored local
directories. Native rendering becomes available as you capture and prepare
supported shaders; other draws continue through fallback.

## Feedback

Report the commit, hardware/driver, scene, reproduction steps, aspect ratio and
render scale. F5 toggles native draws for a useful comparison. Please report visual
errors as well as frame rate changes. See [contributing](CONTRIBUTING.md).

Do not attach game files, shader programs/caches, generated game code, saves or
memory dumps. Review log excerpts for personal paths before sharing them.
Screenshots and reproducible benchmark reports will be added as they are prepared;
private-build performance figures are not measurements of this public preview.
The initial [validation record](docs/VALIDATION.md) states what was checked.

## License and credits

Original BoltsRebuilt contributions are licensed under **GPL-3.0-only**;
see [LICENSE](LICENSE). If you distribute a modified version, GPLv3 requires
offering its corresponding source to recipients under the same license. It
allows commercial use and does not require publishing private changes or sending
changes back to this repository.

Pre-existing third-party work retains its own license and copyright notices.
See [third-party notices](THIRD_PARTY_NOTICES.md) for RexGlue, Xenia and the SDL
controller database, and [provenance](docs/PROVENANCE.md) for compatibility data
researched using reNut. The project license grants no rights to the original
game or its trademarks. BoltsRebuilt is not affiliated with or endorsed by Rare
or Microsoft.
