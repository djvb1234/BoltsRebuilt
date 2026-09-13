# Isolated deferred-command replay control

These original test dependency substitutes are used **only** by
`tools/test_native_deferred_command_list.cpp` together with the actual vendored
`deferred_command_list.cpp`. They replace command-processor pipeline lookup,
configuration flags, profiling includes and the broad D3D12 API include bundle.
The Windows COM interfaces, deferred writers, argument structures and replay
switch are real. No D3D12 device or game process is created.

Never add this include directory to the plugin or another production target.
Each substitute requires `NB_NATIVE_DEFERRED_REPLAY_TEST` to prevent accidental
use. The test uses no imported SDK runtime libraries and no game bytes.

From a Visual Studio x64 developer PowerShell, the lock-owning parent can build
the CPU control with the following command (this document does not run it):

```powershell
cl /nologo /std:c++20 /EHsc /W4 /O2 /DNDEBUG /DNB_NATIVE_DEFERRED_REPLAY_TEST /DNOMINMAX /DWIN32_LEAN_AND_MEAN `
  /IC:\rex\nb\tools\native_deferred_replay_stubs `
  /IC:\rex\nb\src\gpu\vendored\include /IC:\rex\sdk\include `
  C:\rex\nb\tools\test_native_deferred_command_list.cpp `
  C:\rex\nb\src\gpu\vendored\src\graphics\d3d12\deferred_command_list.cpp `
  /Fe:C:\rex\dl\test_native_deferred_command_list.exe
```

The parent must choose an isolated build working directory so object files stay
outside the source tree. Standard Windows SDK linker defaults suffice; no GPU
or shader libraries are called by this control.

The test compares every deferred command family with immediate calls to an
independent semantic recorder, including optional/null fields, variable arrays,
debug strings, source mutation, pipeline resolution at replay time, skipped
draws with unresolved pipelines, all three barrier unions, and list1 absence.
It alternates long/short/empty submissions, forces growth, and checks copy/move,
mode latching, enable/disable transitions and stale-tail exclusion. Padding is
not compared: every active API field is compared exactly, including float bits.

The separate original scratch GPU control is
`C:\rex\dl\native_deferred_gpu_probe.cpp`. To build it, use the same defines and
include paths above, replace the CPU test source with that scratch source, name
the executable `C:\rex\dl\native_deferred_gpu_probe.exe`, and add
`/link d3d12.lib dxgi.lib`. It exercises the actual production Execute function
with 32 synthetic buffer/texture copy and clear submissions, plus two negative
byte-oracle controls. It checks row padding and placement offset, long/short/
empty submissions, both policies and a mid-recording toggle. Optional `--warp`
is functional-only. It attempts the D3D12 debug layer and explicitly reports if
unavailable; absence does not establish debug-clean behavior.

The GPU control has no shaders and does not cover root constants or draws;
their API arguments are covered by the all-command CPU semantic trace. A later
literal draw probe could add those controls. CPU replay equivalence is not a
GPU correctness or frame-rate claim. No benchmark result is implied by the
avoided zero-initialization counters. Only the parent runs controls under the
existing exclusive machine lock.
