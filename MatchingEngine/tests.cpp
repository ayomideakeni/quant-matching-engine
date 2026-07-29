#include "orderBook.hpp"
#include <variant>
#include <string>
#include <optional>
#include <iostream>
#include <random>
#include <chrono>
#include <cassert>

//enum class OpType { Submit, Cancel, Modify};

struct LoggedOp{

    OpType type;
    Order order;
    Id id;
    std::optional<Price> newPrice;
    std::optional<Quantity> newQuantity;
};

std::ostream& operator<<(std::ostream& os, const OrderBook::Fill& fill) {
    os << "(" << fill.price << "," << fill.quantity << "," << fill.agressorId << "," << fill.restingId << ")";
    return os;
}

std::ostream& operator<<(std::ostream& os, const OrderBook::ExpectedLevel& state) {
    os << "(" << static_cast<int>(state.side) << "," << state.price << "," << state.quantity << ")";
    return os;
}


template <typename S>
std::ostream& operator<<(std::ostream& os, const std::vector<S>& vector) {
    for (const auto& i : vector) {
        os << i << " ";
    }
    return os;
}

 struct generator{
        std::mt19937 rng;
        std::vector<Id> restingIds;
        Id nextId = 1;
    };

struct Test {
    struct Modifications {
        Id id;
        std::optional<Price> newPrice;
        std::optional<Quantity> newQuantity;
    };

    struct Failure {
        std::string failureType;
        std::string actualResult;
        std::string expectedResult;

        
        friend std::ostream& operator<<(std::ostream& os, const Failure& failure) {
            os << "[" << failure.failureType << " mismatch - Expected: " << failure.expectedResult 
               << ", Got: " << failure.actualResult << "]";
            return os;
        }
    };



    std::vector<Failure> compareFills(
        const std::vector<OrderBook::Fill>& actualFills, 
        const std::vector<OrderBook::Fill>& expectedFills) {
        
        std::vector<Failure> failures;
        
        if (expectedFills.size() == actualFills.size()) {
            for (size_t i = 0; i < expectedFills.size(); ++i) {
                if (actualFills[i].price != expectedFills[i].price) {
                    failures.push_back({"Price", std::to_string(actualFills[i].price), std::to_string(expectedFills[i].price)});
                }
                if (actualFills[i].quantity != expectedFills[i].quantity) {
                    failures.push_back({"Quantity", std::to_string(actualFills[i].quantity), std::to_string(expectedFills[i].quantity)});
                }
                if (actualFills[i].agressorId != expectedFills[i].agressorId) {
                    failures.push_back({"AggressorId", std::to_string(actualFills[i].agressorId), std::to_string(expectedFills[i].agressorId)});
                }
                if (actualFills[i].restingId != expectedFills[i].restingId) {
                    failures.push_back({"RestingId", std::to_string(actualFills[i].restingId), std::to_string(expectedFills[i].restingId)});
                }
            }
        } else {
            failures.push_back({"Size", std::to_string(actualFills.size()), std::to_string(expectedFills.size())});    
        }
        
        return failures;
    }

    std::vector<OrderBook::Fill> submitAndCollect(OrderBook& book, std::vector<Order>& orders){
        std::vector<OrderBook::Fill> actualFills;
        for (auto& order : orders) {
            auto fills = book.submit(order);
            if(fills.has_value()){
                actualFills.insert(actualFills.end(), fills->begin(), fills->end());
            }
        }
        return actualFills;
    }
    bool ValidationTest(OrderBook& book, std::vector<Order>& orders, const std::vector<bool>& expectedAcceptances){
        for(size_t i = 0; i < orders.size(); ++i){
            auto submission = book.submit(orders[i]);
            if(submission.has_value() != expectedAcceptances[i]) return false;
        }

        return true;
    }

