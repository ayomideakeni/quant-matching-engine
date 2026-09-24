# Dev Log — Matching Engine

Session-by-session record. Short on purpose: what was done, what it showed, what it taught. Newest first.

---

## Status

| Area | State |
|---|---|
| Core engine (submit, cancel, modify, validation, match loop) | ✅ done |
| Testing — 23 replay tests, 100k fuzz / 4 invariants, shrinker | ✅ done |
| Concurrency — single-writer MPSC queue, producer ids, writer loop | ✅ done |
| Concurrent verification — 1.6M ops fuzzed, determinism, TSan + ASan | ✅ done |
| Benchmarking — percentile harness, depth sweep, profiling | ✅ done |
| Optimisation — pool + intrusive list (7.4× at depth) | ✅ done |
| Instrument rebuild + bounded/paced queue | ✅ done |
| Cancel index → `boost::unordered_flat_map` | ✅ adopted |
| Price levels → `absl::btree_map` (`flat_map` rejected) | ✅ adopted |
| `alignas(64)` on `Order` | ⏸ parked — below instrument resolution |
| Lock-free queue | ⏸ on hold — compared on the old instrument |
| Direct-indexed price levels + bitmask | ⬜ in progress (branch) |

**Current:** 2 producers × 800k, cap 64, capacity 4,096 → p50 52.7–54.7 ns, p99.9 240.9–285.8 ns, 15–16M ops/s. Resting submit flat 8.3 ns to 100,000 levels.

---

## Sessions

### README rewrite
- Cut to about a third of its length. Kept every claim the CV relies on; removed the narration, the full bug write-ups, and per-decision rejection tables.
- The long-form reasoning now lives here instead.

### Price-level containers
- **`boost::flat_map`** (sorted vector): resting submit dropped to 4.2 ns flat, but erase shifts the whole array — cancel p99 went to 620.8 ns at 10,000 levels. **Rejected.**
- **`absl::btree_map`** (several keys per node): resting submit flat at 8.3 ns to 100,000 levels, cancel better than `std::map` at every depth, submit-crossing near-flat. **Adopted.**
- A concurrent run showed a ~15× regression. Tested allocator contention by dropping to one producer — it got *worse*, which ruled that theory out. Real cause: the build script was missing `-O3`. Heavily templated code suffers most without inlining.
- Lesson: when a result contradicts the isolated measurement, check the build before theorising.

### Cancel index → open addressing
- `std::unordered_map` chains each bucket to separately allocated nodes — a cache miss per lookup regardless of load. Swapped to `boost::unordered_flat_map`, sized from the pool capacity.
- Cancel ~40% faster at every depth — a constant cost removed, not the depth scaling (growth ratio unchanged).
- Concurrent: p50 −6 ns, retries −15–20%. Bigger than dilution predicts, because a faster writer drains the backlog sooner.
- Found along the way: one `generator` shared across the whole depth sweep shifted inputs between benchmarks. Fixed with a fresh generator per call.

### Four hypotheses
- **Atomics on `head`/`tail`/`count`** were redundant — the mutex already protects them. Removed: p50 78.8–80.1 → 74.9–76.8 ns, clean separation over 8 runs.
- **`alignas(64)` on `Order`** (56 bytes, so most slots straddle two cache lines): p50/p99 bit-identical across 7 runs. The batch-of-10 harness can't resolve an effect that small. Parked, not disproved.
- **Free-list fragmentation:** 500k ops at 45% cancel, 10 checkpoints — flat after warm-up. The pool's contiguity absorbs the scatter.
- **`notify_one` outside the lock:** no measurable change — the writer is almost never waiting. Kept as correct practice.
- **Copy under the lock:** no cheap fix without either shrinking `Request` or changing the publication protocol.
- Together: the critical section's *contents* are cheap; acquiring the lock is the cost.

### Correctness sweep, instrument rebuild, saturation
- **Multi-TU fixes:** ODR violations (`spinCount` removed, `producerOf` made `inline`), `memoryPool` copy assignment deleted, `level` pointers default-initialised, missing `<chrono>`, `ssize_t` → `size_t`, `quantityAt`'s double traversal, const-correctness.
- **Instrument defects:** sample vectors under-reserved (reallocating mid-run), `computeStats` sorting the caller's data in place, unweighted mean, batch-indexed percentiles. Replaced with one `BenchSample` per batch, a weighted mean, and operation-weighted percentiles.
- **Saturation:** every drain returned a full batch at every producer count, including one. The writer is slower than a single producer. The benchmark had sized the queue so it could never fill, so reject-on-full had never run.
- **Bounded + paced:** fixed capacity, producers spin then `yield()`. 2 producers: p50 47 → 80 ns, p99.9 745 → 278 ns. 8 producers: tail gain shrinks, because `yield()` doesn't target the writer.
- **Warm-up sweep:** no effect at 2+ producers — contention dwarfs cold-start.

