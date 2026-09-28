# Xenia resolve shader sources

Unmodified copies of the XeSL sources the RexGlue SDK v0.10.0 resolve copy shaders were compiled from,
taken from https://github.com/xenia-project/xenia at commit
`04d5c40d0dc23e14e3c6015fd37fa8f3c09a0677` (`src/xenia/gpu/shaders/` and
`src/xenia/ui/shaders/xesl.xesli`). BSD-3-Clause, Ben Vanik and Xenia contributors; each file keeps its
license header.

The revision was chosen by comparison, not by record: the DXBC disassembly of all eighteen
`resolve_fast_*` and `resolve_full_*` headers under the SDK's `src/graphics/shaders/bytecode/d3d12_5_1/`
is identical to Xenia's precompiled headers at this commit (and at `3c128142`), and differs from those at
the neighbouring commits and at Xenia's current master.

`../nb_direct_resolve.hlsl` compiles them at runtime with the EDRAM buffer reads redirected to the render
target. The build embeds every file in this directory (`src/gpu/CMakeLists.txt`); the include handler in
`render_target_cache.cpp` resolves `#include` by file name, so the relative paths inside them need no
editing.
