#include <onpair/api.h>
#include <onpair/search/automata/aho_corasick_online_automaton.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"

namespace op = onpair;
using namespace test_helpers;

static op::OnPairColumn make_column(const std::vector<std::string>& strings) {
    op::encoding::TrainingConfig cfg;
    cfg.seed = 42;

    return op::OnPairColumn::compress(strings, cfg);
}

// Helper: multi-pattern search via ColumnView using the online automaton.
static std::vector<size_t> contains_any_online(const op::OnPairColumn& col,
                                               std::span<const std::string_view> patterns) {
    auto v = col.view();
    op::search::AhoCorasickOnlineAutomaton ac(patterns, v.dictionary());
    return v.scan(ac);
}

// Helper: multi-pattern search via the eager automaton (reference).
static std::vector<size_t> contains_any_eager(const op::OnPairColumn& col,
                                              std::span<const std::string_view> patterns) {
    auto v = col.view();
    op::search::AhoCorasickAutomaton ac(patterns, v.dictionary());
    return v.scan(ac);
}

// -- Concept satisfaction -----------------------------------------------------

TEST(AhoCorasickOnlineTest, SatisfiesTokenAutomatonConcept) {
    static_assert(op::search::TokenAutomaton<op::search::AhoCorasickOnlineAutomaton>);
}

TEST(AhoCorasickOnlineTest, SatisfiesDeadDetectable) {
    static_assert(op::search::DeadDetectable<op::search::AhoCorasickOnlineAutomaton>);
}

// -- Basic multi-pattern search -----------------------------------------------

TEST(AhoCorasickOnlineTest, BasicMultiPattern) {
    std::vector<std::string> data = {
        "error: disk full",
        "warning: low memory",
        "info: all ok",
        "fatal: kernel panic",
        "debug: trace",
    };
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"error", "fatal"};
    auto result = contains_any_online(col, patterns);
    std::vector<size_t> expected = {0, 3};
    EXPECT_EQ(result, expected);
}

TEST(AhoCorasickOnlineTest, SinglePattern) {
    std::vector<std::string> data = {"abc", "def", "abc_xyz"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc"};
    auto result = contains_any_online(col, patterns);
    std::vector<size_t> expected = {0, 2};
    EXPECT_EQ(result, expected);
}

TEST(AhoCorasickOnlineTest, NoMatches) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"xyz", "uvw"};
    auto result = contains_any_online(col, patterns);
    EXPECT_TRUE(result.empty());
}

TEST(AhoCorasickOnlineTest, AllStringsMatch) {
    std::vector<std::string> data = {"abc_def", "def_ghi", "ghi_abc"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc", "def", "ghi"};
    auto result = contains_any_online(col, patterns);
    EXPECT_EQ(result.size(), 3u);
}

// -- Empty pattern ------------------------------------------------------------

TEST(AhoCorasickOnlineTest, EmptyPatternMatchesAll) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {""};
    auto result = contains_any_online(col, patterns);
    EXPECT_EQ(result.size(), 2u);
}

TEST(AhoCorasickOnlineTest, EmptyPatternSetMatchesNone) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns;
    auto result = contains_any_online(col, patterns);
    EXPECT_EQ(result.size(), 0u);
}

// -- Overlapping patterns -----------------------------------------------------

