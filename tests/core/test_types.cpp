#include <onpair/core/types.h>
#include <gtest/gtest.h>
#include <limits>

using namespace onpair;

// ── ByteSpan ─────────────────────────────────────────────────────────────────

TEST(ByteSpanTest, SizeIsEndMinusBegin) {
    EXPECT_EQ((ByteSpan{0, 0}.size()), 0u);
    EXPECT_EQ((ByteSpan{0, 1}.size()), 1u);
    EXPECT_EQ((ByteSpan{5, 10}.size()), 5u);
    EXPECT_EQ((ByteSpan{100, 100}.size()), 0u);
}

// ── StreamSpan ────────────────────────────────────────────────────────────────

TEST(StreamSpanTest, SizeIsEndMinusBegin) {
    EXPECT_EQ((StreamSpan{0, 0}.size()), 0u);
    EXPECT_EQ((StreamSpan{0, 1}.size()), 1u);
    EXPECT_EQ((StreamSpan{3, 7}.size()), 4u);
}

// ── TokenRange ────────────────────────────────────────────────────────────────

TEST(TokenRangeTest, DefaultIsEmpty) {
    TokenRange r;
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0u);
}

TEST(TokenRangeTest, EmptyWhenBeginGtLast) {
    EXPECT_TRUE((TokenRange{5, 4}.empty()));
    EXPECT_TRUE((TokenRange{1, 0}.empty()));  // default
}

TEST(TokenRangeTest, NotEmptyWhenBeginLeqLast) {
    EXPECT_FALSE((TokenRange{0, 0}.empty()));
    EXPECT_FALSE((TokenRange{5, 5}.empty()));
    EXPECT_FALSE((TokenRange{0, 100}.empty()));
}

TEST(TokenRangeTest, SizeIsZeroForEmpty) {
    EXPECT_EQ((TokenRange{10, 5}.size()), 0u);
}

TEST(TokenRangeTest, SizeIsOneForSingleElement) {
    EXPECT_EQ((TokenRange{5, 5}.size()), 1u);
    EXPECT_EQ((TokenRange{0, 0}.size()), 1u);
}

TEST(TokenRangeTest, SizeIsLastMinusBeginPlusOne) {
    EXPECT_EQ((TokenRange{3, 7}.size()), 5u);
    EXPECT_EQ((TokenRange{0, 255}.size()), 256u);
}

TEST(TokenRangeTest, ContainsReturnsTrueForBoundaryTokens) {
    TokenRange r{10, 20};
    EXPECT_TRUE(r.contains(10));
    EXPECT_TRUE(r.contains(20));
    EXPECT_TRUE(r.contains(15));
}

TEST(TokenRangeTest, ContainsReturnsFalseOutsideRange) {
    TokenRange r{10, 20};
    EXPECT_FALSE(r.contains(9));
    EXPECT_FALSE(r.contains(21));
}

TEST(TokenRangeTest, ContainsReturnsFalseForEmptyRange) {
    TokenRange r;  // default {1, 0}
    EXPECT_FALSE(r.contains(0));
    EXPECT_FALSE(r.contains(1));
}

// ── max_dict_size ─────────────────────────────────────────────────────────────

TEST(MaxDictSizeTest, Bit12Is4096) {
    EXPECT_EQ(max_dict_size(12), size_t(4096));
}

TEST(MaxDictSizeTest, Bit16Is65536) {
    EXPECT_EQ(max_dict_size(16), size_t(65536));
}

TEST(MaxDictSizeTest, IsPowerOfTwo) {
    for (BitWidth b = 9; b <= 16; ++b)
        EXPECT_EQ(max_dict_size(b), size_t(1) << b);
}

// ── is_valid_bits ─────────────────────────────────────────────────────────────

TEST(IsValidBitsTest, AcceptsRange9To16) {
    for (BitWidth b = 9; b <= 16; ++b)
        EXPECT_TRUE(is_valid_bits(b)) << "expected true for bits=" << int(b);
}

TEST(IsValidBitsTest, Rejects8And17) {
    EXPECT_FALSE(is_valid_bits(8));
    EXPECT_FALSE(is_valid_bits(17));
}

TEST(IsValidBitsTest, Rejects0And255) {
    EXPECT_FALSE(is_valid_bits(0));
    EXPECT_FALSE(is_valid_bits(255));
}

// ── MAX_TOKEN_SIZE ────────────────────────────────────────────────────────────

TEST(MaxTokenSizeTest, Is16) {
    EXPECT_EQ(MAX_TOKEN_SIZE, size_t(16));
}
