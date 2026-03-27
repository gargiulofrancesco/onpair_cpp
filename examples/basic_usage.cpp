#include <onpair/api.h>
#include <onpair/search/automata/kmp_automaton.h>
#include <onpair/search/automata/aho_corasick_automaton.h>
#include <onpair/search/automata/eq_automaton.h>
#include <onpair/search/automata/prefix_automaton.h>
#include <iostream>
#include <string_view>
#include <vector>

int main() {
    // ── 1. Prepare input ──────────────────────────────────────────────────────
    // OnPair accepts any range whose elements convert to string_view:
    // vector<string>, vector<string_view>, span<string_view>, custom ranges…
    const std::vector<std::string_view> strings = {
        "user_000001", "user_000002", "user_000003",
        "admin_001",   "user_000004", "user_000005",
        "guest_001",   "user_000006", "admin_002",
        "user_000007",
    };

    // ── 2. Compress ───────────────────────────────────────────────────────────
    // DynamicThreshold fills the dictionary without requiring a hand-tuned value.
    // The dictionary is always sorted, enabling optimised search operations.
    onpair::encoding::TrainingConfig cfg;
    cfg.bits      = 14;   // 2^14 = 16 384 tokens, 1.75 bytes/token in stream
    cfg.threshold = onpair::encoding::DynamicThreshold{1.0};
    cfg.seed      = 42;   // pin the RNG for reproducible dictionaries

    onpair::OnPairColumn col = onpair::OnPairColumn::compress(strings, cfg);
    auto view = col.view(); // OnPairColumnView provides non-owning access.

    std::cout << "Compressed " << col.num_strings() << " strings into "
              << col.bytes_used() << " bytes ("
              << int(col.bits()) << " bits/token)\n\n";

    // ── 3. Random access ──────────────────────────────────────────────────────
    // Buffer must have DECOMPRESS_BUFFER_PADDING bytes of headroom beyond the
    // longest expected string (over-copy optimisation in the decoder).
    std::vector<char> buf(256 + onpair::DECOMPRESS_BUFFER_PADDING);

    std::cout << "All strings:\n";
    for (size_t i = 0; i < col.num_strings(); ++i) {
        const size_t len = view.decompress(i, buf.data());
        std::cout << "  [" << i << "] \""
                  << std::string_view(buf.data(), len) << "\"\n";
    }

    // ── 4. Substring search ───────────────────────────────────────────────────
    // view.contains() builds a KmpAutomaton internally and scans the token stream.
    std::cout << "\nSubstring search for \"admin\":\n";
    auto admin_hits = view.contains("admin");
    for (size_t idx : admin_hits)
        std::cout << "  [" << idx << "]\n";

    // ── 5. Prefix search ─────────────────────────────────────────────────────
    std::cout << "\nPrefix search for \"user_\":\n";
    auto user_hits = view.starts_with("user_");
    for (size_t idx : user_hits)
        std::cout << "  [" << idx << "]\n";

    // ── 6. Multi-pattern search ──────────────────────────────────────────────
    // Build an Aho-Corasick automaton and scan.
    std::cout << "\nMulti-pattern search for {\"admin\", \"guest\"}:\n";
    std::vector<std::string_view> patterns = {"admin", "guest"};
    onpair::search::AhoCorasickAutomaton ac(patterns, view.dictionary());
    auto multi_hits = view.scan(ac);
    for (size_t idx : multi_hits)
        std::cout << "  [" << idx << "]\n";

    // ── 7. Operator-based combinators ─────────────────────────────────────────
    // Use !, &&, || directly on automata for concise boolean queries.
    onpair::search::KmpAutomaton kmp_user("user", view.dictionary());
    onpair::search::KmpAutomaton kmp_admin("admin", view.dictionary());

    std::cout << "\nNOT contains \"admin\" (operator!):\n";
    for (size_t idx : view.scan(!kmp_admin))
        std::cout << "  [" << idx << "]\n";

    std::cout << "\nContains \"user\" AND NOT \"admin\" (operator&& and !):\n";
    for (size_t idx : view.scan(kmp_user && !kmp_admin))
        std::cout << "  [" << idx << "]\n";

    std::cout << "\nContains \"user\" OR \"admin\" (operator||):\n";
    for (size_t idx : view.scan(kmp_user || kmp_admin))
        std::cout << "  [" << idx << "]\n";

    // ── 8. Composable exact-match and prefix automata ────────────────────────
    // EqAutomaton and PrefixAutomaton satisfy TokenAutomaton, so they compose
    // with all combinators (!, &&, ||) and other automata.
    onpair::search::EqAutomaton eq("admin_001", view.dictionary());
    onpair::search::PrefixAutomaton pa("user_", view.dictionary());

    std::cout << "\nExact match \"admin_001\" via EqAutomaton:\n";
    for (size_t idx : view.scan(eq))
        std::cout << "  [" << idx << "]\n";

    std::cout << "\nStarts with \"user_\" OR exact \"admin_001\":\n";
    for (size_t idx : view.scan(pa || eq))
        std::cout << "  [" << idx << "]\n";

    std::cout << "\nStarts with \"user_\" AND contains \"0000\":\n";
    onpair::search::KmpAutomaton kmp_zeros("0000", view.dictionary());
    for (size_t idx : view.scan(pa && kmp_zeros))
        std::cout << "  [" << idx << "]\n";

    return 0;
}
