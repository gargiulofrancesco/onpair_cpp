#include <onpair/core/dictionary.h>
#include <onpair/core/dictionary_view.h>
#include <gtest/gtest.h>
#include <type_traits>
#include <cstring>

using namespace onpair;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a sorted dict: token 0 = "a", token 1 = "b", token 2 = "c", …
// Just three single-byte tokens in sorted order.
static Dictionary make_abc()
{
    Dictionary s;
    s.bytes   = {'a', 'b', 'c'};
    s.offsets = {0, 1, 2, 3};
    return s;
}

// Build a dict: token 0 = "a", token 1 = "bc", token 2 = "def"
static Dictionary make_varying_tokens()
{
    Dictionary d;
    d.bytes   = {'a', 'b', 'c', 'd', 'e', 'f'};
    d.offsets = {0, 1, 3, 6};
    return d;
}

// Build sorted dict with tokens: "a", "ab", "b" (lexicographic order).
static Dictionary make_prefix()
{
    Dictionary s;
    // "a" = token 0, "ab" = token 1, "b" = token 2
    s.bytes   = {'a', 'a', 'b', 'b'};
    s.offsets = {0, 1, 3, 4};
    return s;
}

// ── DictionaryView ────────────────────────────────────────────────────────────

TEST(DictionaryViewTest, SpanReturnsCorrectRange) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    EXPECT_EQ(v.span(0).begin, 0u); EXPECT_EQ(v.span(0).end, 1u);
    EXPECT_EQ(v.span(1).begin, 1u); EXPECT_EQ(v.span(1).end, 3u);
    EXPECT_EQ(v.span(2).begin, 3u); EXPECT_EQ(v.span(2).end, 6u);
}

TEST(DictionaryViewTest, DataPointsToCorrectByte) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    EXPECT_EQ(*v.data(0), 'a');
    EXPECT_EQ(*v.data(1), 'b');  // "bc" starts at 'b'
    EXPECT_EQ(*v.data(2), 'd');  // "def" starts at 'd'
}

TEST(DictionaryViewTest, TokenSizeIsConsistentWithSpan) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    for (Token t = 0; t < 3; ++t)
        EXPECT_EQ(v.token_size(t), v.span(t).size());
}

TEST(DictionaryViewTest, TokenSizes) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    EXPECT_EQ(v.token_size(0), 1u);  // "a"
    EXPECT_EQ(v.token_size(1), 2u);  // "bc"
    EXPECT_EQ(v.token_size(2), 3u);  // "def"
}

TEST(DictionaryViewTest, NumTokensMatchesStorage) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    EXPECT_EQ(v.num_tokens(), 3u);
}

TEST(DictionaryViewTest, RawPointersPointIntoStorageBuffers) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    EXPECT_EQ(v.raw_bytes(),   d.bytes.data());
    EXPECT_EQ(v.raw_offsets(), d.offsets.data());
}

TEST(DictionaryViewTest, AcceptsEmptyDictionary) {
    Dictionary d;
    DictionaryView v(d);  // must compile and not crash
    EXPECT_EQ(v.num_tokens(), 0u);
}

TEST(DictionaryViewTest, BytesUsedMatchesDictionary) {
    auto d = make_varying_tokens();
    DictionaryView v(d);
    EXPECT_EQ(v.bytes_used(), d.bytes_used());
}

TEST(DictionaryViewTest, BytesUsedUnaffectedByPadding) {
    auto d = make_varying_tokens();
    const size_t before_pad = d.bytes_used();
    d.pad_for_decoder();
    DictionaryView v(d);
    // bytes_used() should report the logical size (no padding), same as before.
    EXPECT_EQ(v.bytes_used(), before_pad);
}

// ── DictionaryView::prefix_range ────────────────────────────────────────

TEST(PrefixRangeTest, EmptyDictionaryReturnsEmptyRange) {
    Dictionary ss;
    DictionaryView v(ss);
    const uint8_t p = 'a';
    EXPECT_TRUE(v.prefix_range(&p, 1).empty());
}

TEST(PrefixRangeTest, ExactSingleTokenMatch) {
    auto ss = make_abc();
    DictionaryView v(ss);
    const uint8_t p = 'b';
    auto r = v.prefix_range(&p, 1);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.begin, 1u);  // "b" is at index 1
    EXPECT_EQ(r.last,  1u);
}

TEST(PrefixRangeTest, PrefixMatchesMultipleTokens) {
    // Dict: "a"=0, "ab"=1, "b"=2
    // prefix "a" should match token 0 ("a") and token 1 ("ab")
    auto ss = make_prefix();
    DictionaryView v(ss);
    const uint8_t p = 'a';
    auto r = v.prefix_range(&p, 1);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.begin, 0u);
    EXPECT_EQ(r.last,  1u);
    EXPECT_EQ(r.size(), 2u);
}

