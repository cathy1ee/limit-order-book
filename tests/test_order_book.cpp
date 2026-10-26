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
