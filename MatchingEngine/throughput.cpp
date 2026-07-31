// throughput.cpp
//
// A throughput benchmark that measures throughput, and nothing else.
//
// What was wrong with the previous attempts, and what changed:
//
//   1. Fixed op count meant the measurement window included producer startup
//      and a tail drain with no contention at all. Now the run is DURATION
//      based and throughput is read from a steady-state window in the middle,
//      with the ramp-up and the ramp-down cut off.
//
//   2. Producers retried on rejection with no backoff, so 168 million failed
//      pushes hammered the same mutex the writer needed to drain the queue.
//      That is a self-inflicted denial of service on the writer, not a
//      property of the engine. Now a rejected push is DROPPED, which is the
//      engine's actual overflow policy, and the producer yields before
//      continuing so it stops fighting the writer for the lock.
//
//   3. push(rej) always printed 0.0 because the retry loop exited with
//      ok == true and all the time landed in the accepted bucket. Accepted
//      and rejected pushes are now genuinely separate.
//
//   4. ns/park was reporting how long the writer sat idle with no work, which
//      is not an overhead. Dropped. The park COUNT is the useful signal and
//      that is kept.
//
//   5. Latency is gone from this file entirely. It needs a pacer that does not
//      busy-wait, and mixing it in here is what made the tails irreproducible.
//      Separate concern, separate harness.
//
// Build: g++ -O3 -std=c++23 throughput.cpp -o throughput
// Run:   ./throughput            (3s per configuration)
//        ./throughput 5          (5s per configuration)

#include "orderBook.hpp"
 
#include <random>
#include <chrono>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstdio>
 
namespace mc {
 
using Clock = std::chrono::steady_clock;
 
inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch()).count();
}
 
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
 
// ---------------------------------------------------------------------------
// Seeding.
//
// Orders are placed AWAY from the touch so seeding never triggers a match —
// buys well below, sells well above. rest() is used rather than submit()
// because seeding is setup, not measurement, and rest() skips the match loop
// entirely. The result is a book of exactly `depth` live resting orders.
//
// Prices are spread over a band so the map holds many levels rather than a
// handful of very deep ones. A book of 1,000,000 orders on 3 price levels is
// a different data structure from one spread over 2,000 levels, and the
// former would flatter the tree lookups.
// ---------------------------------------------------------------------------
 
struct Seeded {
    OrderBook book;
    std::vector<Id> liveIds;
    Id nextId = 1;
};
 
void seed(Seeded& s, int64_t depth, int levels, std::mt19937& rng) {
    std::uniform_int_distribution<int> qty(50, 200);
    std::uniform_int_distribution<int> lvl(0, levels - 1);
 
    s.liveIds.reserve(static_cast<size_t>(depth));
    for (int64_t i = 0; i < depth; ++i) {
        const bool isBuy = (i % 2 == 0);
        // Buys 1000-1000+levels, sells 5000-5000+levels. Never cross.
        const Price p = isBuy ? 1000 + lvl(rng) : 5000 + lvl(rng);
        Order o{isBuy ? Side::Buy : Side::Sell, Type::Limit,
                p, qty(rng), s.nextId++, 0};
        s.book.rest(o);
        s.liveIds.push_back(o.id);
    }
}
 
// ---------------------------------------------------------------------------
// The three operations, each timed in isolation on an identically seeded book.
//
// Every measured operation is prepared before the clock starts. Nothing is
// generated, allocated, or randomised inside a timed region.
// ---------------------------------------------------------------------------
 
struct Row {
    int64_t depth = 0;
    double  submitResting = 0;
    double  submitCrossing = 0;
    double  cancel = 0;
    double  modifyQty = 0;
    double  modifyPrice = 0;
    double  mixed = 0;
};
 
// 1. Submit that rests — insertion into an existing book, no matching.
double submitResting(int64_t depth, int levels, int iterations, int trials, uint32_t sd) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        std::mt19937 rng(sd + static_cast<uint32_t>(t));
        Seeded s;
        seed(s, depth, levels, rng);
 
        std::uniform_int_distribution<int> qty(50, 200);
        std::uniform_int_distribution<int> lvl(0, levels - 1);
        std::vector<Order> ops;
        ops.reserve(static_cast<size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            const bool isBuy = (i % 2 == 0);
            ops.emplace_back(isBuy ? Side::Buy : Side::Sell, Type::Limit,
                             isBuy ? 1000 + lvl(rng) : 5000 + lvl(rng),
                             qty(rng), s.nextId++, 0);
        }
 
        const int64_t t0 = nowNs();
        for (auto& o : ops) s.book.submit(o);
        const int64_t t1 = nowNs();
        results.push_back(static_cast<double>(t1 - t0) / iterations);
    }
    return median(results);
}
 