TEST(AhoCorasickOnlineTest, OverlappingPatterns) {
    std::vector<std::string> data = {"abcdef", "bcde", "xyz"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc", "bcd"};
    auto result = contains_any_online(col, patterns);
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

TEST(AhoCorasickOnlineTest, PrefixPatterns) {
    std::vector<std::string> data = {"abc", "ab", "xyz"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"ab", "abc"};
    auto result = contains_any_online(col, patterns);
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

// -- Generic scan API ---------------------------------------------------------

TEST(AhoCorasickOnlineTest, GenericScanApi) {
    std::vector<std::string> data = {"abc_xyz", "def_uvw", "abc_def"};
    auto col = make_column(data);
    auto v = col.view();

    std::vector<std::string_view> patterns = {"abc", "uvw"};
    op::search::AhoCorasickOnlineAutomaton ac(patterns, v.dictionary());
    auto result = v.scan(ac);
    std::vector<size_t> expected = {0, 1, 2};
    EXPECT_EQ(result, expected);
}

// -- Various bit widths -------------------------------------------------------

TEST(AhoCorasickOnlineTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {"error log", "warning log", "info log"};
    for (int b : {12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 42;
        auto col = op::OnPairColumn::compress(data, cfg);
        std::vector<std::string_view> patterns = {"error", "warning"};
        auto result = contains_any_online(col, patterns);
        EXPECT_EQ(result.size(), 2u) << "bit-width " << b;
    }
}

// -- Consistency with KMP -----------------------------------------------------

TEST(AhoCorasickOnlineTest, SinglePatternMatchesKmp) {
    auto data = make_user_strings(50);
    auto col = make_column(data);

    auto kmp_result = col.view().contains("user_0000");
    std::vector<std::string_view> patterns = {"user_0000"};
    auto online_result = contains_any_online(col, patterns);

    EXPECT_EQ(kmp_result, online_result);
}

// -- Consistency with eager AhoCorasickAutomaton ------------------------------

TEST(AhoCorasickOnlineTest, MatchesEagerOnBasicCorpus) {
    std::vector<std::string> data = {
        "error: disk full",
        "warning: low memory",
        "info: all ok",
        "fatal: kernel panic",
        "debug: trace",
        "error: out of memory",
        "info: started",
    };
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"error", "fatal", "memory"};
    EXPECT_EQ(contains_any_online(col, patterns),
              contains_any_eager(col, patterns));
}

TEST(AhoCorasickOnlineTest, MatchesEagerOnUserStrings) {
    auto data = make_user_strings(200);
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"user_0001", "user_0050", "user_0199", "xyz"};
    EXPECT_EQ(contains_any_online(col, patterns),
              contains_any_eager(col, patterns));
}

TEST(AhoCorasickOnlineTest, MatchesEagerOnRandomCorpus) {
    auto data = make_random_strings(300, 40, 12345);
    auto col = make_column(data);

    // Pick a few substrings that actually appear in the data.
    std::vector<std::string_view> patterns;
    std::vector<std::string> pattern_storage;
    for (int i : {0, 10, 50, 100}) {
        if (data[i].size() >= 3) {
            pattern_storage.push_back(data[i].substr(1, 3));
        }
    }
    for (const auto& p : pattern_storage) patterns.push_back(p);
    // Add a pattern that likely doesn't match.
    patterns.push_back("\x01\x02\x03\x04");

    EXPECT_EQ(contains_any_online(col, patterns),
              contains_any_eager(col, patterns));
}

TEST(AhoCorasickOnlineTest, MatchesEagerWithOverlappingPatterns) {
    std::vector<std::string> data = {
        "abcdef", "bcdefg", "cdefgh", "xyzabc", "nothing",
    };
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc", "bcd", "cde", "def"};
    EXPECT_EQ(contains_any_online(col, patterns),
              contains_any_eager(col, patterns));
}

TEST(AhoCorasickOnlineTest, MatchesEagerAcrossBitWidths) {
    std::vector<std::string> data = make_random_strings(100, 30, 9999);
    std::vector<std::string_view> patterns = {"the", "and", "for"};

    for (int b : {12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 42;
        auto col = op::OnPairColumn::compress(data, cfg);

        EXPECT_EQ(contains_any_online(col, patterns),
                  contains_any_eager(col, patterns))
            << "bit-width " << b;
    }
}

// -- Shared trie constructor --------------------------------------------------

TEST(AhoCorasickOnlineTest, SharedTrieConstructor) {
    std::vector<std::string> data = {"hello world", "foo bar", "hello foo"};
    auto col = make_column(data);
    auto v = col.view();

    std::vector<std::string_view> patterns = {"hello", "bar"};
    auto trie = std::make_shared<op::search::AhoCorasickTrie>(patterns);

    op::search::AhoCorasickOnlineAutomaton ac(trie, v.dictionary());
    auto result = v.scan(ac);
    std::vector<size_t> expected = {0, 1, 2};
    EXPECT_EQ(result, expected);
}
