# OnPair

**Field-level string compression for database systems — with fast random access and compressed-domain pattern matching.**

OnPair compresses a column of strings into a compact bit-packed token stream using a fixed dictionary of frequent patterns learned from the data. Once compressed, strings can be decompressed individually at any index without scanning from the start, and pattern-matching queries (e.g., `LIKE '%pattern%'`) can run directly against the compressed representation — without materialising the original bytes.

---

## Features

- **Fast random access** — each string is compressed independently; retrieving a single entry requires decompressing only the requested data, never neighbouring entries.
- **Optimised bulk decompression** — `decode_all` unpacks an entire compressed column in one tight pass.
- **Compressed-domain search** — run SQL-grade filters directly against the compressed token stream:
  - `contains` — substring search (`LIKE '%p%'`)
  - `starts_with` / `PrefixAutomaton` — prefix search (`LIKE 'p%'`)
  - `equals` / `EqAutomaton` — exact match (`WHERE col = 'value'`)
  - `scan` + `NegatedAutomaton` — negation of any automaton
  - `scan` + `AndAutomaton` / `OrAutomaton` — conjunction and disjunction of any two automata
- **Boolean algebra over compressed search** — `NegatedAutomaton`, `AndAutomaton`, and `OrAutomaton` are zero-cost wrappers that compose any two `TokenAutomaton` instances into a new one. Operator overloads (`!`, `&&`, `||`) let you write `view.scan(a && !b)` directly. Build arbitrarily complex filter predicates and execute them in a single compressed-domain pass, without decompressing a single string.
- **Range-concept input** — the compression API accepts any C++20 range whose elements are convertible to `std::string_view`: `std::vector<std::string>`, `std::span<std::string_view>`, lazy generators, and more.
- **Compile-time bit-width dispatch** — all hot paths are template-specialised per bit-width (12–16) at compile time; no runtime branch on bit-width during decompression or search.
- **Binary serialisation** — write a compressed column to any `std::ostream` and reconstruct it exactly from any `std::istream`.

---

## Quick Start

```cpp
#include <onpair/api.h>
#include <onpair/search/automata/aho_corasick_automaton.h>

// 1. Compress — accepts any C++20 range of string-like values
std::vector<std::string_view> data = {
    "user_000001", "user_000002", "admin_001",
    "user_000003", "guest_001",   "admin_002",
};
onpair::OnPairColumn col = onpair::OnPairColumn::compress(data);
auto view = col.view();

// 2. Random access — decompress any single string in O(tokens)
std::vector<char> buf(256 + onpair::DECOMPRESS_BUFFER_PADDING);
size_t len = view.decompress(0, buf.data());           // → "user_000001"

// 3. Substring search     (SQL: LIKE '%admin%')
auto admin_hits = view.contains("admin");              // → {2, 5}

// 4. Prefix search        (SQL: LIKE 'user_%')
auto user_hits  = view.starts_with("user_");           // → {0, 1, 3}

// 5. Multi-pattern search (SQL: LIKE '%admin%' OR LIKE '%guest%')
std::vector<std::string_view> pats = {"admin", "guest"};
onpair::search::AhoCorasickAutomaton ac(pats, view.dictionary());
auto mixed_hits = view.scan(ac);                       // → {2, 4, 5}

// 6. Exact match          (SQL: WHERE col = 'admin_001')
auto exact_hit  = view.equals("admin_001");            // → {2}

// 7. Negation             (SQL: NOT LIKE '%admin%')
onpair::search::KmpAutomaton kmp_admin("admin", view.dictionary());
auto non_admin = view.scan(!kmp_admin);                // → {0, 1, 3, 4}

// 8. Boolean algebra      (SQL: LIKE '%user%' AND NOT LIKE '%guest%')
onpair::search::KmpAutomaton kmp_user ("user",  view.dictionary());
onpair::search::KmpAutomaton kmp_guest("guest", view.dictionary());
auto result = view.scan(kmp_user && !kmp_guest);
```