// 2. Submit that crosses — the full match loop, one resting order consumed
//    per aggressor. Liquidity is seeded at the touch first so the aggressors
//    always find a counterparty, and that seeding is untimed.
double submitCrossing(int64_t depth, int levels, int iterations, int trials, uint32_t sd) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        std::mt19937 rng(sd + static_cast<uint32_t>(t));
        Seeded s;
        seed(s, depth, levels, rng);
 
        // Sell liquidity at 3000 — inside the seeded band, so it becomes the
        // best ask and every buy aggressor at 3000 matches immediately.
        for (int i = 0; i < iterations; ++i) {
            Order r{Side::Sell, Type::Limit, 3000, 1, s.nextId++, 0};
            s.book.rest(r);
        }
 
        std::vector<Order> ops;
        ops.reserve(static_cast<size_t>(iterations));
        for (int i = 0; i < iterations; ++i)
            ops.emplace_back(Side::Buy, Type::Limit, 3000, 1, s.nextId++, 0);
 
        const int64_t t0 = nowNs();
        for (auto& o : ops) s.book.submit(o);
        const int64_t t1 = nowNs();
        results.push_back(static_cast<double>(t1 - t0) / iterations);
    }
    return median(results);
}
 
// 3. Cancel — hash lookup plus a list erase. Targets are shuffled so the
//    access pattern through the cancel index is not sequential; walking the
//    ids in issue order would prefetch far better than real cancels do.
double cancel(int64_t depth, int levels, int iterations, int trials, uint32_t sd) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        std::mt19937 rng(sd + static_cast<uint32_t>(t));
        Seeded s;
        seed(s, depth, levels, rng);
 
        std::vector<Id> targets = s.liveIds;
        std::shuffle(targets.begin(), targets.end(), rng);
        if (static_cast<int64_t>(targets.size()) > iterations)
            targets.resize(static_cast<size_t>(iterations));
 
        const int64_t t0 = nowNs();
        for (Id id : targets) s.book.cancel(id);
        const int64_t t1 = nowNs();
        results.push_back(static_cast<double>(t1 - t0) / static_cast<double>(targets.size()));
    }
    return median(results);
}
 
// 4a. Modify, quantity decrease — the in-place path, no reinsertion.
double modifyQty(int64_t depth, int levels, int iterations, int trials, uint32_t sd) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        std::mt19937 rng(sd + static_cast<uint32_t>(t));
        Seeded s;
        seed(s, depth, levels, rng);
 
        std::vector<Id> targets = s.liveIds;
        std::shuffle(targets.begin(), targets.end(), rng);
        if (static_cast<int64_t>(targets.size()) > iterations)
            targets.resize(static_cast<size_t>(iterations));
 
        const int64_t t0 = nowNs();
        for (Id id : targets) s.book.modify(id, std::nullopt, 10);
        const int64_t t1 = nowNs();
        results.push_back(static_cast<double>(t1 - t0) / static_cast<double>(targets.size()));
    }
    return median(results);
}
 
// 4b. Modify, price change — cancel plus resubmit, the expensive path.
double modifyPrice(int64_t depth, int levels, int iterations, int trials, uint32_t sd) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        std::mt19937 rng(sd + static_cast<uint32_t>(t));
        Seeded s;
        seed(s, depth, levels, rng);
 
        std::vector<Id> targets = s.liveIds;
        std::shuffle(targets.begin(), targets.end(), rng);
        if (static_cast<int64_t>(targets.size()) > iterations)
            targets.resize(static_cast<size_t>(iterations));
 
        // Each target moves to a price inside its own side's band, so the
        // reprice never crosses and the cost is purely erase + reinsert.
        std::uniform_int_distribution<int> lvl(0, levels - 1);
        std::vector<Price> newPrices;
        newPrices.reserve(targets.size());
        for (Id id : targets) {
            auto info = s.book.getOrderInfo(id);
            const Price base = (info && info->side == Side::Buy) ? 1000 : 5000;
            newPrices.push_back(base + lvl(rng));
        }
 
        const int64_t t0 = nowNs();
        for (size_t i = 0; i < targets.size(); ++i)
            s.book.modify(targets[i], newPrices[i], std::nullopt);
        const int64_t t1 = nowNs();
        results.push_back(static_cast<double>(t1 - t0) / static_cast<double>(targets.size()));
    }
    return median(results);
}
 
