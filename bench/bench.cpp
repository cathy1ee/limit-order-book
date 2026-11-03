// Benchmark: replays the same random order flow through each book and
// reports throughput and per-operation latency percentiles.
//
//   ./build/bench [n_ops] [seed]
//
// Each book gets two passes on a fresh copy of the flow: one timing the whole
// loop (throughput), one timing every operation (latency). The first
// kWarmup operations build the book up to its target depth and are not timed.
#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "lob/order_flow.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kWarmup = 50'000;

struct Result {
    double mops;  // million operations per second
    double p50_ns, p99_ns, p999_ns, max_ns;
    std::size_t trades;
};

double percentile(const std::vector<double>& sorted, double p) {
    const auto i = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[i];
}

template <typename Book>
Result run(const std::vector<lob::Op>& ops) {
    Result r{};
    const std::size_t timed = ops.size() - kWarmup;

    {  // throughput
        Book book;
        for (std::size_t i = 0; i < kWarmup; ++i) lob::apply(book, ops[i]);
        std::size_t trades = 0;
        const auto start = Clock::now();
        for (std::size_t i = kWarmup; i < ops.size(); ++i) trades += lob::apply(book, ops[i]).size();
        const std::chrono::duration<double> secs = Clock::now() - start;
        r.mops = static_cast<double>(timed) / secs.count() / 1e6;
        r.trades = trades;
    }

    {  // latency
        Book book;
        for (std::size_t i = 0; i < kWarmup; ++i) lob::apply(book, ops[i]);
        std::vector<double> ns;
        ns.reserve(timed);
        for (std::size_t i = kWarmup; i < ops.size(); ++i) {
            const auto t0 = Clock::now();
            auto trades = lob::apply(book, ops[i]);
            const auto t1 = Clock::now();
            ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        std::sort(ns.begin(), ns.end());
        r.p50_ns = percentile(ns, 0.50);
        r.p99_ns = percentile(ns, 0.99);
        r.p999_ns = percentile(ns, 0.999);
        r.max_ns = ns.back();
    }
    return r;
}

// Smallest non-zero step the clock reports, so readers know how coarse the
// latency numbers are (about 42 ns on Apple Silicon).
double clock_resolution_ns() {
    double best = 1e9;
    for (int i = 0; i < 1000; ++i) {
        const auto t0 = Clock::now();
        auto t1 = Clock::now();
        while (t1 == t0) t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return best;
}

void print_row(const char* name, const Result& r) {
    std::printf("| %-12s | %8.2f | %7.0f | %7.0f | %8.0f | %9.0f |\n", name, r.mops, r.p50_ns,
                r.p99_ns, r.p999_ns, r.max_ns);
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n_ops = argc > 1 ? std::stoul(argv[1]) : 2'000'000;
    lob::FlowConfig cfg;
    if (argc > 2) cfg.seed = std::stoull(argv[2]);

    const auto ops = lob::OrderFlow(cfg).generate(n_ops + kWarmup);

    const Result before = run<lob::MapOrderBook>(ops);
    const Result after = run<lob::OrderBook>(ops);
    if (before.trades != after.trades) {
        std::fprintf(stderr, "books disagree: %zu vs %zu trades\n", before.trades, after.trades);
        return 1;
    }

    std::printf("%zu ops (seed %llu, after %zu warm-up ops), %zu trades, clock step %.0f ns\n\n",
                n_ops, static_cast<unsigned long long>(cfg.seed), kWarmup, after.trades,
                clock_resolution_ns());
    std::printf("| Book         | Mops/s   | p50 ns  | p99 ns  | p99.9 ns | max ns    |\n");
    std::printf("|--------------|---------:|--------:|--------:|---------:|----------:|\n");
    print_row("MapOrderBook", before);
    print_row("OrderBook", after);
    std::printf("\nSpeedup: %.2fx throughput\n", after.mops / before.mops);
    return 0;
}
