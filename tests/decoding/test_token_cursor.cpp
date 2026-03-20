#include <onpair/core/store.h>
#include <onpair/encoding/parsing/bit_writer.h>
#include <onpair/decoding/token_cursor.h>
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

using namespace onpair;
using namespace onpair::encoding;
using namespace onpair::decoding;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a packed buffer by writing `tokens` with the given bit width.
static Store make_packed(BitWidth bits, const std::vector<Token>& tokens)
{
    Store store;
    store.bit_width = bits;
    {
        BitWriter writer(store);
        for (Token t : tokens) writer.write(t);
    }
    return store;
}

// Read all tokens from a span using dispatch_bits.
static std::vector<Token> read_all(const Store& store,
                                   uint32_t begin, uint32_t end)
{
    std::vector<Token> out;
    dispatch_bits(store.bit_width, [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{begin, end});
        while (cursor.has_more())
            out.push_back(cursor.next());
    });
    return out;
}

static Token max_token(int bits) { return Token((uint32_t(1) << bits) - 1); }

// Returns the number of tokens that fill an exact number of uint64_t words:
//   group_size = lcm(Bits, 64) / Bits
static int group_size(int bits)
{
    int lcm = bits;
    while (lcm % 64 != 0) lcm += bits;
    return lcm / bits;
}

// Returns the first token index whose bits straddle a uint64_t word boundary
// (i.e. (i * bits) % 64 + bits > 64), or -1 if none exist within two groups.
// For 16-bit tokens, every group fills exactly one 64-bit word so no token
// ever straddles; this returns -1.
static int first_straddle_index(int bits)
{
    const int limit = 2 * group_size(bits);
    for (int i = 0; i < limit; ++i) {
        if ((i * bits) % 64 + bits > 64)
            return i;
    }
    return -1;
}

// ── Typed test suite for all five bit widths ──────────────────────────────────

// We use integral_constant so Bits is available at compile time inside tests.
template<int B>
using BW = std::integral_constant<int, B>;

using AllBitWidths = testing::Types<BW<12>, BW<13>, BW<14>, BW<15>, BW<16>>;

template<typename T>
class TokenCursorTypedTest : public testing::Test {
protected:
    static constexpr int Bits = T::value;

    Store make(const std::vector<Token>& tokens) const {
        return make_packed(static_cast<BitWidth>(Bits), tokens);
    }
    std::vector<Token> read(const Store& s, uint32_t b, uint32_t e) const {
        return read_all(s, b, e);
    }
    Token max() const { return max_token(Bits); }
    int   gs()  const { return group_size(Bits); }
};

TYPED_TEST_SUITE(TokenCursorTypedTest, AllBitWidths);

// ── Basic behaviour ───────────────────────────────────────────────────────────

// Empty span: has_more() is immediately false.
TYPED_TEST(TokenCursorTypedTest, EmptySpanHasNoMore) {
    auto store = this->make({});
    EXPECT_TRUE(this->read(store, 0, 0).empty());
}

// Single token, value 0.
TYPED_TEST(TokenCursorTypedTest, SingleZeroToken) {
    auto store = this->make({0});
    auto tokens = this->read(store, 0, 1);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], Token(0));
}

// Single token, max value for the bit width.
TYPED_TEST(TokenCursorTypedTest, SingleMaxToken) {
    Token mx = this->max();
    auto store = this->make({mx});
    auto tokens = this->read(store, 0, 1);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0], mx);
}

// 20 tokens round-trip with varied values.
TYPED_TEST(TokenCursorTypedTest, TwentyTokensRoundTrip) {
    Token mx = this->max();
    std::vector<Token> expected;
    for (int i = 0; i < 20; ++i) expected.push_back(Token(i % (int(mx) + 1)));
    auto store  = this->make(expected);
    auto result = this->read(store, 0, 20);
    EXPECT_EQ(result, expected);
}

// ── Cursor constructed with non-zero begin ────────────────────────────────────

// Constructing with StreamSpan{k, n} where k > 0 must set bit_pos_ = k * Bits,
// not 0.  This tests the constructor directly without any reset_to() call.
TYPED_TEST(TokenCursorTypedTest, ConstructedWithNonZeroBegin) {
    std::vector<Token> tokens = {10, 20, 30, 40, 50};
    auto store = this->make(tokens);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{2, 5});
        EXPECT_EQ(cursor.remaining(), 3u);
        EXPECT_EQ(cursor.next(), Token(30));
        EXPECT_EQ(cursor.next(), Token(40));
        EXPECT_EQ(cursor.next(), Token(50));
        EXPECT_FALSE(cursor.has_more());
    });
}

// ── Default constructor + reset_to() ─────────────────────────────────────────

