# ──────────────────────────────────────────────────────────────────────────────
#  OnPair — a high-performance string compression library
# ──────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.21...3.30)

project(OnPair
    VERSION      0.1.0
    DESCRIPTION  "Short strings compression for fast random and compressed pattern matching"
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

option(ONPAIR_BUILD_TESTS    "Build the OnPair test suite"          OFF)
option(ONPAIR_BUILD_EXAMPLES "Build the OnPair example programs"    ${_onpair_default_examples})
option(ONPAIR_INSTALL        "Generate install rules for OnPair"    ${_onpair_default_install})
option(ONPAIR_WARNINGS       "Enable a strict warning set on OnPair" ${_onpair_default_warnings})

# Performance flags. Both are PRIVATE and opt-in:
#   - -march=native bakes host-CPU instructions into objects, which breaks
#     distro builds where build host ≠ user machine.
#   - LTO objects in installed static archives can require a compatible
#     whole-program link from consumers.
# Final benchmark/application targets must apply these too — most hot-path
# code lives in headers and is instantiated by the consumer.
option(ONPAIR_NATIVE_ARCH "Build with -march=native (binary not portable)" OFF)
option(ONPAIR_ENABLE_LTO  "Enable interprocedural optimisation (LTO/IPO)"  OFF)

# Default to Release only at top-level under a single-config generator.
get_property(_onpair_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND NOT _onpair_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Centralise output layout only at top-level; consumers keep control of theirs.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

include(GNUInstallDirs)
include("${PROJECT_SOURCE_DIR}/cmake/OnPairTargetHelpers.cmake")

# ──────────────────────────────────────────────────────────────────────────────
#  Dependencies
# ──────────────────────────────────────────────────────────────────────────────
#  Boost.Unordered ≥ 1.81 is required — boost::unordered_flat_map landed in
#  Boost 1.81 and the API has been stable since. Prefer a system install;
#  fall back to FetchContent only when the system copy is missing or older.
# ──────────────────────────────────────────────────────────────────────────────

find_package(Boost 1.81 QUIET CONFIG COMPONENTS unordered)
set(_onpair_boost_fetched FALSE)
if(NOT Boost_FOUND)
    message(STATUS "OnPair: system Boost.Unordered ≥ 1.81 not found — fetching upstream")
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

# Fetched Boost targets are not part of any export set, so install(EXPORT)
# would fail at generate time. A working install requires a system Boost
# that downstream find_package(OnPair) consumers can also resolve.
if(ONPAIR_INSTALL AND _onpair_boost_fetched)
    message(STATUS
        "OnPair: disabling install rules — install requires a system "
        "Boost.Unordered ≥ 1.81 (currently fetched via FetchContent).")
    set(ONPAIR_INSTALL OFF)
endif()

# ──────────────────────────────────────────────────────────────────────────────
#  Library target
# ──────────────────────────────────────────────────────────────────────────────
#  Hot-path code (decoder, scan loop, automata) lives in headers. Only the
#  non-template translation units are compiled into the archive.
#
#  OnPair is intentionally STATIC-only. Most of the hot path is templated
#  and instantiated in the consumer's translation units, so a SHARED
#  libonpair would ship only a thin shell of the library. The supported
#  deployment is a STATIC + PIC archive linked into the consumer's host
#  binary or DSO. BUILD_SHARED_LIBS=ON is rejected at configure time so a
#  parent project that toggles it globally does not silently produce a
#  broken libonpair.so.
# ──────────────────────────────────────────────────────────────────────────────

if(BUILD_SHARED_LIBS)
    message(FATAL_ERROR
        "OnPair does not support BUILD_SHARED_LIBS=ON. The library is "
        "intentionally STATIC: most of the hot path is header-only and "
        "instantiated in the consumer, so a shared libonpair would not "
        "carry the performance-critical code. Consume OnPair::onpair as "
        "a STATIC archive linked into your final binary or DSO. "
        "See README §Integration for details.")
endif()

set(ONPAIR_SOURCES
    src/onpair/column/column.cpp
    src/onpair/core/dictionary_view.cpp
    src/onpair/encoding/parsing/parser.cpp
    src/onpair/encoding/training/trainer.cpp
)

add_library(onpair STATIC ${ONPAIR_SOURCES})
add_library(OnPair::onpair ALIAS onpair)

target_compile_features(onpair PUBLIC cxx_std_20)

target_include_directories(onpair PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(onpair PUBLIC Boost::unordered)

set_target_properties(onpair PROPERTIES
    POSITION_INDEPENDENT_CODE ON       # safe to link into a host DSO
    CXX_VISIBILITY_PRESET     hidden   # clean symbol table when embedded
    VISIBILITY_INLINES_HIDDEN ON
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

# ── Opt-in performance settings (PRIVATE; never propagated) ──────────────────
# Benchmark targets should call onpair_optimize_target(<target>) — header-only
# hot paths are instantiated in the final target, so flags applied only here
# do not affect the consumer's code generation.
onpair_apply_configured_optimizations(onpair)

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

    install(FILES "${PROJECT_SOURCE_DIR}/cmake/OnPairTargetHelpers.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OnPair
    )

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/OnPairConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/OnPairConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OnPair
    )

    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/OnPairConfigVersion.cmake"
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        "${PROJECT_BINARY_DIR}/OnPairConfig.cmake"
        "${PROJECT_BINARY_DIR}/OnPairConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OnPair
    )
endif()
