"""Verify actual EmbeddedCore build inputs without changing the reviewed source lock."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import shutil


SLICE_KEYS = {"ios-arm64": "iosArm64Sha256", "ios-arm64-simulator": "iosArm64SimulatorSha256"}


def file_digest(path):
    with Path(path).open("rb") as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        return digest.hexdigest()


def directory_digest(root):
    root = Path(root)
    files = sorted(path for path in root.rglob("*") if path.is_file() and not path.name.startswith("."))
    if not files:
        raise ValueError(f"empty build input: {root.name}")
    digest = hashlib.sha256()
    for path in files:
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def slice_digests(framework):
    framework = Path(framework)
    simulator = [path for path in framework.iterdir()
                 if path.is_dir() and "simulator" in path.name and "arm64" in path.name]
    if len(simulator) != 1:
        raise ValueError("expected one arm64 simulator slice")
    return {"iosArm64Sha256": directory_digest(framework / "ios-arm64"),
            "iosArm64SimulatorSha256": directory_digest(simulator[0])}


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def input_list(root, source_lock, products, framework):
    # Xcode's script sandbox grants literal directory inputs, not recursive reads.
    # Enumerate the exact files/directories the in-build verification will read.
    inputs = [root / "scripts/release/ios_embedded_core.py", source_lock,
              products / "embedded-core-lock.json", products / "build-manifest.json"]
    for directory in (root / "apps/ios/EmbeddedCore/bridge", framework):
        inputs.extend([directory, *sorted(directory.rglob("*"))])
    return "".join(str(path) + "\n" for path in inputs)


def tool_field(field, command, pattern, *, cwd=None):
    value = subprocess.check_output(command, cwd=cwd, text=True).removesuffix("\n")
    if not re.fullmatch(pattern, value, flags=re.ASCII):
        raise ValueError(f"unexpected EmbeddedCore {field} output")
    return value


def check_mobile_tool(go, executable, name, toolchain, bridge, *, env=None):
    metadata = json.loads(subprocess.check_output([go, "version", "-m", "-json", executable], env=env))
    module = metadata["Main"]
    path, version = toolchain["gomobile"].split("@", 1)
    sums = [fields[2] for line in (bridge / "go.sum").read_text().splitlines()
            if len(fields := line.split()) == 3 and fields[:2] == [path, version]]
    if (path != "golang.org/x/mobile" or metadata.get("Path") != f"{path}/cmd/{name}"
            or metadata.get("GoVersion") != f"go{toolchain['go']}"
            or module.get("Path") != path or module.get("Version") != version
            or module.get("Replace") is not None or sums != [module.get("Sum")]
            or not re.fullmatch(r"h1:[A-Za-z0-9+/]{43}=", sums[0])):
        raise ValueError(f"actual {name} toolchain or module differs from the source pin")


def apple_metadata():
    version_pattern = r"[0-9]{1,3}(?:\.[0-9]{1,3}){0,2}"
    return {
        "xcodeVersion": tool_field("Xcode version", ["xcodebuild", "-version"],
                                   rf"Xcode {version_pattern}\nBuild version [0-9]{{1,3}}[A-Z][0-9]{{1,6}}[a-z]?"),
        "sdkVersions": {sdk: tool_field(sdk, ["xcrun", "--sdk", sdk, "--show-sdk-version"], version_pattern)
                        for sdk in ("iphoneos", "iphonesimulator")},
        "sdkBuildVersions": {sdk: tool_field(sdk, ["xcrun", "--sdk", sdk, "--show-sdk-build-version"],
                                            r"[0-9]{1,3}[A-Z][0-9]{1,6}[a-z]?")
                             for sdk in ("iphoneos", "iphonesimulator")},
    }


def verify_apple_pin(metadata, toolchain):
    if (metadata.get("xcodeVersion", "").split("\n")[-1] != "Build version " + toolchain["xcode"]
            or any(key in toolchain and metadata.get(key) != toolchain[key]
                   for key in ("sdkVersions", "sdkBuildVersions"))):
        raise ValueError("actual Apple toolchain differs from the source pin")


def tool_metadata(root, toolchain, bridge):
    for name in ("gomobile", "gobind"):
        executable = shutil.which(name)
        if not executable:
            raise ValueError(f"{name} build tool is missing")
        check_mobile_tool("go", executable, name, toolchain, bridge)
    return {
        "sourceCommit": tool_field("source commit", ["git", "rev-parse", "HEAD"], r"[0-9a-f]{40}", cwd=root),
        "goVersion": tool_field("Go version", ["go", "version"],
                                rf"go version go{re.escape(toolchain['go'])} darwin/(?:arm64|amd64)"),
        "gomobileModule": toolchain["gomobile"],
        "gobindModule": toolchain["gomobile"],
        **apple_metadata(),
    }


def record(root, source_lock, products, mode, xcode_root=None, xcode_lock=None):
    root, source_lock, products = Path(root).resolve(), Path(source_lock).resolve(), Path(products).resolve()
    lock = json.loads(source_lock.read_text())
    if lock.get("status") != "verified_build":
        raise ValueError("source lock must be verified_build")
    framework = products / "AegisEmbeddedCore.xcframework"
    actual = slice_digests(framework)
    if mode == "strict" and any(lock["artifacts"].get(key) != value for key, value in actual.items()):
        raise ValueError("actual EmbeddedCore slices differ from frozen digests")
    compiled_bridge = directory_digest(products / "work/bridge")
    if (products / "compiled-bridge.sha256").read_text().strip() != compiled_bridge:
        raise ValueError("EmbeddedCore bridge copy differs from the gomobile build input")
    metadata = tool_metadata(root, lock["toolchain"], products / "work/bridge")
    if mode == "strict":
        verify_apple_pin(metadata, lock["toolchain"])
    effective = {**lock, "artifacts": {**lock["artifacts"], **actual}}
    write_json(products / "embedded-core-lock.json", effective)
    (products / "build-inputs.xcfilelist").write_text(input_list(
        Path(xcode_root or root).resolve(), Path(xcode_lock or source_lock).resolve(), products, framework))
    manifest = {
        "schemaVersion": 1,
        "kind": "ios-embedded-core-build",
        "digestMode": mode,
        "sourceLockSha256": file_digest(source_lock),
        "bridgeSha256": compiled_bridge,
        "compiledBridgeSha256": compiled_bridge,
        "effectiveLockSha256": file_digest(products / "embedded-core-lock.json"),
        "frameworkSha256": directory_digest(framework),
        "slices": actual,
        "inputFileListSha256": file_digest(products / "build-inputs.xcfilelist"),
        **metadata,
    }
    write_json(products / "build-manifest.json", manifest)
    return manifest


def verify(root, source_lock, products, mode=None):
    root, source_lock, products = Path(root).resolve(), Path(source_lock).resolve(), Path(products).resolve()
    manifest = json.loads((products / "build-manifest.json").read_text())
    if manifest.get("schemaVersion") != 1 or manifest.get("kind") != "ios-embedded-core-build":
        raise ValueError("unsupported EmbeddedCore build manifest")
    file_list = products / "build-inputs.xcfilelist"
    if file_list.read_text() != input_list(root, source_lock, products, products / "AegisEmbeddedCore.xcframework"):
        raise ValueError("Xcode input list points to different build inputs")
    bridge = directory_digest(root / "apps/ios/EmbeddedCore/bridge")
    expected = {
        "sourceLockSha256": file_digest(source_lock),
        "bridgeSha256": bridge,
        "compiledBridgeSha256": bridge,
        "effectiveLockSha256": file_digest(products / "embedded-core-lock.json"),
        "frameworkSha256": directory_digest(products / "AegisEmbeddedCore.xcframework"),
        "slices": slice_digests(products / "AegisEmbeddedCore.xcframework"),
        "inputFileListSha256": file_digest(products / "build-inputs.xcfilelist"),
    }
    if any(manifest.get(key) != value for key, value in expected.items()):
        raise ValueError("actual EmbeddedCore build inputs differ from their manifest")
    source = json.loads(source_lock.read_text())
    toolchain = source["toolchain"]
    if any(manifest.get(key) != toolchain["gomobile"] for key in ("gomobileModule", "gobindModule")):
        raise ValueError("recorded binding tools differ from the source pin")
    if mode == "strict" and any(source["artifacts"].get(k) != v for k, v in expected["slices"].items()):
        raise ValueError("cached slices differ from frozen digests")
    if mode == "strict":
        verify_apple_pin(manifest, toolchain)
    effective = json.loads((products / "embedded-core-lock.json").read_text())
    if source.get("status") != "verified_build" or effective != {
        **source, "artifacts": {**source["artifacts"], **expected["slices"]}
    }:
        raise ValueError("bundled lock differs from the source pins or actual slices")
    return {**manifest, "manifestSha256": file_digest(products / "build-manifest.json")}


def verify_bundle(bundle, products, derived, platform, *, owned_linker_paths=False):
    bundle, products = Path(bundle), Path(products)
    lock = bundle / "Frameworks/AccessKit.framework/embedded-core-lock.json"
    if file_digest(lock) != file_digest(products / "embedded-core-lock.json"):
        raise ValueError("app bundled a different EmbeddedCore lock")
    manifest = bundle / "Frameworks/AccessKit.framework/embedded-core-build.json"
    if file_digest(manifest) != file_digest(products / "build-manifest.json"):
        raise ValueError("app bundled a different EmbeddedCore build manifest")
    from ios_linked_core import verify_link
    return {**verify_link(bundle, products, Path(derived), platform, owned_linker_paths=owned_linker_paths),
            "appBinarySha256": file_digest(bundle / "Aegis"), "bundledLockSha256": file_digest(lock),
            "bundledBuildManifestSha256": file_digest(manifest)}


def verify_prepared_source(manifest, root, commit):
    embedded = Path(root) / "apps/ios/EmbeddedCore"
    bridge = directory_digest(embedded / "bridge")
    if (manifest["sourceCommit"] != commit
            or manifest["sourceLockSha256"] != file_digest(embedded / "embedded-core-lock.json")
            or manifest.get("compiledBridgeSha256") != bridge
            or manifest["bridgeSha256"] != bridge):
        raise ValueError("EmbeddedCore inputs do not match the prepared Alpha source")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("record", "verify", "install"))
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--source-lock", required=True, type=Path)
    parser.add_argument("--products", required=True, type=Path)
    parser.add_argument("--mode", choices=("strict", "ci-rebuild"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--derived", type=Path)
    parser.add_argument("--platform", choices=("device", "simulator"))
    parser.add_argument("--xcode-root", type=Path)
    parser.add_argument("--xcode-lock", type=Path)
    parser.add_argument("--prepared-source", nargs=2, metavar=("ROOT", "COMMIT"))
    args = parser.parse_args()
    if args.prepared_source and args.operation != "install":
        parser.error("--prepared-source requires install")
    result = (record(args.root, args.source_lock, args.products, args.mode or "strict",
                     args.xcode_root, args.xcode_lock) if args.operation == "record"
              else verify(args.root, args.source_lock, args.products, args.mode))
    if args.operation == "install":
        if not args.bundle:
            parser.error("install requires --bundle")
        if args.prepared_source:
            verify_prepared_source(result, *args.prepared_source)
        shutil.copyfile(args.products / "embedded-core-lock.json", args.bundle / "embedded-core-lock.json")
        shutil.copyfile(args.products / "build-manifest.json", args.bundle / "embedded-core-build.json")
    elif args.bundle:
        if not args.derived or not args.platform:
            parser.error("bundle verification requires --derived and --platform")
        result.update(verify_bundle(args.bundle, args.products, args.derived, args.platform))
    if args.output:
        write_json(args.output, result)
    else:
        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
