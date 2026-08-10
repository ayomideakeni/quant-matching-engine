#define TESTS_NO_MAIN 1
#include "tests.cpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <atomic>
#include <vector>

// =====================================================================
//  Depth sweep: every benchmark takes `levels` = the number of distinct
//  prices the book is spread across. Total resting orders and total
//  timed operations are CONSTANT at every depth, so the only thing that
//  changes between rows is the width of the price tree.
//
//    levels = 10     ->  10 very deep levels
//    levels = 10000  ->  10000 very shallow levels
//
//  Read the notes at the bottom of main() before quoting any column.
// =====================================================================

constexpr int BATCH_SIZE          = 10;
constexpr int INPLACE_BATCH_SIZE  = 100;

// ---------------------------------------------------------------------
//  Stats
// ---------------------------------------------------------------------


// ---------------------------------------------------------------------
//  Clock Overhead Calibration (unchanged)
// ---------------------------------------------------------------------
void measureClockOverhead() {
    constexpr int64_t N = 10'000'000;
    using Clock = std::chrono::steady_clock;

    int64_t clock_acc = 0;
    const auto start_clock = Clock::now();
    for (int64_t i = 0; i < N; ++i) {
        clock_acc += Clock::now().time_since_epoch().count();
    }
    const auto end_clock = Clock::now();

    int64_t empty_acc = 0;
    const auto start_empty = Clock::now();
    for (int64_t i = 0; i < N; ++i) {
        empty_acc += i;
    }
    const auto end_empty = Clock::now();

    const double clock_total_ns = std::chrono::duration<double, std::nano>(end_clock - start_clock).count();
    const double empty_total_ns = std::chrono::duration<double, std::nano>(end_empty - start_empty).count();

    const double net_total_ns = clock_total_ns - empty_total_ns;
    const double per_call_ns  = net_total_ns / static_cast<double>(N);
    const double per_pair_ns  = per_call_ns * 2.0;

    std::cout << "====================================================\n";
    std::cout << " Measuring Instrument Calibration (Clock Overhead)\n";
    std::cout << "====================================================\n";
    std::cout << " Clock total time : " << clock_total_ns / 1e6 << " ms\n";
    std::cout << " Empty total time : " << empty_total_ns / 1e6 << " ms\n";
    std::cout << " Per-call overhead: " << per_call_ns << " ns\n";
    std::cout << " Per-pair overhead: " << per_pair_ns << " ns\n";
    std::cout << " (Side-effects: clock_acc=" << clock_acc << ", empty_acc=" << empty_acc << ")\n";
    std::cout << " Note: Empty loop may unroll/vectorize; subtraction is approximate.\n";
    std::cout << "====================================================\n\n";

    
}
void calibrateSpinCost() {
    constexpr int iterations = 1'000'000;
    static std::atomic<size_t> dummy{0};
    size_t sink = 0;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        sink += dummy.load(std::memory_order_acquire);

        asm volatile("" ::: "memory");
    }
    auto end = std::chrono::steady_clock::now();

    auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double nsPerIteration = static_cast<double>(totalNs) / iterations;
    
    // 1000 ns in a microsecond
    double spinsPerMicro = 1000.0 / nsPerIteration;

    std::cout << " Measuring Instrument Calibration (Spin Cost)\n";
    std::cout << "  Total time       : " << totalNs / 1'000'000.0 << " ms\n";
    std::cout << "  Per-load overhead: " << nsPerIteration << " ns\n";
    std::cout << "  Derived spin rate: " << static_cast<int>(spinsPerMicro) << " iterations per 1 us\n";
    std::cout << "  (Side-effects: sink=" << sink << ")\n\n";
}
// ---------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------

// Deterministic shuffle so cancel/modify targets are drawn from across the
// book rather than walking it in seed order, but the draw order is
// reproducible run-to-run.
static void shuffleTargets(std::vector<Id>& ids, int trial) {
    std::mt19937 rng(0xC0FFEEu + static_cast<unsigned>(trial));
    std::shuffle(ids.begin(), ids.end(), rng);
}

// Spread price for index i across [1, levels].
static inline Price spreadPrice(int i, int levels) {
    return static_cast<Price>((i % levels) + 1);
}

// A price spread across [1, levels] with a stride that isn't 1, so
// modify-price targets don't land in a predictable ascending walk.
static inline Price scatterPrice(int i, int levels) {
    return static_cast<Price>((static_cast<int64_t>(i) * 7919 % levels) + 1);
}

