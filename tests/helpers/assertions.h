#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Custom GTest assertions for OnPair tests.
// ─────────────────────────────────────────────────────────────────────────────

#include <onpair/column/column.h>
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace test_helpers {

// Decompress string `idx` from `col` and compare to `expected`.
// Allocates a buffer large enough for any reasonable string.
inline testing::AssertionResult DecompressesTo(
    const onpair::OnPairColumn& col,
    size_t idx,
    const std::string& expected)
{
    std::vector<char> buf(expected.size() + onpair::DECOMPRESS_BUFFER_PADDING + 1024);
    const size_t len = col.view().decompress(idx, buf.data());
    const std::string got(buf.data(), len);
    if (got == expected)
        return testing::AssertionSuccess();
    return testing::AssertionFailure()
        << "decompress(" << idx << ") returned " << len << " bytes.\n"
        << "  got:      " << testing::PrintToString(got) << "\n"
        << "  expected: " << testing::PrintToString(expected);
}

// Round-trip assertion: compress all strings in `original`, decompress each,
// verify exact match.
inline testing::AssertionResult RoundTripOk(
    const std::vector<std::string>& original,
    const onpair::OnPairColumn&     col)
{
    if (col.num_strings() != original.size())
        return testing::AssertionFailure()
            << "num_strings() = " << col.num_strings()
            << ", expected " << original.size();

    std::vector<char> buf(4096 + onpair::DECOMPRESS_BUFFER_PADDING);
    for (size_t i = 0; i < original.size(); ++i) {
        const std::string& exp = original[i];
        if (buf.size() < exp.size() + onpair::DECOMPRESS_BUFFER_PADDING)
            buf.resize(exp.size() + onpair::DECOMPRESS_BUFFER_PADDING);
        const size_t len = col.view().decompress(i, buf.data());
        if (len != exp.size())
            return testing::AssertionFailure()
                << "At index " << i << ": length mismatch: got " << len
                << ", expected " << exp.size();
        if (std::memcmp(buf.data(), exp.data(), len) != 0)
            return testing::AssertionFailure()
                << "At index " << i << ": content mismatch";
    }
    return testing::AssertionSuccess();
}

} // namespace test_helpers

// Convenience macro wrappers.
#define EXPECT_DECOMPRESSES_TO(col, idx, expected) \
    EXPECT_TRUE(test_helpers::DecompressesTo((col), (idx), (expected)))

#define EXPECT_ROUNDTRIP_OK(original, col) \
    EXPECT_TRUE(test_helpers::RoundTripOk((original), (col)))
