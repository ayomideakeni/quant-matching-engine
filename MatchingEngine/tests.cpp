#include "orderBook.hpp"
#include <variant>
#include <string>
#include <optional>
#include <iostream>
#include <random>
#include <chrono>
#include <cassert>
#include <unordered_set>

//enum class OpType { Submit, Cancel, Modify};
struct Stats {
    double mean = 0.0, p50 = 0.0, p99 = 0.0, p999 = 0.0, max = 0.0;
    size_t batches = 0;
    size_t totalOps = 0;
    double batchMean = 0.0;
    size_t batchMax = 0;
    size_t batchMin = SIZE_MAX;
    double totalNs = 0.0;
};

Stats computeStats(const std::vector<BenchSample>&  original) {
    Stats st;
    auto samples = original;
    if (samples.empty()) return st;


    
    for (const auto& s : samples){
        st.totalNs += s.ns;
        st.totalOps += s.ops;
        st.batchMin = std::min(st.batchMin, s.ops);
        st.batchMax = std::max(st.batchMax, s.ops);
        
    }
    st.batches = samples.size();
    st.mean = st.totalNs /  static_cast<double>(st.totalOps);

    st.batchMean = static_cast<double>(st.totalOps) / static_cast<double>(st.batches);

    std::sort(samples.begin(), samples.end(), [](const BenchSample& a, const BenchSample& b){return a.opMean() < b.opMean();});
    
    size_t opsSeen = 0;
    size_t p50 = (st.totalOps * 50 / 100);
    size_t p99 = (st.totalOps * 99 / 100);
    size_t p999 = (st.totalOps * 999 / 1000);
    bool rec50 = false;
    bool rec99 = false;
    bool rec999 = false;

    for(const auto& s : samples){
        opsSeen += s.ops;
        if(!rec50 && opsSeen >= p50){
            st.p50 = s.opMean();
            rec50 = true;
        }
        if(!rec99 && opsSeen >= p99){
            st.p99 = s.opMean();
            rec99 = true;
        }
        if(!rec999 && opsSeen >= p999){
            st.p999 = s.opMean();
            rec999 = true;
        }
    }
    st.max = samples.back().opMean();

    return st;
}

Stats computeStats(const std::vector<double>& original) {
    Stats st;
    if (original.empty()) return st;
 
    auto samples = original;              // don't mutate the caller's vector
    std::sort(samples.begin(), samples.end());
 
    st.batches = samples.size();           // batches == samples here; no
    st.totalOps = samples.size();          // per-batch op count exists in
                                            // this harness (every batch is
                                            // a fixed size decided by the
                                            // caller), so both just reflect
                                            // the sample count.
 
    st.p50  = samples[samples.size() * 50  / 100];
    st.p99  = samples[samples.size() * 99  / 100];
    st.p999 = samples[samples.size() * 999 / 1000];
    st.max  = samples.back();
 
    double sum = 0.0;
    for (double s : samples) sum += s;
    st.mean = sum / static_cast<double>(samples.size());
 
    st.totalNs   = sum;
    st.batchMean = 1.0;   // fixed batch size in this harness; not meaningful
    st.batchMin  = 1;
    st.batchMax  = 1;
 
    return st;
}

// Kept for single-benchmark runs; the sweep uses the table printer instead.
void reportPercentiles(const std::string& label,
                       const std::vector<BenchSample>& samples,
                       size_t drainCap) {
    Stats st = computeStats(samples);
    if (st.batches == 0) return;

    std::print("-----------------------------------------------------\n");
    std::print("{} (drain cap {})\n", label, drainCap);
    std::print("  Batches   : {}\n", st.batches);
    std::print("  Total ops : {}\n", st.totalOps);
    std::print("  Batch size: mean {:.1f}, min {}, max {}\n",
               st.batchMean, st.batchMin, st.batchMax);
    std::print("  Mean      : {:.3f} ns/op\n", st.mean);
    std::print("  p50       : {:.3f} ns/op\n", st.p50);
    std::print("  p99       : {:.3f} ns/op\n", st.p99);
    std::print("  p99.9     : {:.3f} ns/op\n", st.p999);
    std::print("  Max       : {:.3f} ns/op\n", st.max);
    std::print("-----------------------------------------------------\n");
}

struct LoggedOp{

    OpType type;
    Order order;
    Id id;
    std::optional<Price> newPrice;
    std::optional<Quantity> newQuantity;
};

