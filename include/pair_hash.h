#ifndef PAIR_HASH_H
#define PAIR_HASH_H

#include <cstdint>
#include <utility>
#include <robin_hood.h>

/**
 * @brief Hash function for pairs used with robin_hood hash maps
 */
struct PairHash {
    // For pair<uint16_t, uint16_t>
    size_t operator()(const std::pair<uint16_t, uint16_t>& p) const noexcept {
        return robin_hood::hash<uint32_t>{}((static_cast<uint32_t>(p.first) << 16) | p.second);
    }

    // For pair<uint32_t, uint32_t>
    size_t operator()(const std::pair<uint32_t, uint32_t>& p) const noexcept {
        return robin_hood::hash<uint64_t>{}((static_cast<uint64_t>(p.first) << 32) | p.second);
    }

    // For pair<uint64_t, uint8_t>
    size_t operator()(const std::pair<uint64_t, uint8_t>& p) const noexcept {
        return robin_hood::hash<uint64_t>{}(p.first) ^ robin_hood::hash<uint8_t>{}(p.second);
    }
};

#endif // PAIR_HASH_H
