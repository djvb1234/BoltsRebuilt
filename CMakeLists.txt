# nb - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++23, not gnu++23, like the SDK

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.local/nb_manifest.toml")
    message(FATAL_ERROR "Run tools/build-local.ps1 with your own extracted game first. See docs/BUILDING.md.")
endif()
include(generated/rexglue.cmake)
include(cmake/NbRuntime.cmake)

# NbRuntime isolates the SDK DLL in this build tree. The SDK's command-line
# generator also links it, so stage it beside that executable before codegen.
if(TARGET rexglue AND TARGET rexruntime)
    add_custom_target(nb_stage_codegen_runtime
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:rexruntime>" "$<TARGET_FILE_DIR:rexglue>"
        DEPENDS rexglue rexruntime
        VERBATIM)
    add_dependencies(nb_codegen nb_stage_codegen_runtime)
endif()

# Sources
set(NB_SOURCES
    src/main.cpp
    src/hooks/heap_hooks.cpp        # meInternalFree owner-probe skip + header forensics
    src/hooks/pool_trace_hooks.cpp  # streaming pool reset/lookup race trace and serialization
    src/hooks/hooks_m1.cpp          # milestone-1 policy: single tile, scene MSAA off, tiling log
    src/hooks/swap_hook.cpp         # the one strong rex_D3DDevice_Swap wrapper (fan-out)
    src/audio/nb_audio_system.cpp   # SDL audio with a silent fallback when there is no device
    src/input/nb_scripted_input.cpp # scripted controller for reproducible runs (nb_input_script)
    src/ui/nb_fps_overlay.cpp       # F8 game Swap cadence display; no renderer timing changes
    src/ultrawide/nb_ultrawide.cpp  # nb_ultrawide state, cvars and the exports rexgpu-nb reads (docs/ultrawide.md)
    src/hooks/ultrawide_hooks.cpp   # Hor+ aspect, centred XUI HUD, camera snapshot and debug FOV/culling hooks
    src/ui/nb_camera_overlay.cpp    # F10 camera/FOV overlay
)

if(WIN32)
    add_executable(nb WIN32 ${NB_SOURCES})
else()
    add_executable(nb ${NB_SOURCES})
endif()

# Our GPU plugin (needs the SDK as a source tree).
if(REXSDK_DIR)
    add_subdirectory(src/gpu)
    set(NB_GPU_PLUGINS xenos nb)
else()
    message(WARNING "REXSDK_DIR not set: rexgpu-nb is not built, only the stock xenos plugin is staged")
    set(NB_GPU_PLUGINS xenos)
endif()

rexglue_setup_target(nb GPU_PLUGINS ${NB_GPU_PLUGINS})
target_link_libraries(nb PRIVATE rex::runtime d3d12)
nb_stage_runtime_for(nb)

# toml++ for NbApp::OnConfigurePaths (reads game_data_root out of nb.toml). The
# target is `tomlplusplus` in add_subdirectory mode, `rex::tomlplusplus` when installed.
if(TARGET tomlplusplus)
    target_link_libraries(nb PRIVATE tomlplusplus)
else()
    target_link_libraries(nb PRIVATE rex::tomlplusplus)
endif()

# The runtime reads its cvar config from <exe_dir>/nb.toml.
add_custom_command(TARGET nb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/nb.toml" "$<TARGET_FILE_DIR:nb>/nb.toml"
    VERBATIM)

# SDL controller mappings (SDL_GameControllerDB, zlib license, third_party/sdl_gamecontrollerdb).
# The SDK's SDL input driver loads <exe_dir>/gamecontrollerdb.txt (cvar hid_mappings_file).
add_custom_command(TARGET nb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdl_gamecontrollerdb/gamecontrollerdb.txt"
        "$<TARGET_FILE_DIR:nb>/gamecontrollerdb.txt"
    VERBATIM)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/codegen-tool.txt" CONTENT "$<TARGET_FILE:rex::rexglue>")