LoggedOp convToOp(Request request){
        return LoggedOp(request.requestType, request.order, request.id, request.newPrice, request.newQuantity);
    }
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

        int modifyWeight = 50;
        int submitWeight = 80;
        int cancelWeight = 80;



        
        int minSeeded = 1;
        int iterationsDone = 0;
        int checkPoint = 0;

        generator() = default;

        generator(int modifyPct, int submitPct, int cancelPct)
            : modifyWeight(modifyPct),
              submitWeight(submitPct),
              cancelWeight(modifyPct + submitPct)
        {
            assert(modifyPct >= 0 && submitPct >= 0 && cancelPct >= 0);
            assert(modifyPct + submitPct + cancelPct == 100);
        }


        Order generateSubmit(){

        std::uniform_int_distribution<int> sideDist(0, 1);
        std::uniform_int_distribution<int> typeDist(0, 9);
        std::uniform_int_distribution<int> priceDist(1, 100);
        std::uniform_int_distribution<int> quantityDist(1, 100);

            Order o{sideDist(rng) == 0 ? Side::Buy : Side::Sell,
                        typeDist(rng) != 9 ? Type::Limit : Type::Market,
                        priceDist(rng),
                        quantityDist(rng),
                        nextId++,
                        0};
                restingIds.push_back(o.id);
                return o;
        }
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

    bool checkStates(OrderBook& book, std::vector<OrderBook::ExpectedLevel> expectedState){
        std::vector<OrderBook::ExpectedLevel> actualStates;
        for (const auto& state : expectedState) {
            auto it = book.quantityAt(state.side, state.price);
            actualStates.push_back({state.side,state.price,it});
        }

        if (expectedState.size() == actualStates.size()) {
            int i = 0;
            for (const auto& state : expectedState) {
                if (actualStates[i].quantity != expectedState[i].quantity) {
                    return false;
                }
                ++i;
            }
        } else {
            return false;
        }

        return true;
    }



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

    std::optional<std::vector<LoggedOp>> generateAndExecute(OrderBook& book, generator& gen, int iterations, bool* checkpoints = nullptr ,bool* checkInv = nullptr, bool* logs = nullptr){
        std::uniform_int_distribution<int> sideDist(0, 1);
        std::uniform_int_distribution<int> typeDist(0, 9);
        std::uniform_int_distribution<int> priceDist(1, 100);
        std::uniform_int_distribution<int> quantityDist(1, 100);
        std::uniform_int_distribution<int> operationDist(0, 99);
        std::uniform_int_distribution<int> PriceChangeChance(0, 5);
        std::uniform_int_distribution<int> QuantityChangeChance(0, 5);
        std::vector<LoggedOp> history;
        int nextCheckPoint = gen.checkPoint + gen.iterationsDone;
        
         
        for(int i = gen.iterationsDone; i < iterations; ++i){
            ++gen.iterationsDone;
            if(gen.restingIds.size() < gen.minSeeded){
                Order o{sideDist(gen.rng) == 0 ? Side::Buy : Side::Sell,
                        typeDist(gen.rng) != 9 ? Type::Limit : Type::Market,
                        priceDist(gen.rng),
                        quantityDist(gen.rng),
                        gen.nextId++,
                        0};
                book.submit(o);
                gen.restingIds.push_back(o.id);
            } else{
                
                if(checkpoints && gen.iterationsDone >= nextCheckPoint) return std::nullopt;
                std::uniform_int_distribution<size_t> indexDist(0, gen.restingIds.size() - 1);
                auto operation = operationDist(gen.rng);
                if(operation >= 0 && operation < gen.modifyWeight){
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
                        if(logs)history.push_back({OpType::Modify, *orderInfo, idToModify, newPrice, newQuantity});
                }
                if( operation >= gen.modifyWeight && operation < gen.cancelWeight){
                   Order o{sideDist(gen.rng) == 0 ? Side::Buy : Side::Sell,
                    typeDist(gen.rng) != 9 ? Type::Limit : Type::Market,
                    priceDist(gen.rng),
                    quantityDist(gen.rng),
                    gen.nextId++
                   };
                   auto result = book.submit(o);
                   if(result.has_value() && o.type == Type::Limit){
                    gen.restingIds.push_back(o.id);
                    if(logs)history.push_back({OpType::Submit, o, o.id, std::nullopt, std::nullopt});
                   }

                   /*std::cout << "[Iter " << i << "] SUBMIT: " 
                         << (o.type == Type::Limit ? "Limit " : "Market ")
                         << (o.side == Side::Buy ? "Buy" : "Sell") 
                         << " ID " << o.id << " (" << o.quantity << " @ " << o.price << ")\n";*/
                    
                }
                if(operation >= gen.cancelWeight && operation <= 99){
                    size_t idx = indexDist(gen.rng);
                    auto Order = book.getOrderInfo(gen.restingIds[idx]);
                    Id idToCancel = gen.restingIds[idx];
                    book.cancel(idToCancel);
                    gen.restingIds.erase(gen.restingIds.begin() + idx);

                    if(logs)history.push_back({OpType::Cancel, *Order, idToCancel, std::nullopt, std::nullopt});


                    //std::cout << "[Iter " << i << "] CANCEL: ID " << idToCancel << "\n";
                }
                
            }


            if(checkInv){
                if(book.checkNoCrossedBook()){
                    std::cout << "CROSSED BOOK DETECTED at iteration " << i << "\n";
                    return history;
                } 
                if(book.checkNoOrphans() != std::nullopt){
                    std::cout << "ORPHANED ORDER DETECTED at iteration " << i << "\n";
                return history; 
                } 
                if(!book.checkFIFO()){
                std::cout << "FIFO VIOLATION DETECTED at iteration " << i << "\n";
                return history;
    
                } 
            }

    }
    return std::nullopt;

}

