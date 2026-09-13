# rexgpu-nb: our GPU plugin. Milestone 1 step 0 is a clone of the SDK's rexgpu-xenos built
# from the SDK's own graphics sources (BSD-3) with only the entry point replaced, so that
# `gpu_plugin = "nb"` renders exactly like `gpu_plugin = "xenos"`. Later steps replace the
# D3D12 backend files with vendored copies under src/gpu/vendored/ and add our own sources.
#
# Source list mirrors ${REXSDK_DIR}/src/graphics/CMakeLists.txt (v0.10.0, D3D12 variant,
# Vulkan off). Every path is checked at configure time so an SDK bump fails loudly.

include(${CMAKE_SOURCE_DIR}/cmake/nb_sdk_parity.cmake)

set(NB_SDK_GRAPHICS ${REXSDK_DIR}/src/graphics)

set(NB_GPU_SDK_SOURCES
    # Core GPU types
    ${NB_SDK_GRAPHICS}/flags.cpp
    ${NB_SDK_GRAPHICS}/register_file.cpp
    ${NB_SDK_GRAPHICS}/registers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/pipeline/shader/shader.cpp  # exact translation lookup memo
    ${NB_SDK_GRAPHICS}/format/ucode.cpp
    ${NB_SDK_GRAPHICS}/sampler_info.cpp

    # GPU system
    ${NB_SDK_GRAPHICS}/graphics_system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/command_processor.cpp          # vendored: tile-pass counter
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/util/draw_extent_estimator.cpp  # vendored: guards on the vertex extent estimate
    ${NB_SDK_GRAPHICS}/util/draw.cpp
    ${NB_SDK_GRAPHICS}/packet_disassembler.cpp
    ${NB_SDK_GRAPHICS}/primitive_processor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/pipeline/render_target/cache.cpp  # vendored: EDRAM transfer log
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/shared_memory.cpp  # CPU-authoritative snapshots
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/pipeline/texture/cache.cpp  # optional load-first invalidation poll
    ${NB_SDK_GRAPHICS}/pipeline/texture/util.cpp  # local pure helpers allow plugin ThinLTO
    ${NB_SDK_GRAPHICS}/pipeline/shader/interpreter.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/translator.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/translator_disasm.cpp

    # DXBC shader translation
    ${NB_SDK_GRAPHICS}/pipeline/shader/dxbc.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/dxbc_translator.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/dxbc_translator_alu.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/dxbc_translator_fetch.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/dxbc_translator_memexport.cpp
    ${NB_SDK_GRAPHICS}/pipeline/shader/dxbc_translator_om.cpp

    # D3D12 backend (files marked vendored come from src/gpu/vendored, see UPSTREAM.md)
    ${NB_SDK_GRAPHICS}/d3d12/graphics_system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/d3d12/command_processor.cpp    # vendored: step 6, counters, native seam
    ${NB_SDK_GRAPHICS}/d3d12/primitive_processor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/d3d12/render_target_cache.cpp  # vendored: step 1 rtv default
    ${NB_SDK_GRAPHICS}/d3d12/shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/d3d12/shared_memory.cpp  # exact upload byte reuse
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/d3d12/texture_cache.cpp  # verified offline uploads
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/d3d12/deferred_command_list.cpp  # vendored: step 6 counter
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/src/graphics/d3d12/pipeline_cache.cpp       # vendored: step 6 sync translation
)

set(NB_GPU_SDK_THIRDPARTY_SOURCES
    ${REXSDK_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
)

foreach(_src IN LISTS NB_GPU_SDK_SOURCES NB_GPU_SDK_THIRDPARTY_SOURCES)
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR "rexgpu-nb: SDK source missing: ${_src} (SDK layout changed?)")
    endif()
endforeach()

