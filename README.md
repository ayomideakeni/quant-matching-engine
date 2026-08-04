# Matching Engine

*A correct, single-threaded limit order book matching engine written in modern C++ using price-time priority.*

---

## What it does

This project implements the core component of an electronic exchange: a limit order book. The book stores buy and sell orders, matches incoming orders against resting orders on the opposite side, and maintains the remaining orders that are waiting to trade.

Matching follows **price-time priority**. Better prices always execute first, and when multiple orders exist at the same price, the oldest order is matched first (FIFO). When a trade occurs, it executes at the **resting order's price**, matching the behaviour of real-world exchanges.

### Currently implemented and tested

- Submit limit and market orders, with input validation at the door
- Order matching with price-time priority, partial fills across multiple price levels
- Cancel existing orders — O(1) by id, with automatic level cleanup
- Modify existing orders — full price-time-priority fairness rules, verified by consequence
- Property-based invariant testing under 100,000 operations of fuzzed order flow, with a working shrinker

### Planned / in progress

- Concurrency (single-writer-with-queue model)
- Rigorous, multi-workload benchmarking (an informal baseline exists — see *Benchmark preview* below)

See `devlog.md` for detailed, session-by-session build progress.

---

## Build & Run

```bash
mkdir build
cd build

cmake ..
cmake --build .

./tests
```

---

# Design Decisions

The goal of this project is correctness first. Every design choice favours maintaining the matching rules while keeping operations efficient.

## The `Order` type

**Choice**

Each order stores:

- a stable id and a separate, engine-assigned arrival sequence number (`seq`)
- side (`Buy` or `Sell`)
- order type (`Limit` or `Market`)
- price (stored as integer ticks)
- remaining quantity

**Why**

Prices are stored as integer ticks instead of floating-point numbers because floating-point values cannot represent most decimal values exactly. This is a correctness issue, not a performance one — modern floating-point arithmetic is not meaningfully slower than integer arithmetic. The actual problem is that two prices which *should* compare equal can silently fail to, since matching depends on exact price comparison. Integer ticks make comparison and arithmetic exact.

Arrival order is tracked with a monotonically increasing counter (`seq`), stamped by the engine itself at insertion time, rather than a timestamp. A counter is unique and strictly increasing **by construction** — it needs no assumption about clock resolution or monotonicity. A system clock offers neither guarantee: multiple orders can legitimately arrive within the same clock tick, and clocks can be adjusted backwards by the OS.

`id` and `seq` are deliberately separate fields, not one combined value. A caller needs a stable reference to *their own* order that they can use to cancel or modify it later — and at the moment of submission, they cannot know what arrival-number the engine will assign, since that depends on how many other orders have already arrived. Identity has to be independent of the system's internal arrival bookkeeping. Splitting the two also avoids a same-field ambiguity: `id` answers "which order is this," `seq` answers "when did it arrive, relative to everything else" — and only the second is ever used to resolve FIFO ties.

**Rejected**

Floating-point prices were rejected for the reason above.

Timestamps for arrival order were rejected because they don't guarantee uniqueness or monotonicity; a counter does, by construction.

A single combined id/seq field was rejected because it would tie a caller's stable reference to their order to a fact about the system's history that the caller has no way to predict or control at submission time.

---

## Order Book (per side)

**Choice**

Each side of the book uses a separate ordered map from price to price level — `bids` and `asks` are two distinct maps, both sharing the same (default, ascending) comparator. Best bid is read via `rbegin()` (highest price), best ask via `begin()` (lowest price).

**Why**

The two sides need opposite natural orderings — the best bid is the *highest* buy price, the best ask is the *lowest* sell price — and the map that stores each side should make that side's own ordering directly usable without a per-access filter. A single merged map would need a composite key (price + side) to disambiguate two structurally different concepts — a bid at 103 and an ask at 103 are never compared to each other for the purpose of finding the best price on either side, so nothing is gained by forcing them into one key space. Splitting them keeps `best()` — the most frequently called operation in the matcher — a simple, cheap, O(log n) lookup with no filtering.

**Rejected**

A single map keyed by price alone (regardless of side) was rejected: it would require a composite key or an internal filter to separate bids from asks at every access, and it would break the ability to bake each side's natural sort order directly into the container.

A hash map was rejected for the same reason as always: it has no notion of ordering, so finding the next-best price after a level empties would require extra bookkeeping or a full scan.

