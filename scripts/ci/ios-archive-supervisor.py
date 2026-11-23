#!/usr/bin/env python3
"""Keep the unsigned archive worker and its process group owned by the CI step."""

import errno
import os
import select
import signal
import subprocess
import sys
import time


def wait_for_worker(pid, cancelled):
    # Observe exit without reaping. The child's PID reserves the group ID
    # until the supervisor has finished signalling that group.
    if hasattr(os, "waitid"):
        while not cancelled() and os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG) is None:
            time.sleep(0.05)
    else:
        queue = select.kqueue()
        try:
            changes = [select.kevent(pid, filter=select.KQ_FILTER_PROC, flags=select.KQ_EV_ADD,
                                     fflags=select.KQ_NOTE_EXIT)]
            while not cancelled():
                events = queue.control(changes, 1, 0.05)
                changes = []
                if events:
                    event = events[0]
                    if event.flags & select.KQ_EV_ERROR and event.data != errno.ESRCH:
                        raise OSError(event.data, os.strerror(event.data))
                    return
        finally:
            queue.close()


def group_has_workers(pgid):
    # A dead orphan may remain as a zombie until its new parent reaps it.
    # Only the kernel-assigned group and process state are relevant.
    rows = subprocess.check_output(  # nosec B603 - fixed ps invocation, no shell or user arguments.
        ["/bin/ps", "-ax", "-o", "pgid=", "-o", "stat="], text=True, timeout=2)
    group_id = str(pgid)
    return any(len(row) == 2 and row[0] == group_id and not row[1].startswith("Z")
               for row in map(str.split, rows.splitlines()))


def signal_group(pgid, signum):
    try:
        os.killpg(pgid, signum)
    except ProcessLookupError:
        pass


def stop_and_reap(pid):
    try:
        signal_group(pid, signal.SIGTERM)
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline and group_has_workers(pid):
            time.sleep(0.05)
        # Signal even if the last snapshot was empty: an exiting parent can
        # fork a helper while ps is walking the process table.
        signal_group(pid, signal.SIGKILL)
        while group_has_workers(pid):
            signal_group(pid, signal.SIGKILL)
            time.sleep(0.05)
    except (OSError, subprocess.SubprocessError) as error:
        # If process listing is unavailable, kill while the group ID is still
        # reserved and require a kernel confirmation after reaping. A group
        # that still exists could contain zombies, so a failed probe cannot
        # establish cleanup. Never signal it again after releasing the PID.
        try:
            signal_group(pid, signal.SIGKILL)
        except PermissionError:
            pass
        _, status = os.waitpid(pid, 0)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.killpg(pid, 0)
            except ProcessLookupError:
                return status
            except PermissionError:
                pass
            time.sleep(0.05)
        raise RuntimeError(f"could not confirm unsigned archive process group stopped: {error}") from error
    return os.waitpid(pid, 0)[1]


def main(command):
    received = 0

    def cancel(signum, _frame):
        nonlocal received
        received = received or signum

    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    if received:
        return 128 + received
    # Group creation and exec happen in one spawn, with the worker still a
    # direct child in the Actions session. Cancellation records intent even
    # during spawn so cleanup always has the resulting PID before it runs.
    pid = os.posix_spawn(command[0], command, os.environ, setpgroup=0, setsigmask=(),
                         setsigdef=(signal.SIGINT, signal.SIGTERM, signal.SIGPIPE))
    try:
        wait_for_worker(pid, lambda: received)
    finally:
        status = stop_and_reap(pid)
    if received:
        return 128 + received
    code = os.waitstatus_to_exitcode(status)
    return 128 - code if code < 0 else code


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (OSError, RuntimeError) as error:
        sys.exit(f"unsigned archive supervisor: {error}")
