/**
 * @brief Longest Prefix Matcher for OnPair
 * 
 * Provides efficient longest prefix matching using a hybrid approach:
 * - Short matches (≤8 bytes): Direct hash table lookup
 * - Long matches (>8 bytes): Bucketed by 8-byte prefix with suffix verification
 */

#ifndef LONGEST_PREFIX_MATCHER_H
#define LONGEST_PREFIX_MATCHER_H

#include <cstdint>
#include <vector>
#include <utility>
#include <optional>
#include <algorithm>
#include <bit>
#include <functional>
#include <robin_hood.h>
#include "pair_hash.h"

/**
 * @brief Longest prefix matcher supporting arbitrary-length patterns
 * 
 * Combines direct hash lookup for short patterns with bucketed search for long patterns.
 * Optimized for OnPair's token discovery phase where most patterns are short but
 * long patterns provide significant compression benefits.
 * 
 * @tparam V Token ID type (typically uint16_t for OnPair)
 */
template<typename V>
class LongestPrefixMatcher {
private:
    // Bit masks for extracting prefixes of different lengths (little-endian)
    static constexpr uint64_t MASKS[] = {
        0x0000000000000000ULL, // 0 bytes
        0x00000000000000FFULL, // 1 byte
        0x000000000000FFFFULL, // 2 bytes
        0x0000000000FFFFFFULL, // 3 bytes
        0x00000000FFFFFFFFULL, // 4 bytes
        0x000000FFFFFFFFFFULL, // 5 bytes
        0x0000FFFFFFFFFFFFULL, // 6 bytes
        0x00FFFFFFFFFFFFFFULL, // 7 bytes
        0xFFFFFFFFFFFFFFFFULL  // 8 bytes
    };

    // Threshold for switching from direct lookup to bucketed approach
    static constexpr size_t MIN_MATCH = 8;

    robin_hood::unordered_map<std::pair<uint64_t, uint8_t>, V, PairHash> short_match_lookup;
    robin_hood::unordered_map<uint64_t, std::vector<V>> long_match_buckets;
    std::vector<uint8_t> dictionary;
    std::vector<uint32_t> end_positions;

    // Converts byte sequence to little-endian u64 with length masking
    static inline uint64_t bytes_to_u64_le(const uint8_t* bytes, size_t len) {
        uint64_t value = *reinterpret_cast<const uint64_t*>(bytes);
        return value & MASKS[len];
    }

public:
    // Creates a new empty longest prefix matcher
    LongestPrefixMatcher() {
        dictionary.reserve(1024 * 1024);
        end_positions.push_back(0);
    };

    /**
     * @brief Inserts a new pattern with associated token ID
     * 
     * Automatically chooses storage strategy based on pattern length:
     * - Short patterns (≤8 bytes): Direct hash table insertion
     * - Long patterns (>8 bytes): Bucketed by 8-byte prefix with suffix storage
     * 
     * Long pattern buckets are kept sorted by pattern length (descending) for
     * efficient longest-match-first lookup during matching.
     * 
     * @param data Pointer to pattern data
     * @param length Length of the pattern in bytes
     * @param id Token ID to associate with this pattern
     */
    inline void insert(const uint8_t* data, size_t length, V id) {
        if (length > MIN_MATCH) {
            uint64_t prefix = bytes_to_u64_le(data, MIN_MATCH);
            dictionary.insert(dictionary.end(), data + MIN_MATCH, data + length);
            end_positions.push_back(static_cast<uint32_t>(dictionary.size()));

            auto& bucket = long_match_buckets[prefix];
            bucket.push_back(id);

            std::sort(bucket.begin(), bucket.end(), [&](V id1, V id2) {
                size_t len1 = end_positions[id1 + 1] - end_positions[id1];
                size_t len2 = end_positions[id2 + 1] - end_positions[id2];
                return len2 < len1;  // descending by length
            });
        } else {
            uint64_t prefix = bytes_to_u64_le(data, length);
            short_match_lookup.emplace(std::make_pair(prefix, static_cast<uint8_t>(length)), id);
            end_positions.push_back(static_cast<uint32_t>(dictionary.size()));  // same as previous
        }
    }

    /**
     * @brief Finds the longest matching pattern for the given input data
     * 
     * Returns the token ID and match length for the longest pattern that matches
     * the beginning of the input data. Uses two-phase search:
     * 
     * 1. **Long pattern search**: Check bucketed patterns (>8 bytes) first for longest matches
     * 2. **Short pattern search**: Check direct lookup patterns (≤8 bytes) in decreasing length order
     * 
     * @param data Pointer to input data to match against
     * @param length Length of input data in bytes
     * @return Optional pair of (token_id, match_length) if match found, nullopt otherwise
     */
    std::optional<std::pair<V, size_t>> find_longest_match(const uint8_t* data, size_t length) const {
        // Phase 1: Long pattern search (>8 bytes) - check longest matches first
        if (length > MIN_MATCH) {
            uint64_t prefix = bytes_to_u64_le(data, MIN_MATCH);
            auto it = long_match_buckets.find(prefix);
            if (it != long_match_buckets.end()) {
                for (auto id : it->second) {
                    uint32_t start = end_positions[id];
                    uint32_t end = end_positions[id + 1];
                    size_t suffix_len = end - start;

                    if (length >= MIN_MATCH + suffix_len &&
                        std::memcmp(data + MIN_MATCH, &dictionary[start], suffix_len) == 0) {
                        return std::make_pair(id, MIN_MATCH + suffix_len);
                    }
                }
            }
        }

        // Phase 2: Short pattern search (≤8 bytes) - longest to shortest
        for (size_t len = std::min(length, size_t{MIN_MATCH}); len > 0; --len) {
            uint64_t prefix = bytes_to_u64_le(data, len);
            auto it = short_match_lookup.find(std::make_pair(prefix, static_cast<uint8_t>(len)));
            if (it != short_match_lookup.end()) {
                return std::make_pair(it->second, len);
            }
        }

        return std::nullopt;
    }
};

#endif // LONGEST_PREFIX_MATCHER_H