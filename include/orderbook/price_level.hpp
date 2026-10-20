#pragma once

#include <cassert>
#include <list>

#include "orderbook/order.hpp"

namespace orderbook {

// All resting orders at one price, oldest first (time priority).
class PriceLevel {
public:
    // std::list iterators stay valid when other elements are inserted or erased,
    // so the book can store them per order id for O(1) cancel.
    using Handle = std::list<Order>::iterator;

    explicit PriceLevel(Price price) : price_(price) {}

    [[nodiscard]] Handle add(const Order& order) {
        assert(order.price == price_);
        assert(order.quantity > 0);
        total_quantity_ += order.quantity;
        return orders_.insert(orders_.end(), order);
    }

    void remove(Handle handle) {
        total_quantity_ -= handle->quantity;
        orders_.erase(handle);
    }

    void reduce(Handle handle, Quantity amount) {
        assert(amount <= handle->quantity);
        handle->quantity -= amount;
        total_quantity_ -= amount;
    }

    Handle front() {
        assert(!orders_.empty());
        return orders_.begin();
    }

    bool empty() const { return orders_.empty(); }
    Price price() const { return price_; }
    Quantity total_quantity() const { return total_quantity_; }

private:
    Price price_;
    Quantity total_quantity_ = 0;
    std::list<Order> orders_;
};

}  // namespace orderbook
