#pragma once
#include <map>
#include <unordered_map>
#include <list>
#include <vector>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include <optional>
#include <any>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_set>
#include <utility>
#include "orderClass.hpp"

using Price = int64_t;
using Quantity = int64_t;
using Id = int64_t;
using orderIterator = std::list<Order>::iterator;

size_t spinCount = 750;

struct memoryPool{
    std::vector<Order> slots;
    Order* freeHead = nullptr;

    Order* allocate(){
        if(!freeHead){
            return nullptr;
        }
        Order* temp = freeHead;
        freeHead = temp->next;
        temp->next = nullptr;
        temp->prev = nullptr;

        return temp;
    }

    void deallocate(Order* o){
        o->next = freeHead;
        freeHead = o;
    }

    memoryPool(size_t n)
    : slots(std::vector<Order>(n)) 
    {
        for(size_t i = n; i-- > 0;){
            deallocate(&slots[i]);
        }
    }
    // never resize after construction

    memoryPool(const memoryPool&) = delete;
    

 
};

struct LevelIterator{
    Order* current;

    Order& operator*(){
        return *current;
    }

    LevelIterator& operator++(){
        current = current->next;
        return *this;
    }

    bool operator!=(const LevelIterator& other) const{
        return current != other.current;
    }
};
struct ConstLevelIterator{
    const Order* current;

    const Order& operator*() {
        return *current;
    }

    ConstLevelIterator& operator++(){
        current = current->next;
        return *this;
    }

    bool operator!=(const ConstLevelIterator& other) const{
        return current != other.current;
    }
};
constexpr int producerBits = 7;
constexpr int counterBits  = 56;
constexpr int producerShift = counterBits;
constexpr int maxProducers = (1 << producerBits);

enum class OpType { Submit, Cancel, Modify};

struct producer{
int producerId;
int64_t counter = 0;

Id nextId(){
    return(static_cast<int64_t>(producerId) << producerShift) | counter++;
}
};

int producerOf(Id id){
    return static_cast<int>(id >> producerShift);
}


struct level{
    Order* head;
    Order* tail;

    LevelIterator begin(){return {head};}
    LevelIterator end(){return {nullptr};}

    ConstLevelIterator begin() const{ return {head};}
    ConstLevelIterator end() const {return {nullptr};}

    void unlink(Order* o){
       if(o->prev){
        o->prev->next = o->next;
       }else{
        head = o->next;
       }

       if(o->next){
        o->next->prev = o->prev;
       }else{
        tail = o->prev;
       }

       o->prev = nullptr;
       o->next = nullptr;
    }
    void linkBack(Order* o){
        if(!tail){
            head = o;
            tail = o;
        }else{
            auto oldTail = tail;
            oldTail->next = o;
            o->prev = oldTail;
            tail = o;

        }
    }
};



class OrderBook{
private:
    memoryPool pool;
    std::map<Price, level> asks;
    std::map<Price, level> bids;
    std::unordered_map<Id,Order*> cancelIndex;
    int64_t nextSeq = 0;

    

public:

    OrderBook(size_t poolSize = 100000): pool(poolSize) {}

    std::map<Price, level>& getMap(Side side){
        if(side == Side::Buy){
            return bids;
        }else{
            return asks;
        }
    }
    const std::map<Price, level>& getMap(Side side) const{
        if(side == Side::Buy){
            return bids;
        }else{
            return asks;
        }
    }

    Side opposite(Side side){
        if(side == Side::Buy){
            return Side::Sell;
        }else{
            return Side::Buy;
        }
    }

    std::optional<Order> getOrderInfo(Id id) const{
        auto it = cancelIndex.find(id);
        if (it == cancelIndex.end()) return std::nullopt;
        auto orderIt = it->second;
        return Order{orderIt->side, orderIt->type, orderIt->price, orderIt->quantity, id, orderIt->seq};
    }


    bool validate(const Order& o){
        if(contains(o.id)) return false;
        else if(o.quantity <= 0) return false;
        else if(o.type == Type::Limit){
            if(o.price <= 0) return false;
            else return true;
        }
        else return true;
    }

   Order* best(Side s) {
        if (s == Side::Buy) {
            if (!bids.empty()) return bids.rbegin()->second.head;
        } else {
            if (!asks.empty()) return asks.begin()->second.head;
        }
        return nullptr;
    }

   const Order* best(Side s) const {
        if (s == Side::Buy) {
            if (!bids.empty()) return bids.rbegin()->second.head;
        } else {
            if (!asks.empty()) return asks.begin()->second.head;
        }
        return nullptr;
    }
   