// TokenCursor(packed) leaves the span unset (bit_pos_ == bit_end_ == 0).
// reset_to() must correctly initialise bit_pos_ and bit_end_ before reading.
TYPED_TEST(TokenCursorTypedTest, DefaultConstructorThenReset) {
    std::vector<Token> tokens = {7, 8, 9};
    auto store = this->make(tokens);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data());  // no span yet
        EXPECT_FALSE(cursor.has_more());
        cursor.reset_to(StreamSpan{0, 3});
        EXPECT_EQ(cursor.remaining(), 3u);
        EXPECT_EQ(cursor.next(), Token(7));
        EXPECT_EQ(cursor.next(), Token(8));
        EXPECT_EQ(cursor.next(), Token(9));
        EXPECT_FALSE(cursor.has_more());
    });
}

// ── Subspan isolation ─────────────────────────────────────────────────────────

// Reading a subspan [1, 4) from a larger buffer must yield exactly the middle
// tokens and not bleed the sentinel values written at the edges.
TYPED_TEST(TokenCursorTypedTest, SubspanIsolation) {
    const Token edge = Token(this->Bits + 10);   // distinct sentinel
    std::vector<Token> tokens = {edge, 1, 2, 3, edge};
    auto store = this->make(tokens);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{1, 4});
        EXPECT_EQ(cursor.remaining(), 3u);
        EXPECT_EQ(cursor.next(), Token(1));
        EXPECT_EQ(cursor.next(), Token(2));
        EXPECT_EQ(cursor.next(), Token(3));
        EXPECT_FALSE(cursor.has_more());
    });
}

// ── peek() ────────────────────────────────────────────────────────────────────

// peek() returns the same value as the subsequent next(), at every position,
// without advancing the cursor.
TYPED_TEST(TokenCursorTypedTest, PeekAtEveryPosition) {
    std::vector<Token> expected;
    for (int i = 0; i < 10; ++i) expected.push_back(Token(i * 3 + 1));
    auto store = this->make(expected);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{0, 10});
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_TRUE(cursor.has_more()) << "at position " << i;
            EXPECT_EQ(cursor.peek(), expected[i]) << "peek mismatch at " << i;
            EXPECT_EQ(cursor.next(), expected[i]) << "next mismatch at " << i;
        }
        EXPECT_FALSE(cursor.has_more());
    });
}

// ── reset_to() ────────────────────────────────────────────────────────────────

// reset_to() repositions to the start of the same buffer.
TYPED_TEST(TokenCursorTypedTest, ResetToBeginning) {
    std::vector<Token> expected = {1, 2, 3, 4, 5};
    auto store = this->make(expected);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{0, 5});
        cursor.next(); cursor.next();          // advance 2
        cursor.reset_to(StreamSpan{0, 5});     // reset to start
        std::vector<Token> got;
        while (cursor.has_more()) got.push_back(cursor.next());
        EXPECT_EQ(got, expected);
    });
}

// reset_to() mid-stream correctly repositions and updates remaining().
TYPED_TEST(TokenCursorTypedTest, ResetToMidStream) {
    std::vector<Token> tokens = {10, 20, 30, 40, 50};
    auto store = this->make(tokens);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{0, 5});
        cursor.next(); cursor.next();            // consume index 0, 1
        cursor.reset_to(StreamSpan{2, 5});       // reposition to [2, 5)
        EXPECT_EQ(cursor.remaining(), 3u);
        EXPECT_EQ(cursor.next(), Token(30));
        EXPECT_EQ(cursor.next(), Token(40));
        EXPECT_EQ(cursor.next(), Token(50));
        EXPECT_FALSE(cursor.has_more());
    });
}

// Multiple sequential reset_to() calls correctly rebuild internal state.
TYPED_TEST(TokenCursorTypedTest, MultipleResets) {
    std::vector<Token> tokens = {1, 2, 3, 4, 5, 6, 7, 8};
    auto store = this->make(tokens);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{0, 8});
        for (int i = 0; i < 4; ++i) cursor.next();

        // Full reset; re-read everything.
        cursor.reset_to(StreamSpan{0, 8});
        EXPECT_EQ(cursor.remaining(), 8u);
        for (size_t i = 0; i < tokens.size(); ++i)
            EXPECT_EQ(cursor.next(), tokens[i]) << "at i=" << i;

        // Reset to a smaller subrange [3, 6).
        cursor.reset_to(StreamSpan{3, 6});
        EXPECT_EQ(cursor.remaining(), 3u);
        EXPECT_EQ(cursor.next(), tokens[3]);
        EXPECT_EQ(cursor.next(), tokens[4]);
        EXPECT_EQ(cursor.next(), tokens[5]);
        EXPECT_FALSE(cursor.has_more());
    });
}

// remaining() tracks the new count correctly after each reset_to().
TYPED_TEST(TokenCursorTypedTest, RemainingAfterReset) {
    std::vector<Token> tokens(10, Token(7));
    auto store = this->make(tokens);
    dispatch_bits(static_cast<BitWidth>(this->Bits), [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{0, 10});
        cursor.next(); cursor.next(); cursor.next();
        EXPECT_EQ(cursor.remaining(), 7u);

        cursor.reset_to(StreamSpan{5, 10});
        EXPECT_EQ(cursor.remaining(), 5u);

        cursor.next();
        EXPECT_EQ(cursor.remaining(), 4u);

        cursor.reset_to(StreamSpan{0, 3});
        EXPECT_EQ(cursor.remaining(), 3u);
    });
}