### Lock-free queue (prototype)
- Built as a drop-in: per-slot sequence numbers, CAS on the tail, release/acquire publication. TSan clean.
- Measured on the old instrument, which was later found to be flawed. **On hold**, not rejected — needs re-running on the corrected harness.

### Concurrent tail diagnosis
- Hypothesis: condvar wake-up latency.
- Spin-before-sleep: no change.
- Partitioning samples by whether the drain slept: the writer almost never sleeps.
- Timing lock acquisition directly: bimodal — usually instant, occasionally ~100 µs. The tail is lock acquisition.
- Run on the old instrument, so the exact figures aren't trustworthy; the saturation finding later confirmed the cause.

### Optimisation — pool + intrusive list
- `std::list` allocated a node per insert. Replaced with an intrusive list (links on `Order`) over a fixed-capacity pool with a free list threaded through the same `next` field.
- Pool: contiguous storage, no hot-path allocation, fail on exhaustion rather than grow.
- `std::hive` rejected: no ordering guarantee.
- Resting submit: 20.8–62.5 ns depth-dependent → 8.3 ns flat, p99 8.4.

### Profiling
- Two hypotheses registered first: allocation (submit-resting's 10.4 µs max vs 191 ns for crossing) and tree traversal (the depth curve).
- `sample` on macOS. Two methodology fixes: the first run was mostly process startup; `grep -c` counts lines, not sample weight.
- Allocation outweighed tree work ~50:1 by sample weight. The depth curve was cache misses from separately allocated nodes, not tree logic.
- Lock contention: 29–43% of writer time depending on producer count.

### Measuring the concurrent path + batched drain
- Queue overhead ~8 ns/op over direct calls (p50 50 vs 41.7).
- Batched drains: p50 unchanged, p99.9 −20%, max −40%. Negative on the main claim — the overhead isn't per-acquisition cost.
- Drain-cap sweep looked like a 7× tail win; sample count fell in proportion to cap. Dilution, not improvement. Discarded.
- Caught a fuzz test running with a null context and checking nothing; determinism was the test that noticed.

### Percentile harness + depth sweep
- `steady_clock::now()` costs 16.8 ns per call, so timing is batched and reported as percentiles of batch means.
- Coordinated omission considered and ruled out — the harness is closed-loop.
- Depth sweep 10 → 10,000 levels: map-touching operations grew 3–4×, tails proportionate.
- Cold-start follows position in the process, not depth — fixed with a process-level warm-up.

### Concurrency hardening
- Adversarial tests: cancel racing a match, producers outrunning the consumer.
- TSan clean across the suite, including deliberately raced tests.
- Contention sweep at constant total work (1/4/16 producers): more producers, worse worst case. 16 threads on ~10 cores oversubscribes.
- A TSan run observed only one test because most were disabled — re-run properly.

### Fuzzing through the queue + determinism
- 99,000 queued ops, zero violations — but the first "clean" run was 100% submits: a bootstrap check read a structure the new generator no longer filled. Caught by counting cancels in the output.
- Determinism: replaying 1M captured ops single-threaded gives an identical book, per order and in queue position. Proven by injecting a skip and seeing it localised.
- Shrinker through the queue: an injected crossing bug reduced from 19 ops to 2.
- `invReplay` made pure (it was mutating the orders it replayed).

### MPSC queue + single-writer loop
- `producer` with bit-packed ids (1 sign / 7 producer / 56 counter); modular partitioning rejected because the producer count gets baked into every id.
- `RingBuffer`: bounded, mutex + condvar, `count` to tell full from empty, power-of-two capacity with a mask.
- Condvar predicate form (`count > 0 || stopping`); `push` must never wait.
- Two-phase shutdown: drain, don't discard — accepted means accepted.

### Queue design
- `Request` kept separate from `LoggedOp` — a production message vs a test record.
- Reject-on-full chosen over blocking and over dropping old entries.
- Response path (per-producer queues, routed by id bits) designed and deferred.

### Concurrency model, primitives, drills
- Derived single-writer from the fairness rule. Rejected global lock, lock-per-level, lock-free.
- Reproduced a data race and a deadlock, each fixed two ways.

### Core build, testing, shrinker
- Order type, book, match loop, submit, cancel, modify, validation, map unification.
- 23 replay tests; caught the sell-side crossing bug that equal-price tests couldn't see.
- 100k-op fuzz against four invariants; shrinker validated on an injected bug, 34 → 20 ops.
