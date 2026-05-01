include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

function(_onpair_require_build_target target)
    if("${target}" STREQUAL "")
        message(FATAL_ERROR "OnPair: expected a target name")
    endif()

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "OnPair: unknown target '${target}'")
    endif()

    get_target_property(_onpair_imported "${target}" IMPORTED)
    if(_onpair_imported)
        message(FATAL_ERROR
            "OnPair: '${target}' is an imported target. Pass the final "
            "benchmark/application target instead.")
    endif()

    get_target_property(_onpair_type "${target}" TYPE)
    if(_onpair_type STREQUAL "INTERFACE_LIBRARY")
        message(FATAL_ERROR
            "OnPair: '${target}' is an interface target. Pass a build target "
            "such as an executable, static library, or shared library.")
    endif()
endfunction()

function(_onpair_check_ipo_supported_once)
    if(DEFINED _ONPAIR_IPO_SUPPORTED)
        return()
    endif()

    check_ipo_supported(RESULT _onpair_ipo_supported OUTPUT _onpair_ipo_error)
    set(_ONPAIR_IPO_SUPPORTED "${_onpair_ipo_supported}" CACHE INTERNAL
        "Whether IPO/LTO is supported for OnPair helper functions")
    set(_ONPAIR_IPO_ERROR "${_onpair_ipo_error}" CACHE INTERNAL
        "IPO/LTO support check output for OnPair helper functions")
endfunction()

function(onpair_enable_lto target)
    _onpair_require_build_target("${target}")
    _onpair_check_ipo_supported_once()

    if(_ONPAIR_IPO_SUPPORTED)
        set_target_properties("${target}" PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON
        )
    else()
        if(NOT _ONPAIR_IPO_MESSAGE_EMITTED)
            message(STATUS "OnPair: LTO requested but unsupported: ${_ONPAIR_IPO_ERROR}")
            set(_ONPAIR_IPO_MESSAGE_EMITTED TRUE CACHE INTERNAL
                "Whether OnPair already reported unsupported IPO/LTO")
        endif()
    endif()
endfunction()

function(onpair_enable_native_codegen target)
    _onpair_require_build_target("${target}")

    if(MSVC)
        return()
    endif()

    check_cxx_compiler_flag("-march=native" ONPAIR_HAS_MARCH_NATIVE)
    if(ONPAIR_HAS_MARCH_NATIVE)
        target_compile_options("${target}" PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-march=native>
        )
    else()
        message(STATUS
            "OnPair: native code generation requested for '${target}', "
            "but the compiler does not support -march=native")
    endif()
endfunction()

function(onpair_optimize_target target)
    onpair_enable_native_codegen("${target}")
    onpair_enable_lto("${target}")
endfunction()

function(onpair_apply_configured_optimizations target)
    if(ONPAIR_NATIVE_ARCH)
        onpair_enable_native_codegen("${target}")
    endif()

    if(ONPAIR_ENABLE_LTO)
        onpair_enable_lto("${target}")
    endif()
endfunction()
