# Matching Engine

*A limit order book matching engine in modern C++ — price-time priority, a single-writer concurrency layer, and correctness demonstrated rather than asserted.*

**Status: feature-complete.** Built over summer 2026 as a self-directed project. The core is correct, concurrent, verified under both sanitisers, measured across book depths and execution paths, profiled, and optimised through a full profile → optimise → re-measure loop. Everything in *Future Work* is genuinely optional — the project is not paused mid-build.

---

## What it does

This project implements the core component of an electronic exchange: a limit order book. The book stores buy and sell orders, matches incoming orders against resting orders on the opposite side, and maintains the remaining orders waiting to trade.

Matching follows **price-time priority**. Better prices always execute first, and when multiple orders exist at the same price, the oldest order is matched first (FIFO). Trades execute at the **resting order's price**, matching the behaviour of real-world exchanges.

The matching core is single-threaded by design, not by omission. A concurrency layer wraps it without modifying it: many producer threads push requests onto a bounded MPSC queue, and one writer thread owns the book outright. That decision is derived from the fairness rule itself and is defended in full below.

### Status

| | |
|---|---|
| Hand-authored replay tests | 23, all passing |
| Property-based fuzzing (single-threaded) | 100,000 operations, 4 invariants, 0 violations |
| Property-based fuzzing (through the queue) | 1,600,000 operations across 1–16 producers, 0 violations |
| Shrinker | Proven against injected bugs (34→20 single-threaded, 19→2 queued) |
| Determinism | Verified in both directions — byte-identical books on replay |
| ThreadSanitizer | Clean across the full suite |
| Benchmarking | p50/p99/p99.9/max for five operations, four book depths, and the concurrent path |
| Optimisation | Full profile → optimise → re-measure loop; submit-resting from 20.8–62.5 ns to a flat **8.3 ns** |
| AddressSanitizer | Clean across the full suite |

### Implemented

- Submit limit and market orders, with input validation at the door
- Matching with price-time priority and partial fills cascading across price levels
- Cancel by id — O(1), with automatic level cleanup
- Modify with full price-time-priority fairness rules, verified by consequence
- Property-based invariant testing with a working counterexample shrinker
- Bounded MPSC ring buffer (mutex + condition variable) with a stated overflow policy
- Single-writer loop with two-phase drain on shutdown
- Producer-partitioned, self-describing order ids
- Intrusive linked list with a hand-rolled order pool — fixed-capacity, free-list, fail-on-exhaustion
- Latency percentile harness with book-depth sweep and end-to-end concurrent measurement

### Out of scope for v1

Response path (designed, not built), self-trade prevention, persistence, risk and pricing models. See *Scope*.

See `dev_log` for session-by-session build progress — including the bugs, the wrong predictions, and the two optimisations that were built and reverted.

---

## Build & Run

```bash
mkdir build
cd build

cmake ..
cmake --build .

./tests
```

ThreadSanitizer build:

```bash
clang++ -std=c++23 -fsanitize=thread -g -O1 ...
```

---

# Design Decisions

The goal is correctness first. Every choice favours maintaining the matching rules while keeping operations efficient, and every rejected alternative is recorded with the specific ground it was rejected on.

## The `Order` type

**Choice**

Each order stores a stable id, a separate engine-assigned arrival sequence number (`seq`), side, type, price as integer ticks, and remaining quantity.

**Why**

Prices are integer ticks because floating-point cannot represent most decimal values exactly. This is a **correctness** issue, not a performance one — modern floating-point arithmetic is not meaningfully slower than integer arithmetic. The actual problem is that two prices which *should* compare equal can silently fail to, and matching depends on exact price comparison.

Arrival order uses a monotonically increasing counter stamped by the engine at insertion, not a timestamp. A counter is unique and strictly increasing **by construction**, requiring no assumption about clock resolution or monotonicity. A system clock guarantees neither: multiple orders can arrive within one tick, and clocks can be adjusted backwards by the OS.

`id` and `seq` are deliberately separate. A caller needs a stable reference to their own order to cancel or modify it later, and at submission time cannot know what arrival number the engine will assign. Identity has to be independent of the system's internal arrival bookkeeping. `id` answers "which order is this"; `seq` answers "when did it arrive relative to everything else," and only the second resolves FIFO ties.

That split later turned out to be load-bearing for the concurrency model: `seq` becomes the writer's single definition of arrival order, while `id` stays producer-assigned so a producer can name its own order with no round trip.

**Rejected**

Floating-point prices, for the exactness reason above. Timestamps for arrival order, because they guarantee neither uniqueness nor monotonicity. A single combined id/seq field, because it would tie a caller's reference to a fact about system history the caller cannot predict at submission time.

---

## Order Book (per side)

**Choice**

Each side uses a separate ordered map from price to price level. `bids` and `asks` are distinct maps sharing the same default ascending comparator. Best bid is read via `rbegin()`, best ask via `begin()`.

**Why**

The two sides need opposite natural orderings — best bid is the *highest* buy price, best ask the *lowest* sell price — and each side's map should make that ordering directly usable without a per-access filter. A merged map would need a composite key to disambiguate two structurally different concepts; a bid at 103 and an ask at 103 are never compared for the purpose of finding either side's best price, so nothing is gained by forcing them into one key space. Splitting keeps `best()` — the most frequently called operation in the matcher — a cheap O(log n) lookup with no filtering.

**Rejected**

A single price-keyed map, because it needs a composite key or an internal filter at every access and destroys each side's baked-in sort order. A hash map, because it has no ordering, so finding the next-best price after a level empties would need extra bookkeeping or a full scan.

---

## Price Level

**Choice**

Each level stores orders as an **intrusive doubly-linked list** in FIFO order — the `next`/`prev` links live on the `Order` itself, and a `Level` holds only `head` and `tail` pointers. New orders link at the back; matching consumes from the front. Storage comes from a **fixed-capacity pool** (see *The optimisation*).

**Why**

The requirement is stable handles: a cancel index holds a reference to an order for its entire life, so anything that moves orders in memory breaks it. Both an intrusive list and `std::list` provide that. The intrusive version additionally removes the node wrapper and its per-insertion allocation, and lets the pool own the storage layout end to end.

**This started as `std::list`, and the change was driven by measurement, not preference.** `std::list` gives the strongest invalidation guarantee in the standard library — erasing anywhere invalidates *only* the iterator to the erased element — which is exactly what the cancel index needs, and it was the right first choice. What it also does is allocate a node on every insertion, and profiling put that allocation ~50:1 ahead of tree manipulation as a cost. Removing it took submit-resting from 20.8–62.5 ns to a flat 8.3 ns.

**Rejected**

`std::vector`, because it reallocates on growth and every handle into the old buffer dangles. `std::deque`, because its invalidation rules on arbitrary insertion and erasure are weaker and less uniform.

**`std::hive`** (accepted for C++26, formerly `plf::colony`) is designed for exactly this shape — many objects, frequent erasure, stable references, a skipfield keeping iteration cache-friendly. It is rejected on a hard constraint: **hive makes no ordering guarantee**, and a price level is a FIFO queue where *the order is price-time priority*. Restoring order means threading elements with links you own, at which point you have written an intrusive list anyway.

---

## Cancel Index

**Choice**

A hash map from order id to a stable `Order*` into the pool. The index stores **only** the pointer.

**Why**

Cancelling should not require searching the book. The index locates an order in constant time; removal is then also constant time, exactly because of the list guarantee above.

Storing only the iterator is deliberate. An order's own fields are the single source of truth. If the index cached price and side and the order was later modified, the cached copy could silently disagree with the order's actual state — a stale-cache bug with no natural trigger to catch it. Dereferencing live makes that **structurally impossible** rather than merely unnecessary.

The iterator's validity and the id's meaningfulness expire at the same moment: when the order is erased, the list node and the index entry are removed together, so there is never a window where a valid id maps to a dangling iterator.

