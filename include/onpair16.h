#ifndef ONPAIR16_H
#define ONPAIR16_H

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
#include "lpm16.h"

/**
 * @brief OnPair16 string compression algorithm
 * 
 * OnPair16 is an optimized variant of OnPair with a maximum phrase length of 16 bytes.
 * This constraint allows for better performance while still providing good compression
 * for most string collections.
 */
class OnPair16 {
private:
    // Maximum token length constraint for optimization
    static constexpr size_t MAX_LENGTH = 16;

    // Compressed data storage
    std::vector<uint16_t> compressed_data;      // Sequence of token IDs
    std::vector<size_t> string_boundaries;      // End positions for each string

    // Dictionary storage
    std::vector<uint8_t> dictionary_data;       // Raw token data
    std::vector<uint32_t> dictionary_offsets;   // Token end positions in dictionary

public:
    /**
     * @brief Default constructor with no pre-allocation
     * 
     * Creates an OnPair16 compressor with empty vectors. Memory will be allocated
     * dynamically as needed during compression.
     */
    OnPair16() = default;

    /**
     * @brief Construct a new OnPair16 compressor
     * 
     * @param num_strings Expected number of strings (for capacity optimization)
     * @param total_bytes Expected total size of all strings in bytes
     */
    OnPair16(size_t num_strings, size_t total_bytes);
    
    /**
     * @brief Destroy the OnPair16 compressor
     */
    ~OnPair16() = default;

    /**
     * @brief Move constructor
     */
    OnPair16(OnPair16&& other) noexcept = default;

    /**
     * @brief Move assignment operator
     */
    OnPair16& operator=(OnPair16&& other) noexcept = default;

    // Disable copy constructor and copy assignment
    OnPair16(const OnPair16&) = delete;
    OnPair16& operator=(const OnPair16&) = delete;

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
     * @param buffer Buffer to store the decompressed string (must be large enough)
     * @return Size of the decompressed string in bytes
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
     * @brief Get the algorithm name
     * 
     * @return "OnPair16"
     */
    const char* name() const;

private:
    /**
     * @brief Flattens a collection of strings into a single byte array with boundary positions
     * 
     * Converts a vector of strings into the internal representation used by OnPair16:
     * a contiguous byte array with end positions marking string boundaries.
     * 
     * @param strings Vector of strings to flatten
     * @return Pair of (flattened_data, end_positions) where end_positions is a 
     *         prefix sum array starting with 0
     */
    static std::pair<std::vector<uint8_t>, std::vector<size_t>> flatten_strings(const std::vector<std::string>& strings);

    /**
     * @brief Build the token dictionary using OnPair16's constrained pair discovery algorithm
     * 
     * Uses longest prefix matching with 16-byte maximum token length to parse training data
     * and identify frequent adjacent token pairs.
     * 
     * Algorithm:
     * 1. Initialize 256 single-byte tokens  
     * 2. Parse shuffled training data with longest prefix matching
     * 3. Track adjacent token pair frequencies
     * 4. Merge frequent pairs into new tokens (≤16 bytes) until dictionary full
     * 
     * @param data Pointer to concatenated string data
     * @param end_positions Vector of end positions for each string
     * @return Configured LongestPrefixMatcher16 containing the discovered tokens (≤16 bytes each)
     */
    LongestPrefixMatcher16 train_dictionary(const uint8_t* data, const std::vector<size_t>& end_positions);
    
    /**
     * @brief Compress strings using the learned dictionary
     * 
     * Compresses each string independently by greedily applying longest prefix matching
     * with the constructed dictionary. Each string becomes a sequence of token IDs.
     * 
     * @param data Pointer to concatenated string data
     * @param end_positions Vector of end positions for each string
     * @param lpm Trained LongestPrefixMatcher16 for pattern matching
     */
    void parse_data(const uint8_t* data, const std::vector<size_t>& end_positions, const LongestPrefixMatcher16& lpm);
};

#endif // ONPAIR16_H
