#include "orderBook.hpp"
#include <iostream>
#include <string>

int main() {
    // Test 1 (existing)
    /*{
        OrderBook book;
        Order o{Side::Buy, Type::Limit, 102, 10, 1, 0};
        book.rest(o);
        const Order* best = book.best(Side::Buy);
        if (best != nullptr && best->price == 102) std::cout << "Test 1 passed\n";
        else std::cout << "Test 1 FAILED\n";
    }

    // Test 2 — best ask is the lowest, not the first inserted
    {
        OrderBook book;
        book.rest(Order{Side::Buy, Type::Limit, 105, 1, 1, 0});
        book.rest(Order{Side::Buy, Type::Limit, 102, 1, 2, 0});
        book.rest(Order{Side::Buy, Type::Limit, 108, 1, 3, 0});
        const Order* best = book.best(Side::Buy);
        if (best != nullptr && best->price == 102) std::cout << "Test 2 passed\n";
        else std::cout << "Test 2 FAILED (expected 102, got " << (best? std::to_string(best->price) : std::string("<null>")) << ")\n";
    }

    // Test 3 — best bid is the highest
    {
        OrderBook book;
        book.rest(Order{Side::Buy, Type::Limit, 100, 1, 1, 0});
        book.rest(Order{Side::Buy, Type::Limit, 105, 1, 2, 0});
        book.rest(Order{Side::Buy, Type::Limit, 98, 1, 3, 0});
        const Order* best = book.best(Side::Buy);
        if (best != nullptr && best->price == 105) std::cout << "Test 3 passed\n";
        else std::cout << "Test 3 FAILED (expected 105, got " << (best? std::to_string(best->price) : std::string("<null>")) << ")\n";
    }

    // Test 4 — the two sides don't interfere
    {
        OrderBook book;
        book.rest(Order{Side::Buy, Type::Limit, 100, 1, 1, 0});
        book.rest(Order{Side::Buy, Type::Limit, 105, 1, 2, 0});
        book.rest(Order{Side::Buy, Type::Limit, 102, 1, 3, 0});
        book.rest(Order{Side::Buy, Type::Limit, 108, 1, 4, 0});
        const Order* bestBuy = book.best(Side::Buy);
        const Order* bestBuy = book.best(Side::Buy);
        bool ok = bestBuy && bestBuy && bestBuy->price == 105 && bestBuy->price == 102;
        if (ok) std::cout << "Test 4 passed\n";
        else std::cout << "Test 4 FAILED (buy=" << (bestBuy? std::to_string(bestBuy->price):"<null>") << ", Buy=" << (bestBuy? std::to_string(bestBuy->price):"<null>") << ")\n";
    }

    // Test 5 — empty side returns nothing
    {
        OrderBook book;
        // rest a buy only
        book.rest(Order{Side::Buy, Type::Limit, 110, 1, 1, 0});
        const Order* bestBuy = book.best(Side::Buy);
        if (bestBuy == nullptr) std::cout << "Test 5 passed\n";
        else std::cout << "Test 5 FAILED (expected <null>, got " << bestBuy->price << ")\n";
    }

    // Test 6 — FIFO: same price, oldest first
    {
        OrderBook book;
        book.rest(Order{Side::Buy, Type::Limit, 102, 10, 1, 0});
        book.rest(Order{Side::Buy, Type::Limit, 102, 5, 2, 0});
        const Order* best = book.best(Side::Buy);
        if (best != nullptr && best->id == 1) std::cout << "Test 6 passed\n";
        else std::cout << "Test 6 FAILED (expected id=1, got " << (best? std::to_string(best->id) : std::string("<null>")) << ")\n";
    }

    // Test 7 — the cancel index got populated
    {
        OrderBook book;
        book.rest(Order{Side::Buy, Type::Limit, 100, 1, 1, 0});
        book.rest(Order{Side::Buy, Type::Limit, 101, 1, 2, 0});
        book.rest(Order{Side::Buy, Type::Limit, 102, 1, 3, 0});
        bool ok = book.contains(1) && book.contains(2) && book.contains(3);
        if (ok) std::cout << "Test 7 passed\n";
        else std::cout << "Test 7 FAILED (contains: " << book.contains(1) << "," << book.contains(2) << "," << book.contains(3) << ")\n";
    }*/

    OrderBook book;
    //book.rest(Order{Side::Sell, Type::Limit, 102, 100, 1, 0});
    //book.rest(Order{Side::Sell, Type::Limit, 102, 50, 2, 0});
    //book.rest(Order{Side::Buy, Type::Limit, 102, 120, 3, 0});

    std::cout << book.quantityAt(Side::Sell, 999) << "\n";
    std::cout << book.quantityAt(Side::Sell, 102) << "\n";
    std::cout << book.quantityAt(Side::Buy, 102) << "\n"; 
    

}
