#include <onpair/core/dictionary.h>
#include <onpair/core/store.h>
#include <onpair/encoding/parsing/bit_writer.h>
#include <onpair/decoding/detail/decode_all.h>
#include <onpair/column/column.h>
#include <onpair/decoding/decoder.h>
#include <onpair/encoding/training/trainer.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace onpair;
using namespace onpair::encoding;
using namespace onpair::decoding;
using namespace test_helpers;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Build a Store where each element of `per_string` holds the tokens for that
// string. `store.boundaries` is set to the Arrow-style sentinel convention:
//   boundaries[i] = first token index of string i
//   boundaries[n] = total token count
static Store make_store_with_boundaries(
    BitWidth bits,
    const std::vector<std::vector<Token>>& per_string)
{
    Store store;
    store.bit_width = bits;
    store.boundaries.push_back(0);
    {
        BitWriter writer(store);
        for (const auto& tokens : per_string) {
            for (Token t : tokens) writer.write(t);
            store.boundaries.push_back(
                store.boundaries.back() + static_cast<uint32_t>(tokens.size()));
        }
    }
    return store;
}

// Result of calling the offset-aware decode_all overload.
struct OffsetDecodeResult {
    size_t               written;
    std::vector<uint8_t>  bytes;
    std::vector<uint32_t> offsets;
};

// Dispatch the offset-aware decode_all<Bits> at runtime and collect results.
static OffsetDecodeResult decode_with_offsets(
    const Store&      store,
    const Dictionary& dict)
{
    const uint32_t total_tokens  = static_cast<uint32_t>(store.num_tokens());
    const size_t   total_strings = store.num_strings();

    const size_t buf_size =
        size_t(total_tokens) * MAX_TOKEN_SIZE + MAX_TOKEN_SIZE;
    std::vector<uint8_t>  buf(buf_size, 0xCC);
    std::vector<uint32_t> offsets(total_strings + 1, 0xDEADBEEFu);

    const size_t written = dispatch_bits(store.bit_width, [&](auto bw) {
        return decode_all<bw.value>(
            store.packed.data(),
            store.boundaries.data(),
            dict.bytes.data(), dict.offsets.data(),
            total_tokens, total_strings,
            buf.data(), offsets.data());
    });
    buf.resize(written);
    return {written, std::move(buf), std::move(offsets)};
}

// Convert a list of strings to per-string base-token sequences.
// Each character maps directly to Token(byte_value), which is valid for the
// base dictionary (token i == single byte i, for i = 0..255).
static std::vector<std::vector<Token>> tokenize_base(
    const std::vector<std::string>& strings)
{
    std::vector<std::vector<Token>> result;
    result.reserve(strings.size());
    for (const auto& s : strings) {
        std::vector<Token> tokens;
        tokens.reserve(s.size());
        for (unsigned char c : s) tokens.push_back(Token(c));
        result.push_back(std::move(tokens));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Unit tests — direct calls to decode_all<Bits> with boundaries
// ─────────────────────────────────────────────────────────────────────────────

class DecodeAllOffsetsTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, DecodeAllOffsetsTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

// ── 1. Single-string smoke tests ──────────────────────────────────────────────

// One string, one token: offsets = {0, 1}, one byte written.
TEST_P(DecodeAllOffsetsTest, SingleStringSingleToken) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    auto store = make_store_with_boundaries(bw, {{Token('X')}});
    auto r     = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), 2u);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[1], 1u);
    ASSERT_EQ(r.written, 1u);
    EXPECT_EQ(r.bytes[0], uint8_t('X'));
}

// One string, multiple tokens: offsets bracket the entire output.
TEST_P(DecodeAllOffsetsTest, SingleStringMultipleTokens) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict    = make_base_dict();
    auto strings = make_user_strings(1);
    auto store   = make_store_with_boundaries(bw, tokenize_base(strings));
    auto r       = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), 2u);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[1], static_cast<uint32_t>(strings[0].size()));
    ASSERT_EQ(r.written, strings[0].size());
    EXPECT_EQ(std::memcmp(r.bytes.data(), strings[0].data(), strings[0].size()), 0);
}

// Two strings: offsets correctly partition the concatenated output.
TEST_P(DecodeAllOffsetsTest, TwoStrings) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict    = make_base_dict();
    auto strings = make_user_strings(2);
    auto store   = make_store_with_boundaries(bw, tokenize_base(strings));
    auto r       = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), 3u);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[1], static_cast<uint32_t>(strings[0].size()));
    EXPECT_EQ(r.offsets[2], static_cast<uint32_t>(strings[0].size() + strings[1].size()));
    ASSERT_EQ(r.written, strings[0].size() + strings[1].size());
    EXPECT_EQ(std::memcmp(r.bytes.data(),
                          strings[0].data(), strings[0].size()), 0);
    EXPECT_EQ(std::memcmp(r.bytes.data() + r.offsets[1],
                          strings[1].data(), strings[1].size()), 0);
}

