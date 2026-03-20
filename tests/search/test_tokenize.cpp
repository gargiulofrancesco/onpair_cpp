#include <onpair/search/detail/tokenize.h>
#include <onpair/encoding/training/trainer.h>
#include <onpair/core/dictionary.h>
#include <onpair/core/dictionary_view.h>
#include <gtest/gtest.h>
#include "corpus.h"
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace onpair;
using namespace test_helpers;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Train a dictionary from a corpus and return it.
static Dictionary train_dict(const std::vector<std::string>& corpus)
{
    encoding::TrainingConfig cfg;
    cfg.seed = 42;
    auto raw = make_raw(corpus);
    auto result = encoding::train(raw.data.data(), raw.offsets.data(), raw.n, cfg);
    return std::move(result.dict);
}

// Concatenate the byte sequences of a token vector using the dictionary.
static std::string reconstruct(const std::vector<Token>& tokens,
                                DictionaryView dv)
{
    std::string out;
    for (Token t : tokens) {
        const uint8_t* data = dv.data(t);
        const size_t   len  = dv.token_size(t);
        out.append(reinterpret_cast<const char*>(data), len);
    }
    return out;
}

// ── Empty input ───────────────────────────────────────────────────────────────

TEST(TokenizeTest, EmptyStringReturnsNoTokens) {
    auto dict = train_dict(make_user_strings(10));
    DictionaryView dv(dict);
    auto tokens = search::detail::tokenize("", dv);
    EXPECT_TRUE(tokens.empty());
}

// ── Single byte ───────────────────────────────────────────────────────────────

TEST(TokenizeTest, SingleByteProducesOneToken) {
    auto dict = train_dict(make_user_strings(10));
    DictionaryView dv(dict);
    auto tokens = search::detail::tokenize("x", dv);
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(reconstruct(tokens, dv), "x");
}

// ── Reconstruction correctness ────────────────────────────────────────────────

TEST(TokenizeTest, ReconstructionMatchesInput) {
    auto corpus = make_user_strings(50);
    auto dict = train_dict(corpus);
    DictionaryView dv(dict);

    // Tokenize every string in the corpus and verify reconstruction.
    for (const auto& s : corpus) {
        auto tokens = search::detail::tokenize(s, dv);
        EXPECT_EQ(reconstruct(tokens, dv), s)
            << "reconstruction failed for: " << s;
    }
}

TEST(TokenizeTest, ReconstructionWithRandomStrings) {
    auto corpus = make_random_strings(100, 50, 77);
    auto dict = train_dict(corpus);
    DictionaryView dv(dict);

    // Also test strings not in the training corpus.
    auto test_strings = make_random_strings(20, 40, 999);
    for (const auto& s : test_strings) {
        auto tokens = search::detail::tokenize(s, dv);
        EXPECT_EQ(reconstruct(tokens, dv), s)
            << "reconstruction failed for unseen string";
    }
}

TEST(TokenizeTest, ReconstructionWithBinaryStrings) {
    auto corpus = make_binary_strings(50, 30, 13);
    auto dict = train_dict(corpus);
    DictionaryView dv(dict);

    for (const auto& s : corpus) {
        auto tokens = search::detail::tokenize(s, dv);
        EXPECT_EQ(reconstruct(tokens, dv), s);
    }
}

// ── Greedy longest match ──────────────────────────────────────────────────────

TEST(TokenizeTest, GreedyLongestMatchProducesFewerTokens) {
    // With a trained dictionary, multi-byte tokens should be used, producing
    // fewer tokens than the string length.
    std::vector<std::string> corpus(100, "aabb");  // highly repetitive
    encoding::TrainingConfig cfg;
    cfg.threshold = encoding::FixedThreshold{2};
    cfg.seed = 42;
    auto raw = make_raw(corpus);
    auto result = encoding::train(raw.data.data(), raw.offsets.data(), raw.n, cfg);
    DictionaryView dv(result.dict);

    auto tokens = search::detail::tokenize("aabb", dv);
    // "aabb" is 4 bytes; merged tokens should compress this.
    EXPECT_LT(tokens.size(), 4u)
        << "greedy tokenization should use multi-byte tokens";
    EXPECT_EQ(reconstruct(tokens, dv), "aabb");
}

// ── All 256 single-byte values can be tokenized ──────────────────────────────

TEST(TokenizeTest, All256BytesCoveredViaBaseTokens) {
    auto dict = train_dict(make_single_byte_strings());
    DictionaryView dv(dict);

    for (int b = 0; b < 256; ++b) {
        std::string s(1, static_cast<char>(b));
        auto tokens = search::detail::tokenize(s, dv);
        ASSERT_EQ(tokens.size(), 1u) << "byte " << b << " not tokenized";
        EXPECT_EQ(reconstruct(tokens, dv), s) << "byte " << b << " mismatch";
    }
}

// ── Token count is bounded ───────────────────────────────────────────────────

TEST(TokenizeTest, TokenCountNeverExceedsStringLength) {
    auto corpus = make_user_strings(50);
    auto dict = train_dict(corpus);
    DictionaryView dv(dict);

    for (const auto& s : corpus) {
        auto tokens = search::detail::tokenize(s, dv);
        EXPECT_LE(tokens.size(), s.size())
            << "more tokens than bytes for: " << s;
    }
}

// ── Consistency with parser ──────────────────────────────────────────────────

TEST(TokenizeTest, TokenizeMatchesParserOutput) {
    // detail::tokenize uses prefix_range (binary search on sorted dict),
    // while the parser uses LPM (hash map). Both should produce the same
    // token sequence for the same input and dictionary.
    auto corpus = make_user_strings(50);
    encoding::TrainingConfig cfg;
    cfg.seed = 42;
    auto raw = make_raw(corpus);
    auto trained = encoding::train(raw.data.data(), raw.offsets.data(), raw.n, cfg);
    DictionaryView dv(trained.dict);

    for (const auto& s : corpus) {
        // Tokenize via detail::tokenize (binary search).
        auto bs_tokens = search::detail::tokenize(s, dv);

        // Tokenize via LPM (hash map).
        std::vector<Token> lpm_tokens;
        const auto* data = reinterpret_cast<const uint8_t*>(s.data());
        size_t pos = 0;
        while (pos < s.size()) {
            auto m = trained.lpm.find_longest_match(data + pos, s.size() - pos);
            lpm_tokens.push_back(m.first);
            pos += m.second;
        }

        EXPECT_EQ(bs_tokens, lpm_tokens)
            << "tokenize and LPM disagree for: " << s;
    }
}
