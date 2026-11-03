#pragma once

#include "lob/types.hpp"

#include <cstddef>
#include <random>
#include <vector>

namespace lob {

// One instruction for an order book.
struct Op {
    enum class Type : std::uint8_t { Limit, Market, Cancel, Modify };

    Type     type;
    OrderId  id;
    Side     side;
    Price    price;  // unused for Market and Cancel
    Quantity qty;    // unused for Cancel
};

// Applies one Op to any book with the OrderBook interface.
template <typename Book>
std::vector<Trade> apply(Book& book, const Op& op) {
    switch (op.type) {
        case Op::Type::Limit:  return book.add_limit(op.id, op.side, op.price, op.qty);
        case Op::Type::Market: return book.add_market(op.id, op.side, op.qty);
        case Op::Type::Cancel: book.cancel(op.id); return {};
        case Op::Type::Modify: return book.modify(op.id, op.price, op.qty);
    }
    return {};
}

// Parameters for a synthetic order flow. The defaults give a book that holds
// a few thousand orders across a few dozen levels near the touch, with most
// activity at the best prices, as in real markets.
struct FlowConfig {
    std::uint64_t seed            = 42;
    Price         start_mid       = 100'000;
    std::size_t   target_depth    = 5'000;  // resting orders the flow aims to keep
    double        market_share    = 0.05;
    double        cancel_share    = 0.30;
    double        modify_share    = 0.10;
    double        marketable_share = 0.08;  // share of limits priced through the touch
    double        level_decay     = 0.25;   // geometric distance from mid, in ticks
    Quantity      max_qty         = 100;
};

// Generates a reproducible stream of Ops. It tracks the ids it has issued,
// not the book's state, so some cancels and modifies hit orders that have
// already filled; both books must treat those as no-ops, which the
// differential tests check.
class OrderFlow {
public:
    explicit OrderFlow(FlowConfig cfg = {}) : cfg_(cfg), rng_(cfg.seed), mid_(cfg.start_mid) {}

    Op next() {
        // Drift the reference mid by at most one tick so the book moves.
        if (unit_(rng_) < 0.02) mid_ += coin_(rng_) ? 1 : -1;

        const double roll = unit_(rng_);
        const bool over_depth = live_.size() > cfg_.target_depth;
        if (!live_.empty() && (over_depth || roll < cfg_.cancel_share)) return cancel();
        if (!live_.empty() && roll < cfg_.cancel_share + cfg_.modify_share) return modify();
        if (roll < cfg_.cancel_share + cfg_.modify_share + cfg_.market_share) return market();
        return limit();
    }

    std::vector<Op> generate(std::size_t n) {
        std::vector<Op> ops;
        ops.reserve(n);
        for (std::size_t i = 0; i < n; ++i) ops.push_back(next());
        return ops;
    }

private:
    Side random_side() { return coin_(rng_) ? Side::Buy : Side::Sell; }
    Quantity random_qty() {
        return std::uniform_int_distribution<Quantity>(1, cfg_.max_qty)(rng_);
    }

    // Passive orders sit a geometric number of ticks behind the mid; a small
    // share are priced through it and trade on arrival.
    Price random_price(Side side) {
        const Price offset = 1 + std::geometric_distribution<Price>(cfg_.level_decay)(rng_);
        const bool through = unit_(rng_) < cfg_.marketable_share;
        const Price signed_offset = (side == Side::Buy) != through ? -offset : offset;
        return mid_ + signed_offset;
    }

    Op limit() {
        const OrderId id = next_id_++;
        const Side side = random_side();
        live_.push_back({id, side});
        return {Op::Type::Limit, id, side, random_price(side), random_qty()};
    }

    Op market() {
        return {Op::Type::Market, next_id_++, random_side(), 0, random_qty()};
    }

    Op cancel() {
        const Live order = take_random_live();
        return {Op::Type::Cancel, order.id, order.side, 0, 0};
    }

    Op modify() {
        const Live& order = live_[pick(live_.size())];
        return {Op::Type::Modify, order.id, order.side, random_price(order.side), random_qty()};
    }

    struct Live {
        OrderId id;
        Side side;
    };

    std::size_t pick(std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng_);
    }

    Live take_random_live() {
        const std::size_t i = pick(live_.size());
        const Live order = live_[i];
        live_[i] = live_.back();  // swap-remove: O(1)
        live_.pop_back();
        return order;
    }

    FlowConfig cfg_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::bernoulli_distribution coin_{0.5};
    Price mid_;
    OrderId next_id_ = 1;
    std::vector<Live> live_;
};

}  // namespace lob