// ── 2. Offset invariants ──────────────────────────────────────────────────────

// out_offsets[0] is always 0 regardless of string content.
TEST_P(DecodeAllOffsetsTest, FirstOffsetIsAlwaysZero) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    auto store = make_store_with_boundaries(bw,
        tokenize_base(make_random_strings(10, 20, 1)));
    auto r     = decode_with_offsets(store, dict);

    EXPECT_EQ(r.offsets[0], 0u);
}

// out_offsets[n] equals the value returned by decode_all.
TEST_P(DecodeAllOffsetsTest, LastOffsetEqualsReturnValue) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    auto store = make_store_with_boundaries(bw,
        tokenize_base(make_random_strings(10, 20, 2)));
    auto r     = decode_with_offsets(store, dict);

    const size_t n = store.num_strings();
    EXPECT_EQ(r.offsets[n], static_cast<uint32_t>(r.written));
}

// Offsets are monotonically non-decreasing (empty strings produce equal offsets).
TEST_P(DecodeAllOffsetsTest, OffsetsMonotoneNonDecreasing) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict = make_base_dict();
    // Mix of non-empty and empty strings.
    auto store = make_store_with_boundaries(bw, {
        tokenize_base({"AB"})[0],
        {},
        tokenize_base({"C"})[0],
        {},
        tokenize_base({"DEF"})[0],
    });
    auto r = decode_with_offsets(store, dict);

    const size_t n = store.num_strings();
    for (size_t i = 0; i < n; ++i) {
        EXPECT_LE(r.offsets[i], r.offsets[i + 1])
            << "offsets[" << i << "]=" << r.offsets[i]
            << " > offsets[" << (i+1) << "]=" << r.offsets[i+1];
    }
}

// offset[i+1] - offset[i] equals the byte length of string i.
// Uses base tokens so byte length == token count.
TEST_P(DecodeAllOffsetsTest, OffsetDifferencesMatchStringLengths) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict    = make_base_dict();
    auto strings = make_random_strings(8, 15, 3);
    auto store   = make_store_with_boundaries(bw, tokenize_base(strings));
    auto r       = decode_with_offsets(store, dict);

    for (size_t i = 0; i < strings.size(); ++i) {
        const uint32_t len = r.offsets[i + 1] - r.offsets[i];
        EXPECT_EQ(len, strings[i].size())
            << "String " << i << ": expected " << strings[i].size()
            << " bytes, got " << len;
    }
}

// ── 3. Content correctness ────────────────────────────────────────────────────

// Bytes at [offset[i], offset[i+1]) exactly match the expected string content.
TEST_P(DecodeAllOffsetsTest, ByteContentMatchesExpected) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict    = make_base_dict();
    auto strings = make_user_strings(10);
    auto store   = make_store_with_boundaries(bw, tokenize_base(strings));
    auto r       = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), strings.size() + 1);
    for (size_t i = 0; i < strings.size(); ++i) {
        const uint32_t off = r.offsets[i];
        const uint32_t len = r.offsets[i + 1] - off;
        ASSERT_EQ(len, strings[i].size()) << "String " << i;
        EXPECT_EQ(std::memcmp(r.bytes.data() + off,
                               strings[i].data(), len), 0)
            << "String " << i << " content mismatch";
    }
}

// ── 4. Empty string handling ──────────────────────────────────────────────────

// All strings empty: all offsets must be 0 and written = 0.
TEST_P(DecodeAllOffsetsTest, AllEmptyStrings) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict = make_base_dict();
    const int n = 5;
    std::vector<std::vector<Token>> per_string(n, std::vector<Token>{});
    auto store = make_store_with_boundaries(bw, per_string);
    auto r     = decode_with_offsets(store, dict);

    EXPECT_EQ(r.written, 0u);
    ASSERT_EQ(r.offsets.size(), size_t(n) + 1);
    for (size_t i = 0; i <= size_t(n); ++i)
        EXPECT_EQ(r.offsets[i], 0u) << "offsets[" << i << "] != 0";
}

