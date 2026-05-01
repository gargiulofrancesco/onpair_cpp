cmake_minimum_required(VERSION 3.21)

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Do not install GTest targets into the project's install tree.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ── Helper macro ───────────────────────────────────────────────────────────────
# Each test file becomes its own executable so a crash in one file never
# prevents other files from running.  gtest_discover_tests registers each
# individual TEST() case as a separate CTest entry, enabling granular reruns
# with:   ctest --test-dir build -R test_lpm.Insert.SingleByte
function(onpair_test src)
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name}
        PRIVATE
            onpair
            GTest::gtest_main
            GTest::gmock
    )
    target_include_directories(${name}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/helpers
    )
    gtest_discover_tests(${name}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        TEST_PREFIX "${name}."
        PROPERTIES TIMEOUT 60
    )
endfunction()

# ── Core ───────────────────────────────────────────────────────────────────────
onpair_test(core/test_types.cpp)
onpair_test(core/test_dictionary.cpp)
onpair_test(core/test_store.cpp)
onpair_test(core/test_store_view.cpp)
onpair_test(core/test_dictionary_view.cpp)

# ── Encoding ───────────────────────────────────────────────────────────────────
onpair_test(encoding/test_lpm.cpp)
onpair_test(encoding/test_bit_writer.cpp)
onpair_test(encoding/test_trainer.cpp)
onpair_test(encoding/test_parser.cpp)

# ── Decoding ───────────────────────────────────────────────────────────────────
onpair_test(decoding/test_token_cursor.cpp)
onpair_test(decoding/test_decode_all.cpp)
onpair_test(decoding/test_decode_all_offsets.cpp)
onpair_test(decoding/test_decoder.cpp)

# ── Search ────────────────────────────────────────────────────────────────────
onpair_test(search/test_kmp_automaton.cpp)
onpair_test(search/test_prefix_search.cpp)
onpair_test(search/test_aho_corasick_automaton.cpp)
onpair_test(search/test_aho_corasick_lazy_automaton.cpp)
onpair_test(search/test_aho_corasick_online_automaton.cpp)
onpair_test(search/test_eq_search.cpp)
onpair_test(search/test_eq_automaton.cpp)
onpair_test(search/test_prefix_automaton.cpp)
onpair_test(search/test_combinators.cpp)
onpair_test(search/test_tokenize.cpp)

# ── Integration ────────────────────────────────────────────────────────────────
onpair_test(integration/test_roundtrip.cpp)
onpair_test(integration/test_serialization.cpp)
onpair_test(integration/test_column_api.cpp)
