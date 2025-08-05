#ifndef ONPAIR_H
#define ONPAIR_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>
#include <random>
#include <algorithm>
#include <limits>
#include <cmath>
#include <cstring>
#include <robin_hood.h>
#include "lpm.h"

/**
 * @brief OnPair string compression algorithm
 * 
 * OnPair is a compression algorithm specifically designed for collections of short strings.
 */
class OnPair {
private:
    static constexpr size_t FAST_COPY_SIZE = 16;

    // Compressed data storage
    std::vector<uint16_t> compressed_data;      // Sequence of token IDs
    std::vector<size_t> string_boundaries;      // End positions for each string

    // Dictionary storage
    std::vector<uint8_t> dictionary;           // Raw token data
    std::vector<uint32_t> token_boundaries;   // Token end positions in dictionary

public:
    /**
     * @brief Default constructor with no pre-allocation
     * 
     * Creates an OnPair compressor with empty vectors. Memory will be allocated
     * dynamically as needed during compression.
     */
    OnPair() = default;

    /**
     * @brief Construct a new OnPair compressor
     * 
     * @param num_strings Expected number of strings (for capacity optimization)
     * @param total_bytes Expected total size of all strings in bytes
     */
    OnPair(size_t num_strings, size_t total_bytes);
    
    /**
     * @brief Destroy the OnPair compressor
     */
    ~OnPair() = default;

    /**
     * @brief Move constructor
     */
    OnPair(OnPair&& other) noexcept = default;

    /**
     * @brief Move assignment operator
     */
    OnPair& operator=(OnPair&& other) noexcept = default;

    // Disable copy constructor and copy assignment
    OnPair(const OnPair&) = delete;
    OnPair& operator=(const OnPair&) = delete;

    /**
     * @brief Compress a collection of strings
     *
     * @param strings Vector of strings to compress
     */
    void compress_strings(const std::vector<std::string>& strings);

    /**
     * @brief Compress raw string data
     * 
     * @param data Pointer to concatenated string data
     * @param end_positions Vector of end positions for each string
     */
    void compress_bytes(const uint8_t* data, const std::vector<size_t>& end_positions);

    /**
     * @brief Decompress a specific string by index
     * 
     * @param index Index of the string to decompress
     * @param buffer Buffer to store the decompressed string
     * @return Size of the decompressed data in bytes
     */
    size_t decompress_string(size_t index, uint8_t* buffer) const;

    /**
     * @brief Decompress all strings
     * 
     * @param buffer Buffer to store all decompressed strings concatenated
     * @return Total size of decompressed data in bytes
     */
    size_t decompress_all(uint8_t* buffer) const;

    /**
     * @brief Get the total space used by the compressed data
     * 
     * @return Total memory usage in bytes
     */
    size_t space_used() const;

    /**
     * @brief Shrinks all internal buffers to fit their current contents
     * 
     * Reduces the capacity of all internal vectors to match their current size,
     * potentially freeing unused memory. This is useful after compression
     * is complete to minimize memory usage.
     */
    void shrink_to_fit();

private:
    /**
     * @brief Flattens a collection of strings into a single byte array with boundary positions
     * 
     * Converts a vector of strings into the internal representation used by OnPair:
     * a contiguous byte array with end positions marking string boundaries.
     * 
     * @param strings Vector of strings to flatten
     * @return Pair of (flattened_data, end_positions) where end_positions is a 
     *         prefix sum array starting with 0
     */
    static std::pair<std::vector<uint8_t>, std::vector<size_t>> flatten_strings(const std::vector<std::string>& strings);

    /**
     * @brief Build the token dictionary using OnPair's pair discovery algorithm
     * 
     * Uses longest prefix matching to parse training data and identify frequent
     * adjacent token pairs.
     * 
     * Algorithm:
     * 1. Initialize 256 single-byte tokens  
     * 2. Parse shuffled training data with longest prefix matching
     * 3. Track adjacent token pair frequencies
     * 4. Merge frequent pairs into new tokens until dictionary full (65,536 tokens)
     * 
     * @param data Pointer to concatenated string data
     * @param end_positions Vector of end positions for each string
     * @return Configured LongestPrefixMatcher containing the discovered tokens
     */
    LongestPrefixMatcher<uint16_t> train_dictionary(const uint8_t* data, const std::vector<size_t>& end_positions);

    /**
     * @brief Compress strings using the learned dictionary
     * 
     * Compresses each string independently by greedily applying longest prefix matching
     * with the constructed dictionary. Each string becomes a sequence of token IDs.
     * 
     * @param data Pointer to concatenated string data
     * @param end_positions Vector of end positions for each string
     * @param lpm Trained LongestPrefixMatcher for pattern matching
     */
    void parse_data(const uint8_t* data, const std::vector<size_t>& end_positions, const LongestPrefixMatcher<uint16_t>& lpm);
};

#endif // ONPAIR_H