**Rejected**

`std::shared_ptr`, because the index does not own the order — the level does — and shared ownership adds atomic refcounting on the hot path for no benefit. Caching price and side alongside the iterator, for the staleness reason above.

---

## The Match Loop

**Choice**

An incoming order matches against the opposite side while it has quantity remaining, the opposite side is non-empty, and it crosses the current best price there.

**Crossing conditions, stated by the *incoming* order's side** (the resting side is always the opposite, and anchoring the explanation to it is what caused the bug described below):

- Incoming **buy** crosses when `incoming.price >= resting(ask).price`
- Incoming **sell** crosses when `incoming.price <= resting(bid).price`
- A **market** order always crosses — it has no price to compare

**Trade quantity** is `min(incoming.quantity, resting.quantity)`. The amount leaving the aggressor and the amount leaving the resting order must be identical, since this is one exchange and not two independent adjustments.

**Execution price** is always the *resting* order's price. The resting order is the one already established in the book; the aggressor has no price presence there to execute against. A buy at 105 hitting a resting ask at 102 trades at 102 — the aggressor is price-improved.

When a resting order reaches zero it is removed via `cancel`, reusing the side-agnostic removal logic rather than duplicating it inline. If that empties a level, the level is removed from the map. After the loop, a limit remainder rests; a market remainder is dropped.

**Rejected**

One crossing expression for both sides — tried, and wrong. At equal prices the two comparisons agree, which let a real sell-side bug survive the test suite. See *Bugs found and fixed*.

---

## Modify — the Fairness Rule

**Choice**

Reducing quantity **keeps** queue position: an in-place edit of the quantity field, nothing else moves. Increasing quantity, or changing price, **loses** position: the order is cancelled and resubmitted with the same id and a fresh `seq`.

**Why**

Price-time priority exists to be fair to whoever committed first. Reducing only gives something up — nobody queued behind you is harmed by you asking for less. An *increase* is new commitment that exists only as of the modify, later than everyone who queued in between. If the whole order kept its old position, the newly added portion would execute ahead of orders that arrived earlier than the addition itself. Since an order cannot be split into a fairly-positioned part and a new part, the whole order re-queues. A price change is the same: joining a different level's queue for the first time carries no claim to a position there.

The mechanism that *enacts* "lose position" is the fresh `seq` assigned on resubmission — that later sequence number is the observable proof position was lost.

`quantity == 0` is checked first, before price/quantity routing, so modify-to-zero always cancels regardless of what else was requested.

**How it was tested — verify by consequence**

Fairness was tested through its observable effect on fill order rather than by inspecting queue internals. Two orders rest at one price; one is modified; a matcher is submitted; the fill sequence is asserted.

A blind spot was found and closed here: a matcher sized to consume only the front order cannot distinguish "the tracked order moved to the back" from "the tracked order silently vanished" — both produce one fill against the other order. The fix was sizing the matcher to sweep *through* the front order into the tracked one, so both fills emit and their relative order proves presence and position simultaneously.

**Rejected**

Encoding a limit→market type change via a magic price value, because it overloads a legitimate price with hidden meaning and conflates type with price. Type changes are out of scope for v1.

---

## Validation

**Choice**

Three guards before an order enters the book: the id must not already exist, quantity must be positive, and for limit orders price must be positive.

**Why**

A duplicate id would silently overwrite the cancel index's existing entry, orphaning the original order — reachable by nothing, cancellable by nothing. Non-positive quantity is meaningless input. A non-positive price on a limit is meaningless, since a limit's whole function is to trade at a named price.

Market orders are exempt from the price guard **not because zero carries special meaning** — that pattern was rejected for modify, and the reasoning is the same here — but because a market order's crossing decision never consults price at all. Type alone decides. Price is irrelevant input, not a signal.

`seq` gets no guard because there is nothing to guard: it is never supplied by the caller, only stamped by the engine at insertion.

**Interface consequence**

`submit` returns `std::optional<std::vector<Fill>>`. `nullopt` means rejected before matching was attempted; a present, possibly empty vector means validated and matched. This distinction is not cosmetic — the volume-conservation check depends on it, since collapsing "rejected" with "accepted but no fills" would make a rejected order look like an invariant violation.

Under producer-partitioned ids the duplicate-id check becomes **defence in depth** rather than a live necessity: uniqueness is guaranteed by construction, and the check remains a cheap assertion that the contract held against a misbehaving producer.

---

# Concurrency

## The model: single-writer with a queue

**Choice**

One thread owns the `OrderBook` outright — not "has priority on it," owns it. No other thread holds a pointer to it or has any path to its memory. Producers push `Request`s onto a bounded MPSC queue; the writer pops, dispatches on the tag, and calls the **unchanged** `submit` / `cancel` / `modify`.

**Why — derived from the fairness rule, not from convenience**

The book's `best()` returns a non-const `Order*` and the match loop mutates it in place. That single fact — correct and deliberate single-threaded — is what makes the book unsafe the instant a second thread exists, in two distinct ways:

- **The order dies underneath a reader.** Thread A consumes and erases the best resting order while thread B still holds the pointer. Capture-before-erase cannot help, because there is no "before" under your control.
- **Nothing dies, and it is still wrong.** `quantity -= tradeQty` is load, subtract, store. Two buys of 100 against one resting sell of 100: both load 100, both compute a trade of 100, one store overwrites the other. **200 units trade against a resting order that only had 100.** No crash, every pointer valid throughout, and volume conservation — the most financially load-bearing invariant in the suite — violated with no bug in the matching logic at all.

The decisive argument goes past both. **Arrival order is the semantics.** `seq` encodes arrival, the match loop consumes each level front-to-back in `seq` order, and the entire fairness rule is therefore a statement about sequence — whereas parallelism is precisely the licence not to care which of two things went first. Hand the same set of orders to the engine in a different arrival order and genuinely different people get filled at genuinely different prices.

So parallel matching is not rejected because concurrency is difficult. It is rejected because it is **structurally impossible to gain from**: serialise through locks and you gain nothing, don't and the results change. There is no third option.

Given that, the attack is on *sharing* rather than on *concurrent access*. Shared-but-immutable is safe; mutable-but-unshared is safe; only the overlap is undefined behaviour. Immutability is unavailable — a book that cannot change is not a book — so sharing is the thing to eliminate.

Checked against every failure mode: two threads racing on `quantity` — impossible, one writer. Dangling `Order*` — impossible, the only thread that can obtain one is the only thread that can invalidate it. Fairness scrambled by scheduling — impossible, the writer processes in pop order. Deadlock — impossible, no locks on the book at all. The character of that matters: **structurally absent, not defended against.**

**The seam — why the engine is untouched**

The public API already returns by value (`submit → optional<vector<Fill>>`, `cancel`/`modify → bool`), so no caller holds a reference into the book. The writer calls these unchanged and the concurrency layer sits entirely outside them. Every existing test, the 100k fuzz and the shrinker stay valid because the thing they validated did not change — and that is verifiable by diff, not by assertion.

**The honest cost**

The writer is a **throughput ceiling**: one thread's worth of matching, forever, regardless of hardware. The escape hatch is horizontal — **shard by instrument**. One book per symbol, one writer per book, genuinely parallel, because different books share nothing. Arrival order matters within a book and is meaningless across books. That is the parallelism actually available, and it is available precisely because it does not cross the sequential constraint.

**Objection: the queue is itself shared mutable state**

Three reasons this is progress rather than relocation. The **contention surface** collapses from unbounded (every level, order, map node, index entry) to two indices touched by opposite sides. The **operations become trivial and bounded** — push-one and pop-one have essentially no intermediate state, versus `submit` sweeping an unknown number of levels. And decisively, **a queue has no ordering semantics to violate**: matching's correctness *depends on* sequence, while a queue's only job is to *establish* one, and any consistent order will do. Concurrent matching is not a hard problem; it is the wrong problem.

---

