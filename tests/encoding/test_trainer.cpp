#include <onpair/encoding/training/trainer.h>
#include <onpair/encoding/training/config.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include <algorithm>
#include <cstring>

using namespace onpair;
using namespace onpair::encoding;
using namespace test_helpers;

// ── Helpers ───────────────────────────────────────────────────────────────────

static TrainResult train_strings(const std::vector<std::string>& strings,
                                 const TrainingConfig& cfg = {})
{
    auto raw = make_raw(strings);
    return train(raw.data.data(), raw.offsets.data(), raw.n, cfg);
}

// Verify that all 256 single-byte values 0x00-0xFF appear in the dictionary.
// After sorting + merging, they may no longer be at positions 0-255.
static void check_base_tokens(const Dictionary& d)
{
    ASSERT_GE(d.num_tokens(), 256u) << "dictionary must have at least 256 base tokens";
    std::vector<bool> found(256, false);
    for (size_t i = 0; i < d.num_tokens(); ++i) {
        const uint32_t begin = d.offsets[i];
        const uint32_t end   = d.offsets[i + 1];
        if (end - begin == 1u) {
            found[d.bytes[begin]] = true;
        }
    }
    for (int i = 0; i < 256; ++i) {
        EXPECT_TRUE(found[i]) << "base token for byte " << i << " not found in dictionary";
    }
}

// ── Baseline invariant ────────────────────────────────────────────────────────

TEST(TrainerTest, BaseTokensAlwaysSingleBytes) {
    // Any corpus must yield exactly 256 single-byte base tokens first.
    auto result = train_strings(make_user_strings(50));
    check_base_tokens(result.dict);
}

TEST(TrainerTest, BaseTokensOnEmptyInput) {
    // n=0: offsets must still have the sentinel element {0}.
    std::vector<uint8_t>  data    = {};
    std::vector<uint32_t> offsets = {0};
    TrainResult result = train(data.data(), offsets.data(), 0, {});
    check_base_tokens(result.dict);
    EXPECT_EQ(result.dict.num_tokens(), 256u);
}

TEST(TrainerTest, BaseTokensOnSingleEmptyString) {
    // A corpus of one empty string should still produce 256 base tokens.
    std::vector<uint8_t>  data    = {};
    std::vector<uint32_t> offsets = {0, 0};
    auto result = train(data.data(), offsets.data(), 1, {});
    check_base_tokens(result.dict);
    EXPECT_EQ(result.dict.num_tokens(), 256u);
}

// ── Dictionary size bounds ────────────────────────────────────────────────────

TEST(TrainerTest, DictionarySizeDoesNotExceedCapacity) {
    TrainingConfig cfg;
    cfg.bits      = 12;  // max 4096 tokens
    cfg.threshold = FixedThreshold{2};
    // Large repetitive corpus will try to fill the dictionary.
    auto result = train_strings(make_user_strings(500), cfg);
    EXPECT_LE(result.dict.num_tokens(), max_dict_size(cfg.bits));
}

// ── FixedThreshold ────────────────────────────────────────────────────────────

TEST(TrainerTest, ThresholdGatesMerges) {
    // 100 copies of "ab": the pair (a,b) appears exactly 100 times.
    // threshold=2  → merges happen (100 >= 2).
    // threshold=101 → no merge    (100 < 101).
    std::vector<std::string> corpus(100, "ab");

    TrainingConfig cfg_low;
    cfg_low.threshold = FixedThreshold{2};
    EXPECT_GT(train_strings(corpus, cfg_low).dict.num_tokens(), 256u);

    TrainingConfig cfg_high;
    cfg_high.threshold = FixedThreshold{101};
    EXPECT_EQ(train_strings(corpus, cfg_high).dict.num_tokens(), 256u);
}

TEST(TrainerTest, FixedThreshold2MergesFrequentPairs) {
    // "aabb" repeated many times: the pair "aa" and "bb" each appear >= 2 times.
    std::vector<std::string> corpus(50, "aabb");
    TrainingConfig cfg;
    cfg.threshold = FixedThreshold{2};
    auto result = train_strings(corpus, cfg);
    // Should have more than 256 tokens (at least one merge happened).
    EXPECT_GT(result.dict.num_tokens(), 256u);
}

