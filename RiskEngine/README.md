# Pre-Trade Risk Engine (Challenge B)

A single-account pre-trade risk gate in C++17. It reads JSON Lines on stdin and writes one JSON response per request. It survives SIGKILL at any point, including during a write. There are no third-party dependencies: the JSON parser, checksum and hash table are all in [`src/risk_engine.cpp`](src/risk_engine.cpp).

## Build and run

```bash
./build.sh                                   # g++ (or $CXX) -std=c++17 -O2 → build/risk_engine
./run.sh --state-dir /absolute/path/to/state # builds first if the binary is missing or stale
```

The repository root has `build.sh` and `run.sh` wrappers that call these, so `./run.sh --state-dir …` works from the root too. Compiler output always goes to stderr, never to stdout. The build is safe to run concurrently because each build links to a private temp file and renames it into place. The only file written in the state directory is `journal.bin`.

Exit codes: `0` at EOF, `2` on a protocol error, `3` on an I/O or persistence failure (or an inconsistent journal), `64` on bad command-line arguments.

## Tests

```bash
python3 tests/run_tests.py          # full suite (~10 min)
python3 tests/run_tests.py --quick  # ~1 min
```

| Suite | What it checks |
| --- | --- |
| samples | `samples/*.input.jsonl` against `samples/*.expected.jsonl`, both in one run and split into two process lifetimes at every possible line |
| protocol errors | 46 bad lines (blank, malformed, duplicate keys, null, floats, `1e2`, `01`, `-1`, out-of-range values, wrong or extra keys, bad patterns, non-ASCII, 33 symbols, duplicate symbols, line over 64 KiB, …). Each must exit 2, flush earlier responses, print nothing for the bad line, and leave state unchanged after a restart. Also checks unusual but valid formatting: `\u` escapes, `\r`, `-0`, 10^18 |
| streaming | each response arrives while stdin is still open, and a protocol error exits without waiting for EOF |
| fuzz | 150 random histories with retries, conflicts, shuffled keys and whitespace, `\u` escapes, reordered `init.symbols`, early `not_initialised` requests, repeated inits and limit changes. Each runs across 4 process lifetimes and is compared byte for byte with [`tests/model.py`](tests/model.py), a separate naive model that recomputes exposure from scratch on every request |
| torn tail | truncates the journal at random byte offsets, which is what an interrupted append leaves, then re-sends the whole history. Output must equal the model's and the rebuilt journal must be byte-identical |
| crash | 20k–40k request streams, with SIGKILL at random 0–40 ms intervals, up to ~60 kills per history. Every complete response line received must match the model. Sending resumes from the first unacknowledged request, so committed-but-unacknowledged requests are retried. At the end the whole history is retried in random order and every original response must come back |

**About the samples:** this repository doesn't include the challenge's `samples/` files, `SCORING.md` or `DATA.md`. The three fixtures here were rebuilt from the descriptions in the challenge text. Their expected outputs come from the reference model and were checked by hand against every number the text quotes:

- basic: 600 → 870 → 470 → 100
- exposure: 40 → 220 → 200
- a sell that would reduce the position still fails the gross test
- an order exactly at the limit passes

To reproduce the manual recovery example for idempotency, run lines 1–7 (through `f1`), kill the process, then run the rest. The test suite does this for every split point.

## Design

### Request pipeline

For each input line:

1. **Strict parse.** The parser follows RFC 8259 and rejects anything this protocol can never accept: duplicate keys, `true`/`false`/`null`, fractions or exponents, non-zero negative numbers, integers that overflow int64, and non-ASCII characters (no valid identifier, symbol or key can contain them). Nesting is limited to depth 16. A line longer than 64 KiB is rejected before it is parsed.
2. **Schema check.** Each type must have exactly its listed keys, with correct types, identifier and symbol patterns, and value ranges.
3. **Canonical encoding.** The request minus `event_id` is turned into a compact binary form. String escapes are decoded and `init.symbols` is sorted. Two requests are semantically identical exactly when these bytes are equal, so key order, whitespace, escapes and symbol order don't matter.
4. **Event-ID lookup**, before any business rule. A known ID with the same bytes gets its original response. A known ID with different bytes gets `event_id_conflict`, which is not recorded.
5. **Business decision** against the current state (`Engine::apply`). This only changes state when the request is accepted.
6. **Journal append**, then the response is buffered.

### Business state

Each symbol has its position `P`, open buy quantity `B` and open sell quantity `S`. The engine also keeps a running total of filled exposure, `Σ|P|·ref`, and of reserved exposure, `Σ remaining·price`. So every check is O(1), apart from one hash lookup for the order ID and an O(log 32) symbol lookup. Every accepted order stays in an `unordered_map` forever, which keeps closed order IDs occupied. Open orders are also kept in a `std::map`, so a query lists them in ASCII order without sorting. All arithmetic uses `int64_t`, and the spec's bounds guarantee nothing overflows.

### Idempotency records

