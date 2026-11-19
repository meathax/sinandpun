cmake_minimum_required(VERSION 3.25)

# Dawn's pinned macOS artifacts target 12.0. Set the same floor before project()
# initializes the Apple toolchain so direct developer CMake invocations cannot
# accidentally inherit the running SDK's deployment version. This cache entry
# is harmless on non-Apple platforms and remains overridable by a caller.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Minimum macOS version supported by WiiCompiled" FORCE)
endif()
project(mkw_recompiled)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WiiCompiled requires a 64-bit Clang toolchain")
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES MKW_OSX_ARCHITECTURE_COUNT)
    if(MKW_OSX_ARCHITECTURE_COUNT GREATER 1)
        message(FATAL_ERROR
            "WiiCompiled supports one macOS architecture per build directory; "
            "configure separate arm64 and x86_64 build directories")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 _mkw_osx_arch)
    set(CMAKE_SYSTEM_PROCESSOR "${_mkw_osx_arch}" CACHE STRING "Target processor architecture" FORCE)
    set(CMAKE_SYSTEM_PROCESSOR "${_mkw_osx_arch}")
endif()

if(WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(MKW_PLATFORM_WINDOWS TRUE)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|arm64|ARM64)$")
    set(MKW_PLATFORM_MACOS TRUE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        set(MKW_PLATFORM_MACOS_X86_64 TRUE)
    else()
        set(MKW_PLATFORM_MACOS_ARM64 TRUE)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|aarch64|arm64|ARM64)$")
    set(MKW_PLATFORM_LINUX TRUE)
else()
    message(FATAL_ERROR
        "WiiCompiled supports 64-bit LLVM-MinGW Clang on Windows, native Linux x86_64/aarch64, or Apple Clang on macOS x86_64/arm64")
endif()

if(MKW_PLATFORM_WINDOWS)
    # CMake and Ninja write response files with Windows quoting, but clang tokenizes them with
    # POSIX rules by default, where a bare apostrophe in a path opens a quoted span.
    add_link_options(--rsp-quoting=windows)
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "WiiCompiled only supports Release builds")
endif()

option(MKW_BUILD_PRODUCTS "Build translated WiiCompiled product targets" ON)
option(MKW_BUILD_PSQ_TESTS "Build focused PSQ ISA tests" OFF)
set(MKW_X86_CPU_PROFILE "v3" CACHE STRING
    "Minimum x86-64 CPU profile for translated products (v2 or v3)")
set_property(CACHE MKW_X86_CPU_PROFILE PROPERTY STRINGS v2 v3)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$" AND
   NOT MKW_X86_CPU_PROFILE MATCHES "^(v2|v3)$")
    message(FATAL_ERROR "MKW_X86_CPU_PROFILE must be v2 or v3 (got '${MKW_X86_CPU_PROFILE}')")
endif()

set(MKW_SETUP_VERSION "" CACHE STRING "WiiCompiled version recorded in the runtime log")

option(MKW_PLATFORM_TESTS_ONLY "Build host-platform tests without graphics dependencies or game assets" OFF)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HostLibraries.cmake")
if(MKW_PLATFORM_TESTS_ONLY)
    if(MKW_BUILD_PRODUCTS)
        message(FATAL_ERROR "MKW_PLATFORM_TESTS_ONLY requires MKW_BUILD_PRODUCTS=OFF")
    endif()
    add_compile_definitions(NOMINMAX)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PlatformTests.cmake")
    return()
endif()

# Preprocessor definitions that belong to this project's own code (the runtime,
# the translated shards and the product glue) and to nothing else. They are
# applied directory-scoped, immediately after the aurora add_subdirectory() call,
# so every target created afterwards (mkw_runtime_common, the translated shard
# libraries and the products in cmake/PublicProducts.cmake) inherits them while
# aurora-main and its third-party tree stay unaffected.
set(MKW_PROJECT_COMPILE_DEFINITIONS NOMINMAX)

set(CMAKE_CXX_STANDARD 17)

# Declared before the vendored libraries so the third-party block below can
# branch on it and so cmake/NativePrebuilt.cmake is included after the names it
# must provide are known.
set(MKW_NATIVE_PREBUILT_DIR "" CACHE PATH
    "Precompiled aurora/third-party package to link instead of building aurora-main")