TEST(TrainerTest, MergedTokenContentIsCorrect) {
    // With 50 copies of "ab" and threshold=2, the pair "ab" must appear in
    // the dictionary as a multi-byte token.
    std::vector<std::string> corpus(50, "ab");
    TrainingConfig cfg;
    cfg.threshold = FixedThreshold{2};
    auto result = train_strings(corpus, cfg);

    bool found = false;
    for (size_t i = 0; i < result.dict.num_tokens(); ++i) {
        const size_t len = result.dict.offsets[i + 1] - result.dict.offsets[i];
        if (len == 2 &&
            result.dict.bytes[result.dict.offsets[i]]     == 'a' &&
            result.dict.bytes[result.dict.offsets[i] + 1] == 'b') {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "merged token \"ab\" not found in dictionary";
}

// ── Seed reproducibility ──────────────────────────────────────────────────────

TEST(TrainerTest, SameSeed_ProducesIdenticalDictionaries) {
    auto corpus = make_random_strings(100, 40, 12345);
    TrainingConfig cfg;
    cfg.seed = 42;

    auto r1 = train_strings(corpus, cfg);
    auto r2 = train_strings(corpus, cfg);

    ASSERT_EQ(r1.dict.num_tokens(), r2.dict.num_tokens());
    EXPECT_EQ(r1.dict.bytes,   r2.dict.bytes);
    EXPECT_EQ(r1.dict.offsets, r2.dict.offsets);
}

// ── sort_dictionary ───────────────────────────────────────────────────────────

static bool is_lex_sorted(const Dictionary& d)
{
    const size_t n = d.num_tokens();
    for (size_t i = 1; i < n; ++i) {
        const uint8_t* a   = d.bytes.data() + d.offsets[i - 1];
        const size_t   la  = d.offsets[i] - d.offsets[i - 1];
        const uint8_t* b   = d.bytes.data() + d.offsets[i];
        const size_t   lb  = d.offsets[i + 1] - d.offsets[i];
        const size_t   cmp_len = std::min(la, lb);
        int cmp = std::memcmp(a, b, cmp_len);
        if (cmp == 0) cmp = (la < lb) ? -1 : (la > lb) ? 1 : 0;
        if (cmp > 0) return false;  // b < a → not sorted
    }
    return true;
}

TEST(TrainerTest, DictionaryIsAlwaysSorted) {
    auto result = train_strings(make_user_strings(100));
    EXPECT_TRUE(is_lex_sorted(result.dict));
}

TEST(TrainerTest, LpmRemapsCorrectly) {
    auto strings = make_user_strings(30);
    auto result  = train_strings(strings);
    const size_t n = result.dict.num_tokens();

    // For every token in the dictionary, the LPM must map its bytes
    // back to its token ID.
    for (size_t id = 0; id < n; ++id) {
        const uint8_t* p = result.dict.bytes.data() + result.dict.offsets[id];
        const size_t   l = result.dict.offsets[id + 1] - result.dict.offsets[id];
        auto hit = result.lpm.find_longest_match(p, l);
        EXPECT_EQ(hit.first,  Token(id)) << "ID mismatch for token " << id;
        EXPECT_EQ(hit.second, l)         << "length mismatch for token " << id;
    }
}

// ── Token byte length ─────────────────────────────────────────────────────────

TEST(TrainerTest, NoTokenExceedsMaxTokenSize) {
    auto result = train_strings(make_random_strings(100, 50, 99));
    for (size_t i = 0; i < result.dict.num_tokens(); ++i) {
        const size_t len = result.dict.offsets[i + 1] - result.dict.offsets[i];
        EXPECT_LE(len, MAX_TOKEN_SIZE) << "token " << i << " exceeds MAX_TOKEN_SIZE";
    }
}

TEST(TrainerTest, NoTokenHasZeroLength) {
    // Merging must never produce a zero-length token.
    // The mirror invariant to NoTokenExceedsMaxTokenSize.
    struct Corpus { const char* name; std::vector<std::string> strings; };
    TrainingConfig cfg;
    cfg.threshold = FixedThreshold{2};
    cfg.seed = 42;

    const std::vector<Corpus> corpora = {
        {"random",    make_random_strings(100, 50, 77)},
        {"user",      make_user_strings(50)},
        {"binary",    make_binary_strings(50, 30, 13)},
        {"fixed_len", make_fixed_length_strings(20, static_cast<int>(MAX_TOKEN_SIZE))},
    };

    for (const auto& c : corpora) {
        auto result = train_strings(c.strings, cfg);
        for (size_t i = 0; i < result.dict.num_tokens(); ++i) {
            const size_t len = result.dict.offsets[i + 1] - result.dict.offsets[i];
            EXPECT_GT(len, 0u)
                << "corpus=" << c.name << " token " << i << " has zero length";
        }
    }
}

// ── DynamicThreshold ──────────────────────────────────────────────────────────

TEST(TrainerTest, DynamicThresholdProducesMergedTokens) {
    // With enough repetitive input, the dynamic threshold should still create
    // merged tokens (> 256 base tokens).
    TrainingConfig cfg;
    cfg.threshold = DynamicThreshold{0.5};
    cfg.seed = 42;
    auto result = train_strings(make_user_strings(200), cfg);
    EXPECT_GT(result.dict.num_tokens(), 256u);
}

TEST(TrainerTest, DynamicThresholdDoesNotExceedCapacity) {
    TrainingConfig cfg;
    cfg.bits      = 12;  // max 4096 tokens
    cfg.threshold = DynamicThreshold{1.0};
    cfg.seed      = 42;
    auto result   = train_strings(make_user_strings(500), cfg);
    EXPECT_LE(result.dict.num_tokens(), max_dict_size(cfg.bits));
}

TEST(TrainerTest, DynamicThresholdSmallerFractionProducesFewerTokens) {
    // A smaller sample_fraction scans less data, so fewer pair merges happen.
    auto corpus = make_user_strings(500);

    TrainingConfig cfg_small;
    cfg_small.threshold = DynamicThreshold{0.05};
    cfg_small.bits      = 14;
    cfg_small.seed      = 42;

    TrainingConfig cfg_large;
    cfg_large.threshold = DynamicThreshold{1.0};
    cfg_large.bits      = 14;
    cfg_large.seed      = 42;

    auto r_small = train_strings(corpus, cfg_small);
    auto r_large = train_strings(corpus, cfg_large);

    // With the same corpus and seed, scanning more data yields strictly more
    // or equal tokens.  The small fraction should produce ≤ the large.
    EXPECT_LE(r_small.dict.num_tokens(), r_large.dict.num_tokens());
}

TEST(TrainerTest, DynamicThresholdDictionaryIsSorted) {
    TrainingConfig cfg;
    cfg.threshold = DynamicThreshold{0.3};
    cfg.seed      = 42;
    auto result = train_strings(make_user_strings(100), cfg);
    EXPECT_TRUE(is_lex_sorted(result.dict));
}

// ── Dictionary is padded for decoder ──────────────────────────────────────────

TEST(TrainerTest, DictionaryIsPaddedForDecoder) {
    auto result = train_strings(make_user_strings(50));
    // The decoder unconditionally copies MAX_TOKEN_SIZE bytes starting from
    // each token's offset.  pad_for_decoder() must ensure that reading
    // MAX_TOKEN_SIZE bytes from the last token's start stays within the buffer.
    const size_t last_start = result.dict.offsets[result.dict.offsets.size() - 2];
    EXPECT_GE(result.dict.bytes.size(), last_start + MAX_TOKEN_SIZE);
}

// ── Sorted dictionary: no duplicate tokens ────────────────────────────────────

TEST(TrainerTest, NoDuplicateTokensInDictionary) {
    auto result = train_strings(make_user_strings(100));
    const size_t n = result.dict.num_tokens();
    for (size_t i = 1; i < n; ++i) {
        const uint8_t* a  = result.dict.bytes.data() + result.dict.offsets[i - 1];
        const size_t   la = result.dict.offsets[i] - result.dict.offsets[i - 1];
        const uint8_t* b  = result.dict.bytes.data() + result.dict.offsets[i];
        const size_t   lb = result.dict.offsets[i + 1] - result.dict.offsets[i];
        // In a sorted dictionary, no two adjacent tokens can be identical.
        bool same = (la == lb && std::memcmp(a, b, la) == 0);
        EXPECT_FALSE(same) << "duplicate token at positions " << (i - 1) << " and " << i;
    }
}

// ── Corpus type coverage ──────────────────────────────────────────────────────

TEST(TrainerTest, HomogeneousCorpusProducesMerges) {
    // All-'a' strings: only one possible pair (a,a).  Every adjacent position
    // contributes to its frequency, so merges happen under any low threshold.
    TrainingConfig cfg;
    cfg.threshold = FixedThreshold{2};
    cfg.seed      = 42;
    auto result = train_strings(make_homogeneous_strings(50, 16, 'a'), cfg);
    EXPECT_GT(result.dict.num_tokens(), 256u);
    check_base_tokens(result.dict);
}

TEST(TrainerTest, AlternatingCorpusProducesMerges) {
    // "ababab…" strings: the pair (a,b) repeats at every even position.
    TrainingConfig cfg;
    cfg.threshold = FixedThreshold{2};
    cfg.seed      = 42;
    auto result = train_strings(make_alternating_strings(50, 16), cfg);
    EXPECT_GT(result.dict.num_tokens(), 256u);
    check_base_tokens(result.dict);
}

TEST(TrainerTest, MixedLengthCorpusProducesValidDictionary) {
    TrainingConfig cfg;
    cfg.threshold = FixedThreshold{2};
    cfg.seed      = 42;
    auto result = train_strings(make_mixed_length_strings(200, 64, 7), cfg);
    check_base_tokens(result.dict);
    EXPECT_TRUE(is_lex_sorted(result.dict));
    EXPECT_LE(result.dict.num_tokens(), max_dict_size(cfg.bits));
}

// ── All bit widths produce valid dictionaries ─────────────────────────────────

TEST(TrainerTest, AllBitWidthsProduceValidDictionary) {
    auto corpus = make_user_strings(50);
    for (int b : {9, 10, 11, 12, 13, 14, 15, 16}) {
        TrainingConfig cfg;
        cfg.bits = static_cast<BitWidth>(b);
        cfg.seed = 42;
        auto result = train_strings(corpus, cfg);
        check_base_tokens(result.dict);
        EXPECT_TRUE(is_lex_sorted(result.dict)) << "not sorted for bits=" << b;
        EXPECT_LE(result.dict.num_tokens(), max_dict_size(cfg.bits))
            << "overflow for bits=" << b;
    }
}
