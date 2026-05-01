#include <onpair/api.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>

namespace op = onpair;
using namespace test_helpers;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string serialize(const op::OnPairColumn& col)
{
    std::ostringstream oss;
    col.write_to(oss);
    return oss.str();
}

static op::OnPairColumn deserialize(const std::string& data)
{
    std::istringstream iss(data);
    return op::OnPairColumn::read_from(iss);
}

// ── Full round-trip ───────────────────────────────────────────────────────────

TEST(SerializationTest, WriteReadPreservesAllStrings) {
    auto strings = make_user_strings(50);
    auto col = op::OnPairColumn::compress(strings);

    const std::string blob = serialize(col);
    auto col2 = deserialize(blob);

    EXPECT_ROUNDTRIP_OK(strings, col2);
}

TEST(SerializationTest, PreservesNumStrings) {
    auto strings = make_user_strings(25);
    auto col  = op::OnPairColumn::compress(strings);
    auto col2 = deserialize(serialize(col));
    EXPECT_EQ(col2.num_strings(), col.num_strings());
}

TEST(SerializationTest, PreservesBitWidth) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = 14;
    cfg.seed = 1;
    auto col  = op::OnPairColumn::compress(make_user_strings(20), cfg);
    auto col2 = deserialize(serialize(col));
    EXPECT_EQ(col2.bits(), op::BitWidth(14));
}

TEST(SerializationTest, DeserializedColumnDecompressesCorrectly) {
    auto strings = make_random_strings(30, 40, 55);
    auto col  = op::OnPairColumn::compress(strings);
    auto col2 = deserialize(serialize(col));

    std::vector<char> buf(4096 + op::DECOMPRESS_BUFFER_PADDING);
    for (size_t i = 0; i < strings.size(); ++i) {
        const size_t len = col2.view().decompress(i, buf.data());
        EXPECT_EQ(std::string(buf.data(), len), strings[i]) << "at index " << i;
    }
}

// ── Error cases ───────────────────────────────────────────────────────────────

TEST(SerializationTest, CorruptedMagicThrows) {
    auto col = op::OnPairColumn::compress(make_user_strings(5));
    std::string blob = serialize(col);
    blob[0] ^= 0xFF;  // corrupt first byte of magic
    EXPECT_THROW(deserialize(blob), std::runtime_error);
}

TEST(SerializationTest, TruncatedStreamThrows) {
    auto col = op::OnPairColumn::compress(make_user_strings(5));
    const std::string blob = serialize(col);
    // Keep only the first 20 bytes.
    const std::string truncated = blob.substr(0, 20);
    EXPECT_THROW(deserialize(truncated), std::runtime_error);
}

TEST(SerializationTest, InvalidBitWidthInFileThrows) {
    auto col = op::OnPairColumn::compress(make_user_strings(5));
    std::string blob = serialize(col);
    // The bit_width field is at byte 8 (after 8-byte magic).
    blob[8] = 8;  // 8 is a valid bit width
    EXPECT_THROW(deserialize(blob), std::runtime_error);
}

TEST(SerializationTest, EmptyColumnRoundTrips) {
    auto col  = op::OnPairColumn::compress(std::vector<std::string>{});
    auto col2 = deserialize(serialize(col));
    EXPECT_EQ(col2.num_strings(), 0u);
}

// ── Multiple bit widths ───────────────────────────────────────────────────────

class SerializationBitWidthTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, SerializationBitWidthTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

TEST_P(SerializationBitWidthTest, RoundTripPreservesContent) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 77;
    auto strings = make_user_strings(20);
    auto col  = op::OnPairColumn::compress(strings, cfg);
    auto col2 = deserialize(serialize(col));
    EXPECT_EQ(col2.bits(), cfg.bits);
    EXPECT_ROUNDTRIP_OK(strings, col2);
}

