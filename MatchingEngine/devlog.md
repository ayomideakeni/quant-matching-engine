# Dev Log — Matching Engine

*Session-by-session build progress.*

---

## Phase map

| Phase | Focus |
|---|---|
| Design | Domain trace, structure design |
| Core | Order type → book skeleton → match loop → submit → cancel |
| Subtle ops | modify, edge cases, validation |
| Testing | Property-based invariants, shrinker |
| Concurrency (design) | Primitives, model derivation, queue design |
| Concurrency (build) | MPSC queue, single-writer loop, adversarial tests |
| Benchmarking | Percentile harness, depth sweep, profiling, optimisation |

---

## Status board
 
| Component | State | Tests |
|---|---|---|
| `Order` type | ✅ done | integer ticks, seq-not-clock, id-vs-seq |
| Book skeleton (maps, `Level`, `best()`, `rest()`) | ✅ done | covered |
| Cancel index population | ✅ done | covered |
| Match loop | ✅ done | canonical buy/sell, market, rest-remainder, cross-comparison |
| `submit` + replay harness | ✅ done | 6 replay cases |
| `cancel` | ✅ done | 4 cancel cases |
| `modify` | ✅ done | 5 modify cases |
| Edge cases | ✅ done | multi-level sweep, exact-match boundary; self-cross scoped out |
| `validate` / input rejection | ✅ done | 3 validation cases (dup-id, bad-qty, bad-price) |
| Map unification | ✅ done | full 23-test suite green |
| Property-based / invariant tests | ✅ done | 100k-op fuzzed run, 0 violations |
| Shrinker | ✅ done | proven against an injected known bug, 34→20 stable ops |
| Concurrency primitives | ✅ done | derived from this engine's own hot path |
| Model derivation + 3 rejects + id scheme | ✅ done | single-writer-with-queue; producer-partitioned ids |
| Thread reps + deadlock drill | ✅ done | race produced & fixed 2 ways; deadlock produced & fixed 2 ways |
| Queue design + depth dial | ✅ done | `Request` type, reject-on-full, response path scoped, mutex+condvar |
| Producer id packing (1 sign / 7 producer / 56 counter) | ✅ done | round-trip verified across 3 producers |
| `Request` + `Request → LoggedOp` conversion | ✅ done | exercised by both integration tests |
| `RingBuffer` (bounded MPSC, mutex + condvar) | ✅ done | 5 tests inc. 4-thread concurrent run, 30 consecutive clean |
| `writerLoop` (single writer, 2-phase drain) | ✅ done | 2 integration tests — submit, cancel, modify, matching |
| Fuzz through the queue + determinism check | ✅ done | 1.6 M ops clean; determinism proven both ways; shrinker 19→2 on injected bug |
| Concurrency hardening | ✅ done | 3 adversarial tests; clean under ThreadSanitizer; contention sweep 1/4/16 producers |
| Percentile harness | ✅ done | p50/p99/p99.9/max for all five operations + mixed flow |
| Book-size sweep | ✅ done | 4 depths, 10 → 10,000 levels |
| Concurrent path measurement | ✅ done | queue overhead quantified; batched drain measured against a controlled baseline |
| `waitAndDrain` — batched consumer pops | ✅ done | determinism re-verified over 1 M operations |
| Profile → optimise → re-measure | ⬜ next | — |
 
**Status: correct, concurrent, tested, and measured.** 23 hand-written tests · 100k single-threaded fuzzed operations across 4 invariants with zero violations · a shrinker proven against an injected bug · 1.6 M operations through the queue across 1–16 producers · determinism verified in both directions · clean under ThreadSanitizer · latency percentiles across five operations, four book depths, and the end-to-end concurrent path. The matching core is byte-identical to its single-threaded form; the concurrency layer wraps it without touching it.

---

## Session entries

Profiling — adjudicating the pre-registered hypotheses

The discovery gate. Everything until now was observation; this session attributes time to causes. Two hypotheses had been registered in advance, each supported by different evidence:

Allocation — predicted by submit-resting's max of 10,404 ns against submit-crossing's 191 ns, near-identical bodies, the difference being that resting allocates a list node every time.
Tree traversal — predicted by the 3–4× growth across the book-depth sweep, with tails staying proportionate rather than fattening (the signature of a systematic per-operation cost rather than a probabilistic one).

Registering both in advance is what makes the profile informative: it adjudicates a prediction rather than starting from nothing.

Tooling. No Xcode installed, so no Instruments — it ships with Xcode, not with the Command Line Tools. Used macOS's built-in sample instead: attaches to a running process, interrupts at intervals, records the stack, and writes a weighted call tree. Cruder than Instruments (no timeline, no per-thread UI) but it answers the question being asked.

Both are sampling profilers, which is worth understanding before trusting the output. They report where time is spent statistically, not exact counts, and they can under-represent things that are frequent but individually brief. At -O3 much of the code is inlined, so a function inlined into submit doesn't appear as itself — its time is attributed to the caller. That isn't the profiler lying; the code genuinely doesn't exist separately anymore, but it does make attribution coarser.

Built with -O3 -g: optimised for realism, symbols so the output isn't raw addresses.

Two methodology errors, both caught and fixed:

The first run was 80% process startup. 234 of the main thread's 285 samples were _dyld_start — dynamic linking, before any engine code ran. The workload simply wasn't long enough. Fixed by looping the benchmark until the binary runs 15–20 seconds.

sample attaches immediately, so it catches startup regardless of run length. Fixed with ./testprofile & sleep 2 && sample $! 10 -f profile.txt — two seconds of head start, then a ten-second window over steady state. _dyld_start dropped from 234 samples to 2.

A reading error worth recording too. The first attempt at analysis used grep -c to count symbol mentions — which counts lines, not sample weight. In a call tree the same function appears at many depths and across threads, so those ratios were meaningless. The weighted numbers are the leading integer on each line; everything below is parsed from those.

Result — writer thread, 4 producers:

	samples	share
Dispatch (real engine work)	942	53%
waitAndDrain	764	43%
other	75	4%

Almost all of the 43% is std::mutex::lock → _pthread_mutex_firstfit_lock_wait → __psynch_mutexwait — the writer blocked in the kernel waiting for the queue mutex. Not draining, not copying. Waiting. Across all threads __psynch_mutexwait totalled 628 samples, the second-largest entry in the whole profile behind submit itself.

Within the engine's own work, the picture is unambiguous:

Symbol	samples
malloc	413
_free	215
operator new	111
__tree_remove	30
__tree_balance_after_insert	20

Allocation ~600+ samples against ~50 for tree manipulation — more than ten to one.

Checking whether the contention was manufactured by the harness.

concurrentBench's producers push pre-generated requests in a tight loop with zero work between pushes — they do nothing but hammer the mutex as fast as the hardware allows. Real producers parse network messages and run risk checks; they'd touch the queue occasionally rather than constantly. So the 43% might be measuring the harness rather than the system.

Rather than building a calibrated producer-side delay (a spin loop, timed and tuned to a realistic arrival rate — real, but ~20 lines plus calibration), took the cheaper test first: re-profile at one producer, which eliminates producer-versus-producer contention entirely.

	4 producers	1 producer
Dispatch (real work)	53%	65%
waitAndDrain (mostly blocked)	43%	29%
__psynch_mutexwait (all threads)	628	62
std::mutex::lock	—	65

Contention scales with producer count, as the convoying hypothesis predicted. The absolute lock-wait weight collapses by an order of magnitude.

Stated precisely, because the first framing overreached. It is not correct to say contention "isn't an engine property" — the queue is part of the system, and the mutex is genuinely contended. What is correct: contention ranges from 29% to 43% of writer time across the producer counts tested, and where a real deployment would sit depends on producer-side work that hasn't been modelled. The 1-producer figure is not "the true number" either; it is the other extreme. Both are endpoints of a range.

And with contention stripped back, the allocation signal holds and sharpens:

Symbol	samples (1 producer)
malloc	101
_free	66
operator new	38
__tree_balance_after_insert	4
__tree_remove	0

~200 samples of allocation against 4 of tree rebalancing. Roughly fifty to one, and the ratio holds at both producer counts.

The verdict on the two hypotheses.

