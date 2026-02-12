#include "onpair_mini.h"

// -----------------------------------------------------------------------------
// CONSTRUCTORS
// -----------------------------------------------------------------------------

OnPairMini::OnPairMini(size_t threshold, uint8_t bits) 
    : threshold(threshold), bits_per_token(bits) 
{
    assert(threshold > 1 && "Threshold must be greater than 1");
    assert(bits >= 12 && bits <= 16 && "Bits per token must be between 12 and 16");
}

OnPairMini::OnPairMini(size_t num_strings, size_t total_bytes, size_t threshold, uint8_t bits)
    : OnPairMini(threshold, bits)
{    
    packed_data.reserve(total_bytes / 8);
    string_boundaries.reserve(num_strings + 1);
    dictionary.reserve(1024 * 1024);
    token_boundaries.reserve(1 << bits);
}

// -----------------------------------------------------------------------------
// COMPRESSION
// -----------------------------------------------------------------------------

void OnPairMini::compress_strings(const std::vector<std::string>& strings) {
    auto [data, end_positions] = flatten_strings(strings);
    compress_bytes(data.data(), end_positions);
}

void OnPairMini::compress_bytes(const uint8_t* data, const std::vector<size_t>& end_positions) {
    LongestPrefixMatcher16 lpm = train_dictionary(data, end_positions);
    parse_data(data, end_positions, lpm);
}

// -----------------------------------------------------------------------------
// DECOMPRESSION (DISPATCHERS)
// -----------------------------------------------------------------------------

size_t OnPairMini::decompress_string(size_t index, uint8_t* buffer) const {
    switch(bits_per_token) {
        case 12: return decompress_string_impl<12>(index, buffer);
        case 13: return decompress_string_impl<13>(index, buffer);
        case 14: return decompress_string_impl<14>(index, buffer);
        case 15: return decompress_string_impl<15>(index, buffer);
        case 16: return decompress_string_impl<16>(index, buffer);
        default: return decompress_string_impl<16>(index, buffer);
    }
}

size_t OnPairMini::decompress_all(uint8_t* buffer) const {
    switch(bits_per_token) {
        case 12: return decompress_all_impl<12>(buffer);
        case 13: return decompress_all_impl<13>(buffer);
        case 14: return decompress_all_impl<14>(buffer);
        case 15: return decompress_all_impl<15>(buffer);
        case 16: return decompress_all_impl<16>(buffer);
        default: return decompress_all_impl<16>(buffer);
    }
}

// -----------------------------------------------------------------------------
// UTILITIES
// -----------------------------------------------------------------------------

size_t OnPairMini::space_used() const {
    return packed_data.size() * sizeof(uint64_t) + 
           dictionary.size() + 
           token_boundaries.size() * sizeof(uint32_t);
}

void OnPairMini::shrink_to_fit() {
    packed_data.shrink_to_fit();
    string_boundaries.shrink_to_fit();
    dictionary.shrink_to_fit();
    token_boundaries.shrink_to_fit();
}

std::pair<std::vector<uint8_t>, std::vector<size_t>> OnPairMini::flatten_strings(const std::vector<std::string>& strings) {
    size_t total_len = 0;
    for (const auto& str : strings) {
        total_len += str.size();
    }
    
    std::vector<uint8_t> data;
    data.reserve(total_len);
    
    std::vector<size_t> end_positions;
    end_positions.reserve(strings.size() + 1);
    end_positions.push_back(0);
    
    for (const auto& str : strings) {
        data.insert(data.end(), str.begin(), str.end());
        end_positions.push_back(data.size());
    }
    
    return std::make_pair(std::move(data), std::move(end_positions));
}

// -----------------------------------------------------------------------------
// TRAINING AND PARSING
// -----------------------------------------------------------------------------