// ---------------------------------------------------------------------
//  1. Submit — resting only (insertion cost)
// ---------------------------------------------------------------------
Stats benchmarkSubmitResting(generator& gen, int levels, int iterations,
                             int warmup = 1000, int trials = 5) {
    const int lowerBand = std::max(1, levels / 2);          // buys  : [1, lowerBand]
    const int upperBand = std::max(1, levels - lowerBand);  // sells : (lowerBand, levels]

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>((iterations - warmup) / BATCH_SIZE) * trials);

    for (int t = 0; t < trials; ++t) {
        OrderBook book;

        auto priceFor = [&](int i, bool isBuy) -> Price {
            return isBuy ? static_cast<Price>((i / 2) % lowerBand + 1)
                         : static_cast<Price>(lowerBand + (i / 2) % upperBand + 1);
        };

        // --- Seed: `iterations` resting orders spread across `levels` prices.
        // Constant order count at every depth; only the tree width changes.
        for (int i = 0; i < iterations; ++i) {
            bool isBuy = (i % 2 == 0);
            Order o{isBuy ? Side::Buy : Side::Sell, Type::Limit,
                    priceFor(i, isBuy), 100, gen.nextId++, 0};
            book.rest(o);
        }

        // --- Timed orders also span the full band, so we insert across the
        // whole tree rather than repeatedly down one path.
        std::vector<Order> orders;
        orders.reserve(iterations);
        for (int i = 0; i < iterations; ++i) {
            bool isBuy = (i % 2 == 0);
            // offset the index so timed inserts don't mirror the seed order
            orders.emplace_back(isBuy ? Side::Buy : Side::Sell, Type::Limit,
                                priceFor(i + 1, isBuy), 100, gen.nextId++, 0);
        }

        for (int i = 0; i < warmup; ++i) { book.submit(orders[i]); }

        for (int i = warmup; i + BATCH_SIZE <= iterations; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) { book.submit(orders[i + b]); }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    return computeStats(samples);
}

// ---------------------------------------------------------------------
//  2. Submit — always crosses (match loop cost)
//     NB: see the confound note in main(). Matching only ever touches
//     begin(), so this column is structurally near-flat by construction.
// ---------------------------------------------------------------------
Stats benchmarkSubmitCrossing(generator& gen, int levels, int iterations,
                              int warmup = 1000, int trials = 5) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>((iterations - warmup) / BATCH_SIZE) * trials);

    for (int t = 0; t < trials; ++t) {
        OrderBook book;

        // Seed sells across `levels` prices, qty 1 each -> every aggressor
        // is a full fill. Count is constant across depths.
        for (int i = 0; i < iterations + warmup; ++i) {
            Order resting{Side::Sell, Type::Limit, spreadPrice(i, levels), 1, gen.nextId++, 0};
            book.rest(resting);
        }

        // Aggressor priced at the top of the band always crosses the best ask.
        const Price aggressorPrice = static_cast<Price>(levels);

        auto runBatch = [&](int n) {
            for (int i = 0; i < n; ++i) {
                Order aggressor{Side::Buy, Type::Limit, aggressorPrice, 1, gen.nextId++, 0};
                book.submit(aggressor);
            }
        };

        runBatch(warmup);

        const size_t numBatches = static_cast<size_t>((iterations - warmup) / BATCH_SIZE);
        for (size_t b = 0; b < numBatches; ++b) {
            auto start = std::chrono::steady_clock::now();
            runBatch(BATCH_SIZE);
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    return computeStats(samples);
}

// ---------------------------------------------------------------------
//  3. Cancel
// ---------------------------------------------------------------------
Stats benchmarkCancel(generator& gen, int levels, int iterations,
                      int warmup = 1000, int trials = 5) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>((iterations - warmup) / BATCH_SIZE) * trials);

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, spreadPrice(i, levels), 1, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        // Targets drawn from across the book, not in seed order.
        shuffleTargets(ids, t);

        for (int i = 0; i < warmup; ++i) { book.cancel(ids[i]); }

        for (int i = warmup; i + BATCH_SIZE <= iterations; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) { book.cancel(ids[i + b]); }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    return computeStats(samples);
}

// ---------------------------------------------------------------------
//  4. Modify — in place (quantity decrease)
// ---------------------------------------------------------------------
Stats benchmarkModifyInPlace(generator& gen, int levels, int iterations,
                             int warmup = 10'000, int trials = 5) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>((iterations - warmup) / INPLACE_BATCH_SIZE) * trials);

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, spreadPrice(i, levels), 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        shuffleTargets(ids, t);

        for (int i = 0; i < warmup; ++i) {
            book.modify(ids[i], std::nullopt, 50);
        }

        // Side-effect accumulator to defeat dead-store elimination.
        int64_t dummy_sink = 0;

        for (int i = warmup; i + INPLACE_BATCH_SIZE <= iterations; i += INPLACE_BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < INPLACE_BATCH_SIZE; ++b) {
                book.modify(ids[i + b], std::nullopt, 50);
            }
            auto end = std::chrono::steady_clock::now();

            auto info = book.getOrderInfo(ids[i]);
            if (info) dummy_sink += info->quantity;

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(INPLACE_BATCH_SIZE));
        }

        if (dummy_sink == 42) std::cout << " ";
    }
    return computeStats(samples);
}

