#pragma once

#include "lob/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

// A price-time priority limit order book, optimized for throughput.
//
// It has the same interface and behavior as MapOrderBook (the v1 reference);
// the differential tests replay random order flow through both and require
// identical trades. What changed:
//   - Price levels live in a flat array indexed by price (a window of ticks
//     centered on the first order's price), with a bitmap of non-empty levels
//     so the next best price is found 64 levels at a time. Prices outside the
//     window fall back to a std::map, so correctness never depends on it.
//   - Orders are intrusive doubly-linked list nodes, so a level is two
//     pointers and cancel unlinks in O(1) without a separate list allocation.
//   - Nodes come from a memory pool with a free list: no malloc per order.
//   - Each level keeps its total quantity, so volume_at is O(1).
// Bids and asks share the one array: the book is never crossed, so any
// level above the best bid holds asks and any level below the best ask
// holds bids.
class OrderBook {
public:
    // window_levels: how many consecutive ticks the flat array covers.
    explicit OrderBook(std::size_t window_levels = std::size_t{1} << 16);

    OrderBook(const OrderBook&) = delete;  // holds pointers into its own pool
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

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
    struct Node {
        OrderId  id;
        Price    price;
        Node*    prev;
        Node*    next;  // also links the pool's free list
        Quantity qty;
        Side     side;
    };

    struct Level {
        Node*    head = nullptr;  // oldest order: matched first
        Node*    tail = nullptr;
        Quantity total = 0;
    };

    // Fixed-size chunks of nodes with an intrusive free list. Chunks are never
    // freed or moved until the book is destroyed, so node pointers are stable.
    class NodePool {
    public:
        Node* acquire();
        void release(Node* node);

    private:
        static constexpr std::size_t kChunk = 4096;
        std::vector<std::unique_ptr<Node[]>> chunks_;
        Node* free_ = nullptr;
    };

    void match(Side side, Price limit, OrderId id, Quantity& qty, std::vector<Trade>& trades);
    void rest(OrderId id, Side side, Price price, Quantity qty);
    void remove(Node* node);  // unlink a resting node and release it

    // Level storage: the flat window, else the sparse map.
    bool in_window(Price price) const;
    std::size_t slot(Price price) const { return static_cast<std::size_t>(price - base_); }
    Level* find_level(Price price);
    const Level* find_level(Price price) const;
    Level& level_for(Price price);  // creates the level if needed
    void drop_level(Price price);   // called once a level is empty

    // Nearest non-empty level strictly above / below price, in either store.
    std::optional<Price> next_above(Price price) const;
    std::optional<Price> next_below(Price price) const;

    std::vector<Level>         window_;
    std::vector<std::uint64_t> occupied_;  // bit i set <=> window_[i] non-empty
    Price                      base_ = 0;  // price of window_[0]
    bool                       window_placed_ = false;
    std::map<Price, Level>     sparse_;    // levels outside the window

    std::size_t bid_count_ = 0;  // resting orders per side
    std::size_t ask_count_ = 0;
    Price       best_bid_ = 0;   // valid only while the side's count > 0
    Price       best_ask_ = 0;

    NodePool                           pool_;
    std::unordered_map<OrderId, Node*> index_;
};

}  // namespace lob
