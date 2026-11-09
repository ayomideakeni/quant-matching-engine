#!/usr/bin/env python3
"""Test harness for the risk engine.

  python3 tests/run_tests.py [--quick]

Covers: sample fixtures (whole and split by kill/restart at every line),
protocol errors, differential fuzzing against tests/model.py (including
retries, conflicts and reformatted JSON), SIGKILL crash recovery at random
points, and torn journal tails.
"""

import json
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RUN = os.path.join(ROOT, "run.sh")
sys.path.insert(0, HERE)
from model import Model, run_model  # noqa: E402

QUICK = "--quick" in sys.argv
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", msg)
    return cond


def run(state, data, timeout=120):
    p = subprocess.run([RUN, "--state-dir", state], input=data.encode() if isinstance(data, str) else data,
                       capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode(), p.stderr.decode()


def fresh_dir():
    return tempfile.mkdtemp(prefix="risk state ")  # path with a space on purpose


def lines_of(path):
    with open(path) as f:
        return f.read().splitlines()


# ----------------------------------------------------------------- samples

def test_samples():
    for name in ("basic", "idempotency", "exposure"):
        inp = lines_of(os.path.join(ROOT, "samples", name + ".input.jsonl"))
        exp = lines_of(os.path.join(ROOT, "samples", name + ".expected.jsonl"))
        d = fresh_dir()
        rc, out, err = run(d, "\n".join(inp) + "\n")
        check(rc == 0 and out.splitlines() == exp, f"sample {name} (rc={rc} err={err})")
        shutil.rmtree(d)
        # Restart (process exit, no graceful anything) between every pair of lines.
        for cut in range(1, len(inp)):
            d = fresh_dir()
            rc1, o1, _ = run(d, "\n".join(inp[:cut]) + "\n")
            rc2, o2, _ = run(d, "\n".join(inp[cut:]))  # final line without newline
            check(rc1 == 0 and rc2 == 0 and (o1 + o2).splitlines() == exp, f"sample {name} split at {cut}")
            shutil.rmtree(d)
    print("samples ok")


# ----------------------------------------------------------------- protocol errors

INIT = '{"type":"init","event_id":"i1","symbols":[{"symbol":"A","reference_price":10}],' \
       '"limits":{"max_order_qty":100,"max_order_notional":100000,"max_abs_position":100,"max_gross_exposure":1000000}}'
GOOD = '{"type":"new","event_id":"g1","order_id":"o1","symbol":"A","side":"buy","qty":1,"price":10}'
LIM = '"limits":{"max_order_qty":1,"max_order_notional":1,"max_abs_position":1,"max_gross_exposure":1}'

BAD_LINES = [
    "",
    "   ",
    "not json",
    "[]",
    '"str"',
    "{}",
    '{"type":"query"}',
    '{"type":"query","event_id":"q1","extra":1}',
    '{"type":"query","event_id":"q1","event_id":"q2"}',
    '{"type":"query","event_id":null}',
    '{"type":"query","event_id":"bad id"}',
    '{"type":"query","event_id":"' + "x" * 65 + '"}',
    '{"type":"query","event_id":""}',
    '{"type":"query","event_id":"q\\u00e9"}',
    '{"type":"query","event_id":"q1"} x',
    '{"type":"query","event_id":"q1"',
    '{"type":"bogus","event_id":"q1"}',
    '{"type":"QUERY","event_id":"q1"}',
    '{"type":1,"event_id":"q1"}',
    '{"type":"cancel","event_id":"c1","order_id":5}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":0}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":1000001}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":1.0}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":1e2}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":"1"}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":true}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":-1}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":01}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":NaN}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":Infinity}',
    '{"type":"fill","event_id":"f1","order_id":"o1","qty":99999999999999999999999}',
    '{"type":"new","event_id":"n1","order_id":"o2","symbol":"a","side":"buy","qty":1,"price":1}',
    '{"type":"new","event_id":"n1","order_id":"o2","symbol":"A","side":"BUY","qty":1,"price":1}',
    '{"type":"new","event_id":"n1","order_id":"o2","symbol":"A","side":"buy","qty":1,"price":0}',
    '{"type":"new","event_id":"n1","order_id":"o2","symbol":"A","side":"buy","qty":1}',
    '{"type":"new","event_id":"n1","order_id":"o2","symbol":"ABCDEFGHIJKLMNOPQ","side":"buy","qty":1,"price":1}',
    '{"type":"limits","event_id":"l1","limits":{"max_order_qty":1,"max_order_notional":1,"max_abs_position":1}}',
    '{"type":"limits","event_id":"l1","limits":{"max_order_qty":1,"max_order_notional":1,"max_abs_position":1,"max_gross_exposure":1000000000000000001}}',
    '{"type":"limits","event_id":"l1","limits":{"max_order_qty":-1,"max_order_notional":1,"max_abs_position":1,"max_gross_exposure":1}}',
    '{"type":"limits","event_id":"l1","limits":null}',
    '{"type":"init","event_id":"i9","symbols":[],' + LIM + '}',
    '{"type":"init","event_id":"i9","symbols":[{"symbol":"A","reference_price":1},{"symbol":"A","reference_price":2}],' + LIM + '}',
    '{"type":"init","event_id":"i9","symbols":[' + ",".join('{"symbol":"S%d","reference_price":1}' % i for i in range(33)) + '],' + LIM + '}',
    '{"type":"init","event_id":"i9","symbols":[{"symbol":"A","reference_price":1000001}],' + LIM + '}',
    '{"type":"init","event_id":"i9","symbols":{"symbol":"A","reference_price":1},' + LIM + '}',
    '{"type":"query","event_id":"q1"}' + " " * 65536,
]


def test_protocol_errors():
    for bad in BAD_LINES:
        d = fresh_dir()
        rc, out, err = run(d, INIT + "\n" + GOOD + "\n" + bad + "\n" + '{"type":"query","event_id":"after"}\n')
        ok = check(rc == 2, f"bad line exit code {rc}: {bad[:80]!r}")
        ok &= check(out.splitlines() == ['{"event_id":"i1","status":"accepted"}', '{"event_id":"g1","status":"accepted"}'],
                    f"bad line output {out!r}: {bad[:80]!r}")
        ok &= check(err.strip() != "", f"bad line stderr empty: {bad[:80]!r}")
        # The offending line must not have been applied or cached.
        rc, out, _ = run(d, '{"type":"query","event_id":"q1"}\n{"type":"fill","event_id":"f1","order_id":"o1","qty":1}\n')
        exp = run_model([INIT, GOOD, '{"type":"query","event_id":"q1"}',
                         '{"type":"fill","event_id":"f1","order_id":"o1","qty":1}'])[2:]
        check(rc == 0 and out.splitlines() == exp, f"state after bad line {bad[:80]!r}: {out!r}")
        shutil.rmtree(d)

    # Valid-but-unusual formatting that must be accepted.
    d = fresh_dir()
    odd = [INIT,
           ' { "event_id" : "g\\u0031" , "type":"new","order_id":"o1","symbol":"A","side":"buy","qty":1,"price":10 }\r',
           '{"type":"limits","event_id":"l0","limits":{"max_order_qty":-0,"max_order_notional":0,"max_abs_position":0,"max_gross_exposure":1000000000000000000}}',
           '{"type":"query","event_id":"q-_Z9"}']
    rc, out, err = run(d, "\n".join(odd))
    exp = run_model([INIT, GOOD.replace('"g1"', '"g1"'),
                     '{"type":"limits","event_id":"l0","limits":{"max_order_qty":0,"max_order_notional":0,"max_abs_position":0,"max_gross_exposure":1000000000000000000}}',
                     '{"type":"query","event_id":"q-_Z9"}'])
    check(rc == 0 and out.splitlines() == exp, f"odd formatting: rc={rc} {out!r} {err}")
    shutil.rmtree(d)

    # Empty input on an empty directory, then a later init still works.
    d = fresh_dir()
    rc, out, _ = run(d, "")
    check(rc == 0 and out == "", "empty input")
    rc, out, _ = run(d, '{"type":"query","event_id":"q"}\n')
    check(out == '{"event_id":"q","status":"rejected","reason":"not_initialised"}\n', "uninitialised query")
    rc, out, _ = run(d, INIT + "\n")
    check(out == '{"event_id":"i1","status":"accepted"}\n', "init after empty runs")
    shutil.rmtree(d)
    print("protocol errors ok")


# ----------------------------------------------------------------- streaming behaviour

def test_streaming():
    """Each response must arrive without further input or EOF, and a protocol
    error must end the process without waiting for more input."""
    import select
    d = fresh_dir()
    p = subprocess.Popen([RUN, "--state-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)

    def ask(line):
        p.stdin.write((line + "\n").encode())
        p.stdin.flush()
        r, _, _ = select.select([p.stdout], [], [], 10)
        return p.stdout.readline().decode().rstrip("\n") if r else None

    check(ask(INIT) == '{"event_id":"i1","status":"accepted"}', "streaming: init answered while stdin open")
    check(ask(GOOD) == '{"event_id":"g1","status":"accepted"}', "streaming: new answered while stdin open")
    p.stdin.write(b"{bad\n")
    p.stdin.flush()
    try:
        rc = p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        rc = None
    check(rc == 2, f"streaming: protocol error exits 2 with stdin still open (rc={rc})")
    p.stdin.close()
    shutil.rmtree(d)
    print("streaming ok")


# ----------------------------------------------------------------- random workloads

SYMS = ["AAA", "BB", "C_1", "DZ"]


def rand_limits(rng):
    return {"max_order_qty": rng.choice([0, 5, 20, 50, 1000000]),
            "max_order_notional": rng.choice([0, 500, 2000, 20000, 10**18]),
            "max_abs_position": rng.choice([0, 3, 10, 40, 10**18]),
            "max_gross_exposure": rng.choice([0, 1000, 5000, 50000, 10**18])}


def gen_events(rng, n, n_orders=40):
    events = []
    oids = ["o%d" % i for i in range(n_orders)]
    k = 0

    def eid():
        nonlocal k
        k += 1
        return "e%d" % k

    if rng.random() < 0.9:
        events.append({"type": "query", "event_id": eid()} if rng.random() < 0.2 else
                      {"type": "fill", "event_id": eid(), "order_id": "o1", "qty": 1})
    syms = [{"symbol": s, "reference_price": rng.randint(1, 300)} for s in SYMS[:rng.randint(1, len(SYMS))]]
    init = {"type": "init", "event_id": eid(), "symbols": syms,
            "limits": {"max_order_qty": 30, "max_order_notional": 5000, "max_abs_position": 40,
                       "max_gross_exposure": rng.choice([5000, 20000, 100000])}}
    events.append(init)
    while len(events) < n:
        x = rng.random()
        if x < 0.40:
            events.append({"type": "new", "event_id": eid(), "order_id": rng.choice(oids),
                           "symbol": rng.choice(SYMS + ["ZZZ"]), "side": rng.choice(["buy", "sell"]),
                           "qty": rng.randint(1, 35), "price": rng.randint(1, 300)})
        elif x < 0.65:
            events.append({"type": "fill", "event_id": eid(), "order_id": rng.choice(oids), "qty": rng.randint(1, 12)})
        elif x < 0.73:
            events.append({"type": "cancel", "event_id": eid(), "order_id": rng.choice(oids)})
        elif x < 0.77:
            lim = rand_limits(rng) if rng.random() < 0.3 else {
                "max_order_qty": rng.randint(5, 40), "max_order_notional": rng.randint(500, 8000),
                "max_abs_position": rng.randint(5, 60), "max_gross_exposure": rng.randint(2000, 100000)}
            events.append({"type": "limits", "event_id": eid(), "limits": lim})
        elif x < 0.83:
            events.append({"type": "query", "event_id": eid()})
        elif x < 0.85:
            s2 = list(syms)
            rng.shuffle(s2)
            events.append({"type": "init", "event_id": eid(), "symbols": s2, "limits": init["limits"]})
        elif x < 0.97 and events:
            # Retry: identical (possibly re-ordered) or conflicting reuse of an old ID.
            old = dict(rng.choice(events))
            if rng.random() < 0.25:
                if old["type"] == "query":
                    old = {"type": "cancel", "event_id": old["event_id"], "order_id": "o1"}
                elif "qty" in old:
                    old["qty"] = old["qty"] % 30 + 1
                elif old["type"] == "init":
                    old["symbols"] = [dict(s, reference_price=s["reference_price"] + 1) for s in old["symbols"]]
                else:
                    old = {"type": "query", "event_id": old["event_id"]}
            elif old["type"] == "init":
                s2 = list(old["symbols"])
                rng.shuffle(s2)
                old["symbols"] = s2
            events.append(old)
        else:
            events.append(dict(init))  # retry of the successful init
    return events


def fmt(rng, ev):
    """Serialise with random key order, whitespace and escapes (all semantically equal)."""
    def enc(v):
        if isinstance(v, dict):
            items = list(v.items())
            rng.shuffle(items)
            sp = rng.choice(["", " ", "\t"])
            return "{" + sp + ("," + sp).join(enc(k) + sp + ":" + sp + enc(x) for k, x in items) + sp + "}"
        if isinstance(v, list):
            return "[" + ",".join(enc(x) for x in v) + "]"
        if isinstance(v, str):
            if rng.random() < 0.05:
                return '"' + "".join("\\u%04x" % ord(c) if rng.random() < 0.3 else c for c in v) + '"'
            return json.dumps(v)
        return json.dumps(v)
    return enc(ev) if rng.random() < 0.3 else json.dumps(ev, separators=(",", ":"))


def test_fuzz(seeds):
    for seed in seeds:
        rng = random.Random(seed)
        events = gen_events(rng, rng.randint(50, 600))
        lines = [fmt(rng, e) for e in events]
        exp = run_model(lines)
        d = fresh_dir()
        # Feed in a few separate process lifetimes.
        cuts = sorted(rng.sample(range(1, len(lines)), 3))
        out = ""
        prev = 0
        for c in cuts + [len(lines)]:
            rc, o, err = run(d, "\n".join(lines[prev:c]) + "\n")
            check(rc == 0, f"fuzz seed {seed} rc={rc} {err}")
            out += o
            prev = c
        got = out.splitlines()
        if not check(got == exp, f"fuzz seed {seed} mismatch"):
            for i, (g, x) in enumerate(zip(got, exp)):
                if g != x:
                    print("  line", i, lines[i], "\n  got ", g, "\n  want", x)
                    break
        shutil.rmtree(d)
    print("fuzz ok (%d seeds)" % len(seeds))


# ----------------------------------------------------------------- crash recovery

def read_lines_until_killed(proc, deadline):
    """Reads complete lines from proc.stdout until `deadline`, then SIGKILLs it."""
    os.set_blocking(proc.stdout.fileno(), False)
    buf = b""
    while time.time() < deadline and proc.poll() is None:
        try:
            chunk = proc.stdout.read()
        except BlockingIOError:
            chunk = None
        if chunk:
            buf += chunk
        else:
            time.sleep(0.0005)
    proc.send_signal(signal.SIGKILL)
    proc.wait()
    try:
        rest = proc.stdout.read()
        if rest:
            buf += rest
    except BlockingIOError:
        pass
    # Only complete newline-terminated lines are acknowledgements.
    return buf.decode().split("\n")[:-1]


def test_crash(seeds):
    for seed in seeds:
        rng = random.Random(1000 + seed)
        events = gen_events(rng, rng.randint(20000, 40000), n_orders=400)
        lines = [json.dumps(e, separators=(",", ":")) for e in events]
        exp = run_model(lines)
        d = fresh_dir()
        i = 0
        kills = 0
        while i < len(lines):
            payload = ("\n".join(lines[i:]) + "\n").encode()
            proc = subprocess.Popen([RUN, "--state-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)
            # Feed from a thread-free writer: write everything, the pipe buffers / blocks.
            import threading
            def feed():
                try:
                    proc.stdin.write(payload)
                    proc.stdin.close()
                except (BrokenPipeError, OSError):
                    pass
            t = threading.Thread(target=feed)
            t.start()
            got = read_lines_until_killed(proc, time.time() + rng.uniform(0.0, 0.04))
            t.join()
            kills += 1
            ok = check(got == exp[i:i + len(got)], f"crash seed {seed}: acknowledged responses diverge at {i}")
            if not ok:
                break
            i += len(got)
            if kills > 400:
                # Make progress deterministic at the end.
                rc, o, _ = run(d, "\n".join(lines[i:]) + "\n")
                check(o.splitlines() == exp[i:], f"crash seed {seed}: tail")
                i = len(lines)
        # Every request, retried in random order, must return its original response.
        idx = list(range(len(lines)))
        rng.shuffle(idx)
        rc, o, _ = run(d, "\n".join(lines[j] for j in idx) + "\n")
        model = Model()
        for l in lines:
            model.process(json.loads(l))
        want = [model.process(json.loads(lines[j])) for j in idx]
        check(rc == 0 and o.splitlines() == want, f"crash seed {seed}: full retry after recovery")
        shutil.rmtree(d)
        print(f"crash seed {seed}: {len(lines)} events, {kills} kills ok")


def test_torn_tail(seeds):
    """Truncate the journal at arbitrary byte offsets (what an interrupted
    append leaves behind) and check recovery yields a consistent prefix."""
    for seed in seeds:
        rng = random.Random(5000 + seed)
        events = gen_events(rng, 300)
        lines = [json.dumps(e, separators=(",", ":")) for e in events]
        exp = run_model(lines)
        d = fresh_dir()
        run(d, "\n".join(lines) + "\n")
        jpath = os.path.join(d, "journal.bin")
        full = open(jpath, "rb").read()
        for _ in range(15):
            cut = rng.randint(0, len(full))
            with open(jpath, "wb") as f:
                f.write(full[:cut])
            # Find how many requests survived: replay everything and check the
            # outputs equal the model (retries of committed requests return the
            # same response the first execution would, so the full sequence
            # must match exactly).
            rc, o, err = run(d, "\n".join(lines) + "\n")
            check(rc == 0 and o.splitlines() == exp, f"torn tail seed {seed} cut {cut}: {err}")
            # Journal is complete again now.
            check(open(jpath, "rb").read() == full, f"torn tail seed {seed} cut {cut}: journal not rebuilt identically")
        shutil.rmtree(d)
    print("torn tail ok")


if __name__ == "__main__":
    subprocess.run([os.path.join(ROOT, "build.sh")], check=True)
    test_samples()
    test_protocol_errors()
    test_streaming()
    test_fuzz(range(20 if QUICK else 150))
    test_torn_tail(range(3 if QUICK else 10))
    test_crash(range(2 if QUICK else 6))
    print("FAILURES:", failures)
    sys.exit(1 if failures else 0)