// ---------------------------------------------------------------------
//  5. Modify — cancel + resubmit (price change)
// ---------------------------------------------------------------------
Stats benchmarkModifyCancelResubmit(generator& gen, int levels, int iterations,
                                    int warmup = 1000, int trials = 5) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>((iterations - warmup) / BATCH_SIZE) * trials);

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, spreadPrice(i, levels), 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        shuffleTargets(ids, t);

        // Destination price also spans the band: the erase lands anywhere in
        // the tree and the reinsert lands anywhere else. All buys, so nothing
        // crosses.
        for (int i = 0; i < warmup; ++i) {
            book.modify(ids[i], scatterPrice(i, levels), std::nullopt);
        }

        for (int i = warmup; i + BATCH_SIZE <= iterations; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) {
                book.modify(ids[i + b], scatterPrice(i + b, levels), std::nullopt);
            }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    return computeStats(samples);
}

// ---------------------------------------------------------------------
//  6. Mixed flow — generateRequest's weighted mix at a single depth
//
//  Not part of the sweep. This is the composite figure: realistic
//  operation mix against a seeded book, one depth, generation done
//  entirely outside the timed region.
// ---------------------------------------------------------------------
struct MixComposition {
    size_t submits = 0, cancels = 0, modifies = 0, total = 0;
};

static inline void applyRequest(OrderBook& book, const Request& r) {
    switch (r.requestType) {
        case OpType::Submit: {
            Order o = r.order;              // copy: submit takes a mutable Order
            book.submit(o);
            break;
        }
        case OpType::Cancel:
            book.cancel(r.id);
            break;
        case OpType::Modify:
            book.modify(r.id, r.newPrice, r.newQuantity);
            break;
    }
}