## The three rejects

They fail for three different reasons, which is what makes this an analysis rather than three ways of saying "I picked the easy one."

**1 — Global lock. Fails on performance; correct but pointless.**

One mutex around the whole book. Genuinely **correct**: every failure mode prevented, a real total order exists, `seq` assigned consistently inside the critical section. It fails because it buys nothing. Four threads, 400 ns critical section, one at a time → the throughput of the existing single-threaded engine, plus costs the single-threaded version does not pay: lock acquire/release per operation, contention (a blocked thread is descheduled and later woken, and a context switch is *microseconds* against a 400 ns critical section — coordination overhead thousands of times the cost of the work it protects), convoying, and destroyed p99 tail latency.

Root cause: a lock does not create parallelism, it re-serialises threads that could never run in parallel anyway.

**When it would be right:** when the critical section is a small fraction of thread runtime — 10 µs of independent parsing and risk work against 100 ns of shared access is ~1% contention, and the global lock is the correct, simple tool. The test is *what fraction of a thread's runtime sits inside the lock*. Matching **is** the workload, so it approaches 100%.

**2 — Lock per price level. Fails on correctness, even implemented perfectly.**

An aggressor sweeping levels cannot release earlier locks as it moves — a concurrent rest at an already-swept level would be missed — so it holds locks cumulatively **in an order determined by market data**. Two aggressors sweeping in opposite directions (buy ascending, sell descending) acquire in opposite orders: textbook deadlock. The standard fix, global lock ordering, fights the algorithm, since a sell naturally sweeps descending and would have to acquire locks *before knowing it needs them*. Most operations are not level-local anyway: erasing or inserting a level mutates the map, `cancelIndex` is global, and `best()` is inherently global.

**But the killer is not mechanical.** Even with perfect ordering and zero deadlocks, two same-priced orders on two cores fill in whatever order the OS scheduled, with `seq` assigned by whoever got there first. Price-time priority violated with **zero memory bugs**. No amount of locking skill fixes it, because the problem is not in the locking.

**When it would be right:** when elements are genuinely independent with no cross-element ordering rule — a sharded hash map with per-bucket locks is exactly this and works beautifully, because nobody cares which of two concurrent inserts happened first.

**3 — Lock-free book. Fails on feasibility and on verifiability.**

Lock-free structures exist where a mutation is one pointer swap, so a single CAS publishes it. Matching is not that: one incoming order reads best price, mutates a resting order's quantity, appends to fills, may erase a list node, may erase a map entry, updates the cancel index, may insert a remainder. That is a coordinated multi-structure transaction with no single atomic operation that publishes it — you would need multi-word CAS or STM, which is research machinery rather than a design choice. Plus memory ordering, ABA, and safe memory reclamation.

**The decisive argument is specific to this project.** A too-weak memory ordering is a *silent* failure: no crash, no assertion, correct on the overwhelming majority of runs, failing non-deterministically and differently across architectures and optimisation levels. This project's entire testing apparatus is invariant checks over an executed operation stream, and a memory-ordering bug can corrupt the queue in ways that yield a plausible-but-wrong stream. **It is precisely the one bug class this infrastructure is structurally blind to.** On ARM such bugs are *exposed* non-deterministically, which is not the same as *detected*.

So the reasoning is coherence, not effort: adding unverifiable work to a project whose credibility rests on demonstrated correctness is negative value. **An unverifiable claim is worse than an absent one.**

**The unifying statement.** The problem is not concurrency. It is that matching has a semantic requirement — a total arrival order — that concurrent execution destroys. All three rejects attack the *mechanism* of concurrent access; none addresses the *requirement*. Single-writer wins by starting from the requirement: establish the order once, in one cheap place, and let one thread execute against it.

---

## Order id assignment — producer-partitioned

**Choice**

Each producer owns a disjoint id space: **1 sign bit clear · 7 bits producer · 56 bits counter** on `int64_t`. 128 producers, ~7×10<sup>16</sup> ids each — at 1M orders/sec, over two thousand years to exhaust one producer's space. The per-producer counter is a plain **non-atomic** `int64_t`, incremented only by its owning thread.

**Why**

Uniqueness is **by construction**: two producers cannot collide because they were never drawing from the same pool. Same technique as single-writer, one level down — data with exactly one writer has no concurrency problem.

The producer field is 7 bits, not 8, for a specific reason: a field of width *w* at shift *s* occupies bits *s* through *s+w−1*, so an 8-bit field at shift 56 reaches **bit 63, the sign bit**. Shifting into the sign bit of a signed type is UB, and even where it appears to work you get a negative id that surprises everything assuming ids are positive.

`static_cast` happens **before** the shift: `producerId` is an `int`, and shifting a 32-bit value left by 56 overflows the `int` and is UB before the result would ever be widened.

Fields are merged with `|` rather than `+`. For disjoint fields they are identical, but `+` **carries** if the fields ever do overlap, corrupting the high field too, while `|` keeps the damage local — and documents intent: independent fields merged, not numbers summed.

**The latency win, stated precisely:** the producer knows its id **at push time**, so cancellation requires no round trip. It does *not* give response reliability — a response path is needed regardless, because producers must learn about fills. The win is narrower and sharper: it decouples *"can I name my order?"* from *"did my order work?"*

**Why this creates no new correctness problem:** a producer pushing submit then cancel for the same id puts both in one FIFO queue, so the writer pops submit first, always — a cancel **cannot overtake its own submit**. If the submit was rejected, the cancel arrives for an id not in the book, which `cancel` already handles as a clean no-op, tested since cancel was built.

**Rejected**

**Engine-assigned ids** — uniqueness by construction at the engine, but the producer cannot know the id until the writer responds, so **the order is uncancellable until a round trip completes**. Decisive against for a latency-focused project.

**Modular partitioning** (`id % N == p`) — equally collision-free, but **N is baked into every id**, so adding a producer invalidates the decoding of everything already issued. The scheme is not stable under growth. Extraction is also integer division against one shift. Bit-packed ids are additionally **self-describing** — `id >> 56` recovers the issuer — a property designed for debuggability that turned out to solve response-path routing for free.

**A global `atomic<int64_t>` with `fetch_add`** — puts every producer's every order through an atomic RMW on **one shared cache line**: cross-core ping-pong. Partitioned counters are plain increments on thread-local memory.

**FIX-style dual ids** (`ClOrdID` + `OrderID`) — correct at venue scale, and it solves two genuinely different problems: the client's (reference my order immediately with no round trip) and the venue's (one identifier unique across every client, session and day, for audit and regulatory reporting). Understood and deferred: producers here are trusted in-process network threads, not untrusted external clients, so the partition contract is enforceable rather than hopeful.

---

## The queue — bounded MPSC ring buffer

**Choice**

Fixed-capacity `vector<Request>` allocated once at construction, `head`/`tail`/`count`, one `std::mutex`, one `std::condition_variable`, a `stopping` flag. Capacity constrained to a power of two so wrapping is `& (capacity - 1)` rather than integer division on the hot path.

**Bounded, not unbounded — a correctness argument, not a comfort one.** If producers outrun the consumer on an unbounded queue, memory grows without limit (and growth means allocating on the hot path, the exact unpredictability benchmarking exists to attack) and latency grows without limit. An order sitting in a queue while the book moves is technically correct and commercially a wrong outcome. **In a trading system, unbounded latency is a failure.** Bounding forces a stated overflow policy rather than undefined behaviour.

**Full vs empty resolved with a count.** With head and tail alone, empty and full are both `head == tail` — identical state, opposite meaning. The alternatives were sacrificing a slot or keeping a separate count. Count uses every slot and expresses the condvar predicate directly. Accepted cost: a third piece of state that must be updated on every push and pop or it drifts.

