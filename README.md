# Matching Engine

A limit order book matching engine in C++23: price-time priority, a single-writer concurrency layer, and a test suite built to show correctness rather than assume it.

**Status:** feature-complete. Correct single-threaded and concurrent, clean under ThreadSanitizer and AddressSanitizer, benchmarked and profiled.

---

## Current numbers

Single-threaded: resting submit is **8.3 ns**, flat across every depth tested from 10 to 100,000 price levels.

Concurrent — 2 producers × 800,000 ops, drain cap 64, queue capacity 4,096, `-O3 -flto`, Apple M4, five runs:

| | range |
|---|---|
| p50 | 52.7–54.7 ns |
| mean | 62.1–65.9 ns |
| p99 | 173.8–196.0 ns |
| p99.9 | 240.9–285.8 ns |
| throughput | 15.1–16.0 M ops/s |

All percentiles are **percentiles of batch means**, not individual operations (see *Measurement*).

---

## What it does

A book of resting buy and sell orders. Incoming orders match against the opposite side under price-time priority — best price first, earliest arrival first within a price — and any unfilled limit remainder rests. Supports limit and market submit, cancel, and modify, with partial fills sweeping across levels.

---

## Design

**Order.** `id` identifies an order; `seq` is an engine-stamped arrival counter used only for ordering. Kept separate so a caller-supplied value can never affect priority. A system clock was rejected because it guarantees neither uniqueness nor monotonicity.

**Integer ticks.** Prices are integers. Matching depends on price *equality* at the spread, and floating point can't guarantee two equal prices compare equal.

**Two books.** `bids` and `asks` are separate ordered maps, so the best price on each side is just the first or last entry — no filtering.

**Price levels.** Each level is an intrusive doubly linked list: the `next`/`prev` links live on the order itself, and orders come from a fixed-capacity pool allocated up front. This replaced `std::list`, which allocated a node on every insert. `std::hive` was considered and rejected because it gives no ordering guarantee, and a level's order *is* its priority.

**Cancel index.** Order id → pointer to the order, for O(1) cancel. Stores only the pointer so there's no cached copy that could disagree with the order after a modify.

**Match loop.** Crossing is decided by the incoming order's side: a buy crosses at or above the best ask, a sell at or below the best bid, a market order always. Trades execute at the resting order's price.

**Modify.** Reducing quantity edits in place and keeps queue position. Increasing quantity or changing price is a cancel and resubmit, losing position — both represent new commitment that didn't exist when the orders ahead of it arrived.

**Validation.** Rejects duplicate ids, non-positive quantity, and non-positive limit prices. Market orders skip the price check because their crossing decision never reads price. `submit` returns `optional<vector<Fill>>` so a rejection (`nullopt`) is distinguishable from an accepted order that produced no fills (empty vector).

---

## Concurrency

**Why one writer.** Under price-time priority, arrival order *is* the outcome. Two threads matching in parallel produce whichever result the scheduler happens to give — different on every run — so there's no single rule being enforced. Matching is inherently sequential.

**Rejected alternatives.**
- *Global lock* — correct, but matching is the entire workload, so it serialises everything anyway while adding lock cost.
- *Lock-per-level* — two orders at different levels can run in parallel with no race, but whichever thread finishes first gets filled first, so a later order can settle before an earlier one. It breaks fairness even when implemented perfectly.
- *Lock-free book* — a wrong memory ordering doesn't crash, it reads stale but plausible data, which the invariant tests can't detect.

**The design.** Producers push requests into a bounded ring buffer (mutex + condition variable); one writer thread drains it and applies them to the book. The matching code is byte-identical to its single-threaded form.

**Producer ids.** Each id packs the producer number into its high bits and a per-producer counter into the rest (1 sign / 7 producer / 56 counter bit), so producers generate unique ids with no coordination.

**Overflow.** When the queue is full, `push` returns false and the producer decides what to do. Blocking would silently change producer timing; evicting an older accepted request would let a new arrival displace someone else's order.

**Backpressure.** Producers retry with a short spin then `yield()`, which makes the bounded queue self-pacing — a producer can only push once the writer frees a slot.

