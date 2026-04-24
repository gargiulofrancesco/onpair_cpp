#include <onpair/api.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include "assertions.h"
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace op = onpair;
using namespace test_helpers;

// ── Helper ────────────────────────────────────────────────────────────────────

static op::encoding::TrainingConfig make_cfg(
    op::BitWidth bits,
    op::encoding::ThresholdSpec threshold)
{
    op::encoding::TrainingConfig cfg;
    cfg.bits      = bits;
    cfg.threshold = threshold;
    cfg.seed      = 42;
    return cfg;
}

static void verify_roundtrip(const std::vector<std::string>& strings,
                              const op::encoding::TrainingConfig& cfg)
{
    auto col = op::OnPairColumn::compress(strings, cfg);
    EXPECT_ROUNDTRIP_OK(strings, col);
}

static void verify_serialization_roundtrip(const std::vector<std::string>& strings,
                                            const op::encoding::TrainingConfig& cfg)
{
    auto col = op::OnPairColumn::compress(strings, cfg);
    EXPECT_ROUNDTRIP_OK(strings, col);
    std::ostringstream oss;
    col.write_to(oss);
    std::istringstream iss(oss.str());
    auto col2 = op::OnPairColumn::read_from(iss);
    EXPECT_ROUNDTRIP_OK(strings, col2);
    EXPECT_EQ(col.num_strings(), col2.num_strings());
    EXPECT_EQ(col.bits(), col2.bits());
}

// ── Parameterised over bit widths ─────────────────────────────────────────────

struct RoundTripParams {
    op::BitWidth bits;
    op::encoding::ThresholdSpec threshold;
    std::string label;
};

void PrintTo(const RoundTripParams& p, std::ostream* os) { *os << p.label; }

class RoundTripTest : public testing::TestWithParam<RoundTripParams> {};

static std::vector<RoundTripParams> make_params() {
    std::vector<RoundTripParams> out;
    for (int b : {9, 10, 11, 12, 13, 14, 15, 16}) {
        auto bw = static_cast<op::BitWidth>(b);
        out.push_back({bw, op::encoding::DynamicThreshold{0.15},
                       "bits" + std::to_string(b) + "_dynamic"});
        out.push_back({bw, op::encoding::FixedThreshold{2},
                       "bits" + std::to_string(b) + "_fixed"});
    }
    return out;
}

INSTANTIATE_TEST_SUITE_P(AllConfigs, RoundTripTest,
    testing::ValuesIn(make_params()),
    [](const auto& info) { return info.param.label; });

TEST_P(RoundTripTest, UserStrings) {
    auto cfg = make_cfg(GetParam().bits, GetParam().threshold);
    verify_roundtrip(make_user_strings(20), cfg);
}

TEST_P(RoundTripTest, RandomStrings) {
    auto cfg = make_cfg(GetParam().bits, GetParam().threshold);
    verify_roundtrip(make_random_strings(30, 64, 99), cfg);
}

TEST_P(RoundTripTest, BinaryStrings) {
    auto cfg = make_cfg(GetParam().bits, GetParam().threshold);
    verify_roundtrip(make_binary_strings(20, 32, 99), cfg);
}

TEST_P(RoundTripTest, SingleByteStrings) {
    auto cfg = make_cfg(GetParam().bits, GetParam().threshold);
    verify_roundtrip(make_single_byte_strings(), cfg);
}

TEST_P(RoundTripTest, FixedLengthStrings) {
    auto cfg = make_cfg(GetParam().bits, GetParam().threshold);
    verify_roundtrip(make_fixed_length_strings(10, 50), cfg);
}

TEST_P(RoundTripTest, SerializationRoundTrip) {
    auto cfg = make_cfg(GetParam().bits, GetParam().threshold);
    verify_serialization_roundtrip(make_user_strings(20), cfg);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST(RoundTripEdge, EmptyColumn) {
    auto cfg = make_cfg(14, op::encoding::DynamicThreshold{0.15});
    auto col = op::OnPairColumn::compress(std::vector<std::string>{}, cfg);
    EXPECT_EQ(col.num_strings(), 0u);
}

TEST(RoundTripEdge, SingleString) {
    auto cfg = make_cfg(14, op::encoding::DynamicThreshold{0.15});
    std::vector<std::string> data = {"hello"};
    verify_roundtrip(data, cfg);
}

TEST(RoundTripEdge, EmptyStrings) {
    auto cfg = make_cfg(14, op::encoding::DynamicThreshold{0.15});
    verify_roundtrip(make_empty_strings(5), cfg);
}

TEST(RoundTripEdge, MixedLengthStrings) {
    auto cfg = make_cfg(14, op::encoding::DynamicThreshold{0.5});
    verify_roundtrip(make_mixed_length_strings(30, 128, 42), cfg);
}

TEST(RoundTripEdge, HomogeneousStrings) {
    auto cfg = make_cfg(14, op::encoding::FixedThreshold{2});
    verify_roundtrip(make_homogeneous_strings(10, 100, 'z'), cfg);
}

TEST(RoundTripEdge, AlternatingStrings) {
    auto cfg = make_cfg(14, op::encoding::FixedThreshold{2});
    verify_roundtrip(make_alternating_strings(10, 50), cfg);
}

TEST(RoundTripEdge, ArrowStyleRoundTrip) {
    auto cfg = make_cfg(14, op::encoding::DynamicThreshold{0.15});
    std::vector<std::string> data = {"hello", "world", "foo"};
    auto raw = make_raw(data);
    auto col = op::OnPairColumn::compress(
        reinterpret_cast<const char*>(raw.data.data()), raw.offsets.data(),
                                          raw.n, cfg);
    EXPECT_ROUNDTRIP_OK(data, col);
}

TEST(RoundTripEdge, LargeCorpusRoundTrip) {
    auto cfg = make_cfg(16, op::encoding::DynamicThreshold{1.0});
    verify_roundtrip(make_user_strings(500), cfg);
}

TEST(RoundTripEdge, SerializeEmptyColumn) {
    auto cfg = make_cfg(14, op::encoding::DynamicThreshold{0.15});
    auto col = op::OnPairColumn::compress(std::vector<std::string>{}, cfg);
    std::ostringstream oss;
    col.write_to(oss);
    std::istringstream iss(oss.str());
    auto col2 = op::OnPairColumn::read_from(iss);
    EXPECT_EQ(col2.num_strings(), 0u);
}