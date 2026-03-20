#include <onpair/core/store.h>
#include <gtest/gtest.h>

using namespace onpair;

TEST(StoreTest, NumStringsZeroWhenBoundariesEmpty) {
    Store s;
    s.bit_width = 16;
    EXPECT_EQ(s.num_strings(), 0u);
}

TEST(StoreTest, NumStringsZeroWhenBoundariesHasOnlyOneSentinel) {
    Store s;
    s.bit_width = 16;
    s.boundaries = {0};  // Just the sentinel → 0 strings
    EXPECT_EQ(s.num_strings(), 0u);
}

TEST(StoreTest, NumStringsIsCorrect) {
    Store s;
    s.bit_width = 16;
    s.boundaries = {0, 3, 5, 8};  // 3 strings
    EXPECT_EQ(s.num_strings(), 3u);
}

TEST(StoreTest, NumTokensZeroWhenBoundariesEmpty) {
    Store s;
    s.bit_width = 16;
    EXPECT_EQ(s.num_tokens(), 0u);
}

TEST(StoreTest, NumTokensIsLastBoundary) {
    Store s;
    s.bit_width = 16;
    s.boundaries = {0, 4, 7};  // total 7 tokens
    EXPECT_EQ(s.num_tokens(), 7u);
}

TEST(StoreTest, BytesUsedCountsPackedBitsAndBoundaries) {
    Store s;
    s.bit_width  = 16;
    s.packed     = {0xDEAD, 0xBEEF};
    s.boundaries = {0, 2, 4};               // 4 tokens total
    // total_bits = 4 * 16 = 64;  packed_bytes = 8
    // boundaries = 3 * sizeof(uint32_t) = 12
    EXPECT_EQ(s.bytes_used(), 8u + 3 * sizeof(uint32_t));
}

TEST(StoreTest, BytesUsedWithDifferentBitWidth) {
    Store s;
    s.bit_width  = 13;
    s.packed     = {0xDEAD, 0xBEEF};
    s.boundaries = {0, 2, 4};               // 4 tokens total
    // total_bits = 4 * 13 = 52;  packed_bytes = 7
    // boundaries = 3 * sizeof(uint32_t) = 12
    EXPECT_EQ(s.bytes_used(), 7u + 3 * sizeof(uint32_t));
}
