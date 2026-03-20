#include <onpair/api.h>
#include <onpair/search/eq_search.h>
#include <gtest/gtest.h>
#include "corpus.h"

namespace op = onpair;
using namespace test_helpers;

// ── Helpers ───────────────────────────────────────────────────────────────────

static op::OnPairColumn make_sorted_column(const std::vector<std::string>& strings,
                                           op::BitWidth bits = 14)
{
    op::encoding::TrainingConfig cfg;
    cfg.bits            = bits;
    cfg.seed            = 42;
    
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

// ── Basic correctness ────────────────────────────────────────────────────────

TEST(EQSearchTest, SingleMatch) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_sorted_column(data);
    auto result = col.view().equals("def");
    EXPECT_EQ(result, (std::vector<size_t>{1}));
}

TEST(EQSearchTest, NoMatch) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_sorted_column(data);
    EXPECT_TRUE(col.view().equals("xyz").empty());
}

TEST(EQSearchTest, MultipleIdenticalStrings) {
    std::vector<std::string> data = {"abc", "abc", "def", "abc"};
    auto col = make_sorted_column(data);
    auto result = col.view().equals("abc");
    EXPECT_EQ(result, (std::vector<size_t>{0, 1, 3}));
}

TEST(EQSearchTest, EmptyValueMatchesOnlyEmptyStrings) {
    std::vector<std::string> data = {"", "abc", "", "def", ""};
    auto col = make_sorted_column(data);
    auto result = col.view().equals("");
    EXPECT_EQ(result, (std::vector<size_t>{0, 2, 4}));
}

TEST(EQSearchTest, EmptyValueNoEmptyStrings) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_sorted_column(data);
    EXPECT_TRUE(col.view().equals("").empty());
}

TEST(EQSearchTest, PrefixOfValueDoesNotMatch) {
    std::vector<std::string> data = {"abc", "abcd", "abcde"};
    auto col = make_sorted_column(data);
    auto result = col.view().equals("abc");
    // Only "abc" matches, not "abcd" or "abcde".
    EXPECT_EQ(result, (std::vector<size_t>{0}));
}

TEST(EQSearchTest, SuffixOfValueDoesNotMatch) {
    std::vector<std::string> data = {"bc", "abc", "xabc"};
    auto col = make_sorted_column(data);
    auto result = col.view().equals("abc");
    EXPECT_EQ(result, (std::vector<size_t>{1}));
}

TEST(EQSearchTest, ValueLongerThanAllStrings) {
    std::vector<std::string> data = {"a", "b", "c"};
    auto col = make_sorted_column(data);
    EXPECT_TRUE(col.view().equals("abcdefgh").empty());
}

// ── Consistency with brute force ─────────────────────────────────────────────

TEST(EQSearchTest, ConsistencyWithBruteForce) {
    auto data = make_user_strings(50);
    auto col  = make_sorted_column(data);

    // Query for a string that is in the corpus.
    const std::string query = "user_000025";
    auto result   = col.view().equals(query);
    auto expected = brute_eq(data, query);
    EXPECT_EQ(result, expected);

    // Query for a string not in the corpus.
    auto result2 = col.view().equals("user_999999");
    EXPECT_TRUE(result2.empty());
}

// ── Callback form ────────────────────────────────────────────────────────────

TEST(EQSearchTest, CallbackFormMatchesVectorForm) {
    std::vector<std::string> data = {"abc", "def", "abc", "ghi"};
    auto col = make_sorted_column(data);
    auto v = col.view();

    auto vec_result = v.equals("abc");
    std::vector<size_t> cb_result;
    v.equals("abc", [&](size_t idx) { cb_result.push_back(idx); });
    EXPECT_EQ(vec_result, cb_result);
}

// ── All bit widths ────────────────────────────────────────────────────────────

TEST(EQSearchTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {"abc", "def", "abc", "ghi"};
    for (int b : {12, 13, 14, 15, 16}) {
        auto col    = make_sorted_column(data, static_cast<op::BitWidth>(b));
        auto result = col.view().equals("abc");
        EXPECT_EQ(result, (std::vector<size_t>{0, 2})) << "bit-width " << b;
    }
}

// ── Large corpus cross-validation ────────────────────────────────────────────

TEST(EQSearchTest, LargeCorpusCrossValidation) {
    auto data = make_random_strings(200, 30, 123);
    auto col  = make_sorted_column(data);

    // Pick 5 queries: some in corpus, some not.
    for (int qi = 0; qi < static_cast<int>(data.size()); qi += 40) {
        const std::string& q = data[static_cast<size_t>(qi)];
        auto result   = col.view().equals(q);
        auto expected = brute_eq(data, q);
        EXPECT_EQ(result, expected) << "query=" << q;
    }
}
