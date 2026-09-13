# Build and test on Windows

This is a source-only developer preview. There is no prebuilt game executable.
You supply a lawfully obtained game extraction and build locally. The build does
not download game data, upload your files, or modify the source extraction.

## Requirements

- Windows x64 and a Direct3D 12 GPU. The compiler preset targets x86-64-v2.
  Other platforms are not supported by this preview. See
  [compatibility](COMPATIBILITY.md) and the [tested hardware](TEST_HARDWARE.md).
- Your PAL base-game extraction, including `default.xex` and `Bundle/`.
- The supported `default.xex` SHA-1 is `5be7c41a37e3fa1e8fa05f4a0815c9b807dcae74`
  (16,920,576 bytes). Other executables and title updates are rejected.
- PowerShell 7, Git, Python 3.11 or newer, and Visual Studio 2022 Build Tools with the C++ tools,
  C++ Clang tools for Windows, CMake tools, and a Windows 10/11 SDK including FXC.
- Disk space for the SDK, generated C++, compiler intermediates and local shader data.
  The first build is substantial; four parallel compiler jobs are the default.

The SDK is pinned to RexGlue v0.10.0, commit
`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`. Its submodules and seven checked-in
patches are fetched/applied by the helper in a separate `.deps/` directory.
An existing SDK checkout is never reset to a different commit.

## Build

Open PowerShell in the repository:

```powershell
git clone https://github.com/djvb1234/BoltsRebuilt.git
cd BoltsRebuilt
& .\tools\build-local.ps1 -GameRoot 'D:\Games\NutsAndBolts'
```

Use your extraction path. The helper verifies its executable hash before preparing
anything, locates the Visual Studio developer tools, builds the code generator,
generates game code locally, reconfigures CMake and builds the application.
If tools are in a nonstandard Visual Studio installation, pass `-VsInstall`.
Use `-Jobs 2` if memory pressure causes compiler failures.

Executable output: `out/build/win-amd64-release/nb.exe`. The internal `nb` name is
retained for runtime and plugin compatibility; the public project is BoltsRebuilt.
Your game path is saved only in ignored `.local/game.json` and the local manifest.

## First play and native shaders

The native renderer needs shaders generated from your own game. Begin with a
capture run; unsupported or unprepared draws use the SDK rendering path:

```powershell
& .\tools\play.ps1 -CaptureShaders
```

Play the areas you want to test and close the game normally. Then translate and
compile the captured shaders locally:

```powershell
& .\tools\prepare-shaders.ps1
& .\tools\play.ps1
```

If `python` is not on PATH, pass `-Python 'C:\path\to\python.exe'` to the shader helper.
It finds FXC in the Windows SDK; a nonstandard installation can use `-Fxc`.
Shader files are checked before installation. Unhandled shader operations keep
their fallback; a successful translation does not establish full-game correctness.

Revisit new areas with `-CaptureShaders` and repeat preparation to extend coverage.
You do not need somebody else's shader dump or cache. Captures and generated
shaders must stay local. The first encounter with new shader pairs can stutter.

## Ultrawide and comparison

```powershell
& .\tools\play.ps1 -Ultrawide '32:9' -Fullscreen
& .\tools\play.ps1 -Ultrawide '32:9' -RenderScale '4x2' -Fullscreen
& .\tools\play.ps1 -Emulated
```

`4x2` renders 5120x1440 internally and is more demanding than the default scale.
`-Emulated` disables native draw replacements within the custom plugin; it is not
an unmodified upstream-Xenia comparison.

- F5 toggles native draws for comparison.
- F8 toggles the FPS overlay.
- F9 switches an enabled ultrawide view against 16:9.
- F10 toggles camera/FOV information.

The default guest refresh setting allows up to 240 FPS; it does not guarantee
that performance. Some frame-counted behavior can speed up. To test original
30 FPS pacing, set `video_mode_refresh_rate = 60.0` in the built application's
`nb.toml`. Defaults are copied after the application links; a no-op rebuild does
not restore them. With the game closed, this command from the repository root
restores the checked-in configuration (replacing any settings you saved there):

```powershell
Copy-Item -LiteralPath '.\config\nb.toml' -Destination '.\out\build\win-amd64-release\nb.toml'
```

The play helper uses `.local/user-data` for this preview's saves and caches, separate
from other local `nb` installations. Back up saves you value before testing software.

## What to report

The [first-playtest guide](FIRST_PLAYTEST.md) gives a short test sequence.
See [troubleshooting](TROUBLESHOOTING.md) for common setup messages and settings.

Use the repository's playtest issue form. Include the commit, CPU/GPU and driver,
scene and steps, aspect/render scale, and whether the issue also happens with F5
native rendering disabled. Warm shaders before comparing frame rates. Numbers
from different scenes or camera positions are not a controlled comparison.

Review any log excerpt before posting: paths can identify your machine. Do not attach
game files, shader programs/caches, generated C++, saves or crash memory dumps.

## Preview limitations

- Native coverage is scene-dependent; unsupported draws retain fallback.
- Experimental hand-transcribed game-specific shader passes are excluded. The
  generic shader pipeline and fallback are the supported public paths.
- Optional offline asset packs and the private research/benchmark corpus are not
  supplied or required by this starter workflow.
- Historical private-build performance figures are not validation of this preview.
- A passing file scan is not legal clearance. See [provenance](PROVENANCE.md).
