# Limit Order Book

![CI](https://github.com/cathy1ee/limit-order-book/actions/workflows/ci.yml/badge.svg)
![C++20](https://img.shields.io/badge/C++-20-00599C?style=flat&logo=cplusplus&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)

A price-time priority limit order book and matching engine in C++20, built to be correct first and then fast.

## Features

- Limit orders matched by price-time priority (best price first, then oldest order first)
- Partial fills, multi-level sweeps, and remainders that rest in the book
- Trades execute at the resting order's price, so the aggressor gets price improvement
- Market orders that sweep the book and never rest
- O(1) cancel through an order-id index
- Modify: reducing size keeps time priority; a price change or size increase loses it
- Integer tick prices (no floating-point rounding errors)
- Optimized book (memory pool, flat price-level array, intrusive lists): **~1.5x the v1 throughput**, ~38M ops/s
- Differential tests: random order flow through the optimized book and the v1 reference must give identical trades after every operation
- NASDAQ TotalView-ITCH 5.0 replay: a full trading day (368M messages, 8,695 symbols) replays with zero consistency errors
- GoogleTest suite, also run under AddressSanitizer and UndefinedBehaviorSanitizer in CI

## Build and run

Requires CMake 3.20+ and a C++20 compiler (GCC 11+, Clang 14+, or Apple Clang 15+).

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # run the tests
./build/demo                                 # run the example
./build/bench                                # benchmark: v1 vs. optimized
./build/itch_replay FILE [SYMBOL...]         # replay an ITCH 5.0 file
```

GoogleTest is downloaded automatically the first time you configure.

## Usage

```cpp
#include "lob/order_book.hpp"

lob::OrderBook book;
book.add_limit(1, lob::Side::Sell, 10'005, 80);   // rests at the ask
auto trades = book.add_limit(2, lob::Side::Buy, 10'010, 50);
// -> one trade: 50 @ 10005; order 1 keeps 30 resting
book.modify(1, 10'005, 20);                       // size down: keeps its place in line
book.add_market(3, lob::Side::Buy, 100);          // takes what's there; the rest is dropped
```

## Design

There are two books with the same interface. `lob::MapOrderBook` is v1, written for clarity. It is kept as the reference that the tests check against and as the benchmark baseline. `lob::OrderBook` is the optimized one.

| Component | v1 (`MapOrderBook`) | Optimized (`OrderBook`) |
|---|---|---|
| Price levels | `std::map<Price, Level>` per side | Flat array indexed by price, one for both sides, plus a bitmap of non-empty levels |
| Finding the next best price | `map::begin()` after erasing a level | Scan the bitmap 64 levels per word (`std::countr_zero`) |
| Orders at a level | `std::list<Order>` (FIFO) | Intrusive doubly-linked list: a level is two pointers and a running total |
| Order storage | One heap allocation per order | Memory pool with a free list, allocating in 4,096-order chunks |
| `volume_at` | Walk the level's orders | O(1) running total |
| Order lookup | `std::unordered_map<OrderId, Location>` | `std::unordered_map<OrderId, Node*>` |
| Prices | `int64_t` ticks | `int64_t` ticks |

The flat array covers a window of 65,536 ticks centered on the first price that rests. Prices outside it go to a `std::map` overflow, so a stray far-away order never breaks correctness. It only takes the slow path. Bids and asks can share one array because the book is never crossed: every level above the best bid holds asks, and every level below the best ask holds bids.

Correctness comes from **differential testing**. `tests/test_differential.cpp` replays 100k-operation random flows through both books and requires identical trades, best prices, depth at the touch and order counts after every operation. That includes a thin, aggressive book that keeps emptying, and a 64-tick window that pushes most levels into the overflow map.

## Benchmark

`./build/bench [n_ops] [seed]` generates a reproducible random order flow (`include/lob/order_flow.hpp`): 55% limit, 30% cancel, 10% modify, 5% market. About 8% of the limits cross the spread, and the book holds about 5,000 resting orders. It replays the same flow through both books, once for throughput and once timing each operation.

Apple M5 Max, Apple Clang 21, `-O3`, 2M operations after 50k warm-up, 630k trades:

| Book | Mops/s | p50 | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|
| `MapOrderBook` (v1) | 25.5 | 1 clock tick | 125 | 208 |
| `OrderBook` | **37.9** | 0 ticks | **84** | **125** |

**1.49x throughput** (1.45x with seed 43), and about a third off the tail latency. The median operation finishes within one tick of the Apple Silicon clock (41.7 ns), so p50 is shown in clock ticks. It only says both books are at or below what the clock can resolve. The tail percentiles are the meaningful latency numbers. The largest remaining costs are the node-based `std::unordered_map` index and the `std::vector<Trade>` returned by every call. An open-addressing index and a caller-supplied trade buffer are the next steps.

## ITCH 5.0 replay

`itch_replay` reads a [NASDAQ TotalView-ITCH 5.0](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf) file and rebuilds a book per symbol. It reads from stdin with `-`, so you don't need to decompress the multi-GB sample files to disk:

```bash
gunzip -c 01302019.NASDAQ_ITCH50.gz | ./build/itch_replay - AAPL MSFT
```

ITCH is the exchange's output, so the matching has already happened. Replay applies the book events: adds (`A`/`F`) rest, executions (`E`/`C`) and partial cancels (`X`) reduce an order in place and keep its queue position, deletes (`D`) cancel, and replaces (`U`) re-enter the order at the back of the queue. It also checks the engine against real flow and counts any:
- add that trades on arrival
- `E` execution that isn't at the best price
- event for an order it never saw

All three should be zero.

**Halts and pauses.** While a stock is halted or in a limit up/limit down pause, Nasdaq lets its book cross and then clears it with a reopening auction. A continuous matching engine can't hold a crossed book. So in those states, the replayer keeps crossing adds outside the engine, applies the auction's executions to them, and moves them into the book once they no longer cross it. The replayer tracks each stock's state from the `H` (Stock Trading Action) messages.

### Results: Nasdaq, January 30 2019

The full sample day `01302019.NASDAQ_ITCH50.gz` (4.8 GB compressed, 11.2 GB raw), all symbols, on an Apple M5 Max:

| | |
|---|---:|
| Messages | 368,366,634 |
| Book events applied | 363,118,215 |
| Adds / executions / cancels+deletes / replaces | 164.7M / 8.26M / 162.9M / 27.2M |
| Shares executed | 1,119,323,998 |
| Symbols | 8,695 |
| Crossing orders parked during halts | 131 (5 pauses/halts in SXTC, PHUN, DLPN, EYEN) |
| **Crossed adds / off-touch executions / unknown refs** | **0 / 0 / 0** |
| Orders resting after the close | 0 in all 8,695 books (Nasdaq removes day orders at the close) |
| Time | 142 s (2.6M msg/s), 1.9 GB peak memory |

Before halt handling was added, the same replay flagged 216 events. All 216 were in those five halts: crossed adds during the pause, then auction executions on orders the engine had already wrongly matched. The tool prints each flagged event with its time, symbol and trading state, which made that easy to trace.

Decompression takes only 12 s of the 142, so the replay is CPU-bound. It's much slower per event than the benchmark because 8,695 books and two order-id hash maps (the replayer's and each book's) don't fit in cache.

## Roadmap

- [x] Limit orders with price-time priority matching
- [x] Partial fills and multi-level sweeps
- [x] Cancel
- [x] Market orders and modify (quantity down keeps priority, price change loses it)
- [x] Random order-flow generator and benchmark harness (throughput, p50/p99 latency)
- [x] Optimizations: memory pool, flat price-level array, intrusive lists
- [x] NASDAQ ITCH 5.0 parser and replay tool
- [x] Benchmark results: before vs. after
- [x] Replay results on a full NASDAQ sample day (zero consistency errors)
- [ ] Open-addressing order index and caller-supplied trade buffer

## License

MIT