Every operation runs entirely on the compressed token stream — no decompression, no intermediate allocations. The dictionary is compiled into a token-level automaton once at query construction time.

---

## Usage

### Compression

**From any range:**

```cpp
onpair::encoding::TrainingConfig cfg;
cfg.bits      = 14;                                          // 2^14 = 16 384 tokens
cfg.threshold = onpair::encoding::DynamicThreshold{0.15};    // train on 15% of input
cfg.seed      = 42;                                          // reproducible dictionary

onpair::OnPairColumn col = onpair::OnPairColumn::compress(strings, cfg);
```

**From an Apache Arrow buffer (zero-copy ingestion):**

```cpp
// flat byte buffer + Arrow-style offset array (n+1 entries for n strings)
const char*     data    = arrow_string_array.data();
const uint32_t* offsets = arrow_string_array.offsets();
size_t          n       = arrow_string_array.length();

onpair::OnPairColumn col = onpair::OnPairColumn::compress(data, offsets, n, cfg);
```

### Random Access

The decoder uses a branchless over-copy technique: it always copies `MAX_TOKEN_SIZE` (16) bytes per token regardless of the token's true size. This eliminates a branch and a conditional move from the inner decode loop. Callers must provide `DECOMPRESS_BUFFER_PADDING` bytes of headroom beyond the true string size.

```cpp
auto view = col.view();

// Buffer must extend DECOMPRESS_BUFFER_PADDING bytes past the longest expected string.
std::vector<char> buf(max_string_size + onpair::DECOMPRESS_BUFFER_PADDING);

size_t len = view.decompress(idx, buf.data());
std::string_view sv(buf.data(), len);
```

### Substring Search

OnPair pre-compiles a token-level KMP automaton against the dictionary at query preparation time.

```cpp
auto view = col.view();

// LIKE '%pattern%'
auto matches = view.contains("admin");
// matches is a std::vector<size_t> of matching string indices

// Callback form — avoids allocating a result vector
onpair::search::KmpAutomaton kmp("admin", view.dictionary());
view.scan(kmp, [](size_t idx) {
    // process each matching index directly
});
```

For multi-pattern substring search (`LIKE '%P1%' OR LIKE '%P2%' ...`), use `AhoCorasickAutomaton`:

```cpp
#include <onpair/search/automata/aho_corasick_automaton.h>

std::vector<std::string_view> patterns = {"error", "warning", "fatal"};
onpair::search::AhoCorasickAutomaton ac(patterns, view.dictionary());
auto hits = view.scan(ac);
```

### Prefix Scan

OnPair pre-compiles a token-level prefix automaton against the dictionary at query preparation time. The query prefix is tokenised and, for each token position, a range of valid token IDs is precomputed. At scan time, each string is checked in O(query_tokens) — typically a handful of comparisons.

```cpp
auto view = col.view();

// LIKE 'prefix%'
auto matches = view.starts_with("user_");
// matches is a std::vector<size_t> of matching string indices

// Callback form — avoids allocating a result vector
onpair::search::PrefixAutomaton pa("user_", view.dictionary());
view.scan(pa, [](size_t idx) {
    // process each matching index directly
});
```

### Exact Match

`equals` finds all strings that are exactly equal to a given value (`WHERE col = 'value'`). It tokenises the query once at construction time, then rejects strings in O(1) if the token count differs and checks equality in O(query_tokens) otherwise.

```cpp
auto hits = view.equals("admin_001");   // → only the exact string

// Callback form
view.equals("admin_001", [](size_t idx) { /* … */ });
```

For composable queries, use `EqAutomaton` — a `TokenAutomaton` implementation that works with combinators:

```cpp
#include <onpair/search/automata/eq_automaton.h>

onpair::search::EqAutomaton eq("admin_001", view.dictionary());
auto hits = view.scan(eq);              // equivalent to view.equals("admin_001")
auto combined = view.scan(eq && !kmp);  // composable with other automata
```

### Negation, Conjunction, and Disjunction