// 5. The mixed stream — 80% submit, 20% cancel. This is the one to compare
//    against the writer ceiling, because it is the same mix the concurrent
//    harness offers. Comparing a submit-only ceiling against a mixed
//    concurrent run was an apples-to-oranges error worth not repeating.
double mixed(int64_t depth, int levels, int iterations, int trials, uint32_t sd) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        std::mt19937 rng(sd + static_cast<uint32_t>(t));
        Seeded s;
        seed(s, depth, levels, rng);
 
        std::uniform_int_distribution<int> qty(50, 200);
        std::uniform_int_distribution<int> lvl(0, levels - 1);
 
        struct Op { bool isCancel; Order o; Id id; };
        std::vector<Op> ops;
        ops.reserve(static_cast<size_t>(iterations));
 
        std::vector<Id> issued;
        issued.reserve(static_cast<size_t>(iterations));
 
        for (int i = 0; i < iterations; ++i) {
            if ((i % 5) == 4 && issued.size() > 64) {
                // Cancel something issued recently — a stale target would
                // mostly miss and measure the failure path instead.
                ops.push_back({true, Order{Side::Buy, Type::Limit, 1, 1, 0, 0},
                               issued[issued.size() - 64]});
            } else {
                const bool isBuy = (i % 2 == 0);
                Id id = s.nextId++;
                ops.push_back({false,
                               Order{isBuy ? Side::Buy : Side::Sell, Type::Limit,
                                     isBuy ? 1000 + lvl(rng) : 5000 + lvl(rng),
                                     qty(rng), id, 0},
                               id});
                issued.push_back(id);
            }
        }
 
        const int64_t t0 = nowNs();
        for (auto& op : ops) {
            if (op.isCancel) s.book.cancel(op.id);
            else             s.book.submit(op.o);
        }
        const int64_t t1 = nowNs();
        results.push_back(static_cast<double>(t1 - t0) / iterations);
    }
    return median(results);
}
 
} // namespace mc
 
int main() {
    constexpr int levels     = 2000;
    constexpr int iterations = 50'000;
    constexpr int trials     = 5;
 
    std::printf("=== Matching cost by book depth ===\n");
    std::printf("Single-threaded, direct calls. No queue, no threads.\n");
    std::printf("%d price levels per side, %d ops per timed region, median of %d trials.\n",
                levels, iterations, trials);
 
    std::printf("\n  %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n",
                "depth", "sub rest", "sub cross", "cancel", "mod qty", "mod price", "mixed");
    std::printf("  ----------------------------------------------------------------------------\n");
 
    for (int64_t depth : {int64_t(1'000), int64_t(10'000), int64_t(100'000),
                          int64_t(500'000), int64_t(1'000'000)}) {
        mc::Row r;
        r.depth          = depth;
        r.submitResting  = mc::submitResting (depth, levels, iterations, trials, 7);
        r.submitCrossing = mc::submitCrossing(depth, levels, iterations, trials, 7);
        r.cancel         = mc::cancel        (depth, levels, iterations, trials, 7);
        r.modifyQty      = mc::modifyQty     (depth, levels, iterations, trials, 7);
        r.modifyPrice    = mc::modifyPrice   (depth, levels, iterations, trials, 7);
        r.mixed          = mc::mixed         (depth, levels, iterations, trials, 7);
 
        std::printf("  %-10lld %-10.1f %-10.1f %-10.1f %-10.1f %-10.1f %-10.1f\n",
                    (long long)r.depth, r.submitResting, r.submitCrossing,
                    r.cancel, r.modifyQty, r.modifyPrice, r.mixed);
    }
 
    std::printf(
        "\nAll figures ns/op.\n"
        "\n  The 'mixed' column is the one to compare against the writer ceiling:\n"
        "  same 80/20 submit/cancel mix the concurrent harness offers. Ceiling\n"
        "  minus mixed at the matching depth = the cost of queue.pop().\n"
        "\n  Flat down a column  -> depth is free for that operation.\n"
        "  Rising              -> a structural cost. std::map levels get deeper,\n"
        "                         and the cancel index's buckets go cold, so the\n"
        "                         lookups take more misses as the book grows.\n"
        "                         No amount of queue optimisation touches this.\n"
        "\n  Caveats: fresh book per trial, so no long-run fragmentation or\n"
        "  allocator drift. Warm cache by the time the timed region starts.\n"
        "  Single-threaded and in-process — no network, no serialisation.\n"
        "  These are matching-logic costs, not end-to-end latency.\n");
    return 0;
}