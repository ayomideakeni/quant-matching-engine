# Dev Log — Matching Engine

*Session-by-session build progress.*

---

## Phase map (where each piece lives)

| Phase | Window | Focus |
|---|---|---|
| W3 | design | Domain trace, structure design, README seed |
| W4 | core | Order type → book skeleton → match loop → submit → cancel |
| W5 | subtle ops | modify, edge cases, validation |
| W6 | testing | property-based invariants, defence |
| W7 | concurrency (design) | primitives, model derivation, thread reps, queue design |
| W8 | concurrency (build) | MPSC queue, single-writer loop, adversarial tests, defence |

---

## Status board

| Component | State | Tests |
|---|---|---|
| `Order` type | ✅ done | defended (integer ticks, seq-not-clock, id-vs-seq) |
| Book skeleton (maps, `Level`, `best()`, `rest()`) | ✅ done | covered |
| Cancel index population | ✅ done | covered |
| Match loop | ✅ done | canonical buy/sell, market, rest-remainder, cross-comparison |
| `submit` + replay harness | ✅ done | 6 replay cases |
| `cancel` | ✅ done | 4 cancel cases |
| `modify` | ✅ done | 5 modify cases |
| Edge cases | ✅ done (I) | multi-level sweep, exact-match boundary; self-cross scoped out |
| `validate` / input rejection | ✅ done | 3 validation cases (dup-id, bad-qty, bad-price) |
| Map unification (pre-W6 cleanup) | ✅ done | full 23-test suite green |
| Property-based / invariant tests (W6·1–2) | ✅ done | 100k-op fuzzed run, 0 violations |
| Shrinker (W6·3) | ✅ done | proven against an injected known bug, 34→20 stable ops |
| Defence pass (W6·4) | ✅ done | README fully current — every decision defended cold |
| Concurrency primitives (W7·1) | ✅ done | gate met — sequentiality, shared mutable state, queue, false sharing |
| Model derivation + 3 rejects + id scheme (W7·2) | ✅ done | single-writer-with-queue; producer-partitioned ids |
| Thread reps + deadlock drill (W7·3) | ✅ done | race produced & fixed 2 ways; deadlock produced & fixed 2 ways |
| Queue design + depth dial (W7·4) | ✅ done | `Request` type, reject-on-full, response path scoped, MODEST dial |
| Producer id packing (1 sign / 7 producer / 56 counter) | ✅ done | round-trip verified across 3 producers |
| `Request` + `Request → LoggedOp` conversion | ✅ done | exercised by both integration tests |
| `RingBuffer` (bounded MPSC, mutex + condvar) | ✅ done | 5 tests inc. 4-thread concurrent run, 30 consecutive clean |
| `writerLoop` (single writer, 2-phase drain) | ✅ done | 2 integration tests — submit, cancel, modify, matching |
| Fuzz through the queue + determinism check (W8·2) | ⬜ next | — |
| Concurrency debug/harden (W8·3) | ⬜ | — |
| Concurrency defence + cold rep (W8·4) | ⬜ | — |

**Status: PHASE 2 COMPLETE · W7 COMPLETE (design) · PHASE 3 IMPLEMENTATION IN PROGRESS.** W4 core + W5 (modify, edge cases I, validation) + map unification + W6 (property-based fuzzer, shrinker, defence pass) — 23 hand-written tests, 100k fuzzed operations across 4 invariants with zero violations, a proven shrinker, and a README that defends every structural decision cold. **W7 added the full concurrency specification with zero engine code touched: primitives derived from this engine's own hot path, the single-writer-with-queue model derived and three alternatives rejected on three different grounds, producer-partitioned ids decided, thread and deadlock drills run on throwaways, and the queue element type, overflow policy, response-path scope and depth dial all decided in writing. W8·1 shipped the first code since W6: producer-side bit-packed ids, the `Request` type, a bounded mutex+condvar MPSC ring buffer, and the single-writer loop — all outside `OrderBook`, which is byte-identical. Orders now flow producer → queue → writer → book, verified by five ring-buffer tests (including a four-thread concurrent run, thirty consecutive clean) and two integration tests covering submit, cancel, modify and matching through the queue. The 23-test suite, the 100k-operation fuzzer and the shrinker all pass untouched — the regression proof that the matching core was not modified.**

---

## Session entries
### W8·1 — MPSC queue + single-writer loop (COMPLETE)

First session to change the repo since W6. Everything built here is **new code outside the book** — `submit`, `cancel`, `modify` and the match loop are byte-identical, and the full existing suite (23 tests + 100k-op fuzzer + shrinker) passes untouched. That is the gate, and it held.

---

**Producer id construction (the deferred W7·2 bit-packing).** `producer` struct — a `producerId` plus a plain **non-atomic** `int64_t counter`, with `nextId()` packing and post-incrementing. Named constants (`producerBits`, `counterBits`, `producerShift`, `maxProducers`) rather than magic 56s and 255s.

- **Layout: 1 sign bit clear · 7 bits producer · 56 bits counter.** The producer field was cut from 8 bits to 7 for a specific reason: a field of width *w* at shift *s* occupies bits *s* through *s+w−1*, so an 8-bit field at shift 56 reaches **bit 63 — the sign bit**. Shifting into the sign bit of a signed type is UB, and even where it "works" you get a negative id that surprises everything assuming ids are positive. 7 bits keeps the top bit permanently clear and still gives 128 producers, far past any realistic need. (`uint64_t` would recover the bit and allow 256 producers — rejected because `Id` is already `int64_t` throughout `Order`, `cancelIndex`, `validate` and every test, and one unused bit is not worth that churn.)
- **`static_cast` before the shift, not after.** `producerId` is an `int`; shifting a 32-bit value left by 56 overflows the `int` and is UB *before* the result would ever be widened. Widen first, then shift.
- **Why OR works, and what it depends on.** The shifted producer is all zeros in the low 56 bits and the counter is all zeros in the high bits, so at every bit position at most one operand has a 1 — the fields **never contend** and OR merges them cleanly. That disjointness is the whole mechanism: exceed the counter's 56 bits and it would bleed into the producer field silently, and producer 3's ids would start decoding as producer 4's. 2^56 ≈ 7×10^16 ids per producer — at 1 M orders/sec, over two thousand years to exhaust. `|` over `+` because for disjoint fields they're identical, but `+` **carries** if the fields ever do overlap (corrupting the high field too) while `|` keeps the damage local — and `|` documents intent: independent fields merged, not numbers summed.
- **Modular partitioning (`id % N == p`) rejected**, having been considered properly: it is equally collision-free (every integer has exactly one remainder mod N), but **N is baked into every id** — add a fifth producer and every id already issued decodes to the wrong producer, so the scheme isn't stable under growth. Extraction is also integer division (~20–40 cycles) against one shift.
- **Verified by round-trip:** three producers, ten ids each. Blocks start at 2^56, 2×2^56, 3×2^56 — three disjoint regions — counters increment in the low bits with the high portion untouched, every id decodes to its issuer, nothing negative.

---

**`Request` — the queue element.** Flat tagged struct: `OpType` tag, `Order`, `Id`, `optional<Price>`, `optional<Quantity>`. Aggregate (no constructor), brace-initialised.

- **`id` is a plain `Id`, not an optional** — cancel and modify *always* have one, so optional would mean "may legitimately be absent," which is false, and every use site would unwrap something always present. Contrast `newPrice`/`newQuantity`, where absent genuinely *means* "leave unchanged."
- **A `variant` was the more principled design and was deliberately traded away.** A variant makes irrelevant fields *unrepresentable*; the flat struct makes it a convention the tag enforces. Accepted cost: cancel and modify carry a meaningless zeroed `Order`, and `Order{}` is used rather than plausible-looking values specifically so it fails loudly if ever read. Documented in a comment, since the type can no longer say it.
- **`Request → LoggedOp` conversion turned out to be one line.** The two structs are currently field-identical, so the conversion is a straight copy with nothing to dispatch on — a branching version was written, found to be doing nothing, and deleted. **The value was never in the function; it is in the types being separate.** The moment W9 adds a timestamp to `Request` for latency measurement they diverge, and the copy is the price of that independence. Honest state: trivial today, load-bearing later.

---

**`RingBuffer` — bounded MPSC queue, mutex + condvar (the Modest dial).** Fixed-capacity `vector<Request>` allocated once at construction, `head`/`tail`/`count`, one `std::mutex`, one `std::condition_variable`, a `stopping` flag.

- **A class, not a public struct** — `head`, `tail`, `count` and the storage are invariants that must stay consistent, so reaching in from outside would move correctness from the type to every caller. Deliberately the opposite call from `Order` (public struct, pure data, no invariants of its own): same reasoning, different answer.
- **Full vs empty resolved with a count.** With head and tail alone, empty and full are *both* `head == tail` — identical state, opposite meaning. Options were sacrificing a slot (tail never catches head) or a separate count. Count chosen: it uses every slot and expresses the condvar predicate directly ("is there anything to pop" is a direct read). Accepted cost is a third piece of state that must be updated on every push and pop or it drifts.
- **Built and proven single-threaded first, mutex added after** — deliberately, so wrap-around arithmetic and concurrency weren't being debugged simultaneously. Four tests: fills-and-refuses, FIFO-order-out, drains-and-refuses, and the wrap test (push/pop twelve times so both indices wrap three times). **The first three pass with completely broken wrap arithmetic** — on a fresh queue you never reach the end of the array — so the wrap test is the only one that proves the modulo.
- **The mutex wraps the whole method body, not individual lines.** `push` is four steps (check full → write → advance tail → increment count) and the invariant spans all four. Two producers with one slot left would both read not-full, both write to **the same slot** (neither has advanced tail yet), then advance tail twice for one item and double-increment the count — the lost-update race corrupting a data structure rather than a counter. Exactly the atomic-vs-mutex distinction from the W7·3 account drill: an atomic makes one variable's operation indivisible, a mutex makes a region exclusive.
- **Concurrent test:** four producer threads × 5,000 pushes into an oversized queue, drained on the main thread after joining. Checks total popped equals total pushed, every id decodes to a valid producer, and — the check that actually detects corruption — **each producer's ids come out in ascending order relative to each other**. Global order is meaningless (that's the point), but one thread pushed its own items one at a time, so a torn slot, a skipped slot or a duplicate would break that per-producer sequence. Thirty consecutive runs clean.
- **Stated honestly: thirty clean runs is evidence, not proof.** What makes this trustworthy is that the reasoning is simple enough to verify by inspection — one lock, taken on every path, held across the whole invariant. The test corroborates; the design convinces. That asymmetry is exactly the W7·4 argument for rejecting lock-free, where the reasoning *isn't* inspectable and the testing can't close the gap.

---

**The condition variable, and the two things it does at once.** `wait(lock, predicate)` puts the thread to sleep **and atomically releases the mutex while it sleeps**, reacquiring on wake. Both halves are essential: without the release, a writer that slept holding the lock would block every producer, so nothing could ever make the queue non-empty and nothing could wake it. That deadlock is why a condvar can't be built from a mutex and a flag.

