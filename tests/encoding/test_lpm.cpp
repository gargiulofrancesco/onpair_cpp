#include <onpair/encoding/lpm.h>
#include <onpair/core/dictionary.h>
#include <onpair/core/dictionary_view.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace onpair;
using namespace onpair::encoding;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Token insert_str(LongestPrefixMatcher& lpm, std::string_view sv)
{
    return lpm.insert(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

static std::pair<Token, size_t>
find_str(const LongestPrefixMatcher& lpm, std::string_view sv)
{
    return lpm.find_longest_match(
        reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// Build a complete dictionary with the 256 single-byte base tokens plus any
// optional multi-byte tokens passed as extra.
static Dictionary make_test_dictionary(
    std::initializer_list<std::string_view> extra = {})
{
    Dictionary dict;
    dict.offsets.push_back(0);
    for (int i = 0; i < 256; ++i) {
        dict.bytes.push_back(static_cast<uint8_t>(i));
        dict.offsets.push_back(static_cast<uint32_t>(dict.bytes.size()));
    }
    for (std::string_view sv : extra) {
        for (unsigned char c : sv)
            dict.bytes.push_back(c);
        dict.offsets.push_back(static_cast<uint32_t>(dict.bytes.size()));
    }
    return dict;
}

// ── Construction ──────────────────────────────────────────────────────────────

TEST(LpmConstructTest, DefaultConstructorSizeIs256) {
    LongestPrefixMatcher lpm;
    EXPECT_EQ(lpm.size(), size_t(256));
}

TEST(LpmConstructTest, AllSingleBytesFoundAfterConstruction) {
    LongestPrefixMatcher lpm;
    for (int i = 0; i < 256; ++i) {
        const uint8_t b = static_cast<uint8_t>(i);
        auto [tok, len] = lpm.find_longest_match(&b, 1);
        EXPECT_EQ(tok, Token(i))      << "wrong token for byte " << i;
        EXPECT_EQ(len, size_t(1))     << "wrong length for byte " << i;
    }
}

TEST(LpmConstructTest, ZeroByteIsToken0) {
    LongestPrefixMatcher lpm;
    const uint8_t b = 0x00;
    auto [tok, len] = lpm.find_longest_match(&b, 1);
    EXPECT_EQ(tok, Token(0));
    EXPECT_EQ(len, size_t(1));
}

TEST(LpmConstructTest, MaxByteIsToken255) {
    LongestPrefixMatcher lpm;
    const uint8_t b = 0xFF;
    auto [tok, len] = lpm.find_longest_match(&b, 1);
    EXPECT_EQ(tok, Token(255));
    EXPECT_EQ(len, size_t(1));
}

// ── Insert ────────────────────────────────────────────────────────────────────

TEST(LpmInsertTest, FirstInsertReturnsId256) {
    LongestPrefixMatcher lpm;
    EXPECT_EQ(insert_str(lpm, "ab"), Token(256));
}

TEST(LpmInsertTest, SubsequentInsertsIncrementId) {
    LongestPrefixMatcher lpm;
    EXPECT_EQ(insert_str(lpm, "ab"), Token(256));
    EXPECT_EQ(insert_str(lpm, "cd"), Token(257));
    EXPECT_EQ(insert_str(lpm, "ef"), Token(258));
}

TEST(LpmInsertTest, SizeGrowsWithEachInsert) {
    LongestPrefixMatcher lpm;
    EXPECT_EQ(lpm.size(), size_t(256));
    insert_str(lpm, "ab");
    EXPECT_EQ(lpm.size(), size_t(257));
    insert_str(lpm, "cd");
    EXPECT_EQ(lpm.size(), size_t(258));
}

// Short store boundary: exactly 8 bytes (== BUCKET_PREFIX_LEN) goes to short_.
TEST(LpmInsertTest, ExactlyEightBytesShortStore) {
    LongestPrefixMatcher lpm;
    const std::string_view s = "12345678";
    ASSERT_EQ(s.size(), size_t(8));
    Token id = insert_str(lpm, s);
    auto [tok, len] = find_str(lpm, s);
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(8));
}

// Long store boundary: exactly 9 bytes (> BUCKET_PREFIX_LEN) goes to long_.
TEST(LpmInsertTest, ExactlyNineBytesLongStore) {
    LongestPrefixMatcher lpm;
    const std::string_view s = "123456789";
    ASSERT_EQ(s.size(), size_t(9));
    Token id = insert_str(lpm, s);
    // search with trailing byte to ensure the result is exactly 9
    auto [tok, len] = find_str(lpm, "123456789X");
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(9));
}

// Max-size token.
TEST(LpmInsertTest, MaxTokenSizeInsertAndFind) {
    LongestPrefixMatcher lpm;
    const char pat[] = "0123456789abcdef";  // exactly 16 bytes
    ASSERT_EQ(strlen(pat), MAX_TOKEN_SIZE);
    Token id = lpm.insert(reinterpret_cast<const uint8_t*>(pat), MAX_TOKEN_SIZE);
    auto [tok, len] = lpm.find_longest_match(
        reinterpret_cast<const uint8_t*>(pat), MAX_TOKEN_SIZE);
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(MAX_TOKEN_SIZE));
}

// Binary data: sequence containing embedded zero bytes.
TEST(LpmInsertTest, SequenceWithEmbeddedZeroBytes) {
    LongestPrefixMatcher lpm;
    const uint8_t data[] = {0x00, 0x01, 0x02};
    Token id = lpm.insert(data, 3);
    auto [tok, len] = lpm.find_longest_match(data, 3);
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(3));
}

