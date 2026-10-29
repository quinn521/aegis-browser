#!/usr/bin/env python3
"""Check real private-key access before beginning a long native release build."""
import json
from pathlib import Path
import subprocess
import tempfile

from macos_signing import command, load_policy, verify_signature


def verify():
    policy = load_policy()
    with tempfile.TemporaryDirectory(prefix="aegis-signing-preflight-") as temporary:
        source = Path(temporary) / "probe.c"
        binary = source.with_suffix("")
        source.write_text("int main(void) { return 0; }\n")
        command("/usr/bin/xcrun", "clang", "-arch", "arm64", str(source), "-o", str(binary))
        try:
            command("/usr/bin/codesign", "--force", "--sign", policy.get("certificateSha1", "-"),
                    "--timestamp=none", str(binary))
            result = verify_signature(binary, policy)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError):
            raise ValueError("Native signing probe failed; make the source-configured identity/keychain "
                             "available before starting the release build") from None
    return {"status": "macos_signing_verified", **result, "privateKeyExported": False,
            "productBuilt": False, "releaseReady": False}


if __name__ == "__main__":
    try:
        print(json.dumps(verify(), indent=2))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
