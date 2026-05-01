# OnPair

[![CI](https://github.com/gargiulofrancesco/onpair_cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/gargiulofrancesco/onpair_cpp/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/gargiulofrancesco/onpair_cpp/branch/main/graph/badge.svg)](https://codecov.io/gh/gargiulofrancesco/onpair_cpp)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)
![CMake](https://img.shields.io/badge/CMake-3.16%2B-064F8C.svg)
[![License](https://img.shields.io/badge/license-see%20LICENSE-blue.svg)](LICENSE)

**Field-level string compression for database systems, with random access and compressed-domain string predicates.**

OnPair stores a string column as a fixed-width, bit-packed token stream backed by a learned dictionary. It is built for execution engines that need to decompress individual values by row id and evaluate SQL-style filters without first materialising the original byte strings.

## Overview

Generic block codecs optimise byte volume, but database string operators usually need row boundaries, point access, and predicate evaluation. A `LIKE '%needle%'` filter over a compressed block typically becomes: decompress, materialise, scan bytes, discard most rows.

OnPair keeps **each string independently addressable** while sharing a column-level dictionary of frequent byte patterns. Search predicates are compiled into token automata and evaluated directly over the compressed stream:

- `LIKE '%p%'` through token-level KMP.
- `LIKE 'p%'` through prefix-range automata over the sorted dictionary.
- `WHERE col = 'value'` through positional token equality.
- `NOT`, `AND`, and `OR` through composable automata.

The result is a column format that supports point decompression, full-column decompression, and SQL-grade string filters from the same physical representation.

## Architectural Highlights

- **Independent string compression.** Per-string token boundaries provide O(tokens in value) random access; no neighbouring rows or block prefix are decoded.
- **Bit-packed fixed-width store.** Token ids are packed LSB-first at 9-16 bits per token with Arrow-style `n + 1` row boundaries.
- **Compile-time bit-width dispatch.** The runtime bit width is resolved once, then decode and scan loops run through `TokenCursor<Bits>` specialisations.
- **Branchless decoder over-copy.** Decode paths copy `MAX_TOKEN_SIZE` bytes per token and advance by the true token length, removing a length-dependent branch from the inner loop.
- **Sorted dictionary.** Tokens are stored lexicographically, enabling binary-search prefix ranges and sparse automaton transition ranges.
- **Compressed-domain boolean algebra.** Any `TokenAutomaton` can be wrapped with `!`, `&&`, and `||`; combined predicates execute in one pass over the token stream.
- **C++20 range-based API.** Compression accepts any input range whose values convert to `std::string_view`; Arrow-style flat buffers are supported directly.
- **Amortised query compilation.** KMP, prefix, equality, and Aho-Corasick automata precompute dictionary-aware transition state once, then scan many rows.
- **Versioned binary persistence.** Columns serialize to `ONPAIR01` plus dictionary and packed-store arrays.

<!--
## Performance & Benchmarks

Benchmark numbers are intentionally left as placeholders until the benchmark harness and datasets are checked in. Report throughput in GB/s and ratio as `compressed_size / input_size`.

| Codec | Compress GB/s | Point Decode GB/s | Full Decode GB/s | `LIKE '%p%'` Scan GB/s | Size / Input | Notes |
|---|---:|---:|---:|---:|---:|---|
| OnPair | TBD | TBD | TBD | TBD | TBD | Compressed-domain scan; independent row access |
| LZ4 | TBD | TBD | TBD | TBD | TBD | Baseline fast block codec; predicate requires materialization |
| Zstd | TBD | TBD | TBD | TBD | TBD | Baseline ratio-oriented block codec; predicate requires materialization |
| Snappy | TBD | TBD | TBD | TBD | TBD | Baseline database-oriented block codec; predicate requires materialization |

Benchmark dimensions:

- Dataset: real string columns plus synthetic cardinality and prefix-skew sweeps.
- Operations: compression, point decode, full decode, contains, prefix, equality, multi-pattern contains.
- Reporting: median, p95, input bytes/s, compressed bytes/s, output bytes/s, and compressed size.
-->

## Quick Start

```cpp
#include <onpair/api.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace op = onpair;

int main() {
    // 1. Compress any C++20 range of string-like values.
    std::vector<std::string_view> data = {
        "user_000001", "user_000002", "admin_001",
        "user_000003", "guest_001",   "admin_002",
    };

    op::encoding::TrainingConfig cfg;
    cfg.bits      = 14;                                // 16,384-token dictionary
    cfg.threshold = op::encoding::DynamicThreshold{0.15};
    cfg.seed      = 42;                                // reproducible dictionary

    op::OnPairColumn col = op::OnPairColumn::compress(data, cfg);
    op::OnPairColumnView view = col.view();

    // 2. Random access. The buffer needs decoder padding for over-copy.
    std::vector<char> buf(256 + op::DECOMPRESS_BUFFER_PADDING);
    std::size_t len = view.decompress(0, buf.data());  // "user_000001"
    std::string_view value(buf.data(), len);

    // 3. Convenience APIs return row-id vectors.
    auto admin_hits = view.contains("admin");          // LIKE '%admin%'
    auto user_hits  = view.starts_with("user_");       // LIKE 'user_%'
    auto exact_hit  = view.equals("admin_001");        // WHERE col = 'admin_001'

    // 4. Callback APIs avoid allocating a result vector.
    view.contains("admin", [](std::size_t row_id) {
        // Consume matching row ids directly.
    });

    // 5. Multi-pattern search.
    std::vector<std::string_view> patterns = {"admin", "guest"};
    op::search::AhoCorasickAutomaton ac(patterns, view.dictionary());
    view.scan(ac, [](std::size_t row_id) {
        // LIKE '%admin%' OR LIKE '%guest%'
    });

    // 6. Boolean algebra over compressed-domain predicates.
    op::search::KmpAutomaton kmp_user("user", view.dictionary());
    op::search::KmpAutomaton kmp_guest("guest", view.dictionary());

    auto rows = view.scan(kmp_user && !kmp_guest);     // user AND NOT guest
    return 0;
}
```

The scan loop drives automata over token ids. Use callback overloads when the caller already has a selection-vector builder, bitmap writer, or downstream operator sink.

## Advanced Usage / Internals

### Automata Combinators

`TokenAutomaton` is the small execution contract used by the scan loop:

```cpp
void step(onpair::Token token);
bool is_accepted() const;
void reset();
```

Automata may also expose `is_dead()` for early exit. Substring and multi-pattern automata become dead once a match is found; prefix and equality automata become dead once the result can no longer change.

Combinators are lightweight reference wrappers:

```cpp
op::search::KmpAutomaton blocked("@spam.com", view.dictionary());
op::search::KmpAutomaton bounced("bounced",   view.dictionary());
op::search::PrefixAutomaton internal("svc_",  view.dictionary());

view.scan(!blocked && (bounced || internal), [](std::size_t row_id) {
    // NOT blocked AND (bounced OR internal)
});
```

Keep component automata alive for the duration of the scan. Pass composed expressions directly to `scan`, or name intermediate wrappers explicitly when storing them.

### Branchless Over-Copy

Dictionary tokens are capped at `MAX_TOKEN_SIZE == 16`. Decode paths issue a fixed-size copy per token:

```cpp
std::memcpy(out, dict_bytes + token_offset, onpair::MAX_TOKEN_SIZE);
out += token_length;
```

The dictionary is padded so the fixed copy is always in-bounds, and callers provide `DECOMPRESS_BUFFER_PADDING` bytes past the true output length. This trades a small amount of safe over-copy for a simpler decode loop with no token-length branch in the hot path.

### Token-Level Search

- `KmpAutomaton` builds byte-level KMP failure state, then compiles dictionary-token transitions into a dense base table plus sparse exception ranges.
- `AhoCorasickAutomaton` eagerly compiles multi-pattern transitions; `AhoCorasickLazyAutomaton` defers sparse-state expansion until a state is reached; `AhoCorasickOnlineAutomaton` avoids token-level precomputation.
- `PrefixAutomaton` tokenizes the query prefix and precomputes valid divergence ranges through `DictionaryView::prefix_range`.
- `EqAutomaton` tokenizes the query once and rejects by token mismatch or length difference.

## Robustness & CI

The test suite is GoogleTest-based and split by module: core storage, dictionary views, encoding, parsing, decoding, automata, search combinators, serialization, and column integration.

CI runs on:

- **Linux GCC 14** in Debug and Release.
- **Linux Clang 18** in Debug and Release.
- **macOS AppleClang** in Debug and Release.
- **Windows MSVC** in Debug and Release.

Additional CI jobs enforce:

- **ASan + UBSan** on Clang 18.
- **TSan** on Clang 18.
- **Codecov upload** from the GCC coverage build on `main`.
- Weekly scheduled runs in addition to push and pull-request validation.

Local test run:

```bash
cmake -B build -DONPAIR_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure --parallel 4
```

Sanitizer run:

```bash
cmake -B build_san \
  -DONPAIR_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

cmake --build build_san --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build_san --output-on-failure --parallel 4
```

## Integration

### Requirements

- C++20 compiler: GCC 11+, Clang 13+, AppleClang, or MSVC 19.29+.
- CMake 3.16+.
- Boost.Unordered 1.91. If unavailable as a system package, CMake fetches Boost 1.91 through `FetchContent`.

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Link the static library target:

```cmake
target_link_libraries(my_target PRIVATE onpair)
```

The repository also provides a helper for examples and local tools:

```cmake
onpair_executable(my_program.cpp)
```

### FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(
  onpair
  GIT_REPOSITORY https://github.com/gargiulofrancesco/onpair_cpp.git
  GIT_TAG        main
)

FetchContent_MakeAvailable(onpair)

target_link_libraries(my_target PRIVATE onpair)
```

### Arrow-Style Buffers

Use the raw-buffer overload when the input already exists as a contiguous byte buffer plus offsets:

```cpp
const char*     bytes   = arrow_string_array.data();
const uint32_t* offsets = arrow_string_array.offsets(); // n + 1 entries
std::size_t     n       = arrow_string_array.length();

op::OnPairColumn col = op::OnPairColumn::compress(bytes, offsets, n, cfg);
```

### Serialization

```cpp
std::ofstream out("column.onp", std::ios::binary);
col.write_to(out);

std::ifstream in("column.onp", std::ios::binary);
op::OnPairColumn restored = op::OnPairColumn::read_from(in);
```

The binary format starts with `ONPAIR01`, followed by bit width, dictionary bytes and offsets, packed token words, and row token boundaries.
