/*#include <tests.cpp>

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

// ---- 1. Submit, resting-only (no crossing — pure insertion cost) ----
// Orders are all Buy at low prices / Sell at high prices so nothing ever crosses.
void benchmarkSubmitResting(generator& gen, int iterations, int warmup = 1000, int trials = 5) {
    std::uniform_int_distribution<int> quantityDist(1, 100);
    std::uniform_int_distribution<int> priceDist(1, 100);
    std::vector<double> results;

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Order> orders;
        orders.reserve(iterations);
        // Buys priced 1-50, sells priced 51-100 — the two sides can never cross.
        for (int i = 0; i < iterations; ++i) {
            bool isBuy = (i % 2 == 0);
            int price = isBuy ? (priceDist(gen.rng) % 50 + 1) : (priceDist(gen.rng) % 50 + 51);
            orders.emplace_back(isBuy ? Side::Buy : Side::Sell, Type::Limit,
                                 price, quantityDist(gen.rng), gen.nextId++, 0);
        }
        for (int i = 0; i < warmup; ++i) { Order o = orders[i]; book.submit(o); }

        auto start = std::chrono::steady_clock::now();
        for (int i = warmup; i < iterations; ++i) { Order o = orders[i]; book.submit(o); }
        auto end = std::chrono::steady_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        results.push_back(static_cast<double>(ns) / (iterations - warmup));
    }
    report("SUBMIT — resting only (no crossing, insertion cost)", results);
}

// ---- 2. Submit, always crosses (full match-loop cost) ----
// Pre-seed one resting order the aggressor will always match against.
void benchmarkSubmitCrossing(generator& gen, int iterations, int warmup = 1000, int trials = 5) {
    std::vector<double> results;

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        // Deep resting liquidity on the sell side at price 50, so every buy aggressor at 50 matches immediately.
        for (int i = 0; i < iterations + warmup; ++i) {
            Order resting{Side::Sell, Type::Limit, 50, 1, gen.nextId++, 0};
            book.rest(resting); // rest() directly — no matching, pure seed, doesn't pollute the timed cost
        }

        auto runBatch = [&](int n) {
            for (int i = 0; i < n; ++i) {
                Order aggressor{Side::Buy, Type::Limit, 50, 1, gen.nextId++, 0};
                book.submit(aggressor); // always crosses exactly one resting unit
            }
        };
        runBatch(warmup);

        auto start = std::chrono::steady_clock::now();
        runBatch(iterations);
        auto end = std::chrono::steady_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        results.push_back(static_cast<double>(ns) / iterations);
    }
    report("SUBMIT — always crosses (full match-loop cost)", results);
}

// ---- 3. Cancel ----
void benchmarkCancel(generator& gen, int iterations, int trials = 5) {
    std::vector<double> results;

    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        ids.reserve(iterations);
        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, static_cast<Price>((i % 90) + 1), 1, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        auto start = std::chrono::steady_clock::now();
        for (Id id : ids) book.cancel(id);
        auto end = std::chrono::steady_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        results.push_back(static_cast<double>(ns) / iterations);
    }
    report("CANCEL", results);
}

// ---- 4. Modify — the two paths cost very differently ----
void benchmarkModifyInPlace(generator& gen, int iterations, int trials = 5) {
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        for (int i = 0; i < iterations; ++i) {
            Price p = static_cast<Price>((i % 90) + 1);
            Order o{Side::Buy, Type::Limit, p, 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }

        int64_t checksum = 0;

        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < ids.size(); ++i) {
            book.modify(ids[i], std::nullopt, 50);
            checksum += book.getOrderInfo(ids[i])->quantity; // O(1) — reads the ONE mutated order, no level scan
        }
        auto end = std::chrono::steady_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        results.push_back(static_cast<double>(ns) / iterations);

        std::cout << "  (trial " << (t + 1) << " checksum: " << checksum << ")\n";
    }
    report("MODIFY — in-place (quantity decrease)", results);
}

void benchmarkModifyCancelResubmit(generator& gen, int iterations, int trials = 5) {
    // Price-change: cancel + resubmit, the expensive path.
    std::vector<double> results;
    for (int t = 0; t < trials; ++t) {
        OrderBook book;
        std::vector<Id> ids;
        for (int i = 0; i < iterations; ++i) {
            Order o{Side::Buy, Type::Limit, static_cast<Price>((i % 90) + 1), 100, gen.nextId++, 0};
            book.rest(o);
            ids.push_back(o.id);
        }
        auto start = std::chrono::steady_clock::now();
        for (Id id : ids) book.modify(id, 5, std::nullopt); // price-change — cancel + resubmit
        auto end = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        results.push_back(static_cast<double>(ns) / iterations);
    }
    report("MODIFY — cancel+resubmit (price change)", results);
}

// ---- Run everything ----
void runTrueBenchmark(generator& gen, int iterations = 100000) {
    std::cout << "=== Matching Engine Benchmark Suite (submit/cancel/modify isolated, -O3) ===\n";
    benchmarkSubmitResting(gen, iterations);
    benchmarkSubmitCrossing(gen, iterations);
    benchmarkCancel(gen, iterations);
    benchmarkModifyInPlace(gen, iterations);
    benchmarkModifyCancelResubmit(gen, iterations);
    std::cout << "\nNOTE: single-threaded, fixed-size books, no memory-pressure/long-run effects,\n"
              << "      no book-size sweep. This is an informal W9 baseline, not the full W9 suite.\n";
}*/