Stats benchmarkMixedFlow(generator& gen, int levels, int iterations,
                         int warmup, int trials, MixComposition* compOut) {
    const int lowerBand = std::max(1, levels / 2);
    const int upperBand = std::max(1, levels - lowerBand);

    std::vector<double> samples;
    Test tb;
    samples.reserve(static_cast<size_t>((iterations - warmup) / BATCH_SIZE) * trials);

    for (int t = 0; t < trials; ++t) {
        OrderBook book;

        // Seed a two-sided, non-crossing book at this depth. rest() bypasses
        // matching, so the seed must be non-crossing by construction: buys
        // below, sells above.
        for (int i = 0; i < iterations; ++i) {
            bool isBuy = (i % 2 == 0);
            Price p = isBuy ? static_cast<Price>((i / 2) % lowerBand + 1)
                            : static_cast<Price>(lowerBand + (i / 2) % upperBand + 1);
            Order o{isBuy ? Side::Buy : Side::Sell, Type::Limit, p, 100, gen.nextId++, 0};
            book.rest(o);
        }

        // Producer id 1, not 0: producer 0's ids start in the same low block
        // as gen.nextId and would collide with the seeded orders.
        producer prod(1);
        std::vector<Request> flow = tb.generateRequest(gen, prod, iterations);

        if (t == 0 && compOut) {
            for (size_t i = static_cast<size_t>(warmup); i < flow.size(); ++i) {
                switch (flow[i].requestType) {
                    case OpType::Submit: ++compOut->submits;  break;
                    case OpType::Cancel: ++compOut->cancels;  break;
                    case OpType::Modify: ++compOut->modifies; break;
                }
                ++compOut->total;
            }
        }

        const int n = static_cast<int>(flow.size());
        for (int i = 0; i < warmup && i < n; ++i) { applyRequest(book, flow[i]); }

        for (int i = warmup; i + BATCH_SIZE <= n; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) { applyRequest(book, flow[i + b]); }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    return computeStats(samples);
}

// ---------------------------------------------------------------------
//  Process-level warm-up
//
//  Per-benchmark warm-up cannot cover this. Each trial builds a fresh
//  OrderBook, but the FIRST book built in the process is the one that
//  pays for growing the heap: page faults on first touch, arena
//  expansion, the allocator's free lists starting empty. Later books
//  reuse an arena that is already faulted in. That is a per-PROCESS
//  cost, so whichever depth is measured first absorbs it — which is
//  exactly the ~2x tail effect that followed the sweep when it was
//  reversed.
//
//  Sized to the sweep's PEAK, not merely "representative": submit-resting
//  seeds `iterations` orders and then submits `iterations` more, so peak
//  live orders is ~2x iterations. A warm-up smaller than the peak leaves
//  the sweep still faulting new pages.
// ---------------------------------------------------------------------
void processWarmup(generator& gen, int iterations) {
    const int peak   = iterations * 2;
    const int levels = 10'000;   // widest tree the sweep will build

    for (int pass = 0; pass < 2; ++pass) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(peak);

        // Resting inserts across a wide band: grows the arena to peak size
        // and populates the cancel index to peak occupancy.
        for (int i = 0; i < peak; ++i) {
            Order o{Side::Buy, Type::Limit, spreadPrice(i, levels), 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        // Touch each hot path so its code and branch history are warm too.
        const int slice = peak / 8;
        for (int i = 0; i < slice; ++i) {
            book.modify(ids[i], std::nullopt, 50);
        }
        for (int i = slice; i < 2 * slice; ++i) {
            book.modify(ids[i], scatterPrice(i, levels), std::nullopt);
        }
        for (int i = 2 * slice; i < 3 * slice; ++i) {
            book.cancel(ids[i]);
        }

        // Crossing path.
        for (int i = 0; i < slice; ++i) {
            Order sell{Side::Sell, Type::Limit, spreadPrice(i, levels), 1, gen.nextId++, 0};
            book.submit(sell);
        }

        // Book destroyed here: frees everything back to the allocator, which
        // is the state the sweep's first trial will actually start from.
    }
}

// ---------------------------------------------------------------------
//  Sweep driver
// ---------------------------------------------------------------------
struct Row {
    int   levels = 0;
    Stats subRest, subCross, cancel, modQty, modPrice;
};

static void printTable(const char* which,
                       const std::vector<Row>& rows,
                       double Stats::*field) {
    std::printf("\n  %-8s  %-12s %-12s %-12s %-12s %-12s\n",
                which, "sub rest", "sub cross", "cancel", "mod qty", "mod price");
    std::printf("  ------------------------------------------------------------------------\n");
    for (const Row& r : rows) {
        std::printf("  %-8d  %-12.1f %-12.1f %-12.1f %-12.1f %-12.1f\n",
                    r.levels,
                    r.subRest.*field, r.subCross.*field, r.cancel.*field,
                    r.modQty.*field,  r.modPrice.*field);
    }
}

void runDepthSweep(generator& gen, int iterations = 100'000, int trials = 5) {
    std::cout << "=== Matching Engine Depth Sweep (submit/cancel/modify isolated, -O3) ===\n";

    measureClockOverhead();

    // REVERSED. If the anomalous p99 row tracks the FIRST depth measured
    // rather than the 10-level case, it's cold-start bleeding past the
    // per-benchmark warm-up, not a property of shallow books. Flip this list
    // back to ascending to confirm from the other direction.
    const std::vector<int> sweepOrder = {10'000, 1'000, 100, 10};

    std::printf("  sweep order:");
    for (int l : sweepOrder) std::printf(" %d", l);
    std::printf("   (reversed)\n\n");

    std::vector<Row> rows;
    for (int levels : sweepOrder) {
        std::printf("  ... running levels = %d\n", levels);
        std::fflush(stdout);

        Row r;
        r.levels   = levels;
        r.subRest  = benchmarkSubmitResting        (gen, levels, iterations, 1'000,  trials);
        r.subCross = benchmarkSubmitCrossing       (gen, levels, iterations, 1'000,  trials);
        r.cancel   = benchmarkCancel               (gen, levels, iterations, 1'000,  trials);
        r.modQty   = benchmarkModifyInPlace        (gen, levels, iterations, 10'000, trials);
        r.modPrice = benchmarkModifyCancelResubmit (gen, levels, iterations, 1'000,  trials);
        rows.push_back(r);
    }

    std::printf("\n%d resting orders and %d timed ops at every depth; only the number\n"
                "of distinct price levels changes. Median of %d trials. All figures ns/op.\n",
                iterations, iterations, trials);

    // Measured high-to-low; printed low-to-high so the curve reads normally.
    std::vector<Row> sorted = rows;
    std::sort(sorted.begin(), sorted.end(),
              [](const Row& a, const Row& b) { return a.levels < b.levels; });

    printTable("levels", sorted, &Stats::p50);
    printTable("levels", sorted, &Stats::p99);

    std::printf(
        "\n  First table is p50, second is p99.\n"
        "\n  Reading the columns:\n"
        "    Flat down a column -> tree width is free for that operation.\n"
        "    Rising             -> a structural cost: deeper std::map walks,\n"
        "                          colder cancel-index buckets, or both.\n"
        "\n  KNOWN CONFOUND — 'sub cross'. Matching only ever touches the best\n"
        "  level, so tree width cannot affect the match loop directly; this\n"
        "  column should be near-flat by construction. The residual signal in\n"
        "  it is level-EMPTYING frequency: with total resting orders and timed\n"
        "  ops both held constant, more levels means fewer orders per level\n"
        "  means more map-node erases per fill. That cannot be held constant\n"
        "  under the same-book-size constraint. Do not quote this column as a\n"
        "  depth curve. The other four are clean.\n"
        "\n  Caveats: each sample is the mean of a batch (10 ops, 100 for mod\n"
        "  qty), so p99 is a p99 of batch means, not of individual operations —\n"
        "  it understates the true tail. Fresh book per trial, so no long-run\n"
        "  fragmentation or allocator drift. Single-threaded, in-process, no\n"
        "  queue: matching-logic cost, not end-to-end latency.\n");
}

// ---------------------------------------------------------------------
//  Mixed-flow section (separate deliverable, single depth)
// ---------------------------------------------------------------------
void runMixedFlow(generator& gen, int iterations = 100'000, int trials = 5) {
    // Depth is pinned to 100 because generateRequest's price band is a
    // hardcoded priceDist(1, 100). Seeding any other depth would mean the
    // flow only ever touches part of the book, which would make the number
    // meaningless rather than merely narrow.
    constexpr int levels = 100;

    std::printf("\n\n=== Mixed flow (generateRequest weighted mix, depth = %d) ===\n", levels);

    MixComposition comp;
    Stats s = benchmarkMixedFlow(gen, levels, iterations, 1'000, trials, &comp);

    std::printf("\n  %-10s %-10s %-10s %-10s %-10s\n", "mean", "p50", "p99", "p99.9", "max");
    std::printf("  --------------------------------------------------------\n");
    std::printf("  %-10.1f %-10.1f %-10.1f %-10.1f %-10.1f\n",
                s.mean, s.p50, s.p99, s.p999, s.max);

    if (comp.total > 0) {
        const double t = static_cast<double>(comp.total);
        std::printf("\n  Realised mix over the timed region (trial 1): "
                    "submit %.1f%%, cancel %.1f%%, modify %.1f%% of %zu ops\n",
                    100.0 * comp.submits  / t,
                    100.0 * comp.cancels  / t,
                    100.0 * comp.modifies / t,
                    comp.total);
    }

    std::printf(
        "\n  What this is: one composite ns/op under generateRequest's own\n"
        "  weighted mix against a seeded book, generation done entirely\n"
        "  outside the timed region. It is NOT comparable to the isolated\n"
        "  columns above — those measure one operation each on purpose.\n"
        "\n  Read it with three things in mind:\n"
        "    - generateRequest's cancel/modify targets come from its own\n"
        "      rolling id window, so they hit orders the flow itself\n"
        "      submitted, not the seeded book. Some fraction are already\n"
        "      matched away or were market orders that never rested, so\n"
        "      they resolve as index misses rather than real work. That is\n"
        "      realistic, but it means the composite is not a weighted\n"
        "      average of the isolated figures and should not reconcile\n"
        "      against them.\n"
        "    - The mix is the fuzzer's mix (~50%% modify), chosen for bug\n"
        "      pressure, not fitted to real venue flow. It is a stated\n"
        "      assumption, not a measurement.\n"
        "    - The submit path copies the Order out of the Request inside\n"
        "      the timed region. Small POD copy, but it is in the number.\n");
}



int main(int argc, char** argv) {
    calibrateSpinCost();
    int  iterations = (argc > 1) ? std::atoi(argv[1]) : 100'000;
    bool warm       = (argc > 2) ? (std::atoi(argv[2]) != 0) : true;

    generator gen;

    if (warm) {
        std::printf("Process warm-up: 2 passes at peak size (%d orders, 10000 levels)...\n",
                    iterations * 2);
        std::fflush(stdout);
        processWarmup(gen, iterations);
        std::printf("Process warm-up done. Nothing above this line is measured.\n\n");
    } else {
        std::printf("Process warm-up DISABLED (argv[2]=0). Cold-start control run.\n\n");
    }
    

    runDepthSweep(gen, iterations);
    runMixedFlow(gen, iterations);
    return 0;
}