**The mutex wraps the whole method body, not individual lines.** `push` is four steps — check full, write, advance tail, increment count — and the invariant spans all four. Two producers with one slot left would both read not-full, both write to **the same slot**, then advance tail twice for one item. This is the atomic-vs-mutex distinction exactly: an atomic makes one variable's operation indivisible, a mutex makes a region exclusive.

**Two pops, deliberately, because `nullopt` means two different things.** `pop()` is non-blocking and `nullopt` means "nothing right now." `waitAndPop()` blocks, so `nullopt` can only mean **"stopping, and nothing left — exit."** Two behaviours deserve two names; one overloaded name is how a caller waits when it meant to poll.

**A third method, `waitAndDrain`, added after measurement.** It takes the lock once and drains up to N requests into a caller-supplied vector, which the writer then dispatches *outside* the lock. Dispatching under the lock would block producers for N × ~42 ns, which is worse than the behaviour it replaces. The vector is owned and reserved by the writer, so the drain never allocates.

**The result was mostly negative, and that was informative.** Batching cut the far tail ~20–40% and did nothing to p50. If the queue's per-operation cost had been lock acquire/release overhead, amortising it across ten operations would have shown at the median. It did not — which ruled out lock *overhead* and pointed at lock *contention*, later confirmed directly.

**The condition variable is writer-side only.** `wait(lock, predicate)` sleeps *and atomically releases the mutex while sleeping*. Both halves are essential — without the release, a writer sleeping while holding the lock would block every producer, so nothing could make the queue non-empty and nothing could wake it. The predicate form is mandatory because threads can wake with **no notification at all**; the predicate is `count > 0 || stopping`, where the second half is what lets shutdown wake a sleeping writer.

`push` must **not** wait — an early draft that reused the same `wait` deadlocked on the very first call, with the first producer at an empty queue sleeping to wait for an item only it could have supplied. The distinction that resolves it: producers *do* take the mutex, briefly and boundedly, but "producers never block" means **overflow waiting** — sleeping until a slot frees, which is unbounded. A full queue returns `false` immediately.

---

## Overflow policy — reject on full

**Choice**

`push` returns `bool` and never blocks.

**Why — fault isolation.** A full queue refuses *one request* rather than stalling a producer and everything queued behind it. Blocking couples every subsequent request to one request's fate — head-of-line blocking, the same failure shape that makes one big lock bad.

**On the fairness objection:** rejection at capacity is **a capacity report, not a fairness intervention**. Nothing is reordered, nothing favoured, no request jumps another. Price-time priority governs orders that got in; it says nothing about a physically full queue. Blocking is arguably *more* interventionist, since it silently changes when everything behind it arrives.

Rejection is also explicit and attributable — the producer holds the exact failed request and can retry, report or escalate. That is categorically different from a drop, which is silent and unattributable.

**Reject-on-full does not remove backpressure, it relocates the decision.** The queue refuses; the *producer* chooses what to do. Under blocking, one policy is imposed on every producer regardless of what that producer needs. Both choices are demonstrated in the test suite: one test's producer retries with spin-then-yield backoff, another's drops and counts.

**Rejected**

**Block the producer** — nothing is lost and backpressure propagates cleanly, but it stalls the producer and everything behind it, including traffic for unrelated orders and clients. It also cuts badly against the **cancel asymmetry**: a blocked *submit* can delay a risk-reducing *cancel* queued behind it, the operation you least want to lose in a moving market. Legitimate where upstream backpressure is the goal.

**Drop newest** — silent loss; an order the client believes was sent simply vanishes. Indefensible for orders.

**Drop oldest** — correct in market-data feeds, where stale ticks are worthless and only the latest matters; **precisely wrong here**, since the oldest queued requests have the strongest claim under price-time priority. Worth naming as a case that is right in a neighbouring domain and wrong in this one.

**Backpressure, understood properly.** Backpressure is a chain of finite buffers: client → network → kernel socket buffer → producer thread → queue → writer. When the last fills, fullness propagates backwards until it reaches something that cannot push back — **and that boundary is where loss actually happens**. Under TCP the propagation is clean (the receive window shrinks and at zero the sender's stack stops transmitting, so the client's own `send()` blocks). Under UDP there is no window mechanism and the kernel silently discards datagrams — one reason multicast market-data feeds are designed around dropping stale data. **Blocking does not remove loss from a system; it relocates loss to whichever boundary cannot propagate backpressure.**

---

## Shutdown — two-phase drain

`shutdown()` takes the lock, sets `stopping`, notifies. The writer keeps processing until the queue is empty, then exits. `push` refuses once `stopping` is set, so the backlog is finite and the drain terminates.

**Drain rather than discard, and the argument is the overflow policy's.** Reject-on-full exists so that **acceptance means something** — a producer holding `true` knows its request is in the system. If shutdown discarded accepted requests, `true` would silently stop meaning that. Draining preserves a clean, statable guarantee: **accepted implies executed.**

"Finish the in-flight action" needs no design: the writer only checks the flag when it comes back around to `wait`, and a flag cannot preempt a function call, so an in-flight `submit` always completes. `notify_all` rather than `notify_one` on shutdown — equivalent with one writer, but cheap insurance on a path that runs once.

---

## `Request` — the queue element

**Choice**

A flat tagged struct: `OpType` tag, `Order`, `Id`, `optional<Price>`, `optional<Quantity>`. Kept **separate** from the test-side `LoggedOp`, with a one-way `Request → LoggedOp` conversion.

**Why**

The producer cannot simply hand over an `Order` — that covers submit only; cancel needs an id, modify needs an id plus two optionals. Three shapes, one queue.

`id` is a plain `Id`, not an optional, because cancel and modify *always* have one — optional would mean "may legitimately be absent," which is false. Contrast `newPrice`/`newQuantity`, where absent genuinely means "leave unchanged."

**A `variant` was the more principled design and was deliberately traded away.** A variant makes irrelevant fields *unrepresentable*; the flat struct makes it a convention the tag enforces. Accepted cost: cancel and modify carry a meaningless zeroed `Order`, and `Order{}` is used rather than plausible-looking values specifically so it fails loudly if ever read.

**Why not reuse `LoggedOp`:** it is a *test artifact* recording what happened; `Request` is a *production message* describing what a producer wants. They are field-identical today and are different concepts — the moment benchmarking adds a timestamp to `Request`, they diverge. **The value was never in the conversion function, which is one line; it is in the types being separate.** Conversion earns its keep by buying the existing replay machinery for free, including the shrinker, so a failing concurrent run can be minimised by a tool already proven against an injected bug.

**Size note:** the struct is as large as its biggest variant, ~40–48 bytes, so a bounded queue costs `capacity × sizeof(Request)` up front — a 65,536-slot queue is roughly 3 MB. Real, not a rounding error.

---

## `writerLoop` — the single writer

`waitAndPop` → `break` on `nullopt` → dispatch on the tag to the unchanged `submit` / `cancel` / `modify`.

**`seq` is not stamped here.** `rest()` already does `o.seq = nextSeq++`, and that placement is the better one: `seq` orders orders *within a level's queue*, and `rest` is exactly the moment an order enters one. Numbers are issued when they are used, rather than being burned on market orders that fully fill, on cancels, on modifies and on rejected submits. It is also why modify's reposition works — it routes through cancel then submit → rest and picks up a fresh, later `seq` automatically.

The single definition of arrival order is preserved **not because the writer stamps a number, but because only one thread ever calls `rest`**. So `nextSeq++`, a plain non-atomic increment, is only ever executed by the writer. "How is `seq` assignment thread-safe?" answers: it does not need to be.

**A layering rule, learned by getting it wrong.** `Request`, `RingBuffer` and `writerLoop` were initially written *inside* the `OrderBook` class. They live at file scope instead, and the argument is not stylistic: **the book must not know that queues or threads exist.** It is a passive data structure the writer drives from outside, and that separation is what makes "matching logic untouched" true structurally rather than by accident.

---

## Response path — designed, scoped out of v1

