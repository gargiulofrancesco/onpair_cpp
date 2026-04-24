#include <onpair/core/dictionary.h>
#include <onpair/core/store.h>
#include <onpair/encoding/parsing/bit_writer.h>
#include <onpair/decoding/detail/decode_all.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include <cstring>
#include <vector>

using namespace onpair;
using namespace onpair::encoding;
using namespace onpair::decoding;
using namespace test_helpers;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a Store containing the given tokens at the given bit width.
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

// Run decode_all for the given bit width using runtime dispatch.
static std::string decode_all_dispatch(const Store& store,
                                       const Dictionary& dict,
                                       uint32_t total_tokens)
{
    // Buffer: worst case is total_tokens × MAX_TOKEN_SIZE + MAX_TOKEN_SIZE padding.
    const size_t buf_size = size_t(total_tokens) * MAX_TOKEN_SIZE + MAX_TOKEN_SIZE;
    std::vector<uint8_t> buf(buf_size, 0xCC);  // sentinel fill

    const size_t written = dispatch_bits(store.bit_width, [&](auto bw) {
        return decode_all<bw.value>(store.packed.data(),  
                                    dict.bytes.data(), dict.offsets.data(),
                                    total_tokens,
                                    buf.data());
    });
    return std::string(reinterpret_cast<const char*>(buf.data()), written);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

class DecodeAllTest : public testing::TestWithParam<int> {};
INSTANTIATE_TEST_SUITE_P(AllBitWidths, DecodeAllTest,
    testing::Values(9, 10, 11, 12, 13, 14, 15, 16),
    [](const auto& info) { return "bits" + std::to_string(info.param); });

// Zero tokens → returns 0 and writes nothing.
TEST_P(DecodeAllTest, ZeroTokensReturnsZero) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict = make_base_dict();
    auto store = make_packed(bw, {});

    std::vector<uint8_t> buf(MAX_TOKEN_SIZE, 0xCC);
    const size_t written = dispatch_bits(bw, [&](auto bits) {
        return decode_all<bits.value>(store.packed.data(),  
                                    dict.bytes.data(), dict.offsets.data(),
                                    0,
                                    buf.data());
    });
    EXPECT_EQ(written, 0u);
}

// Single base token: verify the decoded byte.
TEST_P(DecodeAllTest, SingleBaseToken) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict  = make_base_dict();
    auto store = make_packed(bw, {Token('H')});  // base token for 'H' = 72

    const std::string result = decode_all_dispatch(store, dict, 1);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 'H');
}

// Decode "hello" via base tokens.
TEST_P(DecodeAllTest, HelloRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict = make_base_dict();
    const std::string expected = "hello";
    std::vector<Token> tokens;
    for (char c : expected) tokens.push_back(Token(static_cast<uint8_t>(c)));
    auto store = make_packed(bw, tokens);

    const std::string result = decode_all_dispatch(store, dict, static_cast<uint32_t>(tokens.size()));
    EXPECT_EQ(result, expected);
}

// Group-boundary test: decode group_size-1, group_size, group_size+1 tokens.
TEST_P(DecodeAllTest, GroupBoundaryTokenCounts) {
    const int bits = GetParam();
    const BitWidth bw = static_cast<BitWidth>(bits);

    int lcm = bits;
    while (lcm % 64 != 0) lcm += bits;
    const int gs = lcm / bits;

    auto dict = make_base_dict();

    for (int count : {gs - 1, gs, gs + 1}) {
        if (count <= 0) continue;
        // Use tokens 0x41='A', 0x42='B', … cycling.
        std::vector<Token> tokens;
        std::string expected;
        for (int i = 0; i < count; ++i) {
            const uint8_t b = static_cast<uint8_t>(0x41 + (i % 26));
            tokens.push_back(Token(b));
            expected.push_back(static_cast<char>(b));
        }
        auto store = make_packed(bw, tokens);
        const std::string result = decode_all_dispatch(store, dict, static_cast<uint32_t>(count));
        EXPECT_EQ(result, expected)
            << "bits=" << bits << " count=" << count;
    }
}

