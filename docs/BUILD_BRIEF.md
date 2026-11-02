# Claude Code Handoff — Trading Stack Build (current: W11 Simulated Exchange)

Brief for Claude Code build sessions. Planning happens in the planning chat (the "Goal Profile & Portfolio Plan" tab of the Master Plan doc is the authority on what each session works on); build sessions happen here. Drop this in the repo as `docs/BUILD_BRIEF.md` and read it at the start of every session.

---

## 0. The big picture

**Goal:** an end-to-end trading system built from scratch in C++23, covering every major layer of a trading firm — venue (simulated exchange), market data (feed handler), execution (firm gateway and pre-trade risk), strategy (market maker), low latency (tick-to-trade) and research (backtesting platform). Every component is measured as it lands: **one-line answer, headline number, one finding.**

**Why:** chosen to teach the most about the job and the parts of the industry around it, one team's work per component. Fixed constraint from IMC's feedback (Sept 2026): every part must have a clear answer and a real number. Target: QD and trading-systems / low-latency SWE roles; all four CV entries finished and measured by **1 August 2027**; build frozen from September 2027.

### Four CV entries (each its own repo: README, tests, headline number, one finding)

| CV entry | Components | What it proves | Headline number to earn |
|---|---|---|---|
| **Simulated Exchange** | Matching engine (done), sequenced L3 feed, retransmission, snapshots, order entry, exchange gateway & risk checks | Exchange-side design, binary protocols, correctness under load | Cost of publishing to the matching path; order-entry round-trip latency |
| **Market Data Feed Handler** | Decoder, local book rebuild, gap detection, snapshot recovery; own feed + live Binance stream | Networking, parsing, recovery, measurement | Decode-to-book p50/p99; recovery time after a gap |
| **Market-Making Strategy & Tick-to-Trade** | Firm-side gateway, order state machine, pre-trade risk, inventory-skewed MM, wired end to end | The full firm-side path, measured | Tick-to-trade p50/p99 by stage; one measured optimisation |
| **Backtesting Platform** | Recorder (normalised capture to disk), columnar storage, deterministic replay through the live strategy code, queue-position-aware fill simulator, results report | Research-platform engineering; avoiding look-ahead bias | Replay speed (events/sec); backtest vs live fill-rate gap |

### Roadmap (later Ws carry exit gates only — broken into tasks when they become current)

| Phase | W | Build | Exit gate |
|---|---|---|---|
| Now → Christmas | **W11** | Simulated Exchange (**current**, §3) | §3 exit gate |
| January | — | Paused (exam mode) | — |
| Feb → exams | W12 | Exchange Gateway & Risk Checks | Every order passes auth + pre-trade checks (position limits, price bands, rate limits, kill switch) over **real TCP sockets**; rejects carry reasons; matching cost unchanged. Closes Simulated Exchange |
| | W13 | Market Data Feed Handler | Decodes own feed + live Binance stream; rebuilds book; detects gaps, recovers from snapshot; decode-to-book p50/p99 and recovery time measured |
| | W14 | Data Capture & Storage | Recorder writes the feed handler's normalised events (own exchange, Binance, Nasdaq ITCH samples) to columnar files continuously, gap-free across restarts; any day queryable by instrument and time. **Left running from here on** |
| May–June | — | Paused (exam mode) | — |
| Summer | W15 | Firm Gateway & Pre-Trade Risk | Order state machine reconciled against the exchange's acks/fills/rejects; position and size limits; kill switch |
| | W16 | Market-Making Strategy | Quotes around a fair price, skews by inventory; profit, fill rate, adverse selection measured against the exchange |
| | W17 | Replay & Fill Simulator | Market maker runs **unchanged** against recorded history through the **same Env interface it uses live**; queue-position-aware fills, latency and fees modelled; deterministic; backtest vs live fill rates compared. Closes Backtesting Platform |
| | W18 | Tick-to-Trade Pipeline | Feed handler → strategy → gateway → exchange end to end; p50/p99 by stage; one perf-found optimisation measured |
| August | — | Buffer: finish and measure anything open; CV rebuilt from finished work | — |

Menu items not on the roadmap (options extension, real-data replay, agent-based simulation, multi-player arena, counterfactual replay, P&L attribution, scenario library) are stretch, picked by interest only after the core W is closed.

### Why W11 decisions matter downstream (keep these consumers in mind)