LongestPrefixMatcher16 OnPairMini::train_dictionary(const uint8_t* data, const std::vector<size_t>& end_positions) {
    token_boundaries.push_back(0);
    
    robin_hood::unordered_map<std::pair<uint16_t, uint16_t>, uint16_t, PairHash> frequency;
    LongestPrefixMatcher16 lpm;
    uint16_t next_token_id = 256;
    bool full_dictionary = false;
    
    // Calculate the hard limit for the dictionary size based on bits
    uint16_t max_dictionary_size = (1 << bits_per_token) - 1;

    // Initialize the dictionary with single-byte tokens
    for(uint16_t i=0; i<=255; i++) {
        uint8_t value = static_cast<uint8_t>(i);
        lpm.insert(&value, 1, i);
        dictionary.push_back(value);
        token_boundaries.push_back(dictionary.size());
    }

    // Shuffle entries
    std::vector<int> shuffled_indices(end_positions.size() - 1);
    std::iota(shuffled_indices.begin(), shuffled_indices.end(), 0);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(shuffled_indices.begin(), shuffled_indices.end(), g);

    // Iterate over entries
    for(auto index : shuffled_indices){ 
        size_t start = end_positions[index];
        size_t end = end_positions[index+1];

        if (full_dictionary) {
            break; 
        }

        if (start == end) {
            continue;
        }

        auto match = lpm.find_longest_match(data + start, end - start);
        uint16_t previous_token_id = match.value().first;
        size_t previous_length = match.value().second; 

        size_t pos = start + previous_length;

        while (pos < end) {
            auto match = lpm.find_longest_match(data + pos, end - pos);
            uint16_t match_token_id = match.value().first;
            size_t match_length = match.value().second; 

            auto token_pair = std::make_pair(previous_token_id, match_token_id);
            frequency[token_pair]++;

            size_t pair_length = previous_length + match_length;
            if (frequency[token_pair] >= threshold && pair_length <= 16) {
                lpm.insert(data + pos - previous_length, previous_length + match_length, next_token_id);
                dictionary.insert(dictionary.end(), data + pos - previous_length, data + pos + match_length);
                token_boundaries.push_back(dictionary.size());
                
                frequency.erase(token_pair);
                previous_token_id = next_token_id;
                previous_length += match_length;

                // Stop if we hit the limit imposed by bit width
                if (next_token_id >= max_dictionary_size) {
                    full_dictionary = true;
                    break;
                }
                
                next_token_id++;
            }
            else {
                previous_token_id = match_token_id;
                previous_length = match_length;
            }

            pos += match_length;
        }
    }

    return std::move(lpm);
}

void OnPairMini::parse_data(const uint8_t* data, const std::vector<size_t>& end_positions, const LongestPrefixMatcher16& lpm) {
    string_boundaries.push_back(0);
    
    // Initialize the BitWriter wrapping the packed_data vector
    bit_packing::BitWriter writer(packed_data);

    for(int i=0; i<end_positions.size()-1; i++) {
        size_t start = end_positions[i];
        size_t end = end_positions[i+1];

        if (start == end) {
            // Write the current token count (NOT bit count) to boundaries
            string_boundaries.push_back(writer.total_bits() / bits_per_token);
            continue;
        }

        size_t pos = start;
        while (pos < end) {
            auto match = lpm.find_longest_match(data + pos, end - pos);
            uint16_t token_id = match->first;
            size_t length = match->second;
            
            // Write bits
            writer.write(token_id, bits_per_token);
            
            pos += length;
        }

        string_boundaries.push_back(writer.total_bits() / bits_per_token);
    }
    
    // CRITICAL: Finalize the buffer to add safety padding for the BitReader
    writer.flush_and_pad();
}

// -----------------------------------------------------------------------------
// DECOMPRESSION IMPLEMENTATIONS
// -----------------------------------------------------------------------------