# Maintainer-only counterpart: describe the configured aurora build so the
# package can be harvested from it.
set(MKW_NATIVE_PREBUILT_EXPORT_DIR "" CACHE PATH
    "Maintainer-only: write a description of the configured aurora build here")
option(MKW_USE_PREBUILT_RUNTIME
    "Link the native prebuilt package's runtime archive when it matches this workspace" ON)

# Runtime configuration is real TOML, parsed by toml11 rather than a project-
# specific line parser. Keep it header-only and vendored so disconnected release
# builds have exactly the same parser as developer builds.
add_library(mkw_toml11 INTERFACE)
add_library(mkw::toml11 ALIAS mkw_toml11)
target_include_directories(mkw_toml11 SYSTEM INTERFACE third_party/toml11)
target_compile_features(mkw_toml11 INTERFACE cxx_std_17)

set(MKW_TRANSLATED_COMPILE_JOBS 0 CACHE STRING
    "Cap on concurrently compiling translated shard TUs via a Ninja job pool (0 = uncapped). \
Scheduling only - never affects output bytes, so it is deliberately outside the canonical flag fingerprint.")
set(MKW_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Optional self-contained C++/WinRT include directory")

# Precompiled aurora + third-party package (see launcher/Prepare-NativePrebuilt.ps1).
# Empty - the default - keeps the from-source behaviour every developer build
# uses. When set, aurora-main and its extern/ tree are
# not compiled at all; imported targets with the same names are wired up from
# the package instead, so this project's own code compiles and links exactly
# as before.
set(MKW_AURORA_DIR "${CMAKE_CURRENT_LIST_DIR}/../aurora-main" CACHE PATH
    "Aurora source directory")
if(NOT EXISTS "${MKW_AURORA_DIR}/CMakeLists.txt"
        OR NOT EXISTS "${MKW_AURORA_DIR}/include/aurora/aurora.h")
    message(FATAL_ERROR
        "Aurora sources are missing at ${MKW_AURORA_DIR}. From the repository root, run "
        "git submodule update --init --recursive, or set MKW_AURORA_DIR to an initialized Aurora checkout.")
endif()

# The translator's explicit float casts are PPC single-precision rounding points.
# Fast-math may erase them and change guest-visible integer conversions.
set(MKW_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)

set(MKW_PPC_SEMANTIC_RUNTIME_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/fpu_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp")
set_source_files_properties(${MKW_PPC_SEMANTIC_RUNTIME_SOURCES} PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "${MKW_TRANSLATED_PPC_FP_OPTIONS}")
# Match the optimization policy of the translated callers as well as their FP policy.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp" APPEND PROPERTY
    COMPILE_OPTIONS -O2 -fno-slp-vectorize)

# ----------------------------------------------------------------------
# Third-party: aurora-main (provides SDL3 + GPU backends)
# ----------------------------------------------------------------------
if(MKW_NATIVE_PREBUILT_DIR)
    # Precompiled aurora + third-party archives. The source trees are still
    # present (the runtime includes aurora headers directly), they are simply
    # not compiled here.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuilt.cmake")
else()
    set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
    if(MKW_PLATFORM_WINDOWS)
        set(DAWN_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER ON CACHE BOOL "" FORCE)
    elseif(MKW_PLATFORM_MACOS)
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    else()
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    endif()
    set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # Provide a tiny stub for DXProgrammableCapture when the SDK/PIX headers are
    # missing (common on MinGW). Dawn only includes the header; no symbols are
    # referenced when PIX isn't present.
    if(MKW_PLATFORM_WINDOWS)
        set(MKW_DX_STUB_DIR "${CMAKE_BINARY_DIR}/aurora_dx_stubs")
        if(NOT EXISTS "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h")
            file(MAKE_DIRECTORY ${MKW_DX_STUB_DIR})
            file(WRITE "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h"
"#pragma once\n// Stubbed PIX capture header for Dawn; no functionality when PIX isn't present.\n")
        endif()
    endif()
    # Deliberately NOT injected project-wide. Only a from-source Dawn build ever
    # includes DXProgrammableCapture.h, and this tree consumes Dawn as a prebuilt
    # package (AURORA_DAWN_PROVIDER=package), so the old
    # include_directories()/add_compile_options(-I...) pair only added a dead
    # -I to every one of the ~500 third-party and runtime compile lines. The
    # aurora_* targets that do want it get it from the
    # target_include_directories() call further down; a from-source Dawn build
    # should add it to the Dawn targets the same way.

    # Keep SDL3 lean when aurora is enabled.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)

    # Aurora components the runtime does not use. They would otherwise fetch
    # extra dependencies at configure time, which offline builds cannot do.
    set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
    # The runtime implements the Wii SDK itself; Aurora stays on its GameCube SDK.
    set(AURORA_TARGET_RVL OFF CACHE BOOL "" FORCE)
    # Every dependency comes from the pinned sources, never the build host.
    set(AURORA_USE_SYSTEM_DEPENDENCIES OFF CACHE BOOL "" FORCE)

    # MinGW/WSL can choke on SDL precompiled headers; disable PCH for aurora/SDL.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    # Isolate aurora build products to a subdirectory under the main build dir.
    add_subdirectory(${MKW_AURORA_DIR} ${CMAKE_BINARY_DIR}/aurora-build EXCLUDE_FROM_ALL)
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

    # Keep upstream Aurora split: several sources intentionally use file-local
    # names that collide under this repo's aggressive unity build settings.
    set(AURORA_TARGETS aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi)
    foreach(t ${AURORA_TARGETS})
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES UNITY_BUILD OFF)
            target_compile_options(${t} PRIVATE -O3 -ffast-math -w -pipe)
            if(MKW_PLATFORM_WINDOWS)
                target_include_directories(${t} PRIVATE ${MKW_DX_STUB_DIR})
            endif()
            # Aurora's own sources include Windows headers and call std::min/max;
            # they relied on the old project-wide NOMINMAX that no longer leaks
            # into this subtree, so the define is applied per target here.
            target_compile_definitions(${t} PRIVATE NOMINMAX)
        endif()
    endforeach()

    # Frame interpolation filters guest NaN/Inf matrices out of a std::sort
    # comparator, which makes its std::isfinite guards load-bearing for memory
    # safety, not just match quality. -ffinite-math-only (part of -ffast-math
    # above) folds those guards to constants and poisons NaN float arguments
    # via nofpclass, so this one file opts back out of the finite-math
    # assumption; the file #errors if built without this. Source-scoped
    # COMPILE_OPTIONS append after the target options, so this overrides the
    # target's -ffast-math for exactly this TU.
    if(TARGET aurora_gx)
        set_source_files_properties("${MKW_AURORA_DIR}/lib/gx/frame_interpolation.cpp"
            TARGET_DIRECTORY aurora_gx
            PROPERTIES COMPILE_OPTIONS "-fno-finite-math-only")
    endif()
