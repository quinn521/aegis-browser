"""Reserve an account's remaining bytes on the same transaction as its settlement."""

import argparse
from contextlib import closing
import json
from pathlib import Path
from time import time_ns

from .store import Unavailable

from . import metering, settlement


def _ledger(db, account, period):
    return db.execute(
        "SELECT w.* FROM quota_writers w JOIN quota_flows f USING(node_id,flow_id) "
        "WHERE account_id=? AND period_id=?",
        (account, period),
    )


def inspect(database, account, period):
    with closing(metering._connect(database, "ro")) as db:
        db.execute("BEGIN")
        held = confirmed = released = 0
        for w in _ledger(db, account, period):
            held += w["held"]
            confirmed += w["confirmed_total"]
            released += w["released_total"]
        cuts = [
            dict(r)
            for r in db.execute(
                "SELECT reason,COUNT(*) AS flows, MAX(cut_at) AS lastCutAt FROM quota_flows "
                "WHERE account_id=? AND period_id=? GROUP BY reason ORDER BY reason",
                (account, period),
            )
        ]
        return dict(
            accountId=account,
            periodId=period,
            grantedBytes=confirmed + released + held,
            heldBytes=held,
            confirmedBytes=confirmed,
            releasedBytes=released,
            flowStates=cuts,
        )


