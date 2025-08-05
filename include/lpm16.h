/**
 * @brief Longest Prefix Matcher for OnPair16
 * 
 * Optimized longest prefix matcher for 16-byte constrained tokens.
 */

#ifndef LONGEST_PREFIX_MATCHER16_H
#define LONGEST_PREFIX_MATCHER16_H

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
 * @brief Optimized longest prefix matcher with 16-byte maximum pattern length
 * 
 * A specialized variant of LongestPrefixMatcher designed for OnPair16's constraint
 * of maximum 16-byte patterns.
 */
class LongestPrefixMatcher16 {
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

    // Maximum entries per bucket
    static constexpr size_t MAX_BUCKET_SIZE = 128;

    robin_hood::unordered_map<std::pair<uint64_t, uint8_t>, uint16_t, PairHash> dictionary;
    robin_hood::unordered_map<uint64_t, std::vector<std::tuple<uint64_t, uint8_t, uint16_t>>> buckets;

    // Converts byte sequence to little-endian u64 with length masking
    static inline uint64_t bytes_to_u64_le(const uint8_t* bytes, size_t len) {
        uint64_t value = *reinterpret_cast<const uint64_t*>(bytes);
        return value & MASKS[len];
    }

    static inline bool is_prefix(uint64_t text, uint64_t prefix, size_t text_size, size_t prefix_size) {
        return prefix_size <= text_size && shared_prefix_size(text, prefix) >= prefix_size;
    }

    static inline size_t shared_prefix_size(uint64_t a, uint64_t b) {
        return std::countr_zero(a ^ b) >> 3;
    }

public:
    /**
     * @brief Default constructor - creates an empty matcher
     */
    LongestPrefixMatcher16() = default;

    /**
     * @brief Inserts a pattern with 16-byte length constraint
     * 
     * Patterns are stored using one of two strategies:
     * - **Short patterns** (≤8 bytes): Direct hash table with (value, length) key
     * - **Long patterns** (9-16 bytes): Bucketed by 8-byte prefix with suffix storage
     * 
     * Long pattern buckets have a size limit (MAX_BUCKET_SIZE). When a bucket is full, 
     * the insertion fails and returns false.
     * 
     * @param data Pointer to pattern data
     * @param length Length of pattern in bytes (must be ≤16)
     * @param id Token ID to associate with this pattern
     * @return true if insertion succeeded, false if bucket was full
     */
    inline bool insert(const uint8_t* data, size_t length, uint16_t id) {
        if (length <= 8) {
            uint64_t value = bytes_to_u64_le(data, length);
            dictionary.emplace(std::make_pair(value, static_cast<uint8_t>(length)), id);
            return true;
        } else {
            uint64_t prefix = bytes_to_u64_le(data, 8);
            auto& bucket = buckets[prefix];

            if (bucket.size() >= MAX_BUCKET_SIZE) {
                return false;
            }

            size_t suffix_len = length - 8;
            uint64_t suffix = bytes_to_u64_le(data + 8, suffix_len);
            
            bucket.emplace_back(suffix, static_cast<uint8_t>(suffix_len), id);
            
            // Sort by suffix length in descending order
            std::sort(bucket.begin(), bucket.end(),
                [](const auto& a, const auto& b) {
                    return std::get<1>(b) < std::get<1>(a);
                });

            return true;
        }
    }

    /**
     * @brief Finds the longest matching pattern with 16-byte constraint
     * 
     * Searches for the longest pattern that matches the beginning of the input data.
     * Uses a two-phase approach optimized for the 16-byte constraint:
     * 
     * 1. **Long pattern search** (9-16 bytes): Check bucketed patterns using prefix
     *    matching followed by suffix verification using bit operations
     * 2. **Short pattern search** (≤8 bytes): Direct hash lookup in decreasing length order
     * 
     * @param data Pointer to input data to match against
     * @param length Length of input data in bytes
     * @return Optional pair of (token_id, match_length) if match found, nullopt otherwise
     */
    inline std::optional<std::pair<uint16_t, size_t>> find_longest_match(const uint8_t* data, size_t length) const {
        // Long match handling
        if (length > 8) {
            size_t suffix_len = std::min(length, size_t{16}) - 8;
            uint64_t prefix = bytes_to_u64_le(data, 8);
            uint64_t suffix = bytes_to_u64_le(data + 8, suffix_len);

            auto bucket_it = buckets.find(prefix);
            if (bucket_it != buckets.end()) {
                const auto& bucket = bucket_it->second;
                for (const auto& entry : bucket) {
                    const auto& [entry_suffix, entry_suffix_len, entry_id] = entry;
                    if (is_prefix(suffix, entry_suffix, suffix_len, entry_suffix_len)) {
                        return std::make_pair(entry_id, 8 + entry_suffix_len);
                    }
                }
            }
        }
        
        // Short match handling
        uint64_t prefix = bytes_to_u64_le(data, 8);
        for (size_t len = std::min(length, size_t{8}); len > 0; --len) {
            prefix &= MASKS[len];
            auto it = dictionary.find(std::make_pair(prefix, static_cast<uint8_t>(len)));
            if (it != dictionary.end()) {
                return std::make_pair(it->second, len);
            }
        }
        
        return std::nullopt;
    }
};

#endif