Rejection returns **synchronously** (`push -> bool`), so the overflow policy is complete on its own. What a full response path additionally carries: acknowledgements, and **fills** — zero to many, arriving arbitrarily later, since a resting order can fill repeatedly over its life. That asynchrony is the design pressure.

Only the writer knows outcomes, so this path is **one producer, many consumers** — the mirror of the request queue.

**Design on record: one SPSC queue per producer.** Routing is free, because the bit-packed ids are self-describing: `id >> 56` recovers the destination with no lookup table. Each per-producer queue is then single-producer/single-consumer — the simplest concurrent structure that exists — so no SPMC machinery is needed anywhere. Cost is N queues of memory.

**Rejected:** one shared SPMC queue, because every producer wakes for every response and filters out what is not theirs, with contention from all readers on one structure. And **callbacks**, because they execute *on the writer thread*, so a slow producer callback directly stalls matching — categorically unacceptable in a single-writer design.

**Why scoped out:** it is the same concurrency lesson at *lower* difficulty than the MPSC request path, so it costs real time and demonstrates nothing new. Scope must be justified by a requirement, not by completeness.

---

# Testing

Four layers, all maintained continuously rather than deferred.

**Layer 1 — replay harness (23 hand-authored tests).** Scripted sequences with asserted exact fills and exact final book state: buy and sell aggressors, rest-remainder, market orders, empty book, multi-level sweeps, exact-match boundaries, four cancel scenarios, five modify scenarios covering every fairness case, an unequal-price cross-comparison test, and three validation guards.

**Layer 2 — property-based invariant testing.** Four invariants checked after every operation under weighted, randomised-but-valid flow: no crossed book, volume conservation, FIFO preserved, no orphaned cancel-index entries. 100,000 single-threaded operations, zero violations.

The invariants are standalone, `const`, pure-observation methods — explicitly **detection, not enforcement**. They watch and report; they never intervene or auto-correct, since acting on a violation would hide the very bug the check exists to expose.

**FIFO's safety was proven, not assumed.** Every insertion path was traced in the actual code: `restInto` only appends at `end()`; the match loop only pops from the front; modify's reduce path mutates a field in place without touching list position; modify's increase and price-change paths go through cancel then submit → rest. No path can reorder a list. The check is kept anyway as regression insurance against a future change silently invalidating the guarantee — which is exactly what happened once already, to a different assumption, during the map unification.

**Layer 3 — the shrinker.** When an invariant fires, the exact failing sequence is captured and reduced: try removing one operation, keep the removal if the failure still reproduces, restore it if not, and repeat full passes until one pass removes nothing.

Proven twice against deliberately injected bugs — 34 operations reduced to a stable 20 single-threaded, and 19 reduced to **2** through the queue, which is the theoretical minimum for a crossed book since it takes exactly two orders on opposite sides that should have matched and did not.

*Known limitation, accepted:* one-at-a-time removal can stall above the true minimum when operations reference each other by id. Removing an early submit does not crash a later cancel of that id — cancel on an unknown id is a clean no-op by design — but it silently changes the book's later trajectory, which can make an operation look load-bearing when it is only *referenced*, not causally necessary. Reaching the global minimum would need group removal, judged out of scope for v1.

**Layer 4 — concurrent verification.** Everything the single-threaded layers assert, plus what only exists once threads do:

- **Fuzzing through the queue.** 1,600,000 operations across 1–16 producers, zero violations. Requires blind generation, since the generator cannot read the book at all — which is the design working, not a limitation to route around. The single-threaded fuzzer was **deliberately kept, not replaced**: it asks *is the matching logic correct?* against a book it can read directly, while the queued one asks *does the concurrency layer preserve that?* If the queued run fails and the single-threaded one passes, the bug is localised to the concurrency layer in one run.
- **The determinism check.** Given the sequence the writer actually executed, replaying it single-threaded through a fresh book produces exactly the same final state — so the concurrency layer decides *what order* things happen in, and nothing else. Comparison granularity was a real decision: total volume catches gross divergence and misses almost everything; per-level quantities catch structural differences; **per-order, in queue order, is the one that matters**, because two books can hold identical level quantities with orders queued in different positions, and the next aggressor to hit that level would fill a *different participant's* order. That is price-time priority broken, and every weaker check calls the books identical. Proven both ways: a deliberately injected capture skip failed the check and localised it to a single missing id at one price with surrounding queue positions intact.
- **Cancel-mid-match.** 500 iterations racing a crossing aggressor against a cancel for the resting order through a start gate. The sharp assertion is `cancelFirst != aggressorFirst` — **exactly one** of the two legal outcomes, ruling out any state no interleaving could produce. Both orderings genuinely occur (roughly 51–124 cancel-first against 376–449 aggressor-first across runs).
- **Backpressure, both policies.** Four producers into a 512-slot queue against a deliberately slowed writer, with the assertion that no operation is lost under sustained pressure — plus `totalRetries > 0`, because **the test has to prove it exercised something**: zero retries would mean the writer kept up and the run was vacuous.
- **ThreadSanitizer — clean across the full suite.** This verifies something categorically different from every other check here. The rest verify that the *book ends in a valid state*; TSan verifies that no two threads accessed the same memory concurrently without synchronisation, **regardless of whether the outcome happened to be correct**. A data race is undefined behaviour even on runs that produce the right answer — exactly the class of bug that survives testing.

  It also **closes an argument rather than merely passing**: lock-free was rejected on the grounds that this project's test infrastructure is structurally blind to memory-ordering bugs, and TSan is precisely the infrastructure that would detect that class. Running it converts "the mutex design is simple enough to verify by inspection" from an assertion into a corroborated claim.

  *Stated precisely, because the limitation is real:* TSan is **dynamic**. A clean run means no race occurred on any path this suite exercised — substantial coverage given the suite includes deliberate races, saturation and fuzzed flow, but evidence rather than proof. It also detects **races**, not all memory-ordering errors: an `acquire` where `seq_cst` was needed is a correctly-synchronised access with insufficient ordering, and TSan can pass while the code is wrong.

---

## The recurring lesson: a passing test is not evidence until you confirm what ran

Four times in this project a test passed while exercising the wrong thing, and every one was caught by checking **output composition** rather than trusting the pass:

1. The fuzzer's type-weighting was inverted, so a "clean" run was against mostly-market flow that starved the interesting paths.
2. An integration test reused one id across three requests, so two were rejected as duplicates and the assertion passed purely because the *first* order rested.
3. The first queued fuzz reported 99,000 clean operations while executing **100% submits and zero cancels or modifies** — a bootstrap condition still tested `restingIds.empty()`, a structure the new code no longer maintains, so the branch containing all three operation types was never reached. Caught by `./tests | grep -c "Cancel Request"` → 0.
4. The first ThreadSanitizer run printed two lines, because most of the suite was disabled and TSan had observed a single test.

The practice is now explicit: **after any change to a generator or to what runs, verify the mix, not just the result.**

---

# Benchmarking

Measurement infrastructure only — no engine code changed to produce these numbers.

## Method, and why each part is there

**The instrument was calibrated before it was used.** `submit` costs 30–60 ns and `steady_clock::now()` is not free. Measured: **16.8 ns per call, 33.7 ns per pair.** If reading the clock costs a large fraction of the operation, that is a **systematic bias** present in every sample in the same direction — more samples give a very precise estimate of the wrong number. Precision and accuracy are different things.

The calibration itself surfaced a trap worth recording: the empty-loop baseline reported **0 ms**, because `empty_acc += i` over a known range is Gauss's sum and the compiler evaluated it at compile time. Printing the *value* kept the value alive but not the loop that computed it. **An unexplained fast result is a measurement artifact until proven otherwise.**

Also worth knowing: `now()` is a **barrier to optimisation** — an opaque call the compiler cannot move memory operations across, and on some platforms a serialising instruction. Timed code can compile differently from the same code untimed.

