# BoltsRebuilt

A static recompilation of Banjo-Kazooie: Nuts & Bolts for PC, built on RexGlue with a custom native Direct3D 12 renderer, `rexgpu-nb`.

The project includes a working rendering implementation, shader translation tooling, ultrawide support, and extensive performance and correctness work. This public repository is being prepared for the source release; the implementation and supporting material are being organized for publication.

## Native renderer

The custom GPU plugin replaces supported guest draws with native D3D12 draws. Its implementation includes:

- A shader-to-HLSL translation pipeline and a runtime shader library with background compilation and pipeline caching.
- Native geometry rendering, including guest vertex and index data, up to sixteen vertex streams, and supported cube-texture fetches.
- Native fullscreen and post-processing passes, including bloom and depth-of-field work.
- Optimizations to command recording and replay, constant and geometry uploads, resource residency, texture lookups, and render-target transfers and resolves.
- Optional local geometry and texture caches, with validation and fallback paths.

The renderer builds on RexGlue's Xenia-derived graphics infrastructure. It retains command-stream parsing, shared resource handling, and per-draw fallback for unsupported shaders or rendering state. Native coverage varies by scene and configuration; full native coverage across the game is still a development goal.

## Ultrawide

Implemented support includes 21:9 and 32:9 aspect ratios, with 5120×1440 gameplay checks documented locally:

- A wider horizontal field of view with the original vertical field of view preserved.
- Culling that follows the wider camera view.
- A centered HUD with its original proportions.
- Configurable internal render scale, including native 5120×1440 output.
- Live aspect-ratio switching and camera/FOV diagnostics.

## Development and validation

The project includes scripted gameplay and camera runs, screenshot capture, same-run performance comparisons, CPU/GPU timing probes, and focused correctness checks. Benchmark results will be published with their scene, configuration, hardware, and measurement method.

The current implementation targets the PAL base-game executable and Windows/D3D12. Unsupported rendering paths and performance bottlenecks remain active work.

## Publication status

Source code, build instructions, attribution, benchmark evidence, and ultrawide screenshots are being prepared for publication. This initial repository is not yet a downloadable or buildable game release.

Game images, game executables, extracted assets, and local research archives are not included.
