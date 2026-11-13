"""Real two-account Xray quota contention, cutoff, restart and confirmed top-up."""
import concurrent.futures
import contextlib
import json
import os
import socket
import socketserver
import sqlite3
import subprocess
import sys
import threading
import time

from ... import orders
from .acceptance import run
from .runtime import HOST, PERIOD, Runtime, require, until
from .settlement import account_command, entitlement
from .tls_target import PlainOrigin

LIMIT = 1 << 20
CHUNK = 8192


class Peer(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        sock.settimeout(15)
        tag = sock.recv(1)
        if tag not in (b"U", b"D", b"I", b"B", b"R", b"E"):
            return
        state = self.server.state
        with self.server.lock:
            state[tag] = {"up": 1, "sent": 0, "closed": False}
        def send(data):
            sock.sendall(data)
            with self.server.lock:
                state[tag]["sent"] += len(data)
        try:
            send(b"+")
            if tag == b"D":
                self.server.start_down.wait(10)
                for _ in range(256):
                    send(b"d" * CHUNK)
                    time.sleep(.004)
            else:
                while data := sock.recv(CHUNK):
                    with self.server.lock:
                        state[tag]["up"] += len(data)
                    if tag in (b"B", b"R"):
                        send(data)
        except OSError:
            pass
        finally:
            with self.server.lock:
                state[tag]["closed"] = True


class GuardRuntime(Runtime):
    def setup_targets(self):
        super().setup_targets()
        target = PlainOrigin((HOST, 0), Peer)
        target.state = {}
        target.lock = threading.Lock()
        target.start_down = threading.Event()
        self.guard_target = target
        self.plain_port = self.serve(target)
        self.plain_authority = f"localhost:{self.plain_port}"

    def seed(self):
        super().seed()
        with contextlib.closing(sqlite3.connect(self.db)) as db:
            db.execute("UPDATE snapshots SET quota_bytes=? WHERE account_id='a'", (LIMIT,))
            db.commit()

    def start_collector(self):
        self.collector = self.launch("collector", [*self.cli, "serve", "--database", self.db,
                                     "--config", self.config, "--socket", self.socket_path, "--enforce-quota"],
                                     socket_path=self.socket_path)

    def start_xray(self, socks):
        self.socks = socks
        super().start_xray(socks)

    def launch(self, name, args, **kwargs):
        if kwargs.get("meter"):
            # Put the switch only in the dedicated Xray server process.
            args = ["env", "AEGIS_XRAY_QUOTA=enforce", *args]
        return super().launch(name, args, **kwargs)

    def restart_xray(self):
        self.xray_process.terminate()
        self.xray_process.wait(timeout=5)
        self.xray_process = self.launch("server-xray-restarted", [self.metered, "run", "-c",
                                      self.root / "xserver.json"], service_port=self.reality_port, meter=True)

    def inspect(self):
        return json.loads(subprocess.check_output([sys.executable, "-m", "services.access_account.quota",
                          "--database", self.db, "--account", "a", "--period", PERIOD]))

    def flow(self, account, tag):
        sock = self.connect(account, plain=True)
        sock.settimeout(12)
        sock.sendall(tag)
        require(sock.recv(1) == b"+", "No target confirmation for " + tag.decode())
        return sock


def denied(rt):
    try:
        probe = rt.flow("a", b"R")
    except (ConnectionError, EOFError, AssertionError):
        return
    probe.close()
    raise AssertionError("A new paid flow was able to reach the origin")


def echo(sock):
    sock.sendall(b"continuity")
    got = b""
    while len(got) < 10:
        more = sock.recv(10 - len(got))
        require(more, "Other account stopped")
        got += more
    require(got == b"continuity", "Other account payload changed")


def closed(sock):
    try:
        return sock.recv(1) == b""
    except ConnectionError:
        return True


def exercise(rt, emit):
    rt.setup()
    for n in ("a", "b"):
        account_command(rt, "bind", n, PERIOD)
    upload, download, idle = [rt.flow("a", tag) for tag in (b"U", b"D", b"I")]
    other = rt.flow("b", b"B")
    echo(other)
    base = rt.inspect()
    emit("open", base)
    require(base["confirmedBytes"] <= 6, "Unexpected pre-content byte usage")

    def send():
        try:
            for _ in range(256):
                upload.sendall(b"u" * CHUNK)
                time.sleep(.004)
        except ConnectionError:
            pass

    def receive():
        n = 0
        while True:
            try:
                data = download.recv(CHUNK)
            except ConnectionError:
                break
            if not data:
                break
            n += len(data)
        return n

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        receiving = pool.submit(receive)
        sending = pool.submit(send)
        rt.guard_target.start_down.set()
        received = receiving.result(timeout=15)
        sending.result(timeout=15)
    require(closed(idle) and closed(upload), "A previously opened socket remained usable")
    until(lambda: all(rt.guard_target.state[t]["closed"] for t in (b"U", b"D", b"I")))
    end = rt.inspect()
    rows = {r["accountId"]: r for r in rt.usage()["accounts"]}
    up = sum(rt.guard_target.state[t]["up"] for t in (b"U", b"D", b"I"))
    down = received + 3  # The three clients received one ready marker each.
    require(up > CHUNK and received > CHUNK, "Both contenders must transfer real data")
    require((up, down) == (rows["a"]["uploadedBytes"], rows["a"]["downloadedBytes"]),
            f"Observed endpoint bytes {(up, down)} disagree with paid writes {rows['a']}")
    require(up + down == LIMIT and end["confirmedBytes"] == LIMIT and end["heldBytes"] == 0
            and end["grantedBytes"] - end["releasedBytes"] == LIMIT, "Shared quota did not reconcile")
    require(entitlement(rt, "a")["status"] == "exhausted", "Account still has paid access")
    denied(rt)
    echo(other)
    emit("exhausted", {"quota": end, "acceptedUplink": up, "deliveredDownlink": down,
                       "offeredTargetDownlink": rt.guard_target.state[b"D"]["sent"],
                       "BContinuedOnExistingSocket": True})

    # Restart both billing daemon and the actual metered node; same database, same priors.
    other.close()
    rt.collector.terminate()
    rt.collector.wait(timeout=5)
    rt.socket_path.unlink(missing_ok=True)
    rt.start_collector()
    rt.restart_xray()
    require(rt.inspect()["confirmedBytes"] == LIMIT, "Restart reset quota consumption")
    denied(rt)
    emit("restart", rt.inspect())

    orders.migrate(rt.db)
    terms = dict(order_id="test-replenishment", account_id="a", plan_id="paid", period_id=PERIOD,
                 panel_client="client-a", quota_bytes=65536, duration_ms=60000)
    orders.record(rt.db, **terms, paid=False)
    denied(rt)
    orders.record(rt.db, **terms, paid=True)
    require(orders.redeem(rt.db, rt.tokens["a"], terms["order_id"], lambda: time.time_ns() // 1000000),
            "Top-up did not settle")
    resumed = rt.flow("a", b"R")
    echo(resumed)
    resumed.close()
    emit("confirmed-topup", {"entitlement": entitlement(rt, "a"), "quota": rt.inspect()})

    old = rt.flow("a", b"E")
    # Make the configured period expire while balance remains. Grant/guard consult the
    # same authorized terms, independently of requests to the account display endpoint.
    expires = time.time_ns() // 1000000 + 1500
    with contextlib.closing(sqlite3.connect(rt.db)) as db:
        db.execute("UPDATE snapshots SET expires_at=? WHERE account_id='a'", (expires,))
        db.commit()
    require(closed(old), "Expiry failed to close the previously open connection")
    elapsed = time.time_ns() // 1000000 - expires
    require(-30 <= elapsed < 3600, "Expiry cutoff outside bounded interval")
    denied(rt)
    value = entitlement(rt, "a")
    require(value["status"] == "expired" and int(value["remainingBytes"]) > 0, "Wrong expiry state")
    emit("expired", {"deadlineMs": expires, "closeDelayMs": elapsed, "entitlement": value,
                     "quota": rt.inspect()})


if __name__ == "__main__":
    run(exercise, runtime_class=GuardRuntime)
