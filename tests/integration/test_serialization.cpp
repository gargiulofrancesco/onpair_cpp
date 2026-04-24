#include <onpair/api.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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
