#include <onpair/core/store.h>
#include <onpair/core/store_view.h>
#include <gtest/gtest.h>

using namespace onpair;

TEST(StoreViewTest, InheritsMetadataFromStore) {
    Store s;
    s.bit_width  = 14;
    s.packed     = {1ull, 2ull};
    s.boundaries = {0, 5, 10};

    StoreView v(s);
    EXPECT_EQ(v.bits(),        14);
    EXPECT_EQ(v.num_strings(), 2u);
    EXPECT_EQ(v.num_tokens(),  10u);
    EXPECT_EQ(v.bytes_used(),  s.bytes_used());
}

TEST(StoreViewTest, StringSpanReturnsCorrectRange) {
    Store s;
    s.bit_width  = 16;
    s.boundaries = {0, 3, 7, 10};

    StoreView v(s);
    EXPECT_EQ(v.string_span(0).begin, 0u);
    EXPECT_EQ(v.string_span(0).end,   3u);
    EXPECT_EQ(v.string_span(1).begin, 3u);
    EXPECT_EQ(v.string_span(1).end,   7u);
    EXPECT_EQ(v.string_span(2).begin, 7u);
    EXPECT_EQ(v.string_span(2).end,  10u);
}

TEST(StoreViewTest, RawPointersPointIntoStore) {
    Store s;
    s.bit_width  = 16;
    s.packed     = {0xABCDull};
    s.boundaries = {0, 1};

    StoreView v(s);
    EXPECT_EQ(v.packed_data(), s.packed.data());
    EXPECT_EQ(v.boundaries(),  s.boundaries.data());
}

TEST(StoreViewTest, EmptyStoreView) {
    Store s;
    s.bit_width = 12;

    StoreView v(s);
    EXPECT_EQ(v.num_strings(), 0u);
    EXPECT_EQ(v.num_tokens(),  0u);
    EXPECT_EQ(v.bytes_used(),  0u);
}
