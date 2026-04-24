#include <onpair/api.h>
#include <onpair/search/automata/token_automaton.h>
#include <onpair/search/automata/kmp_automaton.h>
#include <onpair/search/automata/eq_automaton.h>
#include <onpair/search/automata/prefix_automaton.h>
#include <gtest/gtest.h>
#include "corpus.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace op = onpair;
namespace search = onpair::search;
using namespace test_helpers;

static op::OnPairColumn make_column(const std::vector<std::string>& strings,
                                    op::BitWidth bits = 14)
{
    op::encoding::TrainingConfig cfg;
    cfg.bits = bits;
    cfg.seed = 42;
    return op::OnPairColumn::compress(strings, cfg);
}

static std::vector<size_t> complement(const std::vector<size_t>& matches, size_t n)
{
    std::vector<size_t> result;
    for (size_t i = 0; i < n; ++i)
        if (!std::binary_search(matches.begin(), matches.end(), i))
            result.push_back(i);
    return result;
}

// ─── NegatedAutomaton ─────────────────────────────────────────────────────────

TEST(NegatedAutomaton, ComplementOfKmp) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "guest_001", "admin_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp("admin", v.dictionary());
    auto matches = v.contains("admin");
    search::NegatedAutomaton neg{kmp};
    auto not_matches = v.scan(neg);

    EXPECT_EQ(not_matches, complement(matches, data.size()));

    // Partition: union == all, intersection == empty
    std::vector<size_t> all(data.size());
    std::iota(all.begin(), all.end(), 0);
    std::vector<size_t> uni, inter;
    std::set_union(matches.begin(), matches.end(),
                   not_matches.begin(), not_matches.end(), std::back_inserter(uni));
    std::set_intersection(matches.begin(), matches.end(),
                          not_matches.begin(), not_matches.end(), std::back_inserter(inter));
    EXPECT_EQ(uni, all);
    EXPECT_TRUE(inter.empty());
}

TEST(NegatedAutomaton, DirectConstructionMatchesScan) {
    std::vector<std::string> data = {"foo", "bar", "foobar", "baz"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp("foo", v.dictionary());
    search::NegatedAutomaton neg{kmp};
    auto result = v.scan(neg);

    auto matches = v.contains("foo");
    EXPECT_EQ(result, complement(matches, data.size()));
}

// ─── AndAutomaton ─────────────────────────────────────────────────────────────

TEST(AndAutomaton, IntersectionOfTwoKmp) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_admin", "guest_001", "admin_user_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_user ("user",  v.dictionary());
    search::KmpAutomaton kmp_admin("admin", v.dictionary());
    search::AndAutomaton and_aut{kmp_user, kmp_admin};
    auto result = v.scan(and_aut);

    auto has_user  = v.contains("user");
    auto has_admin = v.contains("admin");
    std::vector<size_t> expected;
    std::set_intersection(has_user.begin(), has_user.end(),
                          has_admin.begin(), has_admin.end(),
                          std::back_inserter(expected));
    EXPECT_EQ(result, expected);
}

