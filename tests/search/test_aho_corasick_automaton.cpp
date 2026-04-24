#include <onpair/api.h>
#include <onpair/search/automata/aho_corasick_automaton.h>
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

// Helper: multi-pattern search via ColumnView.
static std::vector<size_t> contains_any(const op::OnPairColumn& col,
                                         std::span<const std::string_view> patterns) {
    auto v = col.view();
    op::search::AhoCorasickAutomaton ac(patterns, v.dictionary());
    return v.scan(ac);
}

// ── Concept satisfaction ─────────────────────────────────────────────────────

TEST(AhoCorasickTest, SatisfiesTokenAutomatonConcept) {
    static_assert(op::search::TokenAutomaton<op::search::AhoCorasickAutomaton>);
}

TEST(AhoCorasickTest, SatisfiesDeadDetectable) {
    static_assert(op::search::DeadDetectable<op::search::AhoCorasickAutomaton>);
}

// ── Basic multi-pattern search ───────────────────────────────────────────────

TEST(AhoCorasickTest, BasicMultiPattern) {
    std::vector<std::string> data = {
        "error: disk full",
        "warning: low memory",
        "info: all ok",
        "fatal: kernel panic",
        "debug: trace",
    };
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"error", "fatal"};
    auto result = contains_any(col, patterns);
    std::vector<size_t> expected = {0, 3};
    EXPECT_EQ(result, expected);
}

TEST(AhoCorasickTest, SinglePattern) {
    std::vector<std::string> data = {"abc", "def", "abc_xyz"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc"};
    auto result = contains_any(col, patterns);
    std::vector<size_t> expected = {0, 2};
    EXPECT_EQ(result, expected);
}

TEST(AhoCorasickTest, NoMatches) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"xyz", "uvw"};
    auto result = contains_any(col, patterns);
    EXPECT_TRUE(result.empty());
}

TEST(AhoCorasickTest, AllStringsMatch) {
    std::vector<std::string> data = {"abc_def", "def_ghi", "ghi_abc"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc", "def", "ghi"};
    auto result = contains_any(col, patterns);
    EXPECT_EQ(result.size(), 3u);
}

// ── Empty pattern ────────────────────────────────────────────────────────────

TEST(AhoCorasickTest, EmptyPatternMatchesAll) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {""};
    auto result = contains_any(col, patterns);
    EXPECT_EQ(result.size(), 2u);
}

TEST(AhoCorasickTest, EmptyPatternSetMatchesNone) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_column(data);

    // No patterns → no string can match.
    std::vector<std::string_view> patterns;
    auto result = contains_any(col, patterns);
    EXPECT_EQ(result.size(), 0u);
}

// ── Overlapping patterns ─────────────────────────────────────────────────────

TEST(AhoCorasickTest, OverlappingPatterns) {
    std::vector<std::string> data = {"abcdef", "bcde", "xyz"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"abc", "bcd"};
    auto result = contains_any(col, patterns);
    // "abcdef" contains both, "bcde" contains "bcd"
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

TEST(AhoCorasickTest, PrefixPatterns) {
    // "ab" is a prefix of "abc"
    std::vector<std::string> data = {"abc", "ab", "xyz"};
    auto col = make_column(data);

    std::vector<std::string_view> patterns = {"ab", "abc"};
    auto result = contains_any(col, patterns);
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

// ── Using generic scan API ───────────────────────────────────────────────────

TEST(AhoCorasickTest, GenericScanApi) {
    std::vector<std::string> data = {"abc_xyz", "def_uvw", "abc_def"};
    auto col = make_column(data);
    auto v = col.view();

    std::vector<std::string_view> patterns = {"abc", "uvw"};
    op::search::AhoCorasickAutomaton ac(patterns, v.dictionary());
    auto result = v.scan(ac);
    std::vector<size_t> expected = {0, 1, 2};
    EXPECT_EQ(result, expected);
}

// ── Various bit widths ───────────────────────────────────────────────────────

TEST(AhoCorasickTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {"error log", "warning log", "info log"};
    for (int b : {9, 10, 11, 12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 42;
        auto col = op::OnPairColumn::compress(data, cfg);
        std::vector<std::string_view> patterns = {"error", "warning"};
        auto result = contains_any(col, patterns);
        EXPECT_EQ(result.size(), 2u) << "bit-width " << b;
    }
}

// ── Consistency with KMP ─────────────────────────────────────────────────────

TEST(AhoCorasickTest, SinglePatternMatchesKmp) {
    auto data = make_user_strings(50);
    auto col = make_column(data);

    auto kmp_result = col.view().contains("user_0000");
    std::vector<std::string_view> patterns = {"user_0000"};
    auto ac_result = contains_any(col, patterns);

    EXPECT_EQ(kmp_result, ac_result);
}