// Decode all-zero tokens.
TEST_P(DecodeAllTest, AllZeroTokens) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    auto dict = make_base_dict();
    std::vector<Token> tokens(8, Token(0));
    auto store = make_packed(bw, tokens);
    const std::string result = decode_all_dispatch(store, dict, 8);
    EXPECT_EQ(result, std::string(8, '\x00'));
}

// Decode all-0xFF tokens.
TEST_P(DecodeAllTest, AllMaxByteTokens) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    // 0xFF is base token 255 (valid as long as bw ≥ 8, which is always true).
    auto dict = make_base_dict();
    const int n = 10;
    std::vector<Token> tokens(n, Token(0xFF));
    auto store = make_packed(bw, tokens);
    const std::string result = decode_all_dispatch(store, dict, n);
    EXPECT_EQ(result, std::string(n, '\xFF'));
}

// Multiple consecutive multi-byte tokens: verifies that the output byte pointer
// advances correctly after each variable-length token, not just after base tokens.
// Uses three 3-byte tokens in a row so byte count (9) != token count (3).
TEST_P(DecodeAllTest, ConsecutiveMultiByteTokens) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());

    // Dictionary: base tokens 0–255 (single bytes) + three 3-byte tokens.
    //   token 256 = "foo"
    //   token 257 = "bar"
    //   token 258 = "baz"
    Dictionary dict;
    dict.bytes.reserve(256 + 9 + MAX_TOKEN_SIZE);
    dict.offsets.resize(260);
    for (int i = 0; i < 256; ++i) {
        dict.bytes.push_back(static_cast<uint8_t>(i));
        dict.offsets[i] = static_cast<uint32_t>(i);
    }
    dict.offsets[256] = 256;
    for (char c : std::string("foobarbaz")) dict.bytes.push_back(static_cast<uint8_t>(c));
    dict.offsets[257] = 259;
    dict.offsets[258] = 262;
    dict.offsets[259] = static_cast<uint32_t>(dict.bytes.size());
    dict.pad_for_decoder();

    // Encode three consecutive multi-byte tokens.
    std::vector<Token> tokens = {Token(256), Token(257), Token(258)};
    auto store = make_packed(bw, tokens);

    const std::string result = decode_all_dispatch(store, dict, 3);
    // 3 tokens → 9 bytes; if the pointer arithmetic is wrong, output is corrupted.
    EXPECT_EQ(result, "foobarbaz");
}

// Multi-byte tokens: decode_all correctly handles tokens with length > 1.
TEST_P(DecodeAllTest, MultiByteTokenRoundTrip) {
    const BitWidth bw = static_cast<BitWidth>(GetParam());
    // Build a dictionary with a 3-byte token at id 256.
    Dictionary dict;
    dict.bytes.reserve(256 + 3 + MAX_TOKEN_SIZE);
    dict.offsets.resize(258);
    // Base tokens 0–255: single bytes.
    for (int i = 0; i < 256; ++i) {
        dict.bytes.push_back(static_cast<uint8_t>(i));
        dict.offsets[i] = static_cast<uint32_t>(i);
    }
    dict.offsets[256] = 256;
    // Token 256 = "xyz" (3 bytes).
    dict.bytes.push_back('x');
    dict.bytes.push_back('y');
    dict.bytes.push_back('z');
    dict.offsets[257] = static_cast<uint32_t>(dict.bytes.size());
    dict.pad_for_decoder();

    // Encode: 'A', token256("xyz"), 'B'
    std::vector<Token> tokens = {Token('A'), Token(256), Token('B')};
    auto store = make_packed(bw, tokens);

    const std::string result = decode_all_dispatch(store, dict, 3);
    EXPECT_EQ(result, "AxyzB");
}
