#include <onpair/api.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"

namespace op = onpair;
using namespace test_helpers;

// ── Helper: compress with sorted dict ────────────────────────────────────────
static op::OnPairColumn make_column(const std::vector<std::string>& strings) {
    op::encoding::TrainingConfig cfg;
    cfg.seed = 42;
    
    return op::OnPairColumn::compress(strings, cfg);
}

// ── Basic prefix search ─────────────────────────────────────────────────────

TEST(PrefixSearchTest, BasicPrefixMatch) {
    std::vector<std::string> data = {
        "user_000001", "user_000002", "admin_001",
        "user_000003", "guest_001",   "admin_002",
    };
    auto col = make_column(data);
    auto result = col.view().starts_with("user_");
    std::vector<size_t> expected = {0, 1, 3};
    EXPECT_EQ(result, expected);
}

TEST(PrefixSearchTest, AdminPrefix) {
    std::vector<std::string> data = {
        "user_000001", "admin_001", "admin_002", "guest_001",
    };
    auto col = make_column(data);
    auto result = col.view().starts_with("admin");
    std::vector<size_t> expected = {1, 2};
    EXPECT_EQ(result, expected);
}

TEST(PrefixSearchTest, NoMatches) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto result = col.view().starts_with("xyz");
    EXPECT_TRUE(result.empty());
}

TEST(PrefixSearchTest, ExactMatch) {
    std::vector<std::string> data = {"abc", "abcd", "abcde"};
    auto col = make_column(data);
    auto result = col.view().starts_with("abc");
    // All three strings start with "abc"
    EXPECT_EQ(result.size(), 3u);
}

TEST(PrefixSearchTest, PrefixLongerThanString) {
    std::vector<std::string> data = {"ab", "abc", "abcd"};
    auto col = make_column(data);
    auto result = col.view().starts_with("abcde");
    EXPECT_TRUE(result.empty());
}

TEST(PrefixSearchTest, SingleCharPrefix) {
    std::vector<std::string> data = {"abc", "axe", "bcd", "apple"};
    auto col = make_column(data);
    auto result = col.view().starts_with("a");
    // "abc", "axe", "apple" start with "a"
    EXPECT_EQ(result.size(), 3u);
}

// ── Empty prefix matches all ─────────────────────────────────────────────────

TEST(PrefixSearchTest, EmptyPrefixMatchesAll) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto result = col.view().starts_with("");
    EXPECT_EQ(result.size(), 3u);
}

// ── User strings ─────────────────────────────────────────────────────────────

TEST(PrefixSearchTest, UserStringsPrefix) {
    auto data = make_user_strings(50);
    auto col = make_column(data);
    auto result = col.view().starts_with("user_");
    EXPECT_EQ(result.size(), 50u);
}

TEST(PrefixSearchTest, UserStringsSpecificPrefix) {
    auto data = make_user_strings(100);
    auto col = make_column(data);
    // All 100 strings are "user_000000" through "user_000099".
    // All start with "user_0000" (9 chars), so all 100 match.
    auto result = col.view().starts_with("user_0000");
    EXPECT_EQ(result.size(), 100u);

    // "user_00001" narrows to user_000010..user_000019 (10 matches).
    auto result2 = col.view().starts_with("user_00001");
    EXPECT_EQ(result2.size(), 10u);
}

// ── Callback form ────────────────────────────────────────────────────────────

TEST(PrefixSearchTest, CallbackFormMatchesVectorForm) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "guest_001"
    };
    auto col = make_column(data);
    auto v = col.view();

    auto vec_result = v.starts_with("user_");
    std::vector<size_t> cb_result;
    v.starts_with("user_", [&](size_t idx) { cb_result.push_back(idx); });

    EXPECT_EQ(vec_result, cb_result);
}

// ── Various bit widths ───────────────────────────────────────────────────────

TEST(PrefixSearchTest, WorksAcrossBitWidths) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "user_003"
    };
    for (int b : {12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 42;
        
        auto col = op::OnPairColumn::compress(data, cfg);
        auto result = col.view().starts_with("user_");
        EXPECT_EQ(result.size(), 3u) << "bit-width " << b;
    }
}

// ── Valid divergence edge case ───────────────────────────────────────────────

TEST(PrefixSearchTest, PrefixBoundaryWithinToken) {
    // The prefix "use" may end in the middle of a multi-byte token like "user_".
    // Valid divergence should handle this correctly.
    std::vector<std::string> data = {"user_001", "useful", "umbrella"};
    auto col = make_column(data);
    auto result = col.view().starts_with("use");
    // "user_001" and "useful" start with "use"
    EXPECT_EQ(result.size(), 2u);
}

TEST(PrefixSearchTest, PrefixIsExactString) {
    std::vector<std::string> data = {"abc", "abcd", "ab"};
    auto col = make_column(data);
    auto result = col.view().starts_with("abc");
    // "abc" and "abcd" start with "abc"
    std::vector<size_t> expected = {0, 1};
    EXPECT_EQ(result, expected);
}

// ── Cross-validation with brute force ────────────────────────────────────────

TEST(PrefixSearchTest, ConsistencyWithBruteForce) {
    auto data = make_random_strings(200, 30, 123);
    auto col  = make_column(data);

    for (const std::string& prefix : {"a", "ab", "z", "xx"}) {
        auto result = col.view().starts_with(prefix);

        // Brute-force reference.
        std::vector<size_t> expected;
        for (size_t i = 0; i < data.size(); ++i)
            if (data[i].substr(0, std::string(prefix).size()) == prefix)
                expected.push_back(i);

        EXPECT_EQ(result, expected) << "prefix=\"" << prefix << "\"";
    }
}

TEST(PrefixSearchTest, EmptyColumnReturnsEmpty) {
    auto col = make_column({});
    auto result = col.view().starts_with("abc");
    EXPECT_TRUE(result.empty());
}

TEST(PrefixSearchTest, EmptyStringMatchesEmptyPrefix) {
    // Empty string has empty prefix, so empty prefix scan should match it.
    std::vector<std::string> data = {"", "abc", ""};
    auto col = make_column(data);
    auto result = col.view().starts_with("");
    EXPECT_EQ(result.size(), 3u);
}