// ── Serialisation stability ───────────────────────────────────────────────────
//
// These tests guard against two related bugs in the sentinel/padding handling:
//
//  (a) Sentinel included in payload: serialize(col) writes one extra uint64_t
//      word, wasting 8 bytes per column and breaking format compatibility.
//      Caught by PackedWordCountExcludesSentinel.
//
//  (b) Sentinel stripped on write but not restored on read: the deserialized
//      packed buffer is missing its over-read guard.  The next serialize()
//      would then strip one real data word.  Caught by BlobStableAcrossRoundTrips.

// Parse the packed word count embedded in a serialized blob without fully
// deserializing it.  Format after the magic+bit_width header:
//   true_bytes(u32) + bytes_data + offsets_count(u32) + offsets_data + packed_count(u32) + ...
static uint32_t packed_word_count_in_blob(const std::string& blob)
{
    std::istringstream iss(blob);
    iss.seekg(9);  // magic(8) + bit_width(1)
    auto read_u32 = [&]() {
        uint32_t v;
        iss.read(reinterpret_cast<char*>(&v), sizeof(v));
        return v;
    };
    const uint32_t true_bytes    = read_u32();
    iss.seekg(true_bytes, std::ios::cur);
    const uint32_t offsets_count = read_u32();
    iss.seekg(static_cast<std::streamoff>(offsets_count) * sizeof(uint32_t),
              std::ios::cur);
    return read_u32();
}

static size_t expected_packed_words(size_t n_tokens, op::BitWidth bits)
{
    return (n_tokens * static_cast<size_t>(bits) + 63) / 64;
}

class SerializationStabilityTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, SerializationStabilityTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

// serialize(deserialize(blob)) must be byte-for-byte identical to blob.
// A "strip without restore" bug causes the blob to shrink by 8 bytes per hop.
// A "no-strip with restore" bug causes it to grow.
TEST_P(SerializationStabilityTest, BlobStableAcrossRoundTrips) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 42;
    auto strings = make_user_strings(30);

    const std::string blob1 = serialize(op::OnPairColumn::compress(strings, cfg));
    const std::string blob2 = serialize(deserialize(blob1));
    const std::string blob3 = serialize(deserialize(blob2));

    EXPECT_EQ(blob1, blob2) << "blob changed on first round-trip";
    EXPECT_EQ(blob2, blob3) << "blob changed on second round-trip";
    EXPECT_ROUNDTRIP_OK(strings, deserialize(blob3));
}

// The packed_count field in the blob must equal ceil(num_tokens * bits / 64).
// A bug that includes the sentinel in the payload would make it ceil(...) + 1.
TEST_P(SerializationStabilityTest, PackedWordCountExcludesSentinel) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 7;
    auto strings = make_user_strings(25);
    auto col = op::OnPairColumn::compress(strings, cfg);

    const size_t n_tokens = col.view().store().num_tokens();
    EXPECT_EQ(packed_word_count_in_blob(serialize(col)),
              expected_packed_words(n_tokens, cfg.bits));
}

// ── Sentinel edge cases ───────────────────────────────────────────────────────

// Zero tokens: packed is completely empty; no sentinel is added.
// packed_count in the blob must be 0 and decompression must still work.
TEST(SerializationSentinelEdgeCase, AllEmptyStrings) {
    auto strings = make_empty_strings(10);
    auto col = op::OnPairColumn::compress(strings);
    const std::string blob1 = serialize(col);
    const std::string blob2 = serialize(deserialize(blob1));

    EXPECT_EQ(packed_word_count_in_blob(blob1), 0u);
    EXPECT_EQ(blob1, blob2);
    EXPECT_ROUNDTRIP_OK(strings, deserialize(blob2));
}

// Single short string: minimal non-trivial packed payload.
TEST_P(SerializationStabilityTest, SingleShortStringStable) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 1;
    std::vector<std::string> strings{"hello"};
    auto col = op::OnPairColumn::compress(strings, cfg);
    const std::string blob1 = serialize(col);
    const std::string blob2 = serialize(deserialize(blob1));

    EXPECT_EQ(blob1, blob2);
    EXPECT_ROUNDTRIP_OK(strings, deserialize(blob2));
}

