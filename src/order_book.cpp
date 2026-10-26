#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

namespace {

bool crosses(Side side, Price incoming, Price resting) {
    return side == Side::Buy ? incoming >= resting : incoming <= resting;
}

}  // namespace

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    std::vector<Trade> trades;
    if (qty == 0 || contains(id)) return trades;

    Order incoming{id, side, price, qty};
    if (side == Side::Buy) {
        match(incoming, asks_, trades);
    } else {
        match(incoming, bids_, trades);
    }

    if (incoming.qty > 0) rest(incoming);
    return trades;
}

template <typename Book>
void OrderBook::match(Order& incoming, Book& opposite, std::vector<Trade>& trades) {
    while (incoming.qty > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();  // best opposite price
        if (!crosses(incoming.side, incoming.price, level_it->first)) break;

        Level& level = level_it->second;
        while (incoming.qty > 0 && !level.empty()) {
            Order& resting = level.front();  // time priority: oldest first
            const Quantity fill = std::min(incoming.qty, resting.qty);

            const bool buy_incoming = incoming.side == Side::Buy;
            trades.push_back(Trade{
                buy_incoming ? incoming.id : resting.id,
                buy_incoming ? resting.id : incoming.id,
                resting.price,
                fill,
            });

            incoming.qty -= fill;
            resting.qty -= fill;
            if (resting.qty == 0) {
                index_.erase(resting.id);
                level.pop_front();
            }
        }

        if (level.empty()) opposite.erase(level_it);
    }
}

void OrderBook::rest(const Order& order) {
    Level* level = nullptr;
    if (order.side == Side::Buy) {
        level = &bids_[order.price];
    } else {
        level = &asks_[order.price];
    }
    level->push_back(order);
    index_.emplace(order.id, Location{order.side, order.price, std::prev(level->end())});
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    const Location loc = found->second;
    index_.erase(found);

    auto remove_from = [&](auto& book) {
        auto level_it = book.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) book.erase(level_it);
    };

    if (loc.side == Side::Buy) {
        remove_from(bids_);
    } else {
        remove_from(asks_);
    }
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::volume_at(Side side, Price price) const {
    auto sum = [price](const auto& book) -> Quantity {
        auto it = book.find(price);
        if (it == book.end()) return 0;
        Quantity total = 0;
        for (const Order& o : it->second) total += o.qty;
        return total;
    };
    return side == Side::Buy ? sum(bids_) : sum(asks_);
}

}  // namespace lob