// ── find_longest_match ────────────────────────────────────────────────────────

TEST(LpmFindTest, SingleByteFoundWithCorrectId) {
    LongestPrefixMatcher lpm;
    const uint8_t b = 0x42;
    auto [tok, len] = lpm.find_longest_match(&b, 1);
    EXPECT_EQ(tok, Token(0x42));
    EXPECT_EQ(len, size_t(1));
}

// Longer token beats shorter one when both are in the matcher.
TEST(LpmFindTest, LongestMatchWinsOverShorter) {
    LongestPrefixMatcher lpm;
    insert_str(lpm, "abc");
    Token long_id = insert_str(lpm, "abcdefghi");
    auto [tok, len] = find_str(lpm, "abcdefghi");
    EXPECT_EQ(tok, long_id);
    EXPECT_EQ(len, size_t(9));
}

// Falls back to a shorter stored token when the longer one is absent.
TEST(LpmFindTest, FallsBackToShorterIfLongNotPresent) {
    LongestPrefixMatcher lpm;
    Token short_id = insert_str(lpm, "abc");
    auto [tok, len] = find_str(lpm, "abcdef");
    EXPECT_EQ(tok, short_id);
    EXPECT_EQ(len, size_t(3));
}

// Falls back all the way to a single-byte token when nothing else matches.
TEST(LpmFindTest, FallsBackToSingleByte) {
    LongestPrefixMatcher lpm;
    insert_str(lpm, "XY");
    // "XZ" does not match "XY"; only 'X' (token 88) is a valid prefix.
    auto [tok, len] = find_str(lpm, "XZ");
    EXPECT_EQ(tok, Token('X'));
    EXPECT_EQ(len, size_t(1));
}

// Input exactly as long as the stored pattern (no trailing bytes).
TEST(LpmFindTest, ExactMatchNoTrailingBytes) {
    LongestPrefixMatcher lpm;
    Token id = insert_str(lpm, "hello");
    auto [tok, len] = find_str(lpm, "hello");
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(5));
}

