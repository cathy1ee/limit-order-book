#pragma once

#include "lob/types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

// A price-time priority limit order book.
//
// v1 (this version) favors correctness and clarity:
//   - each side is a std::map from price to a FIFO queue of orders
//   - an id -> location index gives O(1) cancel
// Later versions replace these with a flat price array, intrusive lists
// and a memory pool (see the roadmap in the README).
class OrderBook {
public:
    // Adds a limit order. It matches against the opposite side first; any
    // unfilled remainder rests in the book. Returns the trades it produced.
    // Duplicate ids and zero quantity are rejected (no trades, no effect).
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);

    // Removes a resting order. Returns false if the id is not in the book.
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

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