//DISREGARD FIRST RESULT
void fragTest(OrderBook& book, generator& gen, int iterations, int seed, int batchSize){
    int checkPoint = iterations / 10;
    std::vector<BenchSample> samples;
    gen.checkPoint = checkPoint;
    gen.minSeeded = seed;
    bool checkp(true);
    std::print("-----------------------------------------------------\n");

    while(true){
        if(gen.iterationsDone >= iterations) break;
        generateAndExecute(book, gen, iterations, &checkp);
        
            auto start = std::chrono::steady_clock::now();
            for(int i = 0; i < batchSize; ++i){
                auto o = gen.generateSubmit();
                book.submit(o);
            }
            auto end = std::chrono::steady_clock::now();
            double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back({ns, static_cast<size_t>(batchSize), 0.0, false});
            std::println("{}", (ns/batchSize));
        
    }
    
}
bool deterTest(const OrderBook& book1,const OrderBook& book2,std::pair<int,int> priceRange){
    for(int i = priceRange.first; i <= priceRange.second; ++i){
        auto buySide1 = book1.idsAt(Side::Buy, i);
        auto sellSide1 = book1.idsAt(Side::Sell, i);
        auto buySide2 = book2.idsAt(Side::Buy, i);
        auto sellSide2 = book2.idsAt(Side::Sell, i);
        if(buySide1 != buySide2){
            std::println("[FAIL] Side: Buy Price: {} Present Ids: {} Coressponding Ids: {}",i,buySide1, buySide2);
            return false;
        } 
        if(sellSide1 != sellSide2){
            std::println("[FAIL] Side: Sell Price: {} Present Ids: {} Coressponding Ids: {}", i,sellSide1, sellSide2);
            return false;
        } 
    }
    std::println("[PASS] No Id Mismatch Found");
    return true;
}

struct idWindow{
    // must be a power of 2
    static constexpr size_t capacity = 512;
    std::vector<Id> window = std::vector<Id>(capacity);
    size_t cursor = 0;
    size_t fillCount = 0;


    void record(Id id){
        window[cursor] = id;
        cursor = (cursor + 1) & (capacity - 1);
        if(fillCount < capacity){
            ++fillCount;
        }
    }
    std::optional<Id> pick(std::mt19937& rng){
        if(fillCount == 0) return std::nullopt;
        std::uniform_int_distribution<size_t>idPicker(0, fillCount - 1);
        Id chosenId = window[idPicker(rng)];

        return chosenId;
    }
};

std::vector<Request> generateRequest(generator& gen, producer& prod, int iterations){
     std::uniform_int_distribution<int> sideDist(0, 1);
        std::uniform_int_distribution<int> typeDist(0, 9);
        std::uniform_int_distribution<int> priceDist(1, 100);
        std::uniform_int_distribution<int> quantityDist(1, 100);
        std::uniform_int_distribution<int> operationDist(0, 99);
        std::uniform_int_distribution<int> PriceChangeChance(0, 5);
        std::uniform_int_distribution<int> QuantityChangeChance(0, 5);
        std::vector<Request> requests;
        idWindow reqWindow;
        
        while(requests.size() < static_cast<size_t>(iterations)){
            if(reqWindow.window.empty()){
                Order o{
                    sideDist(gen.rng) == 0 ? Side::Buy : Side::Sell,
                        typeDist(gen.rng) != 9 ? Type::Limit : Type::Market,
                        priceDist(gen.rng),
                        quantityDist(gen.rng),
                        prod.nextId(),
                        0
                };
                Request r{
                    OpType::Submit,
                    o,
                    o.id,
                    std::nullopt,
                    std::nullopt    
                };
                reqWindow.record(r.id);
                requests.push_back(r);
            } else{
                auto operation = operationDist(gen.rng);
                if( operation >= 50 && operation <= 79){
                    Order o{
                    sideDist(gen.rng) == 0 ? Side::Buy : Side::Sell,
                        typeDist(gen.rng) != 9 ? Type::Limit : Type::Market,
                        priceDist(gen.rng),
                        quantityDist(gen.rng),
                        prod.nextId(),
                        0
                    };
                    Request r{
                        OpType::Submit,
                        o,
                        o.id,
                        std::nullopt,
                        std::nullopt
                    };
                reqWindow.record(r.id);
                requests.push_back(r);
                }
                if(operation >= 80 && operation <= 99){
                    auto target = reqWindow.pick(gen.rng);
                    if(!target) continue;
                    Request r{
                        OpType::Cancel,
                        Order{},
                        *target,
                        std::nullopt,
                        std::nullopt
                    };
        
                    requests.push_back(r);
                }
                if(operation >= 0 && operation <= 49){
                auto target = reqWindow.pick(gen.rng);
                if(!target) continue;
                std::optional<Price> newPrice;
                std::optional<Quantity> newQuantity;
                if(PriceChangeChance(gen.rng) > 1){
                    newPrice = priceDist(gen.rng);
                }
                if(QuantityChangeChance(gen.rng) > 1){
                    newQuantity = quantityDist(gen.rng);
                }
                Request r{
                    OpType::Modify,
                    Order{},
                    *target,
                    newPrice,
                    newQuantity
                };
                requests.push_back(r);
            }
            }
        }
        return requests;
    }