`NegatedAutomaton`, `AndAutomaton`, and `OrAutomaton` are combinator structs that wrap any `TokenAutomaton` instances. They satisfy `TokenAutomaton` themselves, so they can be passed to `view.scan()` and nested arbitrarily. Operator overloads (`!`, `&&`, `||`) provide concise syntax:

```cpp
auto view = col.view();

// Generic negation: strings NOT accepted by the wrapped automaton
onpair::search::KmpAutomaton kmp("guest", view.dictionary());
auto not_guest = view.scan(!kmp);

// Conjunction: strings containing BOTH "user" and "admin"
onpair::search::KmpAutomaton kmp_u("user",  view.dictionary());
onpair::search::KmpAutomaton kmp_a("admin", view.dictionary());
auto result = view.scan(kmp_u && kmp_a);

// Disjunction: strings containing EITHER "error" OR "fatal"
onpair::search::KmpAutomaton kmp_e("error", view.dictionary());
onpair::search::KmpAutomaton kmp_f("fatal", view.dictionary());
auto hits = view.scan(kmp_e || kmp_f);
```

The struct syntax (`NegatedAutomaton{kmp}`, `AndAutomaton{a, b}`, `OrAutomaton{a, b}`) remains available for contexts where explicit naming is preferred.

`NegatedAutomaton` produces the exact complement of `scan`: together they partition the full index range `[0, n)` with no overlap and no gaps.

### Boolean Algebra over Compressed Search

`NegatedAutomaton<A>`, `AndAutomaton<A,B>`, and `OrAutomaton<A,B>` are zero-cost wrappers that compose any `TokenAutomaton` instances into a new one — satisfying `TokenAutomaton` themselves. Operator overloads (`!`, `&&`, `||`) let you express complex boolean queries concisely:

```cpp
namespace search = onpair::search;

auto view = col.view();

// Build automata once — O(dict_size) precomputation, amortised across the whole column
search::KmpAutomaton blocked("@spam.com", view.dictionary());
search::KmpAutomaton error  ("error",     view.dictionary());

// NOT LIKE '%@spam.com%'
view.scan(!blocked, [](size_t i) { /* safe address */ });

// contains "@spam.com" OR contains "error"
view.scan(blocked || error, [](size_t i) { /* flag for review */ });

// Nest further: NOT spam AND (spam OR error)
view.scan(!blocked && (blocked || error), [](size_t i) { /* … */ });
```

**All early-exit optimisations compose.** When any component automaton signals `is_dead()`, the combinator propagates it immediately: `AndAutomaton` exits as soon as either leg definitively rejects; `OrAutomaton` exits as soon as either leg definitively accepts; `NegatedAutomaton` exits at the same point as the inner automaton. No decompression, no per-byte scanning, no heap allocation at query time.

Because the combinators themselves satisfy `TokenAutomaton`, they nest naturally: `a && !(b || c)` is a valid automaton with no overhead beyond what the component automata themselves require.

### OnPairColumnView — Non-Owning Access

`OnPairColumnView` is a thin non-owning wrapper. It is the correct type to pass to functions that operate on a column but should not own it.

```cpp
void process(onpair::OnPairColumnView view) {
    for (size_t i = 0; i < view.num_strings(); ++i) {
        size_t len = view.decompress(i, buf.data());
        // …
    }
}

auto view = col.view();
process(view);
```

### Serialisation

```cpp
// Write
{
    std::ofstream out("column.onp", std::ios::binary);
    col.write_to(out);
}

// Read back
{
    std::ifstream in("column.onp", std::ios::binary);
    auto col2 = onpair::OnPairColumn::read_from(in);
}
```

The binary format begins with the magic bytes `ONPAIR01` followed by the bit-width byte, then the dictionary and packed token stream.

---

## Architecture

### Module Overview

