cmake_minimum_required(VERSION 3.20)

function(_unideps_normalize_arch raw out_var)
    if(raw MATCHES "^(arm64-v8a|ARM64|arm64|aarch64)$")
        set(${out_var} "aarch64" PARENT_SCOPE)
    elseif(raw MATCHES "^(armeabi-v7a|ARM|arm|armv7.*)$")
        set(${out_var} "arm" PARENT_SCOPE)
    elseif(raw MATCHES "^(x86_64|AMD64|amd64|x64)$")
        set(${out_var} "x86_64" PARENT_SCOPE)
    elseif(raw MATCHES "^(x86|i686|i386|X86)$")
        set(${out_var} "x86" PARENT_SCOPE)
    else()
        set(${out_var} "${raw}" PARENT_SCOPE)
    endif()
endfunction()

function(_unideps_is_android out_var)
    if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR DEFINED ANDROID_ABI
       OR (DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "android"))
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(unideps_detect_target_triple out_var)
    _unideps_is_android(_is_android)
    if(_is_android)
        if(DEFINED ANDROID_ABI AND NOT ANDROID_ABI STREQUAL "")
            set(_raw_arch "${ANDROID_ABI}")
        elseif(DEFINED CMAKE_ANDROID_ARCH_ABI AND NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "")
            set(_raw_arch "${CMAKE_ANDROID_ARCH_ABI}")
        else()
            set(_raw_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        _unideps_normalize_arch("${_raw_arch}" _arch)
        if(_arch STREQUAL "")
            set(_arch "aarch64")
        endif()
        set(${out_var} "${_arch}-linux-android" PARENT_SCOPE)
        return()
    endif()

    set(_raw_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(_raw_arch STREQUAL "" AND DEFINED CMAKE_HOST_SYSTEM_PROCESSOR)
        set(_raw_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(GET CMAKE_OSX_ARCHITECTURES 0 _raw_arch)
    endif()
    _unideps_normalize_arch("${_raw_arch}" _arch)

    if(WIN32)
        if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            set(_triple "${_arch}-pc-windows-msvc")
        elseif(MINGW OR CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(_triple "${_arch}-pc-windows-gnu")
        else()
            set(_triple "${_arch}-pc-windows-msvc")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(_triple "${_arch}-apple-ios")
        else()
            set(_triple "${_arch}-apple-darwin")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        set(_triple "wasm32-unknown-emscripten")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_triple "${_arch}-unknown-linux-gnu")
    else()
        message(FATAL_ERROR "UniDeps: unsupported target system '${CMAKE_SYSTEM_NAME}'")
    endif()
    set(${out_var} "${_triple}" PARENT_SCOPE)
endfunction()

# Returns the MSVC runtime of the consuming project (may contain $<CONFIG> generator
# expressions, which unideps expands), or "" when it should use the default for the
# build type.
function(unideps_detect_runtime out_var)
    set(${out_var} "" PARENT_SCOPE)
    _unideps_is_android(_is_android)
    if(_is_android OR NOT WIN32)
        return()
    endif()
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "")
        set(${out_var} "${CMAKE_MSVC_RUNTIME_LIBRARY}" PARENT_SCOPE)
    elseif(CMAKE_CXX_FLAGS MATCHES "[/-]MTd" OR CMAKE_C_FLAGS MATCHES "[/-]MTd")
        set(${out_var} "MultiThreadedDebug" PARENT_SCOPE)
    elseif(CMAKE_CXX_FLAGS MATCHES "[/-]MT" OR CMAKE_C_FLAGS MATCHES "[/-]MT")
        set(${out_var} "MultiThreaded" PARENT_SCOPE)
    elseif(CMAKE_CXX_FLAGS MATCHES "[/-]MDd" OR CMAKE_C_FLAGS MATCHES "[/-]MDd")
        set(${out_var} "MultiThreadedDebugDLL" PARENT_SCOPE)
    endif()
endfunction()

set(UNIDEPS_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Options for `enabled_if`: cache BOOL/STRING entries (option(), -D...) and normal
# variables holding a boolean constant, as "KEY=VALUE" items in _unideps_option_defs.
# Paths and internal entries are skipped to keep the command line short.
macro(_unideps_collect_options)
    set(_unideps_option_defs "")
    get_cmake_property(_unideps_all_vars VARIABLES)
    foreach(_unideps_var IN LISTS _unideps_all_vars)
        if(_unideps_var MATCHES "^(CMAKE_|UNIDEPS_|ANDROID_|_)" OR NOT _unideps_var MATCHES "^[A-Za-z0-9_]+$")
            continue()
        endif()
        set(_unideps_val "${${_unideps_var}}")
        if(_unideps_val STREQUAL "" OR _unideps_val MATCHES "[;\n]")
            continue()
        endif()
        get_property(_unideps_type CACHE "${_unideps_var}" PROPERTY TYPE)
        if(_unideps_type STREQUAL "BOOL" OR _unideps_type STREQUAL "STRING"
           OR _unideps_val MATCHES "^([Oo][Nn]|[Oo][Ff][Ff]|[Tt][Rr][Uu][Ee]|[Ff][Aa][Ll][Ss][Ee]|[Yy][Ee][Ss]|[Nn][Oo]|[01])$")
            list(APPEND _unideps_option_defs "${_unideps_var}=${_unideps_val}")
        endif()
    endforeach()
endmacro()

# Inside a package that is being built by unideps itself (UNIDEPS_ACTIVE is exported to
# every CMake process unideps starts) running `unideps` again would wait forever for the
# build lock. The outer run builds the dependencies of the package's own unideps.toml
# beforehand, in two steps:
#  1. a probe configure (UNIDEPS_NESTED_PROBE=<file>): the options the package has
#     declared up to this point are written to <file> and the configure stops here. They
#     decide which of the manifest's dependencies are enabled (`enabled_if`);
#  2. the real configure (UNIDEPS_NESTED_TARGETS=<file>): the generated targets file of
#     the built dependencies is included.
macro(_unideps_setup_nested)
    if(UNIDEPS_NESTED_PROBE)
        _unideps_collect_options()
        list(JOIN _unideps_option_defs "\n" _unideps_probe_content)
        file(WRITE "${UNIDEPS_NESTED_PROBE}" "${_unideps_probe_content}\n")
        message(FATAL_ERROR "UniDeps: options written to ${UNIDEPS_NESTED_PROBE}; this configure run only "
                            "probes them and is expected to stop here")
    elseif(UNIDEPS_NESTED_TARGETS AND EXISTS "${UNIDEPS_NESTED_TARGETS}")
        message(STATUS "UniDeps: building inside another unideps run, using ${UNIDEPS_NESTED_TARGETS}")
        include("${UNIDEPS_NESTED_TARGETS}")
    else()
        message(WARNING "UniDeps: unideps_setup() skipped: this project is built by unideps, but it has no "
                        "unideps.toml next to its top-level CMakeLists.txt, so there are no dependencies to set up")
    endif()
endmacro()

macro(_unideps_setup_build)
    set(_unideps_search_hints
        "${UNIDEPS_CMAKE_DIR}/../target/release"
        "${UNIDEPS_CMAKE_DIR}/../target/debug"
        "${CMAKE_CURRENT_SOURCE_DIR}/.unideps/bin"
    )
    find_program(UNIDEPS_EXECUTABLE NAMES unideps HINTS ${_unideps_search_hints})
    if(NOT UNIDEPS_EXECUTABLE)
        message(FATAL_ERROR "UniDeps: executable 'unideps' not found on PATH or in: ${_unideps_search_hints}. "
                            "Set UNIDEPS_EXECUTABLE to its full path.")
    endif()

    get_property(_unideps_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_unideps_multi_config AND NOT CMAKE_BUILD_TYPE)
        message(STATUS "UniDeps: multi-config generator detected; dependencies are built once in Release. "
                       "Pass -DCMAKE_BUILD_TYPE=<cfg> to choose a different configuration.")
    endif()

    unideps_detect_target_triple(_unideps_target)
    unideps_detect_runtime(_unideps_runtime)

    set(_unideps_cmd
        "${UNIDEPS_EXECUTABLE}" build
        --manifest "${_UNIDEPS_MANIFEST}"
        --target "${_unideps_target}"
        --generate-targets "${_UNIDEPS_TARGET_FILE}"
    )
    if(_UNIDEPS_BASE_DIR)
        list(APPEND _unideps_cmd --base-dir "${_UNIDEPS_BASE_DIR}")
    endif()
    if(_UNIDEPS_PRESET)
        list(APPEND _unideps_cmd --preset "${_UNIDEPS_PRESET}")
    endif()
    if(CMAKE_BUILD_TYPE)
        list(APPEND _unideps_cmd --build-type "${CMAKE_BUILD_TYPE}")
    endif()
    if(NOT _unideps_runtime STREQUAL "")
        list(APPEND _unideps_cmd "--msvc-runtime=${_unideps_runtime}")
    endif()
    if(DEFINED BUILD_SHARED_LIBS)
        if(BUILD_SHARED_LIBS)
            list(APPEND _unideps_cmd --default-shared true)
        else()
            list(APPEND _unideps_cmd --default-shared false)
        endif()
    endif()
    if(CMAKE_C_COMPILER)
        list(APPEND _unideps_cmd --c-compiler "${CMAKE_C_COMPILER}")
    endif()
    if(CMAKE_CXX_COMPILER)
        list(APPEND _unideps_cmd --cxx-compiler "${CMAKE_CXX_COMPILER}")
    endif()

    set(_unideps_compiler_id "${CMAKE_C_COMPILER_ID}")
    if(NOT _unideps_compiler_id)
        set(_unideps_compiler_id "${CMAKE_CXX_COMPILER_ID}")
    endif()
    if(_unideps_compiler_id MATCHES "Clang")
        list(APPEND _unideps_cmd --compiler clang)
    elseif(_unideps_compiler_id STREQUAL "MSVC")
        list(APPEND _unideps_cmd --compiler msvc)
    elseif(_unideps_compiler_id STREQUAL "GNU")
        list(APPEND _unideps_cmd --compiler gcc)
    elseif(_unideps_compiler_id)
        string(TOLOWER "${_unideps_compiler_id}" _unideps_compiler_lower)
        list(APPEND _unideps_cmd --compiler "${_unideps_compiler_lower}")
    endif()

    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _unideps_cmd --toolchain-file "${CMAKE_TOOLCHAIN_FILE}")
    endif()

    foreach(_unideps_var ANDROID_ABI ANDROID_PLATFORM ANDROID_NDK ANDROID_STL CMAKE_ANDROID_ARCH_ABI)
        if(DEFINED ${_unideps_var} AND NOT "${${_unideps_var}}" STREQUAL "")
            list(APPEND _unideps_cmd "--cmake-args=-D${_unideps_var}=${${_unideps_var}}")
        endif()
    endforeach()

    _unideps_collect_options()
    foreach(_unideps_def IN LISTS _unideps_option_defs)
        list(APPEND _unideps_cmd "--cmake-args=-D${_unideps_def}")
    endforeach()

    execute_process(
        COMMAND ${_unideps_cmd}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _unideps_res
    )
    if(NOT _unideps_res EQUAL 0)
        message(FATAL_ERROR "UniDeps: 'unideps build' failed (exit code ${_unideps_res})")
    endif()

    if(EXISTS "${_UNIDEPS_TARGET_FILE}")
        include("${_UNIDEPS_TARGET_FILE}")
    else()
        message(FATAL_ERROR "UniDeps: generated targets file not found at ${_UNIDEPS_TARGET_FILE}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_UNIDEPS_MANIFEST}")
endmacro()

# unideps_setup([MANIFEST <path>] [BASE_DIR <dir>] [PRESET <name>] [TARGET_FILE <path>])
#
# Builds the dependencies from unideps.toml and includes the generated targets file.
# A macro (not a function) so that <Pkg>_ROOT / CMAKE_PREFIX_PATH set by the
# generated file are visible to the caller.
macro(unideps_setup)
    cmake_parse_arguments(_UNIDEPS "" "MANIFEST;BASE_DIR;PRESET;TARGET_FILE" "" ${ARGN})
    if(_UNIDEPS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "UniDeps: unknown arguments to unideps_setup(): ${_UNIDEPS_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT _UNIDEPS_MANIFEST)
        set(_UNIDEPS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/unideps.toml")
    endif()
    if(NOT _UNIDEPS_TARGET_FILE)
        set(_UNIDEPS_TARGET_FILE "${CMAKE_CURRENT_BINARY_DIR}/unideps_targets.cmake")
    endif()
    if(NOT _UNIDEPS_PRESET AND DEFINED UNIDEPS_PRESET)
        set(_UNIDEPS_PRESET "${UNIDEPS_PRESET}")
    endif()
    if(NOT _UNIDEPS_BASE_DIR AND DEFINED UNIDEPS_BASE_DIR)
        set(_UNIDEPS_BASE_DIR "${UNIDEPS_BASE_DIR}")
    endif()

    if(DEFINED ENV{UNIDEPS_ACTIVE})
        _unideps_setup_nested()
    else()
        _unideps_setup_build()
    endif()
endmacro()