bool invReplay(std::vector<LoggedOp>& sequence){
    OrderBook book;

    for(size_t i = 0; i < sequence.size(); ++i){
        if(sequence[i].type == OpType::Submit){
            Order o = sequence[i].order;
            Quantity volBefore = book.totalRestingVolume();
            Quantity incomingQty = sequence[i].order.quantity;
            Type orderType = sequence[i].order.type;

            auto result = book.submit(o);

            bool rejected = !result.has_value();
            Quantity tradedQty = 0;
            if(result.has_value()){
                for(const auto& fill : *result){
                    tradedQty += fill.quantity;
                }
            }

            Quantity volAfter = book.totalRestingVolume();

            if(!volumeConserved(volBefore, volAfter, incomingQty, tradedQty, orderType, rejected)){
                std::cout << "VOLUME CONSERVATION VIOLATION at iteration " << i << "\n";
                return false;
            }

        }else if(sequence[i].type == OpType::Cancel){
            bool result = book.cancel(sequence[i].id);
        }else if(sequence[i].type == OpType::Modify){
            bool result = book.modify(sequence[i].id, sequence[i].newPrice, sequence[i].newQuantity);
        }

        if(book.checkNoCrossedBook()){
            std::cout << "CROSSED BOOK DETECTED at iteration " << i << "\n";
            return false;
        } 
        if(book.checkNoOrphans() != std::nullopt){
            std::cout << "ORPHANED ORDER DETECTED at iteration " << i << "\n"; 
            return false;
        } 
        if(!book.checkFIFO()){
            std::cout << "FIFO VIOLATION DETECTED at iteration " << i << "\n";
            return false;
        } 
    }

    return true;
}

 OrderBook& invReplayBook(OrderBook& book,std::vector<LoggedOp>& sequence){
    
    int success = 0;
    int reject = 0;

    std::println("replay cap size: {}",sequence.size());

    for(size_t i = 0; i < sequence.size(); ++i){
        if(sequence[i].type == OpType::Submit){
            Order o = sequence[i].order;
            Quantity volBefore = book.totalRestingVolume();
            Quantity incomingQty = sequence[i].order.quantity;
            Type orderType = sequence[i].order.type;

            auto result = book.submit(o);

            bool rejected = !result.has_value();
            Quantity tradedQty = 0;
            if(result.has_value()){
                ++success;
                for(const auto& fill : *result){
                    tradedQty += fill.quantity;
                }
            }else{
                ++reject;
            }

            Quantity volAfter = book.totalRestingVolume();

            if(!volumeConserved(volBefore, volAfter, incomingQty, tradedQty, orderType, rejected)){
                std::cout << "VOLUME CONSERVATION VIOLATION at iteration " << i << "\n";
            }

        }else if(sequence[i].type == OpType::Cancel){
            bool result = book.cancel(sequence[i].id);
        }else if(sequence[i].type == OpType::Modify){
            bool result = book.modify(sequence[i].id, sequence[i].newPrice, sequence[i].newQuantity);
        }

        if(book.checkNoCrossedBook()){
            std::cout << "CROSSED BOOK DETECTED at iteration " << i << "\n";
        } 
        if(book.checkNoOrphans() != std::nullopt){
            std::cout << "ORPHANED ORDER DETECTED at iteration " << i << "\n"; 
        } 
        if(!book.checkFIFO()){
            std::cout << "FIFO VIOLATION DETECTED at iteration " << i << "\n";
        } 
    }

    std::println("success count: {}", success);
    std::println("reject count: {}", reject);
    return book;
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
                    std::cout << "Removed operation at index " << i << " (ID: " << removed.id << ")\n";
                    OpsRemoved = true;
                }
            }
        }
        return seq;
    }
void RingBufferIntegration(const std::string& testName,std::vector<Request> requests, std::vector<OrderBook::ExpectedLevel> eStates){
    OrderBook rbBook;
    RingBuffer rbT(64);
    std::thread writer(writerLoop, std::ref(rbT), std::ref(rbBook), nullptr, nullptr);
    for(const auto& req : requests){
        auto push = rbT.push(req);
        assert(push);
    }
    rbT.shutdown();
    writer.join();
    
    if(checkStates(rbBook, eStates)){
        std::println("[PASS] Ring Buffer Passed States For: {}", testName);
    }else{
        std::println("[FAIL] Ring Buffer Failed State Tests For: {} ", testName);
    }
}
static void worker(RingBuffer& queue, int producerId, int count) {
    producer prod{producerId, 0};
    Price basePrice = 100 + producerId * 10000;

    for (int i = 0; i < count; ++i) {
        Id id = prod.nextId();
        Price price = basePrice + i;

        bool pushed = queue.push({
            OpType::Submit,
            {Side::Buy, Type::Limit, price, 1, id, 0},
            id,
            std::nullopt,
            std::nullopt
        });
        assert(pushed);
    }
}

void testRingBufferConcurrentMatching() {
    constexpr int numProducers = 4;
    constexpr int countPerProducer = 2500;
    constexpr int totalExpectedOrders = numProducers * countPerProducer;

    OrderBook book;
    RingBuffer queue(totalExpectedOrders + 1000);

    std::thread writer(writerLoop, std::ref(queue), std::ref(book), nullptr, nullptr);

    std::vector<std::thread> producers;
    producers.reserve(numProducers);
    for (int i = 0; i < numProducers; ++i) {
        producers.emplace_back(worker, std::ref(queue), i, countPerProducer);
    }

    for (auto& t : producers) {
        t.join();
    }

    queue.shutdown();

    writer.join();

    int64_t totalRestingQuantity = 0;

    for (int p = 0; p < numProducers; ++p) {
        Price basePrice = 100 + static_cast<Price>(p) * 10000;
        for (int i = 0; i < countPerProducer; ++i) {
            Price price = basePrice + i;
            totalRestingQuantity += book.quantityAt(Side::Buy, price);
        }
    }

    std::println("[PASS] Total Resting Quantity: {}", totalRestingQuantity);
    assert(totalRestingQuantity == totalExpectedOrders);
}

