#ifndef ONPAIRMINI_H
#define ONPAIRMINI_H

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
#include "bit_packing.h"
#include <cassert>

/**
 * @brief OnPairMini string compression algorithm
 * 
 * OnPairMini is a variant of the OnPair compression algorithm optimized for
 * smaller dictionaries and bit-packed token storage.
 */
class OnPairMini {
private:
    static constexpr size_t MAX_LENGTH = 16;

    // Frequency threshold for merging token pairs
    size_t threshold;

    // Bit-packing configuration
    uint8_t bits_per_token;

    // Compressed data storage (Bit-packed)
    std::vector<uint64_t> packed_data;   
    std::vector<size_t> string_boundaries;      // End positions for each string (in terms of token index, not byte index)

    // Dictionary storage
    std::vector<uint8_t> dictionary;           // Raw token data
    std::vector<uint32_t> token_boundaries;    // Token end positions in dictionary

    // Helper template for optimized decompression
    template <int Bits>
    size_t decompress_string_impl(size_t index, uint8_t* buffer) const;

    template <int Bits>
    size_t decompress_all_impl(uint8_t* buffer) const;

public:
    /**
     * @brief Default constructor with no pre-allocation
     *
     * @param threshold Frequency threshold for merging token pairs 
     * @param bits Bits per token (default 16, typically 12-16)
     */
    OnPairMini(size_t threshold, uint8_t bits = 16);

    /**
     * @brief Construct a new OnPairMini compressor
     * * @param num_strings Expected number of strings (for capacity optimization)
     * @param total_bytes Expected total size of all strings in bytes
     * @param threshold Frequency threshold for merging token pairs
     * @param bits Bits per token (default 16)
     */
    OnPairMini(size_t num_strings, size_t total_bytes, size_t threshold, uint8_t bits = 16);
    
    ~OnPairMini() = default;
    OnPairMini(OnPairMini&& other) noexcept = default;
    OnPairMini& operator=(OnPairMini&& other) noexcept = default;
    OnPairMini(const OnPairMini&) = delete;
    OnPairMini& operator=(const OnPairMini&) = delete;

    /**
     * @brief Compress a collection of strings
     */
    void compress_strings(const std::vector<std::string>& strings);

    /**
     * @brief Compress raw string data
     */
    void compress_bytes(const uint8_t* data, const std::vector<size_t>& end_positions);

    /**
     * @brief Decompress a specific string by index
     * @warning Buffer must have MAX_LENGTH padding.
     */
    size_t decompress_string(size_t index, uint8_t* buffer) const;

    /**
     * @brief Decompress all strings
     * @warning Buffer must have MAX_LENGTH padding.
     */
    size_t decompress_all(uint8_t* buffer) const;

    /**
     * @brief Get the total space used by the compressed data
     */
    size_t space_used() const;

    /**
     * @brief Shrinks all internal buffers to fit their current contents
     */
    void shrink_to_fit();

private:
    static std::pair<std::vector<uint8_t>, std::vector<size_t>> flatten_strings(const std::vector<std::string>& strings);

    /**
     * @brief Build the token dictionary using OnPairMini's pair discovery algorithm
     * Respects the maximum dictionary size imposed by bits_per_token.
     */
    LongestPrefixMatcher16 train_dictionary(const uint8_t* data, const std::vector<size_t>& end_positions);

    /**
     * @brief Compress strings using the learned dictionary and bit packer
     */
    void parse_data(const uint8_t* data, const std::vector<size_t>& end_positions, const LongestPrefixMatcher16& lpm);
};

#endif // ONPAIRMINI_H