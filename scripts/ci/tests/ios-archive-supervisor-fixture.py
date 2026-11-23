"""Run the archive supervisor against synthetic host-policy inputs."""

import ctypes
import importlib.util
import json
import os
import plistlib
import signal
import subprocess
import sys
from unittest.mock import patch


def proxy_settings(settings):
    if settings is None:
        return None
    core = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    ref = ctypes.c_void_p
    core.CFDataCreate.argtypes = (ref, ctypes.c_char_p, ctypes.c_long)
    core.CFDataCreate.restype = ref
    core.CFPropertyListCreateWithData.argtypes = (ref, ref, ctypes.c_ulong, ref, ref)
    core.CFPropertyListCreateWithData.restype = ref
    core.CFRelease.argtypes = (ref,)
    core.CFRelease.restype = None
    payload = plistlib.dumps(settings)
    data = core.CFDataCreate(None, payload, len(payload))
    try:
        result = core.CFPropertyListCreateWithData(None, data, 0, None, None)
        assert result
        return result
    finally:
        core.CFRelease(data)


spec = importlib.util.spec_from_file_location("supervisor", sys.argv[1])
supervisor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supervisor)
run = subprocess.run
stop_and_reap = supervisor.stop_and_reap
developer = os.environ.get("ARCHIVE_TEST_DEVELOPER")
cancellation = os.environ.get("ARCHIVE_TEST_CANCELLATION")


def run_probe(command, **options):
    if command[-1] == "--check-system-proxy":
        scenario = os.environ.get("ARCHIVE_TEST_SYSTEM_POLICY", "{}")
        if scenario == "timeout":
            raise subprocess.TimeoutExpired(command, options["timeout"])
        if scenario == "unavailable":
            raise OSError("synthetic proxy policy unavailable")
        with patch.object(supervisor, "copy_macos_proxy_settings", lambda: proxy_settings(json.loads(scenario))):
            assert options["stdout"] == options["stderr"] == subprocess.DEVNULL
            return subprocess.CompletedProcess(command, int(supervisor.macos_proxy_policy_present()))
    if command == ["/usr/bin/xcode-select", "--print-path"] and developer:
        return subprocess.CompletedProcess(command, 0, stdout=os.fsencode(developer) + b"\n")
    if developer and command[0] == os.path.join(developer, "usr/bin/git"):
        env = options["env"]
        options["env"] = {**env, "GIT_CONFIG_NOSYSTEM": "1"}
        # The real supervisor bypasses the Apple Git launcher. Give the
        # fixture launcher its default cache location for explicit file reads.
        options["env"].pop("TMPDIR", None)
        if "--list" in command:
            # Model the Apple Git origin and scope records with keys parsed
            # by real Git from files owned by this synthetic fixture.
            bundled = os.path.join(developer, "usr/share/git-core/gitconfig")
            fixture_configs = [
                (b"system" if "GIT_CONFIG_SYSTEM" in env else b"unknown", env.get("GIT_CONFIG_SYSTEM", bundled)),
                (b"global", env["GIT_CONFIG_GLOBAL"]),
            ]
            records = bytearray()
            for scope, path in fixture_configs:
                if os.path.isfile(path):
                    args = ["/usr/bin/git", "config", "--file", path, "--null", "--name-only", "--list", "--no-includes"]
                    result = run(args, **options)
                    if result.returncode:
                        return result
                    for key in result.stdout.split(b"\0")[:-1]:
                        records += b"\0".join((scope, b"file:" + os.fsencode(path), key, b""))
            return subprocess.CompletedProcess(command, 0, stdout=bytes(records))
        command = ["/usr/bin/git", *command[1:]]
    result = run(command, **options)
    if cancellation == "config-probe" and command[1:2] == ["config"] and "--list" in command:
        os.kill(os.getpid(), signal.SIGTERM)
    return result


def reap_then_cancel(pid, signal_worker):
    status = stop_and_reap(pid, signal_worker)
    os.kill(os.getpid(), signal.SIGINT)
    return status


with patch.object(subprocess, "run", run_probe):
    if os.environ.get("ARCHIVE_TEST_LEAF_COMPILER") == "1":
        # Only for synthetic leaf compilers and verifier commands. The
        # descendant and process-group tests use the native process census.
        supervisor.process_states = lambda *_args: []
    if cancellation == "after-waitpid":
        supervisor.stop_and_reap = reap_then_cancel
    try:
        sys.exit(supervisor.main(*sys.argv[2:]))
    except (OSError, subprocess.SubprocessError) as error:
        supervisor.report(f"unsigned archive supervisor: {error}")
        sys.exit(1)
