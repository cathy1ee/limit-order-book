#pragma once

#include <cstdint>

namespace lob {

using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // integer ticks, never floating point
using Quantity = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;  // remaining quantity
};

struct Trade {
    OrderId  buy_id;
    OrderId  sell_id;
    Price    price;  // execution price (the resting order's price)
    Quantity qty;
};

}  // namespace lob
