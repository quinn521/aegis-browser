"""Loopback-only TCP data path using durable W2 byte permits."""

from __future__ import annotations

import argparse
import asyncio
import secrets
import socket
import sys
from pathlib import Path

from ledger import Ledger, LedgerError, MAX_PERMIT_BYTES


class MeteredRelay:
    def __init__(self, ledger: Ledger, account_id: str, period_id: str,
                 origin_port: int, chunk_bytes: int = 4096,
                 io_timeout: float = 30.0, pause_marker: Path | None = None,
                 pause_after_send_marker: Path | None = None):
        if not 1 <= origin_port <= 65535 or not 1 <= chunk_bytes <= MAX_PERMIT_BYTES:
            raise ValueError("invalid relay bounds")
        if not 0 < io_timeout <= 300:
            raise ValueError("invalid I/O timeout")
        self.ledger = ledger
        self.account_id = account_id
        self.period_id = period_id
        self.origin_port = origin_port
        self.chunk_bytes = chunk_bytes
        self.io_timeout = io_timeout
        self.pause_marker = pause_marker
        self.pause_after_send_marker = pause_after_send_marker
        self.listener: socket.socket | None = None
        self.clients: set[asyncio.Task[None]] = set()

    async def _pipe(self, source: socket.socket, destination: socket.socket,
                    stream_id: str, direction: str) -> bool:
        """Return True when the whole connection must stop (quota or failure)."""
        loop = asyncio.get_running_loop()
        sequence = 0
        try:
            while True:
                data = await asyncio.wait_for(loop.sock_recv(source, self.chunk_bytes), self.io_timeout)
                if not data:
                    try:
                        destination.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                    return False
                sequence += 1
                permit = self.ledger.prepare(
                    self.account_id, self.period_id, stream_id, direction,
                    sequence, secrets.token_hex(16), len(data))
                if permit is None:
                    return True
                if self.pause_marker is not None:
                    # Fault injection: the marker is written only after durable PREPARE.
                    self.pause_marker.write_text(permit.permit_id, encoding="ascii")
                    await asyncio.wait_for(asyncio.Event().wait(), 30.0)
                # sock_sendall succeeds after the bytes enter the local OS send
                # path. It does not prove the peer application received them.
                await asyncio.wait_for(
                    loop.sock_sendall(destination, data[:permit.granted_bytes]),
                    self.io_timeout)
                if self.pause_after_send_marker is not None:
                    self.pause_after_send_marker.write_text(permit.permit_id, encoding="ascii")
                    await asyncio.wait_for(asyncio.Event().wait(), 30.0)
                self.ledger.complete(permit.permit_id, permit.granted_bytes)
                if permit.granted_bytes < len(data):
                    return True
        except (LedgerError, OSError, TimeoutError) as error:
            print(f"relay closed {direction}: {error}", file=sys.stderr, flush=True)
            # If sendall or COMPLETE failed, the full durable permit stays held.
            return True

    async def _client(self, client: socket.socket) -> None:
        loop = asyncio.get_running_loop()
        origin = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        origin.setblocking(False)
        stream_id = secrets.token_hex(16)
        try:
            await asyncio.wait_for(loop.sock_connect(origin, ("127.0.0.1", self.origin_port)), self.io_timeout)
            up = asyncio.create_task(self._pipe(client, origin, stream_id, "up"))
            down = asyncio.create_task(self._pipe(origin, client, stream_id, "down"))
            pending: set[asyncio.Task[bool]] = {up, down}
            while pending:
                done, pending = await asyncio.wait(pending, return_when=asyncio.FIRST_COMPLETED)
                if any(task.result() for task in done):
                    for task in pending:
                        task.cancel()
                    if pending:
                        await asyncio.gather(*pending, return_exceptions=True)
                    break
        except (OSError, TimeoutError) as error:
            print(f"origin connection failed: {error}", file=sys.stderr, flush=True)
        finally:
            client.close()
            origin.close()

    async def serve(self, listen_port: int) -> None:
        if not 0 <= listen_port <= 65535:
            raise ValueError("invalid listen port")
        loop = asyncio.get_running_loop()
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", listen_port))
        listener.listen(128)
        listener.setblocking(False)
        self.listener = listener
        print(f"READY {listener.getsockname()[1]}", flush=True)
        try:
            while True:
                client, _ = await loop.sock_accept(listener)
                client.setblocking(False)
                task = asyncio.create_task(self._client(client))
                self.clients.add(task)
                task.add_done_callback(self.clients.discard)
        finally:
            listener.close()
            for task in self.clients:
                task.cancel()
            if self.clients:
                await asyncio.gather(*self.clients, return_exceptions=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="Local W2 byte-permit relay")
    parser.add_argument("--db", required=True)
    parser.add_argument("--account", required=True)
    parser.add_argument("--period", required=True)
    parser.add_argument("--quota", type=int, required=True)
    parser.add_argument("--origin-port", type=int, required=True)
    parser.add_argument("--listen-port", type=int, default=0)
    parser.add_argument("--chunk-bytes", type=int, default=4096)
    parser.add_argument("--io-timeout", type=float, default=30.0)
    parser.add_argument("--pause-after-prepare-marker", type=Path,
                        help="test-only kill window; waits at most 30 seconds")
    parser.add_argument("--pause-after-send-marker", type=Path,
                        help="test-only kill window before COMPLETE; waits at most 30 seconds")
    args = parser.parse_args()
    try:
        ledger = Ledger(args.db)
        ledger.recover()
        ledger.configure(args.account, args.period, args.quota)
        relay = MeteredRelay(ledger, args.account, args.period, args.origin_port,
                             args.chunk_bytes, args.io_timeout,
                             args.pause_after_prepare_marker,
                             args.pause_after_send_marker)
        asyncio.run(relay.serve(args.listen_port))
    except (LedgerError, ValueError, OSError) as error:
        parser.exit(2, f"relay refused to start: {error}\n")


if __name__ == "__main__":
    main()
