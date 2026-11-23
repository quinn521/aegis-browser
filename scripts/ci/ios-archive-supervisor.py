#!/usr/bin/env python3
"""Stream the unsigned compiler log and drain the compiler's private session."""

import errno
import os
import select
import signal
import subprocess
import sys
import time


POLL_INTERVAL = 0.05
READ_SIZE = 64 * 1024


def process_states():
    rows = subprocess.check_output(  # nosec B603 - fixed ps invocation without command lines.
        ["/bin/ps", "-ax", "-o", "pid=", "-o", "stat="], text=True, timeout=2)
    return [(int(pid), state) for pid, state in map(str.split, rows.splitlines())]


def in_session(pid, session):
    try:
        return os.getsid(pid) == session
    except ProcessLookupError:
        return False


def session_workers(session):
    reported = False
    while True:
        try:
            # Reparented zombies may be visible until their new parent reaps them.
            return [pid for pid, state in process_states()
                    if not state.startswith("Z") and in_session(pid, session)]
        except (OSError, subprocess.SubprocessError) as error:
            # Keep the session ID reserved until the process census succeeds.
            if not reported:
                print(f"waiting to confirm unsigned archive session stopped: {error}", file=sys.stderr)
                reported = True
            time.sleep(POLL_INTERVAL)


def signal_workers(workers, session, signum):
    for pid in workers:
        # Check membership again at the point of signalling.
        if in_session(pid, session):
            try:
                os.kill(pid, signum)
            except ProcessLookupError:
                pass


def stop_and_reap(pid):
    # Leave the compiler unreaped to keep its session ID reserved after exit.
    signal_workers(session_workers(pid), pid, signal.SIGTERM)
    deadline = time.monotonic() + 0.5
    empty = False
    while True:
        workers = session_workers(pid)
        if not workers and empty:
            return os.waitpid(pid, 0)[1]
        # Rescan after an empty snapshot in case an exiting worker forked
        # while ps was collecting the process table.
        empty = not workers
        if time.monotonic() >= deadline:
            signal_workers(workers, pid, signal.SIGKILL)
        time.sleep(POLL_INTERVAL)


def write_all(descriptor, data):
    pending = memoryview(data)
    while pending:
        size = os.write(descriptor, pending)
        if size == 0:
            raise OSError(errno.EIO, "empty write")
        pending = pending[size:]


class ArchiveOutput:
    def __init__(self, path):
        self.log = None
        self.diagnostics = True
        self.failed = False
        try:
            self.log = open(path, "wb", buffering=0)
        except OSError as error:
            self.log_error(error)

    def log_error(self, error):
        self.failed = True
        print(f"failed to retain xcodebuild.log: {error}", file=sys.stderr)

    def write(self, data):
        if self.diagnostics:
            try:
                write_all(sys.stdout.fileno(), data)
            except OSError as error:
                self.diagnostics = False
                self.failed = True
                print(f"failed to stream xcodebuild diagnostics: {error}", file=sys.stderr)
        if self.log is not None:
            try:
                write_all(self.log.fileno(), data)
            except OSError as error:
                self.log_error(error)
                self.close()

    def close(self):
        if self.log is not None:
            try:
                self.log.close()
            except OSError as error:
                self.log_error(error)
            finally:
                self.log = None


def copy_chunk(descriptor, output):
    try:
        data = os.read(descriptor, READ_SIZE)
    except BlockingIOError:
        return False
    if data:
        output.write(data)
    return bool(data)


def stream_until_exit(pid, descriptor, output, cancelled):
    # macOS Python 3.9 lacks waitid. Both observers leave the child unreaped.
    queue = None
    changes = []
    ready = [descriptor]
    try:
        if not hasattr(os, "waitid"):
            queue = select.kqueue()
            changes = [select.kevent(pid, filter=select.KQ_FILTER_PROC, flags=select.KQ_EV_ADD,
                                     fflags=select.KQ_NOTE_EXIT)]
        while not cancelled():
            if queue is None:
                if os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG) is not None:
                    return
            else:
                try:
                    events = queue.control(changes, 1, 0)
                except ProcessLookupError:
                    return
                changes = []
                if events:
                    event = events[0]
                    if event.flags & select.KQ_EV_ERROR and event.data != errno.ESRCH:
                        raise OSError(event.data, os.strerror(event.data))
                    return
            if select.select(ready, [], [], POLL_INTERVAL)[0] and not copy_chunk(descriptor, output):
                ready = []
    finally:
        if queue is not None:
            queue.close()


def run_compiler(command, log, cancelled):
    # Verify process enumeration is available before starting anything that
    # might leave a helper in a second process group.
    process_states()
    if cancelled():
        return 0
    output = ArchiveOutput(log)
    try:
        reader, writer = os.pipe()
        try:
            try:
                os.set_blocking(reader, False)
                if cancelled():
                    return 0
                # Create the session atomically with spawn. The supervisor
                # stays the parent and passes the Actions tracking env intact.
                pid = os.posix_spawn(
                    command[0], command, os.environ, setsid=True, setsigmask=(),
                    setsigdef=(signal.SIGINT, signal.SIGTERM, signal.SIGPIPE),
                    file_actions=[(os.POSIX_SPAWN_DUP2, writer, fd) for fd in (1, 2)])
            finally:
                os.close(writer)
            try:
                stream_until_exit(pid, reader, output, cancelled)
            finally:
                status = stop_and_reap(pid)
            # Helpers can hold the pipe open after the compiler exits. Drain
            # buffered diagnostics only after those helpers have stopped.
            while copy_chunk(reader, output):
                pass
        finally:
            os.close(reader)
    finally:
        output.close()
    code = os.waitstatus_to_exitcode(status)
    if code < 0:
        return 128 - code
    return code or (os.EX_IOERR if output.failed else 0)


def main(log, shell, wrapper, *command):
    received = 0

    def cancel(signum, _frame):
        nonlocal received
        received = received or signum

    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    if received:
        return 128 + received
    code = run_compiler(command, log, lambda: received)
    if received:
        return 128 + received
    if code:
        return code
    os.execv(shell, [shell, wrapper, "--verify-archive"])


if __name__ == "__main__":
    try:
        sys.exit(main(*sys.argv[1:]))
    except (OSError, subprocess.SubprocessError) as error:
        sys.exit(f"unsigned archive supervisor: {error}")
