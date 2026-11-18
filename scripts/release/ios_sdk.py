#!/usr/bin/env python3
"""Check the selected Xcode and the SDK recorded in iOS device executables."""
import json
from pathlib import Path
import re
import subprocess
import sys

XCODE_VERSION = "27.2"
IPHONEOS_SDK_VERSION = "27.2"
MINIMUM_IOS_SDK_MAJOR = 27


def xcrun(*args, environment=None):
    try:
        return subprocess.check_output(
            ["/usr/bin/xcrun", *args], env=environment, text=True, timeout=60
        ).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"Xcode tool failed: xcrun {' '.join(args[:2])}") from error


def verify_toolchain(environment=None):
    version = xcrun("xcodebuild", "-version", environment=environment).splitlines()
    if len(version) != 2 or version[0] != f"Xcode {XCODE_VERSION}" or not version[1].startswith("Build version "):
        raise ValueError(f"Xcode {XCODE_VERSION} is required; found {' / '.join(version) or 'no version'}")
    sdk = xcrun("--sdk", "iphoneos", "--show-sdk-version", environment=environment)
    if sdk != IPHONEOS_SDK_VERSION:
        raise ValueError(f"iPhoneOS SDK {IPHONEOS_SDK_VERSION} is required; found {sdk or 'no version'}")
    return {"xcode": XCODE_VERSION, "xcodeBuild": version[1].removeprefix("Build version "), "iphoneosSdk": sdk}


def device_sdk(output, target):
    commands = re.findall(r"^[ \t]*cmd[ \t]+(\S+)[ \t]*$", output, re.MULTILINE)
    platforms = re.findall(r"^[ \t]*platform[ \t]+(\S+)[ \t]*$", output, re.MULTILINE)
    sdks = re.findall(r"^[ \t]*sdk[ \t]+(\S+)[ \t]*$", output, re.MULTILINE)
    if commands != ["LC_BUILD_VERSION"] or platforms != ["IOS"] or len(sdks) != 1:
        raise ValueError(f"{target}: expected one iOS device LC_BUILD_VERSION")
    sdk = sdks[0]
    if not re.fullmatch(r"[0-9]{1,5}\.[0-9]{1,3}(?:\.[0-9]{1,3})?", sdk) or int(sdk.split(".")[0]) < MINIMUM_IOS_SDK_MAJOR:
        raise ValueError(f"{target}: iOS SDK {MINIMUM_IOS_SDK_MAJOR} or later is required; found {sdk}")
    return sdk


def verify_bundle_sdk(bundle, info):
    name = info.get("CFBundleExecutable")
    if not isinstance(name, str) or Path(name).name != name or name in {"", ".", ".."}:
        raise ValueError(f"{bundle.name}: invalid CFBundleExecutable")
    executable = bundle / name
    if executable.is_symlink() or not executable.is_file():
        raise ValueError(f"{bundle.name}: missing executable")
    architectures = xcrun("lipo", "-archs", str(executable)).split()
    if not architectures or len(set(architectures)) != len(architectures):
        raise ValueError(f"{bundle.name}: invalid executable architecture inventory")
    return {
        arch: device_sdk(xcrun("vtool", "-arch", arch, "-show-build", str(executable)), f"{bundle.name} ({arch})")
        for arch in architectures
    }


if __name__ == "__main__":
    try:
        print(json.dumps(verify_toolchain(), sort_keys=True))
    except ValueError as error:
        sys.exit(str(error))
