#include <thread>
#include <iostream>
#include <functional>
#include <print>
#include <atomic>
#include <mutex>

struct account{
 int balance;
 std::mutex m;
};
void transfer(account& from, account& to, int amount){
    std::scoped_lock lock(from.m, to.m);
    from.balance -= amount;
    to.balance += amount;
    if(from.balance + to.balance == 2000) std::println("scoped lock passed");
}

void repeatTransfer(account& from, account& to, int amount, int times){
for (int i = 0; i < times; ++i) transfer(from, to, amount);

}

int main() {
    account a;
    a.balance = 1000;
    account b;
    b.balance = 1000;
    std::thread t1(repeatTransfer, std::ref(a), std::ref(b), 1,100000);
    std::thread t2(repeatTransfer, std::ref(b), std::ref(a), 1,100000);

    t1.join();
    t2.join();

    return 0;
}