// Differential tests: replay long random order flows through the fast
// OrderBook and the v1 MapOrderBook and require identical results after
// every single operation.
#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "lob/order_flow.hpp"

#include <gtest/gtest.h>

namespace {

void expect_same(const lob::FlowConfig& cfg, std::size_t n_ops, std::size_t window) {
    lob::OrderFlow flow(cfg);
    lob::OrderBook fast(window);
    lob::MapOrderBook reference;

    std::size_t trades_seen = 0;
    for (std::size_t i = 0; i < n_ops; ++i) {
        const lob::Op op = flow.next();
        const auto got = lob::apply(fast, op);
        const auto want = lob::apply(reference, op);
        trades_seen += want.size();

        ASSERT_EQ(got, want) << "trades differ at op " << i;
        ASSERT_EQ(fast.best_bid(), reference.best_bid()) << "op " << i;
        ASSERT_EQ(fast.best_ask(), reference.best_ask()) << "op " << i;
        ASSERT_EQ(fast.order_count(), reference.order_count()) << "op " << i;
        if (auto bid = reference.best_bid()) {
            ASSERT_EQ(fast.volume_at(lob::Side::Buy, *bid),
                      reference.volume_at(lob::Side::Buy, *bid)) << "op " << i;
        }
        if (auto ask = reference.best_ask()) {
            ASSERT_EQ(fast.volume_at(lob::Side::Sell, *ask),
                      reference.volume_at(lob::Side::Sell, *ask)) << "op " << i;
        }
    }
    // Guard against a flow that never trades, which would test little.
    EXPECT_GT(trades_seen, n_ops / 20);
}

}  // namespace

TEST(Differential, DefaultFlowMatchesReference) {
    for (std::uint64_t seed : {1, 2, 3}) {
        lob::FlowConfig cfg;
        cfg.seed = seed;
        expect_same(cfg, 100'000, std::size_t{1} << 16);
    }
}

TEST(Differential, AggressiveThinBookMatchesReference) {
    lob::FlowConfig cfg;
    cfg.seed = 7;
    cfg.target_depth = 50;  // the book often empties on one side
    cfg.market_share = 0.2;
    cfg.marketable_share = 0.3;
    expect_same(cfg, 100'000, std::size_t{1} << 16);
}

TEST(Differential, TinyWindowExercisesOverflowLevels) {
    lob::FlowConfig cfg;
    cfg.seed = 11;
    cfg.level_decay = 0.02;  // orders spread over hundreds of ticks
    expect_same(cfg, 100'000, 64);
}
