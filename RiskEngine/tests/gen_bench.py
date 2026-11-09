#!/usr/bin/env python3
"""Generates a bound-sized history for benchmarking.

  python3 tests/gen_bench.py OUT.jsonl [lines=1000000]

~200,000 distinct accepted orders, up to ~10,000 open at a time, fills,
cancels, rejections, limit changes, retries (incl. of old rejections) and a
sprinkling of queries over 32 symbols.
"""
import json
import random
import sys

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 1000000
rng = random.Random(7)
syms = ["SYM%02d" % i for i in range(32)]
lim = {"max_order_qty": 1000000, "max_order_notional": 10**12, "max_abs_position": 10**12, "max_gross_exposure": 10**18}
lines = [json.dumps({"type": "init", "event_id": "init", "symbols": [{"symbol": s, "reference_price": 100 + i} for i, s in enumerate(syms)], "limits": lim}, separators=(",", ":"))]
open_ids = []
next_order = 0
eid = 0
history = []
with open(out, "w") as f:
    f.write(lines[0] + "\n")
    for i in range(n - 1):
        eid += 1
        e = "e%d" % eid
        x = rng.random()
        if x < 0.00009:  # ~90 queries x <=10k open orders stays within the 1M-entry bound
            ev = {"type": "query", "event_id": e}
        elif x < 0.03 and history:
            f.write(rng.choice(history) + "\n")  # retry
            continue
        elif x < 0.035:
            ev = {"type": "limits", "event_id": e, "limits": dict(lim, max_order_qty=rng.choice([10, 1000000]))}
        elif (x < 0.25 and next_order < 200000 and len(open_ids) < 9990) or not open_ids:
            ev = {"type": "new", "event_id": e, "order_id": "ord-%06d" % next_order, "symbol": rng.choice(syms),
                  "side": rng.choice(["buy", "sell"]), "qty": rng.randint(1, 100), "price": rng.randint(1, 1000000)}
            open_ids.append(next_order)
            next_order += 1
        elif x < 0.32:
            k = rng.randrange(len(open_ids))
            ev = {"type": "cancel", "event_id": e, "order_id": "ord-%06d" % open_ids[k]}
            open_ids[k] = open_ids[-1]
            open_ids.pop()
        else:
            k = rng.randrange(len(open_ids))
            ev = {"type": "fill", "event_id": e, "order_id": "ord-%06d" % open_ids[k], "qty": 1}
        line = json.dumps(ev, separators=(",", ":"))
        if len(history) < 100000:
            history.append(line)
        f.write(line + "\n")