**Batching, and the trade it makes.** A 33.7 ns pair against a 30–60 ns operation makes per-operation timing untenable, so each sample times a batch and divides. Batch size trades measurement overhead against tail resolution, and no setting gives both:

| Batch | Overhead/op | A 10 µs stall reads as |
|---|---|---|
| 1 | ~34 ns (~85%) | perfect resolution |
| 10 | ~3.4 ns (~8%) | +1000 ns — obvious |
| 100 | ~0.34 ns (~1%) | +100 ns — blurring |
| 1000 | ~0.03 ns | +10 ns — lost |

Batch **10** for four of five benchmarks: ~8% overhead, stated, with a stall still visible as a batch ten times slower than its neighbours. Modify-in-place needs **100**, because it sits too close to the measurement floor at 10.

**Honest labelling: these are percentiles of batch means, not of individual operations.** That is weaker than "p99 latency" and is stated as such.

**Percentiles rather than a mean.** A mean is pulled by outliers while hiding them: 99 operations at 40 ns and one at 10,000 ns averages to 139 ns, a number describing **no actual operation**. The tail matters more than the mean here specifically — at a million operations per second, p99 fires ten thousand times a second, so it is not rare, it is constant. And the slow ones cluster at the worst moments: allocation stalls happen when the book is deep, which is when the market is busy.

**Coordinated omission, considered and ruled out.** It is an *open-loop* problem — it bites when load arrives on a fixed schedule and a stall means you never measure what queued up behind it. This harness is *closed-loop*: call, wait for return, call again. There is nothing to omit. It becomes relevant only if a rate-driven generator is built.

## Results — five operations (ns, `-O3`, single-threaded, Apple M4)

Two sets: the baseline that motivated the optimisation, and the current numbers after it.

| Operation | | mean | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| Submit — resting only | before | 38.8 | 33.4 | 83.3 | 237.5 | **10404.1** |
| | **after** | — | **8.3** | **8.4** | — | — |
| Submit — always crosses | before | 38.5 | 37.5 | 50.0 | 95.9 | 191.7 |
| Cancel | before | 28.8 | 29.2 | 41.7 | 70.8 | 154.2 |
| Modify — in-place | before | 3.6 | 3.34 | 5.83 | 10.83 | 19.58 |
| Modify — cancel + resubmit | before | 55.9 | 54.2 | 83.3 | 91.7 | 141.7 |

Mixed flow at depth 100, before → after: mean 44.6 → **37.8–38.5** · p50 41.7 → **37.5** · p99 104.2 → **75–79** · p99.9 187.5 → **100**.

**Submit-resting's tail was pathological and submit-crossing's was not**, despite near-identical means (38.8 vs 38.5) and near-identical bodies. Max differed by **54×**. The mechanical difference is that resting allocated a `std::list` node every time while crossing mostly consumes existing ones. A batch *mean* of 10,404 ns means a single operation inside that batch was likely far worse — the true max is worse than this harness can see, which is an inherent limit of batching, stated rather than hidden.

**That observation drove the whole optimisation phase**, and it turned out to be right for the right reason. See *Profiling* and *The optimisation* below.

**Modify-in-place carries a caveat that the number alone hides.** ~3.3 ns is roughly 10 cycles for a hash lookup, an iterator dereference, branch checks and a write. It is plausible only because this benchmark is unusually cache-friendly — sequential access, ~90 price levels reused, whole working set in L1. The honest claim is *"3.3 ns under a sequential access pattern with a small working set,"* not *"modify costs 3.3 ns."*

## Book-depth sweep (p50, ns)

Reporting a single figure implicitly claims the curve is flat. `std::map` is a red-black tree: finding a level is O(log n) comparisons but also **O(log n) separately-allocated nodes**, so it is O(log n) *potential cache misses*.

Two methodology decisions keep this honest. Depth is swept at **constant orders per level**, not constant total, so tree width is isolated rather than traded against list depth. And the **seeding** is parameterised rather than the generator's price band, because widening the band would also collapse the crossing rate the band was chosen to maximise — moving two variables at once.

**Before the optimisation:**

| levels | sub rest | sub cross | cancel | mod qty | mod price |
|---|---|---|---|---|---|
| 10 | 20.8 | 41.7 | 62.5 | 6.2 | 100.0 |
| 100 | 20.8 | 45.9 | 83.4 | 6.7 | 137.5 |
| 1000 | 29.1 | 50.0 | 120.8 | 7.5 | 191.7 |
| 10000 | 62.5 | 58.4 | 204.1 | 7.1 | 391.6 |

**After:**

| levels | sub rest | sub cross | cancel | mod qty | mod price |
|---|---|---|---|---|---|
| 10 | **8.3** | 33.3 | **37.5** | 5.0 | **70.9** |
| 100 | **8.3** | 41.7 | **45.9** | 4.6 | **100.0** |
| 1000 | **8.3** | 41.6 | **54.2** | 4.6 | **125.0** |
| 10000 | **8.4** | 45.9 | **95.9** | 4.6 | **225.0** |

**Submit-resting is now flat.** It grew 3.0× across the sweep before; now it is 8.3 ns at every depth, reproducible across three runs — **7.4× faster at 10,000 levels, with the depth-dependence gone entirely.** That says its old growth was *wholly* allocation-related (bigger book → more heap pressure → slower `malloc`), not tree traversal.

**And its tail collapsed with it: p99 of 8.4 ns against a p50 of 8.3.** Before, p99 at depth was 108–195 ns against a p50 of 62.5. The tail did not shrink — **it disappeared** — which is precisely what removing a *variable* cost predicts. A free-list pop is the same three instructions every time; there is nothing left to vary.

Cancel is 2.1× faster at depth, modify-price 1.5×. Both still scale, so genuine tree cost remains in those paths. **Modify-in-place is unchanged at ~5 ns** — the control, since it never allocated.

## Concurrent path

Measured by batch-timing the writer loop itself: one thread, one clock, no cross-thread timestamp comparison, and no permanent field added to `Request` to carry a timestamp.

| | before pool | after pool |
|---|---|---|
| p50 | 50.0 | **45.8** |
| p99 | 1425–1546 | **1250–1283** |
| p99.9 | 3954–4296 | **3658–3792** |

**Against a direct-call mixed-flow p50 of 41.7 ns, the queue costs roughly 8 ns per operation** on the common path.

**The same optimisation improved the concurrent path far less than the single-threaded one** — ~13% at p99 against 25%, ~7% at p99.9 against 47%. That gap is the finding, and diagnosing it is below.

---

# Profiling

The discovery gate: everything above is observation; this attributes time to causes.

**Two hypotheses were registered in advance**, each backed by different evidence — **allocation** (submit-resting's 10,404 ns max against crossing's 191 ns, identical bodies) and **tree traversal** (the 3–4× depth curve, with tails staying *proportionate* rather than fattening, which is the signature of a systematic per-operation cost rather than a probabilistic one). Registering both in advance is what makes a profile informative: it adjudicates a prediction rather than starting from nothing.

**Tooling.** No Xcode, so no Instruments — macOS's built-in `sample` instead. Both are *sampling* profilers: they report where time is spent statistically, not exact counts, and at `-O3` an inlined function's time is attributed to its caller, which makes attribution coarser.

**Two methodology errors, both caught.** The first run was **80% process startup** — 234 of 285 main-thread samples were `_dyld_start`. And `sample` attaches immediately, so it catches startup regardless of run length; fixed with a two-second head start before a ten-second window. A third error was in the *reading*: `grep -c` counts lines, not sample weight, and in a call tree the same function appears at many depths.

**Result — writer thread, 4 producers:**

| | samples | share |
|---|---|---|
| Dispatch (real engine work) | 942 | 53% |
| `waitAndDrain` | 764 | **43%** |

Almost all of the 43% is `std::mutex::lock` → `__psynch_mutexwait` — the writer **blocked in the kernel waiting for the queue mutex**.

**Within the engine's own work:**

