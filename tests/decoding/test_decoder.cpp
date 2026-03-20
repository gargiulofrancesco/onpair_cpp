#include <onpair/encoding/training/trainer.h>
#include <onpair/decoding/decoder.h>
#include <onpair/column/column.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"
#include <cstring>
#include <vector>
#include <string>

using namespace onpair;
using namespace onpair::encoding;
using namespace onpair::decoding;
using namespace test_helpers;

// ── Helper ────────────────────────────────────────────────────────────────────

static OnPairColumn compress_default(const std::vector<std::string>& strings)
{
    encoding::TrainingConfig cfg;
    cfg.seed = 42;
    return OnPairColumn::compress(strings, cfg);
}

// ── Random-access decompress ──────────────────────────────────────────────────
// These tests exercise the full pipeline: training → encoding → decoding.

// Primary contract: every compressed string decompresses back to the original.
TEST(DecoderTest, AllUserStringsRoundTrip) {
    auto strings = make_user_strings(50);
    auto col = compress_default(strings);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// Full 0x00–0xFF byte range: verifies the decoder handles every byte value,
// including NUL and high bytes that can trip sign-extension or off-by-one bugs.
TEST(DecoderTest, BinaryStringsRoundTrip) {
    auto strings = make_binary_strings(40, 32, 7);
    auto col = compress_default(strings);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// Empty string embedded in a non-empty corpus: verifies the zero-length fast
// path does not corrupt the token stream or offsets of adjacent strings.
TEST(DecoderTest, EmptyStringAmongOthers) {
    std::vector<std::string> strings = {"hello", "", "world", "", "!"};
    auto col = compress_default(strings);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// Column where every string is empty: the encoded store has no payload bits
// and every decompress call must return length 0.
TEST(DecoderTest, AllEmptyStrings) {
    auto strings = make_empty_strings(8);
    auto col = compress_default(strings);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// Mix of empty, 1-byte, MAX_TOKEN_SIZE-byte, and long strings: exercises all
// length extremes through the same trained dictionary and encoded store.
TEST(DecoderTest, MixedLengthStringsRoundTrip) {
    auto strings = make_mixed_length_strings(40, 128, 17);
    auto col = compress_default(strings);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// ── decompress_all (bulk) ─────────────────────────────────────────────────────

// Verify decompress_all output against the original strings directly.
TEST(DecoderTest, BulkMatchesOriginalStrings) {
    auto strings = make_user_strings(50);
    auto col = compress_default(strings);
    const OnPairColumnView cv = col.view();

    std::string expected;
    for (const auto& s : strings) expected += s;

    std::vector<uint8_t> bulk(expected.size() + DECOMPRESS_BUFFER_PADDING, 0);
    const size_t written = decoding::decompress_all(
        cv.store(), cv.dictionary(), bulk.data());

    ASSERT_EQ(written, expected.size());
    EXPECT_EQ(std::memcmp(bulk.data(), expected.data(), written), 0);
}

// Empty strings in the corpus must not shift the byte offset of subsequent
// strings in the concatenated bulk output.
TEST(DecoderTest, BulkWithEmbeddedEmptyStrings) {
    std::vector<std::string> strings = {"abc", "", "def", "", "", "ghi"};
    auto col = compress_default(strings);
    const OnPairColumnView cv = col.view();

    // Expected flat output: "abcdefghi" (empty strings contribute 0 bytes).
    std::string expected;
    for (const auto& s : strings) expected += s;

    std::vector<uint8_t> bulk(expected.size() + DECOMPRESS_BUFFER_PADDING, 0);
    const size_t written = decoding::decompress_all(
        cv.store(), cv.dictionary(), bulk.data());

    ASSERT_EQ(written, expected.size());
    EXPECT_EQ(std::memcmp(bulk.data(), expected.data(), written), 0);
}

// ── All bit widths ────────────────────────────────────────────────────────────

class DecoderBitWidthTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, DecoderBitWidthTest,
    testing::Values(12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

// Full pipeline at each bit width with a corpus large enough to produce
// merges, so the encoded store contains non-trivial multi-byte token IDs.
TEST_P(DecoderBitWidthTest, UserStringsRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    encoding::TrainingConfig cfg;
    cfg.bits = bw;
    cfg.seed = 42;
    auto strings = make_user_strings(50);
    auto col = OnPairColumn::compress(strings, cfg);

    ASSERT_EQ(col.num_strings(), strings.size());
    ASSERT_EQ(col.bits(), bw);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

// Binary corpus at each bit width: every byte value must survive compression
// and decompression regardless of which bit-packing width is active.
TEST_P(DecoderBitWidthTest, BinaryStringsRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    encoding::TrainingConfig cfg;
    cfg.bits = bw;
    cfg.seed = 42;
    auto strings = make_binary_strings(30, 32, 99);
    auto col = OnPairColumn::compress(strings, cfg);

    ASSERT_EQ(col.bits(), bw);
    EXPECT_ROUNDTRIP_OK(strings, col);
}