// ── Word boundary tests ───────────────────────────────────────────────────────

// group_size() tokens fill an exact number of uint64_t words.  Tokens just
// before, at, and just after that boundary are decoded correctly.
// Three distinct sentinels catch offset-swap bugs that equal values would hide.
TYPED_TEST(TokenCursorTypedTest, WordGroupBoundary) {
    const int gs = this->gs();
    const Token sa = Token(this->Bits + 1);
    const Token sb = Token(this->Bits + 2);
    const Token sc = Token(this->Bits + 3);

    std::vector<Token> tokens(static_cast<size_t>(gs + 2), Token(1));
    tokens[static_cast<size_t>(gs - 1)] = sa;   // last token of first group
    tokens[static_cast<size_t>(gs)]     = sb;   // first token of second group
    tokens[static_cast<size_t>(gs + 1)] = sc;   // one past boundary

    auto store  = this->make(tokens);
    auto result = this->read(store, 0, static_cast<uint32_t>(tokens.size()));
    ASSERT_EQ(result.size(), tokens.size());
    EXPECT_EQ(result[static_cast<size_t>(gs - 1)], sa) << "last token before group boundary";
    EXPECT_EQ(result[static_cast<size_t>(gs)],     sb) << "first token of next group";
    EXPECT_EQ(result[static_cast<size_t>(gs + 1)], sc) << "token after group boundary";
}

// Two consecutive group boundaries with three distinct sentinels per boundary.
TYPED_TEST(TokenCursorTypedTest, TwoConsecutiveGroupBoundaries) {
    const int gs = this->gs();
    const Token sa = Token(this->Bits + 1);
    const Token sb = Token(this->Bits + 2);
    const Token sc = Token(this->Bits + 3);

    const int count = 2 * gs + 2;
    std::vector<Token> tokens(static_cast<size_t>(count), Token(1));
    // Around first boundary:
    tokens[static_cast<size_t>(gs - 1)] = sa;
    tokens[static_cast<size_t>(gs)]     = sb;
    tokens[static_cast<size_t>(gs + 1)] = sc;
    // Around second boundary (different assignment to catch aliasing bugs):
    tokens[static_cast<size_t>(2 * gs - 1)] = sc;
    tokens[static_cast<size_t>(2 * gs)]     = sa;
    tokens[static_cast<size_t>(2 * gs + 1)] = sb;

    auto store  = this->make(tokens);
    auto result = this->read(store, 0, static_cast<uint32_t>(count));
    ASSERT_EQ(result.size(), tokens.size());
    EXPECT_EQ(result[static_cast<size_t>(gs - 1)],     sa);
    EXPECT_EQ(result[static_cast<size_t>(gs)],         sb);
    EXPECT_EQ(result[static_cast<size_t>(gs + 1)],     sc);
    EXPECT_EQ(result[static_cast<size_t>(2 * gs - 1)], sc);
    EXPECT_EQ(result[static_cast<size_t>(2 * gs)],     sa);
    EXPECT_EQ(result[static_cast<size_t>(2 * gs + 1)], sb);
}

// The first position within a group where a token's bits genuinely straddle a
// uint64_t boundary: (i * Bits) % 64 + Bits > 64.  BitWriter splits such a
// token across two uint64_t words; TokenCursor must reassemble it correctly.
// For 16-bit tokens lcm(16, 64) == 64, so no intra-group straddle exists;
// the test is a no-op for that width.
TYPED_TEST(TokenCursorTypedTest, IntraGroupStraddlePosition) {
    const int idx = first_straddle_index(this->Bits);
    if (idx < 0) return;  // 16-bit: all tokens are word-aligned

    const Token sentinel = Token(this->Bits + 5);
    const int count = idx + 2;
    std::vector<Token> tokens(static_cast<size_t>(count), Token(1));
    tokens[static_cast<size_t>(idx)] = sentinel;

    auto store  = this->make(tokens);
    auto result = this->read(store, 0, static_cast<uint32_t>(count));
    ASSERT_EQ(result.size(), tokens.size());
    EXPECT_EQ(result[static_cast<size_t>(idx)], sentinel)
        << "token at intra-group straddle index " << idx
        << " (bit_offset_within_word=" << (idx * this->Bits) % 64 << ")";
}

// Max-value tokens at alternating positions across two full groups stress-tests
// every bit-packing phase, including all straddle positions.
TYPED_TEST(TokenCursorTypedTest, MaxValueAtEveryPhase) {
    const int count = 2 * this->gs() + 1;
    const Token mx  = this->max();
    std::vector<Token> tokens;
    tokens.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        tokens.push_back((i % 2 == 0) ? mx : Token(0));

    auto store  = this->make(tokens);
    auto result = this->read(store, 0, static_cast<uint32_t>(count));
    ASSERT_EQ(result.size(), tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i)
        EXPECT_EQ(result[i], tokens[i]) << "at position " << i;
}