    bool contains(Id id) const {
        return cancelIndex.find(id) != cancelIndex.end();
    }

    
        void restInto(std::map<Price, level>& map, Order* o){
            map[o->price].linkBack(o);
    }

    
    bool rest(Order& o){
        if(!(validate(o))) return false;
        Order* slot = pool.allocate();
        if(slot == nullptr) return false;
        *slot = o;
        slot->next = nullptr;
        slot->prev = nullptr;
            slot->seq = nextSeq++;
        if (o.side == Side::Buy) restInto(bids, slot);
        else                     restInto(asks, slot);

        cancelIndex[slot->id] = slot;
        
        return true;
    }

    struct Fill{
        int64_t price; //resting order price
        int64_t quantity;
        int64_t agressorId;
        int64_t restingId;

        Fill(int64_t price, int64_t quantity, int64_t agressorId, int64_t restingId)
        : price(price), 
        quantity(quantity), 
        agressorId(agressorId), 
        restingId(restingId) {}
            
    
    };
    Quantity totalBidVolume() const {
        Quantity volume = 0;
        for (const auto& [price, lvl] : bids) {
            for (const auto& order : lvl) {
                volume += order.quantity;
            }
        }
        return volume;
    }

    Quantity totalAskVolume() const {
        Quantity volume = 0;
        for (const auto& [price, lvl] : asks) {
            for (const auto& order : lvl) {
                volume += order.quantity;
            }
        }
        return volume;
    }

    Quantity totalRestingVolume() const {
        return totalBidVolume() + totalAskVolume();
    }

    Quantity totalCancelIndexVolume() const {
        Quantity volume = 0;
        for (const auto& [id, orderIt] : cancelIndex) {
            volume += orderIt->quantity;
        }
        return volume;
    }
    
 
    std::optional<std::vector<Fill>> submit(Order& incoming){
        if(!validate(incoming)) return std::nullopt;
        std::vector<Fill> fills;
        auto& oppositeSide = getMap(opposite(incoming.side));
            while (incoming.quantity > 0 && !oppositeSide.empty()) {
               auto resting = best(opposite(incoming.side));
               bool crosses;
               if (incoming.type == Type::Market){
                crosses = true;
               } else {
                if(incoming.side == Side::Buy){
                    crosses = incoming.price >= resting->price;
                }else{
                    crosses = resting->price >= incoming.price;
                }
               }
                    if (!crosses) break;
                    int64_t tradeQty = std::min(incoming.quantity, resting->quantity);
                    incoming.quantity -= tradeQty;
                    resting->quantity -= tradeQty;
                    /*std::cout << "FILL: " << tradeQty << " @ " << resting->price 
                              << " (Aggressor ID: " << incoming.id 
                              << ", Resting ID: " << resting->id << ")\n";*/
                    fills.emplace_back(resting->price, tradeQty, incoming.id, resting->id);

                    if (resting->quantity == 0) {
                        Id restingId = resting->id;
                        cancel(restingId);
                    }
                

            
        }

        if (incoming.type == Type::Limit && incoming.quantity > 0) {
            rest(incoming);
        }
        return fills;
    }

    struct ExpectedLevel {
        Side side;
        Price price;
        int64_t quantity;
    };

    int64_t quantityAt(Side side, Price price) const{
        int64_t levelQty = 0;
        if(side == Side::Buy){
            if(bids.find(price) != bids.end()){
             auto const& priceLevel = bids.at(price);
             for(const auto& i : priceLevel){
                levelQty += i.quantity;
             }
            }
            return levelQty;
        }else{
            if(asks.find(price) != asks.end()){
                auto const& priceLevel = asks.at(price);
                for(auto const& i : priceLevel){
                    levelQty += i.quantity;
                }
            }
            return levelQty;
        }
    }

     bool cancel(Id id){
        auto it = cancelIndex.find(id);
        if(it == cancelIndex.end()) return false;
        auto o = it->second;
        const Price p = o->price;
        const Side s = o->side;
        
        auto& map = getMap(s);
        auto levelIt = map.find(p);

           if(levelIt != map.end()) {
            levelIt->second.unlink(o);
            pool.deallocate(o);
                if(levelIt->second.head == nullptr){
                   map.erase(levelIt);
                }
           }
            
        cancelIndex.erase(it);
        return true;
    }

        
    