// Empty strings between non-empty ones: zero-length spans must not shift
// the byte positions of adjacent strings.
TEST_P(DecodeAllOffsetsTest, EmptyStringsBetweenNonEmpty) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    // {"AB", "", "C", "", "", "DE"}
    auto store = make_store_with_boundaries(bw, {
        {Token('A'), Token('B')},
        {},
        {Token('C')},
        {},
        {},
        {Token('D'), Token('E')},
    });
    auto r = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), 7u);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[1], 2u);  // "AB" = 2 bytes
    EXPECT_EQ(r.offsets[2], 2u);  // ""   = 0 bytes
    EXPECT_EQ(r.offsets[3], 3u);  // "C"  = 1 byte
    EXPECT_EQ(r.offsets[4], 3u);  // ""   = 0 bytes
    EXPECT_EQ(r.offsets[5], 3u);  // ""   = 0 bytes
    EXPECT_EQ(r.offsets[6], 5u);  // "DE" = 2 bytes
    EXPECT_EQ(r.written, 5u);
}

// A single empty string followed by a non-empty one.
TEST_P(DecodeAllOffsetsTest, LeadingEmptyString) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    auto store = make_store_with_boundaries(bw, {
        {},
        {Token('h'), Token('i')},
    });
    auto r = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), 3u);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[1], 0u);
    EXPECT_EQ(r.offsets[2], 2u);
    EXPECT_EQ(r.written, 2u);
}

// A non-empty string followed by a trailing empty one.
TEST_P(DecodeAllOffsetsTest, TrailingEmptyString) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    auto store = make_store_with_boundaries(bw, {
        {Token('o'), Token('k')},
        {},
    });
    auto r = decode_with_offsets(store, dict);

    ASSERT_EQ(r.offsets.size(), 3u);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[1], 2u);
    EXPECT_EQ(r.offsets[2], 2u);
    EXPECT_EQ(r.written, 2u);
}

// ── 5. Consistency with the no-offset overload ───────────────────────────────

// Bytes produced by the offset-aware overload are byte-for-byte identical to
// those produced by the basic decode_all (which shares the same emit path).
TEST_P(DecodeAllOffsetsTest, BytesMatchBasicDecodeAll) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict    = make_base_dict();
    auto strings = make_random_strings(5, 20, 4);
    auto per_string = tokenize_base(strings);

    // Flatten all tokens for the basic (no-offset) overload.
    std::vector<Token> flat;
    for (const auto& ts : per_string)
        flat.insert(flat.end(), ts.begin(), ts.end());

    Store basic_store;
    basic_store.bit_width = bw;
    {
        BitWriter w(basic_store);
        for (Token t : flat) w.write(t);
    }
    const size_t buf_sz = flat.size() * MAX_TOKEN_SIZE + MAX_TOKEN_SIZE;
    std::vector<uint8_t> basic_buf(buf_sz, 0);
    const size_t basic_written = dispatch_bits(bw, [&](auto bits) {
        return decode_all<bits.value>(
            basic_store.packed.data(),
            dict.bytes.data(), dict.offsets.data(),
            static_cast<uint32_t>(flat.size()),
            basic_buf.data());
    });

    auto store = make_store_with_boundaries(bw, per_string);
    auto r     = decode_with_offsets(store, dict);

    ASSERT_EQ(r.written, basic_written);
    EXPECT_EQ(std::memcmp(r.bytes.data(), basic_buf.data(), r.written), 0);
}

// ── 6. Large corpus stress tests ──────────────────────────────────────────────

// 200 random ASCII strings (1–20 bytes). Verifies that all offset invariants
// hold across many super-group boundaries.
TEST_P(DecodeAllOffsetsTest, LargeCorpusOffsetInvariants) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict    = make_base_dict();
    auto strings = make_random_strings(200, 20, 42);
    auto store   = make_store_with_boundaries(bw, tokenize_base(strings));
    auto r       = decode_with_offsets(store, dict);

    const size_t n = strings.size();
    ASSERT_EQ(r.offsets.size(), n + 1);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[n], static_cast<uint32_t>(r.written));

    size_t cum = 0;
    for (size_t i = 0; i < n; ++i) {
        EXPECT_LE(r.offsets[i], r.offsets[i + 1])
            << "Not monotone at i=" << i;
        const uint32_t len = r.offsets[i + 1] - r.offsets[i];
        EXPECT_EQ(len, strings[i].size()) << "Wrong length at i=" << i;
        cum += strings[i].size();
    }
    EXPECT_EQ(r.written, cum);
}