TEST(PrefixRangeTest, NoMatchReturnsEmptyRange) {
    auto ss = make_abc();
    DictionaryView v(ss);
    const uint8_t p = 'z';
    EXPECT_TRUE(v.prefix_range(&p, 1).empty());
}

TEST(PrefixRangeTest, PrefixLongerThanMaxTokenSizeReturnsEmpty) {
    auto ss = make_abc();
    DictionaryView v(ss);
    const uint8_t buf[MAX_TOKEN_SIZE + 1] = {};
    EXPECT_TRUE(v.prefix_range(buf, MAX_TOKEN_SIZE + 1).empty());
}

TEST(PrefixRangeTest, AllFFBytesPrefix) {
    Dictionary ss;
    ss.bytes   = {0xFF, 0xFF, 0xFF};
    ss.offsets = {0, 1, 3};  // token 0 = {0xFF}, token 1 = {0xFF, 0xFF}
    DictionaryView v(ss);
    const uint8_t p = 0xFF;
    auto r = v.prefix_range(&p, 1);
    // Both tokens start with 0xFF → both should be in range.
    EXPECT_EQ(r.begin, 0u);
    EXPECT_EQ(r.last,  1u);
}

TEST(PrefixRangeTest, ExactLengthMatchFirstAndOnlyToken) {
    Dictionary ss;
    ss.bytes   = {'h', 'e', 'l', 'l', 'o'};
    ss.offsets = {0, 5};
    DictionaryView v(ss);
    const uint8_t buf[] = {'h', 'e', 'l', 'l', 'o'};
    auto r = v.prefix_range(buf, 5);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.begin, 0u);
    EXPECT_EQ(r.last,  0u);
}

TEST(PrefixRangeTest, ContainsReturnsTrueForAllTokensInRange) {
    auto ss = make_prefix();
    DictionaryView v(ss);
    const uint8_t p = 'a';
    auto r = v.prefix_range(&p, 1);
    EXPECT_TRUE(r.contains(0));
    EXPECT_TRUE(r.contains(1));
    EXPECT_FALSE(r.contains(2));
}

// ── Additional edge cases ──────────────────────────────────────────────────

TEST(PrefixRangeTest, EmptyPatternMatchesAllTokens) {
    // plen=0: empty prefix is a prefix of every token.
    // Implementation: lower_bound converges to 0; upper overflow → hi=n.
    // Use a valid (non-null) pointer with length 0 to avoid any UB in memcmp.
    auto ss = make_abc();  // 3 tokens: "a", "b", "c"
    DictionaryView v(ss);
    const uint8_t dummy = 0;
    auto r = v.prefix_range(&dummy, 0);
    EXPECT_EQ(r.size(), 3u);
    EXPECT_EQ(r.begin, Token(0));
    EXPECT_EQ(r.last,  Token(2));
}

TEST(PrefixRangeTest, EmptyPatternOnSingleTokenDict) {
    Dictionary ss;
    ss.bytes   = {'x'};
    ss.offsets = {0, 1};
    DictionaryView v(ss);
    const uint8_t dummy = 0;
    auto r = v.prefix_range(&dummy, 0);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.size(), 1u);
}

TEST(PrefixRangeTest, PatternExactlyMaxTokenSizeCanMatch) {
    // A MAX_TOKEN_SIZE-byte token queried with a MAX_TOKEN_SIZE-byte prefix
    // must return a non-empty range.
    Dictionary ss;
    ss.bytes.assign(MAX_TOKEN_SIZE, 'z');
    ss.offsets = {0, static_cast<uint32_t>(MAX_TOKEN_SIZE)};
    DictionaryView v(ss);
    auto r = v.prefix_range(ss.bytes.data(), MAX_TOKEN_SIZE);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.begin, Token(0));
}

TEST(PrefixRangeTest, AllFFMultiBytePrefix) {
    // prefix = {0xFF, 0xFF}: upper-bound increment overflows to n (all 0xFF),
    Dictionary ss;
    ss.bytes   = {0xFF, 0xFF, 0xFF};
    ss.offsets = {0, 1, 3};  // token 0 = {0xFF}, token 1 = {0xFF, 0xFF}
    DictionaryView v(ss);
    const uint8_t p[2] = {0xFF, 0xFF};
    auto r = v.prefix_range(p, 2);
    // Only token 1 has prefix {0xFF, 0xFF}. Token 0 is {0xFF} which is a
    // prefix OF the query, not the other way around.
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.begin, Token(1));
    EXPECT_EQ(r.last,  Token(1));
}

