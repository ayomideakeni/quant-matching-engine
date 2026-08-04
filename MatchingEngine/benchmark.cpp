#define TESTS_NO_MAIN 1
#include "tests.cpp"


constexpr int BATCH_SIZE = 10;
void reportPercentiles(const std::string& label, std::vector<double>& samples) {
    if (samples.empty()) return;

    // 1. Sort to extract percentiles
    std::sort(samples.begin(), samples.end());

    const size_t n = samples.size();
    const double p50  = samples[n * 50 / 100];      // Median
    const double p99  = samples[n * 99 / 100];      // 99th percentile
    const double p999 = samples[n * 999 / 1000];    // 99.9th percentile
    const double max  = samples.back();             // Max tail latency

    // Compute arithmetic mean across all batch samples
    double total_sum = 0.0;
    for (double s : samples) total_sum += s;
    const double mean = total_sum / static_cast<double>(n);

    std::cout << "-----------------------------------------------------\n";
    std::cout << label << " (Samples: " << n << " batches of 10)\n";
    std::cout << "  Mean  : " << mean  << " ns/op\n";
    std::cout << "  p50   : " << p50   << " ns/op\n";
    std::cout << "  p99   : " << p99   << " ns/op\n";
    std::cout << "  p99.9 : " << p999  << " ns/op\n";
    std::cout << "  Max   : " << max   << " ns/op\n";
    std::cout << "-----------------------------------------------------\n";
}

