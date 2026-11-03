#pragma once

#include "lob/types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

// The v1 order book: std::map price levels, std::list FIFO queues and an
// id -> location index. Straightforward and easy to check, so it is kept as
// the reference implementation for tests and the baseline for benchmarks.
// lob::OrderBook (order_book.hpp) has the same interface and is the fast one.
class MapOrderBook {
public:
    // Adds a limit order. It matches against the opposite side first; any
    // unfilled remainder rests in the book. Returns the trades it produced.
    // Duplicate ids and zero quantity are rejected (no trades, no effect).
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);

    // Adds a market order: it takes liquidity at any price, best first, and
    // any quantity left once the opposite side is empty is discarded (it never
    // rests). Duplicate ids and zero quantity are rejected.
    std::vector<Trade> add_market(OrderId id, Side side, Quantity qty);

    // Removes a resting order. Returns false if the id is not in the book.
    bool cancel(OrderId id);

    // Changes a resting order's price and/or quantity.
    //   - same price, lower quantity: reduced in place, keeps time priority
    //   - same price and quantity: no-op
    //   - new price or higher quantity: loses priority; the order is pulled
    //     and re-entered as a new limit order, so it may trade immediately
    // Unknown ids and zero quantity are rejected (use cancel to remove).
    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Best ask minus best bid, in ticks; empty if either side is empty.
    std::optional<Price> spread() const;

    // Total resting quantity at one price on one side (0 if empty).
    Quantity volume_at(Side side, Price price) const;

    std::size_t order_count() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.count(id) != 0; }

private:
    using Level = std::list<Order>;  // FIFO: front = oldest order

    struct Location {
        Side side;
        Price price;
        Level::iterator it;
    };

    template <typename Book>
    void match(Order& incoming, Book& opposite, std::vector<Trade>& trades);

    void rest(const Order& order);

    std::map<Price, Level, std::greater<Price>> bids_;  // best (highest) first
    std::map<Price, Level, std::less<Price>>    asks_;  // best (lowest) first
    std::unordered_map<OrderId, Location>       index_;
};

}  // namespace lob
