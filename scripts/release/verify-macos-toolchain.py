#!/usr/bin/env python3
"""Compile a small Metal shader before beginning the full Chromium build."""
import json
from pathlib import Path
import subprocess
import tempfile


def command(*args):
    return subprocess.check_output(args, stderr=subprocess.PIPE, timeout=120).decode().strip()


def verify() -> dict:
    developer = command("/usr/bin/xcode-select", "-p")
    sdk = command("/usr/bin/xcrun", "--sdk", "macosx", "--show-sdk-version")
    with tempfile.TemporaryDirectory(prefix="aegis-metal-preflight-") as temporary:
        source = Path(temporary) / "probe.metal"
        output = source.with_suffix(".air")
        source.write_text("#include <metal_stdlib>\nusing namespace metal;\n"
                          "kernel void aegis_probe(device uint *value [[buffer(0)]], "
                          "uint i [[thread_position_in_grid]]) { value[i] = i; }\n")
        try:
            # xcrun --find metal alone accepts Xcode's stub even when the
            # separately downloaded Metal Toolchain is absent.
            command("/usr/bin/xcrun", "--sdk", "macosx", "metal", "-c", str(source),
                    "-o", str(output), "--std=macos-metal2.1", "-mmacosx-version-min=10.14")
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            raise ValueError("Metal shader compilation failed; install the selected Xcode's official component "
                             "with xcodebuild -downloadComponent MetalToolchain and rerun this check") from None
        if not output.is_file() or output.stat().st_size == 0:
            raise ValueError("Metal compiler produced no AIR object; full build was not started")
        size = output.stat().st_size
    return {"status": "macos_toolchain_verified", "developerDirectory": developer, "sdkVersion": sdk,
            "metalShaderCompiled": True, "airBytes": size, "productBuilt": False, "releaseReady": False}


if __name__ == "__main__":
    try:
        print(json.dumps(verify(), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
