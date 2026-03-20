#include <onpair/api.h>
#include <onpair/search/automata/eq_automaton.h>
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

// Brute-force reference: indices where strings[i] == value.
static std::vector<size_t> brute_eq(const std::vector<std::string>& strings,
                                     std::string_view value)
{
    std::vector<size_t> result;
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i] == value) result.push_back(i);
    return result;
}

// ── Concept satisfaction ──────────────────────────────────────────────────────

TEST(EqAutomatonTest, SatisfiesTokenAutomaton) {
    static_assert(search::TokenAutomaton<search::EqAutomaton>);
}

TEST(EqAutomatonTest, SatisfiesDeadDetectable) {
    static_assert(search::DeadDetectable<search::EqAutomaton>);
}

// ── Basic correctness ─────────────────────────────────────────────────────────

TEST(EqAutomatonTest, SingleMatch) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("def", v.dictionary());
    auto result = v.scan(eq);
    EXPECT_EQ(result, (std::vector<size_t>{1}));
}

TEST(EqAutomatonTest, NoMatch) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("xyz", v.dictionary());
    EXPECT_TRUE(v.scan(eq).empty());
}

TEST(EqAutomatonTest, MultipleIdenticalStrings) {
    std::vector<std::string> data = {"abc", "abc", "def", "abc"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("abc", v.dictionary());
    auto result = v.scan(eq);
    EXPECT_EQ(result, (std::vector<size_t>{0, 1, 3}));
}

TEST(EqAutomatonTest, EmptyValueMatchesOnlyEmptyStrings) {
    std::vector<std::string> data = {"", "abc", "", "def", ""};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("", v.dictionary());
    auto result = v.scan(eq);
    EXPECT_EQ(result, (std::vector<size_t>{0, 2, 4}));
}

TEST(EqAutomatonTest, EmptyValueNoEmptyStrings) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("", v.dictionary());
    EXPECT_TRUE(v.scan(eq).empty());
}

TEST(EqAutomatonTest, PrefixOfValueDoesNotMatch) {
    std::vector<std::string> data = {"abc", "abcd", "abcde"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("abc", v.dictionary());
    auto result = v.scan(eq);
    EXPECT_EQ(result, (std::vector<size_t>{0}));
}

TEST(EqAutomatonTest, SuffixOfValueDoesNotMatch) {
    std::vector<std::string> data = {"bc", "abc", "xabc"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("abc", v.dictionary());
    auto result = v.scan(eq);
    EXPECT_EQ(result, (std::vector<size_t>{1}));
}

TEST(EqAutomatonTest, ValueLongerThanAllStrings) {
    std::vector<std::string> data = {"a", "b", "c"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("abcdefgh", v.dictionary());
    EXPECT_TRUE(v.scan(eq).empty());
}

// ── Automaton reuse ───────────────────────────────────────────────────────────

TEST(EqAutomatonTest, RescannableSameColumn) {
    std::vector<std::string> data = {"abc", "def", "abc"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("abc", v.dictionary());
    auto r1 = v.scan(eq);
    auto r2 = v.scan(eq);
    EXPECT_EQ(r1, r2);
}

// ── Callback form ─────────────────────────────────────────────────────────────

TEST(EqAutomatonTest, CallbackFormMatchesVectorForm) {
    std::vector<std::string> data = {"abc", "def", "abc", "ghi"};
    auto col = make_column(data);
    auto v = col.view();
    search::EqAutomaton eq("abc", v.dictionary());

    auto vec_result = v.scan(eq);
    std::vector<size_t> cb_result;
    v.scan(eq, [&](size_t idx) { cb_result.push_back(idx); });
    EXPECT_EQ(vec_result, cb_result);
}

// ── Consistency with brute force ──────────────────────────────────────────────

TEST(EqAutomatonTest, ConsistencyWithBruteForce) {
    auto data = make_user_strings(50);
    auto col  = make_column(data);
    auto v    = col.view();

    const std::string query = "user_000025";
    search::EqAutomaton eq(query, v.dictionary());
    auto result   = v.scan(eq);
    auto expected = brute_eq(data, query);
    EXPECT_EQ(result, expected);

    search::EqAutomaton eq2("user_999999", v.dictionary());
    EXPECT_TRUE(v.scan(eq2).empty());
}

// ── All bit widths ────────────────────────────────────────────────────────────

TEST(EqAutomatonTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {"abc", "def", "abc", "ghi"};
    for (int b : {12, 13, 14, 15, 16}) {
        auto col = make_column(data, static_cast<op::BitWidth>(b));
        auto v = col.view();
        search::EqAutomaton eq("abc", v.dictionary());
        auto result = v.scan(eq);
        EXPECT_EQ(result, (std::vector<size_t>{0, 2})) << "bit-width " << b;
    }
}

// ── Large corpus cross-validation ─────────────────────────────────────────────

TEST(EqAutomatonTest, LargeCorpusCrossValidation) {
    auto data = make_random_strings(200, 30, 123);
    auto col  = make_column(data);
    auto v    = col.view();

    for (int qi = 0; qi < static_cast<int>(data.size()); qi += 40) {
        const std::string& q = data[static_cast<size_t>(qi)];
        search::EqAutomaton eq(q, v.dictionary());
        auto result   = v.scan(eq);
        auto expected = brute_eq(data, q);
        EXPECT_EQ(result, expected) << "query=" << q;
    }
}

// ── Empty column ──────────────────────────────────────────────────────────────

TEST(EqAutomatonTest, EmptyColumnReturnsEmpty) {
    auto col = make_column({});
    auto v = col.view();
    search::EqAutomaton eq("abc", v.dictionary());
    EXPECT_TRUE(v.scan(eq).empty());
}
