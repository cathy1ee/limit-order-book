// FlatIndex tests: hand-picked cases plus a long random run checked against
// std::unordered_map after every operation.
#include "lob/flat_index.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

using lob::FlatIndex;
using lob::OrderId;

namespace {

// Distinct non-null pointers to use as values.
std::vector<int> g_values(1 << 16);
int* value_for(OrderId id) { return &g_values[id % g_values.size()]; }

}  // namespace

TEST(FlatIndex, InsertFindErase) {
    FlatIndex<int> index;
    EXPECT_EQ(index.size(), 0u);
    EXPECT_EQ(index.find(7), nullptr);

    index.insert(7, value_for(7));
    index.insert(0, value_for(0));  // id 0 is a valid key
    EXPECT_EQ(index.find(7), value_for(7));
    EXPECT_EQ(index.find(0), value_for(0));
    EXPECT_TRUE(index.contains(0));
    EXPECT_EQ(index.size(), 2u);

    EXPECT_TRUE(index.erase(7));
    EXPECT_FALSE(index.erase(7));   // already gone
    EXPECT_FALSE(index.erase(99));  // never there
    EXPECT_EQ(index.find(7), nullptr);
    EXPECT_EQ(index.size(), 1u);
}

TEST(FlatIndex, GrowsAndKeepsEveryEntry) {
    FlatIndex<int> index(8);
    for (OrderId id = 1; id <= 100'000; ++id) index.insert(id, value_for(id));
    EXPECT_EQ(index.size(), 100'000u);
    for (OrderId id = 1; id <= 100'000; ++id) ASSERT_EQ(index.find(id), value_for(id));
    EXPECT_EQ(index.find(100'001), nullptr);
}

TEST(FlatIndex, EraseKeepsLaterProbeRunReachable) {
    // Fill a tiny table densely so probe runs are long and wrap around the
    // end, then erase from the middle of the runs.
    FlatIndex<int> index(8);
    std::vector<OrderId> ids;
    for (OrderId id = 1; id <= 64; ++id) {
        index.insert(id * 1'000'003, value_for(id));
        ids.push_back(id * 1'000'003);
    }
    for (std::size_t i = 0; i < ids.size(); i += 2) ASSERT_TRUE(index.erase(ids[i]));
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i % 2 == 0) {
            ASSERT_EQ(index.find(ids[i]), nullptr);
        } else {
            ASSERT_NE(index.find(ids[i]), nullptr) << "lost id " << ids[i];
        }
    }
}

TEST(FlatIndex, MatchesUnorderedMapOnRandomOperations) {
    std::mt19937_64 rng(2024);
    FlatIndex<int> index(8);
    std::unordered_map<OrderId, int*> reference;

    // Keys from a small range so inserts, erases and misses all collide often.
    std::uniform_int_distribution<OrderId> key(0, 4'000);
    std::uniform_int_distribution<int> op(0, 2);
    for (int step = 0; step < 300'000; ++step) {
        const OrderId id = key(rng);
        switch (op(rng)) {
            case 0:  // insert if absent (the book never inserts duplicates)
                if (!reference.count(id)) {
                    index.insert(id, value_for(id));
                    reference.emplace(id, value_for(id));
                }
                break;
            case 1:
                ASSERT_EQ(index.erase(id), reference.erase(id) == 1) << "step " << step;
                break;
            default: {
                auto it = reference.find(id);
                ASSERT_EQ(index.find(id), it == reference.end() ? nullptr : it->second)
                    << "step " << step;
            }
        }
        ASSERT_EQ(index.size(), reference.size()) << "step " << step;
    }
    for (const auto& [id, value] : reference) ASSERT_EQ(index.find(id), value);
}
