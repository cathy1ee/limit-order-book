#include "lob/order_book.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace lob {

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

// Lowest set bit at index >= from, or kNone.
std::size_t first_set_from(const std::vector<std::uint64_t>& bits, std::size_t from) {
    std::size_t word = from / 64;
    if (word >= bits.size()) return kNone;
    std::uint64_t w = bits[word] & (~std::uint64_t{0} << (from % 64));
    while (true) {
        if (w != 0) return word * 64 + static_cast<std::size_t>(std::countr_zero(w));
        if (++word == bits.size()) return kNone;
        w = bits[word];
    }
}

// Highest set bit at index <= from, or kNone.
std::size_t last_set_upto(const std::vector<std::uint64_t>& bits, std::size_t from) {
    std::size_t word = from / 64;
    const unsigned shift = 63 - static_cast<unsigned>(from % 64);
    std::uint64_t w = bits[word] & (~std::uint64_t{0} >> shift);
    while (true) {
        if (w != 0) return word * 64 + 63 - static_cast<std::size_t>(std::countl_zero(w));
        if (word-- == 0) return kNone;
        w = bits[word];
    }
}

}  // namespace

// ---- NodePool ----

OrderBook::Node* OrderBook::NodePool::acquire() {
    if (free_ == nullptr) {
        auto chunk = std::make_unique<Node[]>(kChunk);
        for (std::size_t i = 0; i + 1 < kChunk; ++i) chunk[i].next = &chunk[i + 1];
        chunk[kChunk - 1].next = nullptr;
        free_ = &chunk[0];
        chunks_.push_back(std::move(chunk));
    }
    Node* node = free_;
    free_ = node->next;
    return node;
}

void OrderBook::NodePool::release(Node* node) {
    node->next = free_;
    free_ = node;
}

// ---- OrderBook ----

OrderBook::OrderBook(std::size_t window_levels)
    : window_((std::max<std::size_t>(window_levels, 64) + 63) / 64 * 64),
      occupied_(window_.size() / 64, 0) {}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    std::vector<Trade> trades;
    if (qty == 0 || contains(id)) return trades;

    match(side, price, id, qty, trades);
    if (qty > 0) rest(id, side, price, qty);
    return trades;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    std::vector<Trade> trades;
    if (qty == 0 || contains(id)) return trades;

    const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max()
                                          : std::numeric_limits<Price>::min();
    match(side, limit, id, qty, trades);
    return trades;
}

void OrderBook::match(Side side, Price limit, OrderId id, Quantity& qty,
                      std::vector<Trade>& trades) {
    const bool buy = side == Side::Buy;
    std::size_t& opposite_count = buy ? ask_count_ : bid_count_;
    Price& best = buy ? best_ask_ : best_bid_;

    while (qty > 0 && opposite_count > 0 && (buy ? best <= limit : best >= limit)) {
        const Price level_price = best;
        Level& level = *find_level(level_price);

        while (qty > 0 && level.head != nullptr) {
            Node* resting = level.head;
            const Quantity fill = std::min(qty, resting->qty);
            trades.push_back(Trade{
                buy ? id : resting->id,
                buy ? resting->id : id,
                level_price,
                fill,
            });

            qty -= fill;
            resting->qty -= fill;
            level.total -= fill;
            if (resting->qty == 0) {
                index_.erase(resting->id);
                level.head = resting->next;
                if (level.head != nullptr) level.head->prev = nullptr; else level.tail = nullptr;
                pool_.release(resting);
                --opposite_count;
            }
        }

        if (level.head == nullptr) {
            drop_level(level_price);
            if (opposite_count > 0) {
                best = buy ? *next_above(level_price) : *next_below(level_price);
            }
        }
    }
}

void OrderBook::rest(OrderId id, Side side, Price price, Quantity qty) {
    Node* node = pool_.acquire();
    *node = Node{id, price, nullptr, nullptr, qty, side};

    Level& level = level_for(price);
    node->prev = level.tail;
    if (level.tail != nullptr) level.tail->next = node; else level.head = node;
    level.tail = node;
    level.total += qty;
    index_.emplace(id, node);

    if (side == Side::Buy) {
        if (bid_count_++ == 0 || price > best_bid_) best_bid_ = price;
    } else {
        if (ask_count_++ == 0 || price < best_ask_) best_ask_ = price;
    }
}

