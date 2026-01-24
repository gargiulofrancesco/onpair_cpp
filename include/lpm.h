/**
 * @brief Longest Prefix Matcher for OnPair
 * 
 * Provides efficient longest prefix matching using a hybrid approach:
 * - Short matches (≤8 bytes): Direct hash table lookup in decreasing length order
 * - Long matches (>8 bytes): Skip the first 8 bytes via hash table, then trie lookup for suffixes
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
 * Combines direct hash lookup for short patterns with trie lookup for long patterns.
 * Optimized for OnPair's token discovery phase where most patterns are short but
 * long patterns provide significant compression benefits.
 * 
 * @tparam V Token ID type (typically uint16_t for OnPair)
 */
template<typename V>
class LongestPrefixMatcher {
private:
    // Trie node structure for suffix storage of long patterns
    struct TrieNode {
        std::optional<V> id;
        std::vector<std::pair<uint8_t, uint32_t>> children;
    };

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

    // Length of the prefix used for indexing trie roots
    static constexpr size_t TRIE_PREFIX_LEN = 8;

    // Short patterns stored in direct hash table
    robin_hood::unordered_map<std::pair<uint64_t, uint8_t>, V, PairHash> short_match_lookup;
    // Long patterns stored in trie structure
    robin_hood::unordered_map<uint64_t, uint32_t> long_match_roots;
    std::vector<TrieNode> node_pool;

    // Converts byte sequence to little-endian u64 with length masking
    static inline uint64_t bytes_to_u64_le(const uint8_t* bytes, size_t len) {
        uint64_t value = *reinterpret_cast<const uint64_t*>(bytes);
        return value & MASKS[len];
    }

public:
    // Creates a new empty longest prefix matcher
    LongestPrefixMatcher() {
        node_pool.reserve(256 * 1024);
        long_match_roots.reserve(64 * 1024);
    };

    /**
     * @brief Inserts a new pattern with associated token ID
     * 
     * Automatically chooses storage strategy based on pattern length:
     * - Short patterns (≤8 bytes): Direct hash table insertion
     * - Long patterns (>8 bytes): Trie storage for suffixes beyond 8-byte prefix
     * 
     * @param data Pointer to pattern data
     * @param length Length of the pattern in bytes
     * @param id Token ID to associate with this pattern
     */
    inline void insert(const uint8_t* data, size_t length, V id) {
        if (length <= TRIE_PREFIX_LEN) {
            uint64_t prefix = bytes_to_u64_le(data, length);
            short_match_lookup.emplace(std::make_pair(prefix, static_cast<uint8_t>(length)), id);
        } else {
            uint64_t prefix = bytes_to_u64_le(data, TRIE_PREFIX_LEN);

            // Insert suffix into trie
            uint32_t node_idx;
            auto it = long_match_roots.find(prefix);
            if (it == long_match_roots.end()) {
                node_idx = static_cast<uint32_t>(node_pool.size());
                node_pool.emplace_back();
                long_match_roots[prefix] = node_idx;
            } else {
                node_idx = it->second;
            }

            for (size_t i = TRIE_PREFIX_LEN; i < length; ++i) {
                uint8_t byte = data[i];
                auto child_it = std::find_if(node_pool[node_idx].children.begin(), node_pool[node_idx].children.end(), 
                    [byte](const std::pair<uint8_t, uint32_t>& p) { return p.first == byte; });
                
                if (child_it != node_pool[node_idx].children.end()) {
                    node_idx = child_it->second;
                } else {
                    uint32_t new_idx = static_cast<uint32_t>(node_pool.size());
                    node_pool.emplace_back();
                    node_pool[node_idx].children.emplace_back(byte, new_idx);
                    node_idx = new_idx;
                }
            }

            node_pool[node_idx].id = id;
        }
    }

    /**
     * @brief Finds the longest matching pattern for the given input data
     * 
     * Returns the token ID and match length for the longest pattern that matches
     * the beginning of the input data. Uses two-phase search:
     * 
     * 1. **Long pattern search**: Use the first 8 bytes to find trie root, then traverse trie for suffixes beyond 8-byte prefix
     * 2. **Short pattern search**: Check direct lookup patterns (≤8 bytes) in decreasing length order
     * 
     * @param data Pointer to input data to match against
     * @param length Length of input data in bytes
     * @return Optional pair of (token_id, match_length) if match found, nullopt otherwise
     */
    inline std::optional<std::pair<V, size_t>> find_longest_match(const uint8_t* data, size_t length) const {
        // Phase 1: Long pattern search (>8 bytes) - use trie-based lookup for suffixes beyond 8-byte prefix
        if (length > TRIE_PREFIX_LEN) {
            uint64_t prefix = bytes_to_u64_le(data, TRIE_PREFIX_LEN);
            auto it = long_match_roots.find(prefix);

            if (it != long_match_roots.end()) {
                std::optional<std::pair<V, size_t>> best_long_match;
                uint32_t current_idx = it->second;
                size_t current_len = TRIE_PREFIX_LEN;

                while (current_len < length) {
                    uint8_t byte = data[current_len];
                    const auto& node = node_pool[current_idx];
                    
                    auto child_it = std::find_if(node.children.begin(), node.children.end(), 
                        [byte](const std::pair<uint8_t, uint32_t>& p) { return p.first == byte; });

                    if (child_it == node.children.end()) {
                        break;
                    }

                    current_idx = child_it->second;
                    current_len++;

                    if (node_pool[current_idx].id.has_value()) {
                        best_long_match = std::make_pair(*node_pool[current_idx].id, current_len);
                    }
                }

                if (best_long_match.has_value()) {
                    return best_long_match;
                }
            }
        }

        // Phase 2: Short pattern search (≤8 bytes) - longest to shortest
        for (size_t len = std::min(length, size_t{TRIE_PREFIX_LEN}); len > 0; --len) {
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