    bool runReplayTest(const std::string& name,
                       std::vector<Order>& sequence,
                       const std::vector<OrderBook::Fill>& expectedFills,
                       const std::vector<OrderBook::ExpectedLevel>& expectedState) {
        OrderBook book;
        std::vector<OrderBook::Fill> actualFills = submitAndCollect(book, sequence);

        // Utilize compareFills for the new return format
        auto fillFailures = compareFills(actualFills, expectedFills);
        bool passedFills = fillFailures.empty();

        std::vector<int> actualStates;
        for (const auto& state : expectedState) {
            auto it = book.quantityAt(state.side, state.price);
            actualStates.push_back(it);
        }

        bool passedStates = true;
        if (expectedState.size() == actualStates.size()) {
            int i = 0;
            for (const auto& state : expectedState) {
                if (actualStates[i] != expectedState[i].quantity) {
                    passedStates = false;
                    break;
                }
                ++i;
            }
        } else {
            passedStates = false;
        }

        if (passedFills) {
            std::cout << "Passed Fills for " << name << "!\n";
        } else {
            std::cout << "FAILED Fills for " << name << ": \n  Expected " << expectedFills 
                      << "\n  Got: " << actualFills 
                      << "\n  Failures: " << fillFailures << "\n";
        }
        
        if (passedStates) {
            std::cout << "Passed States for " << name << "!\n";
        } else {
            std::cout << "FAILED States for " << name << ": \n  Expected " << expectedState 
                      << "\n  Got: " << actualStates << "\n";
        }

        if (passedFills && passedStates) {
            std::cout << name << " Passed Tests\n";
            return true;
        }
        
        std::cout << name << " Failed Tests\n";
        return false;
    }

    bool CancelTest(std::vector<Order>& sequence,
                    const std::vector<Id>& ids,
                    const std::vector<Id>& expectedCancels,
                    const std::vector<OrderBook::ExpectedLevel>& expectedState) {
        OrderBook book;
        for (auto& o : sequence) {
            book.rest(o);
        }

        std::vector<Id> cancelledIds;
        for (auto id : ids) {
            bool removed = book.cancel(id);
            if (!removed) {
                std::cout << "Cancel failed for: " << id << "\n";
            } else {
                cancelledIds.push_back(id);
            }
        }

        bool passedCancelAmount = (cancelledIds.size() == expectedCancels.size());
        bool passedCancelOrder = true;
        if (passedCancelAmount) {
            for (size_t i = 0; i < cancelledIds.size(); ++i) {
                if (cancelledIds[i] != expectedCancels[i]) {
                    passedCancelOrder = false;
                    break;
                }
            }
        } else {
            passedCancelOrder = false;
        }

        std::vector<int> actualStates;
        for (const auto& state : expectedState) {
            auto it = book.quantityAt(state.side, state.price);
            actualStates.push_back(it);
        }

        bool passedStates = true;
        if (expectedState.size() == actualStates.size()) {
            for (size_t i = 0; i < expectedState.size(); ++i) {
                if (actualStates[i] != expectedState[i].quantity) {
                    passedStates = false;
                    break;
                }
            }
        } else {
            passedStates = false;
        }

        std::cout << (passedCancelAmount ? "Passed Cancel Amount Test\n" : "Failed Cancel Amount Test\n");
        std::cout << (passedCancelOrder ? "Passed Cancel Order Test\n" : "Failed Cancel Order Test\n");
        if (!passedCancelAmount || !passedCancelOrder) {
            std::cout << "FAIL Expected Cancels: " << expectedCancels << " Got: " << cancelledIds << "\n";
        }
        if (!passedStates) {
            std::cout << "FAILED States: Expected " << expectedState << " Got: " << actualStates << "\n";
        } else {
            std::cout << "Passed, got these states: " << actualStates << "\n";
        }

        if (passedCancelAmount && passedCancelOrder && passedStates) {
            std::cout << "Passed Cancel Test\n";
            return true;
        }
        std::cout << "FAILED Cancel Test\n";
        return false;
    }
   
    bool modifyTest(
        const std::string& name, 
        std::vector<Order>& setup,
        const std::vector<Modifications>& orderChanges,
        std::vector<Order>& matcherOrders,
        const std::vector<OrderBook::Fill>& expectedFills,
        const std::vector<OrderBook::ExpectedLevel>& expectedState) { 

        OrderBook book;
        for (auto& order : setup) {
            book.rest(order);
        }
        for (auto& change : orderChanges) {
            book.modify(change.id, change.newPrice, change.newQuantity);
        }

        auto actualFills = submitAndCollect(book, matcherOrders);
        auto failures = compareFills(actualFills, expectedFills);
       
        
        for (const auto& state : expectedState) {
            int actualQty = book.quantityAt(state.side, state.price);
            if (actualQty != state.quantity) {
                failures.push_back({
                    "State Qty at Price " + std::to_string(state.price), 
                    std::to_string(actualQty), 
                    std::to_string(state.quantity)
                });
            }
        }

        if (failures.empty()) {
            std::cout << "Passed Modify Test for " << name << "!\n";
            return true;
        } 
        
        std::cout << "FAILED Modify Test for " << name << ":\n"
                  << "  Expected Fills: " << expectedFills << "\n"
                  << "  Got Fills: " << actualFills << "\n"
                  << "  Failures: " << failures << "\n";
        return false;
    }


