#pragma once
#include <cstdint>
enum class Side{
 
    Buy,
    Sell
};

enum class Type{
    Market,
    Limit
};

struct Order{
    Side side;
    Type type;
    int64_t price;
    int64_t quantity;
    int64_t id;
    int64_t seq;

    Order* next = nullptr;
    Order* prev = nullptr;

};


