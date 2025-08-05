#include "onpair16.h"
#include <vector>
#include <string>
#include <iostream>

int main() {
    // Simulate a database column with user IDs
    std::vector<std::string> strings = {
        "user_000001",
        "user_000002", 
        "user_000003",
        "admin_001",
        "user_000004",
        "user_000005",
        "guest_001",
        "user_000006",
        "admin_002",
        "user_000007",
    };

    // Calculate total size
    size_t n_strings = strings.size();
    size_t total_bytes = 0;
    for (const auto& s : strings) {
        total_bytes += s.size();
    }

    OnPair16 compressor(n_strings, total_bytes);
    compressor.compress_strings(strings);
    
    // Test decompression
    std::vector<uint8_t> buffer(256);    
    for (size_t i = 0; i < n_strings; ++i) {
        size_t length = compressor.decompress_string(i, buffer.data());
        std::string result(buffer.begin(), buffer.begin() + length);
        std::cout << "  [" << i << "] \"" << result << "\"\n";
    }
    
    return 0;
}