// 150 strings with empty entries at every 7th position. Verifies that
// zero-length spans don't corrupt adjacent offsets across super-group
// boundaries.
TEST_P(DecodeAllOffsetsTest, LargeCorpusWithInterspersedEmpties) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict = make_base_dict();

    // Build corpus: empty every 7th, random non-empty otherwise.
    auto non_empty = make_random_strings(150, 15, 99);
    std::vector<std::string> strings;
    strings.reserve(non_empty.size());
    int src = 0;
    for (int i = 0; i < static_cast<int>(non_empty.size()); ++i) {
        strings.push_back((i % 7 == 0) ? std::string{} : non_empty[src++]);
    }

    auto store = make_store_with_boundaries(bw, tokenize_base(strings));
    auto r     = decode_with_offsets(store, dict);

    const size_t n = strings.size();
    ASSERT_EQ(r.offsets.size(), n + 1);
    EXPECT_EQ(r.offsets[0], 0u);
    EXPECT_EQ(r.offsets[n], static_cast<uint32_t>(r.written));

    for (size_t i = 0; i < n; ++i) {
        EXPECT_LE(r.offsets[i], r.offsets[i + 1])
            << "Not monotone at i=" << i;
        const uint32_t len = r.offsets[i + 1] - r.offsets[i];
        EXPECT_EQ(len, strings[i].size()) << "Wrong length at i=" << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration tests — full pipeline via decompress_all(sv, dv, buf, out_offsets)
//
// Multi-byte token offset accounting (offsets count bytes, not token IDs) is
// exercised here through a real trained dictionary, which naturally contains
// multi-byte tokens. See BinaryStringsOffsetRoundTrip and
// MixedLengthStringsOffsetRoundTrip for the most direct coverage.
// ─────────────────────────────────────────────────────────────────────────────

class DecodeAllOffsetsIntegrationTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, DecodeAllOffsetsIntegrationTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

static OnPairColumn compress_at(const std::vector<std::string>& strings, int bits)
{
    encoding::TrainingConfig cfg;
    cfg.bits = static_cast<BitWidth>(bits);
    cfg.seed = 42;
    return OnPairColumn::compress(strings, cfg);
}

// Helper: decompress the entire column using the offset-aware bulk API and
// verify that every string reconstructed from the offsets matches the original.
static void check_offsets_roundtrip(
    const OnPairColumn&            col,
    const std::vector<std::string>& originals)
{
    const OnPairColumnView cv = col.view();
    const size_t n = originals.size();
    ASSERT_EQ(col.num_strings(), n);

    size_t total_bytes = 0;
    for (const auto& s : originals) total_bytes += s.size();
    std::vector<uint8_t>  buf(total_bytes + DECOMPRESS_BUFFER_PADDING, 0xCC);
    std::vector<uint32_t> offsets(n + 1, 0xDEADBEEFu);

    const size_t written = decoding::decompress_all(
        cv.store(), cv.dictionary(), buf.data(), offsets.data());

    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(offsets[n], static_cast<uint32_t>(written));

    for (size_t i = 0; i < n; ++i) {
        const uint32_t off = offsets[i];
        const uint32_t len = offsets[i + 1] - off;
        ASSERT_EQ(len, originals[i].size()) << "String " << i;
        EXPECT_EQ(std::memcmp(buf.data() + off,
                               originals[i].data(), len), 0)
            << "String " << i << " content mismatch";
    }
}

// "user_XXXXXX" strings: structured, highly compressible corpus.
TEST_P(DecodeAllOffsetsIntegrationTest, UserStringsOffsetRoundTrip) {
    auto strings = make_user_strings(50);
    auto col     = compress_at(strings, GetParam());
    check_offsets_roundtrip(col, strings);
}

// Random binary corpus: every byte value must survive the full pipeline.
TEST_P(DecodeAllOffsetsIntegrationTest, BinaryStringsOffsetRoundTrip) {
    auto strings = make_binary_strings(40, 32, 7);
    auto col     = compress_at(strings, GetParam());
    check_offsets_roundtrip(col, strings);
}

// Mixed-length corpus: empty, 1–2 bytes, MAX_TOKEN_SIZE bytes, long strings.
TEST_P(DecodeAllOffsetsIntegrationTest, MixedLengthStringsOffsetRoundTrip) {
    auto strings = make_mixed_length_strings(40, 128, 17);
    auto col     = compress_at(strings, GetParam());
    check_offsets_roundtrip(col, strings);
}

// Homogeneous corpus: maximal pair pressure on a single repeated byte.
// Validates that highly compressed token streams produce correct per-string
// offsets even when boundaries don't align with token boundaries.
TEST_P(DecodeAllOffsetsIntegrationTest, HomogeneousStringsOffsetRoundTrip) {
    auto strings = make_homogeneous_strings(30, 64, 'a');
    auto col     = compress_at(strings, GetParam());
    check_offsets_roundtrip(col, strings);
}

// Alternating corpus: period-2 pair merging pattern.
TEST_P(DecodeAllOffsetsIntegrationTest, AlternatingStringsOffsetRoundTrip) {
    auto strings = make_alternating_strings(30, 60);
    auto col     = compress_at(strings, GetParam());
    check_offsets_roundtrip(col, strings);
}

// Offsets from decompress_all(…, out_offsets) match individual decompress()
// calls, which exercise different code paths (TokenCursor per string).
TEST_P(DecodeAllOffsetsIntegrationTest, OffsetsMatchIndividualDecompress) {
    auto strings = make_user_strings(30);
    auto col     = compress_at(strings, GetParam());
    const OnPairColumnView cv = col.view();
    const size_t n = strings.size();

    // Bulk with offsets.
    size_t total_bytes = 0;
    for (const auto& s : strings) total_bytes += s.size();
    std::vector<uint8_t>  bulk_buf(total_bytes + DECOMPRESS_BUFFER_PADDING, 0);
    std::vector<uint32_t> offsets(n + 1, 0);
    decoding::decompress_all(cv.store(), cv.dictionary(),
                             bulk_buf.data(), offsets.data());

    // Individual decompress() for each string.
    size_t max_len = 0;
    for (const auto& s : strings) max_len = std::max(max_len, s.size());
    std::vector<uint8_t> single_buf(max_len + DECOMPRESS_BUFFER_PADDING, 0);
    for (size_t i = 0; i < n; ++i) {
        const size_t single_written =
            decoding::decompress(cv.store(), cv.dictionary(), i, single_buf.data());

        const uint32_t bulk_len = offsets[i + 1] - offsets[i];
        ASSERT_EQ(bulk_len, single_written) << "String " << i;
        EXPECT_EQ(std::memcmp(bulk_buf.data() + offsets[i],
                               single_buf.data(), single_written), 0)
            << "String " << i;
    }
}

// Embedded empty strings: offsets for empty entries must equal the offset of
// the next non-empty string.
TEST_P(DecodeAllOffsetsIntegrationTest, EmptyStringsEmbedded) {
    const std::vector<std::string> strings = {"hello", "", "world", "", "!"};
    auto col = compress_at(strings, GetParam());
    check_offsets_roundtrip(col, strings);
}

// All-empty strings: entire output must be 0 bytes and all offsets 0.
TEST_P(DecodeAllOffsetsIntegrationTest, AllEmptyStrings) {
    auto strings = make_empty_strings(8);
    auto col     = compress_at(strings, GetParam());
    const OnPairColumnView cv = col.view();
    const size_t n = strings.size();

    std::vector<uint8_t>  buf(DECOMPRESS_BUFFER_PADDING, 0xCC);
    std::vector<uint32_t> offsets(n + 1, 0xDEADBEEFu);
    const size_t written = decoding::decompress_all(
        cv.store(), cv.dictionary(), buf.data(), offsets.data());

    EXPECT_EQ(written, 0u);
    for (size_t i = 0; i <= n; ++i)
        EXPECT_EQ(offsets[i], 0u) << "offsets[" << i << "] != 0";
}

// Offset-aware and plain decompress_all produce the same flat bytes.
TEST_P(DecodeAllOffsetsIntegrationTest, OffsetAwareBytesMatchPlainBulk) {
    auto strings = make_user_strings(50);
    auto col     = compress_at(strings, GetParam());
    const OnPairColumnView cv = col.view();
    const size_t n = strings.size();

    size_t total_bytes = 0;
    for (const auto& s : strings) total_bytes += s.size();

    std::vector<uint8_t> plain_buf(total_bytes + DECOMPRESS_BUFFER_PADDING, 0);
    const size_t plain_written = decoding::decompress_all(
        cv.store(), cv.dictionary(), plain_buf.data());

    std::vector<uint8_t>  offset_buf(total_bytes + DECOMPRESS_BUFFER_PADDING, 0);
    std::vector<uint32_t> offsets(n + 1, 0);
    const size_t offset_written = decoding::decompress_all(
        cv.store(), cv.dictionary(), offset_buf.data(), offsets.data());

    ASSERT_EQ(plain_written, offset_written);
    EXPECT_EQ(std::memcmp(plain_buf.data(), offset_buf.data(), plain_written), 0);
}
