#!/usr/bin/env python3
"""Retain the compiler log and supervise unsigned archive stages in private sessions."""

import ctypes
import errno
import os
import select
import shutil
import signal
import subprocess
import sys
import time


POLL_INTERVAL = 0.05
READ_SIZE = 64 * 1024
TERM_GRACE = 0.5
CENSUS_TIMEOUT = 2
GIT_CONFIG_TIMEOUT = 2
TRANSPORT_OVERRIDES = {
    "GIT_PROXY_COMMAND", "GIT_CONFIG", "GIT_CONFIG_COUNT", "GIT_CONFIG_PARAMETERS",
    "GIT_ASKPASS", "SSH_ASKPASS", "GIT_SSH", "GIT_SSH_COMMAND",
    "GIT_EXEC_PATH", "GIT_TEMPLATE_DIR", "NETRC", "TOOLCHAINS",
}


def archive_environment():
    environment = {name: value for name, value in os.environ.items()
                   if not name.startswith("GIT_TRACE") and name != "GIT_CURL_VERBOSE"}
    # Override Trace2 destinations from system/global Git configuration as well.
    environment.update(GIT_TRACE2="0", GIT_TRACE2_EVENT="0", GIT_TRACE2_PERF="0")
    return environment


def needs_private_diagnostics(environment):
    if any(name.lower().endswith("_proxy") or name in TRANSPORT_OVERRIDES
           or name.startswith(("GIT_CONFIG_KEY_", "GIT_CONFIG_VALUE_", "GIT_HTTP_", "GIT_SSL_"))
           for name in environment):
        return True
    # Relative locations can resolve differently inside package checkouts.
    for name in ("GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM", "HOME", "XDG_CONFIG_HOME"):
        if environment.get(name) and not os.path.isabs(environment[name]):
            return True
    # Xcode selects system Git. If PATH could select a different Git for a
    # helper, its system config may differ from the one probed here.
    if shutil.which("git", path=environment.get("PATH", os.defpath)) != "/usr/bin/git":
        return True
    try:
        git = "/usr/bin/git"
        if sys.platform == "darwin":
            # /usr/bin/git is an Xcode launcher. Probe the selected system Git
            # directly so a cold launcher cache cannot delay archive startup.
            developer = subprocess.check_output(  # nosec B603 - fixed Xcode selection query.
                ["/usr/bin/xcode-select", "--print-path"], env=environment,
                stderr=subprocess.DEVNULL, timeout=GIT_CONFIG_TIMEOUT)
            directory = os.fsdecode(developer.removesuffix(b"\n"))
            if not os.path.isabs(directory) or "\n" in directory or "\0" in directory:
                return True
            git = os.path.join(directory, "usr/bin/git")
        config = subprocess.run(  # nosec B603 - fixed, offline Git metadata query.
            [git, "config", "--null", "--name-only", "--list", "--no-includes"],
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=GIT_CONFIG_TIMEOUT)
    except (OSError, subprocess.SubprocessError):
        return True
    if config.returncode or b"\n" in config.stdout or (config.stdout and not config.stdout.endswith(b"\0")):
        return True
    # Includes can become active only in a package checkout. Config key
    # subsections can themselves contain credentials, so never report the keys.
    return any(key.startswith((b"http.", b"credential.", b"url.", b"remote.", b"include.", b"includeif."))
               or key in (b"core.gitproxy", b"core.askpass", b"core.sshcommand", b"init.templatedir")
               for key in config.stdout.lower().split(b"\0"))


def discard_output(_data):
    # The compiler can relay Git output in arbitrary formats. Keep draining
    # the stage pipe without sending those bytes to either diagnostics sink.
    pass


def process_states(timeout=CENSUS_TIMEOUT):
    rows = subprocess.check_output(  # nosec B603 - fixed ps invocation without command lines.
        ["/bin/ps", "-ax", "-o", "pid=", "-o", "stat="], text=True, timeout=timeout)
    return [(int(pid), state) for pid, state in map(str.split, rows.splitlines())]


def in_session(pid, session):
    try:
        return os.getsid(pid) == session
    except ProcessLookupError:
        return False


def session_workers(session, timeout):
    # Reparented zombies may be visible until their new parent reaps them.
    return [pid for pid, state in process_states(timeout)
            if not state.startswith("Z") and in_session(pid, session)]


