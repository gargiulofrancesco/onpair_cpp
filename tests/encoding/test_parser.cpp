#include <onpair/encoding/training/trainer.h>
#include <onpair/encoding/parsing/parser.h>
#include <onpair/encoding/parsing/bit_writer.h>
#include <onpair/decoding/token_cursor.h>
#include <onpair/core/dictionary_view.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include <cstring>
#include <vector>

using namespace onpair;
using namespace onpair::encoding;
using namespace onpair::decoding;
using namespace test_helpers;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Decode all tokens for string `idx` from `store` and return as a string.
static std::string decode_tokens(const Store& store, DictionaryView dict, size_t idx)
{
    const uint32_t begin = store.boundaries[idx];
    const uint32_t end   = store.boundaries[idx + 1];
    std::string result;
    dispatch_bits(store.bit_width, [&](auto bw) {
        TokenCursor<bw.value> cursor(store.packed.data(), StreamSpan{begin, end});
        while (cursor.has_more()) {
            Token t = cursor.next();
            result.append(reinterpret_cast<const char*>(dict.data(t)), dict.token_size(t));
        }
    });
    return result;
}

// Expected number of uint64_t words needed to pack `n` tokens at `bits` bits each.
static size_t expected_packed_words(size_t n, BitWidth bits)
{
    return (n * bits + 63) / 64;
}

// Train a dictionary on `strings`, then parse the same strings and decode every
// one.  Returns true iff every decoded string matches the original.
static bool roundtrip_all(const std::vector<std::string>& strings,
                           BitWidth bits,
                           uint64_t seed = 42)
{
    if (strings.empty()) return true;
    auto raw = make_raw(strings);
    TrainingConfig cfg;
    cfg.bits      = bits;
    cfg.threshold = FixedThreshold{2};
    cfg.seed      = seed;
    auto [dict, lpm] = train(raw.data.data(), raw.offsets.data(), raw.n, cfg);
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, bits, store);
    DictionaryView dv(dict);
    for (size_t i = 0; i < strings.size(); ++i) {
        if (decode_tokens(store, dv, i) != strings[i]) return false;
    }
    return true;
}

// ── Degenerate inputs ─────────────────────────────────────────────────────────

TEST(ParserTest, ZeroStringsProducesOneBoundary) {
    LongestPrefixMatcher lpm;
    Store store;
    parse(nullptr, nullptr, 0, lpm, 16, store);

    ASSERT_EQ(store.boundaries.size(), 1u);
    EXPECT_EQ(store.boundaries[0], 0u);
    EXPECT_TRUE(store.packed.empty());
    EXPECT_EQ(store.bit_width, 16);
}

TEST(ParserTest, SingleEmptyStringProducesTwoBoundariesAndNoTokens) {
    LongestPrefixMatcher lpm;
    std::vector<uint8_t>  data    = {};
    std::vector<uint32_t> offsets = {0, 0};
    Store store;
    parse(data.data(), offsets.data(), 1, lpm, 16, store);

    ASSERT_EQ(store.boundaries.size(), 2u);
    EXPECT_EQ(store.boundaries[0], 0u);
    EXPECT_EQ(store.boundaries[1], 0u);
    EXPECT_EQ(store.num_tokens(), 0u);
    EXPECT_TRUE(store.packed.empty());
}

TEST(ParserTest, ManyEmptyStringsAllBoundariesAreZero) {
    LongestPrefixMatcher lpm;
    auto raw = make_raw(make_empty_strings(50));
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, 16, store);

    ASSERT_EQ(store.boundaries.size(), 51u);
    for (auto b : store.boundaries)
        EXPECT_EQ(b, 0u);
    EXPECT_EQ(store.num_tokens(), 0u);
    EXPECT_TRUE(store.packed.empty());
}

// ── Structural invariants (parameterized over all valid bit widths) ───────────

class ParserStructuralTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, ParserStructuralTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

TEST_P(ParserStructuralTest, BoundaryCountIsNPlusOne) {
    const BitWidth bits = static_cast<BitWidth>(GetParam());
    LongestPrefixMatcher lpm;
    auto raw = make_raw(make_user_strings(20));
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, bits, store);

    EXPECT_EQ(store.boundaries.size(), raw.n + 1);
    EXPECT_EQ(store.bit_width, bits);
}

TEST_P(ParserStructuralTest, BoundariesAreMonotonic) {
    const BitWidth bits = static_cast<BitWidth>(GetParam());
    LongestPrefixMatcher lpm;
    auto raw = make_raw(make_random_strings(25, 40, 7));
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, bits, store);

    for (size_t i = 1; i < store.boundaries.size(); ++i)
        EXPECT_GE(store.boundaries[i], store.boundaries[i - 1])
            << "non-monotonic at index " << i;
}