   bool  modify(Id id, std::optional<Price> newPrice, std::optional<Quantity> newQuantity){
        auto it = cancelIndex.find(id);
        if (it == cancelIndex.end()) return false;
        if(newQuantity == 0){
            cancel(id);
            return true;
        } 
        auto orderIt = it->second;
        Order& order = *orderIt;
        Price currentPrice = orderIt->price;
        Quantity currentQuantity = orderIt->quantity;
        Side currentSide = orderIt->side;

        if(newPrice.has_value() && *newPrice != order.price){
            cancel(id);
            currentPrice = *newPrice;
            if(newQuantity.has_value()){
            currentQuantity = *newQuantity;
            }
            Order replacement{currentSide, Type::Limit, currentPrice, currentQuantity, id, 0};
            submit(replacement);
            return true;
        
        }else if(newQuantity.has_value()){
                if(*newQuantity >  currentQuantity){
                    cancel(id);
                    currentQuantity = *newQuantity;
                    Order replacement{currentSide, Type::Limit, currentPrice, currentQuantity, id, 0};
                    submit(replacement);
                }else if(order.quantity > *newQuantity){
                    order.quantity = *newQuantity;
                }
                return true;
        }else{
            return false;
        }
           

        
   }

   std::optional<std::vector<Order>> checkNoCrossedBook() const{
    
        const Order* bestBid = best(Side::Buy);
        const Order* bestAsk = best(Side::Sell);

        if(bestBid == nullptr || bestAsk == nullptr){
            return std::nullopt;
        }

        if(bestBid->price >= bestAsk->price){
          return std::vector<Order> {*bestBid, *bestAsk};  
        }else{
            return std::nullopt;
        }
    
   }

   std::optional<std::vector<Order>> checkNoOrphans() const{
    std::vector<Order> orphans;
    for(const auto& id : cancelIndex){
        auto it = id.second;
        Order&  order = *it;
        if(id.first != order.id){
            orphans.push_back(order);
        }
    }
    if(orphans.empty()){
        return std::nullopt;
    }else{
        return orphans;
    }

   }

   bool checkFIFO() const{
    for(const auto& [price, level] : bids){
        int64_t lastSeq = -1;
        for(const auto& order : level){
            if(lastSeq != -1 && order.seq < lastSeq){
                return false;
            }
            lastSeq = order.seq;
        }
    }

    for(const auto& [price, level] : asks){
        int64_t lastSeq = -1;
        for(const auto& order : level){
            if(lastSeq != -1 && order.seq < lastSeq){
                return false;
            }
            lastSeq = order.seq;
        }
    }

    return true;
   }
   
std::vector<Id> idsAt(Side s, Price p) const{
    std::vector<Id> ids;
    auto& map = getMap(s);
    auto priceLevel = map.find(p);
    if(priceLevel != map.end()){
        for(auto o : priceLevel->second){
        ids.push_back(o.id);
    }
    }
    
    return ids;
}

};

struct Request{
    OpType requestType;
    Order order;
    Id id;
    std::optional<Price> newPrice;
    std::optional<Quantity> newQuantity;
};

 

struct RingBuffer {
private:
    size_t capacity;
    std::vector<Request> buffer;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    std::atomic<size_t> count = 0;
    alignas(64) std::mutex m;
    std::condition_variable convar;
    bool stopping = false;

    bool isFull() const {
        if (count == capacity) return true;
        return false;
    }
    bool isEmpty() const {
        if (count == 0) return true;
        return false;
    }
    Request takeRequestLocked() {
        auto request = buffer[head];
        head = (head + 1) % capacity;
        --count;
        return request;
    }
public:

    bool push(const Request& r) {
        std::unique_lock<std::mutex> lock(m);
        if (isFull()) return false;
        else {
            buffer[tail] = r;
            tail = (tail + 1) % capacity;
            ++count;
            convar.notify_all();
            return true;
        }
    }

    std::optional<Request> pop() {
        std::unique_lock<std::mutex> lock(m);
        if (isEmpty()) return std::nullopt;
        else {
            return takeRequestLocked();
        }
    }

    std::optional<Request> waitAndPop() {
        std::unique_lock<std::mutex> lock(m);
        convar.wait(lock, [this]{ return count > 0 || stopping; });
        if (stopping && isEmpty()) return std::nullopt;
        else {
            return takeRequestLocked();
        }
    }

    ssize_t waitAndDrain(std::vector<Request>& out, size_t maxItems, bool* didSleep = nullptr, double* lockNs = nullptr) {
    size_t drained = 0;

    auto lockStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(m);
    auto lockEnd = std::chrono::steady_clock::now();

    if (lockNs) {
        *lockNs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(lockEnd - lockStart).count()
        );
    }

    if (didSleep) {
        *didSleep = (count == 0 && !stopping);
    }

    convar.wait(lock, [this]{ return count > 0 || stopping; });

