#include "lob/order_book.hpp"

#include <gtest/gtest.h>

using lob::OrderBook;
using lob::Side;

TEST(OrderBook, EmptyBookHasNoBestPrices) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook book;
    EXPECT_TRUE(book.add_limit(1, Side::Buy, 100, 10).empty());
    EXPECT_TRUE(book.add_limit(2, Side::Sell, 101, 5).empty());

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, BestPricesTrackTopOfBook) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 99, 1);
    book.add_limit(2, Side::Buy, 100, 1);
    book.add_limit(3, Side::Sell, 103, 1);
    book.add_limit(4, Side::Sell, 102, 1);

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 102);
}

TEST(OrderBook, FullFillRemovesBothOrders) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, 2u);
    EXPECT_EQ(trades[0].sell_id, 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].qty, 10u);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, PartialFillLeavesRemainderResting) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 100, 4);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 4u);
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 6u);
    EXPECT_TRUE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
}

TEST(OrderBook, IncomingRemainderRestsAfterSweep) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 3);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 7u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, TradesExecuteAtRestingPrice) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    auto trades = book.add_limit(2, Side::Buy, 105, 5);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);  // price improvement for the aggressor
}

TEST(OrderBook, TimePriorityWithinALevel) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);  // first in line
    book.add_limit(2, Side::Sell, 100, 5);
    auto trades = book.add_limit(3, Side::Buy, 100, 5);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 1u);
    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.contains(2));
}

TEST(OrderBook, MultiLevelSweepGoesBestPriceFirst) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 102, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.add_limit(3, Side::Sell, 101, 5);
    auto trades = book.add_limit(4, Side::Buy, 101, 12);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(book.best_bid(), 101);              // 2 left over at 101
    EXPECT_EQ(book.volume_at(Side::Buy, 101), 2u);
    EXPECT_EQ(book.best_ask(), 102);              // untouched
}

TEST(OrderBook, SellSideAggressorMatchesBids) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 99, 5);
    auto trades = book.add_limit(3, Side::Sell, 99, 8);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].buy_id, 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].buy_id, 2u);
    EXPECT_EQ(trades[1].qty, 3u);
    EXPECT_EQ(book.volume_at(Side::Buy, 99), 2u);
}

TEST(OrderBook, CancelRemovesRestingOrder) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 100, 5);

    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 5u);

    EXPECT_TRUE(book.cancel(2));
    EXPECT_FALSE(book.best_bid().has_value());  // empty level is removed
}

TEST(OrderBook, CancelUnknownIdFails) {
    OrderBook book;
    EXPECT_FALSE(book.cancel(42));
}

TEST(OrderBook, CancelledOrderIsNotMatched) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.cancel(1);
    auto trades = book.add_limit(3, Side::Buy, 100, 5);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 2u);
}

TEST(OrderBook, RejectsZeroQuantityAndDuplicateIds) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 0);
    EXPECT_EQ(book.order_count(), 0u);

    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(1, Side::Buy, 101, 5);  // duplicate id ignored
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
}

// ---- Market orders ----

TEST(MarketOrder, SweepsLevelsAtAnyPrice) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 150, 5);
    auto trades = book.add_market(3, Side::Buy, 8);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 150);
    EXPECT_EQ(trades[1].qty, 3u);
    EXPECT_EQ(book.volume_at(Side::Sell, 150), 2u);
}

TEST(MarketOrder, UnfilledRemainderNeverRests) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 3);
    auto trades = book.add_market(2, Side::Sell, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 3u);
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(MarketOrder, AgainstEmptyBookDoesNothing) {
    OrderBook book;
    EXPECT_TRUE(book.add_market(1, Side::Buy, 10).empty());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(MarketOrder, RejectsZeroQuantityAndDuplicateIds) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    EXPECT_TRUE(book.add_market(2, Side::Buy, 0).empty());
    EXPECT_TRUE(book.add_market(1, Side::Buy, 5).empty());  // id 1 is resting
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 5u);
}

// ---- Modify ----

TEST(Modify, ReducingQuantityKeepsPriority) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    book.add_limit(2, Side::Sell, 100, 10);
    EXPECT_TRUE(book.modify(1, 100, 4).empty());
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 14u);

    auto trades = book.add_limit(3, Side::Buy, 100, 4);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 1u);  // still first in line
}

TEST(Modify, IncreasingQuantityLosesPriority) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.modify(1, 100, 8);
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 13u);

    auto trades = book.add_limit(3, Side::Buy, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 2u);
}

TEST(Modify, PriceChangeLosesPriorityAndMovesLevel) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 101, 5);
    book.add_limit(3, Side::Buy, 100, 5);
    book.modify(2, 100, 5);  // joins 100 behind orders 1 and 3

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 15u);
    EXPECT_EQ(book.volume_at(Side::Buy, 101), 0u);

    auto trades = book.add_limit(4, Side::Sell, 100, 15);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].buy_id, 1u);
    EXPECT_EQ(trades[1].buy_id, 3u);
    EXPECT_EQ(trades[2].buy_id, 2u);
}

TEST(Modify, PriceChangeThatCrossesTrades) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 102, 5);
    book.add_limit(2, Side::Buy, 100, 8);
    auto trades = book.modify(2, 102, 8);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, 2u);
    EXPECT_EQ(trades[0].price, 102);
    EXPECT_EQ(trades[0].qty, 5u);
    EXPECT_EQ(book.best_bid(), 102);  // remaining 3 rest at the new price
    EXPECT_EQ(book.volume_at(Side::Buy, 102), 3u);
}

TEST(Modify, UnchangedOrderIsANoOp) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.modify(1, 100, 5);

    auto trades = book.add_limit(3, Side::Buy, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 1u);
}

TEST(Modify, RejectsUnknownIdAndZeroQuantity) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    EXPECT_TRUE(book.modify(42, 100, 5).empty());
    EXPECT_FALSE(book.contains(42));

    book.modify(1, 100, 0);
    EXPECT_TRUE(book.contains(1));
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 5u);
}