**Scaling.** Across instruments, not within one: each instrument gets its own book and writer.

---

## Testing

- **23 replay tests** asserting exact fills and final book state. These caught a sell-side crossing bug invisible to equal-price tests, where both comparisons agree.
- **Property-based fuzzing:** 100,000 randomised operations checked against four invariants — no crossed book, no orphaned cancel-index entries, FIFO within each level, volume conservation — zero violations.
- **Counterexample shrinker:** removes operations one at a time while the failure still reproduces. Validated on injected bugs: 34 → 20 operations single-threaded, 19 → 2 through the queue.
- **Concurrent:** 1.6M operations across 1–16 producers with zero violations; determinism shown by replaying 1M captured operations single-threaded to an identical book, per order and in queue position.
- **Sanitizers:** clean under ThreadSanitizer and AddressSanitizer, including deliberately raced tests. TSan catches data races on the paths it executes — the one bug class the invariant checks can't see.

---

## Measurement

**Method.** `steady_clock::now()` costs about 16.8 ns per call — too close to a single operation to time individually — so operations are timed in batches and reported as percentiles of batch means. Fresh book per trial, warm-up discarded.

**Profiling.** The depth sweep showed resting submit growing with book depth, which looked like tree traversal. A sampling profile showed allocation outweighing tree work about 50:1 by sample weight. Replacing `std::list` with the pool-backed intrusive list took resting submit from 20.8–62.5 ns to a flat 8.3 ns.

**The instrument had to be rebuilt.** The concurrent benchmark under-reserved its sample vectors (reallocating mid-run), sorted them in place, and computed unweighted statistics that overweighted small batches. Replaced with one record per batch, reserved once, a weighted mean, and operation-weighted percentiles. Everything below uses the corrected version.

**What it found.** Every drain returned a full batch at every producer count, including one — the writer is slower than a single producer, and the queue was sized so it could never fill. Bounding it and adding retry/yield changed the shape:

| | unbounded | bounded + paced |
|---|---|---|
| 2 producers p50 | 47 ns | 80 ns |
| 2 producers p99.9 | 745 ns | 278 ns |
| 8 producers p50 | 50 ns | 112 ns |
| 8 producers p99.9 | 1,682 ns | 1,340 ns |

The median rises because producers now stay alive and contend for the whole run; the tail falls because pacing stops producers starving the writer — less so at high producer counts, since `yield()` doesn't target the writer.

**Container changes, each measured.**
- *Cancel index* → `boost::unordered_flat_map` (open addressing). Cancel ~40% faster at every depth; concurrent p50 down ~6 ns and retries down 15–20%.
- *Price levels* → `boost::flat_map` was tried and rejected: cheap inserts, but erasing a level shifts the whole array, and cancel p99 hit 620 ns at 10,000 levels.
- *Price levels* → `absl::btree_map`, adopted. Resting submit flat at 8.3 ns to 100,000 levels, and the best concurrent figures above.
- An early btree run looked like a 15× regression. A single-producer test ruled out allocator contention; the real cause was a build missing `-O3`.

**Smaller changes.** Removing redundant atomics from the queue's indices (already protected by the mutex) cut p50 by ~3 ns. `alignas(64)` on `Order` showed no measurable change, below the instrument's resolution at this batch size. A 500,000-operation churn test showed no drift from free-list fragmentation.

---

## Known limitations

- `submit` ignores `rest`'s return value, so an order arriving at a full pool could drop its remainder. Never observed with the pool sized well above peak. `modify`'s cancel-and-resubmit can't hit this, because the cancel frees a slot first.
- Self-crossing orders from the same participant are not prevented.
- A lock-free queue was prototyped early but compared using the old, flawed instrument. On hold, not rejected.

## Out of scope

Risk checks, pricing, persistence, and multiple instruments per book.

## Future work

- Re-run the lock-free comparison on the corrected instrument.
- A writer-targeted wake instead of undirected `yield()`, to keep the tail benefit at high producer counts.
- Per-producer response queues so producers learn why an order failed, not just that it did.