    std::optional<std::vector<LoggedOp>> generateAndExecute(OrderBook& book, generator& gen, int iterations){
        std::uniform_int_distribution<int> sideDist(0, 1);
        std::uniform_int_distribution<int> typeDist(0, 9);
        std::uniform_int_distribution<int> priceDist(1, 100);
        std::uniform_int_distribution<int> quantityDist(1, 100);
        std::uniform_int_distribution<int> operationDist(0, 99);
        std::uniform_int_distribution<int> PriceChangeChance(0, 5);
        std::uniform_int_distribution<int> QuantityChangeChance(0, 5);
        std::vector<LoggedOp> history;
        
         
        for(int i = 0; i < iterations; ++i){
            if(gen.restingIds.empty()){
                Order o{sideDist(gen.rng) == 0 ? Side::Buy : Side::Sell,
                        typeDist(gen.rng) != 9 ? Type::Limit : Type::Market,
                        priceDist(gen.rng),
                        quantityDist(gen.rng),
                        gen.nextId++,
                        0};
                book.submit(o);
                gen.restingIds.push_back(o.id);
            } else{
                std::uniform_int_distribution<size_t> indexDist(0, gen.restingIds.size() - 1);
                auto operation = operationDist(gen.rng);
                if( operation >= 50 && operation <= 79){
                   Order o{sideDist(gen.rng) == 0 ? Side::Buy : Side::Sell,
                    typeDist(gen.rng) != 9 ? Type::Limit : Type::Market,
                    priceDist(gen.rng),
                    quantityDist(gen.rng),
                    gen.nextId++
                   };
                   auto result = book.submit(o);
                   if(result.has_value() && o.type == Type::Limit){
                    gen.restingIds.push_back(o.id);
                    history.push_back({OpType::Submit, o, o.id, std::nullopt, std::nullopt});
                   }

                   /*std::cout << "[Iter " << i << "] SUBMIT: " 
                         << (o.type == Type::Limit ? "Limit " : "Market ")
                         << (o.side == Side::Buy ? "Buy" : "Sell") 
                         << " ID " << o.id << " (" << o.quantity << " @ " << o.price << ")\n";*/
                    
                }
                if(operation >= 80 && operation <= 99){
                    auto Order = book.getOrderInfo(gen.restingIds[indexDist(gen.rng)]);
                    size_t idx = indexDist(gen.rng);
                    Id idToCancel = gen.restingIds[idx];
                    book.cancel(idToCancel);
                    gen.restingIds.erase(gen.restingIds.begin() + idx);

                    history.push_back({OpType::Cancel, *Order, idToCancel, std::nullopt, std::nullopt});


                    //std::cout << "[Iter " << i << "] CANCEL: ID " << idToCancel << "\n";
                }
                if(operation >= 0 && operation <= 49){
                    size_t idx = indexDist(gen.rng);
                    Id idToModify = gen.restingIds[idx];
                    auto orderInfo = book.getOrderInfo(idToModify);
                    if(orderInfo == std::nullopt) continue;
                    std::optional<Price> newPrice;
                    std::optional<Quantity> newQuantity;
                    std::uniform_int_distribution<int> PriceChangePercent(0, 20);
                    std::uniform_int_distribution<int> QuantityChangePercent(0, 20);
                    if(PriceChangeChance(gen.rng) > 1){
                       auto change = 1 + PriceChangePercent(gen.rng)/ 100.0;
                        newPrice = (orderInfo->price * change);
                        //newPrice = (priceDist(gen.rng));
                    }
                    if(QuantityChangeChance(gen.rng) > 1){
                        int change = 1 + QuantityChangePercent(gen.rng)/ 100.0;
                        newQuantity = (orderInfo->quantity * change);
                        //newQuantity = (quantityDist(gen.rng));
                    }
                    book.modify(idToModify, newPrice, newQuantity);
                    if(!book.contains(idToModify)){
                        gen.restingIds.erase(gen.restingIds.begin() + idx);
                    }
                    /*std::cout << "[Iter " << i << "] MODIFY: ID " << idToModify 
                        << " (NewPrice: " << (newPrice ? std::to_string(*newPrice) : "None")
                        << ", NewQty: " << (newQuantity ? std::to_string(*newQuantity) : "None") << ")\n";*/
                        history.push_back({OpType::Modify, *orderInfo, idToModify, newPrice, newQuantity});
                }
            }


            if(book.checkNoCrossedBook()){
                return history;
                std::cout << "CROSSED BOOK DETECTED at iteration " << i << "\n";
            } 
            if(book.checkNoOrphans() != std::nullopt){
               return history;
               std::cout << "ORPHANED ORDER DETECTED at iteration " << i << "\n"; 
            } 
            if(!book.checkFIFO()){
              return history;
              std::cout << "FIFO VIOLATION DETECTED at iteration " << i << "\n";
  
            } 

    }
    return std::nullopt;

}

bool invReplay(std::vector<LoggedOp>& sequence){
    OrderBook book;

    for(int i = 0; i < sequence.size(); ++i){
        if(sequence[i].type == OpType::Submit){
            auto result = book.submit(sequence[i].order);
            if(!result.has_value() && sequence[i].order.type == Type::Limit){
                //std::cout << "Replay failed at iteration " << i << ": Submit failed for order ID " << sequence[i].order.seq << "\n";
            }
        }else if(sequence[i].type == OpType::Cancel){
            bool result = book.cancel(sequence[i].id);
            if(!result){
                //std::cout << "Replay failed at iteration " << i << ": Cancel failed for order ID " << sequence[i].order.seq << "\n";
            }
        }else if(sequence[i].type == OpType::Modify){
            bool result = book.modify(sequence[i].id, sequence[i].newPrice, sequence[i].newQuantity);
            if(!result){
               // std::cout << "Replay failed at iteration " << i << ": Modify failed for order ID " << sequence[i].order.seq << "\n";
            }
        }
        if(book.checkNoCrossedBook()){
                return false;
                std::cout << "CROSSED BOOK DETECTED at iteration " << i << "\n";
            } 
            if(book.checkNoOrphans() != std::nullopt){
               return false;
               std::cout << "ORPHANED ORDER DETECTED at iteration " << i << "\n"; 
            } 
            if(!book.checkFIFO()){
              return false;
              std::cout << "FIFO VIOLATION DETECTED at iteration " << i << "\n";
  
            } 
        }
    return true;

    }
    std::vector<LoggedOp> shrinker(std::vector<LoggedOp>& seq){
        bool OpsRemoved = true;
        while(OpsRemoved){
            OpsRemoved = false;
            for(int i = seq.size() - 1 ; i >= 0; --i){
                LoggedOp removed = seq[i];
                seq.erase(seq.begin() + i);
                if(invReplay(seq)){
                    seq.insert(seq.begin() + i, removed);
                }else{
                    std::cout << "Removed operation at index " << i << " (ID: " << removed.order.id << ")\n";
                    OpsRemoved = true;
                }
            }
        }
        return seq;
    }
};