// Input shorter than every stored multi-byte token — falls back to single byte.
TEST(LpmFindTest, InputShorterThanStoredPattern_FallsToSingleByte) {
    LongestPrefixMatcher lpm;
    insert_str(lpm, "abcde");
    // "ab" is not stored; single byte 'a' (token 97) is the only match.
    auto [tok, len] = find_str(lpm, "ab");
    EXPECT_EQ(tok, Token('a'));
    EXPECT_EQ(len, size_t(1));
}

// Input shorter than longest stored pattern but longer pattern also stored.
TEST(LpmFindTest, InputShorterThanLongestPatternMatchesShorterToken) {
    LongestPrefixMatcher lpm;
    Token id2 = insert_str(lpm, "ab");
    insert_str(lpm, "abcde");
    auto [tok, len] = find_str(lpm, "ab");
    EXPECT_EQ(tok, id2);
    EXPECT_EQ(len, size_t(2));
}

// Boundary: 8-byte token in short store, searched with a longer input.
TEST(LpmFindTest, EightByteTokenWithLongerInput) {
    LongestPrefixMatcher lpm;
    Token id = insert_str(lpm, "ABCDEFGH");
    auto [tok, len] = find_str(lpm, "ABCDEFGHIJ");
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(8));
}

// 9-byte token (long store) wins over 8-byte token (short store).
TEST(LpmFindTest, NineByteBeatsEightByte) {
    LongestPrefixMatcher lpm;
    Token id8 = insert_str(lpm, "ABCDEFGH");
    Token id9 = insert_str(lpm, "ABCDEFGHI");
    auto [tok, len] = find_str(lpm, "ABCDEFGHIJ");
    EXPECT_EQ(tok, id9);
    EXPECT_EQ(len, size_t(9));
}

// One long token is a proper prefix of another (both in the long store).
TEST(LpmFindTest, ShortLongTokenIsPrefixOfLongerLongToken) {
    LongestPrefixMatcher lpm;
    Token id_short = insert_str(lpm, "ABCDEFGHI");    // 9 bytes
    Token id_long  = insert_str(lpm, "ABCDEFGHIJK");  // 11 bytes

    // 10 bytes available → only the 9-byte token matches.
    auto [tok_s, len_s] = find_str(lpm, "ABCDEFGHIJx");
    EXPECT_EQ(tok_s, id_short);
    EXPECT_EQ(len_s, size_t(9));

    // 12 bytes available → the 11-byte token matches.
    auto [tok_l, len_l] = find_str(lpm, "ABCDEFGHIJKx");
    EXPECT_EQ(tok_l, id_long);
    EXPECT_EQ(len_l, size_t(11));
}

// Multiple long tokens with the same 8-byte prefix but different suffixes.
TEST(LpmFindTest, MultipleTokensSameLongPrefix) {
    LongestPrefixMatcher lpm;
    Token id1 = insert_str(lpm, "ABCDEFGHX");
    Token id2 = insert_str(lpm, "ABCDEFGHYZ");

    auto [tok1, len1] = find_str(lpm, "ABCDEFGHX__");
    EXPECT_EQ(tok1, id1);
    EXPECT_EQ(len1, size_t(9));

    auto [tok2, len2] = find_str(lpm, "ABCDEFGHYZ_");
    EXPECT_EQ(tok2, id2);
    EXPECT_EQ(len2, size_t(10));
}

// Long pattern that spans exactly MAX_TOKEN_SIZE bytes.
TEST(LpmFindTest, MaxTokenSizePatternFound) {
    LongestPrefixMatcher lpm;
    const char pat[] = "0123456789abcdef";  // exactly 16 bytes
    ASSERT_EQ(strlen(pat), MAX_TOKEN_SIZE);
    Token id = insert_str(lpm, std::string_view(pat, MAX_TOKEN_SIZE));
    auto [tok, len] = find_str(lpm, std::string_view(pat, MAX_TOKEN_SIZE));
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(MAX_TOKEN_SIZE));
}