TEST(PrefixRangeTest, AllFFPrefixBeyondAllTokens) {
    // No token >= {0xFF, 0xFF} when dict only has {0xFE}.
    Dictionary ss;
    ss.bytes   = {0xFE};
    ss.offsets = {0, 1};
    DictionaryView v(ss);
    const uint8_t p[2] = {0xFF, 0xFF};
    auto r = v.prefix_range(p, 2);
    EXPECT_TRUE(r.empty());
}

TEST(PrefixRangeTest, SingleTokenDictMatchingPrefix) {
    // Dict has one token "hello"; prefix "he" must return that token.
    Dictionary ss;
    ss.bytes   = {'h', 'e', 'l', 'l', 'o'};
    ss.offsets = {0, 5};
    DictionaryView v(ss);
    const uint8_t p[] = {'h', 'e'};
    auto r = v.prefix_range(p, 2);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.begin, Token(0));
}

TEST(PrefixRangeTest, SingleTokenDictNonMatchingPrefix) {
    Dictionary ss;
    ss.bytes   = {'h', 'e', 'l', 'l', 'o'};
    ss.offsets = {0, 5};
    DictionaryView v(ss);
    const uint8_t p[] = {'x'};
    auto r = v.prefix_range(p, 1);
    EXPECT_TRUE(r.empty());
}

TEST(PrefixRangeTest, OverlappingPrefixes_DeepNesting) {
    // Dict: "a"=0, "aa"=1, "aaa"=2, "b"=3
    // Stresses the graduated binary search on tokens sharing nested prefixes.
    Dictionary ss;
    ss.bytes   = {'a', 'a', 'a', 'a', 'a', 'a', 'b'};
    ss.offsets = {0, 1, 3, 6, 7};
    DictionaryView v(ss);

    const uint8_t p_a[]   = {'a'};
    auto r_a = v.prefix_range(p_a, 1);
    EXPECT_EQ(r_a.size(), 3u);  // "a", "aa", "aaa"
    EXPECT_EQ(r_a.begin, Token(0));
    EXPECT_EQ(r_a.last,  Token(2));

    const uint8_t p_aa[]  = {'a', 'a'};
    auto r_aa = v.prefix_range(p_aa, 2);
    EXPECT_EQ(r_aa.size(), 2u);  // "aa", "aaa"
    EXPECT_EQ(r_aa.begin, Token(1));
    EXPECT_EQ(r_aa.last,  Token(2));

    const uint8_t p_aaa[] = {'a', 'a', 'a'};
    auto r_aaa = v.prefix_range(p_aaa, 3);
    EXPECT_EQ(r_aaa.size(), 1u);  // "aaa" only
    EXPECT_EQ(r_aaa.begin, Token(2));

    const uint8_t p_b[]   = {'b'};
    auto r_b = v.prefix_range(p_b, 1);
    EXPECT_EQ(r_b.size(), 1u);
    EXPECT_EQ(r_b.begin, Token(3));
}

TEST(PrefixRangeTest, ContiguousRangeBounds_NoSpillover) {
    // Dict: "apple"=0, "apt"=1, "b"=2
    // Prefix "ap" must return exactly 2 tokens; "b" must NOT be included.
    Dictionary ss;
    ss.bytes   = {'a', 'p', 'p', 'l', 'e',   // "apple"
                  'a', 'p', 't',              // "apt"
                  'b'};                       // "b"
    ss.offsets = {0, 5, 8, 9};
    DictionaryView v(ss);
    const uint8_t p[] = {'a', 'p'};
    auto r = v.prefix_range(p, 2);
    EXPECT_EQ(r.size(), 2u);
    EXPECT_FALSE(r.contains(Token(2)));  // "b" must not be in range
}

TEST(PrefixRangeTest, PrefixEqualsFullTokenContent) {
    // Dict: "ab"=0, "abc"=1, "abd"=2, "b"=3
    // Prefix "ab" (equal to token 0's full content) should match tokens 0, 1, 2.
    Dictionary ss;
    ss.bytes   = {'a', 'b',           // "ab"
                  'a', 'b', 'c',      // "abc"
                  'a', 'b', 'd',      // "abd"
                  'b'};               // "b"
    ss.offsets = {0, 2, 5, 8, 9};
    DictionaryView v(ss);
    const uint8_t p[] = {'a', 'b'};
    auto r = v.prefix_range(p, 2);
    EXPECT_EQ(r.size(), 3u);
    EXPECT_EQ(r.begin, Token(0));
    EXPECT_EQ(r.last,  Token(2));
    EXPECT_FALSE(r.contains(Token(3)));  // "b" must not be in range
}
