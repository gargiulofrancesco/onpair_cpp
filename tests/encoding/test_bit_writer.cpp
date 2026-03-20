#include <onpair/core/store.h>
#include <onpair/encoding/parsing/bit_writer.h>
#include <onpair/decoding/token_cursor.h>
#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

using namespace onpair;
using namespace onpair::encoding;
using namespace onpair::decoding;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::vector<Token> roundtrip(BitWidth bits, const std::vector<Token>& tokens)
{
    Store store;
    store.bit_width = bits;
    {
        BitWriter writer(store);
        for (Token t : tokens) writer.write(t);
    } // destructor flushes

    std::vector<Token> out;
    out.reserve(tokens.size());
    dispatch_bits(bits, [&](auto bw) {
        TokenCursor<bw.value> cursor(
            store.packed.data(),
            StreamSpan{0, static_cast<uint32_t>(tokens.size())});
        while (cursor.has_more())
            out.push_back(cursor.next());
    });
    return out;
}

static Token  max_token(BitWidth bits) { return Token((uint32_t(1) << bits) - 1); }

// Number of tokens that fills an integer number of 64-bit words exactly:
// lcm(bits, 64) / bits.
static int group_size(int bits)
{
    int lcm = bits;
    while (lcm % 64 != 0) lcm += bits;
    return lcm / bits;
}

// Expected number of uint64_t words needed to hold `n` tokens at `bits` bits each.
static size_t expected_packed_words(size_t n, BitWidth bits)
{
    return (n * bits + 63) / 64;
}

// ── Parameterized fixture ─────────────────────────────────────────────────────

class BitWriterTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, BitWriterTest,
    testing::Values(12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

// ── Degenerate inputs ─────────────────────────────────────────────────────────

TEST_P(BitWriterTest, ZeroTokensProducesEmptyPacked) {
    Store store;
    store.bit_width = static_cast<BitWidth>(GetParam());
    {
        BitWriter writer(store);
        EXPECT_EQ(writer.tokens_written(), 0u);
    }
    EXPECT_TRUE(store.packed.empty());
}

// ── Structural invariants ─────────────────────────────────────────────────────

TEST_P(BitWriterTest, PackedSizeIsConsistentWithTokenCount) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    // Use a count that does not fall on an exact group boundary, so a partial
    // word is always present and must be flushed.
    const int n = group_size(GetParam()) + 3;
    Store store;
    store.bit_width = bw;
    { BitWriter writer(store); for (int i = 0; i < n; ++i) writer.write(Token(1)); }

    EXPECT_EQ(store.packed.size(), expected_packed_words(n, bw));
}

TEST_P(BitWriterTest, TokensWrittenCountEqualsTokensWritten) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    Store store;
    store.bit_width = bw;
    BitWriter writer(store);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(writer.tokens_written(), static_cast<size_t>(i));
        writer.write(Token(i));
    }
    EXPECT_EQ(writer.tokens_written(), 10u);
    writer.flush();
    EXPECT_EQ(writer.tokens_written(), 10u);  // flush must not alter the count
}

// ── Round-trip correctness ────────────────────────────────────────────────────

TEST_P(BitWriterTest, SingleZeroTokenRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto result = roundtrip(bw, {0});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 0);
}

TEST_P(BitWriterTest, SingleMaxTokenRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    const Token mx = max_token(bw);
    auto result = roundtrip(bw, {mx});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], mx);
}

TEST_P(BitWriterTest, MixedZeroAndMaxRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    const Token mx = max_token(bw);
    std::vector<Token> tokens;
    for (int i = 0; i < 30; ++i)
        tokens.push_back(i % 2 == 0 ? Token(0) : mx);
    EXPECT_EQ(roundtrip(bw, tokens), tokens);
}

TEST_P(BitWriterTest, IncrementingTokensRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    const int range = max_token(bw) + 1;
    std::vector<Token> tokens;
    for (int i = 0; i < 200; ++i)
        tokens.push_back(Token(i % range));
    EXPECT_EQ(roundtrip(bw, tokens), tokens);
}

// ── Word-boundary cases ───────────────────────────────────────────────────────

// Test the three counts around a perfectly word-aligned boundary:
// one token short, exactly aligned, and one token over.
TEST_P(BitWriterTest, GroupBoundaryTokenCounts) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    const int gs = group_size(GetParam());
    for (int count : {gs - 1, gs, gs + 1}) {
        std::vector<Token> tokens(count, Token(GetParam()));
        EXPECT_EQ(roundtrip(bw, tokens), tokens)
            << "bits=" << GetParam() << " count=" << count;
    }
}

// ── Non-parameterized tests ───────────────────────────────────────────────────

// The destructor alone (no explicit flush) must commit the partial word.
TEST(BitWriterTest_Manual, ImplicitFlushViaDestructor) {
    Store store;
    store.bit_width = 16;
    {
        BitWriter writer(store);
        writer.write(0xABCD);
        // No explicit flush(); the destructor is the only flush path.
    }
    ASSERT_EQ(store.packed.size(), 1u);
    EXPECT_EQ(store.packed[0] & 0xFFFF, 0xABCDull);
}

// Explicit flush() before the destructor must not double-push the partial word.
TEST(BitWriterTest_Manual, ExplicitFlushIsIdempotent) {
    Store store;
    store.bit_width = 16;
    {
        BitWriter writer(store);
        writer.write(0xABCD);
        writer.flush();
        // Destructor calls flush() again; shift_ == 0 so it must be a no-op.
    }
    ASSERT_EQ(store.packed.size(), 1u);
    EXPECT_EQ(store.packed[0] & 0xFFFF, 0xABCDull);
}

// The BitWriter constructor must clear any pre-existing data in store.packed,
// so that a second encoding pass over the same Store always starts fresh.
TEST(BitWriterTest_Manual, ConstructorClearsPreviousData) {
    Store store;
    store.bit_width = 16;
    { BitWriter w(store); w.write(0xAAAA); }
    ASSERT_EQ(store.packed.size(), 1u);

    { BitWriter w(store); w.write(0xBBBB); }
    ASSERT_EQ(store.packed.size(), 1u);
    EXPECT_EQ(store.packed[0] & 0xFFFF, 0xBBBBull);
}

// Verify the raw bit layout when a token straddles a 64-bit word boundary.
//
// At 12 bits, the first straddling occurs at token index 5: the preceding
// five tokens consume exactly 60 bits, leaving 4 bits in word 0 and spilling
// the remaining 8 bits into word 1.
//
//   tokens: [0xABC, 0, 0, 0, 0, 0x123]
//
//   word 0 bits  0-11: 0xABC               (token 0)
//   word 0 bits 12-59: 0x0…0               (tokens 1-4)
//   word 0 bits 60-63: low  4 bits of 0x123 = 0x3
//   ─────────────────────────────────────────────────── word boundary
//   word 1 bits  0- 7: high 8 bits of 0x123 = 0x12
TEST(BitWriterTest_Manual, StradlingBitLayoutAt12Bits) {
    const std::vector<Token> tokens = {0xABC, 0, 0, 0, 0, 0x123};
    Store store;
    store.bit_width = 12;
    { BitWriter writer(store); for (Token t : tokens) writer.write(t); }

    ASSERT_EQ(store.packed.size(), 2u);
    EXPECT_EQ(store.packed[0], 0x3000000000000ABCull);
    EXPECT_EQ(store.packed[1], 0x0000000000000012ull);

    EXPECT_EQ(roundtrip(12, tokens), tokens);
}