// Binary: 10-byte all-zeros sequence (8-byte prefix is all zeros).
TEST(LpmFindTest, BinaryAllZerosLongSequence) {
    LongestPrefixMatcher lpm;
    const uint8_t data[10] = {};
    Token id = lpm.insert(data, 10);
    auto [tok, len] = lpm.find_longest_match(data, 10);
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(10));
}

// Binary: 10-byte all-0xFF sequence.
TEST(LpmFindTest, BinaryAllFFLongSequence) {
    LongestPrefixMatcher lpm;
    const uint8_t data[10] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    Token id = lpm.insert(data, 10);
    auto [tok, len] = lpm.find_longest_match(data, 10);
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(10));
}

// ── LinearBucket → TrieBucket promotion ──────────────────────────────────────
// PROMOTE_THRESHOLD is 128 (internal constant).  Inserting > 128 long tokens
// that share the same 8-byte prefix forces the bucket to be promoted from a
// LinearBucket to a TrieBucket.

TEST(LpmTriePromotionTest, AllTokensFindableAfterPromotion) {
    LongestPrefixMatcher lpm;
    // 130 tokens share prefix "XXXXXXXX" (8 bytes), differing only in the
    // 9th byte.  130 > PROMOTE_THRESHOLD (128) forces promotion.
    const std::string prefix(8, 'X');
    std::vector<Token> inserted;

    for (int i = 0; i < 130; ++i) {
        uint8_t buf[9];
        std::copy(prefix.begin(), prefix.end(), buf);
        buf[8] = static_cast<uint8_t>(i);
        inserted.push_back(lpm.insert(buf, 9));
    }

    for (int i = 0; i < 130; ++i) {
        uint8_t buf[10];
        std::copy(prefix.begin(), prefix.end(), buf);
        buf[8] = static_cast<uint8_t>(i);
        buf[9] = 0xFF;  // trailing byte not part of any stored token
        auto [tok, len] = lpm.find_longest_match(buf, 10);
        EXPECT_EQ(tok, inserted[i]) << "token index " << i;
        EXPECT_EQ(len, size_t(9))   << "token index " << i;
    }
}

TEST(LpmTriePromotionTest, SizeCorrectAfterPromotion) {
    LongestPrefixMatcher lpm;
    const std::string prefix(8, 'Y');

    for (int i = 0; i < 130; ++i) {
        uint8_t buf[9];
        std::copy(prefix.begin(), prefix.end(), buf);
        buf[8] = static_cast<uint8_t>(i);
        lpm.insert(buf, 9);
    }
    EXPECT_EQ(lpm.size(), size_t(256 + 130));
}

// Two-level suffix: tokens with the same 8-byte prefix and same first suffix
// byte but different second suffix bytes (exercises trie depth).
TEST(LpmTriePromotionTest, DeepTrieMultiLevelSuffix) {
    LongestPrefixMatcher lpm;
    const std::string prefix(8, 'Z');
    std::vector<Token> inserted;

    // Insert enough tokens to trigger promotion: same first suffix byte 0x00,
    // varying second suffix byte (10-byte tokens total).
    for (int i = 0; i < 130; ++i) {
        uint8_t buf[10];
        std::copy(prefix.begin(), prefix.end(), buf);
        buf[8] = 0x00;
        buf[9] = static_cast<uint8_t>(i);
        inserted.push_back(lpm.insert(buf, 10));
    }

    for (int i = 0; i < 130; ++i) {
        uint8_t buf[11];
        std::copy(prefix.begin(), prefix.end(), buf);
        buf[8] = 0x00;
        buf[9] = static_cast<uint8_t>(i);
        buf[10] = 0xFF;
        auto [tok, len] = lpm.find_longest_match(buf, 11);
        EXPECT_EQ(tok, inserted[i]) << "token index " << i;
        EXPECT_EQ(len, size_t(10))  << "token index " << i;
    }
}

// ── from_dictionary ───────────────────────────────────────────────────────────

TEST(LpmFromDictTest, SizeMatchesDictionaryBaseOnly) {
    const Dictionary dict = make_test_dictionary();
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    EXPECT_EQ(lpm.size(), size_t(256));
}

