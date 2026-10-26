// Small walkthrough of the order book: rest a few orders, then cross the spread.
#include "lob/order_book.hpp"

#include <iostream>

int main() {
    using lob::Side;
    lob::OrderBook book;

    book.add_limit(1, Side::Buy, 9'990, 100);   // prices in ticks ($99.90)
    book.add_limit(2, Side::Buy, 9'995, 50);
    book.add_limit(3, Side::Sell, 10'005, 80);
    book.add_limit(4, Side::Sell, 10'010, 120);

    std::cout << "Best bid: " << *book.best_bid() << "  Best ask: " << *book.best_ask() << "\n";

    std::cout << "Aggressive buy of 150 @ 10010:\n";
    for (const auto& t : book.add_limit(5, Side::Buy, 10'010, 150)) {
        std::cout << "  trade: buy#" << t.buy_id << " sell#" << t.sell_id << " " << t.qty
                  << " @ " << t.price << "\n";
    }

    std::cout << "Best bid: " << *book.best_bid() << "  Best ask: " << *book.best_ask() << "\n";
    return 0;
}
