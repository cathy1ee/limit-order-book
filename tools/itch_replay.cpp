// Replays a NASDAQ TotalView-ITCH 5.0 file through the order book.
//
//   ./build/itch_replay FILE [SYMBOL...]
//   gunzip -c 01302019.NASDAQ_ITCH50.gz | ./build/itch_replay - AAPL MSFT
//
// With no symbols, every stock is tracked. Prints message counts, the
// consistency checks, replay throughput, and the closing top of book.
#include "lob/itch.hpp"

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
    std::printf("checks (expect 0)\n");
    std::printf("  crossed adds      %llu\n", static_cast<unsigned long long>(s.crossed_adds));
    std::printf("  off-touch execs   %llu\n",
                static_cast<unsigned long long>(s.off_touch_executions));
    std::printf("  unknown refs      %llu\n", static_cast<unsigned long long>(s.unknown_refs));
    std::printf("  malformed         %llu\n", static_cast<unsigned long long>(s.malformed));
    if (filled != 0) std::printf("  trailing bytes    %zu (truncated file?)\n", filled);

    const auto symbols = replayer.symbols_seen();
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
