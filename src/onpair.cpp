#include "onpair.h"

OnPair::OnPair(size_t num_strings, size_t total_bytes) {
    compressed_data.reserve(total_bytes);
    string_boundaries.reserve(num_strings + 1);
    dictionary.reserve(1024 * 1024);
    token_boundaries.reserve(1 << 16);
}

void OnPair::compress_strings(const std::vector<std::string>& strings) {
    auto [data, end_positions] = flatten_strings(strings);
    compress_bytes(data.data(), end_positions);
}

void OnPair::compress_bytes(const uint8_t* data, const std::vector<size_t>& end_positions) {
    LongestPrefixMatcher<uint16_t> lpm = train_dictionary(data, end_positions);
    parse_data(data, end_positions, lpm);
}

size_t OnPair::decompress_string(size_t index, uint8_t* buffer) const {
    const uint8_t* dict_ptr = dictionary.data();
    const uint32_t* offsets_ptr = token_boundaries.data();
    size_t size = 0;

    size_t data_start = string_boundaries[index];
    size_t data_end = string_boundaries[index + 1];

    for (size_t i = data_start; i < data_end; i++) {
        uint16_t token_id = compressed_data[i];

        size_t dict_start = offsets_ptr[token_id];
        size_t dict_end = offsets_ptr[token_id + 1];
        size_t length = dict_end - dict_start;

        // Copy the dictionary entry to the buffer
        std::memcpy(buffer + size, dict_ptr + dict_start, FAST_COPY_SIZE);
        if(length > FAST_COPY_SIZE) {
            std::memcpy(buffer + size + FAST_COPY_SIZE, dict_ptr + dict_start + FAST_COPY_SIZE, length - FAST_COPY_SIZE);
        }

        size += length;
    }

    return size;
}

size_t OnPair::decompress_all(uint8_t* buffer) const {
    const uint8_t* dict_ptr = dictionary.data();
    const uint32_t* offsets_ptr = token_boundaries.data();
    size_t size = 0;

    for (uint16_t token_id : compressed_data) {
        size_t dict_start = offsets_ptr[token_id];
        size_t dict_end = offsets_ptr[token_id + 1];
        size_t length = dict_end - dict_start;

        std::memcpy(buffer + size, dict_ptr + dict_start, FAST_COPY_SIZE);
        if(length > FAST_COPY_SIZE) {
            std::memcpy(buffer + size + FAST_COPY_SIZE, dict_ptr + dict_start + FAST_COPY_SIZE, length - FAST_COPY_SIZE);
        }

        size += length;
    }

    return size;
}

size_t OnPair::space_used() const {
    return compressed_data.size() * sizeof(uint16_t) + 
           dictionary.size() + 
           token_boundaries.size() * sizeof(uint32_t);
}

LongestPrefixMatcher<uint16_t> OnPair::train_dictionary(const uint8_t* data, const std::vector<size_t>& end_positions) {
    token_boundaries.push_back(0);
    
    robin_hood::unordered_map<std::pair<uint16_t, uint16_t>, uint16_t, PairHash> frequency;
    LongestPrefixMatcher<uint16_t> lpm;
    uint16_t next_token_id = 256;
    bool full_dictionary = false;

    // Initialize the dictionary with single-byte tokens
    for(uint16_t i=0; i<=255; i++) {
        uint8_t value = static_cast<uint8_t>(i);
        lpm.insert(&value, 1, i);
        dictionary.push_back(value);
        token_boundaries.push_back(dictionary.size());
    }

    // Shuffle entries
    std::vector<int> shuffled_indices;
    for (int i=0; i<end_positions.size()-1; i++) {
        shuffled_indices.push_back(i);
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(shuffled_indices.begin(), shuffled_indices.end(), g);

    // Set the threshold for merging tokens
    double data_size_mib = static_cast<double>(end_positions.back()) / (1024.0 * 1024.0);
    size_t threshold = static_cast<size_t>(std::fmax(std::log2(data_size_mib), 2.0));

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
            // Find the longest match
            auto match = lpm.find_longest_match(data + pos, end - pos);
            uint16_t match_token_id = match.value().first;
            size_t match_length = match.value().second; 

            // Update token frequency and possibly merge tokens
            auto token_pair = std::make_pair(previous_token_id, match_token_id);
            frequency[token_pair]++;

            if (frequency[token_pair] >= threshold) {
                lpm.insert(data + pos - previous_length, previous_length + match_length, next_token_id);
                dictionary.insert(dictionary.end(), data + pos - previous_length, data + pos + match_length);
                token_boundaries.push_back(dictionary.size());
                
                frequency.erase(token_pair);
                previous_token_id = next_token_id;
                previous_length += match_length;

                if (next_token_id == std::numeric_limits<uint16_t>::max()) {
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

void OnPair::parse_data(const uint8_t* data, const std::vector<size_t>& end_positions, const LongestPrefixMatcher<uint16_t>& lpm) {
    string_boundaries.push_back(0);

    for(int i=0; i<end_positions.size()-1; i++) {
        size_t start = end_positions[i];
        size_t end = end_positions[i+1];

        if (start == end) {
            string_boundaries.push_back(compressed_data.size());
            continue;
        }

        size_t pos = start;
        while (pos < end) {
            // Find the longest match
            auto match = lpm.find_longest_match(data + pos, end - pos);
            uint16_t token_id = match->first;
            size_t length = match->second;
            compressed_data.push_back(token_id);
            pos += length;
        }

        string_boundaries.push_back(compressed_data.size());
    }
}

std::pair<std::vector<uint8_t>, std::vector<size_t>> OnPair::flatten_strings(const std::vector<std::string>& strings) {
    // Calculate total length for efficient allocation
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