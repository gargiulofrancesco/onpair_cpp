#include <onpair/api.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"
#include <span>
#include <ranges>
#include <type_traits>
#include <stdexcept>
#include <vector>
#include <string>

namespace op = onpair;
using namespace test_helpers;

// ── Lifecycle / type traits ───────────────────────────────────────────────────

TEST(ColumnApiTest, OnPairColumnIsNonCopyable) {
    static_assert(!std::is_copy_constructible_v<op::OnPairColumn>,
                  "OnPairColumn must be non-copyable");
    static_assert(!std::is_copy_assignable_v<op::OnPairColumn>,
                  "OnPairColumn must be non-copy-assignable");
}

TEST(ColumnApiTest, OnPairColumnIsMovable) {
    static_assert(std::is_move_constructible_v<op::OnPairColumn>,
                  "OnPairColumn must be move-constructible");
    static_assert(std::is_move_assignable_v<op::OnPairColumn>,
                  "OnPairColumn must be move-assignable");
}

TEST(ColumnApiTest, MoveDoesNotCorruptData) {
    auto strings = make_user_strings(10);
    auto col     = op::OnPairColumn::compress(strings);
    auto moved   = std::move(col);
    EXPECT_ROUNDTRIP_OK(strings, moved);
}

// ── Metadata ──────────────────────────────────────────────────────────────────

TEST(ColumnApiTest, NumStringsIsCorrect) {
    auto strings = make_user_strings(15);
    auto col = op::OnPairColumn::compress(strings);
    EXPECT_EQ(col.num_strings(), 15u);
}

TEST(ColumnApiTest, BitWidthMatchesConfig) {
    for (int b : {12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 1;
        auto col = op::OnPairColumn::compress(make_user_strings(10), cfg);
        EXPECT_EQ(col.bits(), static_cast<op::BitWidth>(b));
    }
}

TEST(ColumnApiTest, BytesUsedIsPositiveForNonEmpty) {
    auto col = op::OnPairColumn::compress(make_user_strings(5));
    EXPECT_GT(col.bytes_used(), 0u);
}

// ── compress(range) with various range types ──────────────────────────────────

TEST(ColumnApiTest, CompressFromVectorOfString) {
    std::vector<std::string> strings = {"alpha", "beta", "gamma"};
    auto col = op::OnPairColumn::compress(strings);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

TEST(ColumnApiTest, CompressFromVectorOfStringView) {
    std::string a = "hello", b = "world";
    std::vector<std::string_view> views = {a, b};
    auto col = op::OnPairColumn::compress(views);
    EXPECT_EQ(col.num_strings(), 2u);
    EXPECT_DECOMPRESSES_TO(col, 0, "hello");
    EXPECT_DECOMPRESSES_TO(col, 1, "world");
}

TEST(ColumnApiTest, CompressFromSpanOfStringView) {
    std::vector<std::string> src = {"one", "two", "three"};
    std::vector<std::string_view> views(src.begin(), src.end());
    auto sp  = std::span<std::string_view>(views);
    auto col = op::OnPairColumn::compress(sp);
    EXPECT_ROUNDTRIP_OK(src, col);
}

// ── Arrow-style overload ──────────────────────────────────────────────────────

TEST(ColumnApiTest, ArrowStyleOverloadProducesCorrectColumn) {
    auto strings = make_user_strings(20);
    auto raw = make_raw(strings);
    auto col = op::OnPairColumn::compress(
        reinterpret_cast<const char*>(raw.data.data()),
        raw.offsets.data(),
        raw.n);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// ── OnPairColumnView ──────────────────────────────────────────────────────────

TEST(ColumnApiTest, ViewNumStringsMatchesColumn) {
    auto strings = make_user_strings(8);
    auto col = op::OnPairColumn::compress(strings);
    const auto cv = col.view();
    EXPECT_EQ(cv.num_strings(), col.num_strings());
}

TEST(ColumnApiTest, ViewBitsMatchesColumn) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = 14;
    cfg.seed = 1;
    auto col = op::OnPairColumn::compress(make_user_strings(5), cfg);
    EXPECT_EQ(col.view().bits(), op::BitWidth(14));
}

TEST(ColumnApiTest, ViewBytesUsedMatchesColumn) {
    auto col = op::OnPairColumn::compress(make_user_strings(5));
    EXPECT_EQ(col.view().bytes_used(), col.bytes_used());
}

TEST(ColumnApiTest, ViewDecompressRoundTrips) {
    auto strings = make_user_strings(5);
    auto col = op::OnPairColumn::compress(strings);
    auto cv = col.view();

    std::vector<char> buf(4096 + op::DECOMPRESS_BUFFER_PADDING);
    for (size_t i = 0; i < strings.size(); ++i) {
        const size_t len = cv.decompress(i, buf.data());
        EXPECT_EQ(std::string(buf.data(), len), strings[i]) << "at index " << i;
    }
}

// ── Search operations via view ────────────────────────────────────────────────

TEST(ColumnApiTest, ContainsSearchViaView) {
    auto col = op::OnPairColumn::compress(make_user_strings(10));
    auto result = col.view().contains("user_");
    EXPECT_EQ(result.size(), 10u);
}

TEST(ColumnApiTest, StartsWithViaView) {
    auto strings = make_user_strings(5);
    auto col = op::OnPairColumn::compress(strings);
    auto result = col.view().starts_with("user_");
    EXPECT_EQ(result.size(), 5u);
}

TEST(ColumnApiTest, EqualsViaView) {
    auto strings = make_user_strings(10);
    auto col = op::OnPairColumn::compress(strings);
    auto result = col.view().equals("user_000005");
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 5u);
}

// ── Decompress all ────────────────────────────────────────────────────────────

TEST(ColumnApiTest, DecompressAllMatchesIndividual) {
    auto strings = make_user_strings(20);
    auto col = op::OnPairColumn::compress(strings);
    auto cv = col.view();

    // Build expected flat buffer from individual decompressions.
    std::string expected;
    std::vector<char> tmp(4096 + op::DECOMPRESS_BUFFER_PADDING);
    for (size_t i = 0; i < strings.size(); ++i) {
        const size_t len = cv.decompress(i, tmp.data());
        expected.append(tmp.data(), len);
    }

    // Use view-level decompress_all.
    std::vector<char> bulk(expected.size() + op::DECOMPRESS_BUFFER_PADDING);
    const size_t written = cv.decompress_all(bulk.data());
    const std::string actual(bulk.data(), written);
    EXPECT_EQ(actual, expected);
}

// ── Store / Dictionary accessors ──────────────────────────────────────────────

TEST(ColumnApiTest, ViewExposesDictionaryAndStore) {
    auto col = op::OnPairColumn::compress(make_user_strings(5));
    auto cv = col.view();
    EXPECT_GT(cv.store().num_strings(), 0u);
    EXPECT_GT(cv.dictionary().num_tokens(), 0u);
}
