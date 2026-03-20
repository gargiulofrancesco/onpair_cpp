#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Test corpus generators and raw-string helpers shared across all test files.
// ─────────────────────────────────────────────────────────────────────────────

#include <onpair/core/types.h>
#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace test_helpers {

// ── Raw string representation (Arrow layout) ──────────────────────────────────

struct RawStrings {
    std::vector<uint8_t>  data;
    std::vector<uint32_t> offsets;
    size_t                n = 0;
};

inline RawStrings make_raw(const std::vector<std::string>& strings)
{
    RawStrings r;
    r.offsets.push_back(0);
    for (const auto& s : strings) {
        r.data.insert(r.data.end(),
                      reinterpret_cast<const uint8_t*>(s.data()),
                      reinterpret_cast<const uint8_t*>(s.data()) + s.size());
        r.offsets.push_back(static_cast<uint32_t>(r.data.size()));
    }
    r.n = strings.size();
    return r;
}

// ── String generators ─────────────────────────────────────────────────────────

// "user_000000", "user_000001", …
inline std::vector<std::string> make_user_strings(int n)
{
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "user_%06d", i);
        v.emplace_back(buf);
    }
    return v;
}

// Random ASCII (printable range 0x20–0x7E) strings of varying length.
inline std::vector<std::string> make_random_strings(int n, int max_len, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>     len_dist(1, max_len);
    std::uniform_int_distribution<uint8_t> byte_dist(0x20, 0x7E);
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        std::string s(len_dist(rng), '\0');
        for (char& c : s) c = static_cast<char>(byte_dist(rng));
        v.push_back(std::move(s));
    }
    return v;
}

// Random binary strings (full 0x00–0xFF byte range).
inline std::vector<std::string> make_binary_strings(int n, int max_len, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>     len_dist(1, max_len);
    std::uniform_int_distribution<uint16_t> byte_dist(0, 255);
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        std::string s(len_dist(rng), '\0');
        for (char& c : s) c = static_cast<char>(byte_dist(rng));
        v.push_back(std::move(s));
    }
    return v;
}

// One string per byte value 0x00–0xFF (single-char strings, including NUL).
inline std::vector<std::string> make_single_byte_strings()
{
    std::vector<std::string> v;
    v.reserve(256);
    for (int i = 0; i < 256; ++i)
        v.emplace_back(1, static_cast<char>(i));
    return v;
}

// n strings all of exactly `len` bytes (filled with 'x').
inline std::vector<std::string> make_fixed_length_strings(int n, int len)
{
    return std::vector<std::string>(n, std::string(len, 'x'));
}

// n copies of the same byte repeated `len` times.
// E.g. make_homogeneous_strings(100, 20, 'a') → 100× "aaaaaaaaaaaaaaaaaaaa"
// Exercises maximal convergence pressure on a single pair.
inline std::vector<std::string> make_homogeneous_strings(int n, int len, char byte_val)
{
    return std::vector<std::string>(n, std::string(len, byte_val));
}

// n copies of a string that alternates two bytes: "ababab…" × n.
// Exercises period-2 pair merging.
inline std::vector<std::string> make_alternating_strings(int n, int len,
                                                          char a = 'a', char b = 'b')
{
    std::string s;
    s.reserve(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) s += (i % 2 == 0) ? a : b;
    return std::vector<std::string>(n, s);
}

// n empty strings.
// The compressor must handle zero-length strings gracefully.
inline std::vector<std::string> make_empty_strings(int n)
{
    return std::vector<std::string>(n, std::string{});
}

// n strings with a mix of lengths: ~25% empty, ~25% 1–2 bytes, ~25% exactly
// onpair::MAX_TOKEN_SIZE bytes, ~25% up to max_long bytes.  Exercises all
// length extremes in a single corpus.
inline std::vector<std::string> make_mixed_length_strings(int n, int max_long,
                                                           uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>     kind(0, 3);
    std::uniform_int_distribution<int>     long_len(
        static_cast<int>(onpair::MAX_TOKEN_SIZE) + 1, max_long);
    std::uniform_int_distribution<uint8_t> byte_dist(0x20, 0x7E);

    std::vector<std::string> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        int len = 0;
        switch (kind(rng)) {
            case 0: len = 0; break;
            case 1: len = 1 + (i % 2); break;
            case 2: len = static_cast<int>(onpair::MAX_TOKEN_SIZE); break;
            case 3: len = long_len(rng); break;
        }
        std::string s(static_cast<size_t>(len), '\0');
        for (char& c : s) c = static_cast<char>(byte_dist(rng));
        v.push_back(std::move(s));
    }
    return v;
}

// ── Base dictionary helper ─────────────────────────────────────────────────────
// Build a DictionaryStorage where token i == single byte i (for i = 0..255).
// This is the minimal valid dictionary for testing decode paths.

#include <onpair/core/dictionary.h>

inline onpair::Dictionary make_base_dict()
{
    onpair::Dictionary d;
    // The decoder over-copies MAX_TOKEN_SIZE bytes per token from dict_bytes.
    // The last token here is byte 255 (length 1), so we need MAX_TOKEN_SIZE-1
    // bytes of trailing zeros to keep the over-copy in bounds.
    // See Dictionary::pad_for_decoder() for the full rationale.
    d.bytes.resize(256 + (onpair::MAX_TOKEN_SIZE - 1), 0);
    d.offsets.resize(257);
    for (int i = 0; i < 256; ++i) {
        d.bytes[i]      = static_cast<uint8_t>(i);
        d.offsets[i]    = static_cast<uint32_t>(i);
    }
    d.offsets[256] = 256;
    return d;
}

} // namespace test_helpers
