"""Independent reference model of the risk engine (business rules + idempotency).

Deliberately naive: gross exposure and reservations are recomputed from the
order list on every request instead of being maintained incrementally, so it
shares no bookkeeping with the C++ implementation. Assumes schema-valid input.
"""

import json


def dumps(obj):
    return json.dumps(obj, separators=(",", ":"))


def canonical(req):
    c = {k: v for k, v in req.items() if k != "event_id"}
    if c.get("type") == "init":
        c["symbols"] = sorted((s["symbol"], s["reference_price"]) for s in c["symbols"])
    return json.dumps(c, sort_keys=True)


class Model:
    def __init__(self):
        self.events = {}  # event_id -> (canonical, response string)
        self.init = False
        self.limits = None
        self.ref = {}  # symbol -> reference price
        self.pos = {}  # symbol -> filled position
        self.orders = {}  # order_id -> dict(symbol, side, remaining, price, open)

    def _reserved(self, sym, side):
        return sum(o["remaining"] for o in self.orders.values()
                   if o["open"] and o["symbol"] == sym and o["side"] == side)

    def _gross(self):
        g = sum(abs(p) * self.ref[s] for s, p in self.pos.items())
        g += sum(o["remaining"] * o["price"] for o in self.orders.values() if o["open"])
        return g

    def _state(self):
        return {
            "limits": dict(self.limits),
            "gross_exposure": self._gross(),
            "positions": [
                {"symbol": s, "position": self.pos[s],
                 "buy_remaining": self._reserved(s, "buy"),
                 "sell_remaining": self._reserved(s, "sell")}
                for s in sorted(self.pos)
            ],
            "orders": [
                {"order_id": oid, "symbol": o["symbol"], "side": o["side"],
                 "remaining": o["remaining"], "price": o["price"]}
                for oid, o in sorted(self.orders.items()) if o["open"]
            ],
        }

    def _decide(self, r):
        t = r["type"]
        if t == "init":
            if self.init:
                return "already_initialised"
            self.init = True
            self.limits = {k: r["limits"][k] for k in
                           ("max_order_qty", "max_order_notional", "max_abs_position", "max_gross_exposure")}
            for s in r["symbols"]:
                self.ref[s["symbol"]] = s["reference_price"]
                self.pos[s["symbol"]] = 0
            return None
        if not self.init:
            return "not_initialised"
        L = self.limits
        if t == "new":
            if r["order_id"] in self.orders:
                return "duplicate_order_id"
            sym = r["symbol"]
            if sym not in self.ref:
                return "unknown_symbol"
            if r["qty"] > L["max_order_qty"]:
                return "order_qty"
            if r["qty"] * r["price"] > L["max_order_notional"]:
                return "order_notional"
            B = self._reserved(sym, "buy") + (r["qty"] if r["side"] == "buy" else 0)
            S = self._reserved(sym, "sell") + (r["qty"] if r["side"] == "sell" else 0)
            P = self.pos[sym]
            if abs(P + B) > L["max_abs_position"] or abs(P - S) > L["max_abs_position"]:
                return "position_limit"
            if self._gross() + r["qty"] * r["price"] > L["max_gross_exposure"]:
                return "gross_exposure"
            self.orders[r["order_id"]] = {"symbol": sym, "side": r["side"], "remaining": r["qty"],
                                          "price": r["price"], "open": True}
            return None
        if t in ("fill", "cancel"):
            o = self.orders.get(r["order_id"])
            if o is None:
                return "unknown_order"
            if not o["open"]:
                return "order_closed"
            if t == "cancel":
                o["remaining"] = 0
                o["open"] = False
                return None
            if r["qty"] > o["remaining"]:
                return "insufficient_remaining"
            o["remaining"] -= r["qty"]
            self.pos[o["symbol"]] += r["qty"] if o["side"] == "buy" else -r["qty"]
            if o["remaining"] == 0:
                o["open"] = False
            return None
        if t == "limits":
            self.limits = {k: r["limits"][k] for k in
                           ("max_order_qty", "max_order_notional", "max_abs_position", "max_gross_exposure")}
            return None
        if t == "query":
            return None
        raise ValueError(t)

    def process(self, r):
        """Returns the exact response line (without newline)."""
        eid = r["event_id"]
        c = canonical(r)
        if eid in self.events:
            oc, resp = self.events[eid]
            if oc == c:
                return resp
            return dumps({"event_id": eid, "status": "rejected", "reason": "event_id_conflict"})
        reason = self._decide(r)
        if reason is not None:
            resp = dumps({"event_id": eid, "status": "rejected", "reason": reason})
        elif r["type"] == "query":
            resp = dumps({"event_id": eid, "status": "accepted", "state": self._state()})
        else:
            resp = dumps({"event_id": eid, "status": "accepted"})
        self.events[eid] = (c, resp)
        return resp


def run_model(lines):
    m = Model()
    return [m.process(json.loads(l)) for l in lines]