def darwin_worker_signaler():
    word = ctypes.c_uint32
    audit_token = word * 8
    library = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
    own_task = word.in_dll(library, "mach_task_self_")
    task_name = library.task_name_for_pid
    task_name.argtypes = (word, ctypes.c_int, ctypes.POINTER(word))
    task_info = library.task_info
    task_info.argtypes = (word, ctypes.c_int, ctypes.POINTER(word), ctypes.POINTER(word))
    deallocate = library.mach_port_deallocate
    deallocate.argtypes = (word, word)
    send_signal = library.proc_signal_with_audittoken
    send_signal.argtypes = (ctypes.POINTER(audit_token), ctypes.c_int)

    def signal_worker(pid, session, signum):
        port = word()
        result = task_name(own_task, pid, ctypes.byref(port))
        if result:
            raise OSError(errno.EIO, f"task_name_for_pid({pid}) returned Mach error {result}")
        token = audit_token()
        count = word(len(token))
        try:
            result = task_info(port, 15, token, ctypes.byref(count))  # TASK_AUDIT_TOKEN
            if result:
                raise OSError(errno.EIO, f"task_info({pid}) returned Mach error {result}")
        finally:
            result = deallocate(own_task, port)
            if result:
                raise OSError(errno.EIO, f"mach_port_deallocate({pid}) returned Mach error {result}")
        if count.value != len(token) or token[5] != pid:  # audit_token_to_pid()
            raise OSError(errno.EIO, f"invalid audit token for helper {pid}")
        if in_session(pid, session):
            ctypes.set_errno(0)
            if send_signal(ctypes.byref(token), signum):
                error = ctypes.get_errno() or errno.EIO
                raise OSError(error, f"could not signal helper {pid}: {os.strerror(error)}")

    return signal_worker


def linux_signal_worker(pid, session, signum):
    descriptor = os.pidfd_open(pid)
    try:
        if in_session(pid, session):
            signal.pidfd_send_signal(descriptor, signum)
    finally:
        os.close(descriptor)


def worker_signaler():
    try:
        if sys.platform == "darwin":
            return darwin_worker_signaler()
        if sys.platform == "linux" and hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
            return linux_signal_worker
    except (AttributeError, ValueError) as error:
        raise OSError(errno.ENOSYS, "lifetime-bound helper signalling is unavailable") from error
    raise OSError(errno.ENOSYS, "lifetime-bound helper signalling is unavailable")


def signal_separate_workers(workers, session, signum, signal_worker):
    problem = None
    for pid in workers:
        try:
            # Check membership before acquiring a handle and again after.
            if pid != session and os.getpgid(pid) != session and in_session(pid, session):
                signal_worker(pid, session, signum)
        except ProcessLookupError:
            pass
        except OSError as error:
            problem = error
    return problem


def signal_primary_group(pid, signum):
    try:
        os.killpg(pid, signum)
    except ProcessLookupError:
        pass
    except PermissionError as error:
        # Darwin can deny signalling a reserved group containing only a zombie.
        return error
    return None


def stop_and_reap(pid, signal_worker):
    # Keep the leader unreaped to reserve both the primary PGID and session ID.
    primary_problem = signal_primary_group(pid, signal.SIGTERM)
    escalate_at = time.monotonic() + TERM_GRACE
    confirm_by = escalate_at + CENSUS_TIMEOUT
    sent_helper_term = False
    empty = False
    problem = None
    while True:
        now = time.monotonic()
        force = now >= escalate_at
        if force:
            primary_problem = signal_primary_group(pid, signal.SIGKILL) or primary_problem
        if now >= confirm_by:
            os.waitpid(pid, os.WNOHANG)
            failure = problem or primary_problem
            detail = f": {failure}" if failure else ""
            raise OSError(errno.EBUSY, f"could not confirm unsigned archive session {pid} stopped{detail}")

        boundary = confirm_by if force else escalate_at
        try:
            # A slow ps must leave time for the primary group's escalation.
            workers = session_workers(pid, boundary - now)
        except (OSError, subprocess.SubprocessError) as error:
            problem = error
            empty = False
        else:
            if not workers and empty:
                reaped, status = os.waitpid(pid, os.WNOHANG)
                if reaped:
                    return status
            # Rescan after an empty snapshot in case an exiting worker forked
            # while ps was collecting the process table.
            empty = not workers
            if force or (workers and not sent_helper_term and time.monotonic() < escalate_at):
                error = signal_separate_workers(workers, pid, signal.SIGKILL if force else signal.SIGTERM, signal_worker)
                problem = error or problem
                sent_helper_term = True
        time.sleep(min(POLL_INTERVAL, max(0, boundary - time.monotonic())))


