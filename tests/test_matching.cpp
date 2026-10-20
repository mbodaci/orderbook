#include <gtest/gtest.h>

#include "orderbook/order_book.hpp"

using namespace orderbook;

// All tests keep bids below asks: crossing orders will match once match() exists.

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
