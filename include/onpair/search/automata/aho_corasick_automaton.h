#pragma once
#include <onpair/search/automata/token_automaton.h>
#include <onpair/core/dictionary_view.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <queue>
#include <span>
#include <string_view>
#include <vector>

namespace onpair::search {

// ─────────────────────────────────────────────────────────────────────────────
// AhoCorasickAutomaton
// ─────────────────────────────────────────────────────────────────────────────
// Token-level Aho-Corasick automaton for multi-pattern substring search.
// Answers: "does this string contain ANY of the given patterns?"
//
// Construction (off hot path):
//   1. Build a byte-level Aho-Corasick DFA (trie + failure links + goto).
//   2. Base pass:   for each dictionary token t, run the AC DFA from state 0
//                   through t's bytes.  Record packed exit state.
//   3. Sparse pass: for each non-zero AC state j, traverse the dictionary's
//                   implicit trie tracking two AC evolutions in parallel
//                   (from j and from 0), pruning when they converge.  Store
//                   exceptions as sorted range vectors.
//
// Packed state format: bit 15 = hit (pattern found), bits 0-14 = AC state.
// Once hit, the state bits are don't-care (is_dead() short-circuits).

class AhoCorasickAutomaton {
public:
    // Constructs from a set of patterns and any DictionaryView.
    // Empty patterns cause every string to match (root is accepting).
    AhoCorasickAutomaton(std::span<const std::string_view> patterns,
                         DictionaryView dict);

    // ── TokenAutomaton / DeadDetectable interface ───────────────────────────
    void step(Token t) noexcept {
        if (hit_) return;

        if (state_ > 0) {
            const auto* r   = sparse_.data() + offsets_[state_];
            const auto* end = sparse_.data() + offsets_[state_ + 1];
            for (; r != end; ++r) {
                if (t < r->range.begin) break;
                if (t <= r->range.last) {
                    hit_   = packed_is_hit(r->packed);
                    state_ = packed_state (r->packed);
                    return;
                }
            }
        }
        uint16_t p = base_[t];
        hit_   = packed_is_hit(p);
        state_ = packed_state (p);
    }

    bool is_accepted() const noexcept { return hit_; }
    void reset()       noexcept       { state_ = 0; hit_ = all_match_; }
    // is_dead() returns true once any pattern has been found.
    bool is_dead()     const noexcept { return hit_; }

    // ── Accessors ───────────────────────────────────────────────────────────
    size_t num_patterns()      const noexcept { return num_patterns_; }
    size_t num_ac_states()     const noexcept { return num_ac_states_; }
    size_t sparse_range_count() const noexcept { return sparse_.size(); }

private:
    static constexpr uint16_t HIT        = 0x8000;
    static constexpr uint16_t STATE_MASK = static_cast<uint16_t>(~HIT);
    static constexpr uint16_t NO_CHILD   = 0xFFFF;  // sentinel: no trie edge

    static constexpr bool     packed_is_hit(uint16_t p) noexcept { return p & HIT; }
    static constexpr uint16_t packed_state (uint16_t p) noexcept { return p & STATE_MASK; }

    // Sparse transition: tokens in [range.begin, range.last] map to `packed`.
    struct SparseTransition {
        TokenRange range;
        uint16_t   packed;   // bit 15 = hit, bits 0-14 = AC state
    };

    uint16_t state_     = 0;
    bool     hit_       = false;
    bool     all_match_ = false;  // true if root is accepting (empty pattern)

    size_t num_patterns_  = 0;
    size_t num_ac_states_ = 0;

    // base_[token] = packed transition from AC state 0.
    std::vector<uint16_t> base_;

