#!/usr/bin/env python3
"""Check the held-open-stdin dialer regression on a prepared macOS owned core."""

import array
import json
import os
from pathlib import Path
import select
import socket
import subprocess  # nosec B404 - launches the digest-verified fixture with fixed arguments.
import sys
import time

from prepare import verify_digest


WRAPPER = """
import os, sys
fd = int(sys.argv[1])
os.dup2(fd, 3, inheritable=True)
if fd != 3:
    os.close(fd)
os.execv(sys.argv[2], [sys.argv[2], "run", "-config", "stdin:"])
"""


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reachable(port):
    with socket.socket() as probe:
        probe.settimeout(0.2)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def dialer_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def stop(child):
    if child.poll() is not None:
        return
    child.terminate()
    try:
        child.wait(5)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait(5)


def check_stock_dialer(binary):
    port = dialer_port()
    env = {"XRAY_BROWSER_DIALER": f"127.0.0.1:{port}"}
    # nosemgrep: dangerous-subprocess-use-audit, dangerous-subprocess-use-tainted-env-args
    with subprocess.Popen([binary, "run", "-config", "stdin:"], env=env, shell=False,  # nosec B603
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as child:
        try:
            deadline = time.monotonic() + 10
            while not reachable(port):
                require(child.poll() is None, "stock Xray exited before reading stdin")
                require(time.monotonic() < deadline, "stock Xray did not open the configured dialer")
                time.sleep(0.025)
        finally:
            stop(child)


def check_owned_dialer(binary):
    port = dialer_port()
    env = {"XRAY_BROWSER_DIALER": f"127.0.0.1:{port}",
           "AEGIS_OWNED_PARENT_PID": str(os.getpid()), "AEGIS_OWNED_LISTENER_LEASE_FD": "3"}
    receiver, sender = socket.socketpair()
    with receiver, sender:
        receiver.settimeout(10)
        # nosemgrep: dangerous-subprocess-use-audit, dangerous-subprocess-use-tainted-env-args
        with subprocess.Popen([sys.executable, "-I", "-B", "-c", WRAPPER, str(sender.fileno()), binary],  # nosec B603
                              env=env, pass_fds=(sender.fileno(),), stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as child:
            sender.close()
            try:
                ready, _, _ = select.select([child.stdout], [], [], 10)
                require(ready and child.stdout.readline(), "owned Xray did not reach config input")
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    require(not reachable(port), "owned Xray opened the auxiliary dialer before EOF")
                    time.sleep(0.025)
                settings = {
                    "log": {"loglevel": "none"},
                    "inbounds": [{"listen": "127.0.0.1", "port": "0", "protocol": "http"}],
                    "outbounds": [{"protocol": "freedom"}],
                }
                child.stdin.write(json.dumps(settings).encode())
                child.stdin.close()
                descriptors = array.array("i")
                payload, controls, _, _ = receiver.recvmsg(2, socket.CMSG_SPACE(descriptors.itemsize))
                for level, kind, data in controls:
                    if (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS):
                        descriptors.frombytes(data)
                for descriptor in descriptors:
                    os.close(descriptor)
                require(payload == b"\x01" and len(descriptors) == 1, "owned Xray did not publish its lease")
                require(not reachable(port), "owned Xray opened the auxiliary dialer after EOF")
            finally:
                stop(child)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: python3 test_startup.py /absolute/path/to/xray")
    executable = Path(sys.argv[1]).resolve(strict=True)
    lock = json.loads(Path(__file__).with_name("owned-core-lock.json").read_text())
    verify_digest(executable, lock["artifact"]["sha256"])
    check_owned_dialer(executable)
    check_stock_dialer(executable)
    print("PASS owned startup and marker-free dialer behavior")
