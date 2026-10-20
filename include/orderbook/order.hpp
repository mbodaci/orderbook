#pragma once

#include <cstdint>

namespace orderbook {

using OrderId = std::uint64_t;
// Integer ticks: floating-point prices cannot be compared for equality reliably.
using Price = std::int64_t;
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;  // remaining, decreases as the order fills
};

}  // namespace orderbook