    // Flattened sparse transitions grouped by AC state.
    // Transitions for state s live at sparse_[offsets_[s] .. offsets_[s+1]).
    std::vector<SparseTransition> sparse_;
    std::vector<uint32_t>         offsets_;
};

// ─── Implementation ─────────────────────────────────────────────────────────

inline AhoCorasickAutomaton::AhoCorasickAutomaton(
    std::span<const std::string_view> patterns,
    DictionaryView dict)
{
    num_patterns_ = patterns.size();
    const size_t num_tokens = dict.num_tokens();

    // ── 1. Build trie ───────────────────────────────────────────────────────
    std::vector<std::vector<std::pair<uint8_t, uint16_t>>> children(1);
    std::vector<bool> is_accepting(1, false);

    // Returns the direct trie child of u for byte c, or NO_CHILD if none.
    auto find_child = [](const std::vector<std::pair<uint8_t, uint16_t>>& ch,
                         uint8_t c) -> uint16_t {
        for (auto& [k, v] : ch)
            if (k == c) return v;
        return NO_CHILD;
    };

    for (const auto& pat : patterns) {
        if (pat.empty()) {
            is_accepting[0] = true;  // empty pattern → root accepts
            continue;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(pat.data());
        uint16_t cur = 0;
        for (size_t i = 0; i < pat.size(); ++i) {
            uint16_t next = find_child(children[cur], p[i]);
            if (next == NO_CHILD) {
                next = static_cast<uint16_t>(children.size());
                children[cur].emplace_back(p[i], next);
                children.emplace_back();
                is_accepting.push_back(false);
            }
            cur = next;
        }
        is_accepting[cur] = true;
    }

    const size_t num_states = children.size();
    num_ac_states_ = num_states;

    all_match_ = is_accepting[0];
    hit_ = all_match_;

    // ── 2. Compute failure links via BFS ────────────────────────────────────
    std::vector<uint16_t> fail(num_states, 0);

    // goto_fn(u, c): complete AC transition — follows failure links until a
    // real trie edge is found, or returns 0 (root self-loop) if none exists.
    // O(depth) per call, depth ≤ max pattern length.
    auto goto_fn = [&](uint16_t u, uint8_t c) -> uint16_t {
        while (true) {
            uint16_t v = find_child(children[u], c);
            if (v != NO_CHILD) return v;
            if (u == 0) return 0;
            u = fail[u];
        }
    };

    std::queue<uint16_t> bfs;
    for (auto& [c, child] : children[0]) {
        fail[child] = 0;
        bfs.push(child);
    }
    while (!bfs.empty()) {
        uint16_t u = bfs.front(); bfs.pop();
        if (is_accepting[fail[u]])
            is_accepting[u] = true;  // propagate accepting through suffix links
        for (auto& [c, child] : children[u]) {
            fail[child] = goto_fn(fail[u], c);
            bfs.push(child);
        }
    }

    // ── 3. Helper: evolve a packed state through one byte ───────────────────
    // Packed format: bit 15 = hit (absorbing), bits 0-14 = AC state.
    auto evolve = [&](uint16_t packed, uint8_t c) -> uint16_t {
        if (packed_is_hit(packed)) return HIT;
        uint16_t next = goto_fn(packed_state(packed), c);
        return is_accepting[next] ? HIT : next;
    };

    // Run AC DFA through a token's bytes, returning packed result.
    auto run_ac = [&](uint16_t start_state, const uint8_t* data,
                      size_t len) -> uint16_t {
        uint16_t state = start_state;
        for (size_t i = 0; i < len; ++i) {
            state = goto_fn(state, data[i]);
            if (is_accepting[state]) return HIT;
        }
        return state;
    };

    // ── 4. Base pass: transitions from state 0 ─────────────────────────────
    base_.resize(num_tokens);
    for (size_t t = 0; t < num_tokens; ++t) {
        const auto tid = static_cast<Token>(t);
        uint16_t packed = run_ac(0, dict.data(tid), dict.token_size(tid));
        if (all_match_) packed = HIT;
        base_[t] = packed;
    }

    // ── 5. Sparse pass — dual-AC trie traversal ────────────────────────────
    //
    // For each AC state j > 0, traverse the dictionary's implicit trie
    // tracking two packed AC states in parallel:
    //   packed_j = state evolved from entry state j
    //   packed_0 = state evolved from state 0
    //
    // Pruning: when packed_j == packed_0, the subtree produces no exceptions.
    // Ranges are merged on-the-fly since tokens are visited in sorted order.

    if (all_match_) {
        // Root is accepting → every token from every state is a hit.
        // No sparse entries needed.
        offsets_.assign(num_states + 1, 0);
        return;
    }

    offsets_.resize(num_states + 1, 0);
    size_t range_start = 0;

    // Extend last transition or push a new one.
    auto emit = [&](TokenRange range, uint16_t packed) {
        if (sparse_.size() > range_start) {
            auto& last = sparse_.back();
            if (last.packed == packed && last.range.last + 1 == range.begin) {
                last.range.last = range.last;
                return;
            }
        }
        sparse_.push_back({range, packed});
    };

    auto traverse = [&](auto& self, TokenRange tr, size_t depth,
                        uint16_t packed_j, uint16_t packed_0) -> void {
        if (packed_j == packed_0 || tr.empty()) return;

        // Absorbing hit: all tokens from this state produce hit.
        // Emit only where base_[t] differs (doesn't have hit).
        if (packed_j & HIT) {
            Token i = tr.begin;
            while (i <= tr.last) {
                if (!(base_[i] & HIT)) {
                    Token start = i;
                    while (i <= tr.last && !(base_[i] & HIT)) ++i;
                    emit({start, static_cast<Token>(i - 1)}, HIT);
                } else {
                    ++i;
                }
            }
            return;
        }

        // Leaf tokens (length == depth) all share exit state packed_j.
        Token cur = tr.begin;
        while (cur <= tr.last && dict.token_size(cur) == depth)
            ++cur;
        if (cur > tr.begin)
            emit({tr.begin, static_cast<Token>(cur - 1)}, packed_j);
        if (cur > tr.last) return;

        // Recurse into subtrees partitioned by byte at `depth`.
        while (cur <= tr.last) {
            uint8_t c = dict.data(cur)[depth];
            Token sub_hi = cur;
            while (sub_hi < tr.last &&
                   dict.data(static_cast<Token>(sub_hi + 1))[depth] == c)
                ++sub_hi;

            self(self, {cur, sub_hi}, depth + 1,
                 evolve(packed_j, c), evolve(packed_0, c));

            cur = static_cast<Token>(sub_hi + 1);
        }
    };

    std::vector<uint8_t> relevant_chars;

    for (size_t j = 1; j < num_states; ++j) {
        range_start = sparse_.size();
        offsets_[j] = static_cast<uint32_t>(range_start);

        // Collect byte labels of trie children along the failure chain from j
        // (excluding root). These are the only bytes where goto_fn(j, c) can
        // differ from goto_fn(0, c): any other byte falls through to the root
        // in both cases and returns the same result.
        relevant_chars.clear();
        {
            uint16_t u = static_cast<uint16_t>(j);
            while (u > 0) {
                for (auto& [c, _] : children[u])
                    relevant_chars.push_back(c);
                u = fail[u];
            }
        }

        // Deduplicate.
        std::sort(relevant_chars.begin(), relevant_chars.end());
        relevant_chars.erase(
            std::unique(relevant_chars.begin(), relevant_chars.end()),
            relevant_chars.end());

        for (uint8_t byte : relevant_chars) {
            if (goto_fn(static_cast<uint16_t>(j), byte) == goto_fn(0, byte)) continue;
            TokenRange range = dict.prefix_range(&byte, 1);
            if (range.empty()) continue;

            traverse(traverse, range, 1,
                     evolve(static_cast<uint16_t>(j), byte),
                     evolve(0, byte));
        }
    }

    offsets_[num_states] = static_cast<uint32_t>(sparse_.size());
}

} // namespace onpair::search
