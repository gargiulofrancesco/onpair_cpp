# Examples are intentionally simple: each .cpp in this directory becomes
# a standalone executable linked against OnPair::onpair. They are *not*
# installed — they exist only to demonstrate API usage.

file(GLOB ONPAIR_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(_src IN LISTS ONPAIR_EXAMPLE_SOURCES)
    get_filename_component(_name ${_src} NAME_WE)
    add_executable(${_name} ${_src})
    target_link_libraries(${_name} PRIVATE OnPair::onpair)
    set_target_properties(${_name} PROPERTIES FOLDER "OnPair/examples")
endforeach()
