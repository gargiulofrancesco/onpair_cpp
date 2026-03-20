#include <onpair/api.h>
#include <onpair/search/automata/prefix_automaton.h>
#include <gtest/gtest.h>
#include "corpus.h"

namespace op = onpair;
namespace search = onpair::search;
using namespace test_helpers;

// ── Helper ────────────────────────────────────────────────────────────────────

static op::OnPairColumn make_column(const std::vector<std::string>& strings,
                                    op::BitWidth bits = 14)
{
    op::encoding::TrainingConfig cfg;
    cfg.bits = bits;
    cfg.seed = 42;
    return op::OnPairColumn::compress(strings, cfg);
}

// Brute-force reference: indices where strings[i] starts with prefix.
static std::vector<size_t> brute_prefix(const std::vector<std::string>& strings,
                                         std::string_view prefix)
{
    std::vector<size_t> result;
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i].size() >= prefix.size() &&
            strings[i].compare(0, prefix.size(), prefix) == 0)
            result.push_back(i);
    return result;
}

// ── Concept satisfaction ──────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, SatisfiesTokenAutomaton) {
    static_assert(search::TokenAutomaton<search::PrefixAutomaton>);
}

TEST(PrefixAutomatonTest, SatisfiesDeadDetectable) {
    static_assert(search::DeadDetectable<search::PrefixAutomaton>);
}

// ── Basic correctness ─────────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, BasicPrefixMatch) {
    std::vector<std::string> data = {
        "user_000001", "user_000002", "admin_001",
        "user_000003", "guest_001",   "admin_002",
    };
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("user_", v.dictionary());
    auto result = v.scan(pa);
    std::vector<size_t> expected = {0, 1, 3};
    EXPECT_EQ(result, expected);
}

TEST(PrefixAutomatonTest, AdminPrefix) {
    std::vector<std::string> data = {
        "user_000001", "admin_001", "admin_002", "guest_001",
    };
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("admin", v.dictionary());
    auto result = v.scan(pa);
    std::vector<size_t> expected = {1, 2};
    EXPECT_EQ(result, expected);
}

TEST(PrefixAutomatonTest, NoMatches) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("xyz", v.dictionary());
    EXPECT_TRUE(v.scan(pa).empty());
}

TEST(PrefixAutomatonTest, ExactMatch) {
    std::vector<std::string> data = {"abc", "abcd", "abcde"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("abc", v.dictionary());
    auto result = v.scan(pa);
    EXPECT_EQ(result.size(), 3u);
}

TEST(PrefixAutomatonTest, PrefixLongerThanString) {
    std::vector<std::string> data = {"ab", "abc", "abcd"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("abcde", v.dictionary());
    EXPECT_TRUE(v.scan(pa).empty());
}

TEST(PrefixAutomatonTest, SingleCharPrefix) {
    std::vector<std::string> data = {"abc", "axe", "bcd", "apple"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("a", v.dictionary());
    auto result = v.scan(pa);
    EXPECT_EQ(result.size(), 3u);
}

// ── Empty prefix matches all ─────────────────────────────────────────────────

TEST(PrefixAutomatonTest, EmptyPrefixMatchesAll) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("", v.dictionary());
    auto result = v.scan(pa);
    EXPECT_EQ(result.size(), 3u);
}

// ── User strings ──────────────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, UserStringsPrefix) {
    auto data = make_user_strings(50);
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("user_", v.dictionary());
    auto result = v.scan(pa);
    EXPECT_EQ(result.size(), 50u);
}

TEST(PrefixAutomatonTest, UserStringsSpecificPrefix) {
    auto data = make_user_strings(100);
    auto col = make_column(data);
    auto v = col.view();

    search::PrefixAutomaton pa1("user_0000", v.dictionary());
    EXPECT_EQ(v.scan(pa1).size(), 100u);

    search::PrefixAutomaton pa2("user_00001", v.dictionary());
    EXPECT_EQ(v.scan(pa2).size(), 10u);
}

// ── Automaton reuse ───────────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, RescannableSameColumn) {
    std::vector<std::string> data = {"user_001", "admin_001", "user_002"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("user_", v.dictionary());
    auto r1 = v.scan(pa);
    auto r2 = v.scan(pa);
    EXPECT_EQ(r1, r2);
}

// ── Callback form ─────────────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, CallbackFormMatchesVectorForm) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "guest_001"
    };
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("user_", v.dictionary());

    auto vec_result = v.scan(pa);
    std::vector<size_t> cb_result;
    v.scan(pa, [&](size_t idx) { cb_result.push_back(idx); });
    EXPECT_EQ(vec_result, cb_result);
}

// ── Various bit widths ────────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "user_003"
    };
    for (int b : {12, 13, 14, 15, 16}) {
        auto col = make_column(data, static_cast<op::BitWidth>(b));
        auto v = col.view();
        search::PrefixAutomaton pa("user_", v.dictionary());
        auto result = v.scan(pa);
        EXPECT_EQ(result.size(), 3u) << "bit-width " << b;
    }
}

// ── Valid divergence edge case ────────────────────────────────────────────────

TEST(PrefixAutomatonTest, PrefixBoundaryWithinToken) {
    std::vector<std::string> data = {"user_001", "useful", "umbrella"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("use", v.dictionary());
    auto result = v.scan(pa);
    EXPECT_EQ(result.size(), 2u);
}

TEST(PrefixAutomatonTest, PrefixIsExactString) {
    std::vector<std::string> data = {"abc", "abcd", "ab"};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("abc", v.dictionary());
    auto result = v.scan(pa);
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

// ── Cross-validation with brute force ─────────────────────────────────────────

TEST(PrefixAutomatonTest, ConsistencyWithBruteForce) {
    auto data = make_random_strings(200, 30, 123);
    auto col  = make_column(data);
    auto v    = col.view();

    for (const std::string& prefix : {"a", "ab", "z", "xx"}) {
        search::PrefixAutomaton pa(prefix, v.dictionary());
        auto result = v.scan(pa);

        auto expected = brute_prefix(data, prefix);
        EXPECT_EQ(result, expected) << "prefix=\"" << prefix << "\"";
    }
}

// ── Empty column ──────────────────────────────────────────────────────────────

TEST(PrefixAutomatonTest, EmptyColumnReturnsEmpty) {
    auto col = make_column({});
    auto v = col.view();
    search::PrefixAutomaton pa("abc", v.dictionary());
    EXPECT_TRUE(v.scan(pa).empty());
}

TEST(PrefixAutomatonTest, EmptyStringMatchesEmptyPrefix) {
    std::vector<std::string> data = {"", "abc", ""};
    auto col = make_column(data);
    auto v = col.view();
    search::PrefixAutomaton pa("", v.dictionary());
    auto result = v.scan(pa);
    EXPECT_EQ(result.size(), 3u);
}
