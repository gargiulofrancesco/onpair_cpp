cmake_minimum_required(VERSION 3.16)
project(OnPair LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Boost Dependency ───────────────────────────────────────────────────────────
find_package(Boost 1.91 QUIET CONFIG COMPONENTS unordered)
if(NOT Boost_FOUND)
    message(STATUS "System Boost.Unordered not found — fetching from upstream")
    include(FetchContent)
    set(BOOST_INCLUDE_LIBRARIES unordered)
    set(BOOST_ENABLE_CMAKE ON)
    FetchContent_Declare(
        Boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
        URL_HASH SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Boost)
endif()

# ── Include paths ──────────────────────────────────────────────────────────────
set(ONPAIR_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── Library ────────────────────────────────────────────────────────────────────
# All hot-path code lives in headers (templates + inline functions).
# Only non-template .cpp files are compiled into the library target.
file(GLOB_RECURSE ONPAIR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/onpair/**/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/onpair/*.cpp
)

add_library(onpair STATIC ${ONPAIR_SOURCES})
target_include_directories(onpair PUBLIC ${ONPAIR_INCLUDE_DIRS})
target_compile_features(onpair PUBLIC cxx_std_20)

target_link_libraries(onpair PUBLIC Boost::unordered)

# ── Compiler optimisation flags ────────────────────────────────────────────────
if(NOT MSVC)
    target_compile_options(onpair PUBLIC $<$<CONFIG:Release>:-O3 -DNDEBUG>)
    check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
    if(HAS_MARCH_NATIVE)
        target_compile_options(onpair PUBLIC -march=native)
    endif()
else()
    target_compile_options(onpair PUBLIC $<$<CONFIG:Release>:/O2 /DNDEBUG>)
endif()

# ── Link-time optimisation ─────────────────────────────────────────────────────
check_ipo_supported(RESULT IPO_OK OUTPUT IPO_ERR)
if(IPO_OK)
    set_target_properties(onpair PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
    message(STATUS "IPO not supported: ${IPO_ERR}")
endif()

# ── Helper macro for executables ───────────────────────────────────────────────
function(onpair_executable src)
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE onpair)
    if(IPO_OK)
        set_target_properties(${name} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

# ── Examples ───────────────────────────────────────────────────────────────────
file(GLOB EXAMPLE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp)
foreach(src ${EXAMPLE_SOURCES})
    onpair_executable(${src})
endforeach()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Tests ──────────────────────────────────────────────────────────────────────
option(ONPAIR_BUILD_TESTS      "Build the test suite"  OFF)
if(ONPAIR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