static void pushAll(RingBuffer& queue, const std::vector<Request>& stream, size_t& retryCount){
    int pushCount = 0;
    size_t spins = 0;
    for(int i = 0; i < stream.size(); ++i){
        while(true){
            bool pushed = queue.push(stream[i]);
            if(pushed) break;
            
            retryCount++;
            spins++;
            if(spins == 64){
                std::this_thread::yield();
                spins = 0;
            }
        }
        
        ++pushCount;
    }
    //std::println("Push Count: {}", pushCount);
}

void testConcurrentGen(int producerCount, int opsPerProd, WriterContext& ctx){
    generator cGen;
    std::vector<std::vector<Request>> streams;
    size_t capSize = 1;
    size_t totalOps = (producerCount * opsPerProd);

    for(int i = 0; i < producerCount; ++i){
        producer prod(i);
        auto prodReqs = generateRequest(cGen, prod, opsPerProd);
        streams.push_back(prodReqs);
    }
    for(auto s : streams){
        while(capSize < totalOps){
            capSize *= 2;
        }
    }
    std::println("Capacity: {}", capSize);
    OrderBook conBook;
    auto totalResting = conBook.totalRestingVolume();
    std::println("Resting already: {}", totalResting);
    RingBuffer queue(capSize);
    ctx.captured.reserve(capSize);
    std::thread writer(writerLoop, std::ref(queue), std::ref(conBook), &ctx, nullptr);

    std::vector<size_t> retryCounts(producerCount, 0);

    std::vector<std::thread> threads;
    for(int i = 0; i < producerCount; ++i){
        threads.emplace_back(pushAll, std::ref(queue), std::ref(streams[i]), std::ref(retryCounts[i]));
    }

    size_t totalRetries = 0;
    for(auto r : retryCounts) totalRetries += r;

    for(auto& t : threads) t.join();
    queue.shutdown();
    writer.join();

    std::vector<LoggedOp> replayCap;
    for(auto op : ctx.captured){
        auto opTolog = convToOp(op);
        replayCap.push_back(opTolog);
    }
    OrderBook rBook;
    auto& replayedBook = invReplayBook(rBook,replayCap);
    auto test = deterTest(conBook, replayedBook, {1,100});
    if(!test){
        std::println("[FAIL] Failed Determinisim Test");
    }else std::println("[PASS] Passed Determinisim Test");

    std::println("ctx capture size: {}", ctx.captured.size());
    if(ctx.invariant.has_value()){
        std::string invariant;
        if(ctx.invariant == vio::crossedBook) invariant = "crossed";
        if(ctx.invariant == vio::fifo) invariant = "fifo";
        if(ctx.invariant == vio::orphan) invariant = "orphan";
        if(ctx.invariant == vio::volumeCon) invariant = "volumeCon";
        auto invIndex = *ctx.invarIndex;
        std::println("VIOLATION at: {}", invIndex);
    }
    return;
}


void concurrentBench(int producerCount, int opsPerProd, int draincap, size_t warmupOps = 0,size_t buffSize = 4096){
    generator cGen;
    std::vector<std::vector<Request>> streams;
    size_t totalOps = (producerCount * opsPerProd);
    BenchContext btx;
    btx.drainCap = draincap;
    btx.samples.reserve(totalOps);
    
    

    if(warmupOps > 0){
        generator warmGen;
        OrderBook warmBook;
        RingBuffer warmQueue(buffSize);
        producer prod(1);

        auto warmUpReqs = generateRequest(warmGen, prod, warmupOps);
        size_t warmRetries = 0;

        std::thread p(pushAll, std::ref(warmQueue), std::ref(warmUpReqs), std::ref(warmRetries));
        std::thread writer(writerLoop, std::ref(warmQueue), std::ref(warmBook), nullptr, nullptr);

        p.join();
        warmQueue.shutdown();
        writer.join();
    }

    for(int i = 0; i < producerCount; ++i){
        producer prod(i);
        auto prodReqs = generateRequest(cGen, prod, opsPerProd);
       streams.push_back(std::move(prodReqs));
    }
    
    std::vector<size_t> retryCounts(producerCount, 0);
    
    OrderBook conBook;
    RingBuffer queue(buffSize);
    auto start = std::chrono::steady_clock::now();

    std::thread writer(writerLoop, std::ref(queue), std::ref(conBook), nullptr, &btx);


    std::vector<std::thread> threads;
    for(int i = 0; i < producerCount; ++i){
        threads.emplace_back(pushAll, std::ref(queue), std::cref(streams[i]), std::ref(retryCounts[i]));
    }
    for(auto& t : threads) t.join();
    queue.shutdown();
    writer.join();

    auto end = std::chrono::steady_clock::now();

    double wallNs =
    std::chrono::duration<double, std::nano>(end - start).count();

    double wallMs = wallNs / 1'000'000.0;

    double seconds = wallNs / 1'000'000'000.0;

    double throughput =
        static_cast<double>(totalOps) / seconds;

    std::println("Wall time   : {:.3f} ms", wallMs);
std::println("Throughput   : {:.3f} M ops/s", throughput / 1'000'000.0);

    size_t totalRetries = 0;
    for(auto r : retryCounts) totalRetries += r;

    reportPercentiles("Bench Percentiles",btx.samples, draincap);
    std::println("Total Retries {}", totalRetries);
    std::println("Retries per op {}", totalRetries / totalOps);

  
    return;
}
void testPoolAllocateN() {
    constexpr size_t N = 100;
    memoryPool pool(N);
    std::unordered_set<Order*> allocatedPtrs;

    for (size_t i = 0; i < N; ++i) {
        Order* ptr = pool.allocate();
        assert(ptr != nullptr && "Allocation within capacity should not return null");
        assert(allocatedPtrs.insert(ptr).second && "Allocated pointers must be unique");
    }

    std::cout << "[PASS] Test 1: Allocate N - all distinct\n";
}