// concurrentBench.cpp
//
// One sustained run of N operations through the full queued path:
//   producers -> RingBuffer -> single writer -> OrderBook
//
// Nothing in orderBook.hpp or tests.cpp is modified. This file only uses the
// public surface: producer::nextId, producerOf, RingBuffer::push/waitAndPop/
// shutdown, and submit/cancel/modify.
//
// Build: c++ -std=c++23 -O3 concurrentBench.cpp -o cbench
//        ./cbench            (default 1,000,000 ops)
//        ./cbench 100000 4   (ops, producers)
//
// This does NOT replace the single-threaded suite. That one measures matching
// cost per operation. This measures matching cost plus hand-off, lock
// contention, and scheduling — a different number answering a different
// question. Keep both.

#include "orderBook.hpp"

#include <thread>
#include <atomic>
#include <deque>
#include <random>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace cbench {

using Clock = std::chrono::steady_clock;

inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Writer loop — a deliberate copy of writerLoop with the per-request
// std::println removed. That println is a syscall on the hot path; leaving it
// in would make this benchmark a measurement of stdout. If a fourth OpType is
// ever added, this switch needs the same case as the real one.
//
// Three strategies, selectable, so you can see what the park/wake actually
// costs without touching the header:
//
//   Park        - what you have now. waitAndPop per item; the writer sleeps
//                 whenever the queue empties, so at low load nearly every
//                 push calls notify_one on a PARKED thread = kernel round trip.
//
//   Drain       - after one blocking wake, keep calling pop() until the queue
//                 is empty, then park again. Parks once per batch instead of
//                 once per item.
//
//   SpinThenPark- try pop() in a bounded spin before parking. The writer stays
//                 running, so producers' notify_one finds no waiter to wake,
//                 which is cheap. Costs a core spinning.
//
// NONE of these remove the per-item lock acquisition — pop() still takes the
// mutex every time. The real fix is a drain-under-one-lock in the header plus
// notifying only on the empty -> non-empty edge. This isolates the wake cost
// only, which is the larger of the two at low load.
// ---------------------------------------------------------------------------

enum class WriterMode { Park, Drain, SpinThenPark };

const char* name(WriterMode m) {
    switch (m) {
        case WriterMode::Park:  return "park-per-item";
        case WriterMode::Drain: return "drain-batch";
        default:                return "spin-then-park";
    }
}

