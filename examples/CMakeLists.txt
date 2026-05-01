# Examples are intentionally simple: each .cpp in this directory becomes
# a standalone executable linked against OnPair::onpair. They are *not*
# installed — they exist only to demonstrate API usage.

file(GLOB ONPAIR_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(_src IN LISTS ONPAIR_EXAMPLE_SOURCES)
    get_filename_component(_name ${_src} NAME_WE)
    set(_target onpair_example_${_name})
    add_executable(${_target} ${_src})
    onpair_apply_configured_optimizations(${_target})
    target_link_libraries(${_target} PRIVATE OnPair::onpair)
    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME ${_name}
        FOLDER "OnPair/examples")
endforeach()