TEST(LpmFromDictTest, SizeMatchesDictionaryWithExtraTokens) {
    const Dictionary dict = make_test_dictionary({"ab", "abcde"});
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    EXPECT_EQ(lpm.size(), size_t(258));
}

TEST(LpmFromDictTest, AllSingleBytesFoundFromDictionary) {
    const Dictionary dict = make_test_dictionary();
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    for (int i = 0; i < 256; ++i) {
        const uint8_t b = static_cast<uint8_t>(i);
        auto [tok, len] = lpm.find_longest_match(&b, 1);
        EXPECT_EQ(tok, Token(i))  << "byte " << i;
        EXPECT_EQ(len, size_t(1)) << "byte " << i;
    }
}

TEST(LpmFromDictTest, SingleByteLookupUsesPositionalId) {
    const Dictionary dict = make_test_dictionary();
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    const uint8_t b = 0x41;
    auto [tok, len] = lpm.find_longest_match(&b, 1);
    EXPECT_EQ(tok, Token(0x41));
    EXPECT_EQ(len, size_t(1));
}

TEST(LpmFromDictTest, MultiByteTokenFoundWithCorrectId) {
    const Dictionary dict = make_test_dictionary({"ab", "abcde"});
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    // "abcde" is token 257; it wins over "ab" (token 256).
    auto [tok, len] = find_str(lpm, "abcde");
    EXPECT_EQ(tok, Token(257));
    EXPECT_EQ(len, size_t(5));
}

TEST(LpmFromDictTest, ShorterMultiByteTokenFallback) {
    const Dictionary dict = make_test_dictionary({"ab", "abcde"});
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    // "abc" has no exact match → falls back to "ab" (token 256).
    auto [tok, len] = find_str(lpm, "abc");
    EXPECT_EQ(tok, Token(256));
    EXPECT_EQ(len, size_t(2));
}

TEST(LpmFromDictTest, LongTokenFromDictionary) {
    const Dictionary dict = make_test_dictionary({"ABCDEFGHI"});  // 9 bytes
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    auto [tok, len] = find_str(lpm, "ABCDEFGHIX");
    EXPECT_EQ(tok, Token(256));
    EXPECT_EQ(len, size_t(9));
}

TEST(LpmFromDictTest, MaxSizeTokenFromDictionary) {
    const char pat[] = "0123456789abcdef";  // exactly 16 bytes
    ASSERT_EQ(strlen(pat), MAX_TOKEN_SIZE);
    const Dictionary dict =
        make_test_dictionary({std::string_view(pat, MAX_TOKEN_SIZE)});
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    auto [tok, len] = find_str(lpm, std::string_view(pat, MAX_TOKEN_SIZE));
    EXPECT_EQ(tok, Token(256));
    EXPECT_EQ(len, size_t(MAX_TOKEN_SIZE));
}

// Inserting after from_dictionary() must continue from num_tokens(), not 256.
TEST(LpmFromDictTest, InsertAfterFromDictionaryContinuesId) {
    const Dictionary dict = make_test_dictionary({"ab", "cd"});
    // dict has 258 tokens (256 base + 2 multi-byte), so the next ID is 258.
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    Token new_id = insert_str(lpm, "ef");
    EXPECT_EQ(new_id, Token(258));
    EXPECT_EQ(lpm.size(), size_t(259));
}

// Token inserted after from_dictionary() must be findable.
TEST(LpmFromDictTest, InsertedTokenAfterFromDictionaryIsSearchable) {
    const Dictionary dict = make_test_dictionary({"ab"});
    auto lpm = LongestPrefixMatcher::from_dictionary(DictionaryView(dict));
    Token id = insert_str(lpm, "xyz");
    auto [tok, len] = find_str(lpm, "xyzW");
    EXPECT_EQ(tok, id);
    EXPECT_EQ(len, size_t(3));
}
