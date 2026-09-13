# Compatibility and known limitations

This page separates evidence for the public source preview from private
development sessions. It is not a list of hardware presumed to fail. The only
documented test machine is the maintainer's
[Core Ultra 9 285K / RTX 5070 Ti system](TEST_HARDWARE.md).

## Current evidence

| Area | Status | What has been established |
| --- | --- | --- |
| Windows Release build | Verified on the documented machine | Fresh pinned SDK, code generation from the supported PAL extraction, application and GPU plugin build. |
| Title screen with shader capture | Verified on the documented machine | The public build rendered the title screen using fallback while capturing shaders. |
| Title screen with native draws enabled | Verified with a small local shader library | Three vertex and three pixel stages were installed; other draws retained fallback. |
| Showdown Town and 32:9 screenshots | Development evidence | The gallery shows private development sessions. It does not establish equivalent coverage or performance in this public preview. |
| Early-level and Mumbo mission progress at native 5120 × 1440 | Maintainer-reported development playtest | [12 September partial session](benchmarks/2026-09-12-ultrawide/README.md): reported progress, 16 minutes of telemetry and a normal process exit. Private build with local shaders/caches; named missions and completion were not independently checked. |
| Full game, all worlds and challenges | Not established | A complete public-build playthrough has not been recorded. |
| Save/reload across extended sessions | Not established | No public-preview regression record yet. |
| Other hardware and driver combinations | Not established | Reports on other NVIDIA, AMD and Intel GPUs, CPUs and laptops are needed. |
| Performance targets or minimum hardware | Not established | No reproducible public-preview benchmark or minimum specification is available. |

The [validation record](VALIDATION.md) explains the checks and their limits.
Source tests and a successful build do not establish graphical correctness or
full gameplay compatibility.

## Supported inputs and platform

- The preview targets **Windows x64 with Direct3D 12**. Linux, macOS and native
  ARM64 builds are not supported by the published workflow.
- The Windows compiler preset targets **x86-64-v2**. A processor below that
  instruction-set baseline is outside the build target; no minimum CPU model
  has been measured.
- Only the PAL base executable with SHA-1
  `5be7c41a37e3fa1e8fa05f4a0815c9b807dcae74` is accepted. Other regions,
  modified executables and title updates are outside this preview's supported
  inputs. This is an input restriction, not a claim that those versions can
  never work.
- The native generic draw path depends on the SDK's bindless resource path,
  which requires D3D12 Resource Binding Tier 2 or higher. When that path is
  unavailable or disabled, native replacements decline draws. This does not
  establish that the fallback renderer will work on every such adapter.

## Rendering and timing limitations

**Native coverage varies by scene.** Shaders must be captured and prepared from
the user's own game. Unsupported shader operations, control flow or draw state
can fall back. Successful translation is not a complete visual correctness test.
Use F5 within the same session when reporting a difference.

**The public workflow differs from the private development setup.** Optional
offline asset packs, pre-generated shaders and earlier hand-transcribed shader
passes are not supplied. The corresponding optional passes decline draws in this
preview. The normal workflow uses generic translation and fallback.

**High frame rates can affect game timing.** The checked-in guest refresh setting
is 480 Hz, which permits up to 240 FPS because the game normally presents every
second vblank. Some frame-counted behavior can run faster. For original 30 FPS
pacing, set `video_mode_refresh_rate = 60.0` in the built application's `nb.toml`
while the game is closed. This setting is a cap, not a performance guarantee.

**Stored settings can affect later tests.** F4's save-to-config operation persists
settings beside the executable. Rebuilding does not necessarily restore them if
the application does not relink. Record deliberate setting changes; the
[troubleshooting guide](TROUBLESHOOTING.md) explains how to restore defaults.

**Shader warmup can stutter.** Report cold compilation separately from steady
play. Do not compare frame-rate numbers from different scenes or times of day.

## How reports become compatibility evidence

Open a [playtest report](https://github.com/djvb1234/BoltsRebuilt/issues/new?template=playtest.yml)
with the release/commit, hardware, driver, scene, settings and reproduction steps.
Working scenes and unsuccessful tests are both useful. A failure on one machine
does not by itself establish a vendor-wide incompatibility.

When adding an entry to this page, link the report and describe its actual scope:
for example, title screen reached, one challenge completed, or save/reload tested.
Keep user reports distinct from a maintainer reproduction, and label untested
areas explicitly. Start with [your first playtest](FIRST_PLAYTEST.md).
