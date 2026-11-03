#pragma once

#include "lob/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lob::itch {

// Rebuilds per-symbol order books from a NASDAQ TotalView-ITCH 5.0 feed.
//
// ITCH is the exchange's output: matching already happened, so replay means
// applying the feed's book events, not re-matching. The messages used are
//   A / F  add order (F carries an MPID)     -> add_limit
//   E / C  order executed (C at a new price) -> reduce, keeping priority
//   X      partial cancel                    -> reduce, keeping priority
//   D      delete                            -> cancel
//   U      replace (new ref, loses priority) -> cancel + add_limit
//   H      trading action (halt/pause/resume) -> tracked per symbol
// Everything else (system events, trades of hidden orders, auction
// imbalances, ...) is counted and skipped. Prices stay in ITCH's units:
// 1/10000 of a dollar.
//
// Halts and limit up/limit down pauses: the book may cross while a symbol is
// not trading, and a reopening auction then clears it. Crossing adds in that
// window are parked outside the engine and enter the book once the auction's
// executions have uncrossed it.
//
// Replay doubles as a consistency check of the engine against real flow: an
// add that trades on arrival, an execution that is not at the best price, or
// an event for an order we never saw are all counted in Stats.
class Replayer {
public:
    struct Stats {
        std::uint64_t messages = 0;      // every message seen
        std::uint64_t book_events = 0;   // A/F/E/C/X/D/U for tracked symbols
        std::uint64_t adds = 0;
        std::uint64_t executions = 0;
        std::uint64_t executed_shares = 0;
        std::uint64_t cancels = 0;       // X and D
        std::uint64_t replaces = 0;
        std::uint64_t parked_adds = 0;   // crossing adds held during a halt or pause
        std::uint64_t crossed_adds = 0;          // add/replace that traded: should be 0
        std::uint64_t off_touch_executions = 0;  // 'E' on an order not at best price
        std::uint64_t unknown_refs = 0;          // event for an order not in the book
        std::uint64_t malformed = 0;             // message shorter than its type needs
    };

    // One flagged event, kept so a replay's anomalies can be explained.
    struct Anomaly {
        enum class Kind : std::uint8_t { CrossedAdd, OffTouchExecution, UnknownRef };
        Kind          kind;
        char          msg_type;     // ITCH message type that triggered it
        char          trading_state;  // the symbol's 'H' state then: T = trading
        std::uint64_t time_ns;      // nanoseconds since midnight
        std::string   symbol;
        OrderId       ref;
    };
    static constexpr std::size_t kMaxAnomalies = 10'000;  // the rest are only counted

    // A Stock Trading Action ('H') message: halts, pauses and resumptions.
    struct StateChange {
        std::uint64_t time_ns;
        std::string   symbol;
        char          state;   // H halted, P paused, Q quotation only, T trading
        std::string   reason;  // e.g. "LUDP" (limit up/down pause), "T1" (news)
    };

    // symbols: tickers to track (e.g. {"AAPL", "MSFT"}); empty tracks all.
    // window_levels is passed to each OrderBook; tracking every symbol
    // defaults to a small window to keep memory down.
    explicit Replayer(std::vector<std::string> symbols = {}, std::size_t window_levels = 0);

    // One message, without the 2-byte length prefix.
    void on_message(std::span<const std::uint8_t> msg);

    // A buffer of length-prefixed messages (the on-disk ITCH 5.0 format).
    // Returns the number of bytes consumed; a trailing partial message is
    // left for the next call.
    std::size_t feed(std::span<const std::uint8_t> data);

    const Stats& stats() const { return stats_; }
    const std::vector<Anomaly>& anomalies() const { return anomalies_; }
    const std::vector<StateChange>& state_changes() const { return state_changes_; }

    // The book for a symbol, or nullptr if it was not tracked or never seen.
    const OrderBook* book(std::string_view symbol) const;
    std::vector<std::string> symbols_seen() const;

private:
    struct Resting {
        std::uint16_t locate;
        Side          side;
        Price         price;
        Quantity      shares;
        bool          parked;  // held outside the book until an auction uncrosses it
    };

    OrderBook* book_for(std::uint16_t locate, std::string_view stock);  // A/F: may create
    OrderBook* tracked(std::uint16_t locate);                           // others: lookup only
    void add(std::uint16_t locate, std::string_view stock, OrderId ref, Side side,
             Price price, Quantity shares);
    void reduce(std::uint16_t locate, OrderId ref, Quantity shares, bool execution);
    void erase(std::uint16_t locate, OrderId ref);
    void replace(std::uint16_t locate, OrderId old_ref, OrderId new_ref, Price price,
                 Quantity shares);
    void flag(Anomaly::Kind kind, std::uint16_t locate, OrderId ref);
    void place(OrderBook& book, std::uint16_t locate, OrderId ref, Side side, Price price,
               Quantity shares);
    void unpark(std::uint16_t locate);

    enum class Track : std::uint8_t { Unknown, Skip, Active };

    std::vector<std::string>                 wanted_;
    std::size_t                              window_levels_;
    std::vector<Track>                       track_;  // by stock locate code
    std::vector<std::unique_ptr<OrderBook>>  books_;  // by stock locate code
    std::unordered_map<std::string, std::uint16_t> locate_of_;
    std::vector<std::string>                 symbol_of_;  // by locate, tracked only
    std::unordered_map<OrderId, Resting>     orders_;  // shares/price per live ref
    std::unordered_map<std::uint16_t, std::vector<OrderId>> parked_;  // by locate, arrival order
    Stats                                    stats_;
    std::vector<Anomaly>                     anomalies_;
    std::vector<char>                        trading_state_;  // by locate, from 'H'
    std::vector<StateChange>                 state_changes_;  // all symbols
    char                                     msg_type_ = 0;  // current message
    std::uint64_t                            msg_time_ = 0;
};

}  // namespace lob::itch
