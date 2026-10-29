#!/usr/bin/env python3
"""Inspect the actual read-only DMG and its signed app before recording a package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile
import time

from macos_signing import load_policy, verify_signature


def command(*args):
    return subprocess.check_output(args, stderr=subprocess.PIPE)


def detach(device):
    for attempt in range(3):
        try:
            command("/usr/bin/hdiutil", "detach", str(device))
            return
        except subprocess.CalledProcessError as error:
            if error.returncode != 16:
                raise
            if attempt < 2:
                time.sleep(1)
    # Only the device returned by this invocation's private attach is eligible.
    # Detach the complete image, including a container left after a busy eject.
    command("/usr/bin/hdiutil", "detach", str(device), "-force")


def verify_app(volume: Path, version: str, policy=None) -> dict:
    policy = policy or load_policy()
    apps = list(volume.glob("*.app"))
    if len(apps) != 1 or apps[0].name != "GCSA-aegis.app" or apps[0].is_symlink():
        raise ValueError("DMG must contain exactly one real GCSA-aegis.app")
    app = apps[0]
    info_path = app / "Contents/Info.plist"
    if info_path.is_symlink():
        raise ValueError("App metadata must not be a symlink")
    info = plistlib.loads(info_path.read_bytes())
    major, minor, _patch, build = map(int, version.split("."))
    expected = {"CFBundleIdentifier": "com.gcsa.aegis", "CFBundleExecutable": "GCSA Aegis",
                "AegisProductVersion": version, "AegisProductVersionLabel": f"Ver {major}.{minor} ({build:03d})"}
    if any(info.get(key) != value for key, value in expected.items()):
        raise ValueError("DMG application identity differs from the requested product version")
    binary = app / "Contents/MacOS/GCSA Aegis"
    if binary.is_symlink() or not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("DMG application executable is missing or unsafe")
    if command("/usr/bin/lipo", "-archs", str(binary)).decode().strip() != "arm64":
        raise ValueError("DMG application must contain the release arm64 executable")
    signature = verify_signature(app, policy, execute=command)
    assessment = {}
    if policy.get('temporary'):
        result = subprocess.run(['/usr/sbin/spctl', '--assess', '--type', 'execute', '--verbose=4', str(app)],
                                capture_output=True, timeout=120)
        assessment = {'gatekeeperAssessment': {'exitCode': result.returncode,
                      'accepted': result.returncode == 0, 'output': result.stderr.decode(errors='replace')},
                      'notarized': False, 'formalDistributionQualified': False}
    applications = volume / "Applications"
    if not applications.is_symlink() or os.readlink(applications) != "/Applications":
        raise ValueError("DMG Applications link is missing or unexpected")
    return {**expected, **signature, **assessment, "architecture": "arm64", "codeSignatureVerified": True}


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def verify(dmg: Path, version: str, policy=None) -> dict:
    policy = policy or load_policy()
    if not re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.[1-9]\d*", version):
        raise ValueError("Expected the four-part native product version")
    if dmg.suffix != ".dmg" or dmg.is_symlink() or not dmg.is_file() or dmg.stat().st_size == 0:
        raise ValueError("Expected a real nonempty DMG file")
    digest = sha256(dmg)
    command("/usr/bin/hdiutil", "verify", str(dmg))
    with tempfile.TemporaryDirectory(prefix="aegis-dmg-verify-") as temporary:
        volume = Path(temporary).resolve() / "volume"
        volume.mkdir()
        attached = False
        device = volume
        try:
            output = command("/usr/bin/hdiutil", "attach", str(dmg.resolve()), "-readonly", "-nobrowse",
                             "-noautoopen", "-plist", "-mountpoint", str(volume))
            attached = True
            entities = plistlib.loads(output).get("system-entities", [])
            if entities and re.fullmatch(r"/dev/disk\d+", entities[0].get("dev-entry", "")):
                device = entities[0]["dev-entry"]
            mounts = [item.get("mount-point") for item in entities if item.get("mount-point")]
            if mounts != [str(volume)]:
                raise ValueError("DMG attached outside the private verification mount")
            application = verify_app(volume, version, policy)
        finally:
            if attached:
                detach(device)
    if sha256(dmg) != digest:
        raise ValueError("DMG changed during verification")
    return {"status": "macos_dmg_verified", "sha256": digest, "application": application,
            "signing": application["signing"], "runtimeAcceptance": False, "releaseReady": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dmg", type=Path)
    parser.add_argument("version")
    parser.add_argument('--signing-mode', choices=['native', 'temporary-ad-hoc'], default='native')
    args = parser.parse_args()
    print(json.dumps(verify(args.dmg, args.version, load_policy(mode=args.signing_mode)), indent=2))
