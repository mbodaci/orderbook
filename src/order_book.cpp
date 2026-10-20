#include "orderbook/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace orderbook {

namespace {

bool crosses(const Order& incoming, Price level_price) {
    if (incoming.type == OrderType::Market) {
        return true;
    }
    return incoming.side == Side::Buy ? level_price <= incoming.price
                                      : level_price >= incoming.price;
}

bool is_self_trade(const Order& incoming, const Order& resting) {
    return incoming.owner != NO_OWNER && incoming.owner == resting.owner;
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

#ifndef NDEBUG
// Checks one side and returns how many orders it holds.
template <typename Levels, typename Index>
std::size_t check_side(const Levels& levels, Side side, const Index& index) {
    std::size_t count = 0;
    for (const auto& [price, level] : levels) {
        assert(!level.empty());
        assert(level.price() == price);
        Quantity total = 0;
        for (const Order& order : level.orders()) {
            assert(order.side == side);
            assert(order.price == price);
            assert(order.quantity > 0);
            // The index entry for this id must point at this exact element
            auto it = index.find(order.id);
            assert(it != index.end() && &*it->second == &order);
            total += order.quantity;
            ++count;
        }
        assert(total == level.total_quantity());
    }
    return count;
}
#endif

}  // namespace

#ifndef NDEBUG
void OrderBook::assert_invariants() const {
    if (!bids_.empty() && !asks_.empty()) {
        assert(bids_.begin()->first < asks_.begin()->first);
    }
    // Every resting order maps to its own index entry, and the counts match,
    // so every index entry is reachable through the levels.
    std::size_t resting = check_side(bids_, Side::Buy, orders_)
                          + check_side(asks_, Side::Sell, orders_);
    assert(resting == orders_.size());
}
#endif

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
            // Self-trade prevention: cancel the incoming remainder, leave the resting order
            if (is_self_trade(incoming, *resting)) {
                incoming.quantity = 0;
                break;
            }
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
    assert_invariants();
    return trades;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return false;
    }
    remove_resting(it->second);
    orders_.erase(it);
    assert_invariants();
    return true;
}

std::optional<std::vector<Trade>> OrderBook::modify_order(OrderId id, Price new_price,
                                                          Quantity new_quantity) {
    auto it = orders_.find(id);
    if (it == orders_.end() || new_quantity == 0) {
        return std::nullopt;
    }
    auto handle = it->second;

    if (new_price == handle->price && new_quantity <= handle->quantity) {
        PriceLevel& level =
            handle->side == Side::Buy ? bids_.at(handle->price) : asks_.at(handle->price);
        level.reduce(handle, handle->quantity - new_quantity);
        assert_invariants();
        return std::vector<Trade>{};
    }

    // Copy before removal invalidates the handle
    Order replacement = *handle;
    replacement.price = new_price;
    replacement.quantity = new_quantity;
    remove_resting(handle);
    orders_.erase(it);
    // add_order checks the invariants once the replacement is in
    return add_order(replacement);
}

void OrderBook::remove_resting(PriceLevel::Handle handle) {
    if (handle->side == Side::Buy) {
        unrest(bids_, handle);
    } else {
        unrest(asks_, handle);
    }
}

std::optional<Quantity> OrderBook::remaining_quantity(OrderId id) const {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return it->second->quantity;
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
