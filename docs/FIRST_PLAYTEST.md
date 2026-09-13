# Your first playtest

The aim of a first report is to record how far this preview gets on your computer
and make any problem reproducible. A successful title-screen launch is useful
feedback; it does not establish that the whole game works. See
[compatibility and known limitations](COMPATIBILITY.md) for the current evidence.

## 1. Build and record your version

Follow [Build and test on Windows](BUILDING.md), including its tool requirements
and supported PAL executable check. This preview provides source code. You build
locally using your own game extraction.

From PowerShell 7 in the repository, record:

```powershell
git rev-parse HEAD
git status --short
```

The first command identifies the exact revision. If the second prints changes,
mention your local edits in the report. Also record your Windows version, CPU,
GPU, GPU driver version, installed RAM, display resolution and refresh rate.
Include your controller model and connection type when reporting input problems.
The reference computer on the [compatibility page](COMPATIBILITY.md) is one tested
configuration, not a minimum hardware specification.

## 2. Start with a shader capture

After the build succeeds:

```powershell
& .\tools\play.ps1 -CaptureShaders
```

This launch disables native draw replacements and captures shaders locally.
Check whether the title screen appears, menus respond and audio plays. If you can
enter gameplay, visit a small, repeatable area. Note the scene and anything that
looks or behaves incorrectly. Close the game normally before the next step.

```powershell
& .\tools\prepare-shaders.ps1
& .\tools\play.ps1
```

The preparation step translates and checks your captured shaders. The second
launch enables native rendering for supported draws; other draws retain fallback.
Revisit the same area. New areas may need another capture and preparation run.
For setup errors, use [Troubleshooting](TROUBLESHOOTING.md).

## 3. Compare one thing at a time

For a rendering problem, stop in the affected scene and keep the camera in the
same position. With the game window focused, press **F5** once to disable native
draw replacements, then again to enable them. Describe whether the problem
changes in either direction. The log records `native generic pass ON` or `OFF`
after each toggle. This compares two paths inside the custom plugin; it is not
a comparison against an unmodified upstream emulator.

For performance observations, first revisit the area to allow shader compilation
to settle. **F8** toggles the FPS overlay. Record a measurement duration, approximate
range and visible pauses for both modes in the same scene. A single overlay
number or a different camera view is not a controlled benchmark.

If you have an ultrawide display, close the game and launch with its aspect ratio:

```powershell
& .\tools\play.ps1 -Ultrawide '32:9' -Fullscreen
```

Use `21:9` instead if that matches your display. **F9** switches the enabled
ultrawide view to 16:9 and back in the same run. Check the HUD, menus and objects
near the edges. F9 keeps the internal render scale unchanged, so it tests framing
rather than the cost of two different render resolutions. **F10** shows the
camera/aspect information and render scale in effect. Start with the default
scale before trying the more demanding `-RenderScale '4x2'` setting.

## 4. Send a useful result

Open a [playtest report](https://github.com/djvb1234/BoltsRebuilt/issues/new?template=playtest.yml)
for working results as well as problems. Include:

- The commit, hardware and driver details from step 1.
- Your launch command, aspect ratio, render scale and any changed settings.
- How far you got, the scene, and short steps someone else can repeat.
- What changed with F5, and with F9 if the problem involves ultrawide.
- Whether shaders were prepared, whether the scene was revisited, and the duration
  of any performance measurement.
- Whether a restart reproduces the result, if you checked.

A failure on one system is a useful observation, not proof that its whole GPU
family is incompatible. Include a small error excerpt when available and remove
personal paths. Do not attach game files, captured or translated shaders, caches,
generated game code, saves or memory dumps. See [Contributing](../CONTRIBUTING.md).

The preview keeps its saves and caches under `.local/user-data`. Preserve saves
you value when changing builds. For accelerated game behavior at high frame rates,
see the original-pacing setting in [Troubleshooting](TROUBLESHOOTING.md#gameplay-and-display).
