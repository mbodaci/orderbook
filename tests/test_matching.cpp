#include <gtest/gtest.h>

#include "orderbook/order_book.hpp"

using namespace orderbook;

TEST(OrderBook, AddThenCancelLeavesBookEmpty) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 10}));
    EXPECT_EQ(book.best_bid(), 100);

    ASSERT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
}
