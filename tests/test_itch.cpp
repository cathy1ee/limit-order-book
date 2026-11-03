// ITCH 5.0 replay tests on hand-built binary messages.
#include "lob/itch.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using lob::Side;
using lob::itch::Replayer;

namespace {

// Builds ITCH messages byte by byte, in the spec's big-endian layout.
class Msg {
public:
    explicit Msg(char type, std::uint16_t locate) {
        u(static_cast<std::uint8_t>(type), 1);
        u(locate, 2);
        u(0, 2);  // tracking number
        u(0, 6);  // timestamp
    }
    Msg& u(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Msg& c(char ch) { return u(static_cast<std::uint8_t>(ch), 1); }
    Msg& stock(const std::string& s) {
        std::string padded = s;
        padded.resize(8, ' ');
        for (char ch : padded) c(ch);
        return *this;
    }
    std::vector<std::uint8_t> bytes;
};

Msg add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t shares,
        const std::string& sym, std::uint32_t price) {
    Msg m('A', loc);
    m.u(ref, 8).c(side).u(shares, 4).stock(sym).u(price, 4);
    return m;
}
Msg add_mpid(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t shares,
             const std::string& sym, std::uint32_t price) {
    Msg m = add(loc, ref, side, shares, sym, price);
    m.c('M').c('P').c('I').c('D');
    m.bytes[0] = 'F';
    return m;
}
Msg exec(std::uint16_t loc, std::uint64_t ref, std::uint32_t shares) {
    Msg m('E', loc);
    m.u(ref, 8).u(shares, 4).u(1, 8);
    return m;
}
Msg exec_price(std::uint16_t loc, std::uint64_t ref, std::uint32_t shares, std::uint32_t price) {
    Msg m('C', loc);
    m.u(ref, 8).u(shares, 4).u(1, 8).c('Y').u(price, 4);
    return m;
}
Msg cancel(std::uint16_t loc, std::uint64_t ref, std::uint32_t shares) {
    Msg m('X', loc);
    m.u(ref, 8).u(shares, 4);
    return m;
}
Msg del(std::uint16_t loc, std::uint64_t ref) {
    Msg m('D', loc);
    m.u(ref, 8);
    return m;
}
Msg replace(std::uint16_t loc, std::uint64_t old_ref, std::uint64_t new_ref,
            std::uint32_t shares, std::uint32_t price) {
    Msg m('U', loc);
    m.u(old_ref, 8).u(new_ref, 8).u(shares, 4).u(price, 4);
    return m;
}

// Concatenates messages with their 2-byte length prefixes, as on disk.
std::vector<std::uint8_t> file(const std::vector<Msg>& msgs) {
    std::vector<std::uint8_t> out;
    for (const auto& m : msgs) {
        out.push_back(static_cast<std::uint8_t>(m.bytes.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(m.bytes.size()));
        out.insert(out.end(), m.bytes.begin(), m.bytes.end());
    }
    return out;
}

constexpr std::uint32_t kPx100 = 1'000'000;  // $100.0000

}  // namespace

TEST(Itch, MessageLengthsMatchTheSpec) {
    EXPECT_EQ(add(1, 1, 'B', 1, "X", 1).bytes.size(), 36u);
    EXPECT_EQ(add_mpid(1, 1, 'B', 1, "X", 1).bytes.size(), 40u);
    EXPECT_EQ(exec(1, 1, 1).bytes.size(), 31u);
    EXPECT_EQ(exec_price(1, 1, 1, 1).bytes.size(), 36u);
    EXPECT_EQ(cancel(1, 1, 1).bytes.size(), 23u);
    EXPECT_EQ(del(1, 1).bytes.size(), 19u);
    EXPECT_EQ(replace(1, 1, 2, 1, 1).bytes.size(), 35u);
}

TEST(Itch, RebuildsTheBookFromEvents) {
    Replayer r;
    const auto data = file({
        add(7, 1, 'B', 100, "AAPL", kPx100),
        add(7, 2, 'B', 200, "AAPL", kPx100),         // behind order 1
        add_mpid(7, 3, 'S', 300, "AAPL", kPx100 + 100),
        add(7, 4, 'S', 50, "AAPL", kPx100 + 200),
        exec(7, 1, 40),          // order 1: 60 left, still first
        cancel(7, 2, 50),        // order 2: 150 left
        exec(7, 1, 60),          // order 1 gone
        del(7, 4),
        replace(7, 3, 5, 120, kPx100 + 50),  // ask moves down to 100.0050
    });
    EXPECT_EQ(r.feed(data), data.size());

    const lob::OrderBook* book = r.book("AAPL");
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->best_bid(), kPx100);
    EXPECT_EQ(book->volume_at(Side::Buy, kPx100), 150u);
    EXPECT_EQ(book->best_ask(), kPx100 + 50);
    EXPECT_EQ(book->volume_at(Side::Sell, kPx100 + 50), 120u);
    EXPECT_FALSE(book->contains(3));
    EXPECT_TRUE(book->contains(5));
    EXPECT_EQ(book->order_count(), 2u);