- **Predicate form, never bare `wait()`** — threads can wake with **no notification at all** (spurious wakeups), so a bare wait means popping an empty queue. The predicate form re-checks on every wake and goes back to sleep if false.
- **Predicate is `count > 0 || stopping`** — two reasons to stop waiting. Without the `stopping` half, setting the shutdown flag would never wake a sleeping writer and shutdown would hang.
- **`push` must NOT wait — a real bug, caught and fixed.** An early draft put the same `wait` in `push`, which deadlocks on the very first call: the first producer to arrive at an empty queue evaluates `count > 0 || stopping`, finds both false, and **sleeps waiting for an item only it could have supplied**. Nothing else pushes, so nothing notifies.
- **The distinction that resolves it — two kinds of waiting.** Producers *do* take the mutex: brief, bounded, nanoseconds, and guaranteed to be released because the holder can't do anything long. What "producers never block" meant in W7·4 is **overflow waiting** — sleeping until a slot frees, which is unbounded. A full queue returns `false` immediately. So the mutex is shared; only the writer ever sleeps on the condvar.

---

**Shutdown — two-phase drain.** `shutdown()` takes the lock, sets `stopping`, and notifies. The writer then keeps processing until the queue is empty and only then exits. `push` refuses once `stopping` is set, so the backlog is finite and the drain terminates.

- **Drain rather than discard, and the argument is the overflow policy's.** Reject-on-full exists so that **acceptance means something** — a producer holding `true` knows its request is in the system. If shutdown discarded accepted requests, `true` would silently stop meaning that, which is precisely the "drop" behaviour rejected as indefensible at W7·4. Draining preserves a clean, statable guarantee: **accepted implies executed.**
- **"Finish the in-flight action" needs no design** — the writer only checks the flag when it comes back around to `wait`, and a flag cannot preempt a function call, so an in-flight `submit` always completes.
- **"Keep queued requests" was considered and discarded on inspection** — the ring buffer is *transit*, not storage; there is nowhere to keep them. Persisting across restarts is a durability subsystem (out of scope, same category as the response path).
- **`notify_all` rather than `notify_one` on shutdown** — with exactly one writer they're equivalent, but `notify_one` would leave a second consumer asleep forever if one ever existed. Cheap insurance on a path that runs once.

---

**Two pops, deliberately, because `nullopt` means two different things.**
- **`pop()` — non-blocking.** `nullopt` = "nothing right now." Used for draining in tests.
- **`waitAndPop()` — blocking.** `nullopt` = **"stopping, and nothing left — exit."** Since it sleeps rather than returning when the queue is merely empty, empty-after-wake can only mean shutdown.

Two behaviours deserve two names; one overloaded name is how a caller waits when it meant to poll. The shared four-step take was extracted into a private `takeRequestLocked()` (assumes lock held and non-empty) so the wrap arithmetic exists once.

---

**`writerLoop` — the single writer.** Free function at file scope: `waitAndPop` → `break` on `nullopt` → dispatch on the tag to the **unchanged** `submit`/`cancel`/`modify`. Return values are discarded with a comment, since v1 has no response path.

- **`seq` is NOT stamped here — corrected mid-session.** The plan assumed the writer would assign it at pop time; reading the actual code showed `rest()` already does `o.seq = nextSeq++`, with `OrderBook` owning `nextSeq`. **`rest`'s placement is the better one:** `seq` orders orders *within a level's queue*, and `rest` is exactly the moment an order enters one, so numbers are issued when they're used rather than being burned on market orders that fully fill, cancels, modifies and rejected submits. It is also why `modify`'s reposition works — it routes through `cancel` then `submit` → `rest` and picks up a fresh, later `seq` automatically.
- **The interview point survives, relocated.** The single definition of arrival order is preserved not because the writer stamps a number, but because **only one thread ever calls `rest`**, so `nextSeq++` — a plain non-atomic increment — is only ever executed by the writer. "How is `seq` assignment thread-safe?" answers: it doesn't need to be.

---

**A layering mistake worth recording.** `Request`, `RingBuffer` and `writerLoop` were all initially written **inside the `OrderBook` class** (hence `OrderBook::Request` in the tests, and a compile error when `std::thread` was handed what turned out to be a non-static member function). Moved to file scope after the class.

The argument is not stylistic: **the book must not know that queues or threads exist.** It is a passive data structure the writer drives from outside, and that separation is what makes "matching logic untouched" true *structurally* rather than by accident. A queue nested inside the book means the book's own definition includes threading machinery. `writerLoop` in particular takes both a `RingBuffer&` and an `OrderBook&` — a member wouldn't need to be handed its own object, which was the giveaway.

---

**Integration tests, and a false pass that only a debug print revealed.** Stage 1 pushed three submits and asserted one level — and passed while proving almost nothing: **all three requests reused `id1`**, because `nextId()` had been called once. Requests two and three were rejected by `validate` as duplicate ids, and the assert on level 100 passed purely because the *first* order rested. Only the printed ids (all identical) exposed it. Same shape as W6's two false-clean fuzz runs: **a passing test is not evidence unless you have confirmed what was actually exercised.**

Fixed by distinct ids per request and asserting *every* level — which is also the argument for the harness, since a helper checking all listed levels by default would have caught it immediately rather than leaving it to a print.

- **`RingBufferIntegration(name, requests, expectedLevels)`** added to the `Test` class: constructs a fresh book and queue, starts the writer, pushes each request, `shutdown()`, `join()`, then compares. **The thread lifecycle is written once**, so the order-dependent part (join producers *before* shutdown; join the writer *before* touching the book) can't be got wrong in a later test.
- **Id assignment was initially inside the harness and had to move out.** Stamping a fresh id per request works for submits and silently breaks cancel and modify, which need the id of an *existing* order — the harness cannot know which order a cancel targets. The caller owns the ids; the harness pushes what it is given.
- **`checkStates` rewritten** to build actual `ExpectedLevel`s from the book and compare structurally rather than comparing bare ints.
- **Stage 2 sequence** exercises every tag and real matching in one run: three resting buys (100/99/98), a **cancel** of the 99 order, a **modify** of the 98 order to quantity 10 (a reduce — keeps position), then a **sell** crossing the 100 level. Expected: 100→40, 99→0, 98→10. Green.
- **Stated limitation:** `quantityAt` returns 0 for an absent price, so 99→0 proves the quantity went to zero, not that the level was *erased* — a ghost level would pass. Acceptable because cancel's erasure is already proven by the four W4·5 cancel tests with a live-probe order; this test is checking that the *request reached cancel through the queue*, a different layer. Airtight would need `contains(id)`, which would mean exposing the book from the harness.
- **Fills are not observable through this path at all** — `writerLoop` discards them and there is no response path, so state assertions are the only instrument. A real and expected consequence of the W7·4 scope decision.

---

**Also this session:** capacity constrained to a power of two and `% capacity` replaced with `& (capacity - 1)` — identical results for powers of two (a power of two minus one is a mask of all-ones in exactly the low bits), avoiding integer division on the hot path. Done *after* the tests were green, so a failure would be attributable to the bit trick rather than to the wrapping logic.

**Gate: MET.** Orders flow producer → queue → writer → fills; matching code diff-provably unchanged; 23 tests, 100k fuzz, shrinker and all five ring-buffer tests green.

**Deferred to W8·2:** fuzzing *through* the queue — the existing `Generator` feeding requests rather than executing directly. Note the restructure this forces: single-threaded, the generator executes and checks invariants after every step; behind the queue nothing can inspect the book mid-flight (which is the design working, not a limitation), so invariant checks move to the writer side or become end-of-run. This also unlocks the **determinism check** deferred since W4·4 — capture the request stream the writer consumed, convert via `Request → LoggedOp`, replay single-threaded through a fresh book, assert identical results. Plus **stage 3**: multiple producer threads pushing while the writer consumes, which is the real configuration and the only one not yet exercised (everything so far pushed from `main`, so the queue has never been contended and drained simultaneously).

**Cards harvested:** bit-packing — shifts, masks, `(1<<n)-1`, why OR needs disjoint fields, the signed-shift and widen-before-shift hazards · power-of-two modulo as a mask · condition variables — atomic release-on-wait, predicate form, spurious wakeups · `unique_lock` vs `lock_guard` (why `wait` needs the former) · ring buffer full-vs-empty ambiguity and the two resolutions · aggregate initialisation and why a class with no user-declared constructor takes braces not parens · `std::optional` is not formattable by `std::format`/`println`, and why the library refuses to guess.

### W7·4 — Queue design + depth dial (COMPLETE — W7 COMPLETE)

Last design session before code. No code written; matching logic untouched. Four decisions: the request type, the overflow policy, the response path, and the depth dial.

---

**Decision 1 — request type: `Request` and `LoggedOp` stay separate, with a one-way `Request → LoggedOp` conversion.**

The producer can't hand over an `Order` — that covers submit only; cancel needs an `Id`, modify needs an `Id` plus two optionals. Three shapes, one queue. **This problem is already solved in this codebase:** `LoggedOp` (W6·3) is exactly that tagged struct, and `invReplay` is already a working consumer loop over a sequence of them.