set(NB_GPU_OWN_SOURCES
    nb_plugin_main.cpp
    nb_graphics_system.cpp
    nb_graphics_system.h
    nb_command_processor.cpp
    nb_command_processor.h
    nb_cp_records.cpp       # frame inventory: per-draw/resolve JSON records (nb_cp_records_*)
    nb_cp_records.h
    native/native_pass.cpp  # native fullscreen pass drawn through the TryNativeDraw seam
    native/native_pass.h
    native/native_geometry_pass.cpp  # native pass over the guest's own vertex data
    native/native_geometry_pass.h
    native/native_shader_library.cpp  # generated (vs, ps) shader pairs from tools/ucode2hlsl.py
    native/native_shader_library.h
    native/native_shader_cache.cpp
    native/native_shader_cache.h
    native/native_shader_hash.cpp
    native/native_shader_hash.h
    native/native_shader_hash_policy.h
    native/native_shader_hash_avx2.cpp
    native/native_replay_worker.cpp
    native/native_replay_worker.h
    native/native_cpu_affinity.cpp
    native/native_cpu_affinity.h
    native/native_precise_wait.cpp
    native/native_precise_wait.h
    native/native_gpu_submission_timing.cpp
    native/native_gpu_submission_timing.h
    native/native_gpu_budget_probe.cpp
    native/native_gpu_budget_probe.h
    native/native_gpu_upload_pool.cpp   # optional native constant slices in CPU-writable video memory
    native/native_gpu_upload_pool.h
    native/native_constant_upload_diagnostics.h
    native/native_upload_shadow.cpp
    native/native_upload_shadow.h
    native/native_asset_cache.cpp
    native/native_asset_cache.h
    native/offline_texture_cache.cpp
    native/offline_texture_cache.h
    native/nb_ultrawide_bridge.cpp  # nb_ultrawide from nb.exe: presenter aspect and default render scale
    native/nb_ultrawide_bridge.h
)

# The HLSL prelude every geometry pass starts with is embedded from native/shaders/prelude.hlsl, the same
# file tools/ucode2hlsl.py prepends for its offline fxc check, so the two sides cannot drift.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/native/shaders/prelude.hlsl NB_NATIVE_PRELUDE_HLSL)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/native/native_prelude.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/native_prelude.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/native/shaders/prelude.hlsl)

add_library(rexgpu-nb SHARED
    ${NB_GPU_SDK_SOURCES}
    ${NB_GPU_SDK_THIRDPARTY_SOURCES}
    ${NB_GPU_OWN_SOURCES})

if(NB_RUNTIME_WATCH_CONTROL_AVAILABLE)
    set_property(SOURCE nb_command_processor.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS NB_HAS_RUNTIME_WATCH_CONTROL=1)
endif()

# Loader convention: rexgpu-<name>[d|rd].dll next to the executable.
rex_config_postfix(rexgpu-nb)
set_target_properties(rexgpu-nb PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gpu
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gpu
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gpu)

nb_apply_sdk_parity(rexgpu-nb)