| Symbol | 4 producers | 1 producer |
|---|---|---|
| `malloc` | 413 | 101 |
| `_free` | 215 | 66 |
| `operator new` | 111 | 38 |
| `__tree_balance_after_insert` | 20 | 4 |
| `__tree_remove` | 30 | **0** |

**Allocation outweighs tree manipulation roughly fifty to one, and the ratio holds at both producer counts.**

**Allocation confirmed. Tree traversal refuted — as stated.** Comparisons and rebalancing are nearly free. But the depth curve was real and reproducible, so something does scale with depth: it is the **cache misses from chasing separately-allocated tree nodes**, not the tree logic. Each hop is a separately-allocated node and therefore a potential trip to main memory. The depth curve is an allocation-layout problem wearing a different hat.

**On the 43% contention figure, stated precisely.** `concurrentBench`'s producers push in a tight loop with zero work between pushes — close to worst-case contention. Re-profiling at one producer dropped `waitAndDrain` to 29% and collapsed `__psynch_mutexwait` from 628 samples to 62. So **contention ranges 29–43% of writer time across the producer counts tested, and where a real deployment sits depends on producer-side work not modelled here.** Neither endpoint is "the true number."

---

# The optimisation

## Intrusive list + order pool

Every `rest` called `push_back` on a `std::list`, allocating a node — a `malloc` on the hottest path, and a **variable** one. Variance is where tail latency comes from.

**Three routes were considered.** A `pmr` allocator (cheapest, and `null_memory_resource` upstream would make "did it stay in the pool?" something the program *enforces* rather than something you assume). A hand-rolled allocator for `std::list` (same category of change at several times the cost — cut). Or an intrusive list with a pool.

**The intrusive list was chosen**, addressing both profile findings with one design, at the stated cost that **it changes allocation *and* layout *and* the container at once, so the resulting curve reflects all three.** That was accepted deliberately: the debugging apparatus already built — four invariants per operation, a million-op fuzzer, a shrinker, a determinism check, both sanitisers — is exactly what makes replacing a proven container survivable.

**`std::hive` was considered and rejected on a hard constraint.** It is designed for precisely this case — many objects, frequent erasure, stable references, a skipfield keeping iteration cache-friendly. But **hive makes no ordering guarantee**, and a price level is a FIFO queue where *the order is price-time priority*. Restoring order means threading elements with links you own, at which point you have written an intrusive list anyway — with a dependency supplying the easy part and the pointer-chasing back.

**The pool.** A `vector<Order>` sized N plus a `freeHead` pointer, with the free list threaded **through the same `next` links the live list uses** — a free slot is not a real order, so its `next` stores the next free slot. Zero extra memory for the bookkeeping, and the constructor is literally "free every slot."

- **Constructed `Order`s rather than raw storage.** The textbook approach is `alignas`-wrapped byte arrays plus placement new. Rejected as ceremony: that machinery produces "a correctly-sized, correctly-aligned block that could hold an `Order`" — which is what an `Order` already is. It earns its place when construction is expensive or the destructor non-trivial; `Order` is six scalars. Noted as *not* what a production allocator would do.
- **Fail on exhaustion, not grow.** Growing *is* a `malloc` — a large one — trading frequent small variance for infrequent large variance, which in tail terms is worse. Falling back to `malloc` reinstates the cost being removed. **Deterministic refusal beats unpredictable delay**, the same argument as the queue's overflow policy one layer down.
- **Copy and assignment deleted**, because `freeHead` points into the object's own vector. That guard immediately caught a real bug at compile time: a replay helper returned an `OrderBook` **by value**, which would have produced a book whose free list pointed into the original's storage.
- **Capacity checking belongs in `rest`, not `validate`.** `validate` asks "is this order well-formed?" — a property of the order, whose answer does not depend on anything else. Exhaustion asks "does the book have room right now?" — a property of book state.

**The iterator that made it tractable.** Changing `Level` from a `std::list` to `head`/`tail` pointers produced 13 compile errors, eight of which were "walk every order in this level" — the volume observers, `checkFIFO`, `quantityAt`, `idsAt`. Giving `Level` a `begin()`/`end()` pair meant **all eight range-for loops compiled with only `.orders` deleted.** That matters disproportionately: several of those eight *are* the correctness apparatus, and a mistake in `checkFIFO` would break the test that catches mistakes elsewhere.

**Verified under AddressSanitizer** across the full suite, plus determinism over 1,000,000 captured operations replaying byte-identical. ASan is load-bearing here: for a change replacing a standard container with hand-written pointer manipulation, checking every memory access matters more than checking outcomes.

## Diagnosing the concurrent tail — three experiments, two negative results

The single-threaded numbers improved dramatically. The concurrent path barely moved. **This is why.**

**Experiment 1 — spin before sleeping. Negative.** Hypothesis: the tail is condvar wake-ups, and the queue is often empty for less than a context switch, so a bounded spin would catch short gaps. Calibrated rather than guessed — **0.357 ns per atomic acquire-load, ~2,800 iterations per microsecond**, noted as an upper bound since an uncontended L1 load is the best case. **Result: nothing**, at either 4 or 16 producers. Measured at both specifically because the 16-producer case had no pool-only baseline, and without one the pool and the spin would have been entangled. **Reverted** — carrying code that does nothing is worse than not having it.

**Experiment 2 — partition samples by whether the drain actually slept. Decisive.** `wait(lock, pred)` does not report whether it blocked, so the technique is to check the predicate yourself *while holding the lock*: if already true, `wait` returns immediately. **Result: exactly 1 drain in 320,000 ever slept.** The entire p99.9 lives in the fast path — 319,999 drains that never touched the condition variable and still produced ~320 samples two orders of magnitude above the median. That eliminates idle time and wake-up latency, and explains experiment 1's null result: there was nothing to catch.

**Experiment 3 — time the lock acquisition separately. The answer.**

| Lock acquisition, per batch | value |
|---|---|
| p50 | **0 ns** |
| p99 | **42 ns** |
| p99.9 | **93,000–104,000 ns** |
| max | 472,000–941,000 ns |

**Starkly bimodal.** 99% of the time the writer takes the lock instantly; roughly 1 in 1,000 times it waits **100 microseconds**.

**The arithmetic closes exactly.** The lock's p99.9 is per *batch*; the drain's p99.9 of ~10,000 ns is per *operation* at drain cap 10. 10,000 × 10 = 100,000 ns. **The entire drain tail is lock acquisition** — convoying, matching the profile's 43%-blocked finding.

And the mean lock cost of ~320 ns against a p50 of 0 is the clearest illustration in the project of why percentiles matter: a rare 100 µs event drags an average into a number describing no actual operation.

## The lock-free queue — built, measured, reverted

With a specific mechanistic diagnosis, it was worth building. Bounded MPSC ring buffer with a **per-slot sequence number** as a publication marker: slot *i* is writable when its sequence equals *i*, readable at *i + 1*, with monotonic indices wrapping only at index time. Producers CAS the tail then **release-store** the sequence to publish; the single consumer **acquire-loads** it before reading and release-stores it forward by capacity to free the slot. ABA does not arise — the CAS is on a monotonically increasing integer, not a pointer.

| | mutex | lock-free |
|---|---|---|
| p99.9 | ~10,000 ns | **542–583 ns** |
| max | 47,000–94,000 ns | **1,000,000–9,000,000 ns** |
| samples per run | 320,000 | 489,000–549,000 |

**It worked on exactly the thing it targeted** — the 100 µs spikes are gone, p99.9 improved **17×**. **And it introduced a worse extreme**: millisecond-scale maxima, two to three orders of magnitude worse and wildly variable. The sample counts also make the comparison unsound — ~500,000 drains for the same work means many returned one or two items, so per-operation figures are not comparable.