void OrderBook::remove(Node* node) {
    const Price price = node->price;
    const Side side = node->side;
    Level& level = *find_level(price);

    if (node->prev != nullptr) node->prev->next = node->next; else level.head = node->next;
    if (node->next != nullptr) node->next->prev = node->prev; else level.tail = node->prev;
    level.total -= node->qty;
    pool_.release(node);

    const bool empty = level.head == nullptr;
    if (empty) drop_level(price);

    if (side == Side::Buy) {
        if (--bid_count_ > 0 && empty && price == best_bid_) best_bid_ = *next_below(price);
    } else {
        if (--ask_count_ > 0 && empty && price == best_ask_) best_ask_ = *next_above(price);
    }
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    Node* node = found->second;
    index_.erase(found);
    remove(node);
    return true;
}

std::vector<Trade> OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    auto found = index_.find(id);
    if (found == index_.end() || new_qty == 0) return {};

    Node* node = found->second;
    if (new_price == node->price && new_qty <= node->qty) {
        find_level(node->price)->total -= node->qty - new_qty;
        node->qty = new_qty;  // reduce in place: time priority is kept
        return {};
    }

    // Price change or size increase: back of the queue at the new terms.
    const Side side = node->side;
    cancel(id);
    return add_limit(id, side, new_price, new_qty);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bid_count_ == 0) return std::nullopt;
    return best_bid_;
}

std::optional<Price> OrderBook::best_ask() const {
    if (ask_count_ == 0) return std::nullopt;
    return best_ask_;
}

std::optional<Price> OrderBook::spread() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

Quantity OrderBook::volume_at(Side side, Price price) const {
    const Level* level = find_level(price);
    if (level == nullptr || level->head == nullptr || level->head->side != side) return 0;
    return level->total;
}

// ---- Level storage ----

bool OrderBook::in_window(Price price) const {
    return window_placed_ && price >= base_ &&
           price - base_ < static_cast<Price>(window_.size());
}

OrderBook::Level* OrderBook::find_level(Price price) {
    if (in_window(price)) return &window_[slot(price)];
    auto it = sparse_.find(price);
    return it == sparse_.end() ? nullptr : &it->second;
}

const OrderBook::Level* OrderBook::find_level(Price price) const {
    return const_cast<OrderBook*>(this)->find_level(price);
}

OrderBook::Level& OrderBook::level_for(Price price) {
    if (!window_placed_) {
        // Center the window on the first price that rests, clamped so the
        // window's range stays representable.
        const Price half = static_cast<Price>(window_.size() / 2);
        const Price lowest = std::numeric_limits<Price>::min();
        base_ = price < lowest + half ? lowest : price - half;
        if (base_ > std::numeric_limits<Price>::max() - static_cast<Price>(window_.size())) {
            base_ = std::numeric_limits<Price>::max() - static_cast<Price>(window_.size());
        }
        window_placed_ = true;
    }
    if (in_window(price)) {
        const std::size_t s = slot(price);
        occupied_[s / 64] |= std::uint64_t{1} << (s % 64);
        return window_[s];
    }
    return sparse_[price];
}

void OrderBook::drop_level(Price price) {
    if (in_window(price)) {
        const std::size_t s = slot(price);
        occupied_[s / 64] &= ~(std::uint64_t{1} << (s % 64));
    } else {
        sparse_.erase(price);
    }
}

std::optional<Price> OrderBook::next_above(Price price) const {
    std::optional<Price> best;
    if (window_placed_ && price < base_ + static_cast<Price>(window_.size()) - 1) {
        const std::size_t from = price < base_ ? 0 : slot(price) + 1;
        const std::size_t s = first_set_from(occupied_, from);
        if (s != kNone) best = base_ + static_cast<Price>(s);
    }
    auto it = sparse_.upper_bound(price);
    if (it != sparse_.end() && (!best || it->first < *best)) best = it->first;
    return best;
}

std::optional<Price> OrderBook::next_below(Price price) const {
    std::optional<Price> best;
    if (window_placed_ && price > base_) {
        const Price top = base_ + static_cast<Price>(window_.size()) - 1;
        const std::size_t from = price > top ? window_.size() - 1 : slot(price) - 1;
        const std::size_t s = last_set_upto(occupied_, from);
        if (s != kNone) best = base_ + static_cast<Price>(s);
    }
    auto it = sparse_.lower_bound(price);
    if (it != sparse_.begin()) {
        --it;
        if (!best || it->first > *best) best = it->first;
    }
    return best;
}

}  // namespace lob
