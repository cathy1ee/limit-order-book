# Limit Order Book

![CI](https://github.com/cathy1ee/limit-order-book/actions/workflows/ci.yml/badge.svg)
![C++20](https://img.shields.io/badge/C++-20-00599C?style=flat&logo=cplusplus&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)

A price-time priority limit order book and matching engine in C++20, built to be correct first and then fast.

## Features

- Limit orders matched by price-time priority (best price first, then oldest order first)
- Partial fills, multi-level sweeps, and remainders that rest in the book
- Trades execute at the resting order's price, so the aggressor gets price improvement
- O(1) cancel through an order-id index
- Integer tick prices (no floating-point rounding errors)
- Unit tests with GoogleTest, run on every push by GitHub Actions

## Build and run

Requires CMake 3.20+ and a C++20 compiler (GCC 11+, Clang 14+, or Apple Clang 15+).

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # run the tests
./build/demo                                 # run the example
```

GoogleTest is downloaded automatically the first time you configure.

## Usage

```cpp
#include "lob/order_book.hpp"

lob::OrderBook book;
book.add_limit(1, lob::Side::Sell, 10'005, 80);   // rests at the ask
auto trades = book.add_limit(2, lob::Side::Buy, 10'010, 50);
// -> one trade: 50 @ 10005; order 1 keeps 30 resting
book.cancel(1);
```

## Design

| Component | v1 choice | Why |
|---|---|---|
| Price levels | `std::map<Price, Level>` per side | Sorted, so the best price is always `begin()` |
| Orders at a level | `std::list<Order>` (FIFO) | Stable iterators make cancel-from-the-middle O(1) |
| Order lookup | `std::unordered_map<OrderId, Location>` | Cancel without searching the book |
| Prices | `int64_t` ticks | Exact comparisons; no floating point on the hot path |

v1 favors clarity. The roadmap below replaces each piece with a faster structure, and the benchmarks will show the difference.

## Roadmap

- [x] Limit orders with price-time priority matching
- [x] Partial fills and multi-level sweeps
- [x] Cancel
- [ ] Market orders and modify (quantity down keeps priority, price change loses it)
- [ ] Random order-flow generator and benchmark harness (throughput, p50/p99 latency)
- [ ] Optimizations: memory pool, flat price-level array, intrusive lists
- [ ] Replay NASDAQ ITCH 5.0 sample data
- [ ] Benchmark results: before vs. after

## License

MIT