- **Reuse rejected:** `LoggedOp` is a *test artifact* recording what happened; `Request` is a *production message* describing what a producer wants. They look alike today and are different concepts. Fusing them means every future change to either drags the other along — adding a timestamp for W9 latency measurement would grow the test replay type a field it has no use for.
- **Fully-separate-with-no-conversion rejected:** the W8·2 determinism check would then need its own replay implementation, duplicating logic already built and proven. Two implementations that can silently drift apart is worse than a coupling.
- **Conversion chosen** because it buys the W6 machinery for free: capture `vector<Request>`, convert, hand to `invReplay` — no second replay engine, and **the shrinker comes along too**, so a failing concurrent run can be minimised by the tool already proven against an injected bug. Cost is one function plus the rule that a field added to either type means revisiting it — a *visible* maintenance point (one function that compiles or doesn't) rather than an invisible coupling spread across the codebase. Same principle as W7·2's queue argument: don't eliminate the coupling, **concentrate it into the smallest, most inspectable surface**.
- **One direction only.** `Request → LoggedOp`. A test artifact never needs to become a production message.
- **Size note for the ring buffer:** the struct is as large as its biggest variant — a full `Order` plus the tag, ~40–48 bytes — so a bounded queue costs `capacity × sizeof(Request)` up front (a 65,536-slot queue ≈ 3 MB). Real, not a rounding error. A variant-based design could shrink it; not judged worth the complexity.

---

**Decision 2 — overflow policy: bounded queue, reject on full. `push` returns `bool` and never blocks.**

**Rationale — fault isolation.** A full queue refuses *one request* rather than stalling a producer and everything queued behind it. Blocking couples every subsequent request to one request's fate — head-of-line blocking — which is the same failure shape that makes one-big-lock bad: don't let one thing's contention become everything's contention.

**On the fairness objection (raised and resolved):** rejection at capacity is **a capacity report, not a fairness intervention**. Nothing is reordered, nothing is favoured, no request jumps another. Price-time priority governs orders that got in; it says nothing about a physically full queue. Blocking is arguably the *more* interventionist option, since it silently changes when everything behind it arrives.

**Also:** rejection is explicit and attributable — the producer holds the exact failed request and can retry, report, or escalate. This is categorically different from a drop, which is silent and unattributable. And it's simpler: no condition variable on the producer path, no waiting, no wake-up logic.

**Rejected:**
- **Block the producer** — nothing is lost and backpressure propagates cleanly (see below), but it stalls the producer thread and everything behind it, including traffic for unrelated orders and clients. Also cuts badly against the **cancel asymmetry**: a blocked *submit* can delay a risk-reducing *cancel* queued behind it, which is the operation you least want to lose in a moving market. Remains a legitimate choice in systems where upstream backpressure is the goal.
- **Drop newest** — silent loss; an order the client believes was sent simply vanishes. Indefensible for orders.
- **Drop oldest** — correct in market-data feeds, where stale ticks are worthless and only the latest matters; **precisely wrong here**, since the oldest queued requests have the strongest claim under price-time priority. Worth naming explicitly as a case that is right in a neighbouring domain and wrong in this one.

**Backpressure, understood properly (concept established this session).** Backpressure is a **chain of finite buffers**: client → network → kernel socket receive buffer → producer thread → queue → writer. When the last fills, fullness propagates backwards until it reaches something that can't push back — **and that boundary is where loss actually happens.** Under TCP the propagation is clean: as the kernel receive buffer fills, the advertised **receive window** shrinks, and at zero the sender's TCP stack stops transmitting, so the client's own `send()` blocks. No packets dropped, and the pressure reaches the client's application code. (Under **UDP** there is no window mechanism — a full receive buffer means the kernel silently discards datagrams, which is one reason multicast market-data feeds are designed around dropping stale data.) **General principle: blocking doesn't remove loss from a system, it relocates loss to whichever boundary can't propagate backpressure.** Knowing where that boundary is, is the skill.

---

**Decision 3 — response path: designed, scoped out of v1, built at W8·3 only if slip allows.**

Rejection returns **synchronously** (`push -> bool`), so the overflow policy is complete on its own — no queue, no asynchrony, no scope risk. That part ships in W8·1 regardless.

**What a full response path carries:** rejections (one per request, immediate, terminal), acknowledgements (accepted and resting), and **fills** — zero to many, arriving arbitrarily later, since a single resting order can fill repeatedly over its life. That last is the design pressure: a rejection is a direct reply, a fill is an asynchronous event long after the request completed.

**Direction and structure:** only the writer knows outcomes, so this path is **one producer (the writer), many consumers (the producers)** — the mirror of the request queue, and a genuinely different problem from MPSC.

**Design on record — one SPSC queue per producer.** Routing is free: the **bit-packed ids from W7·2 are self-describing**, so `id >> 56` recovers the destination producer with no lookup table (a property designed for debuggability that turns out to solve routing). Each per-producer queue is then single-producer/single-consumer — **the simplest concurrent structure that exists** — so no SPMC machinery is needed anywhere. Costs N queues of memory.

- **Rejected — one shared SPMC queue:** every producer wakes for every response and filters out what isn't theirs; contention from all readers on one structure. Same fault-isolation argument as the overflow decision.
- **Rejected — callbacks:** they execute **on the writer thread**, so a slow producer callback directly stalls matching. Categorically unacceptable in a single-writer design.

**Why scoped out:** it is the same concurrency lesson at *lower* difficulty than the MPSC request queue — an SPSC queue plus a shift-and-index — so it costs real time and adds no interview signal the request path doesn't already provide. Same test applied to FIX dual ids at W7·2 and self-cross at W5·2: scope must be justified by a requirement, not by completeness. The defence-pass answer is the design above plus the reason, not "I didn't do responses."

---

**Decision 4 — depth dial: MODEST. Mutex + condition variable MPSC ring buffer.**

Producers never block (rejection on full), so the condition variable is **writer-side only**: the writer sleeps when the queue is empty and wakes on notify. New concept to build carefully — **spurious wakeups**: a waiting thread can wake with no notification, so the wait must always be on a predicate (`cv.wait(lock, pred)`), never bare.

**Lock-free ring buffer rejected — and the decisive argument is verifiability, not difficulty.** A too-weak memory ordering is a **silent** failure: no crash, no assertion, correct on the overwhelming majority of runs, failing non-deterministically and differently across architectures (x86's strong model hides orderings that ARM exposes) and optimisation levels. This project's entire testing apparatus — 23 tests, a 100k-operation fuzzer, four invariants, a proven shrinker — is built on invariant checks over an executed operation stream, and a memory-ordering bug can corrupt the queue in ways that yield a plausible-but-wrong stream. **It is precisely the one bug class this infrastructure is structurally blind to.** On ARM (this project's only development machine) such bugs are *exposed non-deterministically*, which is not the same as *detected* — there is no verification path available here at all.

So the reasoning is coherence, not effort: **adding unverifiable work to a project whose entire credibility rests on demonstrated correctness is negative value.** An unverifiable claim is worse than an absent one, because in an interview it must be defended and can't be. Same principle as the CV corrections and the benchmark caveats.

**Understood at concept level and deliberately not shipped** — CAS, ABA, acquire/release pairing, memory reclamation (all derived at W7·2). The rejection is strong *because* the thing rejected can be described in detail; that is what separates judgement from avoidance. Upgrade path stays open if Phase 3 finishes with slip.

**Consequence:** the `LockFreeStack`/CAS drill does **not** fire — it was conditional on taking the Deep dial.

---

**Gate: MET.** Queue interface designed; overflow policy stated; response path decided; depth dial decided in writing.

**W7 COMPLETE.** Primitives (W7·1), model derivation and the three rejects (W7·2), thread reps and the deadlock drill (W7·3), queue design and depth dial (W7·4). No engine code touched in the entire window — the concurrency layer is fully specified before a line of it is written. **W8·1 — implementation — opens next.**

**Cards harvested:** condition variables and spurious wakeups (predicate-form wait) · backpressure as a chain of finite buffers, and the boundary that can't propagate it is where loss lands · TCP receive window as built-in backpressure vs UDP's silent datagram drop · head-of-line blocking / fault isolation as a design principle · SPSC / MPSC / SPMC as distinct problems with distinct difficulty.

---

### W7·3 — Thread reps + deadlock drill (COMPLETE)

First Phase 3 session that compiles code. All of it throwaway, in a scratch directory outside the repo — nothing here touches the engine. Purpose: *produce* the failures rather than reason about them, so the W7·1 conclusions rest on something observed.

**Rep 1 — thread mechanics and the scheduler.** Two threads printing in a loop. Four consecutive runs of the same binary produced clean, ordered output; the fifth produced `Thread Thread 1 is running.2` — output interleaved **mid-line**, one thread preempted partway through its `<<` chain. `std::cout` guarantees its own internal state won't corrupt, but makes no promise that a chain of `<<` calls stays contiguous; it is several operations, not one. **The unsafety was present in all six runs — only the observation changed.** That is the defining property of concurrency bugs: a sequential bug is a function of input, a concurrency bug is a function of input *and* a scheduling decision that is neither visible nor controllable.

**`join` and why the destructor terminates.** Commenting out both `join()` calls gave `libc++abi: terminating` and an abort — sometimes before any thread output, sometimes after one line, sometimes after two. A `std::thread` object is a *handle*; destroying it does not stop the OS thread. If the handle is destroyed while still **joinable**, the standard calls `std::terminate()`. The reasoning is that neither silent alternative is safe: implicitly detaching would leave a thread running against locals in the scope currently being destroyed (a silent use-after-free in code that looks correct), and implicitly joining would mean a destructor that blocks indefinitely and invisibly, possibly during exception unwinding. **So rather than guess intent, the language forces it to be stated** — `join()` or `detach()`, and saying nothing kills the program. Worth contrasting with use-after-free, where C++ happily hands back garbage: the difference is diagnosability, and a dangling thread reference is essentially undebuggable. `std::jthread` (C++20) joins in its destructor; `std::thread` keeps the old behaviour for compatibility.

**Rep 2 — the race, reproduced.** One shared `int`, two threads, 100,000 `++counter` each. Expected 200,000; observed ~100,000–150,000, **different every run, and the two threads printing different values from each other**. Mechanism: CPUs compute on registers, so `++counter` is *load → add → store*, with two gaps in which another core can act. Both threads load 500, both compute 501, both store 501 — two increments executed, counter advanced by one. The **lost update**.

The important detail is that the numbers clustered plausibly rather than looking like garbage: most iterations don't collide, and each collision costs exactly one increment, so the result is a large but partial loss. **If the expected answer weren't known, nothing about 132,847 announces itself as wrong.** Concurrency corrupts quietly and partially. And the standard's position is stronger than "you might lose increments" — a **data race** (two threads, same location, at least one writing, no synchronisation) is **undefined behaviour**, so the compiler may hoist the counter into a register for the whole loop or reorder freely. "It usually works" is not a description of that program's behaviour, because it has none.

**API frictions worth keeping (both are design choices, not quirks):** `std::thread` **copies its arguments by default**, even into reference parameters — because a thread may outlive the scope that created the argument, so copying is the conservative default and `std::ref` is the caller asserting the lifetime is fine. Passing a bare `1` to an `int&` parameter produced a wall of template noise whose real content was one line naming this file. **`std::mutex` is non-copyable** — deliberately, and conceptually rather than as a technicality: a mutex's whole function is to be a *single shared coordination point*, so per-thread copies would each lock privately, every acquisition would succeed instantly, no thread would exclude any other, and the code would **look** synchronised while providing zero protection. The standard deletes the copy constructor so that mistake is a compile error rather than a silent one — same philosophy as the join-terminate.

**Fix A — mutex.** One `std::mutex` shared by reference, `std::lock_guard` scoped tightly around the increment with explicit inner braces. Exactly 200,000, every run, deterministic. `lock_guard` is RAII: constructor locks, destructor unlocks, so the lock cannot leak through an early return, a `break`, or an exception — **the scope *is* the critical section**, which is why the braces matter (without them the guard would live for the whole loop body and hold the lock across everything else in it). Note the guarantee's real shape: a mutex protects nothing against code that doesn't take it — **the mutex doesn't guard the data, the discipline of always taking it does.**

**Fix B — atomic.** `std::atomic<int>`, loop body left as a bare `++counter`. The *type* changed, not the code: the compiler emits a single hardware read-modify-write with no gap to interleave into. Also 200,000, every run.

**Timing attempt, and the more useful lesson.** `time ./threads` gave mutex 0.014 / 0.013 s and atomic 0.047 / 0.013 s — the atomic version's two runs differing from *each other* by 3.6×, a spread larger than any effect being measured. Correctly read as **the measurement being useless, not the two approaches being equal**: 200,000 increments is microseconds, so this mostly timed process startup and dynamic linking; no warm-up (the 0.047 is almost certainly a cold run); two samples; and `time`'s resolution sits in the same range as the workload. **A single number with no distribution is not a result.** This is W9 methodology arriving unplanned, and it is the same critique due to be run against this project's own `benchmark.cpp` at W9·1. From the mechanics rather than these numbers: for one integer increment atomic normally wins substantially, but the gap only surfaces under real contention with enough iterations to swamp fixed costs.

**Atomic vs mutex — the actual distinction, and it isn't speed.** An atomic makes **one variable's operation** indivisible; a mutex makes **an arbitrary region of code** exclusive. Make both account balances atomic and each line is individually safe, but between them an observer sees money in neither account: **the invariant spans two locations, so no per-variable primitive can protect it.** Atomics for a single variable's operation; mutexes when several things must change together.

**The deadlock drill.** `struct account { int balance; std::mutex m; }`, and a `transfer` holding **both** locks simultaneously — releasing the first before taking the second would open a window where the money exists in neither account and the conserved-total invariant is briefly false. That requirement is precisely what creates the hazard. Two threads looping 100,000 transfers in opposite directions (`transfer(a,b)` and `transfer(b,a)`) **hung on the first run**.

The interleaving: T1 acquires `a.m` → T2 acquires `b.m` → T1 requests `b.m` and blocks → T2 requests `a.m` and blocks. Each holds what the other needs, and a `lock_guard` releases only at scope exit, which neither thread can reach. **It hangs rather than crashing** — no exception, no stack trace, zero CPU, both threads parked. In production that is a silently hung service, often harder to diagnose than a crash.

**The four Coffman conditions** (all four must hold simultaneously, so breaking any one prevents deadlock): mutual exclusion · hold-and-wait · no preemption · circular wait. Mutexes supply 1 and 3 by definition, so practical fixes attack 2 or 4.

- **Fix A — lock ordering, breaks *circular wait*.** Compare the two accounts' **addresses**, swap the pointers so the lower-addressed mutex is always taken first, then lock in that order. Concretely: with `a` at 0x1000 and `b` at 0x2000, `transfer(a,b)` doesn't swap and `transfer(b,a)` does — so **both** threads lock a then b despite running opposite transfers. The swap **decouples lock-acquisition order from transfer direction**; the business logic is untouched, since the mutexes are gates and don't care which account is debited. The specific order is arbitrary and addresses carry no meaning — any total order every thread computes identically would do. **Consistency is the entire requirement**, because a cycle requires some thread to acquire "backwards," and if nobody ever does, no cycle can form.
- **Fix B — `std::scoped_lock`, breaks *hold-and-wait*.** One line, both mutexes. Internally `std::lock`: attempt to acquire all, and on any failure **release everything already held** and retry — so it is never holding one lock while blocking on another. Different condition broken, same guarantee.

Both fixes ran clean, with `a.balance + b.balance == 2000` asserted afterwards — the cross-object invariant that motivated two locks in the first place.

**When to use which — structural, not preferential.** `scoped_lock` needs the **complete set** of mutexes at one point, because its back-off algorithm must be able to release everything it has taken. If lock A is acquired in one function and lock B requested three levels down, the inner code *cannot* release A — it has no handle to that guard. Ordering discipline is the fallback precisely because it requires nobody to release anything: each site independently obeys the same acquisition sequence. This is also why large systems use documented **lock hierarchies** rather than `scoped_lock` everywhere — where locks cross module boundaries, no single point ever knows the full set.

**Why the session points back at the engine — the ladder.** Races because memory was shared and mutable → mutexes because of the race → deadlock because of the mutexes → ordering discipline because of the deadlock. **Each fix creates the conditions for the next problem.** That is the argument for W7·1's conclusion being *eliminate the sharing* rather than *use locks carefully*: single-writer steps off the ladder at the first rung, and no sharing means no race, no mutex on the book, no deadlock, no ordering discipline — structurally absent, not defended against.

It also directly validates W7·2's **lock-per-level** reject, which is this drill at scale: two aggressors sweeping in opposite directions (buy ascending, sell descending) acquire price levels in opposite orders — `transfer(a,b)`/`transfer(b,a)` with *market data* choosing the acquisition order instead of the programmer. And the fix that worked here fights the algorithm there: imposing "always ascending" forces a sell aggressor to acquire locks **before knowing it needs them**, since its natural sweep is descending.

**The interview position this buys, both halves:** *"I've implemented deadlock-free multi-lock transfer two ways — consistent ordering and `scoped_lock` — and I know which Coffman condition each one breaks. And my engine never needs either, because single-writer means there's no second lock to order against."* Demonstrated competence plus the judgement to have designed it out.

**Gate: MET.** Thread spun up, a real race produced and observed (wrong number, varying per run), fixed two ways; deadlock produced deliberately, fixed two ways; invariant asserted.

**Cards harvested (6):** thread lifecycle — joinable, join vs detach, why the destructor terminates, `jthread` · `std::thread` copies its arguments and why, hence `std::ref` · `std::mutex` non-copyable and *why* (a copied mutex silently synchronises nothing) · `lock_guard` as RAII and scoping the critical section · atomic vs mutex — single variable vs multi-object invariant · deadlock — the four Coffman conditions and which fix breaks which.

---

### W7·2 — Deriving the concurrency model (COMPLETE)

Design session, no code. Output: the model, the three rejects with their reasoning, and the id-assignment decision. Nothing in this session touches matching logic.

---

**The model: single-writer-with-queue.** One thread owns the `OrderBook` outright — not "has priority on it," owns it. No other thread holds a pointer to it or has any path to its memory. Producer threads never touch the book; they push *requests* (submit this order / cancel this id / modify this id) onto an MPSC queue. The writer thread loops: pop, dispatch on type, call the existing book method, handle the result.

Checked against every W7·1 failure mode: two threads racing on `quantity` — impossible, one writer. Dangling `Order*` from `best()` — impossible, the only thread that can obtain it is the only thread that can invalidate it. Fairness scrambled by scheduling — impossible, the writer processes in pop order. Deadlock — impossible, no locks on the book at all. The character of those four matters: **structurally absent, not defended against.** No lock to forget, no invariant to maintain, no ordering discipline to get right. The bugs can't be written because the code that would contain them doesn't exist.

**`seq` assignment.** The writer assigns it at pop time. This is what makes price-time priority well-defined again — W7·1 established that under parallel matching "earlier" has no defensible meaning. Precise framing, worth keeping honest: this does **not** guarantee the order that reached the machine first gets the lower `seq` (network jitter and producer scheduling still affect queue-arrival order). What it guarantees is that **the system has exactly one definition of arrival order, and every subsequent decision is consistent with it.** That is what fairness actually requires. Real exchanges have the same property — the sequencer defines truth.

**The seam — why the engine is untouched.** The public API already returns by value (`submit → optional<vector<Fill>>`, `cancel`/`modify → bool`); no caller holds a reference into the book. So the writer calls these **unchanged** and the concurrency layer sits entirely outside them. Not a happy accident — it's why the Phase 3 gate ("matching logic untouched, diff-provable") is realistic. Every test, the 100k fuzz, and the shrinker stay valid because the thing they validated didn't change.

**What remains, and why it's progress.** The queue is shared mutable state under concurrent access — the problem is moved, not eliminated. Three-part argument that this is genuine progress: (1) the **contention surface collapsed** from unbounded (every level, order, map node, index entry) to two indices, head and tail, touched by different sides; (2) the **operations became trivial and bounded** — push-one/pop-one has essentially no intermediate state, vs `submit` sweeping an unknown number of levels with many mid-flight states; (3) **the queue has no ordering semantics to violate** — matching's correctness *depends on* sequence, whereas a queue's only job is to *establish* one, and any consistent order will do. So a concurrent queue is a known, isolated, exhaustively testable problem with a literature behind it. Concurrent matching isn't a hard problem; it's the wrong problem.

**The honest cost.** The writer is a **throughput ceiling** — one thread's worth of matching, forever, regardless of hardware. The escape hatch is horizontal: **shard by instrument.** One book per symbol, one writer per book, genuinely parallel, because *different books share nothing*. Arrival order matters within a book and is meaningless across books. That is the parallelism actually available, and it's available precisely because it doesn't cross the sequential constraint.

**Generalised technique (the reusable form).** 1 — **Eliminate sharing** where possible by giving data a single owner. 2 — **Concentrate** what remains into the smallest, dumbest surface possible (small = few variables; dumb = no domain semantics). 3 — **Solve that one thing properly**, in isolation, with real tests. 4 — **Scale by partitioning** over data that doesn't overlap, never by parallelising over data that does.

---

**The three rejects.** They fail for three *different* reasons, which is what makes this an analysis rather than three ways of saying "I picked the easy one."

**1 — Global lock. Fails on performance; correct but pointless.** One mutex round the whole book, `lock_guard` at the top of each public method. It is genuinely **correct** — every W7·1 failure mode is prevented, a real total order exists, and `seq` is assigned consistently inside the critical section. It fails because it **buys nothing**: four threads, 400 ns critical section, one at a time → 2.5 M orders/sec, which is exactly what the current single-threaded engine does. Identical throughput, plus costs the single-threaded version doesn't pay — lock acquire/release every operation; **contention** (a blocked thread is descheduled and later woken; a context switch is *microseconds* against a 400 ns critical section, so coordination overhead can be thousands of times the cost of the work it protects); **convoying**; and destroyed **p99 tail latency**, which is the number that matters in a latency-sensitive system. Root cause is the W7·1 insight: a lock doesn't create parallelism, it re-serialises threads that could never run in parallel anyway — paying coordination costs for parallelism the domain doesn't permit. **When it would be right:** when the critical section is a small fraction of thread runtime (10 µs of independent parsing/risk work, 100 ns of shared access → ~1% contention, fast path dominates). The test is *what fraction of a thread's runtime is inside the lock* — low → global lock is the right, simple tool; approaching 100% → no locking scheme helps. Matching **is** the workload, so it's the second case.

**2 — Lock per price level. Fails on correctness, even if implemented perfectly.** Finer locks so threads at 100 and 105 proceed concurrently. Breaks down immediately on mechanics: an aggressor sweeping levels can't release earlier locks as it moves (a concurrent rest at an already-swept level would be missed), so it **holds locks cumulatively in an order determined by market data** — and two aggressors sweeping in opposite directions (buy ascending, sell descending) acquire in opposite orders → textbook **deadlock**. The standard fix (global lock-ordering discipline, always ascending) fights the algorithm, since a sell naturally sweeps descending and would have to acquire out of execution order. Worse, most operations aren't level-local anyway: erasing an emptied level or inserting a new one **mutates the map**; `cancelIndex` is **global**; and `best()` — the hottest operation in the engine — is inherently global. So you need tiered locks, which is where the nasty deadlocks live. **But the killer isn't mechanical:** even with perfect lock ordering and zero deadlocks, **fairness is still broken** — two orders at the same price on two cores, and whichever thread the OS schedules first takes the fill regardless of arrival, with `seq` assigned by whoever gets there. Price-time priority violated with *zero memory bugs*. No amount of locking skill fixes it, because the problem isn't in the locking; the domain forbids what's being attempted. **When it would be right:** when elements are genuinely independent and there's no cross-element ordering rule — a sharded hash map with per-bucket locks is exactly this and works beautifully, because nobody cares which of two concurrent inserts "happened first."

**3 — Lock-free book. Fails on feasibility and risk (and fairness anyway).** Lock-free structures exist for queues, stacks, and simple maps — structures where a mutation is one pointer swap, so a single CAS can publish it. Matching is not that: one incoming order reads best price, mutates a resting order's quantity, appends to fills, may erase a list node, may erase a map entry, updates the cancel index, may insert a remainder. That's a **coordinated multi-structure transaction** with no single atomic operation that publishes it — you'd need multi-word CAS or STM, which is research machinery, not a design choice. Specific hazards: **memory ordering** (every atomic needs an explicit ordering; too weak is a *silent* race — no crash, no assertion, correct on 99.9% of runs; x86's strong model hides many incorrect orderings that ARM will expose, so code can pass exhaustively on one machine and fail on another, or pass at `-O0` and fail at `-O3`); **ABA** (CAS checks *"is the value unchanged?"* as a proxy for *"has nothing happened?"* — A→B→A defeats it; needs tagged pointers, hazard pointers, or epoch-based reclamation); and **memory reclamation**, the genuinely hard part — when is it safe to free a node another thread may still be reading. And even fully built, **fairness still breaks**, same as reject 2. The decisive argument for *this* project: it introduces a bug class the existing testing infrastructure (100k fuzz, four invariants, proven shrinker) **structurally cannot detect**, which is a bad trade for a project whose credibility rests on demonstrated correctness. That is a much stronger rejection than "it's hard." **When it would be right:** narrow structures with no domain semantics — which is exactly why lock-free remains a live option for the **queue** at W7·4, and not for the book.

**The unifying statement.** The problem isn't concurrency. It's that matching has a semantic requirement — **a total arrival order** — that concurrent execution destroys. All three rejects attack the *mechanism* of concurrent access; none addresses the *requirement*. Global lock preserves the order and gains nothing; lock-per-level and lock-free sacrifice it for parallelism the domain doesn't permit. Single-writer wins by starting from the requirement: establish the order once, in one cheap place (the queue), and let one thread execute against it — putting parallelism where it's actually available (producers doing network/parsing work; sharding across instruments).

---

**Decision — id assignment: producer-partitioned (Option B).**

Each producer owns a **disjoint id space**: producer id in the high bits, a per-producer counter in the low bits (8/56 split on `int64_t` — 256 producers, ~7×10^16 ids each; at 1 M orders/sec one producer would need over two thousand years to exhaust its space). Uniqueness is **by construction** — two producers cannot collide because they were never drawing from the same pool. Same technique as single-writer, one level down: data with exactly one writer has no concurrency problem.

- **Counter is thread-local and non-atomic** — a plain `int64_t` incremented only by its owning thread. The alternative, one global `atomic<int64_t>` with `fetch_add`, would put every producer's every order through an atomic RMW on **one shared cache line** — the exact cross-core ping-pong from the false-sharing card. Partitioned counters are plain increments on thread-local memory: zero contention, zero coherence traffic. A performance argument, not only a correctness one.
- **Bit-partitioning over modular** (`id % N == p`): N is baked into every id under the modular scheme, so adding a producer later invalidates the arithmetic for everything already issued, whereas a new producer under bit-partitioning simply takes an unused top-bit value. Bit-packed ids are also **self-describing** — `id >> 56` recovers the issuing producer, useful for logging, tracing, and debugging a bad order back to source.
- **Producer id source:** startup constant, passed in at thread construction. Dynamic registration is the flexible alternative and is acceptable *because it coordinates only at startup, never on the hot path* — an ordinary mutex there is fine.
- **Counter never resets.** Monotonic for process lifetime; resetting per session/day would reissue previously-used ids and reintroduce across-time ambiguity.
- **`validate`'s duplicate check stays** as **defence in depth** — uniqueness is guaranteed by construction, and the check remains a cheap assertion that the contract held against a misbehaving producer. (Contrast: under engine-assignment it would become genuinely dead code.)
- **Costs, stated:** ids are sparse rather than a dense 1,2,3 sequence (cosmetically odd in logs, worth a code comment), and it is a **contract** rather than an engine-enforced guarantee.
- **Zero disruption to existing work.** `Id` is already `int64_t` and nothing in the book interprets it — `cancelIndex` hashes it, `validate` compares it. The book is literally unchanged; the scheme lives entirely in producer construction, which is new code. Hand-written tests keep ids 1, 2, 3 (a single-threaded test is effectively producer 0 with a hand-cranked counter). 23 tests, the fuzzer, and the shrinker are untouched.

**The latency win, stated precisely:** the producer knows its id **at push time**, so cancellation requires **no round trip**. (Earlier framing "keeps reliability in responses" was corrected — B does *not* give response reliability; a response path is needed under every option, because producers must learn about **fills**, not just ids. B's win is narrower and sharper: it decouples *"can I name my order?"* from *"did my order work?"*)

**Why this creates no new correctness problem:** a producer pushing *submit* then *cancel* for the same id puts both in one FIFO queue, so the writer pops submit first, always — a cancel **cannot overtake its own submit**. If the submit was rejected, the cancel arrives for an id not in the book, which `cancel` already handles as a clean no-op (tested since W4·5). The latency win falls out of the queue's FIFO property plus behaviour already built.

**Rejected:**
- **Engine-assigned ids (A)** — uniqueness by construction at the engine and `validate`'s check becomes dead code, but the producer can't know the id until the writer responds, so **the order is uncancellable until a round trip completes**. In a fast market that window matters. Decisive against, for a latency-focused project.
- **Client + engine dual ids, FIX-style `ClOrdID` + `OrderID` (C)** — correct at venue scale, and it solves two genuinely different problems: the *client's* (reference my order immediately, with no round trip and no coordination with other clients — which is B's insight generalised, uniqueness scoped to the issuer) and the *venue's* (one identifier unique across every client, session, and day, for audit, reconciliation, and regulatory reporting). Entails a second `Order` field, a `clientId → engineId` resolution index on top of the existing one, a rule for which id appears in which message class, and a scoping decision (unique per session / per day / forever). **Understood and deferred:** producers here are trusted in-process network threads, not untrusted external clients, so B's partition contract is enforceable rather than hopeful. Building C would be scope not justified by a requirement.
- **Status quo (D)** — declare uniqueness a caller contract with validation as a net. Zero implementation and defensible *if stated deliberately*, but weaker than an actual decision.

**Flagged forward (W7·4 queue design):** a **response path** is now a required design item under any id scheme, since producers must learn outcomes (fills, rejections) — likely a second queue in the reverse direction. Noted here so it isn't discovered mid-W8.

**Deferred to W8·1 (implementation):** bit manipulation proper — shifts and masks, `int64_t` vs unsigned, the **sign-bit hazard** (shifting into bit 63 of a signed type is UB — the 8/56 split leaves the top bit clear), construction and extraction, and writing it readably rather than cleverly. Earns a Wave-2 `[cpp]` card.

**Gate: MET.** Model and all three rejects defensible in own words; id assignment decided in writing.

---

### W7·1 — Concurrency primitives (COMPLETE)

No code. Concepts built from the ground up, anchored to this engine's own hot path rather than textbook examples.

**The two failure modes, both derived from the real code.** `best()` returns a non-const `Order*` — a handle to the actual resting order — and the match loop mutates it **in place**. That single design fact (correct and deliberate single-threaded) is what makes the book unsafe the instant a second thread exists:

- **The order dies underneath a reader.** Thread A consumes the best resting order and erases it; thread B is still holding the same pointer and dereferences it. Use-after-free — the exact bug class already hit three times single-threaded (cancel, modify, submit's cleanup), except now the destructive call isn't a line visible above the read. It happens *between* instructions, from another thread. **Capture-before-erase cannot help, because there is no "before" under your control.**
- **Nothing dies, and it's still wrong.** One resting sell of 100; two incoming buys of 100 on two threads. `quantity -= tradeQty` is not one operation — it is load, subtract, store. Both threads load 100 before either stores, both compute a trade of 100, and one store overwrites the other. **200 units trade against a resting order that only had 100.** No crash, no dangling pointer, every pointer valid throughout — and **volume conservation**, the most financially load-bearing of the four fuzzer invariants, is violated with no bug in the matching logic at all.

**Precise definitions, kept distinct.** A **data race** is the memory-level fault: two threads accessing the same location, at least one writing, without synchronisation — and in C++ terms this is **undefined behaviour**, not "you might read a stale value." The compiler is entitled to assume no races exist and optimise accordingly. A **race condition** is broader: the *outcome* depends on timing. The distinction matters because fixing every data race does **not** fix the fairness problem — make `seq` assignment atomic and lock every level correctly, and two same-priced orders on two cores still fill in whichever order the scheduler happened to pick.

**Shared mutable state as the enemy — and precisely why.** It is the **conjunction** that is fatal: shared + mutable + concurrent access. Shared-but-immutable is completely safe (any number of threads may read const data with no guard rails). Mutable-but-unshared is completely safe (thread-local data has nobody to race with). Only the overlap produces UB — which is useful, because it says exactly which of the three to attack. Immutability is unavailable (a book that cannot change is not a book), so the attack must be on sharing or on concurrent access.

**The terminal insight (the session's actual product).** Explored the "restrict *when* threads touch it" branch first — a lock — and found it hollow: with one thread inside `submit` at a time, four threads at 400 ns each give exactly the throughput of the existing single-threaded engine, minus the cost of coordination. **A lock does not create parallelism; it destroys it**, forcing concurrent threads back into single file. Parallelism only ever pays when threads do work that *doesn't* overlap, and here all the work is the same book. Then the per-level refinement, which fails for a deeper reason: `seq` is what encodes arrival, the match loop consumes each level front-to-back in `seq` order, and the entire fairness rule is therefore a statement about **sequence** — whereas parallelism is precisely the licence to not care which of two things went first. Hence:

> **Arrival order is the semantics.** The sequence does not decorate the result, it *determines* it — hand the same set of orders to the engine in a different arrival order and genuinely different people get filled at genuinely different prices. Matching is therefore inherently sequential: parallelise it and you either serialise through locks, gaining nothing, or you don't, and the results change. **There is no third option.**

Worth keeping for interviews: the weak answer to "why didn't you parallelise your matching engine?" is *concurrency is hard*. The strong answer is that it is **structurally impossible to gain from**, derivable from the fairness rule in thirty seconds — a rejection on principle rather than on difficulty.

**The other branch — destroy the sharing.** Exactly one thread ever touches the book. Not one-at-a-time; one, period, with sole ownership and no path from any other thread. Every failure mode above becomes **structurally absent** rather than guarded against, and with no locks there is no contention and no deadlock. This forces the remaining question — how does work reach a structure nothing else can touch? — and the answer is a hand-off point both sides can reach: a queue. Many producers push, one consumer pops. **MPSC** (multiple producer, single consumer). Whatever order requests land in the queue *is* the arrival order, the writer processes them in exactly that order, and `seq` is assigned by the single thread doing the popping — so price-time priority is preserved by construction.

**Objection raised and answered, since the queue is itself shared mutable state under concurrent access.** Three reasons this is progress rather than relocation: the **contention surface** collapses from unbounded (every level, every order, every map node, every index entry) to essentially two indices, head and tail, touched by opposite sides; the **operations become trivial and bounded** (push-one, pop-one — almost no intermediate state, versus `submit` sweeping an unknown number of levels with many mid-flight states another thread could catch); and decisively, **a queue has no ordering semantics to violate** — matching's correctness *depends on* sequence, while a queue's only job is to *establish* one, and any consistent order will do. A concurrent queue is a known, isolated, exhaustively testable problem with real literature behind it. Concurrent matching is not a hard problem; it is the wrong problem.

**Bounded, not unbounded — a correctness argument, not a comfort one.** If producers outrun the consumer on an unbounded queue, memory grows without limit (and growth means **allocating on the hot path**, the exact unpredictability W9–10 exists to attack), and latency grows without limit — an order sitting in the queue while the book moves is *technically* correct and *commercially* a wrong outcome. In a trading system, unbounded latency **is** a failure. So the queue must be bounded, which forces a stated overflow policy rather than undefined behaviour. **Resolved at W7·4: reject on full.**

**False sharing (the hardware rung).** In a bounded ring buffer the consumer advances `head` and producers advance `tail`. Logically independent — no race, no lock needed. But the CPU's unit of coherence is the **64-byte cache line**, not the variable, and a core may only *write* a line it holds **exclusively**. Adjacent `head` and `tail` share a line, so each write **invalidates** the other core's copy of the whole line and forces a re-fetch: the line **ping-pongs**, and every write becomes a cross-core coherence transaction instead of a local cache hit. The sharing is **false** — it exists in the hardware and not in the program; two genuinely independent actors in the code sit on one line underneath. **Fix: change the layout, not the writers** — `alignas(64)` per field or explicit padding, costing a few dozen bytes. (A single-writer "fix" is not available here: head and tail *must* be written by different threads; that is the design.) **The tell: adding threads makes it slower.** Confirm with `perf c2c` on Linux, Instruments on Mac.

**Memory ordering — awareness only, deliberately.** Compilers and CPUs reorder memory operations, preserving only *single-threaded* observable behaviour, so another thread can observe the reordered sequence. Once head and tail become atomics, each operation needs an explicit ordering, and too weak a choice is a **silent** race. Depth deferred to W7·4 — where the Modest dial was taken, so it stays at awareness level.

**Also surfaced (parked):** per-level locking would require holding **multiple locks at once in an order determined by market data** — two aggressors sweeping in opposite directions acquire in opposite orders, which is the textbook deadlock setup. This pre-loaded the W7·3 `AccountManager`/`scoped_lock` drill and became the mechanical half of W7·2's second reject.

**Gate: MET** (on the redo). Sequentiality, shared mutable state anchored to `best()`, what a queue buys, and false sharing — all produced cold. Two items were initially under-produced and logged as re-reps: the *nothing-dies* failure mode (the lost-update case, which is the more interesting half precisely because there is no crash to find), and the stronger half of the queue argument (contention surface, bounded operations, no semantics to violate).

---

### W6·4 — Defence Pass (COMPLETE — Phase 2 exit gate fully met)

Ran the full ten-decision defence cold, no notes: Order type, book structure, price level, cancel index, match loop, modify, validation, fuzzer/invariants, map unification, and the two real bugs found — each required stating what/why/rejected/how-you'd-know-if-wrong/what-you'd-do-differently from memory, with challenge on anything under-specified or imprecisely framed. Corrections made, worth keeping precise going forward:

- **Order type:** integer ticks are about *correctness* (exact comparison), not speed — modern FP arithmetic isn't meaningfully slower than integer; leading with "expensive" invites an easy counter. The counter-for-seq argument leads with "unique and ordered by construction," with "clocks are unreliable" as the supporting rejection, not the other way round.
- **Book structure:** the real justification is mechanical (composite keys, loss of each side's baked-in natural ordering, degraded access on the hottest operation in the engine — `best()`) — "clarity" is a consequence of that, not the driver, and leading with it sounds soft under a technical follow-up.
- **Price level:** the precise `std::list` guarantee is the single most load-bearing fact in the whole design and is worth being word-perfect on — erasing/inserting anywhere invalidates *only* the iterator to the erased element, nothing else, ever. Briefly stated backwards mid-defence; corrected and drilled.
- **Cancel index:** "redundant" sharpened to the concrete failure mode — caching price/side separately from the order risks a stale-cache bug the instant the order is modified and the cache isn't updated in lockstep; storing only the iterator makes that structurally impossible rather than just unnecessary.
- **Match loop:** crossing conditions must be stated by the **incoming** order's side, not the resting order's side (the resting side is always the opposite, and anchoring the explanation to it is what caused a live in-session inversion of the buy/sell crossing logic — the exact shape of the original W5·3 bug, said backwards by habit rather than reintroduced in code).
- **Validation:** the market-order price exemption is because price never participates in a market order's crossing decision at all (type alone decides), not because price `0` carries special meaning — that framing would contradict the "no magic values" position already taken for modify.
- **Fuzzer/invariants:** all four invariants must be named on demand (crossed-book, volume conservation, FIFO, orphaned-index) — an initial answer surfaced only two and omitted volume conservation specifically, the most financially load-bearing of the four. "No crossed book" was also initially overstated as proof that "every trade that should have executed, did" — corrected to its actual, narrower scope: a resting-state structural check that can *result from* a matching failure, not a certificate that matching was exhaustively correct. No unearned scale claims ("millions of operations per second") attached to this project's own numbers.
- **Map unification:** reframed from "I was careful, so nothing went wrong" to the more accurate and more interesting claim — the refactor was safe to *attempt* specifically because a regression suite existed to catch mistakes, and it demonstrably did: two real bugs were introduced and caught within the same session. The bugs are the receipts, not a blemish to smooth over.
- **The two bug stories:** confirmed as genuinely known and understood throughout the pass (correct comparison direction, correct reasoning about equal-price masking, correct fix) — but a *live cold retelling*, prompted under a specific narrative framing, briefly conflated the sell-crossing bug with an unrelated later bug (the fuzzer generator's inverted type-weighting). Concluded that knowing the facts and cleanly narrating them on demand are different skills, and that repeatedly performing the retelling back doesn't add signal once the facts are confirmed solid — the fix was writing each story once, precisely, into the README, so the retelling exists as a stable artifact rather than something reconstructed from scratch under pressure each time.

**README fully rewritten** to reflect all of the above: every design-decision section restated with the corrected framing, two new sections added (Match Loop, Modify, Validation, Map Unification, Property-Based Testing, and a dedicated Bugs Found and Fixed section — several of these didn't exist as their own sections before), the stale "9 passing tests" / "planned" language replaced throughout, and the informal benchmark preview folded in with its caveats intact.

**Phase 2 exit gate — confirmed met in full:** correct (all four operations, edge cases, fairness rules) · tested (23-test replay harness + 100k-operation fuzzer across 4 invariants + a proven shrinker) · defensible (this pass, now written into the README) · visible (live repo, current README + DEVLOG) · scope-clean (no threads, no real benchmarking infrastructure, no persistence snuck into the core).

---

### W6·3 — Shrinker (COMPLETE)

**Why build one at all, given the 100k fuzz run found nothing organically:** raised and resolved directly. Initial instinct was that per-iteration invariant detection (which already reports *which* iteration and *which* invariant failed) makes shrinking redundant, since the failures are "process execution" problems, not data problems. The gap: detection gives you *where*, not *which prior operations were causally responsible*. In a long run, a failure at iteration 4,832 could be caused by the operation at 4,832 itself, or by something that rested at iteration 12 and sat untouched for thousands of steps. Manually reconstructing which of thousands of preceding operations are even relevant is exactly the tedious, error-prone work a shrinker automates — and a shrunk 3-5-operation reproducer is hand-traceable and droppable straight into the permanent test suite; a multi-thousand-operation trace is neither.

**Capture mechanism:** `LoggedOp` — a tagged struct (`OpType::Submit/Cancel/Modify` + the fields each kind needs: `Order` for submit, `Id` for cancel, `Id` + optional new price/quantity for modify). `generateAndExecute` now appends one `LoggedOp` per iteration to a `history` vector and returns it (wrapped in `optional`, `nullopt` = clean full run) the instant any invariant fires — freezing the exact sequence that produced the failure.

**`invReplay(vector<LoggedOp>&) -> bool`:** fresh `OrderBook`, executes each logged operation in order dispatching on its tag, checks all three invariants after each step; `true` = sequence replays clean, `false` = reproduces the failure. This one function is reused by both the initial capture and every step of the shrink loop.

**Bugs caught while building this (a good comparison-to-plan debugging exercise in its own right):**
- Missing braces around the three invariant `if` checks meant only the `return false;` was conditional — the diagnostic `std::cout` lines ran unconditionally every iteration regardless of outcome. Fixed with explicit braces.
- Missing `return true;` at the end of `invReplay` for the clean-replay case — fell off the end of a `bool` function (the same "every path must return" class of bug from `validate`/`best` months ago).
- **First shrinker draft was fundamentally broken and shrank every failing run to 0 operations** (immediately suspicious — an empty book cannot be crossed, so this was a clear signal something was wrong, not a real result). Root cause: `pop_back()` was used regardless of loop index `i`, so the loop was unconditionally stripping the sequence from the back `size()` times rather than testing removal of a *specific* element each iteration; combined with a reinsert line (`seq.push_back(seq[i])`) that read the wrong element anyway since it read *after* the erase had already happened.
- **Second draft** switched to `erase(begin()+i)`/`insert(begin()+i, ...)` at a specific index (correct approach) but iterated *forward* while erasing — erasing index `i` shifts every later element down by one, so forward iteration silently skips elements as the vector shrinks underneath it. Fixed by iterating backward (`size()-1` down to `0`), which only invalidates already-visited indices.
- Also had to explicitly re-verify the keep/discard branch matched `invReplay`'s true-means-clean convention (an inverted condition here was suspected as a possible contributor to the earlier 0-operation result) — confirmed and corrected: "still fails after removal" → discard (irrelevant operation); "now clean after removal" → reinstate (load-bearing operation).

**Final shrink loop:** backward-indexed single pass wrapped in a `while` that repeats full passes until one entire pass removes nothing (a stable fixed point) — not just a single pass, since removing one operation can newly expose another as removable (cascading dependencies).

**Proof run:** temporarily reverted the sell-crossing condition to the old (known-buggy) `incoming.price >= resting.price` for both sides. Fuzzer caught the reintroduced crossed-book violation, captured a 34-operation failing sequence, and the shrinker reduced it to a **stable 20 operations** (a full pass removing none). Bug reverted afterward; reran the 100k fuzz once more to confirm the codebase is back to zero violations.

**Known, stated limitation (not a bug):** one-at-a-time shrinking can stall above the true theoretical minimum when operations reference each other by id across the sequence — e.g. removing an early submit doesn't crash a later cancel of that same id (cancel on an unknown id is already a clean no-op by design), but it silently changes the book's later trajectory, which can make that submit look load-bearing when it's actually just *referenced*, not *causally necessary*. A stable result under a single-element-removal strategy is the correct goal for this algorithm — reaching the absolute global minimum would need group/chunk removal (the kind of thing more sophisticated delta-debugging algorithms do), judged out of scope for v1's shrinker.

---

### W6·1–2 — Property-based invariant testing + fuzzer (COMPLETE)

**Invariants — precisely defined before coding, each derived through explicit reasoning (not just named):**
- **No crossed book:** `best(Buy)->price < best(Sell)->price` whenever both sides are non-empty; vacuously satisfied if either is empty. Implemented as `checkNoCrossedBook() const -> optional<vector<Order>>` — `nullopt` = clean, a populated vector (the two crossing orders) = violation. Required adding a `const` overload of `best()` (the existing one returns non-const `Order*` for the match loop's in-place mutation; the checker needed a read-only path).
- **Volume conservation:** for every trade, the aggressor's quantity decrease must equal the resting order's quantity decrease (the `min(quantities)` line's guarantee, checked after the fact). Lives in the fuzzer/harness against returned `Fill`s — not a book method, since it only needs public `Fill` data.
- **No orphaned cancel-index entries:** every `(id, iterator)` in `cancelIndex` must dereference to an order whose own `.id` matches the key. `checkNoOrphans() const -> optional<vector<Order>>`, same nullopt/violation-list shape.
- **FIFO preserved:** within any level's list, consecutive orders must have non-decreasing `seq`. **Proven structurally impossible to violate** by tracing every insertion path in the actual code (not assumed): `restInto` only appends at `end()`; the match loop only pops from the front; `modify`'s reduce-path only mutates a field in place, never re-inserts; `modify`'s increase/price-change paths go through `cancel` (pure removal) then `submit`→`restInto` (fresh seq, appended at end). No path can reorder a list. Because of this, `checkFIFO` was initially planned to be dropped from the per-operation hot path (O(book size) per call vs. crossed-book/orphans' cheaper cost) — ultimately built anyway (single forward pass, track previous `seq`, flag any decrease) since it was cheap to add and useful as regression insurance against future changes silently invalidating the guarantee (exactly what happened with the map-unification's begin/rbegin assumption a session prior).

**Placement discipline — a real design correction made mid-session:** initially reached for folding FIFO/orphan checks into `validate`. Caught and reversed: `validate` is a *gatekeeping* question (should this new order be admitted) with no view of the whole book; these are *whole-book structural* invariants needing access to all levels/the whole index. Landed on standalone `const`, pure-observation methods on `OrderBook` — explicitly **detection, not enforcement**: they watch and report, never intervene or auto-fix, since acting on a violation would hide the very bug the check exists to expose.

**`getOrderInfo(Id) const -> optional<Order>`:** added to let the generator read a resting order's current price/quantity safely for percentage-based modify generation — returns an owned copy (not an iterator/pointer), so it carries zero dangling risk regardless of what happens to the book afterward. Explicitly rejected an earlier idea of the generator holding its own `id→iterator` map (would have duplicated `cancelIndex` and reintroduced the exact dangling-iterator fragility fixed in the map-unification session — two independent structures holding handles into the same live memory).

**Generator design (`Generator` struct — `rng`, `restingIds` (plain `Id`s, no iterators), `nextId`):**
- Weighted operation mix, modify heaviest (highest-risk, most branching logic) — designed as: ~50% modify, ~30% submit, ~20% cancel; force submit if `restingIds` is empty.
- Type weighted ~90% Limit / 10% Market (only limits rest, so this is what keeps `restingIds` populated with real material for cancel/modify to target — an all-market flow would starve the interesting paths).
- Narrow price band (1–100) — deliberately maximises crossing/matching pressure, pointing fuzz effort at the invariants with actual historical bugs (crossed-book, volume conservation) rather than FIFO, which was independently proven structurally safe by this point.
- Modify's new price/quantity: percentage-of-current-value via `getOrderInfo`, scaled to a **max ~20% change per call** (divisor of 100, not 10 — the original /10 scaling allowed up to 3x per modify, which compounded across repeated modifies on the same order into absurd price drift, e.g. one order observed reaching price 15,918 after six consecutive modifies in an early test run).
- One operation generated and executed against the real book per step (not planned blind in advance) — resolves generator/book state drift, since a tracked "resting" order can get fully matched away by an unrelated later operation; checking the real book's state after each step keeps `restingIds` accurate.

**Bugs caught during generator construction (before any fuzzing even started):**
- Type-weighting was inverted **twice** across two different code paths: first pass used `== 0` out of a `(0,9)` range (9-in-10 → Market, the opposite of intended); after a partial fix, one of the two submit branches (the empty-book bootstrap path) was corrected while the other — the one that actually fires on effectively every iteration once the book has any resting order — was left on the old broken logic. Caught by reading the fuzz log's actual output ratio rather than trusting the code by inspection.
- `checkNoCrossedBook`'s truthiness direction double-checked explicitly (optional is truthy when it *holds a value* — i.e. when a violation was found) since it reads easily backwards; confirmed correct, documented with a comment to prevent a future accidental "fix."

**The run:** 100,000 operations, `-O3`, 0.39s wall time. **Zero invariant violations** — crossed-book, orphans, and FIFO all held for the entire run (confirmed via `./tests > run_output.txt && grep -c "DETECTED" run_output.txt` → `0`, not just eyeballing the log tail). This is the first genuinely volume-tested confidence in the engine's core correctness, beyond the 23 hand-authored scenarios.

**Process note:** two separate false-positive "clean" runs occurred before this one — first, a run where the invariant checks were accidentally commented out (measuring generation speed only, not correctness); second, a run where the type-weighting fix hadn't actually landed in the branch that mattered, so the "clean" pass was against unrepresentative (mostly-market) flow. Both caught by insisting on checking actual log output/ratios rather than trusting that a fix compiled correctly. Worth remembering: a fast, quiet test run is not evidence of correctness unless you've confirmed what was actually being exercised.

---

### Map unification (pre-W6 cleanup — COMPLETE)

- **Motivation:** `bids` used `std::greater<>` and `asks` used the default comparator specifically so `begin()` always meant "best" on both sides — which is why `withOppositeSide`/`withGetSide` had to be *templates* (the two maps were different C++ types). Unified both maps to the default comparator, replacing the templates with three small named helpers: `getMap(Side) -> map&` (the map for a given side, no flipping), `opposite(Side) -> Side` (the enum flip, used explicitly at call sites — e.g. `submit` composes `getMap(opposite(incoming.side))` rather than hiding the flip inside a helper), and `best(Side)` (already existed since W4·2, updated to `bids.rbegin()` / `asks.begin()` now that "best" isn't automatically at `begin()` for both sides).
- **Design call:** kept `rest()` as its own simple if/else rather than routing it through `getMap` — it wasn't duplicating error-prone logic, so adding indirection there would have been simplification for its own sake. Judged case-by-case rather than mechanically applying the new helpers everywhere.
- **Rejected:** reusing `validate()` for post-match cleanup in `submit`'s loop — `validate` is a gatekeeping question ("should this new order be admitted"), cleanup is a different question ("this existing order hit zero, remove it correctly"). Different category, `cancel()` is the right existing tool.
- **Rejected (explicitly, mid-session):** doing a broader cleanup/enhancement pass while at it, on the reasoning "I keep noticing more things, my code quality is improving." Recognised this as the normal experience of touching load-bearing code (re-examination surfaces things every time, doesn't mean growing debt) rather than evidence of a real backlog — and that W6's fuzzer is the disciplined tool for systematically finding exactly this class of thing, not more ad-hoc manual review. Kept the change bounded to the map unification only.
- **Bug 1 — found during the refactor:** `submit`'s post-fill cleanup block still did `oppositeSide.begin()->second.orders.pop_front()` unconditionally. This was fine when the comparator trick made `begin()` mean "best" on both sides — now that both maps share one comparator, `begin()` only means "best" for asks; for bids, best is `rbegin()`. The match itself (via `best()`) correctly read from the right end, but cleanup after a fill was popping/erasing from the *wrong* end whenever the aggressor was a sell (matching against bids). Manifested as failed rest-remainder/market/price-change-modify tests with corrupted state and, eventually, a crash from cumulative map corruption.
  - **Fix:** replaced the inline `begin()`-based pop/erase with a call to `cancel(restingId)` — side-agnostic, works from the order's own fields via the existing index, no begin/rbegin assumption. Capture `resting->id` into a local *before* calling `cancel` (same capture-before-cancel discipline as W5·1's modify), and ensure `fills.emplace_back(...)` runs *before* the cancel call, not after — reading `resting->price`/`resting->id` after `cancel` erases the node is a use-after-free (this was the source of the garbage Fill values and the trace-trap crash seen mid-session).
- **Bug 2 — found immediately after fixing Bug 1:** `cancel`'s internal side-lookup was written as `auto map = getMap(order.side);` — missing the `&`, silently copying the entire map. `cancel` was operating on a throwaway copy, so the real `asks`/`bids` was never actually mutated — explaining why the loop's next iteration kept finding a stale/corrupted view of the book even after Bug 1's fix. Same *class* of mistake as two earlier copy-vs-reference bugs from the same refactor session (the original `getSide`/`getOpposite` drafts also returned by value before being corrected) — worth flagging as a recurring failure mode: always double-check `auto&` vs `auto` at every call site of a function that returns a reference.
- **Full 23-test suite green** after both fixes. No regressions.

### W5·3 — Validation (COMPLETE)

- **`validate(const Order&) -> bool`**: rejects duplicate id (via `contains`), `quantity <= 0`, and `price <= 0` for limit orders only (market price ignored). `seq` exempt — engine-assigned, never caller input, so nothing to guard there.
- **`rest` converted to `bool`**, guard-clause style: `if (!validate(o)) return false;` at top, `return true;` at bottom. Consistent with `cancel`/`modify`'s existing bool-return pattern — no more silent void operations.
- **`submit` now returns `std::optional<std::vector<Fill>>`**: `nullopt` signals rejection (failed `validate` before any matching attempted); a present vector (possibly empty) signals legitimate matching. First time rejection and legitimate-zero-fills are distinguishable.
- **Critical bug found and fixed:** `submit`'s crossing condition used `incoming.price >= resting.price` for BOTH sides. Correct for a buy aggressor; wrong for a sell — should be `resting.price >= incoming.price`. Invisible in prior tests because the existing sell-aggressor test used equal prices, where both comparisons agree. Only manifests when a sell genuinely undercuts a resting bid (the common real case). Caught by a dedicated test with unequal prices (sell 95 vs resting bid 100) — first run correctly failed with 0 fills instead of 1, confirming the diagnosis before the fix; fix applied, full ~20-test suite rerun to confirm no regression. All green.
- **New test — "Cross Comparison Check":** sell 20@95 against resting buy 50@100. Expects fill at the *resting* price (100, not 95, per the execution-price rule), aggressor 2, resting 1; states 30 remaining bid / 0 ask.
- **`ValidationTest` helper**: takes the book by reference, a vector of orders, and a parallel vector of expected accept/reject outcomes — checks each submission's actual outcome against what was expected, rather than just "did anything fail" (needed because the bad-price case proves rejection AND non-false-positive in the same run).
- **3 validation tests, all passing:**
  - Duplicate id: second order with a reused id rejected, first order's quantity (50) untouched (`quantityAt` confirms).
  - Bad quantity: zero and negative quantity both rejected, book stays empty.
  - Bad price: negative/zero limit prices rejected, market order with price 0 accepted in the same run — proves the guard doesn't false-positive on markets, which are exempt from price validation by design.
- **W5 FULLY COMPLETE** — modify, edge cases I, validation, all done and tested. 23 passing tests.

### W5·2 — Edge cases I

- **Multi-level sweep:** 3 resting levels (25/25/50), one aggressor for 100 sweeping all three. Proves the match loop generalises across repeated iterations, not just a single 2-level partial (which every prior test used).
- **Exact-match boundary:** isolated single-level test — rest 50, aggressor exactly 50. One fill, both sides to zero, level erased. A third order (a small buy resting *after* the match) acts as a live probe: if the sell level had a ghost entry instead of being genuinely erased, this order would produce an unexpected fill instead of resting cleanly. It doesn't — proving erasure, not just emptiness.
- **Self-cross:** reasoned out of scope for v1 — `Order` has no participant/account field, so "same trader on both sides" isn't representable. Documented as a possible future extension (participant model), not a bug or gap.
- **Order-id ownership:** reviewed caller-assigned vs engine-assigned ids. Staying caller-assigned for now (test legibility — hand-authored sequences need predictable ids); revisit engine-assignment at W6 fuzzing / W7+ benchmarking, where uniqueness-by-construction matters more than hand-editing convenience. Duplicate-id guard implemented as W5·3 validation item. **[RESOLVED at W7·2 — producer-partitioned ids; see that entry.]**
- **Plan review:** most of the originally-listed W5·2 edge cases (thin book, empty side, 2-level partial) turned out already covered by existing W4 tests. Re-scoped to the two cases above, which are the ones existing tests structurally can't catch (loop-generalisation, exact-zero boundary).

### W5·1 — Modify

- `modify(Id, std::optional<Price>, std::optional<Quantity>)` — target-state signature; optional fields mean "leave unchanged", handles single or combined changes atomically, `nullopt` self-documents (vs a magic 0).
- **Fairness rule derived:** reduce-quantity keeps queue position (in-place edit — asking for less harms no one); increase-quantity and price-change LOSE position (the added quantity / new level arrived later, can't jump earlier orders) → implemented as cancel + resubmit with same id, fresh seq.
- id preserved across modify (stable identity); seq is fresh on resubmit (that's what puts it at the back). The id-vs-seq split from W4·1 paying off.
- `quantity == 0` → cancel, checked FIRST (before price/quantity routing) so "to zero" always cancels regardless of price change.
- Bugs caught: use-after-free (using the order reference after cancel destroys the node) — fixed with capture-before-cancel (side/price/quantity into value locals, then cancel, then rebuild+submit); forgetting to override the captured field with the new value in each change branch.
- Rejected: limit→market via "price 0" (magic-value overloading, mixes the rests/doesn't-rest boundary) — type-change out of scope for v1.
- All five modify cases green: reduce-keeps-position, increase-loses, price-change-loses, modify-to-zero-cancels, unknown-id-noop. Derived and verified the fairness rule **by consequence** (fill-order in the resulting Fills), not by inspecting internal queue structure.
- Caught and closed a verify-by-consequence blind spot: lose-position tests with a single small matcher can't distinguish "order moved to back" from "order dropped" — fixed by sizing the matcher to sweep through the front order into the tracked one, so both fills emit and their order proves presence + position.
- Extended `modifyTest` with `ExpectedLevel` state assertions to close the ghost-order gap (no duplicate left at the old price level on a price-change).

### W4·5 — Cancel (W4 COMPLETE)

- `cancel(Id) -> bool` on OrderBook: look up id in cancel index → get list iterator → capture price as a value → erase order from its level's list (O(1)) → erase from index → remove level from map if now empty.
- **Safety call (defended under challenge):** capture price as a value before erasing (the list node dies on erase); confirmed all reads of the order reference precede the erase, so no use-after-free. The "don't copy" tenet is about whole Orders/containers, not an 8-byte price — capturing a scalar is free. Kept defensive find-guards for now; strip at benchmarking.
- **Self-describing-order insight:** the cancel index stores only the iterator. Side/price are derived from the order's own fields (the order knows where it belongs), so storing them again would be redundant, mutable-in-two-places state.
- **Harness refactored into a `Test` class**; added `CancelTest` alongside `runReplayTest`. Success signal is `cancel`'s bool return, NOT `contains` (which can't distinguish "cancelled" from "never existed").
- 4 cancel tests: single-order (level emptied, neighbour untouched), two-at-same-price (one cancelled, other + level survive), unknown-id (clean no-op), cancel-last-at-price (level removed). All pass.
- **[W7·2 note:** cancel-on-unknown-id being a clean no-op is what makes producer-side cancellation safe under the queue — a cancel enqueued for a rejected submit arrives for an id not in the book and does nothing.**]**

### W4·4 — Submit + Replay Harness

- Renamed `match` → `submit` (it matches AND rests the remainder — the full entry point). `rest` stays as the internal placement helper.
- `quantityAt(Side, Price) -> int64_t` — total resting quantity at a price (0 if absent); the state-inspection primitive. Reasoned that total-per-price + `best()` covers all cases; per-order/FIFO inspection deferred to W6 when the invariant needs it.
- `runReplayTest(name, sequence, expectedFills, expectedState)` — runs a scripted order sequence through `submit`, asserts actual fills == expected (count first, then element-by-element on price+quantity) and actual state == expected (via `quantityAt`). Readable failure output via `operator<<` overloads for `Fill`/`ExpectedLevel`/vectors.
- 5 replay cases: buy aggressor, sell aggressor (genuinely distinct, not a duplicate), rest-remainder, market order (remainder dropped), empty-book. All pass.
- Bugs caught by tracing: every-against-every fills comparison (kept last comparison only), self-comparison in states check (compared quantityAt to itself), price/quantity field-order swap in test data, duplicated sell test masquerading as coverage.
- **Deferred:** determinism check (run-twice-identical) → meaningful only at concurrency (W7), noted as conscious deferral. **[NOW LIVE — promoted to a named W8·2 test. `LoggedOp`/`invReplay` plus the W7·4 `Request → LoggedOp` conversion is the tool; no second replay engine needed.]** `ExpectedLevel` "checks listed levels, won't catch unlisted extras" — fine for authored sequences.

### W4·3 — Match loop (the hardest single day)

- `std::vector<Fill> match(Order&)` — the matching heart. While incoming has quantity and opposite side non-empty: grab best resting order (mutable, from own maps), check crosses, `tradeQty = min(both quantities)`, reduce both, emit Fill at resting price, pop filled resting orders, remove empty levels. After: rest limit remainder / drop market remainder.
- `crosses` generalised: market always; buy-limit `incoming.price >= resting.price`; sell-limit `resting.price >= incoming.price`.
- `withOppositeSide` lambda-template solves the "bids and asks are different types" problem (comparator makes them distinct types) cleanly.
- **`Fill` struct:** price (resting/execution price), quantity, aggressorId, restingId.
- Also erase from cancel index when a resting order fully fills (no orphaned index entries).
- Bugs caught by comparison-to-plan: `=` vs `==` in the zero check (silent), `min` of prices not quantities (the volume-conservation line), branch structure (loop trapped in wrong side), rest-on-full-fill, spurious "dropped" message on a fully-filled limit.
- Proven on canonical buy (2 fills: 100@102, 20@103; 30 resting) AND sell aggressor (highest-bid-first). Both directions correct.
- **No smart pointers:** the level owns the order; match only mutates + removes. A shared_ptr's atomic refcount would be pure cost on the hot path for a non-problem.
- **[W7·1 note:** `quantity -= tradeQty` in this loop is *load, subtract, store* — three operations, not one. That is the exact site of the lost-update failure mode if a second thread ever entered the book.**]**

### W4·2 — Book skeleton

- Built `OrderBook`: two side-maps (`bids` with reversed comparator, `asks` default), `Level { std::list<Order> }`, `best(Side)`, `rest(Order)`.
- Cancel index = `unordered_map<Id, list::iterator>` — populated in `rest`.
- **Design call made solo:** `best()` returns `const Order*` (not `std::optional<Order>`) — a handle to the *actual* resting order so the match loop can mutate it in place; `nullptr` signals empty. A copy would have broken matching. **[This is exactly the path that makes the book thread-unsafe — the anchor for W7·1's shared-mutable-state argument.]**
- `contains(Id)` added as a minimal public observer for the private index.
- 7 tests: top-of-book (lowest ask / highest bid), side independence, empty→nullptr, FIFO-within-level (by id), index population.
- **Known deferrals (W9 / tidy):** `rest(Order o)` takes by value → copies; could move into the list. Remove stray `<iostream>` from the header. Raw pointer from `best()` valid only until that order is filled/cancelled — fine for internal callers.

### W4·1 — Order type

- `Order` struct: side, type, price (int64 ticks), quantity, id, seq. Enums `Side`, `Type` as `enum class`.
- Public struct (pure data, no invariants of its own → no encapsulation needed; invariants live in the book).
- Defended cold: integer ticks (float equality unsafe), seq-not-clock (clocks collide/run backwards), id-vs-seq (identity vs temporal ordering).
- **[W7·2 note:** the id-vs-seq split is what makes the concurrency model work — `seq` becomes writer-assigned at pop time (the single definition of arrival order), while `id` stays producer-assigned so a producer can name its own order without a round trip.**]**

### W3 — Design on paper

- Traced the canonical matching example by hand (2 fills, 30 resting @103) — the replay-test oracle.
- Derived all four structures from the three requirements (fast best-price, FIFO, fast cancel), each with rejected alternative.
- Discovered the `std::list` stable-node property myself from the cancel requirement.
- Checkpoint: **Go** — continue in C++.

---

## W9 optimization candidates (NOT to act on before benchmarking)

Ideas surfaced during W5 while thinking about FIFO inspection. Recorded so the thinking isn't lost — but these are performance-speculative and must be measured, not assumed. The list-based core is correct and tested; do not rewrite it without benchmarks justifying the change.

- **Tombstone-vector vs list levels.** Replace `std::list` levels + iterator-splice cancel with a `std::vector` + `is_cancelled` flag on Order; cancel flips the flag, matching skips tombstones. Trade: O(1) splice-cancel → cache-friendly contiguous storage, but cancelled orders accumulate (needs compaction) and every match branches past dead orders. Which wins depends entirely on workload — a W9 measurement, not a guess. NOTE: the list is currently load-bearing — cancel removes from the *middle* in O(1) via stored iterator; a front/back-only structure can't do that.

- **Raw-pointer cancel index.** Only viable with *stable* storage. Stable with `std::list` (nodes don't move); DANGLES with a vector (reallocation on growth). So this conflicts with the tombstone-vector idea — can't have both. If levels stay list-based, a raw pointer/iterator is already what's used.

- ~~**Map unification.**~~ **DONE** — see "Map unification (pre-W6 cleanup)" above.

- ~~**Engine-assigned order ids.**~~ **RESOLVED at W7·2** — producer-partitioned ids (producer in the high bits, thread-local counter in the low bits). Uniqueness by construction with no engine round trip; `validate`'s duplicate check retained as defence in depth.

- **`rest(Order o)` by-value copy** → move into the list. Logged at W4·2, still awaiting a profile rather than an instinct.

- **Queue sizing / `Request` footprint.** `sizeof(Request)` is set by its largest variant (a full `Order` plus the tag, ~40–48 bytes), so a bounded ring buffer costs `capacity × sizeof(Request)` up front. A variant-based design could shrink it — measure before deciding it matters.

---

## Phase 3 decisions (all resolved in W7)

| Question | Resolution | Where |
|---|---|---|
| Concurrency model | Single-writer-with-queue (MPSC) | W7·2 |
| Rejected alternatives | Global lock (pointless) · lock-per-level (breaks fairness) · lock-free book (unverifiable) | W7·2 |
| Id assignment | Producer-partitioned, 8/56 bit split, thread-local counters | W7·2 |
| Queue element type | `Request`, separate from `LoggedOp`, one-way conversion | W7·4 |
| Bounded-queue overflow | Reject on full; `push -> bool`, never blocks | W7·4 |
| Response path | Designed (per-producer SPSC, routed by id high bits), scoped out of v1; W8·3 if slip allows | W7·4 |
| Queue depth dial | **MODEST** — mutex + condvar; lock-free rejected on verifiability | W7·4 |

---

## Next session

**W8·1 — Implement the MPSC queue + single-writer loop.** The first session to change the repo since W6.

Build: the `Request` tagged struct and its one-way `Request → LoggedOp` conversion; the bounded ring buffer with a mutex, a **writer-side** condition variable (predicate-form wait — spurious wakeups), and `push -> bool` returning false when full; `alignas(64)` separation of head and tail; producer-side id construction (**the bit-packing deep dive lands here** — shifts and masks, `int64_t` vs unsigned, the sign-bit hazard, construction and extraction, readable over clever); and the writer loop that pops, assigns `seq`, and dispatches to the **unchanged** `submit`/`cancel`/`modify`.

**Gate:** orders flow producer → queue → writer → fills, and the matching code is **diff-provably unchanged**. The 23-test suite, the 100k fuzz, and the shrinker must all still pass untouched — they validate a core this session must not have modified.