    const auto& s = r.stats();
    EXPECT_EQ(s.messages, 9u);
    EXPECT_EQ(s.adds, 4u);
    EXPECT_EQ(s.executions, 2u);
    EXPECT_EQ(s.executed_shares, 100u);
    EXPECT_EQ(s.cancels, 2u);
    EXPECT_EQ(s.replaces, 1u);
    EXPECT_EQ(s.crossed_adds, 0u);
    EXPECT_EQ(s.off_touch_executions, 0u);
    EXPECT_EQ(s.unknown_refs, 0u);
}

TEST(Itch, PartialExecutionKeepsQueuePriority) {
    Replayer r;
    r.feed(file({
        add(1, 1, 'S', 100, "MSFT", kPx100),
        add(1, 2, 'S', 100, "MSFT", kPx100),
        exec_price(1, 1, 30, kPx100),
    }));
    EXPECT_EQ(r.book("MSFT")->volume_at(Side::Sell, kPx100), 170u);
    EXPECT_TRUE(r.book("MSFT")->contains(1));
    EXPECT_EQ(r.stats().off_touch_executions, 0u);
}

TEST(Itch, FiltersSymbolsAndSkipsOthers) {
    Replayer r({"AAPL"});
    r.feed(file({
        add(1, 1, 'B', 10, "AAPL", kPx100),
        add(2, 2, 'B', 10, "MSFT", kPx100),
        exec(2, 2, 10),  // MSFT: ignored, not an unknown ref
    }));
    EXPECT_NE(r.book("AAPL"), nullptr);
    EXPECT_EQ(r.book("MSFT"), nullptr);
    EXPECT_EQ(r.stats().book_events, 1u);
    EXPECT_EQ(r.stats().unknown_refs, 0u);
    EXPECT_EQ(r.symbols_seen(), std::vector<std::string>{"AAPL"});
}

TEST(Itch, FlagsInconsistentFeeds) {
    Replayer r;
    r.feed(file({
        add(1, 1, 'B', 10, "AAPL", kPx100),
        add(1, 2, 'B', 10, "AAPL", kPx100 - 100),
        exec(1, 2, 5),                         // not the best bid
        del(1, 99),                            // never added
        add(1, 3, 'S', 10, "AAPL", kPx100),    // crosses the bid
    }));
    const auto& s = r.stats();
    EXPECT_EQ(s.off_touch_executions, 1u);
    EXPECT_EQ(s.unknown_refs, 1u);
    EXPECT_EQ(s.crossed_adds, 1u);
}

TEST(Itch, FeedLeavesPartialMessageForNextCall) {
    Replayer r;
    const auto data = file({add(1, 1, 'B', 10, "AAPL", kPx100), del(1, 1)});
    const std::size_t cut = data.size() - 5;  // split the delete
    const std::size_t used = r.feed({data.data(), cut});
    EXPECT_EQ(used, 2u + 36u);
    EXPECT_TRUE(r.book("AAPL")->contains(1));

    std::vector<std::uint8_t> rest(data.begin() + static_cast<long>(used), data.end());
    EXPECT_EQ(r.feed(rest), rest.size());
    EXPECT_FALSE(r.book("AAPL")->contains(1));
}