---

## Price Level

**Choice**

Each price level stores its orders in a `std::list`, in FIFO order. New orders are appended at the back; matching always removes or partially fills from the front.

**Why**

`std::list` gives the single strongest iterator-stability guarantee available: erasing or inserting anywhere in the list — including the middle — invalidates *only* the iterator to the element actually erased. Every other iterator, held anywhere else in the program, remains valid. This is exactly what a long-lived cancel index needs: it stores iterators that must stay dereferenceable no matter how many unrelated insertions and erasures happen elsewhere in the same list, for as long as the order they point to hasn't itself been removed.

**Rejected**

`std::vector` was rejected because it can reallocate when it grows — if the underlying buffer moves, every iterator and pointer into the old buffer dangles.

`std::deque` was rejected because, while it doesn't relocate elements the way a vector does, its iterator-invalidation rules on arbitrary insertion/erasure are weaker and less uniform than a list's — a real risk for a structure that needs to guarantee iterator stability specifically, not just reference stability.

The cost of this choice — worse cache locality and per-node memory overhead compared to a contiguous container — is accepted for now and revisited in the benchmarking phase, not before.

---

## Cancel Index

**Choice**

A hash map stores:

```
Order ID → stable iterator into a price level's list
```

The index stores only the iterator — no other fields.

**Why**

Cancelling an order should not require searching the book. The index locates an order in constant time; removal is then also constant time via the stored iterator, exactly because of the `std::list` guarantee above.

The index deliberately stores *only* the iterator, not a cached copy of the order's price or side. An order's own fields are the single source of truth; if the index cached its own copy of price/side and the order was later modified, the cached copy could silently disagree with the order's actual current state — a stale-cache bug with no natural trigger to catch it. Dereferencing the iterator live avoids that entirely: there is exactly one place this data lives, so it cannot go stale.

The iterator's validity and the id's meaningfulness expire at exactly the same moment — when the order is erased, both the list node and the index entry are removed together — so there is never a window where a "valid" id maps to a dangling iterator.

**Rejected**

`std::shared_ptr` was rejected because the index does not own the order — the level does. Shared ownership would add atomic reference-counting overhead on the hot path for no benefit.

Storing price and side alongside the iterator was rejected for the staleness reason above.

---

## The Match Loop

**Choice**

An incoming order matches against the resting orders on the *opposite* side while: it still has quantity remaining, the opposite side is non-empty, and it crosses the current best price there.

**Crossing condition**, stated precisely by the *incoming* order's side (not the resting side, which is always the opposite):
- Incoming **buy** crosses when `incoming.price >= resting(ask).price`.
- Incoming **sell** crosses when `incoming.price <= resting(bid).price` (equivalently, `resting.price >= incoming.price`).
- A **market** order always crosses, regardless of price — it has no price to compare.

**Trade quantity** is `min(incoming.quantity, resting.quantity)` — the amount that leaves the aggressor and the amount that leaves the resting order on any single trade must be identical, since it's one exchange, not two independent adjustments. Both sides are reduced by exactly this amount.

**Execution price** is always the *resting* order's price, never the aggressor's. The resting order is the one already established in the book; the aggressor has no price presence there to execute against. A buy at 105 hitting a resting ask at 102 trades at 102 — the aggressor is price-improved.

When a resting order's quantity reaches zero it is removed (via `cancel`, reusing the same side-agnostic removal logic rather than duplicating it inline); if that empties its price level, the level itself is removed from the map. After the loop, a limit order's remaining quantity rests in the book; a market order's remainder is dropped.

**Why**

This is price-time priority applied mechanically: better prices match first (by always reading the current best), and within a price, FIFO order is preserved by only ever matching from the front of a level's list.

**Rejected**

Using one crossing expression for both sides was tried and found wrong: at equal prices, `incoming.price >= resting.price` and `resting.price >= incoming.price` agree, which let a sell-side bug (using the buy-side comparison for sells) go undetected until a test was specifically written with unequal prices. See *Bugs found and fixed*, below.

---

## Modify — the Fairness Rule

**Choice**

Modifying a resting order's quantity **down** keeps its queue position — an in-place edit of the quantity field, nothing else moves. Increasing quantity, or changing price, **loses** queue position: the order is cancelled and resubmitted through the engine with the same id but a fresh, engine-assigned `seq`.