TEST_P(ParserStructuralTest, LastBoundaryEqualsTotalTokenCount) {
    const BitWidth bits = static_cast<BitWidth>(GetParam());
    LongestPrefixMatcher lpm;
    auto raw = make_raw(make_random_strings(15, 30, 99));
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, bits, store);

    EXPECT_EQ(store.boundaries.back(), store.num_tokens());
}

TEST_P(ParserStructuralTest, PackedSizeIsConsistentWithTokenCount) {
    const BitWidth bits = static_cast<BitWidth>(GetParam());
    LongestPrefixMatcher lpm;
    auto raw = make_raw(make_user_strings(20));
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, bits, store);

    EXPECT_EQ(store.packed.size(), expected_packed_words(store.num_tokens(), bits));
}

// ── Round-trip correctness: base tokens (untrained LPM) ───────────────────────

TEST(ParserTest, BaseTokens_SingleKnownString) {
    LongestPrefixMatcher lpm;
    auto d = make_base_dict();
    const std::string expected = "Hello, World!";
    auto raw = make_raw({expected});
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, 16, store);

    EXPECT_EQ(decode_tokens(store, DictionaryView(d), 0), expected);
}

TEST(ParserTest, BaseTokens_AllSingleByteValues) {
    // All 256 single-byte tokens (including NUL) must survive a round trip.
    LongestPrefixMatcher lpm;
    auto d       = make_base_dict();
    auto strings = make_single_byte_strings();
    auto raw     = make_raw(strings);
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, 16, store);

    for (size_t i = 0; i < strings.size(); ++i)
        EXPECT_EQ(decode_tokens(store, DictionaryView(d), i), strings[i])
            << "mismatch for byte value " << i;
}

TEST(ParserTest, BaseTokens_MultipleStrings) {
    LongestPrefixMatcher lpm;
    auto d       = make_base_dict();
    auto strings = make_random_strings(30, 20, 2024);
    auto raw     = make_raw(strings);
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, 16, store);

    for (size_t i = 0; i < strings.size(); ++i)
        EXPECT_EQ(decode_tokens(store, DictionaryView(d), i), strings[i])
            << "decode mismatch at string " << i;
}

// ── Trained LPM: multi-byte tokens are actually used ─────────────────────────

TEST(ParserTest, TrainedLPM_ProducesMultiByteTokens) {
    // After training on a repetitive corpus, the parser must emit fewer tokens
    // than input bytes for a non-trivial string, proving multi-byte tokens are used.
    auto strings = make_homogeneous_strings(50, 40, 'a');
    auto raw = make_raw(strings);
    TrainingConfig cfg;
    cfg.bits      = 16;
    cfg.threshold = FixedThreshold{2};
    cfg.seed      = 42;
    auto [dict, lpm] = train(raw.data.data(), raw.offsets.data(), raw.n, cfg);
    Store store;
    parse(raw.data.data(), raw.offsets.data(), raw.n, lpm, 16, store);

    // String 0 is 40 bytes; with base tokens only that would be 40 tokens.
    const uint32_t tokens_0 = store.boundaries[1] - store.boundaries[0];
    EXPECT_LT(tokens_0, 40u) << "parser did not use any multi-byte tokens";
}

// ── Round-trip correctness: trained LPM, all bit widths ──────────────────────

class ParserRoundTripTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, ParserRoundTripTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

TEST_P(ParserRoundTripTest, UserStrings) {
    EXPECT_TRUE(roundtrip_all(make_user_strings(50), static_cast<BitWidth>(GetParam())));
}

TEST_P(ParserRoundTripTest, RandomAsciiStrings) {
    EXPECT_TRUE(roundtrip_all(make_random_strings(60, 50, 1337), static_cast<BitWidth>(GetParam())));
}

TEST_P(ParserRoundTripTest, BinaryStringsWithNulBytes) {
    // Full 0x00–0xFF byte range, including embedded NUL bytes.
    EXPECT_TRUE(roundtrip_all(make_binary_strings(40, 30, 777), static_cast<BitWidth>(GetParam())));
}

TEST_P(ParserRoundTripTest, HomogeneousStrings) {
    // Maximal convergence pressure on a single repeated pair.
    EXPECT_TRUE(roundtrip_all(make_homogeneous_strings(30, 40, 'a'), static_cast<BitWidth>(GetParam())));
}

TEST_P(ParserRoundTripTest, AlternatingStrings) {
    // Period-2 pair-merging pattern.
    EXPECT_TRUE(roundtrip_all(make_alternating_strings(30, 40), static_cast<BitWidth>(GetParam())));
}

TEST_P(ParserRoundTripTest, MixedLengthStrings) {
    // Empty, 1–2 byte, MAX_TOKEN_SIZE, and longer strings in the same corpus.
    EXPECT_TRUE(roundtrip_all(make_mixed_length_strings(80, 100, 31415), static_cast<BitWidth>(GetParam())));
}
