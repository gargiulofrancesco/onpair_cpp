# OnPair: Short Strings Compression for Fast Random Access

[![Paper](https://img.shields.io/badge/Paper-arXiv:2508.02280-blue)](https://arxiv.org/abs/2508.02280)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

C++ implementation of **OnPair**, a compression algorithm designed for efficient random access on sequences of short strings.

## Overview

OnPair is a field-level compression algorithm designed for workloads requiring fast random access to individual strings in large collections. The compression process consists of two distinct phases:

- **Training Phase**: A longest prefix matching strategy is used to parse the input and identify frequent adjacent token pairs. When the frequency of a pair exceeds a predefined threshold, a new token is created to represent the merged pair. This continues until the dictionary is full or the input data is exhausted. The dictionary supports up to 65,536 tokens, with each token assigned a fixed 2-byte ID.
- **Parsing Phase**: Once the dictionary is constructed, each string is compressed independently into a sequence of token IDs by greedily applying longest prefix matching

OnPair16 is an optimized variant that limits tokens to 16 bytes for better performance.

## Quick Start

```cpp
#include "onpair.h"
#include <vector>
#include <string>
#include <iostream>

int main() {
    std::vector<std::string> strings = {
        "user_000001",
        "user_000002", 
        "user_000003",
        "admin_001",
        "user_000004",
    };

    // Create compressor
    OnPair compressor;
    
    // Compress strings
    compressor.compress_strings(strings);
    
    // Decompress individual strings
    std::vector<uint8_t> buffer(256);    
    for (size_t i = 0; i < strings.size(); ++i) {
        size_t length = compressor.decompress_string(i, buffer.data());
        std::string result(buffer.begin(), buffer.begin() + length);
        std::cout << "  [" << i << "] \"" << result << "\"\n";
    }
    
    return 0;
}
```

## Building from Source

```bash
git clone https://github.com/gargiulofrancesco/onpair_cpp.git
cd onpair_cpp
mkdir build && cd build
cmake ..
make
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Authors

- **Francesco Gargiulo** - [francesco.gargiulo@phd.unipi.it](mailto:francesco.gargiulo@phd.unipi.it)
- **Rossano Venturini** - [rossano.venturini@unipi.it](mailto:rossano.venturini@unipi.it)

*University of Pisa, Italy*