**Why**

Price-time priority exists to be fair to whoever committed first. Reducing quantity only gives something up — nobody queued behind you is harmed by you asking for less, so there is no reason to penalise it. But an *increase* in quantity is new commitment that only exists as of the moment of the modify — later than everyone who queued between the original submission and now. If the whole order (old quantity plus the addition) kept its old position, the newly added portion would unfairly execute ahead of orders that arrived earlier than the addition itself. Since an order can't be split into "the fairly-positioned original part" and "the newly-added part," the practical resolution is that the whole order re-queues. A price change is treated the same way: moving to a different level means joining that level's queue for the first time, with no claim to a position there.

The mechanism that actually enacts "lose position" is the fresh `seq` assigned when the cancelled order is resubmitted through the engine — that new, later sequence number is what places it behind everything already resting at its new position; a fresh seq is the observable proof that position was lost.

`quantity == 0` is checked first, before price/quantity routing, so a modify-to-zero always behaves as a cancel regardless of what else was requested in the same call.

**How it was tested — verify by consequence**

Rather than inspecting internal queue structure directly, modify's fairness rule was tested through its *observable effect*: fill order. Two orders were rested at the same price; one was modified (reduced, or increased); a matching order was then submitted, and the resulting fill sequence was asserted to show the correct order — reduced-quantity order still filling first (position kept), increased-quantity order now filling *after* the other (position lost). A blind spot was found and closed here: a matcher sized to consume only the *front* order can't distinguish "the tracked order moved to the back" from "the tracked order silently vanished" — both produce a single fill against the other order. The fix was sizing the matcher to sweep through the front order into the tracked one, so both fills emit and their relative order proves both presence and position simultaneously.

**Rejected**

Encoding a type change (limit → market) via a magic price value (e.g. price `0` meaning "now a market order") was considered and rejected — it overloads a legitimate price value with hidden meaning, and conflates the order's type with its price field for no real benefit. Type changes are out of scope for v1.

---

## Validation

**Choice**

Every submitted order is checked against three guards before it can enter the book: the id must not already exist in the cancel index; quantity must be greater than zero; and for limit orders specifically, price must be greater than zero.

**Why**

A duplicate id would silently overwrite the cancel index's existing entry for that id, orphaning the original order — reachable by nothing, cancellable by nothing. A zero-or-negative quantity order is meaningless input. A non-positive price on a limit order is meaningless, since a limit's entire function is to trade at a specific, named price.

Market orders are explicitly exempt from the price guard — not because zero has special meaning (that pattern was rejected for modify above, and the reasoning is the same here), but because a market order's crossing decision never consults price at all; type alone determines that it always crosses. Price is simply irrelevant input for a market order, not a signal.

`seq` receives no guard at all, because there is nothing to guard — it is never supplied by the caller, only stamped by the engine at insertion, so it cannot arrive malformed.

**Rejected**

No alternative rejection signal shapes were seriously considered beyond making `submit` return `std::optional<std::vector<Fill>>` — `nullopt` for rejection, a present (possibly empty) vector for a validated, matched order. This distinguishes "rejected before matching was attempted" from "validated and matched, with zero fills" for the first time in the project.

---

## Property-Based Testing — Invariants and the Fuzzer

**Choice**

Beyond the hand-authored replay test suite, four invariants are checked automatically after operations during fuzzed, randomised-but-valid order flow generated by a weighted `Generator`:

- **No crossed book** — best bid strictly below best ask, whenever both sides are non-empty. A resting-state structural check: it flags the state a matching failure can leave behind, not a proof that every match that should have executed, did.
- **Volume conservation** — on every trade, the aggressor's quantity decrease must equal the resting order's quantity decrease. This is the direct check that no units are silently created or destroyed during a match.
- **FIFO preserved** — consecutive orders within a level maintain non-decreasing `seq`.
- **No orphaned cancel-index entries** — every entry dereferences to a real order whose own id matches the key.

These are implemented as standalone, `const`, pure-observation methods — explicitly **detection, not enforcement**. They watch and report; they never intervene or auto-correct, since acting on a violation would hide the very bug the check exists to expose. (An earlier instinct to fold whole-book checks like these into `validate` was corrected: `validate` is a gatekeeping question about one incoming order, with no view of the book as a whole; these are structural claims about everything currently resting, and need a different home.)

