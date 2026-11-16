import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

api = sys.argv[1]
build = Path("source/apps/browser/access-cmake/out/android-x64").resolve()
adb = shutil.which("adb")
serial = os.environ["ANDROID_SERIAL"]
def prop(key):
    return subprocess.check_output([adb, "-s", serial, "shell", "getprop", key], text=True, timeout=10).strip()
sdk = prop("ro.build.version.sdk")
abis = prop("ro.product.cpu.abilist").split(",")
print(f"real device {serial=} {sdk=} {abis=}", flush=True)
assert sdk == api and "x86_64" in abis
tests = json.loads(subprocess.check_output(["ctest", "--test-dir", build, "--show-only=json-v1"]))["tests"]
assert len(tests) == 1 and tests[0]["name"] == "aegis_access_contract"
print("Registered command:", tests[0]["command"], flush=True)
assert tests[0]["command"][-2:] == ["x86_64", "26"]
trace = Path(os.environ["RUNNER_TEMP"]) / "adb-trace.jsonl"
trace.touch(exist_ok=False)
env = {**os.environ, "ADB": str(Path(__file__).with_name("trace-adb").resolve()), "REAL_ADB": adb, "ADB_TRACE_FILE": str(trace)}
test = subprocess.run(["ctest", "--test-dir", build, "-V", "--no-tests=error", "--output-junit", "junit.xml"], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=230)
print(test.stdout, flush=True)
commands = [json.loads(line) for line in trace.read_text().splitlines()]
print(f"CTest exit={test.returncode}; adb calls={commands}", flush=True)
if api == "25":
    assert test.returncode != 0 and "RuntimeError: Android test requires x86_64 and API 26 or newer" in test.stdout
    assert commands == [["get-serialno"], ["-s", serial, "shell", "getprop", "ro.product.cpu.abilist"], ["-s", serial, "shell", "getprop", "ro.build.version.sdk"]]
    print("PASS: API 25 rejected before device deployment", flush=True)
else:
    assert test.returncode == 0 and "PASS: aegis_access native unit (892 checks)" in test.stdout
    assert any(c[2:3] == ["push"] for c in commands)