void testPoolAllocateNPlusOne() {
    constexpr size_t N = 50;
    memoryPool pool(N);

    for (size_t i = 0; i < N; ++i) {
        Order* ptr = pool.allocate();
        assert(ptr != nullptr);
    }

    Order* overflowPtr = pool.allocate();
    assert(overflowPtr == nullptr && "Allocation beyond capacity must return nullptr");

    std::cout << "[PASS] Test 2: Allocate N+1 - overflow returns null\n";
}

void testPoolDeallocateOneAllocate() {
    constexpr size_t N = 10;
    memoryPool pool(N);

    std::vector<Order*> ptrs;
    for (size_t i = 0; i < N; ++i) {
        ptrs.push_back(pool.allocate());
    }

    // Free one item
    Order* freedPtr = ptrs[4];
    pool.deallocate(freedPtr);

    // Re-allocate
    Order* reallocatedPtr = pool.allocate();
    assert(reallocatedPtr != nullptr && "Re-allocation after deallocate should succeed");
    assert(reallocatedPtr == freedPtr && "Recycled pointer should match the recently freed block");

    std::cout << "[PASS] Test 3: Deallocate one, allocate - successfully reused\n";
}

void testPoolAllocateAllFreeAllAllocateAll() {
    constexpr size_t N = 64;
    memoryPool pool(N);

    std::vector<Order*> firstRound;
    for (size_t i = 0; i < N; ++i) {
        Order* ptr = pool.allocate();
        assert(ptr != nullptr);
        firstRound.push_back(ptr);
    }

    assert(pool.allocate() == nullptr && "Pool should be exhausted");

    // Free everything
    for (Order* ptr : firstRound) {
        pool.deallocate(ptr);
    }

    // Allocate N again to verify full recovery
    std::unordered_set<Order*> secondRound;
    for (size_t i = 0; i < N; ++i) {
        Order* ptr = pool.allocate();
        assert(ptr != nullptr && "Free list should be fully recovered");
        assert(secondRound.insert(ptr).second && "Second round allocations must be distinct");
    }

    assert(pool.allocate() == nullptr && "Pool should be exhausted again");

    std::cout << "[PASS] Test 4: Allocate all, free all, allocate all - free list fully recovers\n";
}

void testPoolInterleavedAllocateFree() {
    constexpr size_t N = 20;
    memoryPool pool(N);

    std::unordered_set<Order*> livePtrs;
    std::vector<Order*> history;

    // Fixed sequence of allocate/free steps
    for (int step = 0; step < 500; ++step) {
        // Pseudo-random decision based on current count and step modulo
        bool doAllocate = (livePtrs.size() < N) && (step % 3 != 0 || livePtrs.empty());

        if (doAllocate) {
            Order* ptr = pool.allocate();
            assert(ptr != nullptr);
            // Invariant: returned pointer must NOT already be active in livePtrs
            assert(livePtrs.find(ptr) == livePtrs.end() && "Pointer handed out was already live!");
            livePtrs.insert(ptr);
            history.push_back(ptr);
        } else {
            Order* ptrToFree = *livePtrs.begin();
            livePtrs.erase(livePtrs.begin());
            pool.deallocate(ptrToFree);
        }
    }

    std::cout << "[PASS] Test 5: Interleaved allocate/free - zero double-allocations while live\n";
}

