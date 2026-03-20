#include <onpair/core/dictionary.h>
#include <gtest/gtest.h>
#include <type_traits>

using namespace onpair;

// ── Dictionary ────────────────────────────────────────────────────────────────

TEST(DictionaryTest, NumTokensIsZeroWhenOffsetsEmpty) {
    Dictionary d;
    EXPECT_EQ(d.num_tokens(), 0u);
}

TEST(DictionaryTest, NumTokensIsOffsetsSizeMinusOne) {
    Dictionary d;
    d.offsets = {0, 3, 5, 8};  // 3 tokens
    EXPECT_EQ(d.num_tokens(), 3u);
}

TEST(DictionaryTest, NumTokensIsSingleEntryMinusOne) {
    Dictionary d;
    d.offsets = {0, 7};  // 1 token of 7 bytes
    EXPECT_EQ(d.num_tokens(), 1u);
}

TEST(DictionaryTest, BytesUsedAccountsForBothVectors) {
    Dictionary d;
    d.bytes   = {0x00, 0x01, 0x02};      // 3 bytes
    d.offsets = {0, 1, 2, 3};             // 4 uint32_t = 16 bytes
    const size_t expected = 3 + 4 * sizeof(uint32_t);
    EXPECT_EQ(d.bytes_used(), expected);
}

TEST(DictionaryTest, BytesUsedIsZeroWhenEmpty) {
    Dictionary d;
    EXPECT_EQ(d.bytes_used(), 0u);
}

// ── pad_for_decoder ───────────────────────────────────────────────────────────

TEST(DictionaryTest, PadForDecoderAddsTrailingZeros) {
    Dictionary d;
    d.bytes   = {'h', 'e', 'l', 'l', 'o'};
    d.offsets = {0, 5};  // 1 token of 5 bytes
    d.pad_for_decoder();
    // Should have 5 + (MAX_TOKEN_SIZE - 5) = MAX_TOKEN_SIZE bytes.
    EXPECT_EQ(d.bytes.size(), MAX_TOKEN_SIZE);
}

TEST(DictionaryTest, PadForDecoderIsIdempotent) {
    Dictionary d;
    d.bytes   = {'a', 'b'};
    d.offsets = {0, 2};
    d.pad_for_decoder();
    const size_t size1 = d.bytes.size();
    d.pad_for_decoder();  // second call should be a no-op
    EXPECT_EQ(d.bytes.size(), size1);
}

TEST(DictionaryTest, PadForDecoderNoOpForMaxTokenSize) {
    Dictionary d;
    d.bytes.assign(MAX_TOKEN_SIZE, 'x');
    d.offsets = {0, static_cast<uint32_t>(MAX_TOKEN_SIZE)};
    d.pad_for_decoder();
    // Last token is exactly MAX_TOKEN_SIZE: no padding needed.
    EXPECT_EQ(d.bytes.size(), MAX_TOKEN_SIZE);
}

TEST(DictionaryTest, PadForDecoderPadsBasedOnLastTokenOnly) {
    // Multiple tokens: padding depends only on the last token's length.
    // token 0 = "ab" (2 bytes), token 1 = "cde" (3 bytes)
    Dictionary d;
    d.bytes   = {'a', 'b', 'c', 'd', 'e'};
    d.offsets = {0, 2, 5};
    d.pad_for_decoder();
    // Padding = MAX_TOKEN_SIZE - 3 = 13 zero bytes appended after the 5 data bytes.
    EXPECT_EQ(d.bytes.size(), 5u + (MAX_TOKEN_SIZE - 3));
}

TEST(DictionaryTest, BytesUsedUnchangedAfterPadding) {
    // bytes_used() must use offsets.back() (logical size), not bytes.size()
    // (which includes decoder padding).
    Dictionary d;
    d.bytes   = {'h', 'e', 'l', 'l', 'o'};
    d.offsets = {0, 5};
    const size_t before = d.bytes_used();
    d.pad_for_decoder();
    EXPECT_EQ(d.bytes_used(), before);
    // Post-condition: bytes.size() > offsets.back() (padding exists)
    EXPECT_GT(d.bytes.size(), d.offsets.back());
}

TEST(DictionaryTest, PadForDecoderNoOpWhenFewerThanTwoOffsets) {
    Dictionary d;
    d.offsets = {0};  // sentinel only, no tokens
    d.pad_for_decoder();
    EXPECT_TRUE(d.bytes.empty());
}

TEST(DictionaryTest, PadForDecoderZeroPaddingBytesAreZero) {
    Dictionary d;
    d.bytes   = {'x', 'y'};
    d.offsets = {0, 2};
    d.pad_for_decoder();
    // All bytes beyond offset 2 should be zero.
    for (size_t i = 2; i < d.bytes.size(); ++i)
        EXPECT_EQ(d.bytes[i], 0u) << "non-zero padding at index " << i;
}