class EnforcedMeter(metering.Meter):
    def __init__(self, database, config, clock_ms=lambda: time_ns() // 1_000_000):
        super().__init__(database, config, clock_ms)
        with closing(metering._connect(database, "rw")) as db:
            db.executescript(
                "BEGIN IMMEDIATE;\n" + Path(__file__).with_suffix(".sql").read_text() + "\nCOMMIT;"
            )

    def accept(self, frame):
        self._validate_quota(frame)
        with closing(metering._connect(self.database, "rw")) as db:
            db.execute("PRAGMA synchronous=FULL")
            db.execute("BEGIN IMMEDIATE")
            now = self.clock_ms()
            try:
                result = self._operate(db, frame, now)
            except Unavailable as exc:
                raise ValueError("quota authority unavailable") from exc
            db.commit()
            return result

    @staticmethod
    def _validate_quota(frame):
        if not isinstance(frame, dict):
            raise ValueError("invalid frame")
        base = {"v", "flow", "op"}
        op = frame.get("op")
        # Reuse the protocol version and random flow ID validation.
        metering._validate({k: frame.get(k) for k in base} | {"op": "open", "client": ""})
        if op == "open" and set(frame) == base | {"client", "mode"} and frame["mode"] == "enforce":
            if isinstance(frame["client"], str):
                return
        if op == "finish":
            metering._validate(frame)
            return
        if op == "check" and set(frame) == base:
            return
        if op in ("reserve", "commit"):
            direction = frame.get("direction")
            if (
                not isinstance(direction, str)
                or direction not in metering.PATHS
                or not metering._count(frame.get("ticket"))
                or frame["ticket"] < 1
                or not metering._count(frame.get("bytes"))
                or frame["bytes"] > 16384
            ):
                raise ValueError("invalid ticket")
            fields = base | {"direction", "ticket", "bytes"}
            if (
                op == "reserve"
                and set(frame) == fields | {"path"}
                and frame["bytes"] > 0
                and frame["path"] in metering.PATHS[direction]
            ):
                return
            if (
                op == "commit"
                and set(frame) == fields | {"complete"}
                and type(frame["complete"]) is bool
            ):
                return
        raise ValueError("invalid quota frame")

    def _operate(self, db, frame, now):
        key = (self.node, frame["flow"])
        flow = db.execute("SELECT * FROM quota_flows WHERE node_id=? AND flow_id=?", key).fetchone()
        op = frame["op"]
        if op == "open":
            self._apply(db, {k: v for k, v in frame.items() if k != "mode"}, now)
            if flow is None:
                binding = db.execute(
                    "SELECT account_id,period_id FROM node_meter_flows "
                    "WHERE node_id=? AND flow_id=?",
                    key,
                ).fetchone()
                auth = settlement.authority(db, *binding, now)
                if auth is None or not settlement.admitted(db, *binding, self.node):
                    raise ValueError("no active quota")
                db.execute(
                    "INSERT INTO quota_flows(node_id,flow_id,account_id,period_id,expires_at) "
                    "VALUES (?,?,?,?,?)",
                    (*key, *binding, auth["expires_at"]),
                )
                flow = db.execute(
                    "SELECT * FROM quota_flows WHERE node_id=? AND flow_id=?", key
                ).fetchone()
        if flow is None:
            raise ValueError("unknown quota flow")
        if op == "finish":
            state = db.execute(
                "SELECT uploaded, downloaded FROM node_meter_flows WHERE node_id=? AND flow_id=?",
                key,
            ).fetchone()
            if tuple(state) != (frame["uploadedBytes"], frame["downloadedBytes"]):
                raise ValueError("finish differs from committed writes")
            if frame["complete"] and self._pending(db, key):
                raise ValueError("unconfirmed reservations")
            self._apply(db, frame, now)
            db.execute(
                "UPDATE quota_flows SET cut_at=?,reason='finished' WHERE node_id=? "
                "AND flow_id=? AND cut_at IS NULL",
                (now, *key),
            )
            return {"ok": True}
        if op == "commit":
            return self._commit(db, key, frame, flow, now)
        if flow["cut_at"] is not None:
            return {"ok": False}
        auth = settlement.authority(db, flow["account_id"], flow["period_id"], now)
        if now >= flow["expires_at"] or auth is None:
            self._cut(db, key, now, "expired" if now >= flow["expires_at"] else "unavailable")
            return {"ok": False}
        expiry = auth["expires_at"]
        db.execute(
            "UPDATE quota_flows SET expires_at=? WHERE node_id=? AND flow_id=? AND expires_at<>?",
            (expiry, *key, expiry),
        )
        shared = (flow["account_id"], flow["period_id"])
        held = self._held(db, shared)
        room = auth["quota_bytes"] - auth["uploaded_bytes"] - auth["downloaded_bytes"] - held
        if op == "reserve":
            return self._reserve(db, key, frame, shared, now, room, expiry)
        owns_pending = self._pending(db, key)
        if room <= 0 and not owns_pending:
            self._cut(db, key, now, "quota")
            return {"ok": False}
        return dict(ok=True, expiresAt=expiry)

    @staticmethod
    def _held(db, shared):
        row = db.execute(
            "SELECT held FROM quota_holds WHERE account_id=? AND period_id=?", shared
        ).fetchone()
        return row[0] if row else 0

    @staticmethod
    def _cut(db, key, now, reason):
        db.execute(
            "UPDATE quota_flows SET cut_at=?,reason=? WHERE node_id=? AND flow_id=? "
            "AND cut_at IS NULL",
            (now, reason, *key),
        )

    @staticmethod
    def _pending(db, key):
        return (
            db.execute(
                "SELECT 1 FROM quota_writers WHERE node_id=? AND flow_id=? "
                "AND confirmed IS NULL LIMIT 1",
                key,
            ).fetchone()
            is not None
        )

    @staticmethod
    def _cut_empty(db, shared, now):
        # Fence previous streams atomically, even if a top-up arrives before their next poll.
        db.execute(
            "UPDATE quota_flows SET cut_at=?,reason='quota' WHERE account_id=? AND period_id=? "
            "AND cut_at IS NULL AND NOT EXISTS (SELECT 1 FROM quota_writers w "
            "WHERE w.node_id=quota_flows.node_id AND w.flow_id=quota_flows.flow_id "
            "AND w.confirmed IS NULL)",
            (now, *shared),
        )

    def _reserve(self, db, key, frame, shared, now, room, expiry):
        where = (*key, frame["direction"])
        w = db.execute(
            "SELECT * FROM quota_writers WHERE node_id=? AND flow_id=? AND direction=?", where
        ).fetchone()
        ticket, requested, path = frame["ticket"], frame["bytes"], frame["path"]
        if w is not None and ticket == w["ticket"]:
            if (requested, path) != (w["requested"], w["path"]) or w["confirmed"] is not None:
                raise ValueError("closed or conflicting reservation")
            return dict(ok=True, granted=w["granted"], expiresAt=min(w["deadline"], expiry))
        if (
            ticket != (1 if w is None else w["ticket"] + 1)
            or w is not None
            and w["confirmed"] is None
        ):
            raise ValueError("unresolved ticket")
        if room <= 0:
            if self._pending(db, key):
                return dict(ok=False, reason="pending")
            self._cut(db, key, now, "quota")
            return dict(ok=False)
        granted = min(requested, room)
        db.execute(
            "INSERT INTO quota_writers VALUES (?,?,?,?,?,?,?,?,NULL,NULL,?,0,0) "
            "ON CONFLICT(node_id,flow_id,direction) DO UPDATE SET ticket=excluded.ticket, "
            "path=excluded.path,requested=excluded.requested,granted=excluded.granted,"
            "deadline=excluded.deadline,confirmed=NULL,complete=NULL,held=held+excluded.held",
            (*where, ticket, path, requested, granted, expiry, granted),
        )
        if granted == room:
            self._cut_empty(db, shared, now)
        return dict(ok=True, granted=granted, expiresAt=expiry)

    def _commit(self, db, key, frame, flow, now):
        where = (*key, frame["direction"])
        w = db.execute(
            "SELECT * FROM quota_writers WHERE node_id=? AND flow_id=? AND direction=?", where
        ).fetchone()
        n, complete = frame["bytes"], frame["complete"]
        if w is None or w["ticket"] != frame["ticket"] or n > w["granted"]:
            raise ValueError("unmatched confirmation")
        if w["confirmed"] is not None:
            if (n, complete) != (w["confirmed"], bool(w["complete"])):
                raise ValueError("conflicting confirmation")
            return dict(ok=True)
        released = w["granted"] - n if complete else 0
        db.execute(
            "UPDATE quota_writers SET confirmed=?,complete=?,held=held-?,"
            "confirmed_total=confirmed_total+?,released_total=released_total+? "
            "WHERE node_id=? AND flow_id=? AND direction=?",
            (n, int(complete), n + released, n, released, *where),
        )
        if n:
            row = db.execute(
                "SELECT * FROM node_meter_flows WHERE node_id=? AND flow_id=?", key
            ).fetchone()
            direction, path = frame["direction"], w["path"]
            self._apply(
                db,
                dict(
                    v=1,
                    op="sample",
                    flow=key[1],
                    direction=direction,
                    bytes=row[metering.DIRECTION[direction]] + n,
                    path=path,
                    pathBytes=row[path] + n,
                ),
                now,
            )
        shared = (flow["account_id"], flow["period_id"])
        auth = settlement.authority(db, *shared, now)
        if auth is None or (
            auth["quota_bytes"] - auth["uploaded_bytes"] - auth["downloaded_bytes"]
            <= self._held(db, shared)
        ):
            self._cut_empty(db, shared, now)
        if not complete:
            self._cut(db, key, now, "uncertain")
        return dict(ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("database", "account", "period"):
        parser.add_argument("--" + field, required=True)
    args = parser.parse_args()
    print(json.dumps(inspect(args.database, args.account, args.period)))


if __name__ == "__main__":
    main()
