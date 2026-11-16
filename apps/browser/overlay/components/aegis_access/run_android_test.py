"""Run the independent Access contract on the adb-selected Android device."""

import os
from pathlib import Path
import re
import subprocess
import sys


def adb(*args, **kwargs):
    return subprocess.run(
        [os.environ.get("ADB", "adb"), *args],
        check=True,
        timeout=25,
        **kwargs,
    )


def main(binary):
    print("Android test device:", flush=True)
    serial = adb("get-serialno", capture_output=True, text=True).stdout.strip()
    if not serial or serial == "unknown":
        raise RuntimeError("No Android test device selected")
    print(serial, flush=True)

    def on_device(*args, **kwargs):
        return adb("-s", serial, *args, **kwargs)

    on_device("shell", "getprop", "ro.product.cpu.abilist")
    on_device("shell", "getprop", "ro.build.version.sdk")
    directory = on_device("shell", "mktemp", "-d", "/data/local/tmp/aegis-access.XXXXXXXX",
                    capture_output=True, text=True).stdout.strip()
    if not re.fullmatch(r"/data/local/tmp/aegis-access\.[a-zA-Z0-9]+", directory):
        raise RuntimeError(f"Unexpected test directory: {directory!r}")
    destination = f"{directory}/contract"
    try:
        on_device("push", str(binary), destination)
        on_device("shell", "chmod", "700", destination)
        on_device("shell", destination)
    finally:
        on_device("shell", "rm", "-rf", directory)


if __name__ == "__main__":
    main(Path(sys.argv[1]))
