#!/usr/bin/env python3
"""Build the pinned macOS Access owned core and write its DEV runtime manifest."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent


def verify_digest(path, expected):
    with path.open("rb") as source:
        actual = hashlib.file_digest(source, "sha256").hexdigest()
    require(actual == expected, f"{path.name} SHA-256 mismatch ({actual})")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_command(argv, **kwargs):
    return subprocess.check_output(argv, text=True, **kwargs).strip()


def prepare(output, local_source):
    lock = json.loads((ROOT / "owned-core-lock.json").read_text())
    require(lock["schemaVersion"] == 1, "Unsupported owned-core lock version")
    source = lock["source"]
    for patch in lock["patches"]:
        verify_digest(ROOT / patch["path"], patch["sha256"])

    go_version = "go" + lock["toolchain"]["go"]
    build = lock["build"]
    go_env = dict(os.environ, GOTOOLCHAIN=go_version, GOENV="off", GOFLAGS="",
                  GOEXPERIMENT="", GOWORK="off", GOOS=build["goos"], GOARCH=build["goarch"],
                  GOARM64="v8.0", GOFIPS140=build["gofips140"], CGO_ENABLED="0")
    go_env.pop("GOROOT", None)
    version = read_command(["go", "env", "GOVERSION"], env=go_env)
    require(version == go_version, f"Expected {go_version}, got {version}")

    output.mkdir(parents=True)
    binary = output / "xray"
    with tempfile.TemporaryDirectory(prefix="xray-build-", dir=output) as work:
        repo = Path(work) / "source"
        upstream = str(local_source) if local_source is not None else source["repository"]
        subprocess.run(["git", "clone", "--no-local", "--depth", "1", "--branch",
                        source["tag"], "--no-checkout", "--", upstream, repo], check=True)
        ref = "refs/tags/" + source["tag"]
        commit, tree = read_command(
            ["git", "rev-parse", ref + "^{commit}", ref + "^{tree}"], cwd=repo
        ).splitlines()
        require(commit == source["commit"], f"Xray tag commit mismatch ({commit})")
        require(tree == source["tree"], f"Xray source tree mismatch ({tree})")
        subprocess.run(["git", "-c", "advice.detachedHead=false", "checkout", "--detach",
                        commit], cwd=repo, check=True)
        for patch in lock["patches"]:
            subprocess.run(["git", "apply", "--index", "--whitespace=error",
                            ROOT / patch["path"]], cwd=repo, check=True)
        tree = read_command(["git", "write-tree"], cwd=repo)
        require(tree == lock["patchedTree"], f"Patched Xray tree mismatch ({tree})")
        for path, digest in lock["goModules"].items():
            verify_digest(repo / path, digest)
        subprocess.run(["go", "mod", "verify"], cwd=repo, env=go_env, check=True)
        built = Path(work) / "xray"
        subprocess.run(["go", "build", "-mod=readonly", "-tags=" + build["tags"],
                        "-trimpath", "-buildvcs=false", "-ldflags=" + build["ldflags"],
                        "-o", built, "./main"], cwd=repo, env=go_env, check=True)
        verify_digest(built, lock["artifact"]["sha256"])
        require(built.stat().st_size == lock["artifact"]["bytes"], "Xray size mismatch")
        built.rename(binary)

    manifest = {
        "schemaVersion": 2,
        "binaryPath": str(binary),
        "binarySha256": lock["artifact"]["sha256"],
        "exclusiveLoopbackTcp": True,
        "parentExitWatch": True,
        "parentHeldListenerLease": True,
        "systemAllocatedTcpPort": True,
        "localHttpNoAuth": True,
        "localSocks5NoAuth": True,
    }
    manifest_path = output / "core-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(manifest_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="New directory for the verified core")
    parser.add_argument("--source", type=Path, help="Use a local clone of Xray-core")
    args = parser.parse_args()
    try:
        prepare(args.output.resolve(), args.source.resolve() if args.source else None)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f"Preparing macOS Access owned core failed: {error}")


if __name__ == "__main__":
    main()