```
include/onpair/
│
├── api.h                          ← Single public header
│
├── core/                             ← Plain data structures; no encoding or search logic
│   ├── types.h                       ← Token, BitWidth, MAX_TOKEN_SIZE, span types
│   ├── dictionary.h                  ← Dictionary
│   ├── dictionary_view.h             ← DictionaryView
│   ├── store.h                       ← Store (owns the bit-packed token stream)
│   └── store_view.h                  ← StoreView (non-owning, passed by value)
│
├── encoding/                         ← Write path — produces core storage objects
│   ├── training/
│   │   ├── trainer.h                 ← train()
│   │   ├── config.h                  ← TrainingConfig, FixedThreshold, DynamicThreshold
│   │   └── dynamic_threshold.h       ← Adaptive threshold controller
│   ├── parsing/
│   │   ├── parser.h                  ← parse()
│   │   └── bit_writer.h              ← BitWriter (LSB-first bit-packing)
│   └── lpm.h                         ← LongestPrefixMatcher (two-tier hash structure)
│
├── decoding/                         ← Read path — consumes core storage objects
│   ├── token_cursor.h                ← TokenCursor<Bits>, with_cursor() runtime dispatch
│   ├── detail/
│   │   └── decode_all.h              ← Batch-unrolled decode_all<Bits> (bulk fast path)
│   └── decoder.h                     ← Decoder
│
├── column/                           ← Column-level compression and access
│   ├── column.h                      ← OnPairColumn (owning, move-only)
│   └── column_view.h                 ← OnPairColumnView (non-owning)
│
└── search/                           ← Pattern matching on the token stream
    ├── eq_search.h                   ← EQSearch (positional equality)
    ├── detail/
    │   └── tokenize.h                ← Shared tokenization helper
    └── automata/                     ← TokenAutomaton framework + implementations
        ├── token_automaton.h         ← TokenAutomaton / DeadDetectable concepts,
        │                                combinators + operator overloads (!, &&, ||)
        ├── token_stream.h            ← TokenStream concept
        ├── scan.h                    ← drive() + scan_impl (automaton scan loop)
        ├── kmp_automaton.h           ← KmpAutomaton (single-pattern substring)
        ├── aho_corasick_automaton.h  ← AhoCorasickAutomaton (multi-pattern)
        ├── eq_automaton.h            ← EqAutomaton (exact equality)
        └── prefix_automaton.h        ← PrefixAutomaton (prefix match)

src/onpair/                           ← Non-template implementation files
├── core/
│   └── dictionary_view.cpp           ← DictionaryView::prefix_range
├── encoding/
│   ├── training/trainer.cpp          ← train()
│   └── parsing/parser.cpp            ← parse()
└── column/
    └── column.cpp                    ← compress(), write_to(), read_from()
```

---

## Testing

The test suite uses [Google Test](https://github.com/google/googletest).

```bash
# Configure
cmake -B build -DONPAIR_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --parallel

# Run all tests
ctest --test-dir build --output-on-failure
```

**Run a specific test file:**

```bash
ctest --test-dir build -R test_trainer --output-on-failure
```

**Run a single test case:**

```bash
ctest --test-dir build -R "MaxDictSizeTest.IsPowerOfTwo"
```

**Run with AddressSanitizer + UndefinedBehaviourSanitizer:**

```bash
cmake -B build_asan -DONPAIR_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build_asan --parallel
ctest --test-dir build_asan --output-on-failure
```

Each test file compiles to its own executable, so a crash in one module never prevents the others from running. Every `TEST()` case is registered as a separate CTest entry, enabling granular reruns.

---

## Build

**Requirements:**
- C++20 compiler (GCC ≥ 11, Clang ≥ 13, MSVC 19.29+)
- CMake ≥ 3.16

**Setup:**

```bash
# Configure (Release enables -O3 -march=native and LTO where supported)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

**Run the examples:**

```bash
./build/basic_usage
```

**Add your own executable via the helper:**

```cmake
# In your CMakeLists.txt
onpair_executable(my_program.cpp)   # automatically links onpair
```

Or link manually:

```cmake
target_link_libraries(my_target PRIVATE onpair)
```
