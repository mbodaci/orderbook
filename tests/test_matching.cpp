#include <gtest/gtest.h>

#include <vector>

#include "orderbook/order_book.hpp"

using namespace orderbook;

// The AddOrder and CancelOrder tests keep bids below asks so nothing matches.

TEST(AddOrder, SingleBuySetsBestBid) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(AddOrder, SingleSellSetsBestAsk) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 105, 10}));
    EXPECT_EQ(book.best_ask(), 105);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(AddOrder, SamePriceOrdersAreAllCounted) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 100, 20}));
    ASSERT_TRUE(book.add_order({3, Side::Buy, 100, 30}));
    EXPECT_EQ(book.order_count(), 3u);
    EXPECT_EQ(book.best_bid(), 100);
}

TEST(AddOrder, BestPriceIsHighestBidAndLowestAsk) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 99, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 101, 10}));
    ASSERT_TRUE(book.add_order({3, Side::Buy, 100, 10}));
    ASSERT_TRUE(book.add_order({4, Side::Sell, 105, 10}));
    ASSERT_TRUE(book.add_order({5, Side::Sell, 103, 10}));
    ASSERT_TRUE(book.add_order({6, Side::Sell, 104, 10}));
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.best_ask(), 103);
}

TEST(AddOrder, ZeroQuantityIsRejected) {
    OrderBook book;
    EXPECT_FALSE(book.add_order({1, Side::Buy, 100, 0}));
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(AddOrder, DuplicateIdIsRejectedAndOriginalKept) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    EXPECT_FALSE(book.add_order({1, Side::Sell, 105, 10}));
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(CancelOrder, RemovesOrderButKeepsSharedLevel) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 100, 20}));
    ASSERT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_FALSE(book.cancel_order(1));
}

TEST(CancelOrder, UnknownIdReturnsFalse) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    EXPECT_FALSE(book.cancel_order(42));
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(CancelOrder, LastOrderAtBestBidRemovesLevel) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 101, 10}));
    ASSERT_TRUE(book.cancel_order(2));
    EXPECT_EQ(book.best_bid(), 100);
    ASSERT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(CancelOrder, LastOrderAtBestAskRemovesLevel) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 105, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 103, 10}));
    ASSERT_TRUE(book.cancel_order(2));
    EXPECT_EQ(book.best_ask(), 105);
    ASSERT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(Matching, BuyLimitExactlyFillsRestingSell) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 10}));

    auto trades = book.add_order({2, Side::Buy, 100, 10});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{2, 1, 100, 10}}));
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.remaining_quantity(2), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}

TEST(Matching, SellLimitPartiallyFillsAndRemainderRests) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 5}));

    // Trades at the passive price (100), not the aggressor's limit (99)
    auto trades = book.add_order({2, Side::Sell, 99, 8});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{2, 1, 100, 5}}));
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.remaining_quantity(2), 3u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), 99);
}

TEST(Matching, AggressorSweepsTwoPriceLevels) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 5}));
    ASSERT_TRUE(book.add_order({3, Side::Sell, 102, 5}));

    auto trades = book.add_order({4, Side::Buy, 101, 8});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{4, 1, 100, 5}, {4, 2, 101, 3}}));
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.remaining_quantity(2), 2u);
    EXPECT_EQ(book.remaining_quantity(3), 5u);
    EXPECT_EQ(book.remaining_quantity(4), std::nullopt);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(Matching, SamePriceFillsOldestFirst) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 100, 5}));

    auto trades = book.add_order({3, Side::Buy, 100, 7});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{3, 1, 100, 5}, {3, 2, 100, 2}}));
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.remaining_quantity(2), 3u);
}

// Market orders below use price 0, which would never cross as a limit buy:
// they only match because a market order's price is ignored.

TEST(Matching, MarketBuyFullyFillsAcrossLevels) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 5}));

    auto trades = book.add_order({3, Side::Buy, 0, 7, OrderType::Market});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{3, 1, 100, 5}, {3, 2, 101, 2}}));
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.remaining_quantity(2), 3u);
    EXPECT_EQ(book.remaining_quantity(3), std::nullopt);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(Matching, MarketBuyWithNoAsksReturnsNoTradesAndDoesNotRest) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 5}));

    auto trades = book.add_order({2, Side::Buy, 0, 10, OrderType::Market});
    ASSERT_TRUE(trades);
    EXPECT_TRUE(trades->empty());
    EXPECT_EQ(book.remaining_quantity(2), std::nullopt);
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
}

TEST(Matching, MarketBuyPartialFillDiscardsRemainder) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));

    auto trades = book.add_order({2, Side::Buy, 0, 8, OrderType::Market});
    ASSERT_TRUE(trades);
    Quantity filled = 0;
    for (const Trade& trade : *trades) {
        filled += trade.quantity;
    }
    EXPECT_EQ(filled, 5u);
    EXPECT_LT(filled, 8u);
    EXPECT_EQ(book.remaining_quantity(2), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
}
