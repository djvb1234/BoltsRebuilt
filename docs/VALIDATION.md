# Public preview validation

The current test machine is documented in [Test hardware](TEST_HARDWARE.md).
Both recorded public launch checks selected the NVIDIA GeForce RTX 5070 Ti.
See [compatibility](COMPATIBILITY.md) for coverage beyond these checks.

## Initial build and launch checks

Local checks completed on September 12, 2026:

- Built a fresh recursive checkout of the pinned RexGlue v0.10.0 SDK with the
  seven included patches, then generated game code from the supported local PAL
  extraction and compiled the Windows Release application and both GPU plugins.
- Passed the then-current 33 Python tests for shader translation, dump merging and publication
  checks. Parsed the PowerShell helpers without syntax errors.
- Translated, compiled with FXC and installed a six-stage local shader sample
  (three vertex and three pixel stages) through the shader preparation helper.
- Reached the rendered title screen in capture mode and again in native-enabled
  mode with that small local library. The library is intentionally incomplete;
  these launch checks depend on fallback and do not measure full native coverage.
- Checked the staged source contents for blocked game/build artifacts and common
  credential patterns, and passed Git's whitespace check.

This is a build/setup and launch check, not a complete-game playthrough, a
performance benchmark, an exhaustive shader-library check or legal clearance.
The local input game, generated C++, binaries, shader samples, caches and test
logs are excluded from the public repository. See [provenance](PROVENANCE.md).

## Tagged preview and ongoing source checks

The `v0.1.0-preview.1` preparation adds tester documentation, hardware and
compatibility records, issue routing and Windows helper checks. It does not
change runtime code or establish additional gameplay coverage beyond the checks
above. The runtime source is the implementation published in commit `f6d66d8`.

The Python suite now contains **55 tests**, including the reviewed screenshot
publication checks. The synthetic PowerShell shader fixture checks argument
handling, deliberately rejected compilation results, installation and rollback;
it does not use real game shaders or FXC.

[GitHub Actions](https://github.com/djvb1234/BoltsRebuilt/actions/workflows/source-checks.yml)
runs publication, Python and whitespace checks on Linux. A Windows job runs the
synthetic shader fixture and compiles/runs the standalone ultrawide parser/math
checks. These jobs need no game files, SDK checkout, GPU or uploaded caches.
Their result is source/tool validation, not a full application build or a gameplay
compatibility result. Other standalone C++ tests are not yet all wired into CI.

## Separate development playtest

The [12 September 2026 partial playtest](benchmarks/2026-09-12-ultrawide/README.md)
records the maintainer's reported early-level and Mumbo mission progress in a
private development build, with native rendering enabled at 5120 × 1440.
The 16-minute recording includes a performance timeline, numerical samples,
hardware, runtime settings and binary hashes. The process exited with code 0;
runtime fallback warnings and unimplemented queries were still present.

This extends the documented development evidence. It does not extend the tagged
public preview's launch validation, establish full-game compatibility, or assign
performance measurements to individual missions.