TEST(AndAutomaton, EmptyIntersectionReturnsEmpty) {
    std::vector<std::string> data = {"abc", "def", "ghi"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_x("x", v.dictionary());
    search::KmpAutomaton kmp_y("y", v.dictionary());
    search::AndAutomaton and_aut{kmp_x, kmp_y};
    EXPECT_TRUE(v.scan(and_aut).empty());
}

TEST(AndAutomaton, FullIntersectionReturnsAll) {
    std::vector<std::string> data = {"abc", "xabc", "abcy"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp1("a", v.dictionary());
    search::KmpAutomaton kmp2("b", v.dictionary());
    search::AndAutomaton and_aut{kmp1, kmp2};
    auto result = v.scan(and_aut);
    std::vector<size_t> expected = {0, 1, 2};
    EXPECT_EQ(result, expected);
}

// ─── OrAutomaton ──────────────────────────────────────────────────────────────

TEST(OrAutomaton, UnionOfTwoKmp) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "guest_001", "admin_user_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_user ("user",  v.dictionary());
    search::KmpAutomaton kmp_admin("admin", v.dictionary());
    search::OrAutomaton or_aut{kmp_user, kmp_admin};
    auto result = v.scan(or_aut);

    auto has_user  = v.contains("user");
    auto has_admin = v.contains("admin");
    std::vector<size_t> expected;
    std::set_union(has_user.begin(), has_user.end(),
                   has_admin.begin(), has_admin.end(),
                   std::back_inserter(expected));
    EXPECT_EQ(result, expected);
}

TEST(OrAutomaton, EmptyUnionReturnsEmpty) {
    std::vector<std::string> data = {"abc", "def"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_x("x", v.dictionary());
    search::KmpAutomaton kmp_y("y", v.dictionary());
    search::OrAutomaton or_aut{kmp_x, kmp_y};
    EXPECT_TRUE(v.scan(or_aut).empty());
}

TEST(OrAutomaton, DirectConstructionMatchesScan) {
    std::vector<std::string> data = {"foo", "bar", "foobar", "baz"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_a("foo", v.dictionary());
    search::KmpAutomaton kmp_b("bar", v.dictionary());
    search::OrAutomaton or_aut{kmp_a, kmp_b};
    auto result = v.scan(or_aut);

    auto has_foo = v.contains("foo");
    auto has_bar = v.contains("bar");
    std::vector<size_t> expected;
    std::set_union(has_foo.begin(), has_foo.end(),
                   has_bar.begin(), has_bar.end(),
                   std::back_inserter(expected));
    EXPECT_EQ(result, expected);
}

// ─── Operator overloads ──────────────────────────────────────────────────────

TEST(OperatorOverloads, NotOperator) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "guest_001", "admin_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp("admin", v.dictionary());
    auto matches     = v.contains("admin");
    auto not_matches = v.scan(!kmp);

    EXPECT_EQ(not_matches, complement(matches, data.size()));
}

TEST(OperatorOverloads, AndOperator) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_admin", "guest_001", "admin_user_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_user ("user",  v.dictionary());
    search::KmpAutomaton kmp_admin("admin", v.dictionary());
    auto result = v.scan(kmp_user && kmp_admin);

    auto has_user  = v.contains("user");
    auto has_admin = v.contains("admin");
    std::vector<size_t> expected;
    std::set_intersection(has_user.begin(), has_user.end(),
                          has_admin.begin(), has_admin.end(),
                          std::back_inserter(expected));
    EXPECT_EQ(result, expected);
}

TEST(OperatorOverloads, OrOperator) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "guest_001", "admin_user_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_user ("user",  v.dictionary());
    search::KmpAutomaton kmp_admin("admin", v.dictionary());
    auto result = v.scan(kmp_user || kmp_admin);

    auto has_user  = v.contains("user");
    auto has_admin = v.contains("admin");
    std::vector<size_t> expected;
    std::set_union(has_user.begin(), has_user.end(),
                   has_admin.begin(), has_admin.end(),
                   std::back_inserter(expected));
    EXPECT_EQ(result, expected);
}