// Token counts that span across one full word group plus a remainder,
// and counts that land exactly on a group boundary.  At an exact boundary
// flush() writes only the sentinel (no partial data word), which is the
// path where a missing sentinel would go unnoticed for the longest time.
TEST_P(SerializationStabilityTest, VariousTokenCountsAroundGroupBoundary) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 0;

    // Use homogeneous 1-char strings: each string produces exactly one token
    // (the single-byte token from the base dictionary), giving us fine-grained
    // control over the approximate token count.
    for (int n : {1, 3, 4, 15, 16, 17, 31, 32, 33, 63, 64, 65}) {
        auto strings = make_homogeneous_strings(n, 1, 'a');
        auto col = op::OnPairColumn::compress(strings, cfg);
        const std::string blob1 = serialize(col);
        const std::string blob2 = serialize(deserialize(blob1));

        EXPECT_EQ(blob1, blob2) << "n_strings=" << n;

        const size_t n_tokens = col.view().store().num_tokens();
        EXPECT_EQ(packed_word_count_in_blob(blob1),
                  expected_packed_words(n_tokens, cfg.bits)) << "n_strings=" << n;

        EXPECT_ROUNDTRIP_OK(strings, deserialize(blob2)) << "n_strings=" << n;
    }
}

// Binary strings (full 0x00–0xFF byte range) with multiple round-trips.
TEST_P(SerializationStabilityTest, BinaryStringsStable) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 123;
    auto strings = make_binary_strings(20, 30, 99);
    const std::string blob1 = serialize(op::OnPairColumn::compress(strings, cfg));
    const std::string blob2 = serialize(deserialize(blob1));

    EXPECT_EQ(blob1, blob2);
    EXPECT_ROUNDTRIP_OK(strings, deserialize(blob2));
}

// N consecutive round trips must produce byte-identical blobs and a
// stable bytes_used() metric.  Catches slow drift bugs (e.g., +1 byte
// per cycle) that the 2-hop test would miss.
TEST_P(SerializationStabilityTest, ManyCyclesAreStable) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 99;
    auto strings = make_user_strings(40);

    auto col0 = op::OnPairColumn::compress(strings, cfg);
    const std::string blob0  = serialize(col0);
    const size_t      bytes0 = col0.bytes_used();

    std::string blob = blob0;
    for (int i = 0; i < 10; ++i) {
        auto col_i = deserialize(blob);
        EXPECT_EQ(col_i.bytes_used(), bytes0)
            << "bytes_used() drifted at cycle " << i;
        blob = serialize(col_i);
        EXPECT_EQ(blob, blob0) << "blob drifted at cycle " << i;
        EXPECT_ROUNDTRIP_OK(strings, deserialize(blob))
            << "decompression broke at cycle " << i;
    }
}

// bytes_used() must be invariant across a single deserialize hop.
// A bug that lets dictionary padding leak into bytes_used (e.g., using
// bytes.size() instead of offsets.back()) would show up here.
TEST_P(SerializationStabilityTest, BytesUsedInvariantAcrossRoundTrip) {
    op::encoding::TrainingConfig cfg;
    cfg.bits = static_cast<op::BitWidth>(GetParam());
    cfg.seed = 55;
    auto col1 = op::OnPairColumn::compress(make_user_strings(30), cfg);
    auto col2 = deserialize(serialize(col1));
    EXPECT_EQ(col1.bytes_used(), col2.bytes_used());
    EXPECT_EQ(col1.view().store().bytes_used(),
              col2.view().store().bytes_used());
    EXPECT_EQ(col1.view().dictionary().bytes_used(),
              col2.view().dictionary().bytes_used());
}