// Clock Overhead Calibration Function
void measureClockOverhead() {
    constexpr int64_t N = 10'000'000; // Iteration count for micro-benchmarking accuracy

    // Use steady_clock to match the rest of the benchmark suite
    using Clock = std::chrono::steady_clock;

    // --- 1. Clock Loop ---
    int64_t clock_acc = 0;
    const auto start_clock = Clock::now();
    for (int64_t i = 0; i < N; ++i) {
        clock_acc += Clock::now().time_since_epoch().count();
    }
    const auto end_clock = Clock::now();

    // --- 2. Empty Baseline Loop ---
    int64_t empty_acc = 0;
    const auto start_empty = Clock::now();
    for (int64_t i = 0; i < N; ++i) {
        empty_acc += i;
    }
    const auto end_empty = Clock::now();

    // Convert durations to nanoseconds
    const double clock_total_ns = std::chrono::duration<double, std::nano>(end_clock - start_clock).count();
    const double empty_total_ns = std::chrono::duration<double, std::nano>(end_empty - start_empty).count();

    // Compute per-call and per-pair cost
    const double net_total_ns = clock_total_ns - empty_total_ns;
    const double per_call_ns = net_total_ns / static_cast<double>(N);
    const double per_pair_ns = per_call_ns * 2.0;

    // Output results and consume accumulators so the compiler doesn't optimize away the loops
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

double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void report(const std::string& label, const std::vector<double>& nsPerOpResults) {
    std::cout << "-----------------------------------\n";
    std::cout << label << "\n";
    for (size_t t = 0; t < nsPerOpResults.size(); ++t)
        std::cout << "  Trial " << (t + 1) << ": " << nsPerOpResults[t] << " ns/op\n";
    double median = medianOf(nsPerOpResults);
    std::cout << "  Median: " << median << " ns/op  (" 
              << (1'000'000'000.0 / median) << " ops/sec)\n";
}




// ---- 1. Submit, resting only ----
void benchmarkSubmitResting(generator& gen, int iterations, int warmup = 1000, int trials = 5) {
    std::uniform_int_distribution<int> quantityDist(1, 100);
    std::uniform_int_distribution<int> priceDist(1, 100);

    const int timedOps = iterations - warmup;
    const size_t numBatches = static_cast<size_t>(timedOps / BATCH_SIZE);
    std::vector<double> samples;
    samples.reserve(numBatches * static_cast<size_t>(trials));

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Order> orders;
        orders.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            bool isBuy = (i % 2 == 0);
            int price = isBuy ? (priceDist(gen.rng) % 50 + 1) : (priceDist(gen.rng) % 50 + 51);
            orders.emplace_back(isBuy ? Side::Buy : Side::Sell, Type::Limit,
                                 price, quantityDist(gen.rng), gen.nextId++, 0);
        }

        // Warmup
        for (int i = 0; i < warmup; ++i) { book.submit(orders[i]); }

        // Timed batches
        for (int i = warmup; i < iterations; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) { book.submit(orders[i + b]); }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    reportPercentiles("SUBMIT — resting only (insertion cost)", samples);
}

// ---- 2. Submit, always crosses ----
void benchmarkSubmitCrossing(generator& gen, int iterations, int warmup = 1000, int trials = 5) {
    const int timedOps = iterations - warmup;
    const size_t numBatches = static_cast<size_t>(timedOps / BATCH_SIZE);
    std::vector<double> samples;
    samples.reserve(numBatches * static_cast<size_t>(trials));

    for (int t = 0; t < trials; ++t) {
        OrderBook book;

        for (int i = 0; i < iterations + warmup; ++i) {
            Order resting{Side::Sell, Type::Limit, 50, 1, gen.nextId++, 0};
            book.rest(resting);
        }

        auto runBatch = [&](int n) {
            for (int i = 0; i < n; ++i) {
                Order aggressor{Side::Buy, Type::Limit, 50, 1, gen.nextId++, 0};
                book.submit(aggressor);
            }
        };

        // Warmup
        runBatch(warmup);

        // Timed batches
        for (size_t b = 0; b < numBatches; ++b) {
            auto start = std::chrono::steady_clock::now();
            runBatch(BATCH_SIZE);
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    reportPercentiles("SUBMIT — always crosses (match loop cost)", samples);
}

// ---- 3. Cancel ----
void benchmarkCancel(generator& gen, int iterations, int warmup = 1000, int trials = 5) {
    const int timedOps = iterations - warmup;
    const size_t numBatches = static_cast<size_t>(timedOps / BATCH_SIZE);
    std::vector<double> samples;
    samples.reserve(numBatches * static_cast<size_t>(trials));

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, static_cast<Price>((i % 90) + 1), 1, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        // Warmup
        for (int i = 0; i < warmup; ++i) { book.cancel(ids[i]); }

        // Timed batches
        for (int i = warmup; i < iterations; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) { book.cancel(ids[i + b]); }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    reportPercentiles("CANCEL", samples);
}

constexpr int INPLACE_BATCH_SIZE = 100; 

void benchmarkModifyInPlace(generator& gen, int iterations = 100'000, int warmup = 10'000, int trials = 5) {
    const int timedOps = iterations - warmup;
    const size_t numBatches = static_cast<size_t>(timedOps / INPLACE_BATCH_SIZE);

    std::vector<double> samples;
    samples.reserve(numBatches * static_cast<size_t>(trials));

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            Price p = static_cast<Price>((i % 90) + 1);
            Order o{Side::Buy, Type::Limit, p, 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        // 1. Warmup outside timing region
        for (int i = 0; i < warmup; ++i) { 
            book.modify(ids[i], std::nullopt, 50); 
        }

        // Side-effect accumulator to defeat dead-code elimination (DCE) across compiler optimization flags
        int64_t dummy_sink = 0;

        // 2. Timed batches with N=1000
        for (int i = warmup; i < iterations; i += INPLACE_BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();

            for (int b = 0; b < INPLACE_BATCH_SIZE; ++b) { 
                book.modify(ids[i + b], std::nullopt, 50);
            }

            auto end = std::chrono::steady_clock::now();

            // Force compiler to treat book modifications as observeable side-effects
            auto info = book.getOrderInfo(ids[i]);
            if (info) dummy_sink += info->quantity;

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(INPLACE_BATCH_SIZE));
        }

        // Prevent sink from being optimized away
        if (dummy_sink == 42) std::cout << " "; 
    }

    reportPercentiles("MODIFY — in-place (quantity decrease, BATCH=1000)", samples);
}

// ---- 5. Modify — Cancel/Resubmit ----
void benchmarkModifyCancelResubmit(generator& gen, int iterations, int warmup = 1000, int trials = 5) {
    const int timedOps = iterations - warmup;
    const size_t numBatches = static_cast<size_t>(timedOps / BATCH_SIZE);
    std::vector<double> samples;
    samples.reserve(numBatches * static_cast<size_t>(trials));

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);

        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, static_cast<Price>((i % 90) + 1), 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        // Warmup
        for (int i = 0; i < warmup; ++i) { book.modify(ids[i], 5, std::nullopt); }

        // Timed batches
        for (int i = warmup; i < iterations; i += BATCH_SIZE) {
            auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < BATCH_SIZE; ++b) { book.modify(ids[i + b], 5, std::nullopt); }
            auto end = std::chrono::steady_clock::now();

            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            samples.push_back(static_cast<double>(ns) / static_cast<double>(BATCH_SIZE));
        }
    }
    reportPercentiles("MODIFY — cancel+resubmit (price change)", samples);
}

// ---- Run Everything ----
void runTrueBenchmark(generator& gen, int iterations = 100000) {
    std::cout << "=== Matching Engine Benchmark Suite (submit/cancel/modify isolated, -O3) ===\n";
    
    // 1. Calibrate measuring instrument first
    measureClockOverhead();

    // 2. Run engine micro-benchmarks
    benchmarkSubmitResting(gen, iterations);
    benchmarkSubmitCrossing(gen, iterations);
    benchmarkCancel(gen, iterations);
    benchmarkModifyInPlace(gen, iterations);
    benchmarkModifyCancelResubmit(gen, iterations);

    std::cout << "\nNOTE: single-threaded, fixed-size books, no memory-pressure/long-run effects,\n"
              << "      no book-size sweep. This is an informal W9 baseline, not the full W9 suite.\n";
}



int main(int argc, char** argv) {
    generator gen; // Uses the generator from tests.cpp
    runTrueBenchmark(gen, 100'000);
    return 0;
}