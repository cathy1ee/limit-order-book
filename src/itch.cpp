#include "lob/itch.hpp"

#include <algorithm>

namespace lob::itch {

namespace {

// ITCH fields are big-endian.
std::uint64_t be(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

std::string_view stock_field(const std::uint8_t* p) {
    std::string_view s(reinterpret_cast<const char*>(p), 8);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);  // right-padded
    return s;
}

// Message lengths (without the 2-byte prefix) for the types we decode.
// Every message starts: type(1) locate(2) tracking(2) timestamp(6).
constexpr std::size_t kAddLen = 36, kAddMpidLen = 40, kExecLen = 31, kExecPriceLen = 36,
                      kCancelLen = 23, kDeleteLen = 19, kReplaceLen = 35,
                      kTradingActionLen = 25;

constexpr std::size_t kDefaultWindowOne = std::size_t{1} << 16;  // tracking a few symbols
constexpr std::size_t kDefaultWindowAll = std::size_t{1} << 10;  // tracking ~9,000

}  // namespace

Replayer::Replayer(std::vector<std::string> symbols, std::size_t window_levels)
    : wanted_(std::move(symbols)),
      window_levels_(window_levels != 0 ? window_levels
                     : wanted_.empty()  ? kDefaultWindowAll
                                        : kDefaultWindowOne),
      track_(1 << 16, Track::Unknown),
      books_(1 << 16),
      symbol_of_(1 << 16),
      trading_state_(1 << 16, 'T') {}

std::size_t Replayer::feed(std::span<const std::uint8_t> data) {
    std::size_t pos = 0;
    while (data.size() - pos >= 2) {
        const std::size_t len = static_cast<std::size_t>(be(&data[pos], 2));
        if (data.size() - pos - 2 < len) break;
        on_message(data.subspan(pos + 2, len));
        pos += 2 + len;
    }
    return pos;
}

void Replayer::on_message(std::span<const std::uint8_t> msg) {
    ++stats_.messages;
    if (msg.empty()) return;
    const std::uint8_t* m = msg.data();
    const char type = static_cast<char>(m[0]);

    auto need = [&](std::size_t len) {
        if (msg.size() >= len) return true;
        ++stats_.malformed;
        return false;
    };
    const auto locate = msg.size() >= 3 ? static_cast<std::uint16_t>(be(m + 1, 2)) : 0;
    msg_type_ = type;
    msg_time_ = msg.size() >= 11 ? be(m + 5, 6) : 0;

    switch (type) {
        case 'A':
        case 'F':
            if (!need(type == 'A' ? kAddLen : kAddMpidLen)) return;
            add(locate, stock_field(m + 24), be(m + 11, 8), m[19] == 'B' ? Side::Buy : Side::Sell,
                static_cast<Price>(be(m + 32, 4)), static_cast<Quantity>(be(m + 20, 4)));
            return;
        case 'E':
            if (!need(kExecLen)) return;
            reduce(locate, be(m + 11, 8), static_cast<Quantity>(be(m + 19, 4)), true);
            return;
        case 'C':  // executed at a price other than the order's (e.g. in a cross)
            if (!need(kExecPriceLen)) return;
            reduce(locate, be(m + 11, 8), static_cast<Quantity>(be(m + 19, 4)), true);
            return;
        case 'X':
            if (!need(kCancelLen)) return;
            reduce(locate, be(m + 11, 8), static_cast<Quantity>(be(m + 19, 4)), false);
            return;
        case 'D':
            if (!need(kDeleteLen)) return;
            erase(locate, be(m + 11, 8));
            return;
        case 'U':
            if (!need(kReplaceLen)) return;
            replace(locate, be(m + 11, 8), be(m + 19, 8), static_cast<Price>(be(m + 31, 4)),
                    static_cast<Quantity>(be(m + 27, 4)));
            return;
        case 'H': {
            if (!need(kTradingActionLen)) return;
            const char state = static_cast<char>(m[19]);
            trading_state_[locate] = state;
            unpark(locate);  // resuming: parked orders may now fit the book
            std::string_view reason(reinterpret_cast<const char*>(m + 21), 4);
            while (!reason.empty() && reason.back() == ' ') reason.remove_suffix(1);
            state_changes_.push_back(
                StateChange{msg_time_, std::string(stock_field(m + 11)), state, std::string(reason)});
            return;
        }
        default:
            return;  // not a book event
    }
}

OrderBook* Replayer::book_for(std::uint16_t locate, std::string_view stock) {
    if (track_[locate] == Track::Unknown) {
        const bool want = wanted_.empty() ||
                          std::find(wanted_.begin(), wanted_.end(), stock) != wanted_.end();
        track_[locate] = want ? Track::Active : Track::Skip;
        if (want) {
            books_[locate] = std::make_unique<OrderBook>(window_levels_);
            locate_of_.emplace(std::string(stock), locate);
            symbol_of_[locate] = std::string(stock);
        }
    }
    return tracked(locate);
}

OrderBook* Replayer::tracked(std::uint16_t locate) {
    return track_[locate] == Track::Active ? books_[locate].get() : nullptr;
}

void Replayer::add(std::uint16_t locate, std::string_view stock, OrderId ref, Side side,
                   Price price, Quantity shares) {
    OrderBook* book = book_for(locate, stock);
    if (book == nullptr) return;
    ++stats_.book_events;
    ++stats_.adds;
    place(*book, locate, ref, side, price, shares);
}

// Puts a new order in the book, or parks it if it would cross the book while
// the symbol is halted or paused. In those states Nasdaq lets the book cross
// and clears it with a reopening auction; a continuous matching engine cannot
// hold a crossed book, so the crossing orders wait outside it until the
// auction's executions have uncrossed it (see unpark).
void Replayer::place(OrderBook& book, std::uint16_t locate, OrderId ref, Side side, Price price,
                     Quantity shares) {
    const auto opposite = side == Side::Buy ? book.best_ask() : book.best_bid();
    const bool crosses = opposite && (side == Side::Buy ? price >= *opposite : price <= *opposite);

    if (crosses && trading_state_[locate] != 'T') {
        ++stats_.parked_adds;
        orders_[ref] = Resting{locate, side, price, shares, true};
        parked_[locate].push_back(ref);
        return;
    }
    if (!book.add_limit(ref, side, price, shares).empty()) {
        ++stats_.crossed_adds;
        flag(Anomaly::Kind::CrossedAdd, locate, ref);
    }
    // Track it only if it is resting (a crossed add may have filled).
    if (book.contains(ref)) orders_[ref] = Resting{locate, side, price, shares, false};
}

// Moves parked orders into the book once the symbol trades again and they no
// longer cross it. Orders still crossing (the auction is mid-way) stay parked.
void Replayer::unpark(std::uint16_t locate) {
    auto queue = parked_.find(locate);
    if (queue == parked_.end() || trading_state_[locate] != 'T') return;
    OrderBook* book = tracked(locate);

    std::vector<OrderId> still;
    for (OrderId ref : queue->second) {
        auto it = orders_.find(ref);
        if (it == orders_.end() || !it->second.parked) continue;  // filled or cancelled
        Resting& order = it->second;
        const auto opposite = order.side == Side::Buy ? book->best_ask() : book->best_bid();
        const bool crosses =
            opposite && (order.side == Side::Buy ? order.price >= *opposite
                                                 : order.price <= *opposite);
        if (crosses) {
            still.push_back(ref);
            continue;
        }
        book->add_limit(ref, order.side, order.price, order.shares);
        order.parked = false;
    }
    if (still.empty()) {
        parked_.erase(queue);
    } else {
        queue->second = std::move(still);
    }
}

void Replayer::reduce(std::uint16_t locate, OrderId ref, Quantity shares, bool execution) {
    OrderBook* book = tracked(locate);
    if (book == nullptr) return;
    ++stats_.book_events;
    if (execution) {
        ++stats_.executions;
        stats_.executed_shares += shares;
    } else {
        ++stats_.cancels;
    }

    auto it = orders_.find(ref);
    if (it == orders_.end()) {
        ++stats_.unknown_refs;
        flag(Anomaly::Kind::UnknownRef, locate, ref);
        return;
    }
    Resting& order = it->second;
    // Only plain 'E' executions must be at the touch: 'C' executions happen
    // at a price other than the order's (auctions among them), and parked
    // orders are by definition outside the book.
    if (msg_type_ == 'E' && !order.parked) {
        const auto best = order.side == Side::Buy ? book->best_bid() : book->best_ask();
        if (best != order.price) {
            ++stats_.off_touch_executions;
            flag(Anomaly::Kind::OffTouchExecution, locate, ref);
        }
    }

    if (shares >= order.shares) {
        if (!order.parked) book->cancel(ref);
        orders_.erase(it);
    } else {
        order.shares -= shares;
        if (!order.parked) book->modify(ref, order.price, order.shares);  // keeps priority
    }
    unpark(locate);
}

void Replayer::erase(std::uint16_t locate, OrderId ref) {
    OrderBook* book = tracked(locate);
    if (book == nullptr) return;
    ++stats_.book_events;
    ++stats_.cancels;
    auto it = orders_.find(ref);
    if (it == orders_.end()) {
        ++stats_.unknown_refs;
        flag(Anomaly::Kind::UnknownRef, locate, ref);
        return;
    }
    if (!it->second.parked) book->cancel(ref);
    orders_.erase(it);
    unpark(locate);
}

void Replayer::replace(std::uint16_t locate, OrderId old_ref, OrderId new_ref, Price price,
                       Quantity shares) {
    OrderBook* book = tracked(locate);
    if (book == nullptr) return;
    ++stats_.book_events;
    ++stats_.replaces;

    auto it = orders_.find(old_ref);
    if (it == orders_.end()) {
        ++stats_.unknown_refs;
        flag(Anomaly::Kind::UnknownRef, locate, old_ref);
        return;
    }
    const Side side = it->second.side;
    if (!it->second.parked) book->cancel(old_ref);
    orders_.erase(it);

    place(*book, locate, new_ref, side, price, shares);
    unpark(locate);
}

void Replayer::flag(Anomaly::Kind kind, std::uint16_t locate, OrderId ref) {
    if (anomalies_.size() < kMaxAnomalies) {
        anomalies_.push_back(Anomaly{kind, msg_type_, trading_state_[locate], msg_time_,
                                     symbol_of_[locate], ref});
    }
}

const OrderBook* Replayer::book(std::string_view symbol) const {
    auto it = locate_of_.find(std::string(symbol));
    return it == locate_of_.end() ? nullptr : books_[it->second].get();
}

std::vector<std::string> Replayer::symbols_seen() const {
    std::vector<std::string> out;
    out.reserve(locate_of_.size());
    for (const auto& [symbol, locate] : locate_of_) out.push_back(symbol);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace lob::itch
