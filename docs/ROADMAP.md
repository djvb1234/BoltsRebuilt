# Current priorities

These are areas for testing and development, not promised release dates. The
[compatibility page](COMPATIBILITY.md) records what is already established.

1. **Exercise the public build through gameplay.** Record repeatable tests for
   world entry, challenges, vehicles, audio, controller input and save/reload.
   Keep the tested release and settings with each result.
2. **Collect hardware coverage.** Start with other GPUs, drivers and CPU setups.
   Establish evidence before suggesting minimum specifications or performance
   expectations.
3. **Investigate rendering differences.** Use same-session F5 comparisons to
   narrow problems to native replacements or shared rendering. Improve shader
   translation and fallback handling with focused reproductions.
4. **Make setup easier.** Use first-time tester reports to improve prerequisite
   checks, error messages, shader preparation and the build guide.
5. **Publish repeatable performance measurements.** Record scene, hardware,
   driver, resolution, scale, pacing and shader state. Check image quality and
   timing before promoting optimizations.

Implementation ideas belong in [Discussions](https://github.com/djvb1234/BoltsRebuilt/discussions).
Reproducible failures belong in [Issues](https://github.com/djvb1234/BoltsRebuilt/issues/new/choose).
The public repository remains source-only; a download-and-play game package is
not part of this preview's release plan.