OrderBook::Request makeRequest(int64_t id){
    OrderBook::Request req{};
    req.id = id;
    return req;
}
void pushMany(OrderBook::RingBuffer& queue, int producerId, int count){
    producer prod(producerId);
    

    for(int i = 0; i < count; ++i){
        auto req = makeRequest(prod.nextId());
        bool pushed = queue.push(req);
        assert(pushed);
    }

}

void testRingBufferConcurrent(){
 OrderBook::RingBuffer buffer(100000);
 std::vector<std::thread> threads;
 for(int i = 0; i < 4; ++i){
    threads.emplace_back(pushMany, std::ref(buffer), i, 5000);
 }
 for(auto& t : threads){
    t.join();
 }
 std::vector<int64_t>lastSeen(4, -1);
 int total = 0;
 while(auto item = buffer.pop()){
    ++total;
    auto p = producerOf(item->id);
    assert(p >= 0 && p < 4);
    assert(item->id > lastSeen[p]);
    lastSeen[p] = item->id;
 }
 assert(total == 4 * 5000);

 std::cout << "[PASS] Test 5: Concurrent Producers\n";

}



void testRingBufferFillsAndRefuses() {
    OrderBook::RingBuffer rb(4);

    assert(rb.push(makeRequest(1)) == true);
    assert(rb.push(makeRequest(2)) == true);
    assert(rb.push(makeRequest(3)) == true);
    assert(rb.push(makeRequest(4)) == true);

    assert(rb.push(makeRequest(5)) == false);

    std::cout << "[PASS] Test 1: Fills and Refuses\n";
}

