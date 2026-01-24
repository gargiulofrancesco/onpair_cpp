#ifndef BIT_PACKING_H
#define BIT_PACKING_H

#include <vector>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <bit>

namespace bit_packing {

template<int Bits>
struct BitReader {
    static_assert(Bits > 0 && Bits <= 64, "Bit width must be between 1 and 64");

    static inline uint64_t read(const uint64_t* data, size_t index) {
        const size_t bit_offset = index * Bits;
        const size_t word_idx   = bit_offset >> 6; // bit_offset / 64
        const size_t shift      = bit_offset & 63; // bit_offset % 64

        uint64_t value = data[word_idx];
        uint64_t result = value >> shift;

        if (shift + Bits > 64) {
             uint64_t next_value = data[word_idx + 1];
             result |= (next_value << (64 - shift));
        }

        constexpr uint64_t mask = (Bits == 64) ? ~0ULL : ((1ULL << Bits) - 1);
        return result & mask;
    }
};

class BitWriter {
private:
    std::vector<uint64_t>& storage;
    size_t current_bit_pos;

public:
    explicit BitWriter(std::vector<uint64_t>& target_storage) 
        : storage(target_storage), current_bit_pos(0) {
        storage.clear();
        storage.push_back(0);
    }

    void write(uint64_t value, uint8_t bits) {
        if (bits == 0) return;

        size_t word_idx = current_bit_pos >> 6;
        size_t shift    = current_bit_pos & 63;
        
        if (word_idx >= storage.size()) {
            storage.resize(word_idx + 1, 0);
        }

        uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
        value &= mask;

        storage[word_idx] |= (value << shift);

        size_t bits_in_first_word = 64 - shift;
        if (bits > bits_in_first_word) {
            size_t next_word_idx = word_idx + 1;
            if (next_word_idx >= storage.size()) {
                storage.push_back(0);
            }
            // Safe shift: bits_in_first_word is always < 64 here
            storage[next_word_idx] |= (value >> bits_in_first_word);
        }

        current_bit_pos += bits;
    }

    void flush_and_pad() {
        // Ensure we cover the last used word
        size_t required_words = (current_bit_pos + 63) >> 6;
        if (storage.size() < required_words) {
            storage.resize(required_words, 0);
        }
        // Critical padding for BitReader safety
        storage.push_back(0);
    }
    
    size_t total_bits() const {
        return current_bit_pos;
    }
};

constexpr uint8_t min_bits_for(size_t value) {
    // __builtin_clzll is undefined for 0
    if (value == 0) return 1;
    return 64 - __builtin_clzll(value);
}

}

#endif // BIT_PACKING_H