**Reverted, and the reasoning changed.** The original rejection was *"a too-weak memory ordering is silent and my test infrastructure is blind to it."* That is no longer honest — it was built and TSan came back clean. The measured version: *it eliminated the lock-acquisition spikes but introduced millisecond maxima and an incomparable distribution; the mutex version's behaviour is understood and bounded, and the lock-free version's is neither.*

**One caveat survives regardless:** TSan detects *races*, not insufficient orderings. An `acquire` where `seq_cst` was needed is a correctly-synchronised access with too weak an ordering, so there is no race to find.

## What the sequence established

The tail was hypothesised to be wake-up latency; a spin ruled that out; partitioning proved the writer sleeps essentially never; timing the acquisition attributed the entire tail to lock contention; and building the alternative confirmed the mechanism while surfacing a different problem. **Two negative results and one confirmed diagnosis — and the negatives were the more informative**, because each eliminated a hypothesis that would otherwise still be live.

## Measurement artifacts characterised

**Cold start is per-process, not per-benchmark.** The first sweep showed an anomalous p99 in the 10-level row. Reversing the sweep order moved the anomaly to the 10,000-level row, proving it followed *position*, not depth. Fixed with a process-level warm-up.

**Tail statistics need repetition in a way medians do not.** One cancel p99 read 591.7 in one run and 125.0 in the next with nothing else changed — while medians were reproducible to within a nanosecond throughout.

**One result recorded as unexplained:** submit-crossing's p99 at 10 levels ran 145.9 / 191.7 / 220.8 / 954.2 across runs against ~45–79 ns at every other depth. Persistent in direction, wildly variable in magnitude. Plausible mechanism — at 10 levels the aggressor is far likelier to consume an entire level and pay the erase path — but not confirmed.

## Contention sweep — a characteristic, not a measurement

Total operations held constant at 400,000 (400k×1, 100k×4, 25k×16) so book size and total work are identical and **thread count is the only variable**:

| Producers | Wall time |
|---|---|
| 1 | 2.28 s |
| 4 | 5.05 s |
| 16 | 9.06 s |

The prediction going in was "no measurable difference," reasoning that the writer is thousands of times slower so producers rarely collide. **That reasoning was incomplete: producers contend with each other, not only with the consumer.**

**Caveat, stated rather than buried:** single runs, no variance, and an instrumented writer that dominates absolute times. The *trend* is consistent enough to believe; the individual numbers are not measurements.

**None of these absolute timings say anything about the engine.** They measure the instrumented build. A 30-million-operation run with prints enabled took 40 seconds of CPU and over 90 minutes of wall clock — ~99.99% of it terminal I/O. **Printing per operation makes the print the workload.**

---

# Bugs Found and Fixed

Kept because each carries a transferable lesson, not for completeness.

**The sell-crossing bug.** The match loop used `incoming.price >= resting.price` for both sides. Correct for a buy; a sell needs the opposite comparison. The existing sell-aggressor test used *equal* prices, where both comparisons agree, so it structurally could not distinguish a side-aware implementation from one silently reusing buy-side logic. Caught by writing a test with a sell priced strictly *below* a resting bid, which correctly failed with zero fills where one was expected — confirming the diagnosis before the fix.

*Lesson:* an equal-value edge case is the **worst** place to test a directional comparison first, because it cannot tell a correct implementation from a wrong one that happens to agree at that boundary.

**The map-unification bugs.** Unifying the two side maps to one comparator introduced two real bugs, both caught by the existing suite within the same session. First, the match loop's post-fill cleanup still assumed `begin()` meant "best" unconditionally — true before the change, false after — so a sell aggressor's cleanup popped from the wrong end of the map. Second, `cancel`'s side lookup was `auto map = getMap(order.side);`, missing the `&` and silently copying the entire map, so cancel operated on a throwaway and the real book was never mutated.

*Lesson:* a refactor is safe to attempt not because it is risk-free but because a fast regression suite exists to catch what it breaks. **The bugs are the receipts, not a blemish.** Also: `auto` vs `auto&` at the call site of a reference-returning function is a recurring personal failure mode, and this same mistake appeared three times in one session.

**The impure shrinker oracle.** `invReplay` submitted a reference into the live sequence, and `submit` mutates an order's quantity as it fills — so after one replay the stored orders had modified quantities, and fully-filled orders sat at 0, which `validate` then rejects. The shrinker calls `invReplay` hundreds of times over overlapping subsets of one vector, so every replay after the first ran against a progressively degraded sequence, changing whether the failure reproduced and therefore the shrinker's keep/discard decisions.

*Lesson:* **a shrinker oracle must be pure** — same input, same verdict, every time. This plausibly explains earlier shrink results stalling above the true minimum, which had been attributed to the algorithm.

**The leftover reference.** The first queued fuzz reported 99,000 clean operations while executing zero cancels and zero modifies, because a bootstrap condition still tested a structure the new code no longer populates. Every individual piece was correct — the window worked, the branches were right, the roll was right. The bug was a **reference to a structure that is no longer maintained**, producing a run that looked entirely successful.

*Lesson:* covered above under *the recurring lesson*.

---

# Scope (v1)

### Included

- Matching core with limit and market orders, validation, cancel and modify under full fairness rules
- Property-based invariant testing with a proven shrinker
- Single-writer concurrency layer: producer-partitioned ids, bounded MPSC queue, two-phase drain
- Concurrent verification: queued fuzzing, determinism check, adversarial interleaving tests, ThreadSanitizer
- Intrusive linked list with a hand-rolled fixed-capacity order pool
- Latency percentile harness with a book-depth sweep, concurrent-path measurement, and a profile-driven optimisation loop

### Out of scope

- **Response path** — designed in full (per-producer SPSC, routed by id high bits), not built; demonstrates nothing the request path does not
- **Self-trade prevention** — requires a participant/account model the engine does not have. A plausible future extension, not a gap
- **Lock-free queue** — built and measured, then reverted. It fixed the lock-acquisition tail it targeted (p99.9 ~10,000 ns → 583 ns) but introduced millisecond-scale maxima. The rejection is empirical, not precautionary
- **Persistence / event log** — the ring buffer is transit, not storage
- **Risk, margin, pricing models, derivatives** — dropped, not deferred; the book does not need to know what it is trading

---

# Future Work

- **Reducing lock contention on the queue**, which the diagnosis identified as the entire remaining tail. The lock-free version fixed it and cost more elsewhere; a split-lock design or a different waiting strategy are untried
- A measured comparison against a **flat price array** — motivated by the depth curve, which shows cache misses from chasing tree nodes are real. Rejected for now because it requires **bounding the price range**, a domain constraint the design does not currently make and which would need justifying against a real venue's rules rather than assumed
- Producer-side pacing in the benchmark harness, so contention figures reflect realistic arrival rates rather than a tight push loop
- Horizontal sharding by instrument — one book, one writer per symbol — which is the only scaling path that does not cross the sequential constraint
- The response path, if a requirement for it appears
- A participant/account model, enabling genuine self-trade prevention

### Known limitation

`submit` discards `rest`'s return value. If the order pool were exhausted when a limit order's unmatched remainder tried to rest, the remainder would be dropped silently — the caller would receive its fills with no indication that part of the order never made it into the book. That is the silent-drop behaviour rejected everywhere else in the design, and it is a real hole in the rejection path.

It has never fired: the pool is sized well beyond the peak live order count in every test, and no run has ever exhausted it. But "has not happened" is not the same as "cannot happen", and the fix is to propagate the failure rather than swallow it. Recorded rather than quietly left.

---

## A note on what is claimed

Numbers here are labelled with what they actually measure. Percentiles are of batch means. Benchmarks are pure matching-logic cost on one machine — no network, no persistence, no end-to-end path. The contention sweep is a characteristic with a hypothesis, not a measurement. ThreadSanitizer is dynamic evidence, not proof. Where something is unexplained, it is recorded as unexplained.

That is deliberate. The project's argument is that its correctness is demonstrated, and a demonstrated claim loses its value the moment it sits next to an inflated one.
