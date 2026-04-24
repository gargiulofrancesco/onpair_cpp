#include <onpair/api.h>
#include <onpair/search/automata/kmp_automaton.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"

namespace op = onpair;
using namespace test_helpers;

// ── Helper: compress with sorted dict, default config ────────────────────────
static op::OnPairColumn make_column(const std::vector<std::string>& strings) {
    op::encoding::TrainingConfig cfg;
    cfg.seed = 42;
    return op::OnPairColumn::compress(strings, cfg);
}

// ── Concept satisfaction ─────────────────────────────────────────────────────

TEST(KmpAutomatonTest, SatisfiesTokenAutomatonConcept) {
    static_assert(op::search::TokenAutomaton<op::search::KmpAutomaton>);
}

TEST(KmpAutomatonTest, SatisfiesDeadDetectable) {
    static_assert(op::search::DeadDetectable<op::search::KmpAutomaton>);
}

// ── Empty pattern ────────────────────────────────────────────────────────────

TEST(KmpAutomatonTest, EmptyPatternMatchesAll) {
    auto col = make_column({"abc", "def", "ghi"});
    auto view = col.view();
    auto result = view.contains("");
    EXPECT_EQ(result.size(), 3u);
}

// ── Basic substring search ───────────────────────────────────────────────────

TEST(KmpAutomatonTest, BasicSubstringMatch) {
    std::vector<std::string> data = {
        "hello world", "foo bar", "hello there", "world hello", "xyz"
    };
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("hello");
    std::vector<size_t> expected = {0, 2, 3};
    EXPECT_EQ(result, expected);
}

TEST(KmpAutomatonTest, PatternAtBeginning) {
    std::vector<std::string> data = {"abc_def", "xyz_abc", "abc"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("abc");
    EXPECT_EQ(result.size(), 3u);  // all contain "abc"
}

TEST(KmpAutomatonTest, PatternAtEnd) {
    std::vector<std::string> data = {"hello_xyz", "abc_xyz", "no_match"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("xyz");
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

TEST(KmpAutomatonTest, NoMatches) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("xyz");
    EXPECT_TRUE(result.empty());
}

TEST(KmpAutomatonTest, ExactStringMatch) {
    std::vector<std::string> data = {"abc", "abcd", "ab"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("abc");
    std::vector<size_t> expected = {0, 1};  // "abc" and "abcd" contain "abc"
    EXPECT_EQ(result, expected);
}

TEST(KmpAutomatonTest, SingleCharPattern) {
    std::vector<std::string> data = {"abc", "def", "axe"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("a");
    std::vector<size_t> expected = {0, 2};
    EXPECT_EQ(result, expected);
}

// ── Repeated pattern / KMP failure function ──────────────────────────────────

TEST(KmpAutomatonTest, OverlappingPatternInString) {
    // "aaaa" contains "aa" starting at positions 0, 1, 2.  KMP should find it.
    std::vector<std::string> data = {"aaaa", "ab", "ba"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("aa");
    std::vector<size_t> expected = {0};
    EXPECT_EQ(result, expected);
}

TEST(KmpAutomatonTest, KmpFailureFunctionStress) {
    // Pattern "abab" has a non-trivial LPS: [0, 0, 1, 2].
    // The string "ababab" contains "abab" starting at position 0.
    std::vector<std::string> data = {"ababab", "abab", "abba", "baba"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("abab");
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

// ── User strings (mixed prefixes) ────────────────────────────────────────────

TEST(KmpAutomatonTest, UserStringsContainsUser) {
    auto data = make_user_strings(20);
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("user_");
    EXPECT_EQ(result.size(), 20u);  // all start with "user_"
}

TEST(KmpAutomatonTest, UserStringsContainsSpecificId) {
    auto data = make_user_strings(100);
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("000042");
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 42u);
}

// ── Callback form ────────────────────────────────────────────────────────────

TEST(KmpAutomatonTest, CallbackFormMatchesVectorForm) {
    std::vector<std::string> data = {"abc", "bcd", "cde", "abc_xyz"};
    auto col = make_column(data);
    auto view = col.view();

    op::search::KmpAutomaton kmp("abc", view.dictionary());
    auto vec_result = view.scan(kmp);

    std::vector<size_t> cb_result;
    view.scan(kmp, [&](size_t idx) { cb_result.push_back(idx); });

    EXPECT_EQ(vec_result, cb_result);
}

// ── Automaton reuse across scans ─────────────────────────────────────────────

TEST(KmpAutomatonTest, AutomatonRescannableSameColumn) {
    // Verify that the same automaton can be used for multiple scans on the
    // same column, producing identical results each time (reset() is called
    // internally by drive()).
    std::vector<std::string> data = {"abc", "def", "abc_xyz"};
    auto col = make_column(data);
    auto v = col.view();

    op::search::KmpAutomaton kmp("abc", v.dictionary());
    auto r1 = v.scan(kmp);
    auto r2 = v.scan(kmp);
    EXPECT_EQ(r1, r2);
}

// ── Various bit widths ───────────────────────────────────────────────────────

TEST(KmpAutomatonTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {"the quick brown fox", "lazy dog", "quick fox"};
    for (int b : {9, 10, 11, 12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 42;
        auto col = op::OnPairColumn::compress(data, cfg);
        auto view = col.view();
        auto result = view.contains("quick");
        EXPECT_EQ(result.size(), 2u) << "bit-width " << b;
        EXPECT_EQ(result[0], 0u);
        EXPECT_EQ(result[1], 2u);
    }
}

// ── Pattern longer than any string ───────────────────────────────────────────

TEST(KmpAutomatonTest, PatternLongerThanStrings) {
    std::vector<std::string> data = {"ab", "cd"};
    auto col = make_column(data);
    auto view = col.view();
    auto result = view.contains("abcdefghij");
    EXPECT_TRUE(result.empty());
}

// ── Empty column ─────────────────────────────────────────────────────────────

TEST(KmpAutomatonTest, EmptyColumnReturnsEmpty) {
    auto col = make_column({});
    auto view = col.view();
    auto result = view.contains("abc");
    EXPECT_TRUE(result.empty());
}

// ── Cross-validation with brute force ────────────────────────────────────────

TEST(KmpAutomatonTest, CrossValidationWithBruteForce) {
    auto data = make_random_strings(100, 30, 42);
    auto col = make_column(data);
    auto view = col.view();

    const std::string needle = "ab";
    auto result = view.contains(needle);

    // Brute-force reference.
    std::vector<size_t> expected;
    for (size_t i = 0; i < data.size(); ++i)
        if (data[i].find(needle) != std::string::npos)
            expected.push_back(i);

    EXPECT_EQ(result, expected);
}
