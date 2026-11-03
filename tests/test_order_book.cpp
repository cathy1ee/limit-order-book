// Behavior tests. Every test runs against both implementations: the fast
// OrderBook and the v1 MapOrderBook it must match.
#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"

#include <gtest/gtest.h>

using lob::Side;

using Books = ::testing::Types<lob::OrderBook, lob::MapOrderBook>;

template <typename Book> class OrderBook : public ::testing::Test {};
template <typename Book> class MarketOrder : public ::testing::Test {};
template <typename Book> class Modify : public ::testing::Test {};
TYPED_TEST_SUITE(OrderBook, Books);
TYPED_TEST_SUITE(MarketOrder, Books);
TYPED_TEST_SUITE(Modify, Books);

TYPED_TEST(OrderBook, EmptyBookHasNoBestPrices) {
    TypeParam book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBook, NonCrossingOrdersRest) {
    TypeParam book;
    EXPECT_TRUE(book.add_limit(1, Side::Buy, 100, 10).empty());
    EXPECT_TRUE(book.add_limit(2, Side::Sell, 101, 5).empty());

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.order_count(), 2u);
}

TYPED_TEST(OrderBook, BestPricesTrackTopOfBook) {
    TypeParam book;
    book.add_limit(1, Side::Buy, 99, 1);
    book.add_limit(2, Side::Buy, 100, 1);
    book.add_limit(3, Side::Sell, 103, 1);
    book.add_limit(4, Side::Sell, 102, 1);

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 102);
}

TYPED_TEST(OrderBook, FullFillRemovesBothOrders) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_id, 2u);
    EXPECT_EQ(trades[0].sell_id, 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].qty, 10u);
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(OrderBook, PartialFillLeavesRemainderResting) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 100, 4);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 4u);
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 6u);
    EXPECT_TRUE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
}

TYPED_TEST(OrderBook, IncomingRemainderRestsAfterSweep) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 3);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 7u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TYPED_TEST(OrderBook, TradesExecuteAtRestingPrice) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);
    auto trades = book.add_limit(2, Side::Buy, 105, 5);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);  // price improvement for the aggressor
}

TYPED_TEST(OrderBook, TimePriorityWithinALevel) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);  // first in line
    book.add_limit(2, Side::Sell, 100, 5);
    auto trades = book.add_limit(3, Side::Buy, 100, 5);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 1u);
    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.contains(2));
}

TYPED_TEST(OrderBook, MultiLevelSweepGoesBestPriceFirst) {
    TypeParam book;
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

TYPED_TEST(OrderBook, SellSideAggressorMatchesBids) {
    TypeParam book;
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

TYPED_TEST(OrderBook, CancelRemovesRestingOrder) {
    TypeParam book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 100, 5);

    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 5u);

    EXPECT_TRUE(book.cancel(2));
    EXPECT_FALSE(book.best_bid().has_value());  // empty level is removed
}

TYPED_TEST(OrderBook, CancelUnknownIdFails) {
    TypeParam book;
    EXPECT_FALSE(book.cancel(42));
}

TYPED_TEST(OrderBook, CancelledOrderIsNotMatched) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.cancel(1);
    auto trades = book.add_limit(3, Side::Buy, 100, 5);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 2u);
}

TYPED_TEST(OrderBook, RejectsZeroQuantityAndDuplicateIds) {
    TypeParam book;
    book.add_limit(1, Side::Buy, 100, 0);
    EXPECT_EQ(book.order_count(), 0u);

    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(1, Side::Buy, 101, 5);  // duplicate id ignored
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
}

// ---- Market orders ----

TYPED_TEST(MarketOrder, SweepsLevelsAtAnyPrice) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 150, 5);
    auto trades = book.add_market(3, Side::Buy, 8);

    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 150);
    EXPECT_EQ(trades[1].qty, 3u);
    EXPECT_EQ(book.volume_at(Side::Sell, 150), 2u);
}

TYPED_TEST(MarketOrder, UnfilledRemainderNeverRests) {
    TypeParam book;
    book.add_limit(1, Side::Buy, 100, 3);
    auto trades = book.add_market(2, Side::Sell, 10);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 3u);
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TYPED_TEST(MarketOrder, AgainstEmptyBookDoesNothing) {
    TypeParam book;
    EXPECT_TRUE(book.add_market(1, Side::Buy, 10).empty());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(MarketOrder, RejectsZeroQuantityAndDuplicateIds) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);
    EXPECT_TRUE(book.add_market(2, Side::Buy, 0).empty());
    EXPECT_TRUE(book.add_market(1, Side::Buy, 5).empty());  // id 1 is resting
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 5u);
}

// ---- Modify ----

TYPED_TEST(Modify, ReducingQuantityKeepsPriority) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 10);
    book.add_limit(2, Side::Sell, 100, 10);
    EXPECT_TRUE(book.modify(1, 100, 4).empty());
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 14u);

    auto trades = book.add_limit(3, Side::Buy, 100, 4);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 1u);  // still first in line
}

TYPED_TEST(Modify, IncreasingQuantityLosesPriority) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.modify(1, 100, 8);
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 13u);

    auto trades = book.add_limit(3, Side::Buy, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 2u);
}

TYPED_TEST(Modify, PriceChangeLosesPriorityAndMovesLevel) {
    TypeParam book;
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

TYPED_TEST(Modify, PriceChangeThatCrossesTrades) {
    TypeParam book;
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

TYPED_TEST(Modify, UnchangedOrderIsANoOp) {
    TypeParam book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.modify(1, 100, 5);

    auto trades = book.add_limit(3, Side::Buy, 100, 5);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_id, 1u);
}

TYPED_TEST(Modify, RejectsUnknownIdAndZeroQuantity) {
    TypeParam book;
    book.add_limit(1, Side::Buy, 100, 5);
    EXPECT_TRUE(book.modify(42, 100, 5).empty());
    EXPECT_FALSE(book.contains(42));

    book.modify(1, 100, 0);
    EXPECT_TRUE(book.contains(1));
    EXPECT_EQ(book.volume_at(Side::Buy, 100), 5u);
}

// ---- Fast book only: prices outside the flat window ----

TEST(OrderBookWindow, PricesOutsideTheWindowStillMatchInOrder) {
    lob::OrderBook book(64);              // window covers ~64 ticks around 1000
    book.add_limit(1, Side::Sell, 1'000, 5);
    book.add_limit(2, Side::Sell, 5'000, 5);  // far above: overflow map
    book.add_limit(3, Side::Sell, 1'010, 5);
    book.add_limit(4, Side::Buy, -3'000, 5);  // far below
    EXPECT_EQ(book.best_ask(), 1'000);
    EXPECT_EQ(book.best_bid(), -3'000);

    auto trades = book.add_market(5, Side::Buy, 15);
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 1'000);
    EXPECT_EQ(trades[1].price, 1'010);
    EXPECT_EQ(trades[2].price, 5'000);
    EXPECT_FALSE(book.best_ask().has_value());

    EXPECT_TRUE(book.cancel(4));
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBookWindow, BestPriceCanMoveFromOverflowIntoWindow) {
    lob::OrderBook book(64);
    book.add_limit(1, Side::Buy, 1'000, 5);    // places the window
    book.add_limit(2, Side::Buy, 9'000, 5);    // better bid, outside the window
    EXPECT_EQ(book.best_bid(), 9'000);
    book.cancel(2);
    EXPECT_EQ(book.best_bid(), 1'000);
    EXPECT_EQ(book.volume_at(Side::Buy, 1'000), 5u);
}
