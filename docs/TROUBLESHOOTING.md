# Troubleshooting

Start with the first relevant error, the command you ran and the commit you
built. These notes cover checks and behavior implemented by the public helpers.
They do not establish a list of incompatible GPUs; see
[compatibility and known limitations](COMPATIBILITY.md) for reported coverage.

## Build and game setup

| Message or symptom | What to check |
| --- | --- |
| PowerShell reports that the script requires version 7.0 | Open PowerShell 7. The helpers require it; the older Windows PowerShell is not sufficient. |
| `Select the extracted game folder containing default.xex.` | Point `-GameRoot` at the extraction folder itself. It must contain `default.xex` and `Bundle/`. |
| `The extracted Bundle directory is missing.` | Check that your game extraction is complete and that `-GameRoot` selects the correct folder. |
| `Unsupported executable` or `Unsupported game executable.` | Compare your executable's SHA-1 with the supported PAL base-game hash in [BUILDING.md](BUILDING.md). Other executables are rejected. |
| `Title updates are not supported.` | Supply a separate, unmodified supported base-game extraction. The build helper rejects a `default.xexp` in that folder. |
| Visual Studio C++ Build Tools, Clang or Ninja cannot be found | Check the Visual Studio components listed in [BUILDING.md](BUILDING.md). For an installation the helper cannot locate, pass its root directory with `-VsInstall`. |
| `The SDK is not the pinned v0.10.0 revision.` | The helper intentionally leaves an existing SDK checkout alone. Use a separate project checkout for this preview rather than resetting an SDK that contains your work. |
| Compiler failures under memory pressure | Retry the same build command with `-Jobs 2`; the default is four jobs. Include the first compiler error if it still fails. |
| `Local executable missing` | Complete `tools/build-local.ps1` successfully. The application is expected at `out/build/win-amd64-release/nb.exe`; `-PrepareOnly` does not build it. |
| The play helper asks you to run the build helper first | Its local game-path settings are missing. Build with `-GameRoot` as shown in the guide. If you already have a successful build and moved only your extraction, pass the new path to `play.ps1 -GameRoot`. |

Run the commands from the repository in PowerShell. Keep paths containing spaces
inside quotes, as in the examples in [BUILDING.md](BUILDING.md).

## Local shaders

| Message or symptom | What to check |
| --- | --- |
| `No local native shaders found. Draws will fall back` | This is a warning, not a failed launch. Play with `-CaptureShaders`, close the game, then run `prepare-shaders.ps1`. |
| `Dump directory not found` | Run `play.ps1 -CaptureShaders` first. The public helper writes to `.local/shader-dump`. |
| Python cannot be found | Install Python 3.11+ as listed in the build guide, or pass `-Python 'C:\path\to\python.exe'` to `prepare-shaders.ps1`. |
| `fxc.exe was not found` | Install the Windows SDK component providing FXC, or pass `-Fxc 'C:\path\to\fxc.exe'` to `prepare-shaders.ps1`. |
| `Game or build process is running` | Close the game and let builds or shader compilations finish before retrying. The shader helper checks for those processes across the machine. |
| The shader helper cannot create `.local/machine.lock` because it exists | Another shader preparation may own the lock. Check its `pid` against the running processes before taking any action; do not remove a live process's lock. |
| `No supported shader stages were generated` | The helper installed no new library files. Check the printed generation log; visit another area in a capture run if no useful stages were collected. Report persistent translator errors. |
| `Shader translation failed`, `translator reported an internal error`, or `stage checks failed` | Keep the diagnostics locally and report the first relevant error text. The helper checks the staged output before installing it; failed stage checks install no library files. Do not post the generated shader sources or compiled files. |

The helper prints its temporary staging directory and retains diagnostic logs.
Report selected error lines with personal paths removed. A successful preparation
checks individual shader stages; runtime combinations and scene correctness still
need playtesting. Unsupported draws continue through fallback.

## Gameplay and display

**A rendering error changes with F5.** Report both states, the scene and your
local shader preparation status. If needed, start with
`& .\tools\play.ps1 -Emulated` to leave native draw replacements disabled.
This still uses the custom plugin. If the problem remains in both modes, include
that observation rather than assigning a cause.

**New areas stutter.** First encounters with shader pairs can compile at runtime.
Revisit the same area before measuring steady performance. Persistent pauses
still need a report with the scene, timing and settings; shader compilation is
not an explanation for every pause.

**Gameplay runs too quickly at high frame rates.** The checked-in configuration
allows up to 240 FPS, and some behavior depends on frame count. With the game
closed, set `video_mode_refresh_rate = 60.0` in
`out/build/win-amd64-release/nb.toml` to test the original 30 FPS pacing.
Record this change in your report. Defaults are copied after the application
links; a no-op rebuild does not restore them. To replace saved settings with the
checked-in defaults, close the game and run from the repository root:

```powershell
Copy-Item -LiteralPath '.\config\nb.toml' -Destination '.\out\build\win-amd64-release\nb.toml'
```

**Settings changed after using F4.** The save-to-config action writes options
beside the executable for later launches. Record any changes when testing, or
use the restore command above. The play helper still supplies its own launch
options, including native/capture mode, aspect ratio and fullscreen.

**The log says no audio output device was found.** The application can continue
with silent audio in this case. Connect or enable an audio device, then restart
the game. Other audio failures need their own report; this does not explain all
cases of missing sound.

**The log contains `NtQueryInformationFile(XFileXctdCompressionInformation)`
followed by `unimplemented`.** This message appeared in successful public-preview
title-screen checks. Its presence alone does not mean launch failed. Include the
actual visible symptom and surrounding relevant errors if the game stops.

**F9 does nothing.** Launch with an ultrawide aspect, such as
`-Ultrawide '32:9'`, and focus the game window. F9 does not enable an aspect that
was off at launch. F10 shows whether ultrawide is active or suspended.
F5, F8 and F10 also require the game window to have focus.

**Ultrawide looks incorrect or a higher render scale is slow.** Compare with F9
in the same scene, record the scale shown by F10, then try the default launch
settings for comparison. The `4x2` setting requests 5120x1440 internally and is
more demanding than the default; it is not a requirement for testing ultrawide.

**The game exits with a nonzero code.** The play helper reports that exit code,
but the code alone does not identify the cause. Include the last action, scene,
launch settings and relevant error lines. Avoid uploading a complete memory dump.

## Still stuck

Use the [build/setup form](https://github.com/djvb1234/BoltsRebuilt/issues/new?template=setup.yml)
for build or shader preparation failures and the
[playtest form](https://github.com/djvb1234/BoltsRebuilt/issues/new?template=playtest.yml)
for runtime results. Include the first relevant error and a command with personal
paths replaced by examples. The [first playtest guide](FIRST_PLAYTEST.md) lists the
hardware and comparison details that make a report useful.