void testRingBufferFIFOOrder() {
    OrderBook::RingBuffer rb(4);

    rb.push(makeRequest(101));
    rb.push(makeRequest(102));
    rb.push(makeRequest(103));
    rb.push(makeRequest(104));

    auto r1 = rb.pop();
    assert(r1.has_value() && r1->id == 101);

    auto r2 = rb.pop();
    assert(r2.has_value() && r2->id == 102);

    auto r3 = rb.pop();
    assert(r3.has_value() && r3->id == 103);

    auto r4 = rb.pop();
    assert(r4.has_value() && r4->id == 104);

    std::cout << "[PASS] Test 2: FIFO Order Out\n";
}

void testRingBufferDrainsAndRefuses() {
    OrderBook::RingBuffer rb(4);

    rb.push(makeRequest(1));
    rb.push(makeRequest(2));
    rb.push(makeRequest(3));
    rb.push(makeRequest(4));

    for (int i = 0; i < 4; ++i) {
        rb.pop();
    }

    auto extraPop = rb.pop();
    assert(!extraPop.has_value());

    std::cout << "[PASS] Test 3: Drains and Refuses\n";
}

void testRingBufferWrapAround() {
    OrderBook::RingBuffer rb(4);

    for (int64_t i = 1; i <= 12; ++i) {
        bool pushed = rb.push(makeRequest(i));
        assert(pushed == true);

        auto popped = rb.pop();
        assert(popped.has_value());
        assert(popped->id == i);
    }

    assert(!rb.pop().has_value());

    std::cout << "[PASS] Test 4: Wrap-Around Test\n";
}

void runRingBufferTests() {
    testRingBufferFillsAndRefuses();
    testRingBufferFIFOOrder();
    testRingBufferDrainsAndRefuses();
    testRingBufferWrapAround();
    testRingBufferConcurrent();
}