TEST(Itch, ShortMessagesAreCountedNotRead) {
    Replayer r;
    auto m = add(1, 1, 'B', 10, "AAPL", kPx100);
    m.bytes.resize(20);
    r.on_message(m.bytes);
    EXPECT_EQ(r.stats().malformed, 1u);
    EXPECT_EQ(r.book("AAPL"), nullptr);
}

namespace {

Msg trading_action(std::uint16_t loc, const std::string& sym, char state, const std::string& reason) {
    Msg m('H', loc);
    m.stock(sym).c(state).c(' ');
    std::string r = reason;
    r.resize(4, ' ');
    for (char ch : r) m.c(ch);
    return m;
}

}  // namespace

TEST(Itch, TradingActionMessageLength) {
    EXPECT_EQ(trading_action(1, "X", 'T', "").bytes.size(), 25u);
}

// A limit up/limit down pause: orders cross while paused, the reopening
// auction executes them ('C' at the auction price), and trading resumes on
// an uncrossed book. None of that is an anomaly.
TEST(Itch, PauseAndReopeningAuctionReplayCleanly) {
    Replayer r;
    const std::uint32_t bid = kPx100, ask = kPx100 + 100;
    r.feed(file({
        add(1, 1, 'B', 100, "SXTC", bid),
        add(1, 2, 'S', 100, "SXTC", ask),
        trading_action(1, "SXTC", 'P', "LUDP"),
        add(1, 3, 'B', 60, "SXTC", ask + 100),   // crosses: parked
        add(1, 4, 'S', 50, "SXTC", bid - 100),   // crosses the bid: parked
        add(1, 5, 'B', 10, "SXTC", bid - 200),   // does not cross: rests
        del(1, 4),                               // cancelled while parked
        trading_action(1, "SXTC", 'T', ""),
        exec_price(1, 3, 60, ask),               // auction: 3 buys 60 of 2's 100
        exec_price(1, 2, 60, ask),
    }));

    const auto& s = r.stats();
    EXPECT_EQ(s.parked_adds, 2u);
    EXPECT_EQ(s.crossed_adds, 0u);
    EXPECT_EQ(s.off_touch_executions, 0u);
    EXPECT_EQ(s.unknown_refs, 0u);

    const lob::OrderBook* book = r.book("SXTC");
    EXPECT_EQ(book->best_bid(), bid);
    EXPECT_EQ(book->best_ask(), ask);
    EXPECT_EQ(book->volume_at(Side::Sell, ask), 40u);
    EXPECT_FALSE(book->contains(3));  // fully executed in the auction
    EXPECT_FALSE(book->contains(4));
    EXPECT_TRUE(book->contains(5));
}

TEST(Itch, ParkedOrderJoinsBookWhenItNoLongerCrosses) {
    Replayer r;
    r.feed(file({
        add(1, 1, 'S', 100, "PHUN", kPx100),
        trading_action(1, "PHUN", 'P', "LUDP"),
        add(1, 2, 'B', 30, "PHUN", kPx100 + 500),  // parked
        trading_action(1, "PHUN", 'T', ""),        // still crosses order 1: stays parked
        del(1, 1),                                 // ask gone: order 2 can rest
    }));
    const lob::OrderBook* book = r.book("PHUN");
    EXPECT_TRUE(book->contains(2));
    EXPECT_EQ(book->best_bid(), kPx100 + 500);
    EXPECT_EQ(r.stats().crossed_adds, 0u);
}

TEST(Itch, CrossingAddWhileTradingIsStillFlagged) {
    Replayer r;
    r.feed(file({
        add(1, 1, 'S', 100, "AAPL", kPx100),
        trading_action(1, "AAPL", 'T', ""),
        add(1, 2, 'B', 30, "AAPL", kPx100),
    }));
    EXPECT_EQ(r.stats().parked_adds, 0u);
    EXPECT_EQ(r.stats().crossed_adds, 1u);
}
