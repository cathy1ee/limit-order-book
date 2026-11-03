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
                      kCancelLen = 23, kDeleteLen = 19, kReplaceLen = 35;

constexpr std::size_t kDefaultWindowOne = std::size_t{1} << 16;  // tracking a few symbols
constexpr std::size_t kDefaultWindowAll = std::size_t{1} << 10;  // tracking ~9,000

}  // namespace

Replayer::Replayer(std::vector<std::string> symbols, std::size_t window_levels)
    : wanted_(std::move(symbols)),
      window_levels_(window_levels != 0 ? window_levels
                     : wanted_.empty()  ? kDefaultWindowAll
                                        : kDefaultWindowOne),
      track_(1 << 16, Track::Unknown),
      books_(1 << 16) {}

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
    if (!book->add_limit(ref, side, price, shares).empty()) ++stats_.crossed_adds;
    // Track it only if it is resting (a crossed add may have filled).
    if (book->contains(ref)) orders_[ref] = Resting{locate, side, price, shares};
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
        return;
    }
    Resting& order = it->second;
    if (execution) {
        const auto best = order.side == Side::Buy ? book->best_bid() : book->best_ask();
        if (best != order.price) ++stats_.off_touch_executions;
    }

    if (shares >= order.shares) {
        book->cancel(ref);
        orders_.erase(it);
    } else {
        order.shares -= shares;
        book->modify(ref, order.price, order.shares);  // size down: keeps priority
    }
}

void Replayer::erase(std::uint16_t locate, OrderId ref) {
    OrderBook* book = tracked(locate);
    if (book == nullptr) return;
    ++stats_.book_events;
    ++stats_.cancels;
    if (orders_.erase(ref) == 0) {
        ++stats_.unknown_refs;
        return;
    }
    book->cancel(ref);
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
        return;
    }
    const Side side = it->second.side;
    orders_.erase(it);
    book->cancel(old_ref);

    if (!book->add_limit(new_ref, side, price, shares).empty()) ++stats_.crossed_adds;
    if (book->contains(new_ref)) orders_[new_ref] = Resting{locate, side, price, shares};
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
