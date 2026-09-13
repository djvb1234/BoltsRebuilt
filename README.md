# BoltsRebuilt

**Banjo-Kazooie: Nuts & Bolts on Windows, with native Direct3D 12 rendering and ultrawide support.**

BoltsRebuilt is a static recompilation project built on
[RexGlue](https://github.com/rexglue/rexglue-sdk). It includes the host application,
a custom GPU plugin, shader translation tools, game compatibility hooks, and
the profiling tools used to develop them.

[Preview release](https://github.com/djvb1234/BoltsRebuilt/releases/tag/v0.1.0-preview.1) · [Build and play](docs/BUILDING.md) · [First playtest](docs/FIRST_PLAYTEST.md) · [Screenshots](docs/SCREENSHOTS.md) · [Discussions](https://github.com/djvb1234/BoltsRebuilt/discussions)

[![Showdown Town at sunset, rendered across a 32:9 display with a centered HUD](docs/screenshots/showdown-town-32x9.png)](docs/screenshots/showdown-town-32x9.png)

*Showdown Town at 5120 × 1440, with 32:9 framing and the HUD kept at its original
proportions. Development capture; see the [gallery notes](docs/SCREENSHOTS.md)
for dates and configuration.*

## The implementation

The renderer replaces supported guest draws with native D3D12 draws, using
shaders translated from your own game. It builds on RexGlue's Xenia-derived
command-stream and resource infrastructure, with per-draw fallback for unsupported
shaders and rendering state.

| Area | Included work |
| --- | --- |
| Native renderer | Geometry, vertex/index streams, stencil state, texture and sampler bindings, shader libraries, and pipeline caching. |
| Shader translation | Local shader capture, translation to HLSL, stage validation, background compilation, and fallback when a shader is unsupported. |
| Ultrawide | 21:9 and 32:9 views, wider horizontal field of view, matching culling, a centered HUD, and configurable internal render scale. |
| Runtime integration | Game compatibility hooks, controller input, audio integration, and local save/cache paths. |
| Development tools | Scripted controller runs, frame and camera overlays, CPU/GPU timing probes, and focused correctness tests. |

Command recording and replay, memory uploads, texture lookups, render-target
transfers, and resolves are also active areas of optimization.
The [architecture guide](docs/ARCHITECTURE.md) explains how the pieces fit together.

## In game

| Showdown Town | After dark |
| --- | --- |
| [![Driving through Showdown Town beside Mumbo's Motors](docs/screenshots/town-street.png)](docs/screenshots/town-street.png) | [![A lit world portal in Showdown Town at night](docs/screenshots/town-at-night.png)](docs/screenshots/town-at-night.png) |

The [screenshot gallery](docs/SCREENSHOTS.md) includes the ultrawide comparison,
a closer HUD view, and the world-entry interface. These are real development
captures. Frame-rate overlays are individual observations, not benchmark results
for the public preview.

## Build and play

This is a **source-only developer preview**. You provide the supported game
extraction and build locally; game files and a prebuilt game executable are not
distributed here.

Start with [v0.1.0-preview.1](https://github.com/djvb1234/BoltsRebuilt/releases/tag/v0.1.0-preview.1)
for a named version to test. Its release notes describe the included source,
requirements and known limits. The generated source archives are not a ready-to-run game.

| Requirement | Current target |
| --- | --- |
| Platform | Windows x64 with a Direct3D 12 GPU |
| Game | PAL base-game extraction; title updates are not supported |
| Tools | PowerShell 7, Git, Python 3.11+, Visual Studio 2022 C++/Clang tools, and a Windows SDK |
| Build dependency | RexGlue v0.10.0, downloaded at a pinned revision by the helper |

Follow the [full build guide](docs/BUILDING.md) for the required components and
supported executable hash. Once the tools are installed:

```powershell
git clone https://github.com/djvb1234/BoltsRebuilt.git
cd BoltsRebuilt
& .\tools\build-local.ps1 -GameRoot 'D:\Games\NutsAndBolts'
& .\tools\play.ps1 -CaptureShaders
```

Visit the areas you want to test, then close the game. Prepare those shaders
and launch with native rendering enabled:

```powershell
& .\tools\prepare-shaders.ps1
& .\tools\play.ps1
```

For a 32:9 display:

```powershell
& .\tools\play.ps1 -Ultrawide '32:9' -Fullscreen
```

Generated game code, shaders, builds, saves, and local game paths stay in ignored
directories. Unsupported draws continue through fallback as you build up your
local shader library.

## Project status and feedback

The initial source preview builds on Windows and has passed title-screen launch
checks. Full-game compatibility, native rendering coverage, and performance
testing remain ongoing. The [validation record](docs/VALIDATION.md) lists the
checks performed and their limits.

The [12 September development playtest](docs/benchmarks/2026-09-12-ultrawide/README.md)
records the maintainer's reported early-level and Mumbo mission progress, with
16 minutes of telemetry at native 5120 × 1440. It averaged **70.02 FPS game swap
cadence across the whole session**, including menus and loading. The report
includes a timeline, downloadable samples and settings; it used a private
development build with local caches, so it is separate from public-preview validation.

The maintainer's test machine is an **Intel Core Ultra 9 285K, NVIDIA GeForce RTX
5070 Ti and 32 GB RAM**, running Windows 11 with a 5120 x 1440 display.
[Full hardware and driver details](docs/TEST_HARDWARE.md) describe that configuration;
they are not minimum requirements. [Compatibility and known limitations](docs/COMPATIBILITY.md)
separates verified checks from development captures and areas that still need testing.
Other hardware is untested in the published record, not presumed incompatible.

Playtest reports are useful even when something works. Include your commit,
hardware and driver, scene, aspect ratio, and render scale. For rendering problems,
use **F5** to compare with native draws disabled and describe what changes.
The [issue forms](https://github.com/djvb1234/BoltsRebuilt/issues/new/choose) guide
you through the details. See [contributing](CONTRIBUTING.md) before attaching logs
or submitting code.

For questions and suggestions, use [Discussions](https://github.com/djvb1234/BoltsRebuilt/discussions).
The [current priorities](docs/ROADMAP.md) describe where further testing and development can help.

## Documentation

- [Build and play](docs/BUILDING.md) — installation, local shaders, controls, and ultrawide settings.
- [First playtest](docs/FIRST_PLAYTEST.md) — a short test session and what to report.
- [Troubleshooting](docs/TROUBLESHOOTING.md) — setup messages, shaders, settings and pacing.
- [Compatibility](docs/COMPATIBILITY.md) — verified checks, known limits and untested areas.
- [Test hardware](docs/TEST_HARDWARE.md) — the maintainer's machine and graphics driver.
- [Development playtest](docs/benchmarks/2026-09-12-ultrawide/README.md) — partial progress and a native 5120 × 1440 performance recording.
- [Screenshot gallery](docs/SCREENSHOTS.md) — captures, aspect comparison, and image provenance.
- [Architecture](docs/ARCHITECTURE.md) — runtime, rendering paths, and source layout.
- [Validation](docs/VALIDATION.md) — what has been tested for the source preview.
- [Source provenance](docs/PROVENANCE.md) — origins of the code and compatibility data.
- [Current priorities](docs/ROADMAP.md) — testing and development work to focus on next.

## License and credits

Original BoltsRebuilt contributions are licensed under **GPL-3.0-only**;
see [LICENSE](LICENSE). Distributed modified versions must make their corresponding
source available under GPLv3. Commercial use is allowed; private changes do not
have to be published.

The project builds on work by the **RexGlue**, **Xenia**, and **SDL GameControllerDB**
contributors, with compatibility research informed by **reNut**. Third-party code
retains its own notices and licenses; see [third-party notices](THIRD_PARTY_NOTICES.md)
and [source provenance](docs/PROVENANCE.md).

Banjo-Kazooie: Nuts & Bolts and the game content visible in screenshots belong to
their respective rights holders. The source-code license does not license that
content or its trademarks. BoltsRebuilt is not affiliated with or endorsed by
Rare or Microsoft.