endif()

# Everything from here down is this project's own code, so this is where the
# project definitions enter the directory scope. A directory-scoped definition
# seeds the COMPILE_DEFINITIONS of targets as they are created, which means:
#   * aurora-main and its third-party tree were added above -> unaffected;
#   * the runtime library, the translated shard libraries and the products in
#     cmake/RuntimeLibrary.cmake and cmake/PublicProducts.cmake are all created
#     below (those files are include()d, not add_subdirectory()d, so they share
#     this directory scope) -> they pick the definitions up exactly as before.
if(MKW_PROJECT_COMPILE_DEFINITIONS)
    add_compile_definitions(${MKW_PROJECT_COMPILE_DEFINITIONS})
endif()

# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build (measured
# at ~55 ms across the 100+ globs this build already re-verifies for SDL), and it
# buys correctness: without it, a newly added src/*.cpp that is not referenced by
# a registration file is silently never compiled and never errors. The stale-glob
# failure mode is worth far more than the milliseconds.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
if(MKW_PLATFORM_MACOS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory.cpp")
    if(MKW_PLATFORM_MACOS_ARM64)
        # HostContext's Apple Silicon backend is implemented in a small assembly
        # companion. It must be part of the product runtime as well as the
        # standalone context test; otherwise the final executable is missing
        # mkw_co_init/mkw_co_switch at link time.
        enable_language(ASM)
        list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
    endif()
else()
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
endif()
set(MKW_PLATFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/platform/host_platform.cpp")
set(MKW_BASE_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/base_product.cpp")
# The host ISA guard is the one translation unit that must not receive the
# x86-64-v3 flags, so it gets its own object library instead of riding along in
# the runtime library. See cmake/PublicProducts.cmake and the file's own header.
set(MKW_CPU_BASELINE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/host_cpu_baseline.cpp")
list(REMOVE_ITEM SOURCES ${MKW_BASE_PRODUCT_SOURCE}
     ${MKW_CPU_BASELINE_SOURCE} ${MKW_PLATFORM_SOURCE})

include("${CMAKE_CURRENT_LIST_DIR}/cmake/PlatformTests.cmake")

set(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/RuntimeLibrary.cmake")

# The prebuilt export also carries the game-independent runtime as a static
# archive (x86_64 only; AArch64 compiles it locally with -mcpu=native). It is
# defined here, after the project definitions, so its flags match the products'.
if(MKW_NATIVE_PREBUILT_EXPORT_DIR AND NOT MKW_NATIVE_PREBUILT_DIR)
    if(MKW_HOST_IS_X86_64)
        mkw_add_runtime_library(mkw_runtime STATIC ${SOURCES})
        set_target_properties(mkw_runtime PROPERTIES EXCLUDE_FROM_ALL ON)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuiltExport.cmake")
endif()

# The translator emits the complete, content-addressed source graph. Consuming
# this one manifest keeps configure independent of the 28k generated function
# files.
if(MKW_BUILD_PRODUCTS)
    set(MKW_TRANSLATED_SHARD_MANIFEST
        "${CMAKE_CURRENT_LIST_DIR}/../generated/build_shards/shards.cmake"
        CACHE FILEPATH "Translator-owned aggregate shard manifest")
    # The prebuilt export only needs the aurora/third-party closure and the runtime
    # configured above, so a packaging machine without a translation stops here.
    if(MKW_NATIVE_PREBUILT_EXPORT_DIR AND NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(STATUS "No translator shard manifest; configuring the native prebuilt export only")
        return()
    endif()
    if(NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(FATAL_ERROR
            "Missing translator-owned shard manifest: ${MKW_TRANSLATED_SHARD_MANIFEST}. "
            "Run Translator.Cli emit-build-shards first; see translator/README.md.")
    endif()
    include("${MKW_TRANSLATED_SHARD_MANIFEST}")
    message(STATUS
        "Translator graph: ${MKW_SHARED_BASE_FUNCTION_COUNT}/${MKW_BASE_FUNCTION_COUNT} base functions shared; "
        "${MKW_PROFILE_SENSITIVE_CALLER_COUNT} profile-sensitive callers")

    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PublicProducts.cmake")
else()
    if(MKW_PLATFORM_MACOS)
        # Compile-only audit of native runtime sources. It deliberately avoids
        # translated products until their host dependencies are portable.
        # runtime/src is game-independent; per-game glue lives in runtime/game.
        add_library(mkw_macos_native_compile OBJECT ${SOURCES})
        target_include_directories(mkw_macos_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${MKW_AURORA_DIR}/include")
        target_compile_features(mkw_macos_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(mkw_macos_native_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
        target_link_libraries(mkw_macos_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            mkw::toml11)
        if(MKW_PLATFORM_MACOS_X86_64)
            target_link_libraries(mkw_macos_native_compile PRIVATE mkw::libco)
            target_compile_options(mkw_macos_native_compile PRIVATE
                -march=x86-64-${MKW_X86_CPU_PROFILE})
            if(MKW_X86_CPU_PROFILE STREQUAL "v2")
                target_compile_definitions(mkw_macos_native_compile PRIVATE MKW_X86_CPU_PROFILE_V2=1)
            endif()
        endif()
        set_target_properties(mkw_macos_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    add_custom_target(mkw_platform_paths_check DEPENDS mkw_platform)
    message(STATUS "Translated product targets disabled (MKW_BUILD_PRODUCTS=OFF)")
endif()
