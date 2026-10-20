#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "orderbook/order.hpp"
#include "orderbook/price_level.hpp"

namespace orderbook {

class OrderBook {
public:
    [[nodiscard]] bool add_order(const Order& order);
    [[nodiscard]] bool cancel_order(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::size_t order_count() const { return orders_.size(); }

private:
    // Bids sorted high-to-low, asks low-to-high: begin() is the best price on both sides.
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel> asks_;
    // The handle points at the resting Order, which carries its side and price,
    // so cancel can find the right level without storing anything else.
    std::unordered_map<OrderId, PriceLevel::Handle> orders_;
};

}  // namespace orderbook
