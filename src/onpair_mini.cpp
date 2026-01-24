#include "onpair_mini.h"

OnPairMini::OnPairMini(size_t threshold, uint8_t bits) 
    : threshold(threshold), bits_per_token(bits) 
{
    assert(threshold > 1 && "Threshold must be greater than 1");
    assert(bits >= 9 && bits <= 16 && "Bits per token must be between 9 and 16");
}

OnPairMini::OnPairMini(size_t num_strings, size_t total_bytes, size_t threshold, uint8_t bits)
    : OnPairMini(threshold, bits)
{    
    packed_data.reserve(total_bytes / 8);
    string_boundaries.reserve(num_strings + 1);
    dictionary.reserve(1024 * 1024);
    token_boundaries.reserve(1 << bits);
}

void OnPairMini::compress_strings(const std::vector<std::string>& strings) {
    auto [data, end_positions] = flatten_strings(strings);
    compress_bytes(data.data(), end_positions);
}

void OnPairMini::compress_bytes(const uint8_t* data, const std::vector<size_t>& end_positions) {
    LongestPrefixMatcher16 lpm = train_dictionary(data, end_positions);
    parse_data(data, end_positions, lpm);
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

    // --- OPTIMIZATION 1: FAST PATH (16-bit) ---
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

    // --- OPTIMIZATION 2: Variable Bits ---
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

    // If we are on byte boundaries, treat data as a raw array.
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

    // --- INCREMENTAL DECODING (For 9-15 bits) ---
    // Avoids calculating "i * Bits" (multiplication) every iteration.
    // Maintains a running "window" into the bit stream.
    
    const uint64_t* packed_ptr = packed_data.data();
    
    // State variables
    size_t current_word_idx = 0;
    size_t current_shift = 0;
    
    // Pre-load the first word
    uint64_t window = packed_ptr[0]; 
    
    // Constant mask for extraction
    constexpr uint64_t mask = (1ULL << Bits) - 1;

    for (size_t i = 0; i < total_tokens; i++) {
        // Extract Token
        uint64_t token_val = (window >> current_shift) & mask;
        
        // Handle Split Tokens 
        if (current_shift + Bits > 64) {
             uint64_t next_word = packed_ptr[current_word_idx + 1];
             token_val |= (next_word << (64 - current_shift)) & mask;
        }
        
        uint16_t token_id = static_cast<uint16_t>(token_val);

        // Advance Bit Cursor
        current_shift += Bits;
        
        // If we exhausted the current word, move to the next
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

// -----------------------------------------------------------------------------
// DISPATCHERS
// -----------------------------------------------------------------------------

size_t OnPairMini::decompress_string(size_t index, uint8_t* buffer) const {
    switch(bits_per_token) {
        case 9:  return decompress_string_impl<9>(index, buffer);
        case 10: return decompress_string_impl<10>(index, buffer);
        case 11: return decompress_string_impl<11>(index, buffer);
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
        case 9:  return decompress_all_impl<9>(buffer);
        case 10: return decompress_all_impl<10>(buffer);
        case 11: return decompress_all_impl<11>(buffer);
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

// -----------------------------------------------------------------------------
// COMPRESSION / TRAINING
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