#include "orderbook/order_book.hpp"

namespace orderbook {

namespace {

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

bool OrderBook::add_order(const Order& order) {
    if (order.quantity == 0 || orders_.contains(order.id)) {
        return false;
    }
    auto handle = order.side == Side::Buy ? rest(bids_, order) : rest(asks_, order);
    orders_.emplace(order.id, handle);
    return true;
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
