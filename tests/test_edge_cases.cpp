#include <gtest/gtest.h>

#include <vector>

#include "orderbook/order_book.hpp"

using namespace orderbook;

TEST(CancelEdgeCases, PartiallyFilledOrderCanCancelRemainder) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 100, 4}));
    ASSERT_EQ(book.remaining_quantity(1), 6u);

    EXPECT_TRUE(book.cancel_order(1));
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.best_ask(), std::nullopt);
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(CancelEdgeCases, FullyFilledRestingOrderCannotBeCancelled) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 100, 10}));

    EXPECT_FALSE(book.cancel_order(1));
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(CancelEdgeCases, FullyFilledAggressorCannotBeCancelled) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 100, 10}));

    EXPECT_FALSE(book.cancel_order(2));
}

TEST(CancelEdgeCases, MarketOrderIdCannotBeCancelledAfterExecution) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Buy, 0, 8, OrderType::Market}));

    EXPECT_FALSE(book.cancel_order(2));
}

TEST(CancelEdgeCases, UnknownIdOnEmptyBookReturnsFalse) {
    OrderBook book;
    EXPECT_FALSE(book.cancel_order(1));
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(CancelEdgeCases, UnknownIdLeavesBothSidesUntouched) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 99, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 10}));

    EXPECT_FALSE(book.cancel_order(3));
    EXPECT_EQ(book.remaining_quantity(1), 10u);
    EXPECT_EQ(book.remaining_quantity(2), 10u);
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.best_ask(), 101);
}

TEST(ModifyOrder, QuantityDownKeepsTimePriority) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 10}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 100, 10}));

    auto modified = book.modify_order(1, 100, 4);
    ASSERT_TRUE(modified);
    EXPECT_TRUE(modified->empty());
    EXPECT_EQ(book.remaining_quantity(1), 4u);

    auto trades = book.add_order({3, Side::Buy, 100, 6});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{3, 1, 100, 4}, {3, 2, 100, 2}}));
}

TEST(ModifyOrder, QuantityUpLosesTimePriority) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 100, 5}));

    ASSERT_TRUE(book.modify_order(1, 100, 8));
    EXPECT_EQ(book.remaining_quantity(1), 8u);

    auto trades = book.add_order({3, Side::Buy, 100, 6});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{3, 2, 100, 5}, {3, 1, 100, 1}}));
}

TEST(ModifyOrder, PriceChangeMovesLevelAndLosesTimePriority) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 5}));

    ASSERT_TRUE(book.modify_order(1, 101, 5));
    EXPECT_EQ(book.best_ask(), 101);

    auto trades = book.add_order({3, Side::Buy, 101, 6});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{3, 2, 101, 5}, {3, 1, 101, 1}}));
}

TEST(ModifyOrder, PriceChangeThatCrossesMatchesImmediately) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 99, 5}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 5}));

    auto trades = book.modify_order(1, 101, 5);
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{1, 2, 101, 5}}));
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(ModifyOrder, UnknownIdIsRejected) {
    OrderBook book;
    EXPECT_FALSE(book.modify_order(1, 100, 5));
}

TEST(ModifyOrder, ZeroQuantityIsRejectedAndOrderKept) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 100, 5}));

    EXPECT_FALSE(book.modify_order(1, 100, 0));
    EXPECT_EQ(book.remaining_quantity(1), 5u);
}

constexpr OwnerId ALICE = 7;
constexpr OwnerId BOB = 8;

TEST(SelfTradePrevention, CancelsIncomingAndKeepsResting) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5, OrderType::Limit, ALICE}));

    auto trades = book.add_order({2, Side::Buy, 100, 5, OrderType::Limit, ALICE});
    ASSERT_TRUE(trades);
    EXPECT_TRUE(trades->empty());
    EXPECT_EQ(book.remaining_quantity(1), 5u);
    EXPECT_EQ(book.remaining_quantity(2), std::nullopt);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(SelfTradePrevention, KeepsFillsBeforeTheSelfMatch) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 3, OrderType::Limit, BOB}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 5, OrderType::Limit, ALICE}));

    auto trades = book.add_order({3, Side::Buy, 101, 10, OrderType::Limit, ALICE});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{3, 1, 100, 3}}));
    EXPECT_EQ(book.remaining_quantity(2), 5u);
    EXPECT_EQ(book.remaining_quantity(3), std::nullopt);
    EXPECT_EQ(book.best_bid(), std::nullopt);
}

TEST(SelfTradePrevention, AppliesToMarketOrders) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5, OrderType::Limit, ALICE}));

    auto trades = book.add_order({2, Side::Buy, 0, 5, OrderType::Market, ALICE});
    ASSERT_TRUE(trades);
    EXPECT_TRUE(trades->empty());
    EXPECT_EQ(book.remaining_quantity(1), 5u);
}

TEST(SelfTradePrevention, AppliesWhenModifyCrosses) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Buy, 99, 5, OrderType::Limit, ALICE}));
    ASSERT_TRUE(book.add_order({2, Side::Sell, 101, 5, OrderType::Limit, ALICE}));

    auto trades = book.modify_order(1, 101, 5);
    ASSERT_TRUE(trades);
    EXPECT_TRUE(trades->empty());
    EXPECT_EQ(book.remaining_quantity(1), std::nullopt);
    EXPECT_EQ(book.remaining_quantity(2), 5u);
}

TEST(SelfTradePrevention, DifferentOwnersTrade) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5, OrderType::Limit, ALICE}));

    auto trades = book.add_order({2, Side::Buy, 100, 5, OrderType::Limit, BOB});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{2, 1, 100, 5}}));
}

TEST(SelfTradePrevention, OrdersWithoutOwnerAlwaysTrade) {
    OrderBook book;
    ASSERT_TRUE(book.add_order({1, Side::Sell, 100, 5}));

    auto trades = book.add_order({2, Side::Buy, 100, 5});
    ASSERT_TRUE(trades);
    EXPECT_EQ(*trades, (std::vector<Trade>{{2, 1, 100, 5}}));
}