template <int Bits>
size_t OnPairMini::decompress_string_impl(size_t index, uint8_t* buffer) const {
    const uint8_t* dict_ptr = dictionary.data();
    const uint32_t* offsets_ptr = token_boundaries.data();
    
    size_t size = 0;
    size_t start_idx = string_boundaries[index];
    size_t end_idx = string_boundaries[index + 1];

    // --- 16-bit (Treat data as an uint16_t array) ---
    if constexpr (Bits == 16) {
        const uint16_t* raw_tokens = reinterpret_cast<const uint16_t*>(packed_data.data());
        for (size_t i = start_idx; i < end_idx; i++) {
            uint16_t token_id = raw_tokens[i];
            size_t dict_start = offsets_ptr[token_id];
            size_t length = offsets_ptr[token_id + 1] - dict_start;
            std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
            size += length;
        }
        return size;
    }

    // --- Variable Bits ---
    const uint64_t* packed_ptr = packed_data.data();
    constexpr uint64_t mask = (1ULL << Bits) - 1;

    // Calculate bit position once for the start of the string
    size_t bit_offset = start_idx * Bits;
    size_t current_word_idx = bit_offset >> 6; // / 64
    size_t current_shift    = bit_offset & 63; // % 64
    
    // Pre-load the window at the correct spot
    uint64_t window = packed_ptr[current_word_idx];

    // Scan: Iterate tokens sequentially
    for (size_t i = start_idx; i < end_idx; i++) {
        // Extract Token
        uint64_t token_val = (window >> current_shift) & mask;
        
        // Handle Split Tokens
        if (current_shift + Bits > 64) {
             uint64_t next_word = packed_ptr[current_word_idx + 1];
             token_val |= (next_word << (64 - current_shift)) & mask;
        }
        
        uint16_t token_id = static_cast<uint16_t>(token_val);

        // Advance Cursor
        current_shift += Bits;
        
        // Update Window if needed
        if (current_shift >= 64) {
            current_shift -= 64;
            current_word_idx++;
            window = packed_ptr[current_word_idx];
        }

        // Copy Data
        size_t dict_start = offsets_ptr[token_id];
        size_t length = offsets_ptr[token_id + 1] - dict_start;
        std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
        size += length;
    }

    return size;
}