void runmemoryPoolTests() {
    testPoolAllocateN();
    testPoolAllocateNPlusOne();
    testPoolDeallocateOneAllocate();
    testPoolAllocateAllFreeAllAllocateAll();
    testPoolInterleavedAllocateFree();
}


};




 void testCancelMidMatch() {
    constexpr int iterations = 500;
    size_t cancelWon = 0, aggressorWon = 0;

    for (int it = 0; it < iterations; ++it) {
        OrderBook book;
        RingBuffer queue(1024);
        WriterContext ctx;
        ctx.captured.reserve(8);

        std::thread writer(writerLoop, std::ref(queue), std::ref(book), &ctx, nullptr);

        Order resting{Side::Buy, Type::Limit, 100, 50, 1001, 0};
        bool pushed = queue.push(Request{OpType::Submit, resting, 1001, std::nullopt, std::nullopt});
        assert(pushed);

        // Synchronise on the writer's counter, NOT by reading the book.
        while (ctx.processed.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        Order aggressor{Side::Sell, Type::Limit, 100, 50, 1002, 0};
        Request reqAggressor{OpType::Submit, aggressor, 1002, std::nullopt, std::nullopt};
        Request reqCancel   {OpType::Cancel, Order{},   1001, std::nullopt, std::nullopt};

        std::atomic<bool> go{false};
        std::thread t1([&]{ while (!go.load(std::memory_order_acquire)) {} queue.push(reqAggressor); });
        std::thread t2([&]{ while (!go.load(std::memory_order_acquire)) {} queue.push(reqCancel);    });
        go.store(true, std::memory_order_release);   // start gate: both threads already live

        t1.join();
        t2.join();
        queue.shutdown();
        writer.join();                               // join establishes happens-before on the book

        assert(!book.checkNoCrossedBook() && "Crossed book after cancel-mid-match");
        assert(!book.checkNoOrphans()     && "Orphaned cancelIndex entry");
        assert(book.checkFIFO()           && "FIFO violated");
        assert(ctx.captured.size() == 3   && "Request lost or duplicated");

        Quantity restingSell = book.quantityAt(Side::Sell, 100);
        Quantity restingBuy  = book.quantityAt(Side::Buy,  100);

        bool cancelFirst    = (restingSell == 50 && restingBuy == 0);  // cancel landed, aggressor rested
        bool aggressorFirst = (restingSell == 0  && restingBuy == 0);  // full cross, cancel no-opped

        assert((cancelFirst != aggressorFirst) && "Book in a state no interleaving can produce");
        cancelFirst ? ++cancelWon : ++aggressorWon;
    }

    std::println("[PASS] Adversarial Test 1: cancel-mid-match ({} cancel-first, {} aggressor-first)",
                 cancelWon, aggressorWon);
}
    struct ProducerResult { size_t accepted = 0; size_t rejected = 0; };

void backpressureWorker(RingBuffer& queue, int producerId,
                        int opsCount, ProducerResult& out) {
    producer prod(producerId);
    Price basePrice = 1000 + (producerId * 1000);

    for (int i = 0; i < opsCount; ++i) {
        Order o{Side::Buy, Type::Limit, basePrice + (i % 10), 10, prod.nextId(), 0};
        Request r{OpType::Submit, o, o.id, std::nullopt, std::nullopt};
        if (queue.push(r)) ++out.accepted;      // no retry — rejection is the point
        else               ++out.rejected;
    }
}

void testRejectOnFullUnderContention() {
    constexpr int    producerCount  = 4;
    constexpr int    opsPerProducer = 50000;
    constexpr size_t attempted      = size_t(producerCount) * opsPerProducer;
    constexpr size_t queueCapacity  = 32;       // deliberately far too small

    OrderBook book;
    RingBuffer queue(queueCapacity);
    WriterContext ctx;
    ctx.captured.reserve(attempted);

    std::vector<ProducerResult> results(producerCount);   // distinct elements: no data race

    std::thread writer(writerLoop, std::ref(queue), std::ref(book), &ctx, nullptr);

    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (int i = 0; i < producerCount; ++i)
        producers.emplace_back(backpressureWorker, std::ref(queue), i, opsPerProducer, std::ref(results[i]));

    for (auto& p : producers) p.join();
    queue.shutdown();
    writer.join();

    size_t accepted = 0, rejected = 0;
    for (const auto& r : results) { accepted += r.accepted; rejected += r.rejected; }

    // The property under test — two-sided.
    assert(accepted + rejected == attempted && "Request vanished: neither accepted nor rejected");
    assert(ctx.captured.size() == accepted  && "accepted-implies-executed violated");

    // Test validity: zero rejections means this run proved nothing.
    assert(rejected > 0 && "Queue never saturated — reduce queueCapacity");

    assert(!book.checkNoCrossedBook() && "Crossed book under saturation");
    assert(!book.checkNoOrphans()     && "Orphaned cancelIndex entry");
    assert(book.checkFIFO()           && "FIFO violated under saturation");
    assert(book.totalRestingVolume() == static_cast<Quantity>(accepted) * 10
           && "Volume conservation failed");

    std::println("[PASS] Adversarial Test 2: reject-on-full accepted={} rejected={} ({:.1f}% dropped)",
                 accepted, rejected, 100.0 * double(rejected) / double(attempted));
}

void retryingWorker(RingBuffer& queue, int producerId,
                    int opsCount, size_t& retriesOut) {
    producer prod(producerId);
    Price basePrice = 1000 + (producerId * 1000);
    size_t retries = 0;

    for (int i = 0; i < opsCount; ++i) {
        Order o{Side::Buy, Type::Limit, basePrice + (i % 10), 10, prod.nextId(), 0};
        Request r{OpType::Submit, o, o.id, std::nullopt, std::nullopt};

        size_t spins = 0;
        while (!queue.push(r)) {                 // absorb backpressure rather than drop
            ++retries;
            if (++spins >= 64) {                 // spin briefly before paying for a syscall
                spins = 0;
                std::this_thread::yield();
            }
        }
    }
    retriesOut = retries;
}

void testProducerOutrunsConsumer() {
    constexpr int    producerCount  = 4;
    constexpr int    opsPerProducer = 5000;
    constexpr size_t totalOps       = size_t(producerCount) * opsPerProducer;
    constexpr size_t queueCapacity  = 512;

    OrderBook book;
    RingBuffer queue(queueCapacity);
    WriterContext ctx;
    ctx.captured.reserve(totalOps);

    std::vector<size_t> retries(producerCount, 0);

    std::thread writer(writerLoop, std::ref(queue), std::ref(book), &ctx, nullptr);

    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (int i = 0; i < producerCount; ++i)
        producers.emplace_back(retryingWorker, std::ref(queue), i, opsPerProducer, std::ref(retries[i]));

    for (auto& p : producers) p.join();   // A: no more pushes possible
    queue.shutdown();                     // B: signal drain-then-exit
    writer.join();                        // C: writer has finished

    size_t totalRetries = 0;
    for (size_t r : retries) totalRetries += r;

    assert(ctx.captured.size() == totalOps && "Operation lost under sustained backpressure");
    assert(totalRetries > 0 && "Writer kept up — contention never occurred, test is vacuous");

    assert(!book.checkNoCrossedBook() && "Crossed book after high contention");
    assert(!book.checkNoOrphans()     && "Orphan entries in cancelIndex");
    assert(book.checkFIFO()           && "FIFO broken under high producer contention");
    assert(book.totalRestingVolume() == static_cast<Quantity>(totalOps) * 10
           && "Volume conservation check failed");

    std::println("[PASS] Adversarial Test 3: producer-outruns-consumer (retries absorbed: {})", totalRetries);
}

    Request makeRequest(int64_t id){
    Request req{};
    req.id = id;
    return req;
}
void pushMany(RingBuffer& queue, int producerId, int count){
    producer prod(producerId);
    

    for(int i = 0; i < count; ++i){
        auto req = makeRequest(prod.nextId());
        bool pushed = queue.push(req);
        assert(pushed);
    }

}

void testRingBufferConcurrent(){
 RingBuffer buffer(100000);
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
    RingBuffer rb(4);

    assert(rb.push(makeRequest(1)) == true);
    assert(rb.push(makeRequest(2)) == true);
    assert(rb.push(makeRequest(3)) == true);
    assert(rb.push(makeRequest(4)) == true);

    assert(rb.push(makeRequest(5)) == false);

    std::cout << "[PASS] Test 1: Fills and Refuses\n";
}

void testRingBufferFIFOOrder() {
    RingBuffer rb(4);

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
    RingBuffer rb(4);

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
    RingBuffer rb(4);

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

void testVolumeConservedPredicate() {
    std::cout << "Running volumeConserved direct unit tests...\n";

    // Test 1: Rejected, delta 0, traded 0 -> true
    assert(volumeConserved(100, 100, 50, 0, Type::Limit, true) == true);

    // Test 2: Rejected, delta 50, traded 0 -> false
    assert(volumeConserved(100, 150, 50, 0, Type::Limit, true) == false);

    // Test 3: Rejected, delta 0, traded 20 -> false
    assert(volumeConserved(100, 100, 50, 20, Type::Limit, true) == false);

    // Test 4: Limit, incoming 100, traded 0, delta +100 -> true
    // (volBefore = 0, volAfter = 100)
    assert(volumeConserved(0, 100, 100, 0, Type::Limit, false) == true);

    // Test 5: Limit, incoming 100, traded 40, delta +20 -> true
    // Net effect: +100 incoming - 40 resting removed - 40 aggressor filled = +20 delta
    assert(volumeConserved(1000, 1020, 100, 40, Type::Limit, false) == true);

    // Test 6: Limit, incoming 100, traded 100, delta -200 -> false
    // Expected delta: 100 - (100 * 2) = -100. Passing -200 should fail.
    assert(volumeConserved(1000, 800, 100, 100, Type::Limit, false) == false);

    // Test 7: Market, incoming 100, traded 60, delta -60 -> true
    // Net effect: 60 resting volume consumed, market order does not rest -> delta = -60
    assert(volumeConserved(1000, 940, 100, 60, Type::Market, false) == true);

    // Test 8: Market, incoming 100, traded 60, delta 0 -> false
    assert(volumeConserved(1000, 1000, 100, 60, Type::Market, false) == false);

    std::cout << "[PASS] All 8 volumeConserved unit tests PASSED successfully!\n";
}

/*void runRingBufferTests(){
    //testRingBufferConcurrent();
    testRingBufferFillsAndRefuses();
    testRingBufferFIFOOrder();
    testRingBufferDrainsAndRefuses();
    testRingBufferWrapAround();
    testVolumeConservedPredicate();
    testCancelMidMatch();
    testProducerOutrunsConsumer();
    testRejectOnFullUnderContention();


}*/



#ifndef TESTS_NO_MAIN


int main(){
    Test t;

    /*std::vector<Order> buyAggressorOrders {
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

    //runRingBufferTests();
    //t.testRingBufferConcurrentMatching();

    WriterContext ctx;
    t.testConcurrentGen(1, 1000000, ctx);
    
    if(ctx.invariant.has_value()){
        
        std::vector<LoggedOp> convedOps;
        for(auto reqs : ctx.captured){
           convedOps.push_back(convToOp(reqs));
           
        }
        std::cout << "Fuzzing detected an issue, attempting to shrink the sequence...\n";
        auto shrunk = t.shrinker(convedOps);
        auto replay = t.invReplay(convedOps);
        std::cout << "Shrunk sequence to " << shrunk.size() << " operations.\n";
    } else {
        std::cout << "Fuzzing completed without detecting issues.\n";

    }*/

    int drainCap = 1024;
    
        for (int i = 0; i < 5; ++i) {
            generator gen;
            OrderBook book;
            t.concurrentBench(2, 800000, 64, 10000);
            //generator fragGen(45,10,45);
            //t.fragTest(book, fragGen, 500'000, 5000, 1000);
        //t.generateAndExecute(book, gen, 400000);
        }
    
    
   

    //t.runmemoryPoolTests();
 
        return 0;
}
#endif