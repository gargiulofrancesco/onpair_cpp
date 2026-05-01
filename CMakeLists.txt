# ──────────────────────────────────────────────────────────────────────────────
#  OnPair — a high-performance string compression library
# ──────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.21)

project(OnPair
    VERSION      0.1.0
    DESCRIPTION
        "Short strings compression for fast random and compressed pattern matching"
    HOMEPAGE_URL "https://github.com/gargiulofrancesco/onpair_cpp"
    LANGUAGES    CXX
)

# ──────────────────────────────────────────────────────────────────────────────
#  Project-level configuration
# ──────────────────────────────────────────────────────────────────────────────
#  OnPair supports three consumption modes: stand-alone build, add_subdirectory,
#  and find_package. Defaults below differ for top-level vs. embedded builds —
#  a parent project never inherits our tests, examples, install rules, or
#  CPU-specific flags.
# ──────────────────────────────────────────────────────────────────────────────

if(PROJECT_IS_TOP_LEVEL)
    set(_onpair_default_examples ON)
    set(_onpair_default_install  ON)
    set(_onpair_default_warnings ON)
else()
    set(_onpair_default_examples OFF)
    set(_onpair_default_install  OFF)
    set(_onpair_default_warnings OFF)
endif()

option(ONPAIR_BUILD_TESTS    "Build the OnPair test suite"                  OFF)
option(ONPAIR_BUILD_EXAMPLES "Build the OnPair example programs"            ${_onpair_default_examples})
option(ONPAIR_INSTALL        "Generate install rules for OnPair"            ${_onpair_default_install})
option(ONPAIR_WARNINGS       "Enable a strict warning set on OnPair"        ${_onpair_default_warnings})

# Performance flags. -march=native stays opt-in: it bakes host-CPU instructions
# into the archive, which breaks distro builds where the build host differs
# from the user's machine. LTO defaults ON for top-level only; embedded builds
# leave the policy to the parent. LTO is scoped per-config below so Debug
# (sanitizer, coverage) builds never produce bitcode archives.
option(ONPAIR_NATIVE_ARCH    "Build with -march=native (binary not portable)"  OFF)
option(ONPAIR_ENABLE_LTO     "Enable interprocedural optimisation (LTO/IPO)"   ${PROJECT_IS_TOP_LEVEL})

# Default to Release only at top-level under a single-config generator.
# Multi-config generators (Ninja Multi-Config, Visual Studio, Xcode) ignore
# CMAKE_BUILD_TYPE.
get_property(_onpair_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND NOT _onpair_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Centralise executables in build/bin only at top-level; consumers keep
# control of their own output layout.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)
include(GNUInstallDirs)

# ──────────────────────────────────────────────────────────────────────────────
#  Dependencies
# ──────────────────────────────────────────────────────────────────────────────

# Prefer a system-installed Boost.Unordered; fetch upstream only as a fallback.
find_package(Boost 1.91 QUIET CONFIG COMPONENTS unordered)
set(_onpair_boost_fetched FALSE)
if(NOT Boost_FOUND)
    message(STATUS "OnPair: system Boost.Unordered not found — fetching upstream")
    include(FetchContent)
    set(BOOST_INCLUDE_LIBRARIES unordered)
    set(BOOST_ENABLE_CMAKE ON)
    FetchContent_Declare(
        Boost
        URL      https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
        URL_HASH SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Boost)
    set(_onpair_boost_fetched TRUE)
endif()

# Disable install when Boost was fetched: the fetched targets are not in any
# export set, so install(EXPORT) would fail at generate time. A working
# install requires a system Boost.Unordered that find_package(OnPair)
# consumers can also resolve.
if(ONPAIR_INSTALL AND _onpair_boost_fetched)
    message(STATUS
        "OnPair: disabling install rules — install requires a system "
        "Boost.Unordered ≥ 1.91 (currently fetched via FetchContent).")
    set(ONPAIR_INSTALL OFF)
endif()

# ──────────────────────────────────────────────────────────────────────────────
#  Library target
# ──────────────────────────────────────────────────────────────────────────────
#  Hot-path code (decoder, scan loop, automata) lives in headers. Only
#  non-template translation units are compiled into the archive.
# ──────────────────────────────────────────────────────────────────────────────

set(ONPAIR_SOURCES
    src/onpair/column/column.cpp
    src/onpair/core/dictionary_view.cpp
    src/onpair/encoding/parsing/parser.cpp
    src/onpair/encoding/training/trainer.cpp
)

# Library type follows BUILD_SHARED_LIBS; consumers may override.
add_library(onpair ${ONPAIR_SOURCES})
add_library(OnPair::onpair ALIAS onpair)

target_compile_features(onpair PUBLIC cxx_std_20)

target_include_directories(onpair PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(onpair PUBLIC Boost::unordered)

set_target_properties(onpair PROPERTIES
    POSITION_INDEPENDENT_CODE ON       # allow linking into shared libs
    CXX_VISIBILITY_PRESET     hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION                   ${PROJECT_VERSION}
    SOVERSION                 ${PROJECT_VERSION_MAJOR}
)

# ── Strict warning set (PRIVATE — never propagated to consumers) ──────────────
if(ONPAIR_WARNINGS)
    if(MSVC)
        target_compile_options(onpair PRIVATE /W4 /permissive-)
    else()
        target_compile_options(onpair PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor
            -Wold-style-cast -Woverloaded-virtual
            -Wcast-align -Wunused -Wdouble-promotion
        )
    endif()
endif()

# ── -march=native (PRIVATE; opt-in) ───────────────────────────────────────────
# PRIVATE so consumers never inherit a host-CPU flag on their own TUs. Hot-path
# code is header-only, so consumers' own architecture flags drive its codegen.
if(ONPAIR_NATIVE_ARCH AND NOT MSVC)
    check_cxx_compiler_flag("-march=native" ONPAIR_HAS_MARCH_NATIVE)
    if(ONPAIR_HAS_MARCH_NATIVE)
        target_compile_options(onpair PRIVATE -march=native)
    endif()
endif()

# ── Link-Time Optimisation (LTO / IPO) ────────────────────────────────────────
# Per-config so Debug-mode sanitizer/coverage builds never produce LLVM bitcode
# archives, which some linkers (notably GNU ld) reject at link time.
if(ONPAIR_ENABLE_LTO)
    check_ipo_supported(RESULT _onpair_ipo_ok OUTPUT _onpair_ipo_err)
    if(_onpair_ipo_ok)
        set_target_properties(onpair PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON
        )
    else()
        message(STATUS "OnPair: LTO requested but unsupported: ${_onpair_ipo_err}")
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
#  Examples & tests
# ──────────────────────────────────────────────────────────────────────────────

if(ONPAIR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ONPAIR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ──────────────────────────────────────────────────────────────────────────────
#  Install & package config
# ──────────────────────────────────────────────────────────────────────────────

if(ONPAIR_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS onpair
        EXPORT  OnPairTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
            PATTERN "*.hpp"
            PATTERN "*.h"
            PATTERN "*.ipp"
    )

    install(EXPORT OnPairTargets
        FILE        OnPairTargets.cmake
        NAMESPACE   OnPair::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OnPair
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OnPairConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OnPairConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OnPair
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OnPairConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/OnPairConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/OnPairConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OnPair
    )
endif()
