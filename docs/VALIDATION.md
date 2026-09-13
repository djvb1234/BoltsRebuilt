# Initial source preview validation

Local checks completed on September 12, 2026:

- Built a fresh recursive checkout of the pinned RexGlue v0.10.0 SDK with the
  seven included patches, then generated game code from the supported local PAL
  extraction and compiled the Windows Release application and both GPU plugins.
- Passed all 33 Python tests for shader translation, dump merging and publication
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