Allocation confirmed. It was ranked first on reasoning (a malloc is 50–100 ns against a whole submit of 30–60 ns, and it's variable, which is where tail latency comes from), predicted by the submit-resting tail, and the profile puts it fifty to one ahead of the alternative.

Tree traversal refuted — as stated. Comparisons and rebalancing are nearly free: __tree_remove at zero samples in the single-producer profile. But the depth curve was real and reproducible across four runs, so something does scale with depth. The resolution: it is the cache misses from chasing separately-allocated tree nodes, not the tree logic itself. Each hop in a red-black tree is a separately-allocated node and therefore a potential trip to main memory; the work is trivial, the memory access pattern is not.

Which means the depth curve is an allocation-layout problem wearing a different hat — and points at the same class of fix.

And it explains the earlier negative result on batching. Batched drains didn't improve p50 because the contention isn't per-acquisition overhead — it's the writer genuinely blocked while producers hold the lock. Reducing the number of acquisitions doesn't help when each wait is long. That result made no sense against a lock-overhead model; it makes sense against a blocked-on-contention model.

Stated limitations.

The profile covers concurrentBench, not the engine in isolation. It includes generateRequest, thread creation and joins — real time, but not engine time. A single-threaded profile of the mixed-flow benchmark would isolate the engine more cleanly. Not done, on the grounds that the allocation signal is decisive at both producer counts; worth stating rather than glossing.

Sampling profilers are statistical. These are proportions with sampling error, not exact accounting, and inlining at -O3 blurs attribution between caller and callee.

reportPercentiles runs per benchmark iteration and does I/O, so a small amount of the profile is print overhead rather than engine work.

Next: order pooling, with a measured justification rather than an assumed one. Every rest currently allocates a std::list node — a malloc on the hottest path, and a variable one. A pool replaces it with a free-list pop. The correctness suite (23 tests, both fuzzers, determinism, TSan) must stay green through it, and the before/after curve is what the whole phase was set up to produce.

### Measuring the concurrent path, and batched consumer pops
 
Everything measured until now called the book directly. This session put a number on what the concurrency layer costs, then attempted to reduce it — and the attempt produced a **negative result on the main claim** and a modest positive on the tail.
 
---
 
**What "latency" means here, and why there are two answers.** Single-threaded it was unambiguous: call `submit`, it returns, one thread, one clock, subtract. The concurrent path spans two threads, and a request's life has three phases:
 
- **Push** — construct a `Request`, take the mutex, write a slot, notify. Nanoseconds.
- **Queue wait** — sits in the buffer until the writer reaches it. **Not a fixed cost**: microseconds if the writer is asleep and must be woken, or 500 × execution time if 500 requests are ahead of it. Unbounded and load-dependent.
- **Execution** — pop and dispatch. The 30–60 ns already measured.
So there are two legitimate quantities. **End-to-end latency** (push to executed) is what a client experiences and is dominated by load. **Queue overhead** is what the design costs. The second is the one that decides anything, so it is the one measured.
 
**Rejected: timestamping each `Request`.** End-to-end measurement would need a timestamp field carried on every request — 8 bytes on the ring buffer's element type, permanently, changing its memory footprint, to answer a question that is mostly about how hard you push rather than how good the engine is. The same reasoning that kept the invariant checks behind an optional pointer applies: **don't make a measurement permanent overhead.**
 
**Chosen: batch-time the writer loop itself.** One thread, one clock, no cross-thread timestamp comparison, no new field. The measured quantity is `pop + dispatch`; subtracting the known direct-call cost isolates the pop.
 
Implemented behind a **separate `BenchContext*`**, distinct from the existing `WriterContext*`. One context per purpose rather than one context with modes — the correctness context exists to catch failures, the bench context to measure, and fusing them would mean every field implicitly carrying "which mode am I in?". A consequence worth watching: `writerLoop` now takes two optional pointers, which is approaching the point where an options struct would read better.
 
**A `std::thread` gotcha that cost a debugging cycle:** `std::thread` **does not apply default arguments** — it stores the callable and the arguments, then invokes them later, so it sees only what was supplied. Adding a defaulted fourth parameter broke every existing call site.
 
---
 
**Baseline — 4 producers, batch 10, five runs. Remarkably stable:**
 
| | value |
|---|---|
| p50 | **50.0** every run |
| p99 | 1425–1546 |
| p99.9 | 5308–5350 |
| max | 11.6k–18k |
 
**Against the direct-call mixed-flow baseline of p50 41.7 ns, the queue costs roughly 8 ns per operation** on the common path — the pop, the mutex, the dispatch branch. Less than an uncontended mutex acquire/release would suggest (15–25 ns), which makes sense: the lock is uncontended most of the time and some of the cost overlaps work the writer does anyway.
 
**The tail is a different story, and it is not the engine.** p99 at ~1500 ns is 30× the median, against ~2.5× in the direct-call benchmarks. Those are **condvar sleeps** — the writer emptying the queue, sleeping, and being woken, which costs microseconds.
 
**Doubling the operations changed nothing** (p50 identical, p99 1545 → 1650). If the tail were ramp-up and drain effects, a longer run would have diluted them. It didn't, so the queue is emptying *throughout* — four producers cannot outrun the writer.
 
**At 16 producers the shape inverts:**
 
| Producers | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| 4 | 50 | ~1500 | ~5340 | 11.6k–18k |
| 16 | 54 | 137–367 | 17k–30k | 91k–4.4M |
 
**More producers: fewer sleeps, worse worst-case.** The queue stays fed so the common case improves sharply, but when the writer *does* contend for the mutex against sixteen producers it queues behind them — convoying — and a 4.4 ms outlier is a thread descheduled while waiting.
 
**The variance across runs is itself the finding.** At 4 producers everything is reproducible: p50 exactly 50.0 five times, p99 within 8%. At 16, max ranges 91,508 → 4,419,980 across four identical runs — a 48× spread. **Heavy contention doesn't just cost more, it makes behaviour unpredictable**, which matters more than the averages in a latency-sensitive context. And it reinforces the standing rule: medians are reproducible, tails need repetition, a single max is an anecdote.
 
---
 
**The attempted optimisation: batched consumer pops.**
 
**The reasoning.** The writer took the mutex once per request: acquire → copy one request → advance head → release → dispatch. Ten operations meant ten acquire/release pairs. Draining N requests under a single acquisition should amortise that to near zero.
 
**`waitAndDrain(std::vector<Request>& out, size_t maxItems) -> size_t`** — takes the lock, waits on the existing predicate, then loops up to `maxItems` while `count > 0` reusing the existing private `takeRequestLocked()`. Returns how many were drained; **0 means stopping-and-empty**, the writer's exit signal.
 
- **By reference, not by return value.** Returning a vector would construct and destroy one per batch — allocation on the hot path, exactly what the change is meant to remove. The writer owns one, reserves it once, and reuses the memory forever. Same pattern as the test reserving `ctx.captured` before handing it over.
- **Dispatch happens outside the lock.** `waitAndDrain` returns having released the mutex; the writer then processes. Dispatching under the lock would block producers for N × ~42 ns, which is far worse than the behaviour being replaced.
- **`waitAndPop` retained** — it is the simpler contract and existing tests use it.
- **`processOne` restructured** from pop-and-dispatch into dispatch-only, taking `Request&` (not `const&` — `submit` mutates the order as it fills). The `nullopt` handling moved up into the drain call, and with it the flag, the partial-batch discard and the completed-counter all disappeared: **drains are complete units by construction**.
**A secondary benefit that turned out to be the only one:** each condvar wake-up now drains a whole burst rather than yielding one item and going back to sleep.
 
---
 
**Correctness re-verified first, and it caught a silent failure.** The first run failed determinism with the replayed book **completely empty** — `Corresponding Ids: []`. `ctx.captured.size()` was **0**, while the concurrent book had filled normally.
 
Cause: the new `BenchContext*` parameter meant every `std::thread` call site needed a fourth argument, and the fuzz test's writer was started without its `WriterContext`. So `ctx` was null inside the writer, and **every guarded block silently did nothing** — no capture, no invariant checks.
 
**The fuzz would have reported "no violations" while checking nothing.** Only determinism caught it, because it is the one test that compares against something rather than looking for the absence of a problem. That is the fifth instance in this project of *the test passed while exercising the wrong thing*.
 
Fixed: 23 tests, single-threaded fuzz, queued fuzz, TSan, and **determinism over 1,000,000 captured operations replaying byte-identical**. That last one is the real check on this change — draining 64 requests under one lock and dispatching them afterwards produces exactly the same execution sequence as popping one at a time.
 
---
 
**Result — controlled comparison at the same batch size (4 producers, 800k operations, cap 10 vs pop-one-at-a-time):**
 
| | before | after |
|---|---|---|
| p50 | 50.0 | 50.0–54.1 |
| p99 | 1425–1546 | 1462–1550 |
| p99.9 | 5308–5350 | **3954–4296** |
| max | 11.6k–18k | **7.9k–10.3k** |
 
**p50 unchanged. p99 unchanged. p99.9 down ~20%, max down ~40%.**
 
**The prediction was wrong in the main.** The ~8 ns of overhead versus direct calls is **not** lock acquire/release — if it were, amortising it across ten operations would have shown at p50 and it didn't. It must be the condvar predicate evaluation, the copy out of the ring buffer, or the dispatch branch. Worth noting the drain also *adds* cost: each request is now touched twice (written into the vector under the lock, read again during dispatch) rather than once, which is worse for cache and may be cancelling whatever the lock saving was.
 
**What did improve is the far tail**, and by the predicted mechanism: fewer sleeps, because one wake-up drains a burst. That it shows at p99.9 and max but not at p99 says sleeps were always a sub-1% event.
 
---
 
**A confounded result, caught before it was believed.** Sweeping the drain cap upward appeared to show dramatic tail improvement:
 
| cap | samples | p50 | p99 | p99.9 |
|---|---|---|---|---|
| 64 | 12,500 | 55.3 | 537 | 947 |
| 128 | 6,251 | 57.9 | 353 | 540 |
| 1024 | 782 | 58.7 | 108 | 149 |
| 4096 | 197 | 56.7 | 77.7 | 136 |
 
**The sample counts give it away.** 800,000 operations throughout, and every drain returned the full cap — so each sample is now the mean of `drainCap` operations. **This is the batch-size dilution trade in a new place:** at cap 4096, a 10 µs stall spreads across 4096 operations and adds 2.4 ns per operation, i.e. becomes invisible. The p99 falling from 537 → 78 is **mostly dilution, not improvement** — the same effect that made modify-in-place look tail-free at batch 1000.
 
The one figure not confounded by cap size is p50, and it drifts *upward* slightly (50 → 55–60) as the cap grows.
 
**So the only honest comparison is at equal batch size**, which is the table above. Quoting the cap-4096 numbers would have been a real misreport, and the tell was structural rather than statistical: sample count falling in exact proportion to cap size means the measurement unit changed, not the system.
 
---
 
**Assessment.** Batching bought a **20–40% reduction in the far tail and nothing on the common path**, for a new queue method and a restructured writer. Marginal, and worth keeping only because tail latency is the number that matters here and the code is not complex. The negative result on p50 is the more useful finding: it rules out lock overhead as the explanation for the queue's ~8 ns cost and points the profiler somewhere else.
 
**Caveat on all of these numbers:** the book grows throughout each run, so per-operation cost includes depth effects that vary across the measurement. These are not fixed figures for a fixed book.
 
**Outstanding:** the drain cap is currently a placeholder pending a proper sweep at constant sample resolution · `reportPercentiles` still hardcodes "batches of 10" in its label while several batch sizes are now in use.
 
---

### Percentile harness and book-size sweep

No engine code changed — all measurement infrastructure in `benchmark.cpp`. Two findings emerged that were not predicted by the pre-registered optimisation candidates.

**Starting position.** `benchmark.cpp` already had warm-up discard (in two of five benchmarks), multiple trials with the *median* reported rather than a single number, `steady_clock`, a fresh book per trial, five *isolated* per-operation benchmarks rather than one averaged mixed figure, and a checksum in the modify benchmark to defeat dead-store elimination. What was missing was per-operation distribution, a depth sweep, and any measurement of the concurrent path.

**Coordinated omission — considered and ruled out.** It is an **open-loop** problem: it bites when load arrives on a fixed schedule and a stall means you never measure what queued up behind it. This harness is **closed-loop** — call `submit`, wait for it to return, call again — so there is nothing to omit. It becomes relevant only if a rate-driven generator is ever built.

---

**Calibrating the instrument first.** Before adding per-operation timing, measured what the timer itself costs — because `submit` is 30–60 ns and `steady_clock::now()` is not free. If reading the clock costs 25 ns against a 40 ns operation, ~40% of every recorded number is the act of recording, and that is a **systematic bias** present in every sample in the same direction. More samples give a very precise estimate of the wrong number. **Precision and accuracy are different things**, and this is the cleanest illustration of it in the project.

Method: a million `now()` calls in a loop, accumulating `time_since_epoch().count()` into a printed sink so dead-store elimination can't delete them, minus an empty-loop baseline.

**Result: 16.8 ns per call, 33.7 ns per pair.**

**The baseline loop was itself deleted.** It reported **0 ms**. `empty_acc += i` over a known range is Gauss's sum, so the compiler evaluated it at compile time and removed the loop; printing the *value* kept the value alive but not the loop that computed it. So the subtraction subtracted zero and the 16.8 ns still includes loop overhead. Not serious in practice — that overhead is genuinely paid per measurement — but exactly the dead-store trap, biting in the branch that looked safe. **An unexplained fast result is a measurement artifact until proven otherwise.**

Mechanism worth recording: `now()` is a **barrier to optimisation** — an opaque call the compiler can't see into, so it can't move memory operations across it, and on some platforms a hardware serialising instruction. **Timed code can therefore compile differently from the same code untimed.** The act of measuring changes the thing measured.

---

**Batching, and the trade it makes.** A 33.7 ns pair against a 30–60 ns operation makes per-operation timing untenable — it would roughly double every number. So each sample times a **batch** and divides.

**Batch size trades measurement accuracy against tail resolution**, and no setting gives both:

| Batch | Overhead/op | A 10 µs stall reads as |
|---|---|---|
| 1 | ~34 ns (~85%) | perfect resolution |
| 10 | ~3.4 ns (~8%) | +1000 ns — obvious |
| 100 | ~0.34 ns (~1%) | +100 ns — blurring |
| 1000 | ~0.03 ns | +10 ns — lost |

Chose **10** for the four main benchmarks: ~8% overhead, stated, and a stall still shows as a batch ten times slower than its neighbours. Anything past 100 gives away the tail, which is the entire reason for measuring percentiles. **Modify-in-place needed 100** because it sits too close to the measurement floor at batch 10.

**Honest labelling:** these are percentiles **of batch means**, not of individual operations. Weaker than "p99 latency" and stated as such.

---

**Why percentiles rather than a mean.** A mean is pulled by outliers while hiding them: 99 operations at 40 ns and one at 10,000 ns averages to 139 ns, a number describing **no actual operation**. p50 says 40, p99 says 10,000, and both are true statements about real operations.

The tail matters more than the mean here specifically. At a million operations per second, p99 fires ten thousand times a second — it isn't rare, it's constant. And the slow ones **cluster at the worst moments**: allocation stalls happen when the book is deep, which is when the market is busy, which is when latency costs money.

---

**Results — five operations, batched, warmed up.**

| Operation | mean | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| Submit — resting only | 38.8 | 33.4 | 83.3 | 237.5 | **10404.1** |
| Submit — always crosses | 38.5 | 37.5 | 50.0 | 95.9 | 191.7 |
| Cancel | 28.8 | 29.2 | 41.7 | 70.8 | 154.2 |
| Modify — in-place | 3.6 | 3.34 | 5.83 | 10.83 | 19.58 |
| Modify — cancel+resubmit | 55.9 | 54.2 | 83.3 | 91.7 | 141.7 |

**Submit-resting's tail is pathological and submit-crossing's is not**, despite near-identical bodies (mean 38.8 vs 38.5). Max differs by **54×** — 10,404 ns against 191 ns. The mechanical difference between them is that **resting allocates a `std::list` node every time** while crossing mostly consumes existing ones. That is the top-ranked optimisation candidate showing up in data before any profiling — but consistent-with is not evidence-of, and the profiler adjudicates.

A *batch mean* of 10,404 ns means that batch took ~11 µs, so a **single** operation inside it was likely far worse. The true max is worse than this harness can see — an inherent limit of batching, stated rather than hidden.

**Cancel has the tightest distribution** (p99 only 43% above p50, max 5×) — index lookup and list erase, no allocation. Which makes its behaviour in the depth sweep more surprising, not less.

**Modify-in-place initially reported 2.8 ns mean with p50 4.1** — a mean *below* the median, impossible for a right-skewed latency distribution. That was quantisation against the ~3.4 ns measurement floor, not a distribution. Batch 100 lifted it clear. **The absolute number still carries a caveat:** ~3.3 ns is roughly 10 cycles, and the operation does a hash lookup, an iterator dereference, branch checks and a write. It is plausible only because this benchmark is unusually cache-friendly — a contiguous `ids` vector accessed sequentially, ~90 price levels reused, whole working set in L1. The honest claim is *"3.3 ns under a sequential access pattern with a small working set"*, not *"modify costs 3.3 ns."*

---

**The book-size sweep.** Every number above is at one depth, and reporting a single figure implicitly claims the curve is flat. `std::map` is a red-black tree: finding a level is O(log n) comparisons but also **O(log n) separately-allocated nodes**, so it is O(log n) *potential cache misses*. At a hundred levels the tree sits in L2 and lookups are nearly free; at ten thousand, each hop is a probable trip to main memory.

**Design decision — constant orders per level, not constant total.** Sweeping depth with a constant *total* order count would trade tree width against list depth, moving two variables at once. Constant orders-per-level isolates tree cost, at the price of total memory growing with depth.

**Design decision — parameterise the seeding, not the generator.** Widening `generateRequest`'s price band to fill a deep book would also collapse the crossing rate (the 1–100 band was chosen deliberately to maximise matching pressure), again moving two variables. So the isolated benchmarks got a `levels` parameter distributing the seed across N prices, with timed operations spanning the full band — otherwise the tree is wide but only one path through it is ever walked. `generateRequest` is used separately for a **mixed-flow** composite at a single depth.

**p50, reproducible to within a nanosecond or two across four runs:**

| levels | sub rest | sub cross | cancel | mod qty | mod price |
|---|---|---|---|---|---|
| 10 | 20.8 | 41.7 | 62.5 | 6.2 | 100.0 |
| 100 | 20.8 | 45.9 | 83.4 | 6.7 | 137.5 |
| 1000 | 29.1 | 50.0 | 120.8 | 7.5 | 191.7 |
| 10000 | 62.5 | 58.4 | 204.1 | 7.1 | 391.6 |

**Growth from 10 → 10,000 levels: submit-resting 3.0× · submit-crossing 1.4× · cancel 3.3× · modify-qty flat · modify-price 3.9×.**

**Everything that touches the map scales with depth; the one operation that doesn't touch the map is flat.** Modify-price scales worst because it does the most map work — a cancel plus a submit.

**Mixed flow** (weighted operation mix via `generateRequest`, depth 100): mean 44.6 · p50 41.7 · p99 104.2 · p99.9 187.5 · max 679.2. Stable across runs.

---

**Cancel scaling 3.3× was not predicted, and reading the code explains it.** Cancel does a hash lookup and an O(1) list erase — neither should care about level count. But the cleanup step is:

```cpp
if (map.at(p).orders.empty()) { map.erase(p); }
```

`at(p)` walks the tree; `erase(p)` walks it **again** to find the same node, then rebalances. Plus the traversal that located the order in the first place. **Three tree traversals per cancel where one would do.** The fix needs no new data structure — `find` once, test through the returned iterator, and `erase(iterator)`, which is O(1) amortised with no second search. (`at()` also bounds-checks and throws, which is unnecessary when the level is known to exist.)

**Deliberately not fixed yet.** It is a candidate found by measurement; fixing it now would mean the baseline no longer matches the profile, destroying the before/after curve.

---

**What this does to the ranked candidate list.** Two findings, two different signatures, not in conflict:

- **The tail** (submit-resting max 10,404 ns vs crossing's 191 ns, identical bodies) points at **allocation** — a rare, expensive event.
- **The depth curve** (systematic 3–4× growth, tails staying *proportionate* rather than fattening) points at **tree traversal** — a constant, growing tax.

The proportionate tails are the informative part: if depth were causing *occasional* expensive events, p99/p50 would widen as misses became more likely. It doesn't — the whole distribution shifts together, which is what a systematic per-operation cost looks like rather than a probabilistic one.

So the candidate list is amended: **the flat-price-array candidate is now evidence-backed rather than merely reasoned**, and **cancel's redundant traversals** are a new, concrete, zero-risk candidate that wasn't on the list at all.

---

**Two measurement artifacts characterised, both worth keeping as method.**

**Cold-start is per-process, not per-benchmark.** The first sweep showed the 10-level row with a p99 of ~100–108 ns for submit-resting against a p50 of 20.8 — a 5× ratio nothing else exhibited. Reversing the sweep order to `{10000, 1000, 100, 10}` moved the anomaly to the 10,000-level row, proving it followed *position*, not depth. Per-benchmark warm-up does not cover it, because caches and branch predictors are cold at **process** start. Fixed with a process-level warm-up run before the sweep; p50s were always stable, and the tails stabilised too (cancel p99 at 10,000 levels: 795.8 → 266.6).

**Single runs can be off by 4× in the tail.** After the process warm-up, cancel's p99 at 10 levels read 591.7 in one run and 125.0 in the next, with everything else unchanged. Noise, not signal — and a reminder that the *medians* were reproducible to a nanosecond throughout while the tails were not. **Tail statistics need repetition in a way medians don't.**

**One unexplained result, recorded as unexplained:** submit-crossing's p99 at 10 levels ran 145.9 / 191.7 / 220.8 across runs, against ~70–79 ns at every other depth. Persistent in direction, wildly variable in magnitude. Plausible mechanism — at 10 levels the aggressor is far more likely to consume an entire level and pay the erase path — but not confirmed.

**Outstanding:** `reportPercentiles` hardcodes "batches of 10" in its label while two different batch sizes are now in use.

---
### Concurrency hardening — adversarial tests and ThreadSanitizer

No new engine code. This session finished the outstanding test coverage and ran the first external verification the project has had.

**Reject-on-full, finally exercised.** The overflow policy had, until now, **never been tested**: every queue in every prior test was oversized, so `push` had never returned `false` in anger. Now tested both as a unit (fill a 4-slot queue, confirm rejection, drain and confirm exactly capacity items come out uncorrupted) and under contention.

**Cancel-mid-match gate fixed.** The previous result was 500 aggressor-first, 0 cancel-first — the start gate synchronised *release* but not *readiness*, so the first-constructed thread had been spinning on the flag for however long the second thread's construction took, microseconds that are an eternity at this scale. With readiness signalling added the split is roughly **51–124 cancel-first / 376–449 aggressor-first** across runs: both orderings genuinely exercised, and in every cancel-first case the book ended in the legal cancel-first state.

**Also closed:** the shrink log now reads `removed.id` rather than `removed.order.id`, which was always `0` for cancel and modify since those carry `Order{}`. And the unpacked ids seen in determinism runs (`110`, `1200`, `9742`) turned out benign — **producer 0's high bits are zero**, so its packed ids *are* small integers. The bit-packing working as designed, briefly misread as state contamination; confirmed by printing `totalRestingVolume()` as `0` immediately after construction.

---

**Producer-outruns-consumer, and the design point it demonstrates.** Four producers, 5,000 operations each, into a 512-slot queue, against a writer slowed by full instrumentation. Producers use a **retry loop with spin-then-yield backoff** (64 spins before yielding — spinning is cheaper than a syscall for short waits, yielding avoids burning a core for long ones) rather than dropping on rejection.

The conceptual point: **reject-on-full does not remove backpressure, it relocates the decision.** The queue refuses; the *producer* chooses what to do about it. Here it absorbs the pressure locally by retrying. In the reject-on-full test the producer chooses to drop and count. Under blocking, neither choice exists — one policy is imposed on every producer regardless of what that producer needs. The two tests are the paired demonstration.

Assertions: `captured.size() == totalOps` (no operation lost under sustained backpressure), `totalRetries > 0` (**the test proved something** — zero retries would mean the writer kept up and the run was vacuous), all four invariants, and total resting volume exactly `totalOps × 10`.

**The retry count needs context or it misreads.** ~48–50 million retries for 20,000 operations. That is a measure of how saturated the queue was, driven by the instrumented writer running O(book size) checks per operation and therefore being thousands of times slower than the producers. It is not a property of the engine — same caveat as the 99.4% rejection figure from the reject-on-full test.

---

**ThreadSanitizer — clean across the full suite.**

`clang++ -std=c++23 -fsanitize=thread -g -O1`, full suite. **No races reported anywhere**, including the cancel-mid-match test (which deliberately races two threads through a start gate), the backpressure tests hammering the queue mutex from four producers, and 15,000 fuzzed operations through the queue.

**Why this matters specifically.** Every check written so far verifies that the *book ends in a valid state*. TSan verifies something categorically different: that no two threads accessed the same memory concurrently without synchronisation, **regardless of whether the outcome happened to be correct**. A data race is undefined behaviour even on runs that produce the right answer — which is exactly the class of bug that survives testing. It works by instrumenting every memory access and maintaining a happens-before graph from synchronisation events (mutex acquire/release, thread create/join, atomics with appropriate ordering); an unordered concurrent access with at least one writer is reported with both stack traces.

**It closes an argument rather than merely passing.** The lock-free queue was rejected on the grounds that a too-weak memory ordering is silent and **this project's test infrastructure is structurally blind to it**. TSan is precisely the infrastructure that *would* detect that class of bug. Running it converts "the mutex design's reasoning is simple enough to verify by inspection" from an assertion into a corroborated claim.

**Stated precisely, because the limitation is real:** TSan is **dynamic** — it only observes paths actually executed, so a clean run means no race occurred on any path this suite exercised. Given the suite includes deliberate races, saturation and fuzzed flow, that is substantial coverage, but it is evidence rather than proof. It also detects **races**, not all memory-ordering errors: an `acquire` where `seq_cst` was needed is a correctly-synchronised access with insufficient ordering, and TSan can pass while the code is wrong.

**One process note:** the first TSan run printed only two lines. Most of the suite was disabled, so TSan had observed a single test. Re-enabled and re-run for the result above. That is the fourth time in this project that *"what did that run actually exercise?"* has been the decisive question. The practice is now explicit: **after any change to what runs, verify what ran.**

---

**Sustained flow.** 1,000,000 operations across 3 producers, clean, 5:13 wall. 1,600,000 operations across 16 producers, clean, 4:03. Determinism held on every run.

---

**The contention sweep — a methodology correction and a real finding.**

**First attempt was invalid.** Producer counts of 1/4/16 were run at 100k ops *each*, so total operations scaled with producer count: 100k / 400k / 1.6M. Three variables moved at once, and the dominant one was not contention. The instrumented writer's checks are **O(book size) per operation** and the book grows with op count, so total work is roughly **quadratic in operations**: 4× the ops gave 35× the time (quadratic predicts 16×; the excess is plausibly the book outgrowing cache levels, making each traversal slower per element as well as more numerous).

**Redone with total operations held constant** at 400,000 — 400k×1, 100k×4, 25k×16 — so book size, total checking work and total pushes are identical and the *only* variable is thread count:

| Producers | Wall time |
|---|---|
| 1 | 2.28 s |
| 4 | 5.05 s |
| 16 | 9.06 s |

**Roughly doubling per 4× in producers — a real contention cost.** The prediction going in was "no measurable difference," reasoning that the writer is thousands of times slower so the queue sits empty and producers rarely collide. **That reasoning was incomplete: producers contend with each other, not only with the consumer.** Sixteen threads whose entire job is lock → write → notify → unlock, hammering one mutex as fast as they can, is close to worst-case contention regardless of what the consumer is doing.

Three plausible contributors, stated as hypotheses:
- **The writer shares that mutex.** It takes the same lock on every pop, so producer contention **starves the consumer** — the writer queues behind producers to get in, and since the writer is the bottleneck, slowing it slows everything. This is convoying, and it is the same phenomenon that made the global lock unattractive, surfacing in the one place a lock survived.
- **`notify_one` fires on every successful push**, inside the critical section, even when the writer isn't waiting.
- **Oversubscription** — 16 threads on ~10 cores means threads are descheduled *while holding the mutex*, so everyone waits on a thread that isn't running.

**Caveat:** single runs, no repetition, no variance, and the instrumented writer dominates absolute times. The *trend* across three points is consistent enough to believe; the individual numbers are not measurements. Recorded as a **known characteristic with a hypothesis**, not a measured result.

**Also recorded:** none of this session's timings say anything about the engine. They measure the instrumented build, where checking dominates matching by orders of magnitude. A 30-million-operation run with prints enabled took 40 seconds of CPU and over 90 minutes of wall clock — ~99.99% of it terminal I/O. **Printing per operation makes the print the workload.**

---

### Fuzzing through the queue + determinism check

Everything new here is test infrastructure plus a `WriterContext` parameter on `writerLoop`. The book gained read-only observers (`totalBidVolume`, `totalAskVolume`, `totalRestingVolume`, `totalCancelIndexVolume`, `idsAt`) and a `const` overload of `getMap` — all `const`, pure observation. **No matching logic changed.**

**The restructure the queue forces, and why it isn't a workaround.** Single-threaded, `generateAndExecute` is *generate → execute → check*, fused, on one thread, reading the book between steps: `getOrderInfo` for modify's percentage scaling, and `restingIds` pruned by observing outcomes. Behind the queue those three things live in three places — generation on a producer thread, execution on the writer, checking wherever the book is reachable, which is only the writer. **The generator cannot read the book at all.** That is the design working, not a limitation to route around, so the existing function could not simply be pointed at `push`.

**The single-threaded fuzzer was deliberately kept, not replaced.** The two test different things: single-threaded asks *is the matching logic correct?* against a book it can read directly with no threading in the picture; queued asks *does the concurrency layer preserve that?* If the queued one fails and the single-threaded one passes, the bug is localised to the concurrency layer in one run.

**Three honest consequences of blind generation:**
- **Modifies are absolute, not proportional.** The old `/100` scaling existed to stop price drift compounding across repeated modifies on one order; absolute values from a bounded range can't drift, so the problem it solved doesn't arise — but the operation is a different shape.
- **Nominal weighting ≠ effective weighting.** The roll is still 50/30/20, but cancels and modifies targeting ids that already filled or were rejected become clean no-ops. The mix should be reported as nominal.
- **The stream is fully blind and fully deterministic** — generated before anything executes, so it can't react to outcomes, but it's replayable without re-rolling the rng, which is what makes a failure reproducible.

---

**`idWindow` — a recency heuristic, not simulated reaction.** A fixed 512-slot circular store of recently-issued ids with a write cursor and a fill count; `record` overwrites the oldest, `pick` returns a uniformly-chosen entry (`optional`, `nullopt` when empty). **Framing kept precise:** this does *not* simulate reaction — reaction means responding to actual outcomes, which is impossible from a producer thread. It is a recency heuristic: recently-issued ids are statistically more likely to still be live, so a rolling window raises the proportion of cancels and modifies that actually do something. Without it, the target list grows unboundedly and a 100k run generates mostly no-ops in the back half.

Simpler than the `RingBuffer` for two reasons: no head index (nothing is consumed, only overwritten) and no full/empty ambiguity (full is the steady state). Capacity is a power of two so the wrap is `& (capacity - 1)`.

Tested in isolation before use — empty returns `nullopt`; under-filled samples only from what was recorded; **and after 600 records into 512 slots, only the last 512 are reachable.** The first two pass with broken wrap arithmetic; the third is the one that proves it.

---

**The bug that made the first clean run meaningless.** The first full queued fuzz reported 99,000 operations with zero violations. It was worthless: `generateRequest`'s bootstrap condition still tested `gen.restingIds.empty()` — the *old* fuzzer's list, which the new code never populates, since ids are recorded into `reqWindow` instead. So `restingIds` stayed empty for all 33,000 iterations per producer, the bootstrap branch fired every time, and **the `else` containing all three operation branches was never reached.** 100% submits, zero cancels, zero modifies.

Every individual piece was correct — the window worked, the branches were right, the roll was right. The bug was a **leftover reference to a structure that is no longer maintained**, and it produced a run that looked entirely successful: sensible ids, no assertion failures, exactly `iterations` requests out.

**Caught by `./tests | grep -c "Cancel Request"` → 0.** That is now three instances in this project of *the test passed while exercising the wrong thing* — the inverted type-weighting in the original fuzzer, the duplicate-id false pass in the first integration test, and this — and **all three were caught by checking output composition rather than trusting the pass.** After any change to a generator, verify the mix, not just the result.

**A second, smaller instance in the same function:** capture came out at 98,991 rather than 99,000 — three short per producer, too regular to be noise. Cause: `continue` inside a `for` loop still advances the counter, so a skipped iteration consumed one of the 33,000 without producing a request. Converted to a `while` that runs until the requested count is actually met.

---

**Volume conservation, and where it had to live.** The fourth invariant was never a book method — it is a property of *one submit in isolation*, which is why it lived in the harness against returned `Fill`s. The writer discards fills, so it was initially absent from the queued path.

Resolved by checking it in the writer, which is the only thing holding both the book and `submit`'s return. **The general point:** behind the queue, invariant checking is exactly as easy as it was single-threaded — the writer sees a serial stream of operations against a book nobody else touches. The only thing that changed is *where* the checking code sits, not what it can observe.

The check used is stronger than the original per-fill version: capture `totalRestingVolume()` and the incoming quantity **before** `submit` (which takes `Order&` and decrements it as it fills — reading quantity afterwards gives the remainder, so the check would compare against 0 and silently pass), then compare the delta. Limit: `after − before == Q − 2T`. Market: `after − before == −T`. Rejected (`nullopt`): delta 0 **and** zero fills. That last distinction is exactly what `optional<vector<Fill>>` exists for — collapsing rejected with accepted-but-no-fills would make a rejected order look like a violation.

**Extracted as a pure predicate `volumeConserved(before, after, incomingQty, tradedQty, type, rejected)`** rather than a helper that performs the submit. A helper owning the submit inverts the relationship — the check should observe the operation, not own it. A predicate over five integers deduplicates the equation (needed in both `writerLoop` and `invReplay`) without routing anything through anything, and it is **independently testable**: eight direct cases with no book and no threads, including four negatives, since a predicate that always returns true passes every positive test.

Caught while testing it: all three branches were initially **inverted relative to the function name**, returning true on violation while the caller negated — a double negative meaning violations were silently missed and correct behaviour flagged. Exactly what extraction-then-direct-testing exists to find.

---

**`WriterContext` — how information crosses the thread boundary.** The writer can't return anything and can't usefully assert, so the context carries: the captured request stream, an optional invariant enum plus the index it fired at, and the offending orders. Passed as `WriterContext*` defaulting to `nullptr`.

**Pointer over `#ifdef`, deliberately.** A compile-time flag means the checking code isn't compiled in normal builds, so it can rot silently, and "did I compile with the flag?" becomes a question — uncomfortably close to the commented-out-checks failure mode. The pointer costs one perfectly-predicted branch per operation, always compiles, and makes the writer honest about not requiring any of it. The benchmark simply passes nothing.

**Why capture is needed at all, given the requests already exist as a vector.** That vector is what was *generated*, not what was *executed*. Multiple producers interleave unpredictably, so the writer's pop order is an arbitrary merge that differs every run; rejected pushes never arrive; and **the specific interleaving is often what caused a violation.** The queue's output order is information that exists nowhere else — created at pop time, gone if not recorded there. Capture happens **before** dispatch so a failing request is always in the record.

**After a violation: stop checking, stop capturing, keep draining.** Three behaviours, all keyed off `ctx->invariant.has_value()`. A writer that stopped draining would leave producers pushing into a queue nobody empties, so the queue fills, `push` returns false, the worker's assert fires, and the failure appears to be in the ring buffer — three layers from the actual bug. **The fuzzer is designed so a violation surfaces where it happened, not where it propagated to.**

`ctx.captured` is `reserve`d by the *test* before being handed over, so the writer never reallocates on the hot path — the writer doesn't know why, which keeps test knowledge in the test.

---

**A determinism-breaking bug in `invReplay`, found while making it the shrinker's oracle.** `invReplay` did `book.submit(sequence[i].order)` — a reference into the live vector — and `submit` mutates the order's quantity as it fills. So **after one replay the stored orders had modified quantities**, and orders that fully filled sat at 0, which `validate` then rejects. The shrinker calls `invReplay` hundreds of times over overlapping subsets of one vector, so every replay after the first ran against a progressively degraded sequence, changing whether the failure reproduced and therefore the shrinker's keep/discard decisions.

Fixed by copying each order into a local before submitting. `invReplay` is now **pure** — same input, same verdict, every time — which is what a shrinker oracle has to be. This plausibly contributed to earlier shrink results stalling above the true minimum, which was attributed to the algorithm at the time.

**Also fixed in passing:** every invariant check in `generateAndExecute` and `invReplay` had `return` *before* the diagnostic `std::cout`, so the message was dead code and violations reported silently. And the fuzzer's cancel branch called `indexDist(gen.rng)` **twice** — once for the `getOrderInfo` lookup and once for `idx` — so the order logged to history could differ from the order actually cancelled, corrupting shrinker input.

**Replaying through `writerLoop` instead of `invReplay` was considered and rejected.** It would need a queue, a thread, a shutdown and a join per shrink attempt — thousands of thread creations for twenty-operation sequences — and, decisively, it would reintroduce scheduling into what must be a **deterministic** oracle. If the verdict ever varied, the shrink loop would thrash, reinstating operations it had correctly removed. The invariants are not multithreaded properties; once the queue has determined the order, replay is pure.

---

**The queued fuzz: 99,000 operations across 3 producers, zero violations.** Verified composition rather than trusting the pass — modifies appearing in all four combinations (price-only, quantity-only, both, neither), cancels firing, mix roughly matching the weighting.

**And proven capable of failing.** The sell-crossing condition was reverted to the known-buggy form, the queued fuzz caught the crossed-book violation, captured 19 operations, and the shrinker reduced it to **2** — the theoretical minimum for a crossed book, since it takes exactly two orders on opposite sides that should have matched and didn't. Bug reverted, re-run clean.

That run validated three things a clean run cannot: the **writer-side checks are live** rather than inert; the **capture ordering is correct**, with the failing operation last; and the **`Request → LoggedOp` conversion works**, needing no changes to the shrinker at all.

---

**The determinism check.** The claim: given the sequence the writer actually executed, replaying it single-threaded through a fresh book produces **exactly the same final state**. The concurrency layer decides *what order* things happen in, and nothing else.

**Comparison granularity was a real decision.** Total resting volume catches gross divergence and misses almost everything. Per-level quantities catch structural differences. **Per-order, in queue order, is the one that matters:** two books can hold identical level quantities with orders queued in different positions — and the next aggressor to hit that level would fill a *different participant's* order. That is price-time priority broken, and every weaker check calls the books identical. So the weaker checks test that quantities survived; only this one tests that **arrival order** survived, which is the actual claim.

Required `idsAt(Side, Price) const -> vector<Id>` — walks a level front to back, so the returned vector *is* the queue position. Two bugs while writing it, with a shared root cause: `getMap` had no `const` overload (so a `const` method couldn't call it), and `map[p]` **inserts a default-constructed level if the price is absent**, meaning the observer would mutate the book it was inspecting. Adding the const overload fixed both at once — a `const map&` has no `operator[]`, forcing `find`. **The const-correctness violation and the accidental-insertion bug were the same bug**, and taking constness seriously surfaced it.

`deterTest` walks prices 1–100 (the generator's band, so nothing can rest outside it) comparing `idsAt` on both books for both sides with `vector::operator==`, which checks size then element-by-element in order.

**Proven both ways.** Injected a skip in the writer so it executed some operations without capturing them; the check failed and **localised it to a single missing id at one price with surrounding queue positions intact** — a granularity no weaker check could reach. Reverted: 99,000 operations, byte-identical books.

**One vacuous-test scare worth recording:** the first version passed the already-populated book to the replay instead of a fresh one, so it compared a book against itself and would have passed regardless. The tell was `CROSSED BOOK DETECTED at iteration 0` — iteration 0 of a *fresh* replay cannot cross, since an empty book has one side.

---

**Cancel-mid-match.** 500 iterations, each: rest an order, wait on an atomic counter until the writer has processed it (synchronising on the writer's own progress rather than by reading the book, which a test thread must not touch), then release two threads through a start gate pushing a crossing aggressor and a cancel for the resting order. After joining and shutdown: all four invariants, `captured.size() == 3`, and — the sharp assertion — **exactly one of the two legal outcomes**, cancel-first (aggressor rests, 50 on the sell side) or aggressor-first (full cross, both sides empty). `cancelFirst != aggressorFirst` rules out any state no interleaving could produce.

**`ctx.processed`** — an atomic counter added for that synchronisation. It is the **only** field in `WriterContext` read while the writer is still running; everything else is written by the writer and read after the join, where the join itself establishes ordering. So this is the one place in the whole design where memory ordering is load-bearing rather than decorative — `acquire`/`release` pairing.

---
### MPSC queue + single-writer loop

First session to change the repo since the testing phase. Everything built here is **new code outside the book** — `submit`, `cancel`, `modify` and the match loop are byte-identical, and the full existing suite (23 tests + 100k-op fuzzer + shrinker) passes untouched. That is the gate, and it held.

---

**Producer id construction (deferred from the model derivation).** `producer` struct — a `producerId` plus a plain **non-atomic** `int64_t counter`, with `nextId()` packing and post-incrementing. Named constants (`producerBits`, `counterBits`, `producerShift`, `maxProducers`) rather than magic 56s and 255s.

- **Layout: 1 sign bit clear · 7 bits producer · 56 bits counter.** The producer field was cut from 8 bits to 7 for a specific reason: a field of width *w* at shift *s* occupies bits *s* through *s+w−1*, so an 8-bit field at shift 56 reaches **bit 63 — the sign bit**. Shifting into the sign bit of a signed type is UB, and even where it "works" you get a negative id that surprises everything assuming ids are positive. 7 bits keeps the top bit permanently clear and still gives 128 producers, far past any realistic need. (`uint64_t` would recover the bit and allow 256 producers — rejected because `Id` is already `int64_t` throughout `Order`, `cancelIndex`, `validate` and every test, and one unused bit is not worth that churn.)
- **`static_cast` before the shift, not after.** `producerId` is an `int`; shifting a 32-bit value left by 56 overflows the `int` and is UB *before* the result would ever be widened. Widen first, then shift.
- **Why OR works, and what it depends on.** The shifted producer is all zeros in the low 56 bits and the counter is all zeros in the high bits, so at every bit position at most one operand has a 1 — the fields **never contend** and OR merges them cleanly. That disjointness is the whole mechanism: exceed the counter's 56 bits and it would bleed into the producer field silently, and producer 3's ids would start decoding as producer 4's. 2^56 ≈ 7×10^16 ids per producer — at 1 M orders/sec, over two thousand years to exhaust. `|` over `+` because for disjoint fields they're identical, but `+` **carries** if the fields ever do overlap (corrupting the high field too) while `|` keeps the damage local — and `|` documents intent: independent fields merged, not numbers summed.
- **Modular partitioning (`id % N == p`) rejected**, having been considered properly: it is equally collision-free (every integer has exactly one remainder mod N), but **N is baked into every id** — add a fifth producer and every id already issued decodes to the wrong producer, so the scheme isn't stable under growth. Extraction is also integer division (~20–40 cycles) against one shift.
- **Verified by round-trip:** three producers, ten ids each. Blocks start at 2^56, 2×2^56, 3×2^56 — three disjoint regions — counters increment in the low bits with the high portion untouched, every id decodes to its issuer, nothing negative.

---

**`Request` — the queue element.** Flat tagged struct: `OpType` tag, `Order`, `Id`, `optional<Price>`, `optional<Quantity>`. Aggregate (no constructor), brace-initialised.

- **`id` is a plain `Id`, not an optional** — cancel and modify *always* have one, so optional would mean "may legitimately be absent," which is false, and every use site would unwrap something always present. Contrast `newPrice`/`newQuantity`, where absent genuinely *means* "leave unchanged."
- **A `variant` was the more principled design and was deliberately traded away.** A variant makes irrelevant fields *unrepresentable*; the flat struct makes it a convention the tag enforces. Accepted cost: cancel and modify carry a meaningless zeroed `Order`, and `Order{}` is used rather than plausible-looking values specifically so it fails loudly if ever read. Documented in a comment, since the type can no longer say it.
- **`Request → LoggedOp` conversion turned out to be one line.** The two structs are currently field-identical, so the conversion is a straight copy with nothing to dispatch on — a branching version was written, found to be doing nothing, and deleted. **The value was never in the function; it is in the types being separate.** The moment benchmarking adds a timestamp to `Request` for latency measurement they diverge, and the copy is the price of that independence. Honest state: trivial today, load-bearing later.

---

**`RingBuffer` — bounded MPSC queue, mutex + condvar (the Modest dial).** Fixed-capacity `vector<Request>` allocated once at construction, `head`/`tail`/`count`, one `std::mutex`, one `std::condition_variable`, a `stopping` flag.

- **A class, not a public struct** — `head`, `tail`, `count` and the storage are invariants that must stay consistent, so reaching in from outside would move correctness from the type to every caller. Deliberately the opposite call from `Order` (public struct, pure data, no invariants of its own): same reasoning, different answer.
- **Full vs empty resolved with a count.** With head and tail alone, empty and full are *both* `head == tail` — identical state, opposite meaning. Options were sacrificing a slot (tail never catches head) or a separate count. Count chosen: it uses every slot and expresses the condvar predicate directly ("is there anything to pop" is a direct read). Accepted cost is a third piece of state that must be updated on every push and pop or it drifts.
- **Built and proven single-threaded first, mutex added after** — deliberately, so wrap-around arithmetic and concurrency weren't being debugged simultaneously. Four tests: fills-and-refuses, FIFO-order-out, drains-and-refuses, and the wrap test (push/pop twelve times so both indices wrap three times). **The first three pass with completely broken wrap arithmetic** — on a fresh queue you never reach the end of the array — so the wrap test is the only one that proves the modulo.
- **The mutex wraps the whole method body, not individual lines.** `push` is four steps (check full → write → advance tail → increment count) and the invariant spans all four. Two producers with one slot left would both read not-full, both write to **the same slot** (neither has advanced tail yet), then advance tail twice for one item and double-increment the count — the lost-update race corrupting a data structure rather than a counter. Exactly the atomic-vs-mutex distinction from the account-transfer drill: an atomic makes one variable's operation indivisible, a mutex makes a region exclusive.
- **Concurrent test:** four producer threads × 5,000 pushes into an oversized queue, drained on the main thread after joining. Checks total popped equals total pushed, every id decodes to a valid producer, and — the check that actually detects corruption — **each producer's ids come out in ascending order relative to each other**. Global order is meaningless (that's the point), but one thread pushed its own items one at a time, so a torn slot, a skipped slot or a duplicate would break that per-producer sequence. Thirty consecutive runs clean.
- **Stated honestly: thirty clean runs is evidence, not proof.** What makes this trustworthy is that the reasoning is simple enough to verify by inspection — one lock, taken on every path, held across the whole invariant. The test corroborates; the design convinces. That asymmetry is exactly the queue-design argument for rejecting lock-free, where the reasoning *isn't* inspectable and the testing can't close the gap.

---

**The condition variable, and the two things it does at once.** `wait(lock, predicate)` puts the thread to sleep **and atomically releases the mutex while it sleeps**, reacquiring on wake. Both halves are essential: without the release, a writer that slept holding the lock would block every producer, so nothing could ever make the queue non-empty and nothing could wake it. That deadlock is why a condvar can't be built from a mutex and a flag.

- **Predicate form, never bare `wait()`** — threads can wake with **no notification at all** (spurious wakeups), so a bare wait means popping an empty queue. The predicate form re-checks on every wake and goes back to sleep if false.
- **Predicate is `count > 0 || stopping`** — two reasons to stop waiting. Without the `stopping` half, setting the shutdown flag would never wake a sleeping writer and shutdown would hang.
- **`push` must NOT wait — a real bug, caught and fixed.** An early draft put the same `wait` in `push`, which deadlocks on the very first call: the first producer to arrive at an empty queue evaluates `count > 0 || stopping`, finds both false, and **sleeps waiting for an item only it could have supplied**. Nothing else pushes, so nothing notifies.
- **The distinction that resolves it — two kinds of waiting.** Producers *do* take the mutex: brief, bounded, nanoseconds, and guaranteed to be released because the holder can't do anything long. What "producers never block" meant in the queue design is **overflow waiting** — sleeping until a slot frees, which is unbounded. A full queue returns `false` immediately. So the mutex is shared; only the writer ever sleeps on the condvar.

---

**Shutdown — two-phase drain.** `shutdown()` takes the lock, sets `stopping`, and notifies. The writer then keeps processing until the queue is empty and only then exits. `push` refuses once `stopping` is set, so the backlog is finite and the drain terminates.

- **Drain rather than discard, and the argument is the overflow policy's.** Reject-on-full exists so that **acceptance means something** — a producer holding `true` knows its request is in the system. If shutdown discarded accepted requests, `true` would silently stop meaning that, which is precisely the "drop" behaviour rejected as indefensible in the queue design. Draining preserves a clean, statable guarantee: **accepted implies executed.**
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
- **The single definition of arrival order is preserved not because the writer stamps a number, but because **only one thread ever calls `rest`**, so `nextSeq++` — a plain non-atomic increment — is only ever executed by the writer. "How is `seq` assignment thread-safe?" answers: it doesn't need to be.

---

**A layering mistake worth recording.** `Request`, `RingBuffer` and `writerLoop` were all initially written **inside the `OrderBook` class** (hence `OrderBook::Request` in the tests, and a compile error when `std::thread` was handed what turned out to be a non-static member function). Moved to file scope after the class.

The argument is not stylistic: **the book must not know that queues or threads exist.** It is a passive data structure the writer drives from outside, and that separation is what makes "matching logic untouched" true *structurally* rather than by accident. A queue nested inside the book means the book's own definition includes threading machinery. `writerLoop` in particular takes both a `RingBuffer&` and an `OrderBook&` — a member wouldn't need to be handed its own object, which was the giveaway.

---

**Integration tests, and a false pass that only a debug print revealed.** Stage 1 pushed three submits and asserted one level — and passed while proving almost nothing: **all three requests reused `id1`**, because `nextId()` had been called once. Requests two and three were rejected by `validate` as duplicate ids, and the assert on level 100 passed purely because the *first* order rested. Only the printed ids (all identical) exposed it. Same shape as the two false-clean fuzz runs earlier: **a passing test is not evidence unless you have confirmed what was actually exercised.**

Fixed by distinct ids per request and asserting *every* level — which is also the argument for the harness, since a helper checking all listed levels by default would have caught it immediately rather than leaving it to a print.

- **`RingBufferIntegration(name, requests, expectedLevels)`** added to the `Test` class: constructs a fresh book and queue, starts the writer, pushes each request, `shutdown()`, `join()`, then compares. **The thread lifecycle is written once**, so the order-dependent part (join producers *before* shutdown; join the writer *before* touching the book) can't be got wrong in a later test.
- **Id assignment was initially inside the harness and had to move out.** Stamping a fresh id per request works for submits and silently breaks cancel and modify, which need the id of an *existing* order — the harness cannot know which order a cancel targets. The caller owns the ids; the harness pushes what it is given.
- **`checkStates` rewritten** to build actual `ExpectedLevel`s from the book and compare structurally rather than comparing bare ints.
- **Stage 2 sequence** exercises every tag and real matching in one run: three resting buys (100/99/98), a **cancel** of the 99 order, a **modify** of the 98 order to quantity 10 (a reduce — keeps position), then a **sell** crossing the 100 level. Expected: 100→40, 99→0, 98→10. Green.
- **Stated limitation:** `quantityAt` returns 0 for an absent price, so 99→0 proves the quantity went to zero, not that the level was *erased* — a ghost level would pass. Acceptable because cancel's erasure is already proven by the four cancel tests with a live-probe order; this test is checking that the *request reached cancel through the queue*, a different layer. Airtight would need `contains(id)`, which would mean exposing the book from the harness.
- **Fills are not observable through this path at all** — `writerLoop` discards them and there is no response path, so state assertions are the only instrument. A real and expected consequence of the response-path scope decision.

---

**Also this session:** capacity constrained to a power of two and `% capacity` replaced with `& (capacity - 1)` — identical results for powers of two (a power of two minus one is a mask of all-ones in exactly the low bits), avoiding integer division on the hot path. Done *after* the tests were green, so a failure would be attributable to the bit trick rather than to the wrapping logic.

---

### Queue design + depth dial

Last design session before code. No code written; matching logic untouched. Four decisions: the request type, the overflow policy, the response path, and the depth dial.

---

**Decision 1 — request type: `Request` and `LoggedOp` stay separate, with a one-way `Request → LoggedOp` conversion.**

The producer can't hand over an `Order` — that covers submit only; cancel needs an `Id`, modify needs an `Id` plus two optionals. Three shapes, one queue. **This problem is already solved in this codebase:** `LoggedOp` is exactly that tagged struct, and `invReplay` is already a working consumer loop over a sequence of them.

- **Reuse rejected:** `LoggedOp` is a *test artifact* recording what happened; `Request` is a *production message* describing what a producer wants. They look alike today and are different concepts. Fusing them means every future change to either drags the other along — adding a timestamp for latency measurement would grow the test replay type a field it has no use for.
- **Fully-separate-with-no-conversion rejected:** the determinism check would then need its own replay implementation, duplicating logic already built and proven. Two implementations that can silently drift apart is worse than a coupling.
- **Conversion chosen** because it buys the existing replay machinery for free: capture `vector<Request>`, convert, hand to `invReplay` — no second replay engine, and **the shrinker comes along too**, so a failing concurrent run can be minimised by the tool already proven against an injected bug. Cost is one function plus the rule that a field added to either type means revisiting it — a *visible* maintenance point (one function that compiles or doesn't) rather than an invisible coupling spread across the codebase. Same principle as the queue argument: don't eliminate the coupling, **concentrate it into the smallest, most inspectable surface**.
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

**Decision 3 — response path: designed, scoped out of v1.**

Rejection returns **synchronously** (`push -> bool`), so the overflow policy is complete on its own — no queue, no asynchrony, no scope risk. That part ships with the queue regardless.

**What a full response path carries:** rejections (one per request, immediate, terminal), acknowledgements (accepted and resting), and **fills** — zero to many, arriving arbitrarily later, since a single resting order can fill repeatedly over its life. That last is the design pressure: a rejection is a direct reply, a fill is an asynchronous event long after the request completed.

**Direction and structure:** only the writer knows outcomes, so this path is **one producer (the writer), many consumers (the producers)** — the mirror of the request queue, and a genuinely different problem from MPSC.

**Design on record — one SPSC queue per producer.** Routing is free: the **bit-packed ids are self-describing**, so `id >> 56` recovers the destination producer with no lookup table (a property designed for debuggability that turns out to solve routing). Each per-producer queue is then single-producer/single-consumer — **the simplest concurrent structure that exists** — so no SPMC machinery is needed anywhere. Costs N queues of memory.

- **Rejected — one shared SPMC queue:** every producer wakes for every response and filters out what isn't theirs; contention from all readers on one structure. Same fault-isolation argument as the overflow decision.
- **Rejected — callbacks:** they execute **on the writer thread**, so a slow producer callback directly stalls matching. Categorically unacceptable in a single-writer design.

**Why scoped out:** it is the same concurrency lesson at *lower* difficulty than the MPSC request queue — an SPSC queue plus a shift-and-index — so it costs real time and demonstrates nothing the request path doesn't already demonstrate. Same test applied to FIX dual ids in the model derivation and to self-cross during edge-case work: scope must be justified by a requirement, not by completeness.

---

**Decision 4 — depth dial: MODEST. Mutex + condition variable MPSC ring buffer.**

Producers never block (rejection on full), so the condition variable is **writer-side only**: the writer sleeps when the queue is empty and wakes on notify. New concept to build carefully — **spurious wakeups**: a waiting thread can wake with no notification, so the wait must always be on a predicate (`cv.wait(lock, pred)`), never bare.

**Lock-free ring buffer rejected — and the decisive argument is verifiability, not difficulty.** A too-weak memory ordering is a **silent** failure: no crash, no assertion, correct on the overwhelming majority of runs, failing non-deterministically and differently across architectures (x86's strong model hides orderings that ARM exposes) and optimisation levels. This project's entire testing apparatus — 23 tests, a 100k-operation fuzzer, four invariants, a proven shrinker — is built on invariant checks over an executed operation stream, and a memory-ordering bug can corrupt the queue in ways that yield a plausible-but-wrong stream. **It is precisely the one bug class this infrastructure is structurally blind to.** On ARM (this project's only development machine) such bugs are *exposed non-deterministically*, which is not the same as *detected* — there is no verification path available here at all.

So the reasoning is coherence, not effort: **adding unverifiable work to a project whose entire credibility rests on demonstrated correctness is negative value.** An unverifiable claim is worse than an absent one.

**Understood at concept level and deliberately not shipped** — CAS, ABA, acquire/release pairing, memory reclamation (all derived in the model derivation). The rejection is strong *because* the thing rejected can be described in detail; that is what separates judgement from avoidance. Upgrade path stays open if Phase 3 finishes with slip.

**Consequence:** the `LockFreeStack`/CAS drill does **not** fire — it was conditional on taking the Deep dial.

---

**The concurrency design phase closed here.** No engine code was touched across any of it — primitives, model derivation, the thread drills and the queue design all landed before a line of the layer was written.

---

### Thread reps + deadlock drill

First concurrency session that compiles code. All of it throwaway, in a scratch directory outside the repo — nothing here touches the engine. Purpose: *produce* the failures rather than reason about them, so the primitives conclusions rest on something observed.

**Rep 1 — thread mechanics and the scheduler.** Two threads printing in a loop. Four consecutive runs of the same binary produced clean, ordered output; the fifth produced `Thread Thread 1 is running.2` — output interleaved **mid-line**, one thread preempted partway through its `<<` chain. `std::cout` guarantees its own internal state won't corrupt, but makes no promise that a chain of `<<` calls stays contiguous; it is several operations, not one. **The unsafety was present in all six runs — only the observation changed.** That is the defining property of concurrency bugs: a sequential bug is a function of input, a concurrency bug is a function of input *and* a scheduling decision that is neither visible nor controllable.

**`join` and why the destructor terminates.** Commenting out both `join()` calls gave `libc++abi: terminating` and an abort — sometimes before any thread output, sometimes after one line, sometimes after two. A `std::thread` object is a *handle*; destroying it does not stop the OS thread. If the handle is destroyed while still **joinable**, the standard calls `std::terminate()`. The reasoning is that neither silent alternative is safe: implicitly detaching would leave a thread running against locals in the scope currently being destroyed (a silent use-after-free in code that looks correct), and implicitly joining would mean a destructor that blocks indefinitely and invisibly, possibly during exception unwinding. **So rather than guess intent, the language forces it to be stated** — `join()` or `detach()`, and saying nothing kills the program. Worth contrasting with use-after-free, where C++ happily hands back garbage: the difference is diagnosability, and a dangling thread reference is essentially undebuggable. `std::jthread` (C++20) joins in its destructor; `std::thread` keeps the old behaviour for compatibility.

**Rep 2 — the race, reproduced.** One shared `int`, two threads, 100,000 `++counter` each. Expected 200,000; observed ~100,000–150,000, **different every run, and the two threads printing different values from each other**. Mechanism: CPUs compute on registers, so `++counter` is *load → add → store*, with two gaps in which another core can act. Both threads load 500, both compute 501, both store 501 — two increments executed, counter advanced by one. The **lost update**.

The important detail is that the numbers clustered plausibly rather than looking like garbage: most iterations don't collide, and each collision costs exactly one increment, so the result is a large but partial loss. **If the expected answer weren't known, nothing about 132,847 announces itself as wrong.** Concurrency corrupts quietly and partially. And the standard's position is stronger than "you might lose increments" — a **data race** (two threads, same location, at least one writing, no synchronisation) is **undefined behaviour**, so the compiler may hoist the counter into a register for the whole loop or reorder freely. "It usually works" is not a description of that program's behaviour, because it has none.

**API frictions worth keeping (both are design choices, not quirks):** `std::thread` **copies its arguments by default**, even into reference parameters — because a thread may outlive the scope that created the argument, so copying is the conservative default and `std::ref` is the caller asserting the lifetime is fine. Passing a bare `1` to an `int&` parameter produced a wall of template noise whose real content was one line naming this file. **`std::mutex` is non-copyable** — deliberately, and conceptually rather than as a technicality: a mutex's whole function is to be a *single shared coordination point*, so per-thread copies would each lock privately, every acquisition would succeed instantly, no thread would exclude any other, and the code would **look** synchronised while providing zero protection. The standard deletes the copy constructor so that mistake is a compile error rather than a silent one — same philosophy as the join-terminate.

**Fix A — mutex.** One `std::mutex` shared by reference, `std::lock_guard` scoped tightly around the increment with explicit inner braces. Exactly 200,000, every run, deterministic. `lock_guard` is RAII: constructor locks, destructor unlocks, so the lock cannot leak through an early return, a `break`, or an exception — **the scope *is* the critical section**, which is why the braces matter (without them the guard would live for the whole loop body and hold the lock across everything else in it). Note the guarantee's real shape: a mutex protects nothing against code that doesn't take it — **the mutex doesn't guard the data, the discipline of always taking it does.**

**Fix B — atomic.** `std::atomic<int>`, loop body left as a bare `++counter`. The *type* changed, not the code: the compiler emits a single hardware read-modify-write with no gap to interleave into. Also 200,000, every run.

**Timing attempt, and the more useful lesson.** `time ./threads` gave mutex 0.014 / 0.013 s and atomic 0.047 / 0.013 s — the atomic version's two runs differing from *each other* by 3.6×, a spread larger than any effect being measured. Correctly read as **the measurement being useless, not the two approaches being equal**: 200,000 increments is microseconds, so this mostly timed process startup and dynamic linking; no warm-up (the 0.047 is almost certainly a cold run); two samples; and `time`'s resolution sits in the same range as the workload. **A single number with no distribution is not a result.** This is benchmarking methodology arriving unplanned, and it is the same critique due to be run against this project's own `benchmark.cpp` at benchmarking·1. From the mechanics rather than these numbers: for one integer increment atomic normally wins substantially, but the gap only surfaces under real contention with enough iterations to swamp fixed costs.

**Atomic vs mutex — the actual distinction, and it isn't speed.** An atomic makes **one variable's operation** indivisible; a mutex makes **an arbitrary region of code** exclusive. Make both account balances atomic and each line is individually safe, but between them an observer sees money in neither account: **the invariant spans two locations, so no per-variable primitive can protect it.** Atomics for a single variable's operation; mutexes when several things must change together.

**The deadlock drill.** `struct account { int balance; std::mutex m; }`, and a `transfer` holding **both** locks simultaneously — releasing the first before taking the second would open a window where the money exists in neither account and the conserved-total invariant is briefly false. That requirement is precisely what creates the hazard. Two threads looping 100,000 transfers in opposite directions (`transfer(a,b)` and `transfer(b,a)`) **hung on the first run**.

The interleaving: T1 acquires `a.m` → T2 acquires `b.m` → T1 requests `b.m` and blocks → T2 requests `a.m` and blocks. Each holds what the other needs, and a `lock_guard` releases only at scope exit, which neither thread can reach. **It hangs rather than crashing** — no exception, no stack trace, zero CPU, both threads parked. In production that is a silently hung service, often harder to diagnose than a crash.

**The four Coffman conditions** (all four must hold simultaneously, so breaking any one prevents deadlock): mutual exclusion · hold-and-wait · no preemption · circular wait. Mutexes supply 1 and 3 by definition, so practical fixes attack 2 or 4.

- **Fix A — lock ordering, breaks *circular wait*.** Compare the two accounts' **addresses**, swap the pointers so the lower-addressed mutex is always taken first, then lock in that order. Concretely: with `a` at 0x1000 and `b` at 0x2000, `transfer(a,b)` doesn't swap and `transfer(b,a)` does — so **both** threads lock a then b despite running opposite transfers. The swap **decouples lock-acquisition order from transfer direction**; the business logic is untouched, since the mutexes are gates and don't care which account is debited. The specific order is arbitrary and addresses carry no meaning — any total order every thread computes identically would do. **Consistency is the entire requirement**, because a cycle requires some thread to acquire "backwards," and if nobody ever does, no cycle can form.
- **Fix B — `std::scoped_lock`, breaks *hold-and-wait*.** One line, both mutexes. Internally `std::lock`: attempt to acquire all, and on any failure **release everything already held** and retry — so it is never holding one lock while blocking on another. Different condition broken, same guarantee.

Both fixes ran clean, with `a.balance + b.balance == 2000` asserted afterwards — the cross-object invariant that motivated two locks in the first place.

**When to use which — structural, not preferential.** `scoped_lock` needs the **complete set** of mutexes at one point, because its back-off algorithm must be able to release everything it has taken. If lock A is acquired in one function and lock B requested three levels down, the inner code *cannot* release A — it has no handle to that guard. Ordering discipline is the fallback precisely because it requires nobody to release anything: each site independently obeys the same acquisition sequence. This is also why large systems use documented **lock hierarchies** rather than `scoped_lock` everywhere — where locks cross module boundaries, no single point ever knows the full set.

**Why the session points back at the engine — the ladder.** Races because memory was shared and mutable → mutexes because of the race → deadlock because of the mutexes → ordering discipline because of the deadlock. **Each fix creates the conditions for the next problem.** That is the argument for the primitives conclusion being *eliminate the sharing* rather than *use locks carefully*: single-writer steps off the ladder at the first rung, and no sharing means no race, no mutex on the book, no deadlock, no ordering discipline — structurally absent, not defended against.

It also directly validates the **lock-per-level** reject, which is this drill at scale: two aggressors sweeping in opposite directions (buy ascending, sell descending) acquire price levels in opposite orders — `transfer(a,b)`/`transfer(b,a)` with *market data* choosing the acquisition order instead of the programmer. And the fix that worked here fights the algorithm there: imposing "always ascending" forces a sell aggressor to acquire locks **before knowing it needs them**, since its natural sweep is descending.

**Both halves are worth stating together:** deadlock-free multi-lock transfer implemented two ways, with the Coffman condition each one breaks identified — and an engine that needs neither, because single-writer means there is no second lock to order against.

---

### Deriving the concurrency model

Design session, no code. Output: the model, the three rejects with their reasoning, and the id-assignment decision. Nothing in this session touches matching logic.

---

**The model: single-writer-with-queue.** One thread owns the `OrderBook` outright — not "has priority on it," owns it. No other thread holds a pointer to it or has any path to its memory. Producer threads never touch the book; they push *requests* (submit this order / cancel this id / modify this id) onto an MPSC queue. The writer thread loops: pop, dispatch on type, call the existing book method, handle the result.

Checked against every failure mode identified in the primitives session: two threads racing on `quantity` — impossible, one writer. Dangling `Order*` from `best()` — impossible, the only thread that can obtain it is the only thread that can invalidate it. Fairness scrambled by scheduling — impossible, the writer processes in pop order. Deadlock — impossible, no locks on the book at all. The character of those four matters: **structurally absent, not defended against.** No lock to forget, no invariant to maintain, no ordering discipline to get right. The bugs can't be written because the code that would contain them doesn't exist.

**`seq` assignment.** The writer assigns it at pop time. This is what makes price-time priority well-defined again — the primitives session established that under parallel matching "earlier" has no defensible meaning. Precise framing, worth keeping honest: this does **not** guarantee the order that reached the machine first gets the lower `seq` (network jitter and producer scheduling still affect queue-arrival order). What it guarantees is that **the system has exactly one definition of arrival order, and every subsequent decision is consistent with it.** That is what fairness actually requires. Real exchanges have the same property — the sequencer defines truth.

**The seam — why the engine is untouched.** The public API already returns by value (`submit → optional<vector<Fill>>`, `cancel`/`modify → bool`); no caller holds a reference into the book. So the writer calls these **unchanged** and the concurrency layer sits entirely outside them. Not a happy accident — it's why the Phase 3 gate ("matching logic untouched, diff-provable") is realistic. Every test, the 100k fuzz, and the shrinker stay valid because the thing they validated didn't change.

**What remains, and why it's progress.** The queue is shared mutable state under concurrent access — the problem is moved, not eliminated. Three-part argument that this is genuine progress: (1) the **contention surface collapsed** from unbounded (every level, order, map node, index entry) to two indices, head and tail, touched by different sides; (2) the **operations became trivial and bounded** — push-one/pop-one has essentially no intermediate state, vs `submit` sweeping an unknown number of levels with many mid-flight states; (3) **the queue has no ordering semantics to violate** — matching's correctness *depends on* sequence, whereas a queue's only job is to *establish* one, and any consistent order will do. So a concurrent queue is a known, isolated, exhaustively testable problem with a literature behind it. Concurrent matching isn't a hard problem; it's the wrong problem.

**The honest cost.** The writer is a **throughput ceiling** — one thread's worth of matching, forever, regardless of hardware. The escape hatch is horizontal: **shard by instrument.** One book per symbol, one writer per book, genuinely parallel, because *different books share nothing*. Arrival order matters within a book and is meaningless across books. That is the parallelism actually available, and it's available precisely because it doesn't cross the sequential constraint.

**Generalised technique (the reusable form).** 1 — **Eliminate sharing** where possible by giving data a single owner. 2 — **Concentrate** what remains into the smallest, dumbest surface possible (small = few variables; dumb = no domain semantics). 3 — **Solve that one thing properly**, in isolation, with real tests. 4 — **Scale by partitioning** over data that doesn't overlap, never by parallelising over data that does.

---

**The three rejects.** They fail for three *different* reasons, which is what makes this an analysis rather than three ways of saying "I picked the easy one."

**1 — Global lock. Fails on performance; correct but pointless.** One mutex round the whole book, `lock_guard` at the top of each public method. It is genuinely **correct** — every failure mode is prevented, a real total order exists, and `seq` is assigned consistently inside the critical section. It fails because it **buys nothing**: four threads, 400 ns critical section, one at a time → 2.5 M orders/sec, which is exactly what the current single-threaded engine does. Identical throughput, plus costs the single-threaded version doesn't pay — lock acquire/release every operation; **contention** (a blocked thread is descheduled and later woken; a context switch is *microseconds* against a 400 ns critical section, so coordination overhead can be thousands of times the cost of the work it protects); **convoying**; and destroyed **p99 tail latency**, which is the number that matters in a latency-sensitive system. Root cause is the core insight: a lock doesn't create parallelism, it re-serialises threads that could never run in parallel anyway — paying coordination costs for parallelism the domain doesn't permit. **When it would be right:** when the critical section is a small fraction of thread runtime (10 µs of independent parsing/risk work, 100 ns of shared access → ~1% contention, fast path dominates). The test is *what fraction of a thread's runtime is inside the lock* — low → global lock is the right, simple tool; approaching 100% → no locking scheme helps. Matching **is** the workload, so it's the second case.

**2 — Lock per price level. Fails on correctness, even if implemented perfectly.** Finer locks so threads at 100 and 105 proceed concurrently. Breaks down immediately on mechanics: an aggressor sweeping levels can't release earlier locks as it moves (a concurrent rest at an already-swept level would be missed), so it **holds locks cumulatively in an order determined by market data** — and two aggressors sweeping in opposite directions (buy ascending, sell descending) acquire in opposite orders → textbook **deadlock**. The standard fix (global lock-ordering discipline, always ascending) fights the algorithm, since a sell naturally sweeps descending and would have to acquire out of execution order. Worse, most operations aren't level-local anyway: erasing an emptied level or inserting a new one **mutates the map**; `cancelIndex` is **global**; and `best()` — the hottest operation in the engine — is inherently global. So you need tiered locks, which is where the nasty deadlocks live. **But the killer isn't mechanical:** even with perfect lock ordering and zero deadlocks, **fairness is still broken** — two orders at the same price on two cores, and whichever thread the OS schedules first takes the fill regardless of arrival, with `seq` assigned by whoever gets there. Price-time priority violated with *zero memory bugs*. No amount of locking skill fixes it, because the problem isn't in the locking; the domain forbids what's being attempted. **When it would be right:** when elements are genuinely independent and there's no cross-element ordering rule — a sharded hash map with per-bucket locks is exactly this and works beautifully, because nobody cares which of two concurrent inserts "happened first."

**3 — Lock-free book. Fails on feasibility and risk (and fairness anyway).** Lock-free structures exist for queues, stacks, and simple maps — structures where a mutation is one pointer swap, so a single CAS can publish it. Matching is not that: one incoming order reads best price, mutates a resting order's quantity, appends to fills, may erase a list node, may erase a map entry, updates the cancel index, may insert a remainder. That's a **coordinated multi-structure transaction** with no single atomic operation that publishes it — you'd need multi-word CAS or STM, which is research machinery, not a design choice. Specific hazards: **memory ordering** (every atomic needs an explicit ordering; too weak is a *silent* race — no crash, no assertion, correct on 99.9% of runs; x86's strong model hides many incorrect orderings that ARM will expose, so code can pass exhaustively on one machine and fail on another, or pass at `-O0` and fail at `-O3`); **ABA** (CAS checks *"is the value unchanged?"* as a proxy for *"has nothing happened?"* — A→B→A defeats it; needs tagged pointers, hazard pointers, or epoch-based reclamation); and **memory reclamation**, the genuinely hard part — when is it safe to free a node another thread may still be reading. And even fully built, **fairness still breaks**, same as reject 2. The decisive argument for *this* project: it introduces a bug class the existing testing infrastructure (100k fuzz, four invariants, proven shrinker) **structurally cannot detect**, which is a bad trade for a project whose credibility rests on demonstrated correctness. That is a much stronger rejection than "it's hard." **When it would be right:** narrow structures with no domain semantics — which is exactly why lock-free remains a live option for the **queue** in the queue design, and not for the book.

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

**Why this creates no new correctness problem:** a producer pushing *submit* then *cancel* for the same id puts both in one FIFO queue, so the writer pops submit first, always — a cancel **cannot overtake its own submit**. If the submit was rejected, the cancel arrives for an id not in the book, which `cancel` already handles as a clean no-op (tested since cancel was built). The latency win falls out of the queue's FIFO property plus behaviour already built.

**Rejected:**
- **Engine-assigned ids (A)** — uniqueness by construction at the engine and `validate`'s check becomes dead code, but the producer can't know the id until the writer responds, so **the order is uncancellable until a round trip completes**. In a fast market that window matters. Decisive against, for a latency-focused project.
- **Client + engine dual ids, FIX-style `ClOrdID` + `OrderID` (C)** — correct at venue scale, and it solves two genuinely different problems: the *client's* (reference my order immediately, with no round trip and no coordination with other clients — which is B's insight generalised, uniqueness scoped to the issuer) and the *venue's* (one identifier unique across every client, session, and day, for audit, reconciliation, and regulatory reporting). Entails a second `Order` field, a `clientId → engineId` resolution index on top of the existing one, a rule for which id appears in which message class, and a scoping decision (unique per session / per day / forever). **Understood and deferred:** producers here are trusted in-process network threads, not untrusted external clients, so B's partition contract is enforceable rather than hopeful. Building C would be scope not justified by a requirement.
- **Status quo (D)** — declare uniqueness a caller contract with validation as a net. Zero implementation and defensible *if stated deliberately*, but weaker than an actual decision.

---

### Concurrency primitives

No code. Concepts built from the ground up, anchored to this engine's own hot path rather than textbook examples.

**The two failure modes, both derived from the real code.** `best()` returns a non-const `Order*` — a handle to the actual resting order — and the match loop mutates it **in place**. That single design fact (correct and deliberate single-threaded) is what makes the book unsafe the instant a second thread exists:

- **The order dies underneath a reader.** Thread A consumes the best resting order and erases it; thread B is still holding the same pointer and dereferences it. Use-after-free — the exact bug class already hit three times single-threaded (cancel, modify, submit's cleanup), except now the destructive call isn't a line visible above the read. It happens *between* instructions, from another thread. **Capture-before-erase cannot help, because there is no "before" under your control.**
- **Nothing dies, and it's still wrong.** One resting sell of 100; two incoming buys of 100 on two threads. `quantity -= tradeQty` is not one operation — it is load, subtract, store. Both threads load 100 before either stores, both compute a trade of 100, and one store overwrites the other. **200 units trade against a resting order that only had 100.** No crash, no dangling pointer, every pointer valid throughout — and **volume conservation**, the most financially load-bearing of the four fuzzer invariants, is violated with no bug in the matching logic at all.

**Precise definitions, kept distinct.** A **data race** is the memory-level fault: two threads accessing the same location, at least one writing, without synchronisation — and in C++ terms this is **undefined behaviour**, not "you might read a stale value." The compiler is entitled to assume no races exist and optimise accordingly. A **race condition** is broader: the *outcome* depends on timing. The distinction matters because fixing every data race does **not** fix the fairness problem — make `seq` assignment atomic and lock every level correctly, and two same-priced orders on two cores still fill in whichever order the scheduler happened to pick.

**Shared mutable state as the enemy — and precisely why.** It is the **conjunction** that is fatal: shared + mutable + concurrent access. Shared-but-immutable is completely safe (any number of threads may read const data with no guard rails). Mutable-but-unshared is completely safe (thread-local data has nobody to race with). Only the overlap produces UB — which is useful, because it says exactly which of the three to attack. Immutability is unavailable (a book that cannot change is not a book), so the attack must be on sharing or on concurrent access.

**The terminal insight (the session's actual product).** Explored the "restrict *when* threads touch it" branch first — a lock — and found it hollow: with one thread inside `submit` at a time, four threads at 400 ns each give exactly the throughput of the existing single-threaded engine, minus the cost of coordination. **A lock does not create parallelism; it destroys it**, forcing concurrent threads back into single file. Parallelism only ever pays when threads do work that *doesn't* overlap, and here all the work is the same book. Then the per-level refinement, which fails for a deeper reason: `seq` is what encodes arrival, the match loop consumes each level front-to-back in `seq` order, and the entire fairness rule is therefore a statement about **sequence** — whereas parallelism is precisely the licence to not care which of two things went first. Hence:

> **Arrival order is the semantics.** The sequence does not decorate the result, it *determines* it — hand the same set of orders to the engine in a different arrival order and genuinely different people get filled at genuinely different prices. Matching is therefore inherently sequential: parallelise it and you either serialise through locks, gaining nothing, or you don't, and the results change. **There is no third option.**

Note the shape of that conclusion: parallel matching is not rejected because concurrency is difficult, but because it is **structurally impossible to gain from** — a result derivable from the fairness rule alone.

**The other branch — destroy the sharing.** Exactly one thread ever touches the book. Not one-at-a-time; one, period, with sole ownership and no path from any other thread. Every failure mode above becomes **structurally absent** rather than guarded against, and with no locks there is no contention and no deadlock. This forces the remaining question — how does work reach a structure nothing else can touch? — and the answer is a hand-off point both sides can reach: a queue. Many producers push, one consumer pops. **MPSC** (multiple producer, single consumer). Whatever order requests land in the queue *is* the arrival order, the writer processes them in exactly that order, and `seq` is assigned by the single thread doing the popping — so price-time priority is preserved by construction.

**Objection raised and answered, since the queue is itself shared mutable state under concurrent access.** Three reasons this is progress rather than relocation: the **contention surface** collapses from unbounded (every level, every order, every map node, every index entry) to essentially two indices, head and tail, touched by opposite sides; the **operations become trivial and bounded** (push-one, pop-one — almost no intermediate state, versus `submit` sweeping an unknown number of levels with many mid-flight states another thread could catch); and decisively, **a queue has no ordering semantics to violate** — matching's correctness *depends on* sequence, while a queue's only job is to *establish* one, and any consistent order will do. A concurrent queue is a known, isolated, exhaustively testable problem with real literature behind it. Concurrent matching is not a hard problem; it is the wrong problem.

**Bounded, not unbounded — a correctness argument, not a comfort one.** If producers outrun the consumer on an unbounded queue, memory grows without limit (and growth means **allocating on the hot path**, the exact unpredictability benchmarking exists to attack), and latency grows without limit — an order sitting in the queue while the book moves is *technically* correct and *commercially* a wrong outcome. In a trading system, unbounded latency **is** a failure. So the queue must be bounded, which forces a stated overflow policy rather than undefined behaviour. **Resolved in the queue design: reject on full.**

**False sharing (the hardware rung).** In a bounded ring buffer the consumer advances `head` and producers advance `tail`. Logically independent — no race, no lock needed. But the CPU's unit of coherence is the **64-byte cache line**, not the variable, and a core may only *write* a line it holds **exclusively**. Adjacent `head` and `tail` share a line, so each write **invalidates** the other core's copy of the whole line and forces a re-fetch: the line **ping-pongs**, and every write becomes a cross-core coherence transaction instead of a local cache hit. The sharing is **false** — it exists in the hardware and not in the program; two genuinely independent actors in the code sit on one line underneath. **Fix: change the layout, not the writers** — `alignas(64)` per field or explicit padding, costing a few dozen bytes. (A single-writer "fix" is not available here: head and tail *must* be written by different threads; that is the design.) **The tell: adding threads makes it slower.** Confirm with `perf c2c` on Linux, Instruments on Mac.

**Memory ordering — awareness only, deliberately.** Compilers and CPUs reorder memory operations, preserving only *single-threaded* observable behaviour, so another thread can observe the reordered sequence. Once head and tail become atomics, each operation needs an explicit ordering, and too weak a choice is a **silent** race. Depth deferred to the queue design, where the mutex+condvar option was taken, so it stays at awareness level.

**Also surfaced (parked):** per-level locking would require holding **multiple locks at once in an order determined by market data** — two aggressors sweeping in opposite directions acquire in opposite orders, which is the textbook deadlock setup. This pre-loaded the account-transfer drill and became the mechanical half of the lock-per-level reject.

---

### Design review and README pass

Ran the full ten-decision defence cold, no notes: Order type, book structure, price level, cancel index, match loop, modify, validation, fuzzer/invariants, map unification, and the two real bugs found — each required stating what/why/rejected/how-you'd-know-if-wrong/what-you'd-do-differently from memory, with challenge on anything under-specified or imprecisely framed. Corrections made, worth keeping precise going forward:

- **Order type:** integer ticks are about *correctness* (exact comparison), not speed — modern FP arithmetic isn't meaningfully slower than integer; leading with "expensive" invites an easy counter. The counter-for-seq argument leads with "unique and ordered by construction," with "clocks are unreliable" as the supporting rejection, not the other way round.
- **Book structure:** the real justification is mechanical (composite keys, loss of each side's baked-in natural ordering, degraded access on the hottest operation in the engine — `best()`) — "clarity" is a consequence of that, not the driver, and leading with it sounds soft under a technical follow-up.
- **Price level:** the precise `std::list` guarantee is the single most load-bearing fact in the whole design and is worth being word-perfect on — erasing/inserting anywhere invalidates *only* the iterator to the erased element, nothing else, ever. Briefly stated backwards mid-defence; corrected and drilled.
- **Cancel index:** "redundant" sharpened to the concrete failure mode — caching price/side separately from the order risks a stale-cache bug the instant the order is modified and the cache isn't updated in lockstep; storing only the iterator makes that structurally impossible rather than just unnecessary.
- **Match loop:** crossing conditions must be stated by the **incoming** order's side, not the resting order's side (the resting side is always the opposite, and anchoring the explanation to it is what caused a live in-session inversion of the buy/sell crossing logic — the exact shape of the original sell-crossing bug, said backwards by habit rather than reintroduced in code).
- **Validation:** the market-order price exemption is because price never participates in a market order's crossing decision at all (type alone decides), not because price `0` carries special meaning — that framing would contradict the "no magic values" position already taken for modify.
- **Fuzzer/invariants:** all four invariants must be named on demand (crossed-book, volume conservation, FIFO, orphaned-index) — an initial answer surfaced only two and omitted volume conservation specifically, the most financially load-bearing of the four. "No crossed book" was also initially overstated as proof that "every trade that should have executed, did" — corrected to its actual, narrower scope: a resting-state structural check that can *result from* a matching failure, not a certificate that matching was exhaustively correct. No unearned scale claims ("millions of operations per second") attached to this project's own numbers.
- **Map unification:** reframed from "I was careful, so nothing went wrong" to the more accurate and more interesting claim — the refactor was safe to *attempt* specifically because a regression suite existed to catch mistakes, and it demonstrably did: two real bugs were introduced and caught within the same session. The bugs are the receipts, not a blemish to smooth over.
- **The two bug stories:** confirmed as genuinely known and understood throughout the pass (correct comparison direction, correct reasoning about equal-price masking, correct fix) — but a *live cold retelling*, prompted under a specific narrative framing, briefly conflated the sell-crossing bug with an unrelated later bug (the fuzzer generator's inverted type-weighting). Concluded that knowing the facts and cleanly narrating them on demand are different skills, and that repeatedly performing the retelling back doesn't add signal once the facts are confirmed solid — the fix was writing each story once, precisely, into the README, so the retelling exists as a stable artifact rather than something reconstructed from scratch under pressure each time.

**README fully rewritten** to reflect all of the above: every design-decision section restated with the corrected framing, two new sections added (Match Loop, Modify, Validation, Map Unification, Property-Based Testing, and a dedicated Bugs Found and Fixed section — several of these didn't exist as their own sections before), the stale "9 passing tests" / "planned" language replaced throughout, and the informal benchmark preview folded in with its caveats intact.

**Phase 2 exit gate — confirmed met in full:** correct (all four operations, edge cases, fairness rules) · tested (23-test replay harness + 100k-operation fuzzer across 4 invariants + a proven shrinker) · defensible (this pass, now written into the README) · visible (live repo, current README + DEVLOG) · scope-clean (no threads, no real benchmarking infrastructure, no persistence snuck into the core).

---

### Shrinker

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

### Property-based invariant testing + fuzzer

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

### Map unification

- **Motivation:** `bids` used `std::greater<>` and `asks` used the default comparator specifically so `begin()` always meant "best" on both sides — which is why `withOppositeSide`/`withGetSide` had to be *templates* (the two maps were different C++ types). Unified both maps to the default comparator, replacing the templates with three small named helpers: `getMap(Side) -> map&` (the map for a given side, no flipping), `opposite(Side) -> Side` (the enum flip, used explicitly at call sites — e.g. `submit` composes `getMap(opposite(incoming.side))` rather than hiding the flip inside a helper), and `best(Side)` (already existed from the skeleton, updated to `bids.rbegin()` / `asks.begin()` now that "best" isn't automatically at `begin()` for both sides).
- **Design call:** kept `rest()` as its own simple if/else rather than routing it through `getMap` — it wasn't duplicating error-prone logic, so adding indirection there would have been simplification for its own sake. Judged case-by-case rather than mechanically applying the new helpers everywhere.
- **Rejected:** reusing `validate()` for post-match cleanup in `submit`'s loop — `validate` is a gatekeeping question ("should this new order be admitted"), cleanup is a different question ("this existing order hit zero, remove it correctly"). Different category, `cancel()` is the right existing tool.
- **Rejected (explicitly, mid-session):** doing a broader cleanup/enhancement pass while at it, on the reasoning "I keep noticing more things, my code quality is improving." Recognised this as the normal experience of touching load-bearing code (re-examination surfaces things every time, doesn't mean growing debt) rather than evidence of a real backlog — and that the fuzzer is the disciplined tool for systematically finding exactly this class of thing, not more ad-hoc manual review. Kept the change bounded to the map unification only.
- **Bug 1 — found during the refactor:** `submit`'s post-fill cleanup block still did `oppositeSide.begin()->second.orders.pop_front()` unconditionally. This was fine when the comparator trick made `begin()` mean "best" on both sides — now that both maps share one comparator, `begin()` only means "best" for asks; for bids, best is `rbegin()`. The match itself (via `best()`) correctly read from the right end, but cleanup after a fill was popping/erasing from the *wrong* end whenever the aggressor was a sell (matching against bids). Manifested as failed rest-remainder/market/price-change-modify tests with corrupted state and, eventually, a crash from cumulative map corruption.
  - **Fix:** replaced the inline `begin()`-based pop/erase with a call to `cancel(restingId)` — side-agnostic, works from the order's own fields via the existing index, no begin/rbegin assumption. Capture `resting->id` into a local *before* calling `cancel` (same capture-before-cancel discipline as modify), and ensure `fills.emplace_back(...)` runs *before* the cancel call, not after — reading `resting->price`/`resting->id` after `cancel` erases the node is a use-after-free (this was the source of the garbage Fill values and the trace-trap crash seen mid-session).
- **Bug 2 — found immediately after fixing Bug 1:** `cancel`'s internal side-lookup was written as `auto map = getMap(order.side);` — missing the `&`, silently copying the entire map. `cancel` was operating on a throwaway copy, so the real `asks`/`bids` was never actually mutated — explaining why the loop's next iteration kept finding a stale/corrupted view of the book even after Bug 1's fix. Same *class* of mistake as two earlier copy-vs-reference bugs from the same refactor session (the original `getSide`/`getOpposite` drafts also returned by value before being corrected) — worth flagging as a recurring failure mode: always double-check `auto&` vs `auto` at every call site of a function that returns a reference.
- **Full 23-test suite green** after both fixes. No regressions.

### Validation

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
- Modify, edge cases, and validation all done and tested. 23 passing tests.

### Edge cases I

- **Multi-level sweep:** 3 resting levels (25/25/50), one aggressor for 100 sweeping all three. Proves the match loop generalises across repeated iterations, not just a single 2-level partial (which every prior test used).
- **Exact-match boundary:** isolated single-level test — rest 50, aggressor exactly 50. One fill, both sides to zero, level erased. A third order (a small buy resting *after* the match) acts as a live probe: if the sell level had a ghost entry instead of being genuinely erased, this order would produce an unexpected fill instead of resting cleanly. It doesn't — proving erasure, not just emptiness.
- **Self-cross:** reasoned out of scope for v1 — `Order` has no participant/account field, so "same trader on both sides" isn't representable. Documented as a possible future extension (participant model), not a bug or gap.
- **Order-id ownership:** reviewed caller-assigned vs engine-assigned ids. Staying caller-assigned for now (test legibility — hand-authored sequences need predictable ids); revisit engine-assignment when fuzzing or concurrency needs it, where uniqueness-by-construction matters more than hand-editing convenience. Duplicate-id guard implemented in validation. **[RESOLVED in the model derivation — producer-partitioned ids; see that entry.]**
- **Scope review:** most of the originally-listed edge cases (thin book, empty side, 2-level partial) turned out already covered by existing tests. Re-scoped to the two cases above, which are the ones existing tests structurally can't catch (loop-generalisation, exact-zero boundary).

### Modify

- `modify(Id, std::optional<Price>, std::optional<Quantity>)` — target-state signature; optional fields mean "leave unchanged", handles single or combined changes atomically, `nullopt` self-documents (vs a magic 0).
- **Fairness rule derived:** reduce-quantity keeps queue position (in-place edit — asking for less harms no one); increase-quantity and price-change LOSE position (the added quantity / new level arrived later, can't jump earlier orders) → implemented as cancel + resubmit with same id, fresh seq.
- id preserved across modify (stable identity); seq is fresh on resubmit (that's what puts it at the back). The id-vs-seq split paying off.
- `quantity == 0` → cancel, checked FIRST (before price/quantity routing) so "to zero" always cancels regardless of price change.
- Bugs caught: use-after-free (using the order reference after cancel destroys the node) — fixed with capture-before-cancel (side/price/quantity into value locals, then cancel, then rebuild+submit); forgetting to override the captured field with the new value in each change branch.
- Rejected: limit→market via "price 0" (magic-value overloading, mixes the rests/doesn't-rest boundary) — type-change out of scope for v1.
- All five modify cases green: reduce-keeps-position, increase-loses, price-change-loses, modify-to-zero-cancels, unknown-id-noop. Derived and verified the fairness rule **by consequence** (fill-order in the resulting Fills), not by inspecting internal queue structure.
- Caught and closed a verify-by-consequence blind spot: lose-position tests with a single small matcher can't distinguish "order moved to back" from "order dropped" — fixed by sizing the matcher to sweep through the front order into the tracked one, so both fills emit and their order proves presence + position.
- Extended `modifyTest` with `ExpectedLevel` state assertions to close the ghost-order gap (no duplicate left at the old price level on a price-change).

### Cancel

- `cancel(Id) -> bool` on OrderBook: look up id in cancel index → get list iterator → capture price as a value → erase order from its level's list (O(1)) → erase from index → remove level from map if now empty.
- **Safety call (defended under challenge):** capture price as a value before erasing (the list node dies on erase); confirmed all reads of the order reference precede the erase, so no use-after-free. The "don't copy" tenet is about whole Orders/containers, not an 8-byte price — capturing a scalar is free. Kept defensive find-guards for now; strip at benchmarking.
- **Self-describing-order insight:** the cancel index stores only the iterator. Side/price are derived from the order's own fields (the order knows where it belongs), so storing them again would be redundant, mutable-in-two-places state.
- **Harness refactored into a `Test` class**; added `CancelTest` alongside `runReplayTest`. Success signal is `cancel`'s bool return, NOT `contains` (which can't distinguish "cancelled" from "never existed").
- 4 cancel tests: single-order (level emptied, neighbour untouched), two-at-same-price (one cancelled, other + level survive), unknown-id (clean no-op), cancel-last-at-price (level removed). All pass.
- **[Later note:** cancel-on-unknown-id being a clean no-op is what makes producer-side cancellation safe under the queue — a cancel enqueued for a rejected submit arrives for an id not in the book and does nothing.**]**

### Submit + Replay Harness

- Renamed `match` → `submit` (it matches AND rests the remainder — the full entry point). `rest` stays as the internal placement helper.
- `quantityAt(Side, Price) -> int64_t` — total resting quantity at a price (0 if absent); the state-inspection primitive. Reasoned that total-per-price + `best()` covers all cases; per-order/FIFO inspection deferred until the invariants need it.
- `runReplayTest(name, sequence, expectedFills, expectedState)` — runs a scripted order sequence through `submit`, asserts actual fills == expected (count first, then element-by-element on price+quantity) and actual state == expected (via `quantityAt`). Readable failure output via `operator<<` overloads for `Fill`/`ExpectedLevel`/vectors.
- 5 replay cases: buy aggressor, sell aggressor (genuinely distinct, not a duplicate), rest-remainder, market order (remainder dropped), empty-book. All pass.
- Bugs caught by tracing: every-against-every fills comparison (kept last comparison only), self-comparison in states check (compared quantityAt to itself), price/quantity field-order swap in test data, duplicated sell test masquerading as coverage.
- **Deferred:** determinism check (run-twice-identical) → meaningful only at concurrency, noted as a conscious deferral. **[Now live — `LoggedOp`/`invReplay` plus the `Request → LoggedOp` conversion is the tool; no second replay engine needed.]** `ExpectedLevel` "checks listed levels, won't catch unlisted extras" — fine for authored sequences.

### Match loop

- `std::vector<Fill> match(Order&)` — the matching heart. While incoming has quantity and opposite side non-empty: grab best resting order (mutable, from own maps), check crosses, `tradeQty = min(both quantities)`, reduce both, emit Fill at resting price, pop filled resting orders, remove empty levels. After: rest limit remainder / drop market remainder.
- `crosses` generalised: market always; buy-limit `incoming.price >= resting.price`; sell-limit `resting.price >= incoming.price`.
- `withOppositeSide` lambda-template solves the "bids and asks are different types" problem (comparator makes them distinct types) cleanly.
- **`Fill` struct:** price (resting/execution price), quantity, aggressorId, restingId.
- Also erase from cancel index when a resting order fully fills (no orphaned index entries).
- Bugs caught by comparison-to-plan: `=` vs `==` in the zero check (silent), `min` of prices not quantities (the volume-conservation line), branch structure (loop trapped in wrong side), rest-on-full-fill, spurious "dropped" message on a fully-filled limit.
- Proven on canonical buy (2 fills: 100@102, 20@103; 30 resting) AND sell aggressor (highest-bid-first). Both directions correct.
- **No smart pointers:** the level owns the order; match only mutates + removes. A shared_ptr's atomic refcount would be pure cost on the hot path for a non-problem.
- **[Later note:** `quantity -= tradeQty` in this loop is *load, subtract, store* — three operations, not one. That is the exact site of the lost-update failure mode if a second thread ever entered the book.**]**

### Book skeleton

- Built `OrderBook`: two side-maps (`bids` with reversed comparator, `asks` default), `Level { std::list<Order> }`, `best(Side)`, `rest(Order)`.
- Cancel index = `unordered_map<Id, list::iterator>` — populated in `rest`.
- **Design call made solo:** `best()` returns `const Order*` (not `std::optional<Order>`) — a handle to the *actual* resting order so the match loop can mutate it in place; `nullptr` signals empty. A copy would have broken matching. **[This is exactly the path that makes the book thread-unsafe — the anchor for the shared-mutable-state argument.]**
- `contains(Id)` added as a minimal public observer for the private index.
- 7 tests: top-of-book (lowest ask / highest bid), side independence, empty→nullptr, FIFO-within-level (by id), index population.
- **Known deferrals (benchmarking / tidy):** `rest(Order o)` takes by value → copies; could move into the list. Remove stray `<iostream>` from the header. Raw pointer from `best()` valid only until that order is filled/cancelled — fine for internal callers.

### Order type

- `Order` struct: side, type, price (int64 ticks), quantity, id, seq. Enums `Side`, `Type` as `enum class`.
- Public struct (pure data, no invariants of its own → no encapsulation needed; invariants live in the book).
- Defended cold: integer ticks (float equality unsafe), seq-not-clock (clocks collide/run backwards), id-vs-seq (identity vs temporal ordering).
- **[Later note:** the id-vs-seq split is what makes the concurrency model work — `seq` becomes writer-assigned at pop time (the single definition of arrival order), while `id` stays producer-assigned so a producer can name its own order without a round trip.**]**

### Design on paper

- Traced the canonical matching example by hand (2 fills, 30 resting @103) — the replay-test oracle.
- Derived all four structures from the three requirements (fast best-price, FIFO, fast cancel), each with rejected alternative.
- Discovered the `std::list` stable-node property myself from the cancel requirement.
- Checkpoint: **Go** — continue in C++.

---

## Optimisation candidates (not to act on before profiling)

Ideas surfaced while thinking about FIFO inspection. Recorded so the thinking isn't lost — but these are performance-speculative and must be measured, not assumed. The list-based core is correct and tested; do not rewrite it without benchmarks justifying the change.

- **Tombstone-vector vs list levels.** Replace `std::list` levels + iterator-splice cancel with a `std::vector` + `is_cancelled` flag on Order; cancel flips the flag, matching skips tombstones. Trade: O(1) splice-cancel → cache-friendly contiguous storage, but cancelled orders accumulate (needs compaction) and every match branches past dead orders. Which wins depends entirely on workload — a a measurement, not a guess. NOTE: the list is currently load-bearing — cancel removes from the *middle* in O(1) via stored iterator; a front/back-only structure can't do that.

- **Raw-pointer cancel index.** Only viable with *stable* storage. Stable with `std::list` (nodes don't move); DANGLES with a vector (reallocation on growth). So this conflicts with the tombstone-vector idea — can't have both. If levels stay list-based, a raw pointer/iterator is already what's used.

- ~~**Map unification.**~~ **DONE** — see "Map unification" above.

- ~~**Engine-assigned order ids.**~~ **RESOLVED in the model derivation** — producer-partitioned ids (producer in the high bits, thread-local counter in the low bits). Uniqueness by construction with no engine round trip; `validate`'s duplicate check retained as defence in depth.

- **`rest(Order o)` by-value copy** → move into the list. Logged when the skeleton was built, still awaiting a profile rather than an instinct.

- **Queue sizing / `Request` footprint.** `sizeof(Request)` is set by its largest variant (a full `Order` plus the tag, ~40–48 bytes), so a bounded ring buffer costs `capacity × sizeof(Request)` up front. A variant-based design could shrink it — measure before deciding it matters.

---

## Concurrency decisions

| Question | Resolution |
|---|---|
| Concurrency model | Single-writer-with-queue (MPSC) |
| Rejected alternatives | Global lock (pointless) · lock-per-level (breaks fairness) · lock-free book (unverifiable) |
| Id assignment | Producer-partitioned, 8/56 bit split, thread-local counters |
| Queue element type | `Request`, separate from `LoggedOp`, one-way conversion |
| Bounded-queue overflow | Reject on full; `push -> bool`, never blocks |
| Response path | Designed (per-producer SPSC, routed by id high bits), scoped out of v1 |
| Queue implementation | Mutex + condvar; lock-free rejected on verifiability |

---