- **The feed format (W11·2) is consumed by** the feed handler (W13), the recorder (W14) and the replay engine (W17). Design it once, well; a change later ripples through three components.
- **Order entry (W11·8–11) is the port that** the exchange gateway (W12) wraps with risk checks and the firm gateway (W15) talks to. Its reply set (ack / reject with reason / fill) is what W15's order state machine reconciles against.
- **Snapshots and retransmission (W11·7a–d) are what** W13's recovery uses.
- **Transport (W11·9):** W12 requires real TCP sockets, so an in-process choice now means a planned migration in W12.
- **Determinism:** the engine's deterministic replay is the foundation W17 builds on — don't break it.
- Claude Code must **not** design or build ahead into later Ws; note downstream implications in the session report instead.

### Learning tracks that run through the build

- **Networking (main gap):** TCP vs UDP, multicast, sockets, non-blocking I/O, epoll, Nagle/TCP_NODELAY, kernel bypass at concept level. *Done when the exchange's order entry and feed run over real sockets he wrote.* Sources: Beej's Guide; Kerrisk (TLPI) sockets/epoll chapters.
- **Linux and perf tooling:** perf stat/record, flame graphs, cache/branch counters, pinning, honest tail measurement. *Done when one optimisation is found with perf and measured before/after.*
- **Concurrency and the memory model (gaps only):** happens-before, release sequences, fences, hazard pointers. *Done when the MPSC queue's orderings are defended line by line.*
- **C++ depth:** value categories, move semantics, RVO, perfect forwarding, templates/concepts/constexpr/CRTP, STL internals, UB, object lifetime, **strict aliasing** (directly relevant to W11·2 byte handling).
- **Low-latency design:** references — Carl Cook "When a Microsecond Is an Eternity"; David Gross "Trading at light speed"; **Nasdaq TotalView-ITCH 5.0 and OUCH specs** (real-world reference points for the feed and order entry).
- **Research data and backtesting (for W14/W17):** Arrow/Parquet, Kleppmann storage chapters, kdb+/q, look-ahead bias.

Teach these at the point the build needs them, not ahead.

---

## 1. How these sessions run

**Roles**
- **Ayo writes the code.** Claude Code audits, explains and teaches. Do not write production code for him unless he explicitly asks for a specific piece.
- **Debugging stays his.** When something breaks, ask *where* it diverged from the intended behaviour; don't name the bug.

**Teaching contract**
- **Explanation-first** for any new concept, at the point it's needed: trace the full mechanism — what happens, why, and what follows — with concrete numbers. No skipped steps, no assumed background, no vague paraphrase of things he already knows (say "data race", not "threads clashing").
- Bottom-up and sequential: build from foundations, connect each idea to what he already knows (OS, concurrency, lock-free, memory ordering, cache behaviour).
- **Design decisions: present options and trade-offs only — no recommendation, no "lean".** A stated preference biases the choice.
- **Corrections on correctness only.** Name what's wrong and why, directly. No softening, no lengthy caveats.
- Say immediately if something fundamental is wrong.
- Flag where knowledge ends and speculation begins.

**Discipline**
- **Scope:** finish and measure each task before the next. Scope must be justified by a requirement, not completeness.
- **Measurement:** write predictions down *before* running; compare back to back in one session; report percentiles of batch means, stated as such; check build flags (`-O3 -flto`) whenever a result looks wrong.
- **Devlog:** engineering record only — no coaching language.
- **CV claims:** only completed, measured, defensible work. README and CV never contradict; latency figures carry their method ("percentiles of batch means" where that applies).
- **If friends join:** each person owns whole components, agreed up front; CV claims cover only what Ayo built.
- **Priority if something gives:** cut project scope first (fewer components), never the measurement at the end of each.

**End of every session:** produce a session report (template in §8) for the planning chat.

---

## 2. Repo

