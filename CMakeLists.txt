cmake_minimum_required(VERSION 3.10)

# Set the project name
project(OnPair)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include CheckCXXCompilerFlag to detect compiler support for flags
include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

# Set a default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Release" FORCE)
endif()

# Define include directories
set(PROJECT_INCLUDE_DIRS
    include
    external/robin_hood
)

# Create the OnPair library
set(ONPAIR_SOURCES
    src/onpair.cpp
    src/onpair16.cpp
)

add_library(onpair_lib STATIC ${ONPAIR_SOURCES})
target_include_directories(onpair_lib PUBLIC ${PROJECT_INCLUDE_DIRS})

# Build examples
file(GLOB PROJECT_EXECUTABLES examples/*.cpp)

# Create targets with common properties
foreach(executable_path ${PROJECT_EXECUTABLES})
    # Get the target name by removing the path and the extension
    get_filename_component(executable_name ${executable_path} NAME_WE)
    
    # Create the executable target
    add_executable(${executable_name} ${executable_path})
    
    # Include directories and link library
    target_include_directories(${executable_name} PRIVATE ${PROJECT_INCLUDE_DIRS})
    target_link_libraries(${executable_name} onpair_lib)
endforeach()

# Build tests
file(GLOB TEST_SOURCES tests/*.cpp)

foreach(test_path ${TEST_SOURCES})
    # Get the test name by removing the path and the extension
    get_filename_component(test_name ${test_path} NAME_WE)
    
    # Create the test executable
    add_executable(${test_name} ${test_path})
    
    # Include directories and link library
    target_include_directories(${test_name} PRIVATE ${PROJECT_INCLUDE_DIRS})
    target_link_libraries(${test_name} onpair_lib)
endforeach()

# Define a reusable variable for optimization flags
if(NOT MSVC)
    set(OPTIMIZATION_FLAGS "-O3 -DNDEBUG -flto")
else()
    set(OPTIMIZATION_FLAGS "/O2 /DNDEBUG /GL")
endif()

# Set release-specific compiler and linker flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OPTIMIZATION_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${OPTIMIZATION_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${OPTIMIZATION_FLAGS}")

# Enable inter-procedural optimizations if supported (modern CMake)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    foreach(executable_path ${PROJECT_EXECUTABLES})
        # Get the target name by removing the path and the extension
        get_filename_component(executable_name ${executable_path} NAME_WE)
        
        # Enable IPO (LTO) for the target
        set_property(TARGET ${executable_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endforeach()
else()
    message(WARNING "IPO (LTO) is not supported: ${ipo_error}")
endif()

# Check for `-march=native` support and append it if supported
check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
if(COMPILER_SUPPORTS_MARCH_NATIVE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
endif()

# Specify output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
