# Keep the source-built runtime and its optional nb-owned transformation in the
# nb build tree. The installed SDK path remains unchanged when the option is OFF.
include_guard(GLOBAL)

option(NB_RUNTIME_WATCH_FASTPATH
       "Build the guarded physical write-watch shortcut into nb's runtime" OFF)
set(NB_RUNTIME_WATCH_CONTROL_AVAILABLE FALSE)

# This target runs even when an unchanged import library lets nb.exe avoid a
# relink. The SDK's POST_BUILD copy alone cannot guarantee a fresh runtime DLL.
function(nb_stage_runtime_for host_target)
    if(NOT TARGET rexruntime)
        return()
    endif()
    get_target_property(_isolated rexruntime NB_RUNTIME_ISOLATED)
    if(NOT _isolated)
        return()
    endif()
    if(NOT TARGET "${host_target}")
        message(FATAL_ERROR "nb_stage_runtime_for requires an existing host target")
    endif()
    if(TARGET nb_stage_runtime)
        message(FATAL_ERROR "nb_stage_runtime_for may only be called once")
    endif()

    # cmake_minimum_required(3.25) selects CMP0112 NEW, so TARGET_FILE_DIR does
    # not create a dependency back to the host and a cycle with add_dependencies.
    add_custom_target(nb_stage_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${host_target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:rexruntime>" "$<TARGET_FILE_DIR:${host_target}>"
        DEPENDS rexruntime "$<TARGET_FILE:rexruntime>"
        VERBATIM)
    add_dependencies("${host_target}" nb_stage_runtime)
endfunction()

if(NOT REXSDK_DIR)
    if(NB_RUNTIME_WATCH_FASTPATH)
        message(FATAL_ERROR
            "NB_RUNTIME_WATCH_FASTPATH requires a source SDK (-DREXSDK_DIR=...). "
            "An installed SDK runtime cannot have its sources replaced.")
    endif()
    return()
endif()
if(NOT TARGET rexruntime)
    message(FATAL_ERROR
        "NbRuntime.cmake must be included after the source SDK creates rexruntime")
endif()
get_target_property(_nb_runtime_imported rexruntime IMPORTED)
if(_nb_runtime_imported)
    if(NB_RUNTIME_WATCH_FASTPATH)
        message(FATAL_ERROR "NB_RUNTIME_WATCH_FASTPATH cannot modify an imported rexruntime")
    endif()
    return()
endif()
get_target_property(_nb_runtime_type rexruntime TYPE)
if(NOT _nb_runtime_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "NbRuntime.cmake requires the source SDK's shared rexruntime target")
endif()

get_filename_component(_nb_project_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_nb_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/nb_runtime")
set(_nb_runtime_output "${_nb_runtime_dir}/$<CONFIG>")
# Mandatory for every source SDK build, including when the watch option is OFF.
# Set configuration-specific properties too: they otherwise override these base
# properties and could retain an SDK output path supplied by another toolchain.
foreach(_property RUNTIME_OUTPUT_DIRECTORY LIBRARY_OUTPUT_DIRECTORY
                  ARCHIVE_OUTPUT_DIRECTORY PDB_OUTPUT_DIRECTORY)
    set_property(TARGET rexruntime PROPERTY "${_property}" "${_nb_runtime_output}")
    set(_configs Debug Release RelWithDebInfo MinSizeRel
                 ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES _configs)
    foreach(_config IN LISTS _configs)
        string(TOUPPER "${_config}" _config_upper)
        set_property(TARGET rexruntime PROPERTY "${_property}_${_config_upper}"
                     "${_nb_runtime_output}")
    endforeach()
endforeach()
set_property(TARGET rexruntime PROPERTY NB_RUNTIME_ISOLATED TRUE)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/nb_runtime/runtime_$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:rexruntime>\n" NEWLINE_STYLE LF)

# The control API has stable exports in both build modes. Its implementation
# defaults to disabled and reports whether the generated shortcut is available.
set(_nb_watch_control "${_nb_project_root}/src/runtime/watch_control.cpp")
if(NOT EXISTS "${_nb_watch_control}")
    message(FATAL_ERROR "nb runtime control source is missing: ${_nb_watch_control}")
endif()
target_sources(rexruntime PRIVATE "${_nb_watch_control}")
if(NB_RUNTIME_WATCH_FASTPATH)
    set(_nb_watch_build_value 1)
else()
    set(_nb_watch_build_value 0)
endif()
set_source_files_properties("${_nb_watch_control}" TARGET_DIRECTORY rexruntime
    PROPERTIES COMPILE_DEFINITIONS "NB_RUNTIME_WATCH_FASTPATH=${_nb_watch_build_value}")
set(NB_RUNTIME_WATCH_CONTROL_AVAILABLE TRUE)
message(STATUS "nb: source-built rexruntime outputs are isolated under ${_nb_runtime_dir}")

if(NOT NB_RUNTIME_WATCH_FASTPATH)
    return()
endif()

file(REAL_PATH "${REXSDK_DIR}/src/system/xmemory.cpp" _nb_sdk_xmemory)
file(REAL_PATH "${REXSDK_DIR}/include/rex/system/xmemory.h" _nb_sdk_xmemory_header)
set(_nb_source_sha "97d19808714c2cb656c8d3c053cfd29a42d9b399bae47c9b3652ae21807fbf48")
set(_nb_header_sha "21d05c176449d6dd12cfb4134bcad615565f4255458fde5e0a70915f1e05062b")
foreach(_kind source header)
    if(_kind STREQUAL "source")
        set(_path "${_nb_sdk_xmemory}")
    else()
        set(_path "${_nb_sdk_xmemory_header}")
    endif()
    file(SHA256 "${_path}" _actual)
    if(NOT "${_actual}" STREQUAL "${_nb_${_kind}_sha}")
        message(FATAL_ERROR
            "nb: runtime watch ${_kind} is not the reviewed SDK revision.\n"
            "  file: ${_path}\n  expected: ${_nb_${_kind}_sha}\n  actual: ${_actual}\n"
            "Review the changed contract before updating the pinned hash.")
    endif()
endforeach()

set(_nb_anchor "${CMAKE_CURRENT_LIST_DIR}/runtime_watch_anchor.txt")
set(_nb_replacement "${CMAKE_CURRENT_LIST_DIR}/runtime_watch_replacement.txt")
set(_nb_watch_header "${_nb_project_root}/src/runtime/watch_fastpath.h")
set(_nb_control_header "${_nb_project_root}/src/runtime/watch_control.h")
foreach(_input "${_nb_anchor}" "${_nb_replacement}" "${_nb_watch_header}" "${_nb_control_header}")
    if(NOT EXISTS "${_input}")
        message(FATAL_ERROR "nb runtime watch input is missing: ${_input}")
    endif()
endforeach()
file(READ "${_nb_sdk_xmemory}" _nb_original)
file(READ "${_nb_anchor}" _nb_anchor_text)
file(READ "${_nb_replacement}" _nb_replacement_text)
# Raw file hashes were checked first. Match logical lines consistently even if
# an approved source or project input uses CRLF; generated source always uses LF.
string(REPLACE "\r\n" "\n" _nb_original "${_nb_original}")
string(REPLACE "\r\n" "\n" _nb_anchor_text "${_nb_anchor_text}")
string(REPLACE "\r\n" "\n" _nb_replacement_text "${_nb_replacement_text}")
if("${_nb_anchor_text}" STREQUAL "")
    message(FATAL_ERROR "nb runtime watch anchor must not be empty")
endif()
string(FIND "${_nb_original}" "${_nb_anchor_text}" _nb_anchor_first)
if(_nb_anchor_first LESS 0)
    message(FATAL_ERROR "nb runtime watch anchor matched zero times; expected exactly one")
endif()
# Search from one character after the first match, including overlapping matches.
math(EXPR _nb_after_first "${_nb_anchor_first} + 1")
string(SUBSTRING "${_nb_original}" ${_nb_after_first} -1 _nb_remaining)
string(FIND "${_nb_remaining}" "${_nb_anchor_text}" _nb_anchor_second)
if(NOT _nb_anchor_second LESS 0)
    message(FATAL_ERROR "nb runtime watch anchor matched more than once; expected exactly one")
endif()
string(REPLACE "${_nb_anchor_text}" "${_nb_replacement_text}" _nb_patched "${_nb_original}")
set(_nb_generated_text
    "#include \"${_nb_watch_header}\"\n#include \"${_nb_control_header}\"\n\n${_nb_patched}")
# file(GENERATE) evaluates generator expressions. Refuse unexpected expressions
# in C++ text rather than silently changing source bytes during generation.
string(FIND "${_nb_generated_text}" "$<" _nb_generator_expression)
if(NOT _nb_generator_expression LESS 0)
    message(FATAL_ERROR "nb runtime watch source contains unsupported CMake generator-expression text")
endif()

get_target_property(_nb_runtime_sources rexruntime SOURCES)
get_target_property(_nb_runtime_source_dir rexruntime SOURCE_DIR)
set(_nb_original_count 0)
foreach(_source IN LISTS _nb_runtime_sources)
    if(_source MATCHES "^\\$<")
        continue()
    endif()
    file(REAL_PATH "${_source}" _absolute BASE_DIRECTORY "${_nb_runtime_source_dir}")
    if("${_absolute}" STREQUAL "${_nb_sdk_xmemory}")
        math(EXPR _nb_original_count "${_nb_original_count} + 1")
    endif()
endforeach()
if(NOT _nb_original_count EQUAL 1)
    message(FATAL_ERROR
        "nb runtime watch expected exactly one SDK xmemory.cpp in rexruntime, found ${_nb_original_count}")
endif()

set(_nb_generated_xmemory "${_nb_runtime_dir}/xmemory.cpp")
file(GENERATE OUTPUT "${_nb_generated_xmemory}" CONTENT "${_nb_generated_text}" NEWLINE_STYLE LF)
set_source_files_properties("${_nb_sdk_xmemory}" TARGET_DIRECTORY rexruntime
                            PROPERTIES HEADER_FILE_ONLY ON)
set_source_files_properties("${_nb_generated_xmemory}" TARGET_DIRECTORY rexruntime
                            PROPERTIES GENERATED TRUE)
target_sources(rexruntime PRIVATE "${_nb_generated_xmemory}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_nb_sdk_xmemory}" "${_nb_sdk_xmemory_header}"
    "${_nb_anchor}" "${_nb_replacement}" "${_nb_watch_header}" "${_nb_control_header}")
message(STATUS "nb: runtime watch source/header hashes and unique anchor verified")