- `github.com/ayomideakeni/quant-matching-engine` (also synced as `Serengetiix2/quant-matching-engine`), code under `MatchingEngine/`
- `orderClass.hpp` — `Side`, `Type` enums; `Order { side, type, int64 price, quantity, id, seq, Order* next, prev }` (56 bytes)
- `orderBook.hpp` — `memoryPool`, intrusive `level`, `OrderBook` (`absl::btree_map` per side, `boost::unordered_flat_map<Id, Order*>` cancel index), `Request`, `RingBuffer` (mutex) and `LockFreeQueue` (MPSC, `USE_LOCKFREE`), `writerLoop`
- `tests.cpp` (23 replay tests, 100k-op property fuzzer with 4 invariants, shrinker, 1M-op determinism replay), `benchmark.cpp`, `throughput.cpp`
- Build: `build.sh`, `build_asan.sh`, `build_tsan.sh`, `run.sh`, `run_bench.sh`
- Types: `Price`, `Quantity`, `Id` are all `int64_t`
- Ids: plain counters in most tests; `producer::nextId()` = `(producerId << 56) | counter` (7 producer bits, 56 counter bits, top bit 0)
- `validate`: rejects live duplicate id, `quantity <= 0`, limit `price <= 0`. **No upper bounds yet.**
- Platform: Apple ARM64 (little-endian). x86 also little-endian.

### Every place the book changes (capture points for W11·3)

| Where | What |
|---|---|
| `rest()` | order added to a level |
| `submit()` loop | `resting->quantity -= tradeQty` + `Fill{price, quantity, aggressorId, restingId}` |
| `submit()` full fill | calls `cancel(restingId)` internally |
| `submit()` limit remainder | calls `rest(incoming)` |
| `cancel()` | order removed |
| `modify()` qty down | `order.quantity = newQuantity` in place (keeps priority) |
| `modify()` price change / qty up | `cancel()` then `submit()` of a replacement, same id (can cross and trade) |
| `modify()` qty 0 | `cancel()` |

All on the single writer thread.

### Audit findings (must be handled in W11)

1. **Full fill goes through `cancel()`.** With implied removal (Execute to zero = gone), capturing Delete inside `cancel()` would wrongly emit a Delete after a full fill. Capture must distinguish the two.
2. **`submit()` ignores `rest()`'s return.** If the pool is full the post-trade remainder silently vanishes. Only publish Add when `rest()` succeeded.
3. **Modify to an invalid price loses the order** (cancel succeeds, replacement fails `validate`, `modify` still returns true). Feed would correctly show Delete with no Add; the caller is misinformed.
4. **`validate` needs the wire caps** (see §4): price ≤ 2,147,483,647, quantity ≤ 1,073,741,823 — otherwise the engine accepts orders the feed can't represent.

---

## 3. W11 task list

| Task | Build | Gate | Status |
|---|---|---|---|
| W11·1 | Market data concepts | Explain cold: rebuild-from-feed, one missing message, why fixed-layout binary | **Done (14 Oct)** |
| W11·2 | Message set and byte layout | Spec in `docs/`: every message, field offsets, padding, byte order decided | **In progress** |
| W11·3 | Capture points in the engine | Every book change maps to exactly one message; matching logic untouched | |
| W11·4 | Handoff off the writer thread | Option chosen, rejects defended, cost prediction written down | |
| W11·5 | Implement emission and sequencing *(discovery-gated)* | Correct messages, gap-free sequence numbers, existing tests unchanged and green | |
| W11·6 | Verify with a reference decoder | Book rebuilt from the feed matches the engine across the 1M-op replay | |
| W11·7 | Measure the cost to matching | Back to back, with vs without the publisher, stated as a number | |
| W11·7a | Retransmission design | Cache size, request/reply messages, serving thread decided; served off the writer thread | |
| W11·7b | Implement retransmission *(discovery-gated)* | Requested range returned exactly; range older than cache refused cleanly; matching cost unchanged | |
| W11·7c | Snapshot design | Format, channel, and how it's taken without stalling matching, decided | |
| W11·7d | Implement snapshots *(discovery-gated)* | Snapshot at seq N + live feed from N+1 rebuilds the engine's book exactly | |
| W11·8 | Order entry protocol | Every request (new, cancel, modify) has defined replies (ack, reject with reason, fill) | |
| W11·9 | Transport decision | In-process queues vs sockets, decided and defended | |
| W11·10 | Response path design | How each reply reaches the right client, decided | |
| W11·11 | Implement order entry *(discovery-gated)* | External client sends orders and gets correct replies | |
| W11·12 | Adversarial tests | Malformed requests, cancel of unknown id, both sides of a fill reported, TSan/ASan clean | |
| W11·13 | Defence and close | One-line answer, headline number, one finding, README section, LinkedIn post | |

**Exit gate:** the exchange publishes a sequenced binary feed that rebuilds its book exactly, serves retransmissions and snapshots, accepts external orders with correct replies, and the cost of publishing to the matching path is measured.

Tasks are sized for ~1–2 sessions as a guide. Discovery-gated tasks can't be sized until earlier ones reveal them.