**Why this layer exists at all**

Example-based tests only ever cover the scenarios the author thought to write. Property-based testing checks that something which must *always* be true stays true across flow the author never specifically imagined — moving from "does the outcome match what I foresaw" to "is the outcome verifiably correct regardless of the sequence that produced it." Order flow at any real venue is large and highly variable; not every state that could occur can be hand-architected in advance.

**The generator**

A `Generator` produces one operation at a time (submit / cancel / modify, weighted with modify heaviest, since it has the most branching logic and the highest historical bug rate), executes it directly against the live book, and tracks which ids are currently resting in a plain `vector<Id>` — deliberately *not* a second `id → iterator` map, which would have duplicated the cancel index and reintroduced exactly the dangling-handle fragility the design elsewhere avoids. Submitted order types are weighted ~90% limit / 10% market, since only limits rest, and an all-market flow would starve cancel and modify of real targets. Prices are drawn from a narrow band to deliberately maximise crossing pressure, pointing fuzzing effort at the invariants with an actual bug history (crossed-book, volume conservation) over the one already proven structurally safe by tracing every insertion path in the code (FIFO — see below).

**The run:** 100,000 operations, zero violations across all four invariants, confirmed by grepping the full run's output rather than eyeballing a log tail.

**FIFO's safety was proven, not assumed:** every code path that inserts into a level's list either appends at the end with a fresh seq (`restInto`, and modify's cancel-then-resubmit paths), or only ever mutates an existing node's quantity field in place without touching list position (modify's reduce path), or only ever removes from the front (the match loop). No path can reorder a list. Because of this, the check is cheap to run per-operation and kept mainly as regression insurance against a future change silently invalidating the guarantee — which is exactly what happened once already, to a different assumption, during the map unification below.

**The shrinker**

When the fuzzer's invariant checks fire, the exact sequence of operations that produced the failure is captured (a `LoggedOp` history), and a shrink loop reduces it: repeatedly try removing one operation, keep the removal if the failure still reproduces, restore it if the failure disappears, and repeat full passes until one entire pass removes nothing. Proven by deliberately reverting the sell-crossing fix (below), letting the fuzzer catch the reintroduced bug, and reducing the resulting 34-operation failing sequence to a stable 20 operations before reverting the injected bug.

A known, accepted limitation: one-at-a-time removal can stall above the true minimum when operations reference each other's ids across the sequence — removing an early submit doesn't crash a later cancel of that same id (cancel on an unknown id is already a clean no-op by design), but it silently changes the book's later trajectory, which can make an operation look load-bearing when it's only *referenced*, not *causally necessary*. A stable result under single-element removal is the correct goal for this algorithm; reaching the true global minimum would need group/chunk removal, judged out of scope for v1.

---

## Map Unification (a pre-W6 simplification)

**Choice**

`bids` and `asks` were originally different C++ types — `bids` used `std::greater<>` specifically so that `begin()` meant "best" on both sides, which forced the code that needed to operate generically on "whichever side" to be templated (`withOppositeSide`/`withGetSide`). Both maps were unified to the same (default) comparator, and the templates were replaced with two small named functions: `getMap(Side)` returning that side's own map, and `opposite(Side)` flipping buy/sell — composed explicitly at call sites (e.g. the match loop reads `getMap(opposite(incoming.side))`) rather than hidden inside a helper.

**Why it was safe to attempt**

Not because the refactor itself was risk-free — it wasn't — but because a 23-test regression suite already existed to catch mistakes immediately, and doing it before the fuzzer was built meant the fuzzer would stress-test the final, simplified code shape rather than a version about to change out from under it. No caller outside `OrderBook` — not the tests, not the fuzzer — needed to change at all, since the public interface (`submit`, `cancel`, `modify`, `best`) stayed identical; only the private implementation did.

**What it actually found — the receipts, not just the claim**

The refactor introduced two real bugs, both caught by the existing suite within the same session:

1. The match loop's post-fill cleanup still assumed `begin()` meant "best" unconditionally — true before the unification (the comparator trick made it universally true), false after (now `begin()` only means best for asks; bids' best is `rbegin()`). A sell aggressor's cleanup was popping/erasing from the wrong end of the map. Fixed by replacing the inline cleanup with a call to the existing, side-agnostic `cancel(restingId)` — with the id captured into a local *before* the call, since reading the resting order's fields *after* cancelling it would be a use-after-free.
2. `cancel`'s internal side lookup was written `auto map = getMap(order.side);` — missing the `&`, silently copying the entire map. `cancel` was operating on a throwaway copy; the real book was never mutated. The same class of mistake (a reference-returning function's result assigned to `auto` instead of `auto&`) occurred twice more while building the fuzzer's generator helpers during the same broader session — worth treating as a recurring personal failure mode to watch for specifically.