void benchWriterLoop(RingBuffer& queue,
                     OrderBook& book,
                     std::vector<std::vector<int64_t>>& dispatchTs,
                     std::atomic<int64_t>& dispatched,
                     WriterMode mode,
                     int spinLimit,
                     std::atomic<int64_t>& parkCount,
                     std::atomic<int64_t>& parkNs)
{
    // Park detection without extra lock traffic: time each waitAndPop. If the
    // queue had work, the call is just lock + pop + unlock, well under a
    // microsecond. If the writer actually blocked on the condition variable
    // it paid a kernel round trip and the call takes microseconds. The
    // threshold sits in the gap between those two populations.
    //
    // This is inference from duration, not a direct observation of blocking —
    // a lock acquisition that loses a race could in principle cross the
    // threshold. Cross-check against parksPerOp: if it lands near 1.0 at low
    // rates and near 0 at high ones, the split is clean.
    constexpr int64_t parkThresholdNs = 1'000;

    int64_t parks = 0, waited = 0;

    auto apply = [&](Request& r) {
        if (r.requestType == OpType::Submit) {
            book.submit(r.order);
        } else if (r.requestType == OpType::Modify) {
            book.modify(r.id, r.newPrice, r.newQuantity);
        } else {
            book.cancel(r.id);
        }
        int p = producerOf(r.id);
        dispatchTs[static_cast<size_t>(p)].push_back(nowNs());
        dispatched.fetch_add(1, std::memory_order_relaxed);
    };

    while (true) {
        if (mode == WriterMode::SpinThenPark) {
            bool got = false;
            for (int s = 0; s < spinLimit; ++s) {
                if (auto r = queue.pop()) { apply(*r); got = true; break; }
            }
            if (got) continue;
        }

        const int64_t waitStart = nowNs();
        auto request = queue.waitAndPop();
        const int64_t elapsed = nowNs() - waitStart;
        if (elapsed > parkThresholdNs) { ++parks; waited += elapsed; }

        if (!request) break;
        apply(*request);

        if (mode == WriterMode::Drain) {
            while (auto more = queue.pop()) apply(*more);
        }
    }

    parkCount.store(parks, std::memory_order_relaxed);
    parkNs.store(waited, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Percentiles
// ---------------------------------------------------------------------------

struct Stats {
    size_t n = 0;
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

Stats percentiles(std::vector<int64_t> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        return static_cast<double>(v[static_cast<size_t>(q * (v.size() - 1))]);
    };
    s.n = v.size();
    s.p50 = at(0.50); s.p90 = at(0.90); s.p99 = at(0.99); s.p999 = at(0.999);
    s.max = static_cast<double>(v.back());
    return s;
}

// ---------------------------------------------------------------------------
// Workload — built entirely before the timed region, so rng and allocation
// cost are not part of what gets measured.
//
// 80% submits, 10% cancels, 10% quantity modifies, targets drawn from a
// rolling window of recently issued ids so they stay live rather than
// hitting orders that filled ages ago. Prices cluster tight enough that a
// meaningful fraction crosses.
// ---------------------------------------------------------------------------

std::vector<Request>
buildWorkload(producer& prod, int n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> qtyDist(1, 100);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> opDist(0, 99);
    std::uniform_int_distribution<int> priceDist(45, 55);

    std::vector<Request> out;
    out.reserve(static_cast<size_t>(n));
    std::deque<Id> window;

    for (int i = 0; i < n; ++i) {
        int roll = opDist(rng);

        if (roll >= 80 && !window.empty()) {
            std::uniform_int_distribution<size_t> pick(0, window.size() - 1);
            Request r{};
            r.id = window[pick(rng)];
            if (roll < 90) {
                r.requestType = OpType::Cancel;
            } else {
                r.requestType = OpType::Modify;
                r.newQuantity = qtyDist(rng);
            }
            out.push_back(r);
            continue;
        }

        Id id = prod.nextId();
        bool isBuy = (sideDist(rng) == 0);

        Request r{};
        r.requestType = OpType::Submit;
        r.order = Order{isBuy ? Side::Buy : Side::Sell, Type::Limit,
                        priceDist(rng), qtyDist(rng), id, 0};
        r.id = id;
        out.push_back(r);

        window.push_back(id);
        if (window.size() > 1024) window.pop_front();
    }
    return out;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

struct Config {
    int64_t totalOps      = 1'000'000;
    int     producers     = 4;
    size_t  queueCapacity = 65'536;
    int     warmupPerProducer = 5'000;
    double  paceRatePerProducer = 0;   // 0 = saturation. >0 enables latency.
    bool    retryOnReject = false;     // closed loop: never lose an op
    WriterMode writerMode = WriterMode::Park;
    int     spinLimit     = 200;       // SpinThenPark only
    uint32_t seed = 1234;
};

struct Summary {
    double aggregateRate = 0;
    double nsPushAccepted = 0;
    double contendedTput = 0;
    double parksPerOp = 0;
    double nsPerPark = 0;
    double rejPct = 0;
    Stats  lat;
    bool   invariantsOk = false;
};

Summary run(const Config& cfg) {
    const int P = cfg.producers;
    const int N = static_cast<int>(cfg.totalOps / P);
    const int64_t attempted = static_cast<int64_t>(P) * N;

    std::printf("-----------------------------------------------------------\n");
    std::printf("Sustained run: %lld ops | %d producers x %d | queue %zu | %s | writer %s\n",
                (long long)attempted, P, N, cfg.queueCapacity,
                cfg.paceRatePerProducer > 0 ? "paced" : "saturation",
                name(cfg.writerMode));

    OrderBook book;
    RingBuffer queue(cfg.queueCapacity);

    // Pre-build all work. Producer ids are disjoint by construction, so no
    // id collides across threads and validate()'s duplicate check never fires.
    std::vector<std::vector<Request>> work(static_cast<size_t>(P));
    for (int p = 0; p < P; ++p) {
        producer prod{p, 0};
        work[static_cast<size_t>(p)] = buildWorkload(prod, N, cfg.seed + p);
    }

    // Latency side-channel, matched by ordinal rather than by id.
    // A producer's pushes are FIFO among themselves and there is exactly one
    // writer, so the writer's k-th dispatch for producer p is producer p's
    // k-th ACCEPTED push. Rejected pushes are simply never recorded, which
    // keeps the two sequences aligned. No field added to Request.
    std::vector<std::vector<int64_t>> enqueueTs(static_cast<size_t>(P));
    std::vector<std::vector<int64_t>> dispatchTs(static_cast<size_t>(P));
    for (int p = 0; p < P; ++p) {
        enqueueTs[static_cast<size_t>(p)].reserve(static_cast<size_t>(N));
        dispatchTs[static_cast<size_t>(p)].reserve(static_cast<size_t>(N));
    }

    std::atomic<int64_t> dispatched{0}, accepted{0}, rejected{0};
    std::atomic<int64_t> pushAcceptedNs{0}, pushRejectedNs{0};

    std::atomic<int64_t> parkCount{0}, parkNs{0};
    std::thread writer(benchWriterLoop, std::ref(queue), std::ref(book),
                       std::ref(dispatchTs), std::ref(dispatched),
                       cfg.writerMode, cfg.spinLimit,
                       std::ref(parkCount), std::ref(parkNs));

    // Start gate, 20 ms out, so every producer thread exists before the clock
    // opens. Without it the first thread spawned runs against an already
    // elapsed schedule and reports a tail that isn't real.
    const int64_t t0 = nowNs() + 20'000'000;
    const int64_t periodNs = cfg.paceRatePerProducer > 0
        ? static_cast<int64_t>(1e9 / cfg.paceRatePerProducer) : 0;

    std::vector<std::thread> prods;
    prods.reserve(static_cast<size_t>(P));

    for (int p = 0; p < P; ++p) {
        prods.emplace_back([&, p] {
            auto& mine = work[static_cast<size_t>(p)];
            auto& enq  = enqueueTs[static_cast<size_t>(p)];
            int64_t acc = 0, rej = 0, busyAcc = 0, busyRej = 0;

            while (nowNs() < t0) { }          // line up on the gate

            for (int i = 0; i < N; ++i) {
                int64_t stamp;
                if (periodNs > 0) {
                    // Open loop. The sample is timed from the INTENDED send
                    // time, not from when this thread got around to pushing,
                    // so a stall lands in the tail instead of disappearing
                    // from it. That is the coordinated-omission fix.
                    stamp = t0 + static_cast<int64_t>(i) * periodNs;
                    while (nowNs() < stamp) { }
                } else {
                    stamp = nowNs();
                }

                // Time the push and nothing else. Wrapping the whole loop
                // iteration would fold the pacing busy-wait into the figure
                // and report the pacing period back as a cost.
                //
                // Accepted and rejected pushes are separated because they are
                // different code paths: a rejection takes the lock, sees
                // isFull(), and returns without copying the ~48-byte Request
                // or calling notify_one. Averaging them together reports
                // whatever the rejection rate happened to be that run.
                const int64_t pushStart = nowNs();
                bool ok = queue.push(mine[static_cast<size_t>(i)]);
                if (!ok && cfg.retryOnReject) {
                    // Closed loop. The producer is now throttled by the writer
                    // rather than by the clock, so every operation is
                    // delivered and wall time measures real capacity. This is
                    // also textbook coordinated omission — a producer stuck
                    // retrying is not timing the requests piling up behind it
                    // — which is exactly why latency is not reported here.
                    do {
                        ++rej;
                        ok = queue.push(mine[static_cast<size_t>(i)]);
                    } while (!ok);
                }
                const int64_t elapsed = nowNs() - pushStart;

                if (ok) {
                    busyAcc += elapsed;
                    enq.push_back(stamp);
                    ++acc;
                } else {
                    busyRej += elapsed;
                    ++rej;
                }
            }

            pushAcceptedNs.fetch_add(busyAcc, std::memory_order_relaxed);
            pushRejectedNs.fetch_add(busyRej, std::memory_order_relaxed);
            accepted.fetch_add(acc, std::memory_order_relaxed);
            rejected.fetch_add(rej, std::memory_order_relaxed);
        });
    }

    for (auto& t : prods) t.join();

    // Boundary between the two regimes. Everything dispatched after this
    // point had no producer competing for the lock, so averaging across it
    // describes neither phase.
    const int64_t dispatchedAtJoin = dispatched.load();
    const int64_t joinTs = nowNs();

    // Let the writer finish everything that got in, then stop it.
    while (dispatched.load(std::memory_order_relaxed)
           < accepted.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    queue.shutdown();
    writer.join();

    // -----------------------------------------------------------------------
    // Results. Wall time ends at the LAST dispatch, not at the drain spin
    // above, which would fold its own overshoot into the number.
    // -----------------------------------------------------------------------

    const int64_t acc = accepted.load();
    const int64_t rej = rejected.load();
    const int64_t dis = dispatched.load();

    int64_t lastDispatch = t0;
    std::vector<int64_t> lat;
    lat.reserve(static_cast<size_t>(acc));

    for (int p = 0; p < P; ++p) {
        const auto& e = enqueueTs[static_cast<size_t>(p)];
        const auto& d = dispatchTs[static_cast<size_t>(p)];
        const size_t k = std::min(e.size(), d.size());
        if (k) lastDispatch = std::max(lastDispatch, d[k - 1]);
        for (size_t i = static_cast<size_t>(cfg.warmupPerProducer); i < k; ++i)
            lat.push_back(d[i] - e[i]);      // warm-up discarded
    }

    const double wallNs = static_cast<double>(lastDispatch - t0);
    const double throughput = dis ? static_cast<double>(dis) * 1e9 / wallNs : 0;
    const double nsPerOp    = dis ? wallNs / static_cast<double>(dis) : 0;
    const double nsPushAcc  = acc ? static_cast<double>(pushAcceptedNs.load())
                                  / static_cast<double>(acc) : 0;
    const double nsPushRej  = rej ? static_cast<double>(pushRejectedNs.load())
                                  / static_cast<double>(rej) : 0;
    const double rejPct     = 100.0 * static_cast<double>(rej)
                            / static_cast<double>(attempted);

    std::printf("  wall         : %.3f s\n", wallNs / 1e9);
    std::printf("  accepted %lld | rejected %lld (%.3f%%) | dispatched %lld\n",
                (long long)acc, (long long)rej, rejPct, (long long)dis);
    const double contendedNs = static_cast<double>(joinTs - t0);
    const double contendedTput = dispatchedAtJoin
        ? static_cast<double>(dispatchedAtJoin) * 1e9 / contendedNs : 0;
    const int64_t drained = dis - dispatchedAtJoin;

    std::printf("  contended    : %.0f ops/sec over %.1f ms  (%lld dispatched while producers live)\n",
                contendedTput, contendedNs / 1e6, (long long)dispatchedAtJoin);
    std::printf("  drain after  : %lld ops with no producer contention\n",
                (long long)drained);
    std::printf("  blended      : %.0f ops/sec  (%.1f ns/op — spans both regimes, use with care)\n",
                throughput, nsPerOp);
    const double parksPerOp = dis ? static_cast<double>(parkCount.load())
                                  / static_cast<double>(dis) : 0;
    const double nsPerPark  = parkCount.load()
        ? static_cast<double>(parkNs.load()) / static_cast<double>(parkCount.load()) : 0;

    std::printf("  writer parks : %.3f per dispatched op  (%lld total, %.0f ns each)\n",
                parksPerOp, (long long)parkCount.load(), nsPerPark);

    std::printf("  push (acc)   : %.1f ns  (lock + copy Request + notify_one)\n",
                nsPushAcc);
    if (rej)
        std::printf("  push (rej)   : %.1f ns  (lock + isFull early-out, no copy, no notify)\n",
                    nsPushRej);

    Stats latStats;
    if (periodNs > 0) {
        latStats = percentiles(std::move(lat));
        const Stats& s = latStats;
        std::printf("  end-to-end   : p50 %.0f | p90 %.0f | p99 %.0f | p99.9 %.0f | max %.0f ns  (n=%zu)\n",
                    s.p50, s.p90, s.p99, s.p999, s.max, s.n);
        std::printf("                 enqueue -> dispatch complete, from intended send time\n");
        if (rejPct > 0.5)
            std::printf("  *** %.2f%% rejected and therefore unsampled. These percentiles\n"
                        "      describe accepted requests only and understate the tail.\n", rejPct);
    } else {
        std::printf("  latency      : not reported. In saturation the refused requests have\n"
                    "                 no sample, so any tail printed here would be the tail\n"
                    "                 of whatever got lucky. Use a pace rate for percentiles.\n");
    }

    // -----------------------------------------------------------------------
    // The run is also a correctness check. A million operations through the
    // queue that leave the book violating an invariant is a concurrency bug,
    // and it costs nothing to look.
    // -----------------------------------------------------------------------
    bool fifoOk = book.checkFIFO();
    auto orphans = book.checkNoOrphans();
    std::printf("  invariants   : FIFO %s | orphans %s\n",
                fifoOk ? "ok" : "VIOLATED",
                orphans ? "VIOLATED" : "ok");

    Summary out;
    out.aggregateRate = cfg.paceRatePerProducer * P;
    out.nsPushAccepted = nsPushAcc;
    out.contendedTput = contendedTput;
    out.parksPerOp = parksPerOp;
    out.nsPerPark  = nsPerPark;
    out.rejPct = rejPct;
    out.lat = latStats;
    out.invariantsOk = fifoOk && !orphans;
    return out;
}

// ---------------------------------------------------------------------------
// Rate ladder.
//
// This is the experiment that names the bottleneck, and it works because the
// two candidate mechanisms move in OPPOSITE directions as offered load rises:
//
//   Lock contention  -> accepted-push cost RISES. More producers arriving more
//                       often means more time queued behind the mutex.
//
//   Writer park/wake -> accepted-push cost FALLS. At low rates the queue is
//                       near-empty, so the writer sleeps between almost every
//                       item and each push pays a notify_one that wakes a
//                       parked thread. Raise the rate, the writer stops
//                       sleeping, and the wake cost disappears.
//
// One is a queueing problem, one is a syscall problem, and they want different
// fixes. Read the direction of the curve, not the magnitude of any one point.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Reference number: what the writer can do with nobody competing.
//
// Single thread. Fill the queue, then drain it inline — no producer thread,
// no contention, no condition variable, no wake. This is pop + dispatch +
// matching and nothing else, and it is the ceiling every concurrent number
// below should be read against. If the concurrent throughput is far under
// this, the gap is the cost of the hand-off, not the cost of matching.
// ---------------------------------------------------------------------------

void writerCeiling(int64_t totalOps, size_t queueCapacity) {
    OrderBook book;
    RingBuffer queue(queueCapacity);
    producer prod{0, 0};

    auto work = buildWorkload(prod, static_cast<int>(totalOps), 99);

    int64_t done = 0, drainNs = 0;
    size_t cursor = 0;

    while (cursor < work.size()) {
        // Fill (untimed — this is producer work, not writer work).
        while (cursor < work.size() && queue.push(work[cursor])) ++cursor;

        // Drain (timed).
        const int64_t t = nowNs();
        while (auto r = queue.pop()) {
            auto& q = *r;
            if      (q.requestType == OpType::Submit) book.submit(q.order);
            else if (q.requestType == OpType::Modify) book.modify(q.id, q.newPrice, q.newQuantity);
            else                                      book.cancel(q.id);
            ++done;
        }
        drainNs += nowNs() - t;
    }

    std::printf("\n=== Writer ceiling — single thread, zero contention ===\n");
    std::printf("  %lld ops in %.3f s -> %.0f ops/sec  (%.1f ns/op)\n",
                (long long)done, drainNs / 1e9,
                static_cast<double>(done) * 1e9 / static_cast<double>(drainNs),
                static_cast<double>(drainNs) / static_cast<double>(done));
    std::printf("  pop + dispatch + match. No thread, no cv, no wake, no lock contention.\n");
    std::printf("  invariants   : FIFO %s | orphans %s\n",
                book.checkFIFO() ? "ok" : "VIOLATED",
                book.checkNoOrphans() ? "VIOLATED" : "ok");
}

// ---------------------------------------------------------------------------
// Closed-loop throughput. Producers retry until accepted, so all ops are
// delivered and wall time measures capacity rather than the pacer's setting.
// Latency is deliberately not reported: a retrying producer is not timing
// what piles up behind it.
// ---------------------------------------------------------------------------

void throughputSweep(int64_t totalOps, std::initializer_list<int> producerCounts) {
    std::printf("\n\n=== Closed-loop throughput — every op delivered ===\n");

    std::vector<std::pair<int, Summary>> rows;
    for (int p : producerCounts) {
        Config c;
        c.totalOps      = totalOps;
        c.producers     = p;
        c.retryOnReject = true;
        c.paceRatePerProducer = 0;
        std::printf("\n");
        rows.emplace_back(p, run(c));
    }

    std::printf("\n  %-12s %-16s %-12s %-10s\n",
                "producers", "throughput/s", "push(acc)ns", "parks/op");
    std::printf("  ------------------------------------------------------\n");
    for (const auto& [p, r] : rows)
        std::printf("  %-12d %-16.0f %-12.1f %-10.3f\n",
                    p, r.contendedTput, r.nsPushAccepted, r.parksPerOp);

    std::printf("\n  Throughput rising with producers -> the writer had headroom.\n"
                "  Throughput flat  -> the writer is the bottleneck, as designed.\n"
                "  Throughput FALLING -> contention is costing more than the extra\n"
                "                        producers deliver. That is the finding.\n");
}

void rateLadder(int64_t totalOps, int producers,
                std::initializer_list<double> aggregateRates) {
    std::printf("\n\n=== Rate ladder — %d producer(s) ===\n", producers);

    std::vector<Summary> rows;
    for (double aggregate : aggregateRates) {
        Config c;
        c.totalOps            = totalOps;
        c.producers           = producers;
        c.paceRatePerProducer = aggregate / producers;
        std::printf("\n");
        rows.push_back(run(c));
    }

    std::printf("\n  %-12s %-10s %-10s %-12s %-9s %-9s %-10s\n",
                "offered/s", "parks/op", "ns/park", "push(acc)ns", "p50 ns", "p99 ns", "p99.9 ns");
    std::printf("  --------------------------------------------------------------------------------\n");
    for (const auto& r : rows) {
        std::printf("  %-12.0f %-10.3f %-10.0f %-12.1f %-9.0f %-9.0f %-10.0f\n",
                    r.aggregateRate, r.parksPerOp, r.nsPerPark, r.nsPushAccepted,
                    r.lat.p50, r.lat.p99, r.lat.p999);
    }
    std::printf("\n  parks/op near 1.0  -> the writer sleeps between almost every request,\n"
                "                        so each push wakes a parked thread. Wake theory holds.\n"
                "  parks/op near 0.0 but push cost still high -> not the wake. Look at cache:\n"
                "                        at low rates the mutex and ring buffer lines go cold\n"
                "                        between a producer's pushes and every one takes a miss.\n");
}

} // namespace cbench

int main(int argc, char** argv) {
    cbench::Config cfg;
    if (argc > 1) cfg.totalOps  = std::atoll(argv[1]);
    if (argc > 2) cfg.producers = std::atoi(argv[2]);

    // 1. The reference: uncontended, single-threaded writer ceiling.
    cbench::writerCeiling(cfg.totalOps, cfg.queueCapacity);

    // 2. The throughput number: closed loop, every op delivered, swept
    //    across producer counts.
    cbench::throughputSweep(cfg.totalOps, {1, 2, 4, 8});

    return 0;
}