    if (stopping && isEmpty()) return drained;
    else {
        out.clear();
        while (count > 0 && drained < maxItems) {
            auto req = takeRequestLocked();
            out.push_back(std::move(req));
            ++drained;
        }
    }
    return drained;
}

    void shutdown() {
        std::unique_lock<std::mutex> lock(m);
        stopping = true;
        convar.notify_all();
    }

    RingBuffer(size_t capacity)
        : capacity(capacity), buffer(capacity) {}
};

enum class vio {
    crossedBook,
    orphan,
    fifo,
    volumeCon
};

struct WriterContext {
    std::vector<Request> captured;
    std::optional<vio> invariant;
    std::optional<size_t> invarIndex;
    std::optional<std::vector<Order>> offendingOrders;
    std::atomic<size_t> processed{0};
};

struct BenchContext {
    std::vector<double> samples;
    std::vector<double> lockAcquireNs; // Time spent blocked attempting to acquire std::mutex (per batch)
    std::vector<double> totalBatchNs;  // Total time including lock acquisition + drain + processing (per op)
    std::vector<bool> didSleep;
    size_t drainCap = 64;
};

bool volumeConserved(Quantity volBefore, Quantity volAfter, Quantity incomingQuantity, Quantity tradedQty, Type orderType, bool rejected) {
    if (rejected) {
        return ((volAfter - volBefore) == 0 && tradedQty == 0);
    } else if (orderType == Type::Limit) {
        return (((volAfter - volBefore) == incomingQuantity - (tradedQty * 2)));
    } else {
        return (((volAfter - volBefore) == -tradedQty));
    }
}

void writerLoop(RingBuffer& queue, OrderBook& book, WriterContext* ctx = nullptr, BenchContext* btx = nullptr) {
    std::vector<Request> drained;

    auto processOne = [&](Request& cRequest) -> void {
        if (ctx && !ctx->invariant) {
            ctx->captured.push_back(cRequest);
            ctx->processed.fetch_add(1, std::memory_order_release);
        }
        auto& cOrder = cRequest.order;
        auto& rType = cRequest.requestType;
        if (rType == OpType::Submit) {
            if (ctx) {
                auto volPreSub = book.totalRestingVolume();
                auto cOrderQuantity = cOrder.quantity;
                auto cOrderType = cOrder.type;
                auto fills = book.submit(cOrder);
                auto volPostSub = book.totalRestingVolume();
                int64_t tradeQuantity = 0;
                if (fills.has_value()) {
                    for (const auto& fill : *fills) {
                        tradeQuantity += fill.quantity;
                    }
                    if (!(volumeConserved(volPreSub, volPostSub, cOrderQuantity, tradeQuantity, cOrderType, false))) {
                        ctx->invariant = vio::volumeCon;
                        ctx->invarIndex = ctx->captured.size() - 1;
                    }
                } else {
                    if (!(volumeConserved(volPreSub, volPostSub, cOrderQuantity, tradeQuantity, cOrderType, true))) {
                        ctx->invariant = vio::volumeCon;
                        ctx->invarIndex = ctx->captured.size() - 1;
                    }
                }
            } else {
                book.submit(cOrder);
            }
        } else if (rType == OpType::Modify) {
            book.modify(cRequest.id, cRequest.newPrice, cRequest.newQuantity);
        } else {
            book.cancel(cRequest.id);
        }

        if (ctx && !ctx->invariant) {
            if (book.checkNoCrossedBook()) {
                ctx->invariant = vio::crossedBook;
                ctx->invarIndex = ctx->captured.size() - 1;
            }
            if (book.checkNoOrphans() != std::nullopt) {
                ctx->invariant = vio::orphan;
                ctx->invarIndex = ctx->captured.size() - 1;
            }
            if (!book.checkFIFO()) {
                ctx->invariant = vio::fifo;
                ctx->invarIndex = ctx->captured.size() - 1;
            }
        }
    };

    if (btx) {
        drained.reserve(btx->drainCap);

        while (true) {
            bool slept = false;
            double lockNs = 0.0;
            auto start = std::chrono::steady_clock::now();

            size_t n = queue.waitAndDrain(drained, btx->drainCap, &slept, &lockNs);
            if (n == 0) break;

            for (auto& r : drained) {
                processOne(r);
            }

            auto end = std::chrono::steady_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            btx->samples.push_back(static_cast<double>(ns) / static_cast<double>(n));
            btx->didSleep.push_back(slept);
            btx->lockAcquireNs.push_back(lockNs);
        }
    } else {
        drained.reserve(64);
        while (true) {
            size_t n = queue.waitAndDrain(drained, 64);
            if (n == 0) break;
            for (auto& r : drained) {
                processOne(r);
            }
        }
    }
}