int main(){
    Test t;

    std::vector<Order> buyAggressorOrders {
        {Side::Sell, Type::Limit, 102, 100, 1, 0},
        {Side::Sell, Type::Limit, 103, 50, 2, 0},
        {Side::Buy, Type::Limit, 103, 90, 3, 0}
    };
    std::vector<OrderBook::Fill> buyAggressorFills {{102, 90, 3, 1}};
    std::vector<OrderBook::ExpectedLevel> buyAggressorLevels {{Side::Sell, 102, 10}};
    t.runReplayTest("buy aggressor", buyAggressorOrders, buyAggressorFills, buyAggressorLevels);

    std::vector<Order> sellAggressorOrders {
        {Side::Buy, Type::Limit, 103, 100, 1, 0},
        {Side::Buy, Type::Limit, 102, 50, 2, 0},
        {Side::Sell, Type::Limit, 103, 90, 3, 0}
    };
    std::vector<OrderBook::Fill> sellAggressorFills {{103, 90, 3, 1}};
    std::vector<OrderBook::ExpectedLevel> sellAggressorLevels {{Side::Buy, 103, 10}};
    t.runReplayTest("sell aggressor", sellAggressorOrders, sellAggressorFills, sellAggressorLevels);

    std::vector<Order> restRemainderOrders {{Side::Sell, Type::Limit, 100, 100, 1, 0},
                                             {Side::Sell, Type::Limit, 105, 50, 2, 0},
                                             {Side::Buy, Type::Limit, 110, 200, 3, 0}};
    std::vector<OrderBook::Fill> restRemainderFills {{100, 100, 3, 1}, {105, 50, 3, 2}};
    std::vector<OrderBook::ExpectedLevel> restRemainderLevels {{Side::Buy, 110, 50}};
    t.runReplayTest("rest remainder", restRemainderOrders, restRemainderFills, restRemainderLevels);

    std::vector<Order> marketOrderOrders {{Side::Sell, Type::Limit, 100, 100, 1, 0},
                                          {Side::Sell, Type::Limit, 105, 50, 2, 0},
                                          {Side::Buy, Type::Market, 0, 120, 3, 0}};
    std::vector<OrderBook::Fill> marketOrderFills {{100, 100, 3, 1}, {105, 20, 3, 2}};
    std::vector<OrderBook::ExpectedLevel> marketOrderLevels {};
    t.runReplayTest("market order", marketOrderOrders, marketOrderFills, marketOrderLevels);

    std::vector<Order> emptyBookOrders {{Side::Buy, Type::Limit, 110, 50, 1, 0}};
    std::vector<OrderBook::Fill> emptyBookFills {};
    std::vector<OrderBook::ExpectedLevel> emptyBookLevels {{Side::Buy, 110, 50}};
    t.runReplayTest("empty book", emptyBookOrders, emptyBookFills, emptyBookLevels);

    std::vector<Order> cancelSingleOrderSequence {
        {Side::Buy, Type::Limit, 100, 50, 1, 0},
        {Side::Buy, Type::Limit, 101, 25, 2, 0}
    };
    std::vector<Id> cancelSingleOrderIds {1};
    std::vector<Id> cancelSingleOrderExpected {1};
    std::vector<OrderBook::ExpectedLevel> cancelSingleOrderLevels {{Side::Buy, 100, 0}, {Side::Buy, 101, 25}};
    t.CancelTest(cancelSingleOrderSequence, cancelSingleOrderIds, cancelSingleOrderExpected, cancelSingleOrderLevels);

    std::vector<Order> cancelTwoAtSamePriceSequence {
        {Side::Sell, Type::Limit, 100, 20, 1, 0},
        {Side::Sell, Type::Limit, 100, 30, 2, 0}
    };
    std::vector<Id> cancelSharedPriceIds {1};
    std::vector<Id> cancelSharedPriceExpected {1};
    std::vector<OrderBook::ExpectedLevel> cancelSharedPriceLevels {{Side::Sell, 100, 30}};
    t.CancelTest(cancelTwoAtSamePriceSequence, cancelSharedPriceIds, cancelSharedPriceExpected, cancelSharedPriceLevels);

    std::vector<Order> cancelUnknownIdSequence {
        {Side::Sell, Type::Limit, 100, 20, 1, 0}
    };
    std::vector<Id> cancelUnknownIdIds {999};
    std::vector<Id> cancelUnknownIdExpected {};
    std::vector<OrderBook::ExpectedLevel> cancelUnknownIdLevels {{Side::Sell, 100, 20}};
    t.CancelTest(cancelUnknownIdSequence, cancelUnknownIdIds, cancelUnknownIdExpected, cancelUnknownIdLevels);

    std::vector<Order> cancelLastAtPriceSequence {
        {Side::Buy, Type::Limit, 100, 50, 1, 0}
    };

    std::vector<Order> reduceKeepPosOrders{
        {Side::Sell, Type::Limit, 100, 100, 1, 0},
        {Side::Sell, Type::Limit, 100, 50, 2, 0}
    };
    std::vector<Test::Modifications> reduceKeepPosMods{
        Test::Modifications{1, std::nullopt, 60}
    };
    std::vector<Order> reduceKeepPosMatcher{
        {Side::Buy, Type::Limit, 100, 60, 3, 0}
    };
    std::vector<OrderBook::Fill> reduceKeepPosFills{
        {100, 60, 3, 1}
    };
    std::vector<OrderBook::ExpectedLevel> reduceKeepPosStates{
        {Side::Sell, 100, 50} // Proves A's remaining 40 was cancelled correctly, leaving only B (50)
    };
    t.modifyTest("Reduce Price Keep Position", reduceKeepPosOrders, reduceKeepPosMods, reduceKeepPosMatcher, reduceKeepPosFills, reduceKeepPosStates);


    std::vector<Order> increaseLosePosOrders{
        {Side::Sell, Type::Limit, 100, 60, 1, 0},
        {Side::Sell, Type::Limit, 100, 50, 2, 0}
    };
    std::vector<Test::Modifications> increaseLosePosMods{
        Test::Modifications{1, std::nullopt, 100}
    };
    std::vector<Order> increaseLosePosMatcher{
        {Side::Buy, Type::Limit, 100, 100, 3, 0}
    };
    std::vector<OrderBook::Fill> increaseLosePosFills{
        {100, 50, 3, 2},
        {100, 50, 3, 1}
    };
    std::vector<OrderBook::ExpectedLevel> increaseLosePosStates{
        {Side::Sell, 100, 50} // Proves 50 of A remains
    };
    t.modifyTest("Increase Price Lose Position", increaseLosePosOrders, increaseLosePosMods, increaseLosePosMatcher, increaseLosePosFills, increaseLosePosStates);


    std::vector<Order> priceChangeLosePosOrders{
        {Side::Sell, Type::Limit, 100, 100, 1, 0},
        {Side::Sell, Type::Limit, 101, 100, 2, 0}
    };
    std::vector<Test::Modifications> priceChangeLosePosMods{
        Test::Modifications{1, 101, std::nullopt}
    };
    std::vector<Order> priceChangeLosePosMatcher{
        {Side::Buy, Type::Limit, 101, 150, 3, 0}
    };
    std::vector<OrderBook::Fill> priceChangeLosePosFills{
        {101, 100, 3, 2},
        {101, 50, 3, 1}
    };
    std::vector<OrderBook::ExpectedLevel> priceChangeLosePosStates{
        {Side::Sell, 100, 0}, // Proves no ghost left behind!
        {Side::Sell, 101, 50}
    };
    t.modifyTest("Price Change Loses Position", priceChangeLosePosOrders, priceChangeLosePosMods, priceChangeLosePosMatcher, priceChangeLosePosFills, priceChangeLosePosStates);


    std::vector<Order> modifyToZeroCancelOrders{
        {Side::Sell, Type::Limit, 100, 100, 1, 0}
    };
    std::vector<Test::Modifications> modifyToZeroCancelMods{
        Test::Modifications{1, std::nullopt, 0}
    };
    std::vector<Order> modifyToZeroCancelMatcher{
        {Side::Buy, Type::Limit, 100, 50, 2, 0}
    };
    std::vector<OrderBook::Fill> modifyToZeroCancelFills{};
    std::vector<OrderBook::ExpectedLevel> modifyToZeroCancelStates{
        {Side::Sell, 100, 0} // Verifies book is empty
    };
    t.modifyTest("Modify Qty to Zero Cancels", modifyToZeroCancelOrders, modifyToZeroCancelMods, modifyToZeroCancelMatcher, modifyToZeroCancelFills, modifyToZeroCancelStates);



    std::vector<Order> modifyUnknownIdOrders{
        {Side::Sell, Type::Limit, 100, 100, 1, 0}
    };
    std::vector<Test::Modifications> modifyUnknownIdMods{
        Test::Modifications{999, 105, 50}
    };
    std::vector<Order> modifyUnknownIdMatcher{
        {Side::Buy, Type::Limit, 100, 100, 2, 0}
    };
    std::vector<OrderBook::Fill> modifyUnknownIdFills{
        {100, 100, 2, 1}
    };
    std::vector<OrderBook::ExpectedLevel> modifyUnknownIdStates{
        {Side::Sell, 100, 0}
    };
    t.modifyTest("Modify Unknown ID", modifyUnknownIdOrders, modifyUnknownIdMods, modifyUnknownIdMatcher, modifyUnknownIdFills, modifyUnknownIdStates);



    std::vector<Order> MultiLevelOrders{
        {Side::Sell, Type::Limit, 100, 25, 1, 0},
        {Side::Sell, Type::Limit, 101, 25, 2, 0},
        {Side::Sell, Type::Limit, 102, 50, 3, 0},
        {Side::Buy, Type::Limit, 102, 100, 4, 0}
    };

    std::vector<OrderBook::Fill> MultiLevelFills{
        {100, 25, 4, 1},
        {101, 25, 4, 2},
        {102, 50, 4, 3}
    };

    std::vector<OrderBook::ExpectedLevel> MultiLevelLevels{
        {Side::Sell, 100, 0},
        {Side::Sell, 101, 0},
        {Side::Sell, 102, 0},
        {Side::Buy, 102, 0}

    };
    t.runReplayTest("MultiLevel",MultiLevelOrders, MultiLevelFills, MultiLevelLevels);

    std::vector<Order> ExactMatchOrders{
        {Side::Sell, Type::Limit, 100, 50, 1, 0}, 
        {Side::Buy,  Type::Limit, 100, 50, 2, 0}, 
        {Side::Buy,  Type::Limit, 100, 10, 3, 0}  
    };

    std::vector<OrderBook::Fill> ExactMatchFills{
        {100, 50, 2, 1}
    };

    std::vector<OrderBook::ExpectedLevel> ExactMatchLevels{
        {Side::Sell, 100, 0}, 
        {Side::Buy,  100, 10} 
    };

    t.runReplayTest("Exact Match Boundary", ExactMatchOrders, ExactMatchFills, ExactMatchLevels);

    std::vector<Order> crossCheck{
        {Side::Buy, Type::Limit, 100, 50, 1, 0},
        {Side::Sell, Type::Limit, 95, 20, 2, 0},
    };

    std::vector<OrderBook::Fill> crossCheckFills{
        {100, 20, 2, 1}
    };

    std::vector<OrderBook::ExpectedLevel> crossCheckLevels{
        {Side::Buy, 100, 30},
        {Side::Sell, 95, 0}
    };
    t.runReplayTest("Cross Comparision Check", crossCheck, crossCheckFills, crossCheckLevels);

    OrderBook dupIdBook;
    std::vector<Order> dupIdOrders{
        {Side::Buy, Type::Limit, 100, 50, 1, 0},
        {Side::Buy, Type::Limit, 100, 30, 1, 0}
    };
    std::vector<bool> dupIdExpected{true, false};
    
    bool passedDup = t.ValidationTest(dupIdBook, dupIdOrders, dupIdExpected);
    if (passedDup && dupIdBook.quantityAt(Side::Buy, 100) == 50) {
        std::cout << "Passed Duplicate ID Test!\n";
    } else {
        std::cout << "FAILED Duplicate ID Test\n";
    }

    OrderBook badQtyBook;
    std::vector<Order> badQtyOrders{
        {Side::Sell, Type::Limit, 100, 0, 1, 0},
        {Side::Sell, Type::Limit, 100, -10, 2, 0}
    };
    std::vector<bool> badQtyExpected{false, false};
    
    bool passedBadQty = t.ValidationTest(badQtyBook, badQtyOrders, badQtyExpected);
    if (passedBadQty && badQtyBook.quantityAt(Side::Sell, 100) == 0) {
        std::cout << "Passed Bad Quantity Test!\n";
    } else {
        std::cout << "FAILED Bad Quantity Test\n";
    }

    OrderBook badPriceBook;
    std::vector<Order> badPriceOrders{
        {Side::Buy, Type::Limit, 0, 50, 1, 0},
        {Side::Buy, Type::Limit, -5, 50, 2, 0},
        {Side::Buy, Type::Market, 0, 50, 3, 0}
    };
    std::vector<bool> badPriceExpected{false, false, true}; 
    
    bool passedBadPrice = t.ValidationTest(badPriceBook, badPriceOrders, badPriceExpected);
    if (passedBadPrice) {
        std::cout << "Passed Bad Price Test!\n";
    } else {
        std::cout << "FAILED Bad Price Test\n";
    }

    std::vector<Id> cancelLastAtPriceIds {1};
    std::vector<Id> cancelLastAtPriceExpected {1};
    std::vector<OrderBook::ExpectedLevel> cancelLastAtPriceLevels {};
    t.CancelTest(cancelLastAtPriceSequence, cancelLastAtPriceIds, cancelLastAtPriceExpected, cancelLastAtPriceLevels);

    generator gen;
    OrderBook book;
    auto fuzz = t.generateAndExecute(book, gen, 400000);
    if(fuzz.has_value()){
        std::cout << "Fuzzing detected an issue, attempting to shrink the sequence...\n";
        auto shrunk = t.shrinker(*fuzz);
        std::cout << "Shrunk sequence to " << shrunk.size() << " operations.\n";
    } else {
        std::cout << "Fuzzing completed without detecting issues.\n";
    }

    producer a(1);
    producer b(2);
    producer c(3);
    for(int i = 0; i < 10; ++i){
        auto aId = a.nextId();
        auto bId = b.nextId();
        auto cId = c.nextId();
        std::println("a Id: {}",aId);
        std::println("producer id: {}",producerOf(aId));
        std::println("b Id: {}",bId);
        std::println("producer id: {}",producerOf(bId));
        std::println("c Id: {}",cId);
        std::println("producer id: {}",producerOf(cId));
        
    }

    runRingBufferTests();

    return 0;
}