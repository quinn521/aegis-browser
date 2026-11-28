"""Inject synthetic failures at archive supervisor boundaries."""

import errno
import importlib.util
import os
import signal
import subprocess
import sys
import time


spec = importlib.util.spec_from_file_location("supervisor", sys.argv[1])
supervisor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supervisor)
stop_and_reap = supervisor.stop_and_reap
process_states = supervisor.process_states
reaped_stages = 0
startup_attempts = 0


def cancel_before_spawn():
    os.kill(os.getpid(), signal.SIGTERM)
    return []


def slow_startup_census(*args):
    global startup_attempts
    if not args:
        startup_attempts += 1
        if startup_attempts % supervisor.CENSUS_START_ATTEMPTS:
            time.sleep(supervisor.CENSUS_TIMEOUT)
            raise subprocess.TimeoutExpired("/bin/ps", supervisor.CENSUS_TIMEOUT)
    return process_states(*args)


def reap_then_cancel(pid, signal_worker):
    global reaped_stages
    status = stop_and_reap(pid, signal_worker)
    reaped_stages += 1
    if reaped_stages == 2:  # Compiler, after the successful pre-compile check.
        os.kill(os.getpid(), signal.SIGINT)
    return status


def fail_log_write(_descriptor, _data):
    raise OSError(errno.ENOSPC, "synthetic log write failure", "synthetic-log-error-value")


if os.environ.get("ARCHIVE_TEST_LEAF_COMPILER") == "1":
    # Only for synthetic leaf commands. Tests with descendants use the native census.
    supervisor.process_states = lambda *_args: []
if os.environ.get("ARCHIVE_TEST_SLOW_CENSUS") == "1":
    supervisor.process_states = slow_startup_census
cancellation = os.environ.get("ARCHIVE_TEST_CANCELLATION")
if cancellation == "before-spawn":
    supervisor.process_states = cancel_before_spawn
if cancellation == "after-waitpid":
    supervisor.stop_and_reap = reap_then_cancel
if os.environ.get("ARCHIVE_TEST_LOG_FAILURE") == "write":
    supervisor.write_all = fail_log_write
sys.exit(supervisor.cli(*sys.argv[2:]))