---

## Bugs Found and Fixed

Two are worth walking through in full, since both were genuinely invisible to the tests that existed at the time they were introduced.

**The sell-crossing bug.** The match loop's crossing check used `incoming.price >= resting.price` for both buy and sell aggressors. This is correct for a buy, but a sell should use the opposite comparison — `resting.price >= incoming.price`. The existing sell-aggressor test at the time used *equal* prices, where both comparisons happen to agree, so it could not distinguish a correctly side-aware implementation from one silently reusing the buy-side logic for sells. It was caught by deliberately writing a new test with a sell priced strictly *below* a resting bid — the case where the two comparisons diverge — which correctly failed with zero fills where one was expected, confirming the diagnosis before the fix. Lesson: an equal-value edge case can be the *worst* place to test a directional comparison first, since it structurally cannot tell a correct implementation from a wrong one that happens to agree at that boundary.

**The map-unification bugs.** Covered above — both are a direct product of one underlying lesson: a refactor is safe to attempt not because it's risk-free, but because a fast, reliable regression suite exists to catch what it breaks, immediately, before it goes any further.

---

## Testing

Two layers, both maintained continuously rather than deferred to the end.

**Layer 1 — replay harness (23 hand-authored tests).** Scripted order sequences with asserted exact fills and exact final book state: buy/sell aggressors, rest-remainder, market orders, empty-book, multi-level sweeps, exact-match boundaries, four cancel scenarios, five modify scenarios (all five fairness cases, verified by consequence), a dedicated unequal-price cross-comparison test, and three validation-guard tests.

**Layer 2 — property-based invariant testing.** See above: 100,000 fuzzed operations, four invariants, zero violations, plus a working shrinker proven against a deliberately injected known bug.

---

## Benchmark preview (informal — not the full W9 suite)

An early, single-threaded baseline was captured ahead of schedule while the tooling was fresh, isolating each operation rather than averaging across mixed flow:

| Operation | Median latency |
|---|---|
| Submit (resting only, no cross) | ~60 ns |
| Submit (always crosses) | ~40 ns |
| Cancel | ~30 ns |
| Modify (cancel + resubmit path) | ~62 ns |

Methodology: `-O3`, multiple trials with median reported, `std::steady_clock`, fresh book per trial. These numbers describe **pure matching-logic cost only** — no network I/O, no persistence, no concurrency, no contention, small fixed book depth, no sustained-run memory-pressure effects — and should not be read as an end-to-end latency claim. They're presented here as evidence the correctness-first data structures aren't paying a large hidden performance tax, not as the final benchmark result. The rigorous, multi-workload, book-size-swept version is W9's job.

---

## Scope (v1)

### Included

- Single-threaded matching engine
- Limit and market orders, with input validation
- Cancel and modify, with full price-time-priority fairness rules
- Property-based invariant testing with a working shrinker
- An informal single-threaded benchmark baseline

### Out of Scope

Intentionally deferred to later phases, or dropped outright:

- Multi-threaded / concurrent matching (next phase)
- Rigorous, multi-workload performance benchmarking (W9)
- Self-trade prevention — requires a participant/account model the engine doesn't currently have; a plausible future extension, not a gap
- Persistence / event log
- Risk, margin, pricing models, derivatives — dropped, not deferred; the book doesn't need to know what it's trading

---

## Future Work

- Single-writer concurrent architecture (queue-based hand-off to a matching thread)
- Rigorous benchmarking with realistic synthetic order flow, swept across book sizes
- Memory pools / allocator optimisation, and a measured comparison against a tombstone-vector level layout — deferred until there's a benchmark to justify either choice
- Engine-assigned (rather than caller-assigned) order ids, if fuzzing/benchmarking at higher volume needs uniqueness-by-construction
- A participant/account model, enabling genuine self-trade prevention