template <int Bits>
size_t OnPairMini::decompress_all_impl(uint8_t* buffer) const {
    const uint8_t* dict_ptr = dictionary.data();
    const uint32_t* offsets_ptr = token_boundaries.data();
    size_t size = 0;
    size_t total_tokens = string_boundaries.back();

    // --- 16-bit (Treat data as an uint16_t array) ---
    if constexpr (Bits == 16) {
        const uint16_t* raw_tokens = reinterpret_cast<const uint16_t*>(packed_data.data());
        for (size_t i = 0; i < total_tokens; i++) {
            uint16_t token_id = raw_tokens[i];
            size_t dict_start = offsets_ptr[token_id];
            size_t length = offsets_ptr[token_id + 1] - dict_start;
            std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
            size += length;
        }
        return size;
    }

    // --- 12-bit (Fully unrolled, 16 tokens per 3 words) ---
    if constexpr (Bits == 12) {
        const uint64_t* packed_ptr = packed_data.data();
        constexpr uint64_t MASK = 0xFFF;

        const size_t full_groups = total_tokens >> 4;   // / 16
        const size_t remainder   = total_tokens & 15;   // % 16

        for (size_t g = 0; g < full_groups; g++) {
            const uint64_t w0 = packed_ptr[0];
            const uint64_t w1 = packed_ptr[1];
            const uint64_t w2 = packed_ptr[2];
            packed_ptr += 3;

            // Fully unrolled extraction — no branches, no loops
            const uint16_t tokens[16] = {
                static_cast<uint16_t>( (w0)                            & MASK),  //  0: bits   0–11
                static_cast<uint16_t>( (w0 >> 12)                      & MASK),  //  1: bits  12–23
                static_cast<uint16_t>( (w0 >> 24)                      & MASK),  //  2: bits  24–35
                static_cast<uint16_t>( (w0 >> 36)                      & MASK),  //  3: bits  36–47
                static_cast<uint16_t>( (w0 >> 48)                      & MASK),  //  4: bits  48–59
                static_cast<uint16_t>(((w0 >> 60) | (w1 << 4))        & MASK),   //  5: bits  60–71  (split w0/w1)
                static_cast<uint16_t>( (w1 >> 8)                       & MASK),  //  6: bits  72–83
                static_cast<uint16_t>( (w1 >> 20)                      & MASK),  //  7: bits  84–95
                static_cast<uint16_t>( (w1 >> 32)                      & MASK),  //  8: bits  96–107
                static_cast<uint16_t>( (w1 >> 44)                      & MASK),  //  9: bits 108–119
                static_cast<uint16_t>(((w1 >> 56) | (w2 << 8))        & MASK),   // 10: bits 120–131 (split w1/w2)
                static_cast<uint16_t>( (w2 >> 4)                       & MASK),  // 11: bits 132–143
                static_cast<uint16_t>( (w2 >> 16)                      & MASK),  // 12: bits 144–155
                static_cast<uint16_t>( (w2 >> 28)                      & MASK),  // 13: bits 156–167
                static_cast<uint16_t>( (w2 >> 40)                      & MASK),  // 14: bits 168–179
                static_cast<uint16_t>( (w2 >> 52)                      & MASK),  // 15: bits 180–191
            };

            // Decode all 16 tokens
            for (int j = 0; j < 16; j++) {
                const size_t dict_start = offsets_ptr[tokens[j]];
                const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                size += length;
            }
        }

        // Tail: decode remaining 0–15 tokens with scalar bit-walking
        if (remainder > 0) {
            size_t current_shift = 0;
            size_t word_offset = 0;
            uint64_t window = packed_ptr[0];

            for (size_t i = 0; i < remainder; i++) {
                uint64_t token_val = (window >> current_shift) & MASK;
                if (current_shift + 12 > 64) {
                    token_val |= (packed_ptr[word_offset + 1] << (64 - current_shift)) & MASK;
                }
                const uint16_t token_id = static_cast<uint16_t>(token_val);

                current_shift += 12;
                if (current_shift >= 64) {
                    current_shift -= 64;
                    word_offset++;
                    window = packed_ptr[word_offset];
                }

                const size_t dict_start = offsets_ptr[token_id];
                const size_t length = offsets_ptr[token_id + 1] - dict_start;
                std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                size += length;
            }
        }

        return size;
    }

    // --- 13-bit (4 sub-groups of 16 tokens, 13 words per super-group) ---
    // Process 16 tokens at a time to match 12-bit's decode batch size.
    // Bit offset cycles {0, 16, 32, 48} across sub-groups before realigning.
    if constexpr (Bits == 13) {
        const uint64_t* packed_ptr = packed_data.data();
        constexpr uint64_t M = 0x1FFF;
        const size_t full_super_groups = total_tokens >> 6;  // / 64
        const size_t remainder = total_tokens & 63;

        for (size_t g = 0; g < full_super_groups; g++) {
            // Sub-group 0: bit offset 0, words [0..3]
            {
                const uint64_t w0 = packed_ptr[0], w1 = packed_ptr[1], w2 = packed_ptr[2], w3 = packed_ptr[3];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>( w0                        & M),
                    static_cast<uint16_t>((w0 >> 13)                 & M),
                    static_cast<uint16_t>((w0 >> 26)                 & M),
                    static_cast<uint16_t>((w0 >> 39)                 & M),
                    static_cast<uint16_t>(((w0 >> 52) | (w1 << 12)) & M),
                    static_cast<uint16_t>((w1 >>  1)                 & M),
                    static_cast<uint16_t>((w1 >> 14)                 & M),
                    static_cast<uint16_t>((w1 >> 27)                 & M),
                    static_cast<uint16_t>((w1 >> 40)                 & M),
                    static_cast<uint16_t>(((w1 >> 53) | (w2 << 11)) & M),
                    static_cast<uint16_t>((w2 >>  2)                 & M),
                    static_cast<uint16_t>((w2 >> 15)                 & M),
                    static_cast<uint16_t>((w2 >> 28)                 & M),
                    static_cast<uint16_t>((w2 >> 41)                 & M),
                    static_cast<uint16_t>(((w2 >> 54) | (w3 << 10)) & M),
                    static_cast<uint16_t>((w3 >>  3)                 & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 1: bit offset 16, words [3..6]
            {
                const uint64_t w3 = packed_ptr[3], w4 = packed_ptr[4], w5 = packed_ptr[5], w6 = packed_ptr[6];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w3 >> 16)                 & M),
                    static_cast<uint16_t>((w3 >> 29)                 & M),
                    static_cast<uint16_t>((w3 >> 42)                 & M),
                    static_cast<uint16_t>(((w3 >> 55) | (w4 <<  9)) & M),
                    static_cast<uint16_t>((w4 >>  4)                 & M),
                    static_cast<uint16_t>((w4 >> 17)                 & M),
                    static_cast<uint16_t>((w4 >> 30)                 & M),
                    static_cast<uint16_t>((w4 >> 43)                 & M),
                    static_cast<uint16_t>(((w4 >> 56) | (w5 <<  8)) & M),
                    static_cast<uint16_t>((w5 >>  5)                 & M),
                    static_cast<uint16_t>((w5 >> 18)                 & M),
                    static_cast<uint16_t>((w5 >> 31)                 & M),
                    static_cast<uint16_t>((w5 >> 44)                 & M),
                    static_cast<uint16_t>(((w5 >> 57) | (w6 <<  7)) & M),
                    static_cast<uint16_t>((w6 >>  6)                 & M),
                    static_cast<uint16_t>((w6 >> 19)                 & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 2: bit offset 32, words [6..9]
            {
                const uint64_t w6 = packed_ptr[6], w7 = packed_ptr[7], w8 = packed_ptr[8], w9 = packed_ptr[9];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w6 >> 32)                 & M),
                    static_cast<uint16_t>((w6 >> 45)                 & M),
                    static_cast<uint16_t>(((w6 >> 58) | (w7 <<  6)) & M),
                    static_cast<uint16_t>((w7 >>  7)                 & M),
                    static_cast<uint16_t>((w7 >> 20)                 & M),
                    static_cast<uint16_t>((w7 >> 33)                 & M),
                    static_cast<uint16_t>((w7 >> 46)                 & M),
                    static_cast<uint16_t>(((w7 >> 59) | (w8 <<  5)) & M),
                    static_cast<uint16_t>((w8 >>  8)                 & M),
                    static_cast<uint16_t>((w8 >> 21)                 & M),
                    static_cast<uint16_t>((w8 >> 34)                 & M),
                    static_cast<uint16_t>((w8 >> 47)                 & M),
                    static_cast<uint16_t>(((w8 >> 60) | (w9 <<  4)) & M),
                    static_cast<uint16_t>((w9 >>  9)                 & M),
                    static_cast<uint16_t>((w9 >> 22)                 & M),
                    static_cast<uint16_t>((w9 >> 35)                 & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 3: bit offset 48, words [9..12]
            {
                const uint64_t w9 = packed_ptr[9], w10 = packed_ptr[10], w11 = packed_ptr[11], w12 = packed_ptr[12];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w9 >> 48)                  & M),
                    static_cast<uint16_t>(((w9 >> 61) | (w10 <<  3)) & M),
                    static_cast<uint16_t>((w10 >> 10)                 & M),
                    static_cast<uint16_t>((w10 >> 23)                 & M),
                    static_cast<uint16_t>((w10 >> 36)                 & M),
                    static_cast<uint16_t>((w10 >> 49)                 & M),
                    static_cast<uint16_t>(((w10 >> 62) | (w11 << 2)) & M),
                    static_cast<uint16_t>((w11 >> 11)                 & M),
                    static_cast<uint16_t>((w11 >> 24)                 & M),
                    static_cast<uint16_t>((w11 >> 37)                 & M),
                    static_cast<uint16_t>((w11 >> 50)                 & M),
                    static_cast<uint16_t>(((w11 >> 63) | (w12 << 1)) & M),
                    static_cast<uint16_t>((w12 >> 12)                 & M),
                    static_cast<uint16_t>((w12 >> 25)                 & M),
                    static_cast<uint16_t>((w12 >> 38)                 & M),
                    static_cast<uint16_t>((w12 >> 51)                 & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            packed_ptr += 13;
        }

        if (remainder > 0) {
            size_t current_shift = 0, word_offset = 0;
            uint64_t window = packed_ptr[0];
            for (size_t i = 0; i < remainder; i++) {
                uint64_t token_val = (window >> current_shift) & M;
                if (current_shift + 13 > 64)
                    token_val |= (packed_ptr[word_offset + 1] << (64 - current_shift)) & M;
                const uint16_t token_id = static_cast<uint16_t>(token_val);
                current_shift += 13;
                if (current_shift >= 64) { current_shift -= 64; word_offset++; window = packed_ptr[word_offset]; }
                const size_t dict_start = offsets_ptr[token_id];
                const size_t length = offsets_ptr[token_id + 1] - dict_start;
                std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                size += length;
            }
        }
        return size;
    }

    // --- 14-bit (2 sub-groups of 16 tokens, 7 words per super-group) ---
    // Bit offset cycles {0, 32} across 2 sub-groups before realigning.
    if constexpr (Bits == 14) {
        const uint64_t* packed_ptr = packed_data.data();
        constexpr uint64_t M = 0x3FFF;
        const size_t full_super_groups = total_tokens >> 5;  // / 32
        const size_t remainder = total_tokens & 31;

        for (size_t g = 0; g < full_super_groups; g++) {
            // Sub-group 0: bit offset 0, words [0..3]
            {
                const uint64_t w0 = packed_ptr[0], w1 = packed_ptr[1], w2 = packed_ptr[2], w3 = packed_ptr[3];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>( w0                         & M),
                    static_cast<uint16_t>((w0 >> 14)                  & M),
                    static_cast<uint16_t>((w0 >> 28)                  & M),
                    static_cast<uint16_t>((w0 >> 42)                  & M),
                    static_cast<uint16_t>(((w0 >> 56) | (w1 <<  8))  & M),
                    static_cast<uint16_t>((w1 >>  6)                  & M),
                    static_cast<uint16_t>((w1 >> 20)                  & M),
                    static_cast<uint16_t>((w1 >> 34)                  & M),
                    static_cast<uint16_t>((w1 >> 48)                  & M),
                    static_cast<uint16_t>(((w1 >> 62) | (w2 <<  2))  & M),
                    static_cast<uint16_t>((w2 >> 12)                  & M),
                    static_cast<uint16_t>((w2 >> 26)                  & M),
                    static_cast<uint16_t>((w2 >> 40)                  & M),
                    static_cast<uint16_t>(((w2 >> 54) | (w3 << 10))  & M),
                    static_cast<uint16_t>((w3 >>  4)                  & M),
                    static_cast<uint16_t>((w3 >> 18)                  & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 1: bit offset 32, words [3..6]
            {
                const uint64_t w3 = packed_ptr[3], w4 = packed_ptr[4], w5 = packed_ptr[5], w6 = packed_ptr[6];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w3 >> 32)                  & M),
                    static_cast<uint16_t>((w3 >> 46)                  & M),
                    static_cast<uint16_t>(((w3 >> 60) | (w4 <<  4))  & M),
                    static_cast<uint16_t>((w4 >> 10)                  & M),
                    static_cast<uint16_t>((w4 >> 24)                  & M),
                    static_cast<uint16_t>((w4 >> 38)                  & M),
                    static_cast<uint16_t>(((w4 >> 52) | (w5 << 12))  & M),
                    static_cast<uint16_t>((w5 >>  2)                  & M),
                    static_cast<uint16_t>((w5 >> 16)                  & M),
                    static_cast<uint16_t>((w5 >> 30)                  & M),
                    static_cast<uint16_t>((w5 >> 44)                  & M),
                    static_cast<uint16_t>(((w5 >> 58) | (w6 <<  6))  & M),
                    static_cast<uint16_t>((w6 >>  8)                  & M),
                    static_cast<uint16_t>((w6 >> 22)                  & M),
                    static_cast<uint16_t>((w6 >> 36)                  & M),
                    static_cast<uint16_t>((w6 >> 50)                  & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            packed_ptr += 7;
        }

        if (remainder > 0) {
            size_t current_shift = 0, word_offset = 0;
            uint64_t window = packed_ptr[0];
            for (size_t i = 0; i < remainder; i++) {
                uint64_t token_val = (window >> current_shift) & M;
                if (current_shift + 14 > 64)
                    token_val |= (packed_ptr[word_offset + 1] << (64 - current_shift)) & M;
                const uint16_t token_id = static_cast<uint16_t>(token_val);
                current_shift += 14;
                if (current_shift >= 64) { current_shift -= 64; word_offset++; window = packed_ptr[word_offset]; }
                const size_t dict_start = offsets_ptr[token_id];
                const size_t length = offsets_ptr[token_id + 1] - dict_start;
                std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                size += length;
            }
        }
        return size;
    }

    // --- 15-bit (4 sub-groups of 16 tokens, 15 words per super-group) ---
    // Bit offset cycles {0, 48, 32, 16} across sub-groups before realigning.
    if constexpr (Bits == 15) {
        const uint64_t* packed_ptr = packed_data.data();
        constexpr uint64_t M = 0x7FFF;
        const size_t full_super_groups = total_tokens >> 6;  // / 64
        const size_t remainder = total_tokens & 63;

        for (size_t g = 0; g < full_super_groups; g++) {
            // Sub-group 0: bit offset 0, words [0..3]
            {
                const uint64_t w0 = packed_ptr[0], w1 = packed_ptr[1], w2 = packed_ptr[2], w3 = packed_ptr[3];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>( w0                         & M),
                    static_cast<uint16_t>((w0 >> 15)                  & M),
                    static_cast<uint16_t>((w0 >> 30)                  & M),
                    static_cast<uint16_t>((w0 >> 45)                  & M),
                    static_cast<uint16_t>(((w0 >> 60) | (w1 <<  4))  & M),
                    static_cast<uint16_t>((w1 >> 11)                  & M),
                    static_cast<uint16_t>((w1 >> 26)                  & M),
                    static_cast<uint16_t>((w1 >> 41)                  & M),
                    static_cast<uint16_t>(((w1 >> 56) | (w2 <<  8))  & M),
                    static_cast<uint16_t>((w2 >>  7)                  & M),
                    static_cast<uint16_t>((w2 >> 22)                  & M),
                    static_cast<uint16_t>((w2 >> 37)                  & M),
                    static_cast<uint16_t>(((w2 >> 52) | (w3 << 12))  & M),
                    static_cast<uint16_t>((w3 >>  3)                  & M),
                    static_cast<uint16_t>((w3 >> 18)                  & M),
                    static_cast<uint16_t>((w3 >> 33)                  & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 1: bit offset 48, words [3..7]
            {
                const uint64_t w3 = packed_ptr[3], w4 = packed_ptr[4], w5 = packed_ptr[5], w6 = packed_ptr[6], w7 = packed_ptr[7];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w3 >> 48)                  & M),
                    static_cast<uint16_t>(((w3 >> 63) | (w4 <<  1))  & M),
                    static_cast<uint16_t>((w4 >> 14)                  & M),
                    static_cast<uint16_t>((w4 >> 29)                  & M),
                    static_cast<uint16_t>((w4 >> 44)                  & M),
                    static_cast<uint16_t>(((w4 >> 59) | (w5 <<  5))  & M),
                    static_cast<uint16_t>((w5 >> 10)                  & M),
                    static_cast<uint16_t>((w5 >> 25)                  & M),
                    static_cast<uint16_t>((w5 >> 40)                  & M),
                    static_cast<uint16_t>(((w5 >> 55) | (w6 <<  9))  & M),
                    static_cast<uint16_t>((w6 >>  6)                  & M),
                    static_cast<uint16_t>((w6 >> 21)                  & M),
                    static_cast<uint16_t>((w6 >> 36)                  & M),
                    static_cast<uint16_t>(((w6 >> 51) | (w7 << 13))  & M),
                    static_cast<uint16_t>((w7 >>  2)                  & M),
                    static_cast<uint16_t>((w7 >> 17)                  & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 2: bit offset 32, words [7..11]
            {
                const uint64_t w7 = packed_ptr[7], w8 = packed_ptr[8], w9 = packed_ptr[9], w10 = packed_ptr[10], w11 = packed_ptr[11];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w7 >> 32)                   & M),
                    static_cast<uint16_t>((w7 >> 47)                   & M),
                    static_cast<uint16_t>(((w7 >> 62) | (w8 <<  2))   & M),
                    static_cast<uint16_t>((w8 >> 13)                   & M),
                    static_cast<uint16_t>((w8 >> 28)                   & M),
                    static_cast<uint16_t>((w8 >> 43)                   & M),
                    static_cast<uint16_t>(((w8 >> 58) | (w9 <<  6))   & M),
                    static_cast<uint16_t>((w9 >>  9)                   & M),
                    static_cast<uint16_t>((w9 >> 24)                   & M),
                    static_cast<uint16_t>((w9 >> 39)                   & M),
                    static_cast<uint16_t>(((w9 >> 54) | (w10 << 10))  & M),
                    static_cast<uint16_t>((w10 >>  5)                  & M),
                    static_cast<uint16_t>((w10 >> 20)                  & M),
                    static_cast<uint16_t>((w10 >> 35)                  & M),
                    static_cast<uint16_t>(((w10 >> 50) | (w11 << 14)) & M),
                    static_cast<uint16_t>((w11 >>  1)                  & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            // Sub-group 3: bit offset 16, words [11..14]
            {
                const uint64_t w11 = packed_ptr[11], w12 = packed_ptr[12], w13 = packed_ptr[13], w14 = packed_ptr[14];
                const uint16_t tokens[16] = {
                    static_cast<uint16_t>((w11 >> 16)                  & M),
                    static_cast<uint16_t>((w11 >> 31)                  & M),
                    static_cast<uint16_t>((w11 >> 46)                  & M),
                    static_cast<uint16_t>(((w11 >> 61) | (w12 <<  3)) & M),
                    static_cast<uint16_t>((w12 >> 12)                  & M),
                    static_cast<uint16_t>((w12 >> 27)                  & M),
                    static_cast<uint16_t>((w12 >> 42)                  & M),
                    static_cast<uint16_t>(((w12 >> 57) | (w13 <<  7)) & M),
                    static_cast<uint16_t>((w13 >>  8)                  & M),
                    static_cast<uint16_t>((w13 >> 23)                  & M),
                    static_cast<uint16_t>((w13 >> 38)                  & M),
                    static_cast<uint16_t>(((w13 >> 53) | (w14 << 11)) & M),
                    static_cast<uint16_t>((w14 >>  4)                  & M),
                    static_cast<uint16_t>((w14 >> 19)                  & M),
                    static_cast<uint16_t>((w14 >> 34)                  & M),
                    static_cast<uint16_t>((w14 >> 49)                  & M),
                };
                for (int j = 0; j < 16; j++) {
                    const size_t dict_start = offsets_ptr[tokens[j]];
                    const size_t length = offsets_ptr[tokens[j] + 1] - dict_start;
                    std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                    size += length;
                }
            }
            packed_ptr += 15;
        }

        if (remainder > 0) {
            size_t current_shift = 0, word_offset = 0;
            uint64_t window = packed_ptr[0];
            for (size_t i = 0; i < remainder; i++) {
                uint64_t token_val = (window >> current_shift) & M;
                if (current_shift + 15 > 64)
                    token_val |= (packed_ptr[word_offset + 1] << (64 - current_shift)) & M;
                const uint16_t token_id = static_cast<uint16_t>(token_val);
                current_shift += 15;
                if (current_shift >= 64) { current_shift -= 64; word_offset++; window = packed_ptr[word_offset]; }
                const size_t dict_start = offsets_ptr[token_id];
                const size_t length = offsets_ptr[token_id + 1] - dict_start;
                std::memcpy(buffer + size, dict_ptr + dict_start, MAX_LENGTH);
                size += length;
            }
        }
        return size;
    }

    return 0; // unreachable: all bit widths 12-16 handled above
}