def write_all(descriptor, data):
    pending = memoryview(data)
    while pending:
        size = os.write(descriptor, pending)
        if size == 0:
            raise OSError(errno.EIO, "empty write")
        pending = pending[size:]


def write_console(data, stream=sys.stdout):
    # A stopped Actions reader must neither backpressure the child nor prevent
    # session cleanup. Drop console bytes that cannot be written immediately.
    if stream is None:
        return
    try:
        descriptor = stream.fileno()
        blocking = os.get_blocking(descriptor)
        if blocking:
            os.set_blocking(descriptor, False)
        try:
            os.write(descriptor, data)
        finally:
            if blocking:
                os.set_blocking(descriptor, True)
    except (OSError, ValueError):
        pass


def report(message):
    write_console(f"{message}\n".encode(errors="backslashreplace"), sys.stderr)


class ArchiveOutput:
    def __init__(self, path):
        self.log = None
        self.failed = False
        try:
            self.log = open(path, "wb", buffering=0)
        except OSError as error:
            self.log_error(error)

    def log_error(self, error):
        self.failed = True
        report(f"failed to retain xcodebuild.log: {error}")

    def write(self, data):
        if self.log is not None:
            try:
                write_all(self.log.fileno(), data)
            except OSError as error:
                self.log_error(error)
                self.close()
        write_console(data)

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
        output(data)
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


def run_stage(command, environment, output, cancelled, signal_worker):
    # Verify process enumeration is available before starting anything that
    # might leave a helper in a second process group.
    if cancelled():
        return 0
    process_states()
    reader, writer = os.pipe()
    try:
        try:
            os.set_blocking(reader, False)
            if cancelled():
                return 0
            # Create the session atomically with spawn. The supervisor stays
            # the direct parent and passes the Actions tracking env intact.
            pid = os.posix_spawn(
                command[0], command, environment, setsid=True, setsigmask=(),
                setsigdef=(signal.SIGINT, signal.SIGTERM, signal.SIGPIPE),
                file_actions=[(os.POSIX_SPAWN_DUP2, writer, fd) for fd in (1, 2)])
        finally:
            os.close(writer)
        try:
            stream_until_exit(pid, reader, output, cancelled)
        finally:
            status = stop_and_reap(pid, signal_worker)
        # Helpers can hold the pipe open after the direct child exits. Drain
        # buffered diagnostics only after those helpers have stopped.
        while copy_chunk(reader, output):
            pass
    finally:
        os.close(reader)
    code = os.waitstatus_to_exitcode(status)
    if code < 0:
        return 128 - code
    return code


def main(log, shell, wrapper, *command):
    received = 0

    def cancel(signum, _frame):
        nonlocal received
        received = received or signum

    signal.signal(signal.SIGINT, cancel)
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)

    def cancelled():
        return received != 0

    signal_worker = worker_signaler()
    environment = archive_environment()
    private_diagnostics = needs_private_diagnostics(environment)
    output = ArchiveOutput(log)
    try:
        if private_diagnostics:
            output.write(b"unsigned archive diagnostics withheld for proxy or unverified Git transport settings\n")
        code = run_stage(command, environment, discard_output if private_diagnostics else output.write,
                         cancelled, signal_worker)
        if private_diagnostics:
            status = 128 + received if received else code
            output.write(f"unsigned archive compiler exited with status {status}\n".encode("ascii"))
    finally:
        output.close()
    if received:
        return 128 + received
    if code or output.failed:
        return code or os.EX_IOERR
    code = run_stage([shell, wrapper, "--verify-archive"], environment,
                     discard_output if private_diagnostics else write_console, cancelled, signal_worker)
    if private_diagnostics and code:
        report(f"unsigned archive verification exited with status {code}")
    return 128 + received if received else code


if __name__ == "__main__":
    try:
        sys.exit(main(*sys.argv[1:]))
    except (OSError, subprocess.SubprocessError) as error:
        report(f"unsigned archive supervisor: {error}")
        sys.exit(1)
