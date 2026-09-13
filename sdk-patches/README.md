# Source patches for RexGlue v0.10.0

`tools/build-local.ps1` applies these patches to the pinned `.deps/rexglue-sdk`
checkout in filename order. For manual use, apply from the SDK root with
`git apply <patch>`. Re-check against upstream on every SDK bump. The PR references
below describe the historical v0.10.0 baseline, not their current upstream status.

- `0001-mspack-dir-real-sources.patch`: `thirdparty/CMakeLists.txt` pointed `MSPACK_DIR` at
  `libmspack/cabextract/mspack`, whose `.c` files are git symlinks that Windows checks out as one-line
  text stubs (clang: "expected identifier or '('"). Point at `libmspack/libmspack/mspack`, the real
  sources. Upstream fix pending as rexglue-sdk PR #422.

## 0002-imgui-include-dirs-propagate.patch

`src/kernel/CMakeLists.txt`: `rex/ui/style.h` includes `<imgui.h>`, but `rexruntime` did not
propagate imgui's include directories, so a consumer in `add_subdirectory` mode fails to compile
`rex/rex_app.h`. Adds a `BUILD_INTERFACE`-only `target_include_directories(rexruntime PUBLIC ...)`.
Upstream: PR #423 (open at v0.10.0). Harmless for the installed SDK, which ships imgui headers.

## 0003-version-from-sdk-root.patch

`CMakeLists.txt`: `rex_resolve_version` ran `git describe` in `CMAKE_SOURCE_DIR`, which is the
consumer's repo in `add_subdirectory` mode, so the SDK version came from the wrong repository.
Passes `SOURCE_DIR ${REXGLUE_ROOT}`. Upstream: PR #424 (open at v0.10.0).

## 0004-heap-free-guard.patch (script: apply_0004_heap_guard.py)

`src/kernel/crt/heap.cpp`: the rexcrt heap (o1heap-backed `RtlAllocateHeap`/`RtlFreeHeap`
replacement) crashed twice in ~18 boots inside `o1heapFree` (o1heap.c ~500, write to 0x3C)
while freeing a block whose o1heap fragment header held garbage, reached from the game's
`meInternalFree`. o1heap validates nothing in release builds. The patch stamps every live
allocation (`SizeHeader::reserved = "REXHEAP!"`), flips the stamp on free, and rejects a free,
size or realloc whose stamp or o1heap header is not sane, logging the guest return address
(rate-limited) instead of corrupting the heap. Local diagnostic; not upstream material as-is.

## 0005-audio-submitframe-null-guard.patch (script: apply_0005_0006_audio_xam.py)

`src/audio/audio_system.cpp`: `AudioSystem::SubmitFrame` dereferenced `clients_[index].driver`
with only an assert in front of it. The game re-creates its audio device about nine seconds after
boot; a frame submitted for the client that was just unregistered crashed the Release build
(audio_system.cpp:276, read of address 0). The guard drops such frames with a rate-limited
warning. Upstream-worthy as a bug report.

## 0006-xamalloc-flags-warning.patch (same script)

`src/kernel/xam/xam_info.cpp`: `XamAlloc_entry` asserted `unk == 0`; Nuts & Bolts passes a
non-zero flag, which halted every Debug build in a breakpoint (xam_info.cpp:301). The assert is
now a warning logged once per distinct value; the allocation proceeds as before.

## 0007-video-refresh-clamp-480.patch

`src/kernel/xboxkrnl/xboxkrnl_video.cpp`: raises the guest video-mode refresh clamp
from 240 to 480 Hz to match the application's optional higher refresh setting.
This changes the guest pacing ceiling; it is not a performance measurement.
