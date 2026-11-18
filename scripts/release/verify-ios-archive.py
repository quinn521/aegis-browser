#!/usr/bin/env python3
"""Verify a diagnostic archive; this does not qualify an IPA or TestFlight upload."""
import argparse
import json
from pathlib import Path
import plistlib
import re
import subprocess
from ios_sdk import verify_bundle_sdk
from safari_manifest import verify as verify_safari_manifest


def verify(archive: Path, version: str, build: str, *, require_ios_sdk=False) -> dict:
    if not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", version):
        raise ValueError("Invalid marketing version")
    if not re.fullmatch(r"[1-9]\d{0,9}", build):
        raise ValueError("Invalid build")
    applications = archive / "Products/Applications"
    apps = list(applications.glob("*.app"))
    if len(apps) != 1 or apps[0].name != "Aegis.app":
        raise ValueError("Expected exactly one Aegis.app")
    bundles = [apps[0], *sorted((apps[0] / "PlugIns").glob("*.appex"))]
    expected = {"Aegis.app": "com.gcsa.aegis", "SafariWebExtension.appex": "com.gcsa.aegis.safari",
                "ShareExtension.appex": "com.gcsa.aegis.share"}
    if {p.name for p in bundles} != set(expected):
        raise ValueError("Missing or unexpected app extensions")
    entries = []
    device_sdks = {}
    for bundle in bundles:
        with (bundle / "Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        identity = {key: info.get(key) for key in
                    ["CFBundleIdentifier", "CFBundleShortVersionString", "CFBundleVersion"]}
        if list(identity.values()) != [expected[bundle.name], version, build]:
            raise ValueError(f"Archive identity mismatch: {bundle.name}")
        entries.append(identity)
        if require_ios_sdk:
            device_sdks[expected[bundle.name]] = verify_bundle_sdk(bundle, info)
    safari = apps[0] / "PlugIns/SafariWebExtension.appex"
    verify_safari_manifest(lambda name: (safari / name).read_bytes())
    result = {"status": "unsigned_archive_verified", "applications": entries,
              "signing": "disabled", "IPA": False, "TestFlight": False, "releaseReady": False}
    if require_ios_sdk:
        result["deviceSDKs"] = device_sdks
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("version")
    parser.add_argument("build")
    parser.add_argument("--require-ios-sdk", action="store_true")
    args = parser.parse_args()
    try:
        result = verify(args.archive, args.version, args.build, require_ios_sdk=args.require_ios_sdk)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")
    result["source"] = {key: subprocess.check_output(["git", "rev-parse", ref], text=True).strip()
                        for key, ref in [("sha", "HEAD"), ("tree", "HEAD^{tree}")]}
    print(json.dumps(result, indent=2))
