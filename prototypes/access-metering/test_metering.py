"""Bounded unit and real loopback regression tests for the W2 fixture."""

from __future__ import annotations

import os
import select
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from ledger import Ledger, LedgerError, MAX_PENDING_PERMITS, MAX_PERMIT_BYTES
from origin import EchoOrigin


RELAY = Path(__file__).with_name("relay.py")


def wait_until(predicate, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("bounded wait expired")


def receive_exact(connection: socket.socket, count: int) -> bytes:
    result = bytearray()
    while len(result) < count:
        data = connection.recv(count - len(result))
        if not data:
            raise AssertionError(f"connection closed after {len(result)} of {count} bytes")
        result.extend(data)
    return bytes(result)


class LedgerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aegis-w2-ledger-")
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "ledger.db"
        self.ledger = Ledger(self.path)
        self.ledger.configure("account", "period-1", 10)

    def test_account_wide_two_direction_reservations_and_idempotent_completion(self) -> None:
        first = self.ledger.prepare("account", "period-1", "stream-a", "up", 1, "permit-a", 4)
        second = self.ledger.prepare("account", "period-1", "stream-b", "down", 1, "permit-b", 4)
        third = self.ledger.prepare("account", "period-1", "stream-c", "up", 1, "permit-c", 4)
        self.assertEqual((first.granted_bytes, second.granted_bytes, third.granted_bytes), (4, 4, 2))
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes, balance.remaining_bytes), (0, 10, 0))
        self.assertIsNone(self.ledger.prepare("account", "period-1", "stream-d", "down", 1, "permit-d", 1))
        for permit in (first, second, third):
            self.assertTrue(self.ledger.complete(permit.permit_id, permit.granted_bytes))
        self.assertFalse(self.ledger.complete("permit-a", 4))
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes, balance.remaining_bytes), (10, 0, 0))

    def test_duplicate_gap_wrong_amount_and_oversized_grant_fail_closed(self) -> None:
        self.ledger.prepare("account", "period-1", "stream", "up", 1, "permit", 4)
        for sequence, permit_id, requested in ((1, "replay", 1), (3, "gap", 1),
                                               (2, "permit", 1), (2, "huge", MAX_PERMIT_BYTES + 1)):
            with self.subTest(sequence=sequence, permit_id=permit_id):
                with self.assertRaises(LedgerError):
                    self.ledger.prepare("account", "period-1", "stream", "up",
                                        sequence, permit_id, requested)
        with self.assertRaises(LedgerError):
            self.ledger.complete("permit", 3)
        self.assertEqual(self.ledger.snapshot("account", "period-1").held_bytes, 4)
        self.assertTrue(self.ledger.complete("permit", 4))
        self.assertFalse(self.ledger.complete("permit", 4))

    def test_stale_period_cannot_advance_or_reuse_old_quota(self) -> None:
        self.ledger.prepare("account", "period-1", "stream", "up", 1, "permit", 4)
        with self.assertRaises(LedgerError):
            self.ledger.rollover("account", "period-1", "period-2", 5)
        self.ledger.complete("permit", 4)
        self.ledger.rollover("account", "period-1", "period-2", 5)
        self.assertEqual(self.ledger.snapshot("account", "period-2").remaining_bytes, 5)
        for operation in (
            lambda: self.ledger.snapshot("account", "period-1"),
            lambda: self.ledger.prepare("account", "period-1", "old", "up", 1, "old-permit", 1),
            lambda: self.ledger.complete("permit", 4),
            lambda: self.ledger.configure("account", "period-2", 6),
            lambda: self.ledger.rollover("account", "period-2", "period-1", 10),
        ):
            with self.assertRaises(LedgerError):
                operation()
        self.assertEqual(self.ledger.snapshot("account", "period-2").actual_bytes, 0)

    def test_pending_cap_fails_closed_even_when_quota_remains(self) -> None:
        self.ledger.configure("large", "period-1", MAX_PENDING_PERMITS * MAX_PERMIT_BYTES + 1)
        for index in range(MAX_PENDING_PERMITS):
            permit = self.ledger.prepare("large", "period-1", f"stream-{index}", "up",
                                         1, f"permit-{index}", MAX_PERMIT_BYTES)
            self.assertEqual(permit.granted_bytes, MAX_PERMIT_BYTES)
        self.ledger.configure("other-account", "period-1", 1)
        balance = self.ledger.snapshot("large", "period-1")
        self.assertEqual(balance.remaining_bytes, 1)
        self.assertEqual(balance.held_bytes, MAX_PENDING_PERMITS * MAX_PERMIT_BYTES)
        with self.assertRaisesRegex(LedgerError, "pending permit cap"):
            self.ledger.prepare("large", "period-1", "extra", "down", 1, "extra", 1)
        with self.assertRaisesRegex(LedgerError, "pending permit cap"):
            self.ledger.prepare("other-account", "period-1", "other-stream", "up",
                                1, "other-permit", 1)
        self.assertEqual(self.ledger.snapshot("large", "period-1"), balance)
        self.ledger.complete("permit-0", MAX_PERMIT_BYTES)
        self.assertEqual(self.ledger.prepare("large", "period-1", "extra", "down", 1,
                                             "extra", 1).granted_bytes, 1)

    def test_reopened_uncertain_permit_cannot_complete_without_proof(self) -> None:
        self.ledger.prepare("account", "period-1", "stream", "up", 1, "pending", 4)
        reopened = Ledger(self.path)
        self.assertEqual(reopened.snapshot("account", "period-1").uncertain_bytes, 0)
        self.assertEqual(reopened.recover(), 1)
        with self.assertRaisesRegex(LedgerError, "uncertain permit"):
            reopened.complete("pending", 4)
        balance = reopened.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes,
                          balance.uncertain_bytes, balance.remaining_bytes), (0, 4, 4, 6))

    def test_pending_cap_survives_recovery_without_reissue(self) -> None:
        self.ledger.configure("large", "period-1", MAX_PENDING_PERMITS * MAX_PERMIT_BYTES + 1)
        for index in range(MAX_PENDING_PERMITS):
            self.ledger.prepare("large", "period-1", f"stream-{index}", "up",
                                1, f"permit-{index}", MAX_PERMIT_BYTES)
        reopened = Ledger(self.path)
        self.assertEqual(reopened.recover(), MAX_PENDING_PERMITS)
        balance = reopened.snapshot("large", "period-1")
        self.assertEqual(balance.uncertain_bytes, MAX_PENDING_PERMITS * MAX_PERMIT_BYTES)
        self.assertEqual(balance.remaining_bytes, 1)
        with self.assertRaisesRegex(LedgerError, "pending permit cap"):
            reopened.prepare("large", "period-1", "fresh", "up", 1, "fresh-permit", 1)
        with self.assertRaisesRegex(LedgerError, "uncertain permit"):
            reopened.complete("permit-0", MAX_PERMIT_BYTES)

    def test_corrupt_or_uninitialized_file_refuses_start(self) -> None:
        for content in (b"not a database", b""):
            path = Path(self.temporary.name) / f"bad-{len(content)}.db"
            path.write_bytes(content)
            with self.assertRaises(LedgerError):
                Ledger(path)


class RelayTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aegis-w2-relay-")
        self.addCleanup(self.temporary.cleanup)
        self.db = Path(self.temporary.name) / "ledger.db"
        self.ledger = Ledger(self.db)
        self.origin = EchoOrigin()
        self.origin.start()
        self.addCleanup(self.origin.close)
        self.processes: list[subprocess.Popen[str]] = []
        self.connections: list[socket.socket] = []
        self.addCleanup(self._cleanup_processes)

    def _cleanup_processes(self) -> None:
        for connection in self.connections:
            connection.close()
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=2)

    def _start_relay(self, quota: int, chunk: int = 4, marker: Path | None = None,
                     after_send_marker: Path | None = None) -> int:
        self.ledger.configure("account", "period-1", quota)
        command = [sys.executable, "-u", str(RELAY), "--db", str(self.db),
                   "--account", "account", "--period", "period-1", "--quota", str(quota),
                   "--origin-port", str(self.origin.port), "--chunk-bytes", str(chunk),
                   "--io-timeout", "2"]
        if marker is not None:
            command.extend(("--pause-after-prepare-marker", str(marker)))
        if after_send_marker is not None:
            command.extend(("--pause-after-send-marker", str(after_send_marker)))
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=environment)
        self.processes.append(process)
        readable, _, _ = select.select([process.stdout], [], [], 3)
        if not readable:
            self.fail("relay did not become ready within 3 seconds")
        line = process.stdout.readline().strip()
        if not line.startswith("READY "):
            self.fail(f"relay startup failed: {line}; exit={process.poll()}")
        return int(line.split()[1])

    def _connect(self, port: int) -> socket.socket:
        connection = socket.create_connection(("127.0.0.1", port), timeout=2)
        connection.settimeout(2)
        self.connections.append(connection)
        return connection

    def test_usage_changes_while_long_connection_stays_open(self) -> None:
        connection = self._connect(self._start_relay(64))
        for round_number in range(1, 4):
            connection.sendall(b"ping")
            self.assertEqual(receive_exact(connection, 4), b"ping")
            wait_until(lambda: self.ledger.snapshot("account", "period-1").actual_bytes == 8 * round_number)
            wait_until(lambda: self.origin.counts() == (4 * round_number, 4 * round_number))
            balance = self.ledger.snapshot("account", "period-1")
            self.assertEqual((balance.actual_bytes, balance.held_bytes), (8 * round_number, 0))
            self.assertEqual(self.origin.counts(), (4 * round_number, 4 * round_number))
        self.assertEqual(self.origin.counts(), (12, 12))

    def test_actual_forwarding_stops_at_shared_two_direction_quota(self) -> None:
        connection = self._connect(self._start_relay(10))
        connection.sendall(b"abcd")
        self.assertEqual(receive_exact(connection, 4), b"abcd")
        wait_until(lambda: self.ledger.snapshot("account", "period-1").actual_bytes == 8)
        wait_until(lambda: self.origin.counts() == (4, 4))
        self.assertEqual(self.ledger.snapshot("account", "period-1").actual_bytes, 8)
        self.assertEqual(self.origin.counts(), (4, 4))
        connection.sendall(b"wxyz")
        self.assertEqual(connection.recv(8), b"")
        wait_until(lambda: self.origin.counts()[0] == 6)
        wait_until(lambda: self.ledger.snapshot("account", "period-1").actual_bytes == 10)
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes, balance.remaining_bytes), (10, 0, 0))
        self.assertEqual(self.origin.counts()[0], 6)
        with sqlite3.connect(self.db) as connection_db:
            amounts = dict(connection_db.execute("""SELECT direction, SUM(actual_bytes)
                FROM permits WHERE state='COMPLETE' GROUP BY direction""").fetchall())
        self.assertEqual(amounts, {"up": 6, "down": 4})

    def test_two_streams_share_one_account_and_both_directions(self) -> None:
        port = self._start_relay(16)
        first = self._connect(port)
        second = self._connect(port)
        first.sendall(b"one!")
        self.assertEqual(receive_exact(first, 4), b"one!")
        wait_until(lambda: self.ledger.snapshot("account", "period-1").actual_bytes == 8)
        second.sendall(b"two!")
        self.assertEqual(receive_exact(second, 4), b"two!")
        wait_until(lambda: self.ledger.snapshot("account", "period-1").actual_bytes == 16)
        wait_until(lambda: self.origin.counts() == (8, 8))
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.remaining_bytes), (16, 0))
        self.assertEqual(self.origin.counts(), (8, 8))
        first.sendall(b"more")
        self.assertEqual(first.recv(1), b"")
        self.assertEqual(self.origin.counts(), (8, 8))

    def test_runtime_ledger_mismatch_blocks_forwarding(self) -> None:
        connection = self._connect(self._start_relay(32))
        with sqlite3.connect(self.db) as connection_db:
            connection_db.execute("PRAGMA application_id=0")
        connection.sendall(b"stop")
        self.assertEqual(connection.recv(1), b"")
        self.assertEqual(self.origin.counts()[0], 0)

    def test_kill_after_prepare_keeps_uncertain_bytes_held_after_restart(self) -> None:
        marker = Path(self.temporary.name) / "prepared.marker"
        first_port = self._start_relay(8, chunk=8, marker=marker)
        first = self._connect(first_port)
        first.sendall(b"12345678")
        wait_until(marker.exists)
        self.assertEqual(self.origin.counts()[0], 0)
        first_process = self.processes[-1]
        first_process.kill()
        first_process.wait(timeout=2)

        self.ledger = Ledger(self.db)
        self.assertEqual(self.ledger.recover(), 1)
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes,
                          balance.uncertain_bytes, balance.remaining_bytes), (0, 8, 8, 0))
        second = self._connect(self._start_relay(8, chunk=8))
        second.sendall(b"abcdefgh")
        self.assertEqual(second.recv(1), b"")
        self.assertEqual(self.origin.counts()[0], 0)
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes,
                          balance.uncertain_bytes, balance.remaining_bytes), (0, 8, 8, 0))

    def test_kill_after_socket_send_preserves_forwarded_but_unsettled_uncertainty(self) -> None:
        sink = EchoOrigin(echo=False)
        sink.start()
        self.addCleanup(sink.close)
        self.origin = sink
        marker = Path(self.temporary.name) / "sent.marker"
        port = self._start_relay(8, chunk=8, after_send_marker=marker)
        client = self._connect(port)
        client.sendall(b"12345678")
        wait_until(marker.exists)
        wait_until(lambda: sink.counts()[0] == 8)
        self.processes[-1].kill()
        self.processes[-1].wait(timeout=2)
        self.ledger = Ledger(self.db)
        self.assertEqual(self.ledger.recover(), 1)
        balance = self.ledger.snapshot("account", "period-1")
        self.assertEqual((balance.actual_bytes, balance.held_bytes,
                          balance.uncertain_bytes, balance.remaining_bytes), (0, 8, 8, 0))
        self.assertEqual(sink.counts(), (8, 0))
        second = self._connect(self._start_relay(8, chunk=8))
        second.sendall(b"abcdefgh")
        self.assertEqual(second.recv(1), b"")
        self.assertEqual(sink.counts(), (8, 0))


if __name__ == "__main__":
    unittest.main()
