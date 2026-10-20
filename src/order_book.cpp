#include "orderbook/order_book.hpp"

#include <algorithm>

namespace orderbook {

namespace {

bool crosses(const Order& incoming, Price level_price) {
    if (incoming.type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? level_price <= incoming.price
                                      : level_price >= incoming.price;
}

// Templated because bids_ and asks_ have different comparators, hence different types.
template <typename Levels>
PriceLevel::Handle rest(Levels& levels, const Order& order) {
    auto it = levels.try_emplace(order.price, order.price).first;
    return it->second.add(order);
}

template <typename Levels>
void unrest(Levels& levels, PriceLevel::Handle handle) {
    auto it = levels.find(handle->price);
    it->second.remove(handle);
    if (it->second.empty()) {
        levels.erase(it);
    }
}

}  // namespace

// Defined here rather than in the header: it is only ever instantiated in this file.
template <typename Levels>
void OrderBook::match(Order& incoming, Levels& opposite, std::vector<Trade>& trades) {
    // Walk levels from the best price inward
    while (incoming.quantity > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        PriceLevel& level = level_it->second;
        if (!crosses(incoming, level.price())) {
            break;
        }

        // Fill resting orders oldest first
        while (incoming.quantity > 0 && !level.empty()) {
            auto resting = level.front();
            Quantity fill = std::min(incoming.quantity, resting->quantity);
            trades.push_back({incoming.id, resting->id, level.price(), fill});
            incoming.quantity -= fill;
            if (fill == resting->quantity) {
                // Erase the index entry first: remove() invalidates the handle
                orders_.erase(resting->id);
                level.remove(resting);
            } else {
                level.reduce(resting, fill);
            }
        }

        if (level.empty()) {
            opposite.erase(level_it);
        }
    }
}

std::optional<std::vector<Trade>> OrderBook::add_order(const Order& order) {
    if (order.quantity == 0 || orders_.contains(order.id)) {
        return std::nullopt;
    }

    std::vector<Trade> trades;
    Order remaining = order;
    if (remaining.side == Side::Buy) {
        match(remaining, asks_, trades);
    } else {
        match(remaining, bids_, trades);
    }

    // Unfilled market quantity is discarded, never rested
    if (remaining.quantity > 0 && remaining.type == OrderType::Limit) {
        auto handle = remaining.side == Side::Buy ? rest(bids_, remaining)
                                                  : rest(asks_, remaining);
        orders_.emplace(remaining.id, handle);
    }
    return trades;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return false;
    }
    if (it->second->side == Side::Buy) {
        unrest(bids_, it->second);
    } else {
        unrest(asks_, it->second);
    }
    orders_.erase(it);
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

}  // namespace orderbook