# Optional compiler experiment, scoped to this DLL. Preserve the SDK ABI,
# strict floating-point model and baseline ISA. Cross-DLL interfaces must not
# be treated as closed-world, so no whole-program visibility/vtable options.
option(NB_GPU_THINLTO "Enable Release ThinLTO for the native GPU plugin only" OFF)
if(NB_GPU_THINLTO)
    get_filename_component(NB_GPU_LINKER_NAME "${CMAKE_LINKER}" NAME)
    if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
       NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU" OR
       NOT NB_GPU_LINKER_NAME MATCHES "^lld-link(\\.exe)?$")
        message(FATAL_ERROR "NB_GPU_THINLTO requires Windows clang++ and lld-link")
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT NB_GPU_IPO_SUPPORTED OUTPUT NB_GPU_IPO_ERROR LANGUAGES CXX)
    if(NOT NB_GPU_IPO_SUPPORTED)
        message(FATAL_ERROR "Native GPU ThinLTO is unavailable: ${NB_GPU_IPO_ERROR}")
    endif()
    set_property(TARGET rexgpu-nb PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    target_link_options(rexgpu-nb PRIVATE
        "$<$<CONFIG:Release>:LINKER:/opt:lldltojobs=4>"
        "$<$<CONFIG:Release>:LINKER:/lldltocache:${CMAKE_CURRENT_BINARY_DIR}/thinlto-cache>"
        "$<$<CONFIG:Release>:LINKER:/lldltocachepolicy:cache_size=0%:cache_size_bytes=2g>")
endif()

# Vendored SDK sources (src/gpu/vendored/UPSTREAM.md): their include dir must come BEFORE the SDK's so
# our copy of a header wins; the SDK's D3D12 sources include the prebuilt shader bytecode as
# "../shaders/bytecode/d3d12_5_1/*.h" relative to their own directory, which the -I fallback resolves
# for the vendored copies. Requires a reconfigure (the PCH is built through the same search path).
target_include_directories(rexgpu-nb BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored)
# A newly vendored header changes which file an existing #include resolves to.
# Ninja only knows the previously selected SDK file, so a normal reconfigure can
# otherwise retain old-size allocators beside new-size constructors. Change the
# compiler command for every plugin object whenever an override changes. This
# also forces constructor/allocator agreement after existing-header ABI edits.
file(GLOB_RECURSE NB_VENDORED_HEADERS CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/vendored/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendored/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendored/include/*.hpp")
list(SORT NB_VENDORED_HEADERS)
set(NB_VENDORED_HEADER_SET "")
foreach(NB_VENDORED_HEADER IN LISTS NB_VENDORED_HEADERS)
    set(NB_VENDORED_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vendored/include/${NB_VENDORED_HEADER}")
    file(SHA256 "${NB_VENDORED_HEADER_PATH}" NB_VENDORED_HEADER_CONTENT_HASH)
    string(APPEND NB_VENDORED_HEADER_SET "${NB_VENDORED_HEADER} ${NB_VENDORED_HEADER_CONTENT_HASH}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NB_VENDORED_HEADER_PATH}")
endforeach()
string(SHA256 NB_VENDORED_HEADER_SET_HASH "${NB_VENDORED_HEADER_SET}")
target_compile_definitions(rexgpu-nb PRIVATE
    NB_VENDORED_HEADER_SET_HASH="${NB_VENDORED_HEADER_SET_HASH}")
target_include_directories(rexgpu-nb PRIVATE
    ${REXSDK_DIR}/thirdparty/renderdoc
    ${REXSDK_DIR}   # thirdparty/dxbc/DXBCChecksum.h is included by that path
    ${REXSDK_DIR}/src/graphics/d3d12   # bytecode fallback for the vendored d3d12 sources
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR})       # native_prelude.h

target_compile_options(rexgpu-nb PRIVATE -Wno-unused-variable)

rex_target_pch(rexgpu-nb
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/memory.h>
    <rex/thread/mutex.h>
    <rex/system/xmemory.h>
    <rex/graphics/xenos.h>
    <rex/graphics/registers.h>
    <rex/graphics/format/ucode.h>
    <rex/graphics/pipeline/shader/shader.h>)

set_source_files_properties(${NB_GPU_SDK_THIRDPARTY_SOURCES}
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Exact optional shader hashing only: never raise the ISA of the GPU target or
# its PCH. The native object boundary prevents ThinLTO from importing AVX2 into
# baseline callers; CPU/OS admission stays in native_shader_hash.cpp.
set_source_files_properties(native/native_shader_hash_avx2.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "-mavx2;-fno-lto")

# Everything PRIVATE, like the SDK: never link the OBJECT libraries (rexcore/rexui);
# rexruntime already contains them and relinking would duplicate global state.
target_link_libraries(rexgpu-nb PRIVATE
    rexruntime
    xxHash::xxhash
    d3d12
    dxgi
    dxguid
    bcrypt
    d3dcompiler)   # runtime HLSL compilation of the native passes (D3DCompile)