Every committed request (accepted or rejected, from any lifetime) is stored as one packed record: `id | canonical bytes | result code | (offset, length)`. Records live in 1 MiB arena blocks, so there is no per-record allocation and no copying when the arena grows. An open-addressing hash table indexes them by `event_id`. A non-query response is rebuilt from the result code and is byte-identical to the original. A query's historical response is not kept in memory; it is read back from the journal with `pread` at the recorded offset.

### Persistence and the durability argument

`journal.bin` is an append-only sequence of records. Each record is `u32 len | u32 crc32c | payload`, where the payload is `id | canonical request | result code | query response bytes (accepted queries only)`.

- **Commit point.** A request is committed once `write(2)` has returned for all of its record's bytes. The data is then in the kernel page cache, which outlives the process. The challenge only requires surviving a process crash, so the engine doesn't call `fsync`.
- **Order of writes.** Responses are written to stdout only after their journal bytes have been written. Requests are processed in batches: all complete lines already in the input buffer are handled, then the journal batch is written with one `write`, then the response batch with another. The engine always flushes before it might block on `read`, so no response waits for more input. A fatal protocol error flushes in the same order (journal, then stdout) and then exits 2.
- **SIGKILL during a write.** When a regular-file `write` is interrupted, the bytes that reach the file are a prefix of what was written. So the only possible damage is an incomplete last record. Recovery reads records in order and stops at the first one that is incomplete. It cuts the file back to the last complete record with `ftruncate`; those cut bytes were never acknowledged. The truncation is atomic and idempotent, so being killed again during recovery is safe. A *complete* record with a bad CRC, an undecodable request, a duplicate event ID, or a replayed decision that differs from the recorded code is never the result of a crash. In those cases the engine exits 3 rather than silently dropping acknowledged history.
- **Recovery.** Recovery replays each record's canonical request through the same `Engine::apply` used for live requests. It checks that each decision matches the recorded code and rebuilds the idempotency index. Query responses are not regenerated; only their offsets are recorded. The rebuilt state is exactly the result of processing the committed prefix once. A request that committed but was never acknowledged returns its committed response when retried. A request that never committed runs fresh from the previous state.
- **I/O errors.** A failed or short journal write, a failed `pread` of a historical response, or a failed stdout write prints to stderr and exits 3. Nothing from the failed batch is acknowledged, and every earlier record is intact. SIGPIPE is ignored so that a closed stdout is reported as an error instead of killing the process silently.
- **Storage.** No snapshots, compaction or temporary files, so peak disk usage is the journal itself. Its size is about the size of the canonical input (smaller than the JSON) plus the bytes of non-retried query responses. Within the stated bounds (256 MiB of input, at most 1M query entries), that is well under 2 GiB. Retries and conflicts add nothing.

### Measured costs

Measured on a 4-vCPU Linux container with `tests/gen_bench.py`. The benchmark history has 1,000,000 lines: 32 symbols, 200,000 distinct orders, up to ~10k open orders at once, about 48% rejections, 3% retries, limit changes, and 93 large queries (≤ 1M entries in total).

| Measurement | Result |
| --- | --- |
| Full 1M-line history, piped in one go | 2.2 s wall (1.2 s user + 0.2 s sys), about 1.2 µs CPU per request |
| Restart (replays 76 MB journal) + one 10k-order query | 0.47 s wall |
| Retrying all 1M lines after a restart | 1.3 s, byte-identical output, journal unchanged |
| Peak RSS | 97 MiB |
| Journal size | 76 MB for a 75.5 MB input |
| Lockstep round trip through a pipe (one line in, wait for its response) | p50 26 µs, p99 74 µs. Each response is its own journal `write` plus stdout `write`, and most of the time is pipe and scheduler latency |

Memory scales roughly as `events × (id + canonical bytes + 13)` for the idempotency records, plus about 100 bytes per accepted order. At the stated bounds the worst case is a few hundred MiB, within the 1 GiB limit. Recovery time is linear in journal size, at roughly 150 MB/s including CRC checks and replay.

## Interpretation choices

These are places where the challenge text allows more than one reading:

- **Duplicate symbols in `init`** are treated as a schema error, so they are fatal. The text lists uniqueness with the schema, not with the business rejection reasons.
- **`-0`** is accepted as the integer 0: it is a valid JSON integer token. A non-zero negative is out of range, so it is fatal.
- **`\uXXXX` escapes** that decode to ASCII are accepted, and they compare equal to the unescaped form. Anything non-ASCII is fatal, because no field allows it.
- **Line length:** a line's content (excluding `\n`) can be at most 65,535 bytes, and the final line without a newline follows the same rule. A longer line exits 2 as soon as it is detected, without reading further input.
- **Trailing `\r`** is accepted as JSON whitespace.
- An empty input on an empty directory creates an empty `journal.bin` and leaves the account uninitialised.

## Limitations

- **No `fsync`.** The challenge requires surviving process death only, not power loss or a host crash, so the page cache is the durability boundary. Adding `fdatasync` after each batch would cover power loss as well, at the cost of a disk flush per batch.
- **The journal is never compacted.** It stays within the 2 GiB limit under the stated workload bounds, but a history far beyond those bounds would keep growing.
- **The journal format is host-endian** (little-endian is enforced at compile time) and is only meant to be read by the same build.
