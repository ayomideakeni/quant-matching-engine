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
    std::list<Order> orders;
    //orderIterator iterator;
};



class OrderBook{
private:
    std::map<Price, level> asks;
    std::map<Price, level> bids;
    std::unordered_map<Id,orderIterator> cancelIndex;
    int64_t nextSeq = 0;

public:

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
        orderIterator orderIt = it->second;
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
            if (!bids.empty()) return &bids.rbegin()->second.orders.front();
        } else {
            if (!asks.empty()) return &asks.begin()->second.orders.front();
        }
        return nullptr;
    }

   const Order* best(Side s) const {
        if (s == Side::Buy) {
            if (!bids.empty()) return &bids.rbegin()->second.orders.front();
        } else {
            if (!asks.empty()) return &asks.begin()->second.orders.front();
        }
        return nullptr;
    }
   
    bool contains(Id id) const {
        return cancelIndex.find(id) != cancelIndex.end();
    }

    template <typename BookSide>
        orderIterator restInto(BookSide& book, Order& o){
            auto& lst = book[o.price].orders;
            return lst.insert(lst.end(), o);
    }

    
    bool rest(Order& o){
        if(!(validate(o))) return false;
            o.seq = nextSeq++;
        orderIterator it;
        if (o.side == Side::Buy) it = restInto(bids, o);
        else                     it = restInto(asks, o);

        cancelIndex[o.id] = it;
        
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
            for (const auto& order : lvl.orders) {
                volume += order.quantity;
            }
        }
        return volume;
    }

    Quantity totalAskVolume() const {
        Quantity volume = 0;
        for (const auto& [price, lvl] : asks) {
            for (const auto& order : lvl.orders) {
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
             auto const& priceLevel = bids.at(price).orders;
             for(const auto& i : priceLevel){
                levelQty += i.quantity;
             }
            }
            return levelQty;
        }else{
            if(asks.find(price) != asks.end()){
                auto const& priceLevel = asks.at(price).orders;
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
        orderIterator orderIt = it->second;
        const Price p = orderIt-> price;
        const Order& order = *orderIt;
        
        auto& map = getMap(order.side);

           if(map.find(order.price) != map.end()) {
            map.at(order.price).orders.erase(orderIt);
                if(map.at(p).orders.empty()){
                    map.erase(p);
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
        orderIterator orderIt = it->second;
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
        for(const auto& order : level.orders){
            if(lastSeq != -1 && order.seq < lastSeq){
                return false;
            }
            lastSeq = order.seq;
        }
    }

    for(const auto& [price, level] : asks){
        int64_t lastSeq = -1;
        for(const auto& order : level.orders){
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
        for(auto o : priceLevel->second.orders){
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

 


struct RingBuffer{
private:
    size_t capacity;
    std::vector<Request> buffer;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    size_t count = 0;
    alignas(64) std::mutex m;
    std::condition_variable convar;
    bool stopping = false;
   

    bool isFull() const {
        if(count == capacity) return true;
        return false;
    }
    bool isEmpty() const{
        if(count == 0) return true;
        return false;
    }
    Request takeRequestLocked(){
        auto request = buffer[head];
        head = (head + 1) % capacity;
        --count;
        return request;
    }
public:

    bool push(const Request& r){
        std::unique_lock<std::mutex> lock(m);
        if(isFull()) return false;
        else{
            buffer[tail] = r;
            tail = (tail + 1) % capacity;
            ++count;
            convar.notify_all();
            return true;
        }
        
    }
    std::optional<Request> pop(){
        std::unique_lock<std::mutex> lock(m);
        if(isEmpty()) return std::nullopt;
        else{
            return takeRequestLocked();
        }
    }
    std::optional<Request> waitAndPop(){
        std::unique_lock<std::mutex> lock(m);
        convar.wait(lock, [this]{ return count > 0 || stopping; });
        if(stopping && isEmpty()) return std::nullopt;
        else{
            return takeRequestLocked();
        }
    }
    void shutdown(){
        std::unique_lock<std::mutex> lock(m);
        stopping = true;
        convar.notify_all();
    }
    RingBuffer(size_t capacity)
    :capacity(capacity),
    buffer(capacity){}

};

enum class vio{
    crossedBook,
    orphan,
    fifo,
    volumeCon
};

struct WriterContext{
    std::vector<Request> captured;
    std::optional<vio> invariant;
    std::optional<size_t> invarIndex;
    std::optional<std::vector<Order>> offendingOrders;
    std::atomic<size_t> processed{0};
};

bool volumeConserved(Quantity volBefore, Quantity volAfter, Quantity incomingQuantity, Quantity tradedQty, Type orderType, bool rejected){
    if(rejected){
        return ((volAfter - volBefore) == 0 && tradedQty == 0);
    }else if(orderType == Type::Limit){
        return (((volAfter - volBefore) == incomingQuantity - (tradedQty*2)));
    }else{
        return (((volAfter - volBefore) == -tradedQty));
    }
}

void writerLoop(RingBuffer& queue, OrderBook& book, WriterContext* ctx = nullptr){

    while(true){
        auto request = queue.waitAndPop();
        if(!request) break;
        else{
            auto& cRequest = *request;
            //std::println("Popped Request ID: {}", cRequest.id);
            if(ctx && !ctx->invariant){
                ctx->captured.push_back(cRequest);
                ctx->processed.fetch_add(1, std::memory_order_release);
            }
            auto& cOrder = cRequest.order;
            auto& rType = cRequest.requestType;
            if(rType == OpType::Submit){
                if(ctx){
                auto volPreSub = book.totalRestingVolume();
                auto cOrderQuantity = cOrder.quantity;
                auto cOrderType = cOrder.type;
                auto fills = book.submit(cOrder);
                auto volPostSub = book.totalRestingVolume();
                int64_t tradeQuantity = 0;
                if(fills.has_value()){
                    for(const auto& fill :*fills){
                        tradeQuantity += fill.quantity;
                    }
                    if(!(volumeConserved(volPreSub, volPostSub, cOrderQuantity, tradeQuantity, cOrderType, false))){
                        ctx->invariant = vio::volumeCon;
                        ctx->invarIndex = ctx->captured.size() - 1;
                    }
                }else{
                    if(!(volumeConserved(volPreSub, volPostSub, cOrderQuantity, tradeQuantity, cOrderType, true))){
                        ctx->invariant = vio::volumeCon;
                        ctx->invarIndex = ctx->captured.size() - 1;
                        
                    }
                }
                }else{
                    book.submit(cOrder);

                }
                //std::println("Submitted Order ID: {}", cOrder.id);
            }else if(rType == OpType::Modify){
                book.modify(cRequest.id, cRequest.newPrice, cRequest.newQuantity);
                //std::println("Modified Order ID : {} with price: {} and quantity: {}", cRequest.id, cRequest.newPrice.value_or((-1)), cRequest.newQuantity.value_or(-1));
            }else{
                book.cancel(cRequest.id);
                //std::println("Cancel Request Fufilled");
            }
            if(ctx && !ctx->invariant){
                if(book.checkNoCrossedBook()){
                ctx->invariant = vio::crossedBook;
                ctx->invarIndex = ctx->captured.size() - 1;
                
            } 
            if(book.checkNoOrphans() != std::nullopt){
               ctx->invariant = vio::orphan;
               ctx->invarIndex = ctx->captured.size() - 1;
            
            } 
            if(!book.checkFIFO()){
                ctx->invariant = vio::fifo;
                ctx->invarIndex = ctx->captured.size() - 1;
            
            } 
            }
            
        }
    }

}



