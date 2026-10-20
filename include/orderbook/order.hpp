#pragma once

#include <cstdint>

namespace orderbook {

using OrderId = std::uint64_t;
// Integer ticks: floating-point prices cannot be compared for equality reliably.
using Price = std::int64_t;
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

struct Order {
    OrderId id;
    Side side;
    Price price;        // ignored for market orders
    Quantity quantity;  // remaining, decreases as the order fills
    OrderType type = OrderType::Limit;
};

struct Trade {
    OrderId aggressor_id;
    OrderId passive_id;
    Price price;
    Quantity quantity;
};

}  // namespace orderbook
