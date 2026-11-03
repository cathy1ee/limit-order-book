// Replays a NASDAQ TotalView-ITCH 5.0 file through the order book.
//
//   ./build/itch_replay FILE [SYMBOL...]
//   gunzip -c 01302019.NASDAQ_ITCH50.gz | ./build/itch_replay - AAPL MSFT
//
// With no symbols, every stock is tracked. Prints message counts, the
// consistency checks, replay throughput, and the closing top of book.
#include "lob/itch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void print_price(std::optional<lob::Price> p) {
    if (p) {
        std::printf("%10.4f", static_cast<double>(*p) / 10'000.0);  // ITCH: 4 decimals
    } else {
        std::printf("%10s", "-");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE|- [SYMBOL...]\n", argv[0]);
        return 2;
    }

    std::FILE* in = std::strcmp(argv[1], "-") == 0 ? stdin : std::fopen(argv[1], "rb");
    if (in == nullptr) {
        std::perror(argv[1]);
        return 1;
    }

    lob::itch::Replayer replayer(std::vector<std::string>(argv + 2, argv + argc));

    // Read in large chunks; a message split across chunks is carried over.
    std::vector<std::uint8_t> buf(std::size_t{1} << 22);
    std::size_t filled = 0;
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const std::size_t got = std::fread(buf.data() + filled, 1, buf.size() - filled, in);
        if (got == 0) break;
        filled += got;
        const std::size_t used = replayer.feed({buf.data(), filled});
        std::memmove(buf.data(), buf.data() + used, filled - used);
        filled -= used;
    }
    const std::chrono::duration<double> secs = std::chrono::steady_clock::now() - start;
    if (in != stdin) std::fclose(in);

    const auto& s = replayer.stats();
    std::printf("messages            %llu (%.1f M msg/s)\n",
                static_cast<unsigned long long>(s.messages),
                static_cast<double>(s.messages) / secs.count() / 1e6);
    std::printf("book events         %llu\n", static_cast<unsigned long long>(s.book_events));
    std::printf("  adds              %llu\n", static_cast<unsigned long long>(s.adds));
    std::printf("  executions        %llu (%llu shares)\n",
                static_cast<unsigned long long>(s.executions),
                static_cast<unsigned long long>(s.executed_shares));
    std::printf("  cancels/deletes   %llu\n", static_cast<unsigned long long>(s.cancels));
    std::printf("  replaces          %llu\n", static_cast<unsigned long long>(s.replaces));
    std::printf("  parked in halts   %llu\n", static_cast<unsigned long long>(s.parked_adds));
    std::printf("checks (expect 0)\n");
    std::printf("  crossed adds      %llu\n", static_cast<unsigned long long>(s.crossed_adds));
    std::printf("  off-touch execs   %llu\n",
                static_cast<unsigned long long>(s.off_touch_executions));
    std::printf("  unknown refs      %llu\n", static_cast<unsigned long long>(s.unknown_refs));
    std::printf("  malformed         %llu\n", static_cast<unsigned long long>(s.malformed));
    if (filled != 0) std::printf("  trailing bytes    %zu (truncated file?)\n", filled);

    using Kind = lob::itch::Replayer::Anomaly::Kind;
    const auto& anomalies = replayer.anomalies();
    if (!anomalies.empty()) {
        const std::size_t show = std::min<std::size_t>(anomalies.size(), 300);
        std::printf("\nflagged events (first %zu)\n", show);
        for (std::size_t i = 0; i < show; ++i) {
            const auto& a = anomalies[i];
            const char* kind = a.kind == Kind::CrossedAdd          ? "crossed-add"
                             : a.kind == Kind::OffTouchExecution ? "off-touch"
                                                                 : "unknown-ref";
            const std::uint64_t secs = a.time_ns / 1'000'000'000;
            std::printf("  %02llu:%02llu:%02llu.%09llu  %-11s %c  state %c  %-6s ref %llu\n",
                        static_cast<unsigned long long>(secs / 3600),
                        static_cast<unsigned long long>(secs / 60 % 60),
                        static_cast<unsigned long long>(secs % 60),
                        static_cast<unsigned long long>(a.time_ns % 1'000'000'000), kind,
                        a.msg_type, a.trading_state, a.symbol.c_str(),
                        static_cast<unsigned long long>(a.ref));
        }

        // Trading-state changes for the symbols that had flagged events.
        std::printf("\ntrading-state changes for flagged symbols\n");
        for (const auto& c : replayer.state_changes()) {
            const bool flagged = std::any_of(anomalies.begin(), anomalies.end(),
                                             [&](const auto& a) { return a.symbol == c.symbol; });
            if (!flagged) continue;
            const std::uint64_t secs = c.time_ns / 1'000'000'000;
            std::printf("  %02llu:%02llu:%02llu  %-6s -> %c  %s\n",
                        static_cast<unsigned long long>(secs / 3600),
                        static_cast<unsigned long long>(secs / 60 % 60),
                        static_cast<unsigned long long>(secs % 60), c.symbol.c_str(), c.state,
                        c.reason.c_str());
        }
    }

    const auto symbols = replayer.symbols_seen();
    std::size_t resting = 0, nonempty = 0;
    for (const auto& sym : symbols) {
        const std::size_t n = replayer.book(sym)->order_count();
        resting += n;
        nonempty += n != 0;
    }
    std::printf("\nat end: %zu orders resting in %zu of %zu books\n", resting, nonempty,
                symbols.size());

    std::printf("\n%-8s %10s %10s %8s\n", "symbol", "bid", "ask", "orders");
    std::size_t shown = 0;
    for (const auto& sym : symbols) {
        if (shown++ == 20) {
            std::printf("... %zu more\n", symbols.size() - 20);
            break;
        }
        const lob::OrderBook* book = replayer.book(sym);
        std::printf("%-8s ", sym.c_str());
        print_price(book->best_bid());
        std::printf(" ");
        print_price(book->best_ask());
        std::printf(" %8zu\n", book->order_count());
    }
    return 0;
}
