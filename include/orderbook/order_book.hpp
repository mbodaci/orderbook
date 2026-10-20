#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "orderbook/order.hpp"
#include "orderbook/price_level.hpp"

namespace orderbook {

class OrderBook {
public:
    // nullopt means rejected (zero quantity or duplicate id); an empty vector means
    // accepted with no fills.
    [[nodiscard]] std::optional<std::vector<Trade>> add_order(const Order& order);
    [[nodiscard]] bool cancel_order(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::size_t order_count() const { return orders_.size(); }
    std::optional<Quantity> remaining_quantity(OrderId id) const;

private:
    template <typename Levels>
    void match(Order& incoming, Levels& opposite, std::vector<Trade>& trades);

    // Bids sorted high-to-low, asks low-to-high: begin() is the best price on both sides.
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel> asks_;
    // The handle points at the resting Order, which carries its side and price,
    // so cancel can find the right level without storing anything else.
    std::unordered_map<OrderId, PriceLevel::Handle> orders_;
};

}  // namespace orderbook