---

## 4. W11·2 — decisions locked so far

**Feed model**
- **L3 / order-by-order.** Every message names an order id.
- **Book events, not requests:** Add, Execute, Delete, Reduce. Modify is expressed through these (Delete + Add for price change / qty up; Reduce for qty down; Delete for qty 0).
- **Feed states facts** — the receiver never re-runs matching.
- **Execute:** carries resting order id, quantity filled, price. **Removal at zero is implied** (no separate Delete).
- **Reduce:** carries "reduce by X" (not "new quantity"). Execute and Reduce are the same operation for the receiver: subtract, remove at zero.
- **No aggressor id** anywhere (anonymity; not needed for the book).
- **Feed sequence number is the publisher's own counter**, one per message emitted — **not** `Order::seq` (that's the queue-priority counter).

**Message layout: fixed 16 bytes for every message, two 64-bit fields**

| Field | Bits | Contents |
|---|---|---|
| Field A | 64 | order id — full `int64_t` as-is |
| Field B | 64 | message type (2) + side (1) + price (31) + quantity (30) |

- Price cap: 2³¹ − 1 = 2,147,483,647 ticks. Quantity cap: 2³⁰ − 1 = 1,073,741,823. Enforced in `validate`.
- Order type (Market/Limit) is never sent: market orders never rest.
- Unused bits per message type: Execute leaves side unused; Reduce leaves price unused; Delete uses only id + type.

**Packet header: one 64-bit word**

| Bits | Field |
|---|---|
| 56 | sequence number of the first message in the packet |
| 7 | message count (≤ 127 per packet) |
| 1 | reserved, always 0 |

- Message *n* in a packet has sequence number `first + n`.
- Gap detection is per packet (UDP drops whole packets).
- Typical payload: ~1,472 bytes per UDP datagram − 8-byte header = 1,464 → 91 messages of 16 bytes.

**Rules**
- Pack/unpack with **`uint64_t`** shifts and masks — right-shifting a negative `int64_t` is arithmetic and sign-extends, corrupting extracted fields.
- The wire format matches field *types*, not `Order`'s memory layout.

## 5. W11·2 — still open (next session starts here)

Present each as options + trade-offs, no lean:

1. **Field order:** which 64-bit field occupies bytes 0–7 — the id field or the type/side/price/quantity field? (The decoder needs type first to know how to read the rest.)
2. **Bit positions inside Field B:** where type sits (top bits vs bottom bits) and the order of side/price/quantity — fixed for every message type.
3. **Byte order on the wire:** little-endian (native on ARM64/x86) vs big-endian (network byte order) — and what each costs the encoder/decoder.
4. **How bytes get written/read:** e.g. building a `uint64_t` and `memcpy` into the buffer vs other approaches; why not `reinterpret_cast` on a packed struct (alignment, strict aliasing, compiler padding).
5. **Write the spec** in `docs/feed_spec.md` — every message, every bit position, header, byte order, caps. That closes W11·2's gate.

---

## 6. Later in W11 — context to carry

- **Retransmission:** client requests a sequence range; served from a ring buffer of recent packets on a separate request channel and thread — never the writer thread. Finite cache → old ranges refused.
- **Snapshots:** "book as of seq N"; client applies snapshot, discards buffered messages ≤ N, applies the rest. Needed for late joiners and gaps older than the cache.
- **Client recovery logic** (for the reference decoder in W11·6): on a gap, stop trusting the book, buffer incoming messages, recover (retransmit or snapshot), then resume.
- **Order entry** replies are private (per client), separate from the public feed. A client learns of its own fills from order entry, not the feed.

## 7. Parked candidate (not part of W11)

- **Shrink `Order` (56 bytes → aim 32):** narrower enums, smaller quantity, reordering, maybe pool indices instead of pointers. Pick up after W11·7 when the benchmark is set up; measure deep-level sweeps and cancels first; change only if they show it.

---

## 8. End-of-session report template (paste back to the planning chat)

```
## Session report — [date]
Task(s): W11·_
Gate status: met / not met — [why]

### Done
- ...

### Decisions made (with the rejected options and why)
- ...

### Concepts covered
- ...

### Measurements (prediction written first → result; percentiles of batch means; build flags)
- ...

### Bugs found / where they diverged
- ...

### Downstream implications (for W12–W18, noted not built)
- ...

### Open questions / blockers
- ...

### Next session starts with
- ...
```
