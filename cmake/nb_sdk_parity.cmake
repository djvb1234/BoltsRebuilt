# nb_apply_sdk_parity(<target>)
#
# The SDK root CMakeLists.txt applies a set of directory-scoped settings to its own
# targets (add_compile_definitions / add_compile_options / include_directories). In
# add_subdirectory mode none of that reaches our directory, but rexgpu-nb compiles the
# SDK's graphics sources and shares headers with rexruntime.dll whose layout depends on
# some of these defines (REXGLUE_ENABLE_PROFILING, REXGLUE_ENABLE_PERF_COUNTERS). A
# mismatch is an ODR hazard, so this function mirrors the SDK exactly.
#
# Keep in sync with ${REXSDK_DIR}/CMakeLists.txt (v0.10.0):
#   lines 21-32   options + profiling defines (non-Release only)
#   lines 102-105 C++23, extensions off
#   lines 131-140 global compile options
#   line  160     REX_PLATFORM_WINDOWS=1
#   line  184     REXGLUE_BUILD_CONFIG="$<CONFIG>"
#   lines 249-254 -Wall -Wextra, include_directories(${REXGLUE_ROOT}/include)
include_guard(GLOBAL)

function(nb_apply_sdk_parity target)
    if(NOT REXSDK_DIR)
        message(FATAL_ERROR "nb_apply_sdk_parity: REXSDK_DIR is not set (source-tree SDK required)")
    endif()

    target_compile_definitions(${target} PRIVATE
        $<$<AND:$<BOOL:${REXGLUE_ENABLE_TRACY}>,$<NOT:$<CONFIG:Release>>>:REXGLUE_ENABLE_PROFILING>
        $<$<AND:$<BOOL:${REXGLUE_ENABLE_PERF_COUNTERS}>,$<NOT:$<CONFIG:Release>>>:REXGLUE_ENABLE_PERF_COUNTERS>
        REX_PLATFORM_WINDOWS=1
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    target_compile_options(${target} PRIVATE
        -fno-strict-aliasing
        -ffp-model=strict
        -fno-char8_t
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>
        -Wall
        -Wextra)

    # SDK public headers, plus the SDK's generated rex/version.h in its binary dir.
    target_include_directories(${target} PRIVATE
        ${REXSDK_DIR}/include
        ${CMAKE_BINARY_DIR}/rexglue-sdk/include)

    # The SDK sets CMAKE_DEBUG_POSTFIX/CMAKE_RELWITHDEBINFO_POSTFIX in its own directory
    # scope, so rex_config_postfix() sees empty values from here. The plugin loader looks
    # for rexgpu-<name>d.dll / rexgpu-<name>rd.dll in those configurations; mirror the SDK.
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        DEBUG_POSTFIX d
        RELWITHDEBINFO_POSTFIX rd)

    # -msse4.1 and friends, same helper the SDK applies to every target.
    rexglue_apply_target_settings(${target})
endfunction()