TEST(OperatorOverloads, NestedExpression) {
    // contains "user" AND NOT contains "admin"
    std::vector<std::string> data = {
        "user_only", "admin_only", "user_admin", "neither"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_u("user",  v.dictionary());
    search::KmpAutomaton kmp_a("admin", v.dictionary());
    auto result = v.scan(kmp_u && !kmp_a);

    // Only "user_only" (index 0) should match
    EXPECT_EQ(result, std::vector<size_t>{0});
}

TEST(OperatorOverloads, OperatorsMatchStructSyntax) {
    std::vector<std::string> data = {
        "user_admin_001", "user_001", "admin_001", "guest_001"
    };
    auto col = make_column(data);
    auto v = col.view();

    // Struct syntax
    search::KmpAutomaton kmp_u1("user",  v.dictionary());
    search::KmpAutomaton kmp_a1("admin", v.dictionary());
    search::AndAutomaton and_aut{kmp_u1, kmp_a1};
    auto struct_result = v.scan(and_aut);

    // Operator syntax
    search::KmpAutomaton kmp_u2("user",  v.dictionary());
    search::KmpAutomaton kmp_a2("admin", v.dictionary());
    auto op_result = v.scan(kmp_u2 && kmp_a2);

    EXPECT_EQ(struct_result, op_result);
}

// ─── Nested combinators ───────────────────────────────────────────────────────

TEST(Combinators, NegatedAnd) {
    std::vector<std::string> data = {
        "user_only", "admin_only", "user_admin", "neither"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp_u("user",  v.dictionary());
    search::KmpAutomaton kmp_a("admin", v.dictionary());
    search::AndAutomaton both{kmp_u, kmp_a};
    search::NegatedAutomaton not_both{both};
    auto result = v.scan(not_both);

    search::KmpAutomaton ku2("user",  v.dictionary());
    search::KmpAutomaton ka2("admin", v.dictionary());
    search::AndAutomaton both2{ku2, ka2};
    auto and_result = v.scan(both2);
    EXPECT_EQ(result, complement(and_result, data.size()));
}

// ─── Aliasing precondition ────────────────────────────────────────────────────
// Combinators hold references.  Using the *same* automaton object in both
// branches of And/Or violates the distinct-objects precondition: both sides
// are stepped on every token, so the shared state is advanced twice.
// These tests document the pitfall and verify correct behaviour when the
// precondition is satisfied (distinct objects with the same pattern).

TEST(Combinators, AliasedAndProducesWrongResult) {
    // And(kmp, kmp) with the SAME object — should behave like a single kmp,
    // but aliasing double-steps the automaton on every token, corrupting the
    // KMP state machine and producing wrong results.
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_admin", "guest_001", "admin_user_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    // Correct: two distinct objects with the same pattern
    search::KmpAutomaton kmp1("admin", v.dictionary());
    search::KmpAutomaton kmp2("admin", v.dictionary());
    search::AndAutomaton correct_and{kmp1, kmp2};
    auto correct = v.scan(correct_and);

    // Aliased: same object used in both branches → double-stepped
    search::KmpAutomaton kmp_shared("admin", v.dictionary());
    search::AndAutomaton aliased{kmp_shared, kmp_shared};
    auto aliased_result = v.scan(aliased);

    EXPECT_FALSE(correct.empty());
    EXPECT_NE(aliased_result, correct)
        << "aliased And unexpectedly produced the correct result; "
           "if this triggers, the test assumptions may need updating";
}

TEST(Combinators, DistinctObjectsSamePatternIsCorrect) {
    // Same logical query (kmp && !kmp) but with distinct automaton objects.
    std::vector<std::string> data = {"foo", "bar", "foobar", "baz"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp1("foo", v.dictionary());
    search::KmpAutomaton kmp2("foo", v.dictionary());
    auto result = v.scan(kmp1 && !kmp2);

    EXPECT_TRUE(result.empty());
}

TEST(Combinators, AndAcrossBitWidths) {
    std::vector<std::string> data = {
        "user_admin_001", "user_001", "admin_001", "guest_001"
    };
    for (int b : {9, 10, 11, 12, 13, 14, 15, 16}) {
        op::encoding::TrainingConfig cfg;
        cfg.bits = static_cast<op::BitWidth>(b);
        cfg.seed = 42;
        auto col = op::OnPairColumn::compress(data, cfg);
        auto v   = col.view();

        search::KmpAutomaton kmp_u("user",  v.dictionary());
        search::KmpAutomaton kmp_a("admin", v.dictionary());
        search::AndAutomaton and_aut{kmp_u, kmp_a};
        auto result = v.scan(and_aut);

        EXPECT_EQ(result.size(), 1u) << "bit-width " << b;
        EXPECT_EQ(result[0], 0u)     << "bit-width " << b;
    }
}

// ─── EqAutomaton combinators ──────────────────────────────────────────────────

TEST(Combinators, NegatedEq) {
    std::vector<std::string> data = {"abc", "def", "ghi", "abc"};
    auto col = make_column(data);
    auto v = col.view();

    search::EqAutomaton eq("abc", v.dictionary());
    auto matches = v.scan(eq);
    auto not_matches = v.scan(!eq);

    EXPECT_EQ(not_matches, complement(matches, data.size()));
}

TEST(Combinators, EqAndKmp) {
    // exact match "admin_001" AND contains "admin"
    std::vector<std::string> data = {
        "admin_001", "admin_002", "user_001", "admin_001"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::EqAutomaton eq("admin_001", v.dictionary());
    search::KmpAutomaton kmp("admin", v.dictionary());
    auto result = v.scan(eq && kmp);

    // Only the exact matches for "admin_001" (which also contain "admin")
    EXPECT_EQ(result, (std::vector<size_t>{0, 3}));
}

TEST(Combinators, EqOrEq) {
    std::vector<std::string> data = {"abc", "def", "ghi", "abc", "def"};
    auto col = make_column(data);
    auto v = col.view();

    search::EqAutomaton eq1("abc", v.dictionary());
    search::EqAutomaton eq2("def", v.dictionary());
    auto result = v.scan(eq1 || eq2);

    EXPECT_EQ(result, (std::vector<size_t>{0, 1, 3, 4}));
}

TEST(Combinators, ContainsButNotExact) {
    // contains "abc" AND NOT exact "abc" → superstrings only
    std::vector<std::string> data = {"abc", "abcd", "xabc", "ab"};
    auto col = make_column(data);
    auto v = col.view();

    search::KmpAutomaton kmp("abc", v.dictionary());
    search::EqAutomaton eq("abc", v.dictionary());
    auto result = v.scan(kmp && !eq);

    EXPECT_EQ(result, (std::vector<size_t>{1, 2}));
}

// ─── PrefixAutomaton combinators ──────────────────────────────────────────────

TEST(Combinators, NegatedPrefix) {
    std::vector<std::string> data = {
        "user_001", "admin_001", "user_002", "guest_001"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::PrefixAutomaton pa("user_", v.dictionary());
    auto matches = v.scan(pa);
    auto not_matches = v.scan(!pa);

    EXPECT_EQ(not_matches, complement(matches, data.size()));
}

TEST(Combinators, PrefixAndContains) {
    // starts with "user_" AND contains "admin"
    std::vector<std::string> data = {
        "user_001", "user_admin_001", "admin_001", "user_admin"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::PrefixAutomaton pa("user_", v.dictionary());
    search::KmpAutomaton kmp("admin", v.dictionary());
    auto result = v.scan(pa && kmp);

    // "user_admin_001" and "user_admin" start with "user_" and contain "admin"
    EXPECT_EQ(result, (std::vector<size_t>{1, 3}));
}

TEST(Combinators, PrefixOrEq) {
    // starts with "admin" OR exact "guest_001"
    std::vector<std::string> data = {
        "admin_001", "user_001", "guest_001", "admin_002"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::PrefixAutomaton pa("admin", v.dictionary());
    search::EqAutomaton eq("guest_001", v.dictionary());
    auto result = v.scan(pa || eq);

    EXPECT_EQ(result, (std::vector<size_t>{0, 2, 3}));
}

TEST(Combinators, PrefixAndNotPrefix) {
    // starts with "user_" AND NOT starts with "user_000"
    std::vector<std::string> data = {
        "user_001", "user_0001", "user_123", "admin_001"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::PrefixAutomaton pa1("user_", v.dictionary());
    search::PrefixAutomaton pa2("user_000", v.dictionary());
    auto result = v.scan(pa1 && !pa2);

    // "user_001" does NOT start with "user_000", "user_123" does NOT.
    // "user_0001" DOES start with "user_000".
    auto prefix1 = v.scan(pa1);
    auto prefix2 = v.scan(pa2);

    std::vector<size_t> expected;
    for (size_t idx : prefix1)
        if (!std::binary_search(prefix2.begin(), prefix2.end(), idx))
            expected.push_back(idx);
    EXPECT_EQ(result, expected);
}

// ─── Mixed three-way nesting ──────────────────────────────────────────────────

TEST(Combinators, NestedPrefixEqKmp) {
    // (starts with "user_" OR exact "admin_001") AND contains "_00"
    std::vector<std::string> data = {
        "user_001", "user_123", "admin_001", "admin_002", "guest_001"
    };
    auto col = make_column(data);
    auto v = col.view();

    search::PrefixAutomaton pa("user_", v.dictionary());
    search::EqAutomaton eq("admin_001", v.dictionary());
    search::KmpAutomaton kmp("_00", v.dictionary());

    auto result = v.scan((pa || eq) && kmp);

    // user_001 starts with user_ and contains _00 → yes
    // user_123 starts with user_ but does NOT contain _00 → no
    // admin_001 exact match and contains _00 → yes
    // admin_002 neither starts with user_ nor exact admin_001 → no
    // guest_001 → no
    EXPECT_EQ(result, (std::vector<size_t>{0, 2}));
}