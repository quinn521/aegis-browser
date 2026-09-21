#!/usr/bin/env python3
"""Run the Access GTest targets in a fixed, patched Chromium checkout."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from typing import Iterable

ROOT = Path(__file__).resolve().parents[2]
BROWSER = ROOT / "apps" / "browser"
ARGS_FILE = BROWSER / "args" / "aegis.gn"
COMMIT_FILE = BROWSER / "CHROMIUM_COMMIT"
VERSION_FILE = BROWSER / "CHROMIUM_VERSION"
PATCH_DIR = BROWSER / "patches"
V8_PATCH_DIR = PATCH_DIR / "v8"

TARGETS = (
    ("//components/aegis_access:access_identity_generation_state_unittests",
     "components/aegis_access:access_identity_generation_state_unittests",
     "access_identity_generation_state_unittests"),
    ("//components/aegis_access:access_proxy_selection_generation_state_unittests",
     "components/aegis_access:access_proxy_selection_generation_state_unittests",
     "access_proxy_selection_generation_state_unittests"),
    ("//components/aegis_access:access_base_proxy_config_generation_state_unittests",
     "components/aegis_access:access_base_proxy_config_generation_state_unittests",
     "access_base_proxy_config_generation_state_unittests"),
    ("//components/aegis_access:request_generation_tuple_builder_unittests",
     "components/aegis_access:request_generation_tuple_builder_unittests",
     "request_generation_tuple_builder_unittests"),
    ("//components/aegis_access:browser_request_metadata_seed_unittests",
     "components/aegis_access:browser_request_metadata_seed_unittests",
     "browser_request_metadata_seed_unittests"),
    ("//components/aegis_access:request_ownership_registry_unittests",
     "components/aegis_access:request_ownership_registry_unittests",
     "request_ownership_registry_unittests"),
    ("//components/aegis_access:policy_publication_ack_tracker_unittests",
     "components/aegis_access:policy_publication_ack_tracker_unittests",
     "policy_publication_ack_tracker_unittests"),
    ("//components/aegis_access:request_dispatch_gate_unittests",
     "components/aegis_access:request_dispatch_gate_unittests",
     "request_dispatch_gate_unittests"),
    ("//components/aegis_access:aegis_access_unittests",
     "components/aegis_access:aegis_access_unittests",
     "aegis_access_unittests"),
    ("//chrome/browser/aegis/access:access_service_coordinator_unittests",
     "chrome/browser/aegis/access:access_service_coordinator_unittests",
     "access_service_coordinator_unittests"),
    ("//chrome/browser/aegis/access:access_browser_request_adapter_unittests",
     "chrome/browser/aegis/access:access_browser_request_adapter_unittests",
     "access_browser_request_adapter_unittests"),
    ("//chrome/browser/aegis/access:access_request_dispatch_state_unittests",
     "chrome/browser/aegis/access:access_request_dispatch_state_unittests",
     "access_request_dispatch_state_unittests"),
    ("//chrome/browser/aegis/access:access_identity_generation_source_unittests",
     "chrome/browser/aegis/access:access_identity_generation_source_unittests",
     "access_identity_generation_source_unittests"),
    ("//chrome/browser/aegis/access:access_proxy_selection_generation_source_unittests",
     "chrome/browser/aegis/access:access_proxy_selection_generation_source_unittests",
     "access_proxy_selection_generation_source_unittests"),
    ("//chrome/browser/aegis/access:access_network_context_transport_unittests",
     "chrome/browser/aegis/access:access_network_context_transport_unittests",
     "access_network_context_transport_unittests"),
    ("//chrome/browser/aegis/access:access_rule_store_unittests",
     "chrome/browser/aegis/access:access_rule_store_unittests",
     "access_rule_store_unittests"),
    ("//chrome/browser/aegis/access:access_published_request_runtime_unittests",
     "chrome/browser/aegis/access:access_published_request_runtime_unittests",
     "access_published_request_runtime_unittests"),
)


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def read_pin(path: Path) -> str:
    for raw in path.read_text(encoding="utf-8").splitlines():
        value = raw.strip()
        if value and not value.startswith("#"):
            return value
    raise ValueError(f"pin file is empty: {path}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def tool(name: str, env: dict[str, str] | None = None) -> str:
    search_path = env.get("PATH") if env else None
    found = shutil.which(name, path=search_path)
    if not found:
        raise ValueError(f"required tool is missing: {name}")
    return str(Path(found).resolve(strict=True))


def git(repo: Path, *args: str, env: dict[str, str] | None = None) -> str:
    return subprocess.check_output(
        [tool("git", env), "-C", str(repo), *args],
        env=env,
        text=True,
        stderr=subprocess.STDOUT,
    ).strip()


def is_ancestor(repo: Path, base: str, head: str) -> bool:
    result = subprocess.run(
        [tool("git"), "-C", str(repo), "merge-base", "--is-ancestor", base, head],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError("git merge-base failed while validating Chromium identity")
    return result.returncode == 0


def series(directory: Path) -> list[str]:
    path = directory / "series"
    names = [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not names or len(names) != len(set(names)):
        raise ValueError(f"patch series is empty or duplicated: {path}")
    for name in names:
        if Path(name).name != name or ".." in name or "\\" in name:
            raise ValueError(f"unsafe patch path: {name}")
        if not (directory / name).is_file():
            raise ValueError(f"patch is missing: {directory / name}")
    return names


def require_clean(repo: Path, label: str, *, ignore_submodules: bool = False) -> None:
    args = ["status", "--porcelain"]
    if ignore_submodules:
        args.append("--ignore-submodules=all")
    if git(repo, *args):
        raise ValueError(f"{label} checkout is dirty: {repo}")


def verify_tree(repo: Path, base: str, patches: Path) -> str:
    fd, index_name = tempfile.mkstemp(prefix="aegis-access-tree-")
    os.close(fd)
    index = Path(index_name)
    index.unlink()
    env = dict(os.environ, GIT_INDEX_FILE=str(index))
    try:
        git(repo, "read-tree", base, env=env)
        for name in series(patches):
            git(
                repo,
                "apply",
                "--cached",
                "--whitespace=nowarn",
                str(patches / name),
                env=env,
            )
        expected = git(repo, "write-tree", env=env)
        actual = git(repo, "rev-parse", "HEAD^{tree}")
        if expected != actual:
            raise ValueError(
                f"patched source tree does not match the current patch series: {repo}"
            )
        return actual
    finally:
        index.unlink(missing_ok=True)


def count_gtests(output: str) -> int:
    return sum(
        1
        for line in output.splitlines()
        if line.startswith("  ")
        and line.strip()
        and not line.lstrip().startswith("#")
    )


def run_logged(
    command: Iterable[str],
    *,
    cwd: Path,
    env: dict[str, str],
    log: Path,
) -> None:
    with log.open("w", encoding="utf-8") as stream:
        subprocess.run(
            list(command),
            cwd=cwd,
            env=env,
            stdout=stream,
            stderr=subprocess.STDOUT,
            check=True,
        )


def run_target(
    target: tuple[str, str, str],
    *,
    src: Path,
    out: Path,
    report: Path,
    env: dict[str, str],
    autoninja: str,
    jobs: int,
) -> dict[str, object]:
    label, ninja_target, binary_name = target
    build_log = report / f"{binary_name}.build.log"
    list_log = report / f"{binary_name}.list.log"
    test_log = report / f"{binary_name}.test.log"

    run_logged(
        [autoninja, "-C", str(out), ninja_target],
        cwd=src,
        env=env,
        log=build_log,
    )

    binary = out / binary_name
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"compiled GTest binary is missing: {binary}")

    listed = subprocess.check_output(
        [str(binary), "--gtest_list_tests"],
        cwd=src,
        env=env,
        text=True,
        stderr=subprocess.STDOUT,
    )
    list_log.write_text(listed, encoding="utf-8")
    test_count = count_gtests(listed)
    if test_count <= 0:
        raise ValueError(f"GTest target reported zero tests: {label}")

    run_logged(
        [
            str(binary),
            f"--test-launcher-jobs={jobs}",
            "--test-launcher-retry-limit=0",
            "--test-launcher-print-test-stdio=always",
        ],
        cwd=src,
        env=env,
        log=test_log,
    )
    return {
        "label": label,
        "ninjaTarget": ninja_target,
        "binary": binary_name,
        "tests": test_count,
        "result": "PASS",
        "buildLog": build_log.name,
        "listLog": list_log.name,
        "testLog": test_log.name,
    }


def resolve_chromium_root() -> Path:
    explicit = os.environ.get("CHROMIUM_ROOT")
    if explicit:
        return Path(explicit).expanduser().resolve()
    marker = BROWSER / ".chromium-root"
    if marker.is_file():
        return Path(marker.read_text(encoding="utf-8").strip()).expanduser().resolve()
    return (Path.home() / "Projects" / "GCSA-aegis-chromium").resolve()


def execution_env() -> dict[str, str]:
    env = dict(os.environ)
    depot = Path(
        os.environ.get("DEPOT_TOOLS_DIR", str(Path.home() / "depot_tools"))
    ).expanduser()
    if depot.is_dir():
        env["PATH"] = str(depot.resolve()) + os.pathsep + env.get("PATH", "")
    env.setdefault("DEPOT_TOOLS_UPDATE", "0")
    return env


def acquire_lock(src: Path) -> Path:
    parent = src.parent
    candidate_lock = parent / ".aegis-ci-lock"
    if candidate_lock.exists():
        raise ValueError(f"Chromium candidate build is already using this checkout: {candidate_lock}")
    lock = parent / ".aegis-access-gtest-lock"
    try:
        lock.mkdir()
    except FileExistsError as error:
        raise ValueError(f"another Access GTest run is already active: {lock}") from error
    return lock


def write_summary(result: dict[str, object], report: Path) -> None:
    lines = [
        "# Fixed Chromium Access GTest evidence",
        "",
        f"- Result: {result.get('status')}",
        f"- Product HEAD: {result.get('productHead')}",
        f"- Chromium: {result.get('chromiumVersion')} ({result.get('chromiumPin')})",
        f"- Chromium HEAD: {result.get('chromiumHead')}",
        f"- V8 base: {result.get('v8Base')}",
        f"- Targets: {result.get('targetCount', 0)}",
        f"- Tests: {result.get('testCount', 0)}",
        "",
        "| Target | Tests | Result |",
        "| --- | ---: | --- |",
    ]
    for item in result.get("targets", []):
        lines.append(f"| {item['label']} | {item['tests']} | {item['result']} |")
    if result.get("error"):
        lines += ["", f"Failure: {result['error']}"]
    (report / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def execute(options: argparse.Namespace) -> Path:
    chromium_root = resolve_chromium_root()
    src = chromium_root / "src"
    if not (src / "BUILD.gn").is_file():
        raise ValueError(f"fixed Chromium src is missing: {src}")

    require_clean(ROOT, "product", ignore_submodules=True)
    require_clean(src, "Chromium", ignore_submodules=True)

    product_head = git(ROOT, "rev-parse", "HEAD")
    product_tree = git(ROOT, "rev-parse", "HEAD^{tree}")
    chromium_pin = read_pin(COMMIT_FILE)
    chromium_version = read_pin(VERSION_FILE)
    chromium_head = git(src, "rev-parse", "HEAD")
    if not is_ancestor(src, chromium_pin, chromium_head):
        raise ValueError(
            f"pinned Chromium commit is not an ancestor of the checkout: {chromium_pin}"
        )

    chromium_tree = verify_tree(src, chromium_pin, PATCH_DIR)
    v8_base = git(src, "rev-parse", chromium_pin + ":v8")
    v8 = src / "v8"
    require_clean(v8, "V8", ignore_submodules=True)
    v8_head = git(v8, "rev-parse", "HEAD")
    if not is_ancestor(v8, v8_base, v8_head):
        raise ValueError(f"pinned V8 commit is not an ancestor of the checkout: {v8_base}")
    v8_tree = verify_tree(v8, v8_base, V8_PATCH_DIR)

    out = (
        Path(options.out).expanduser().resolve()
        if options.out
        else src / "out" / "AegisAccessTests"
    )
    report = (
        Path(options.report_dir).expanduser().resolve()
        if options.report_dir
        else ROOT / ".artifacts" / "chromium-native" / product_head[:12]
    )
    report.mkdir(parents=True, exist_ok=True)

    minimum = options.min_free_gib
    free_gib = shutil.disk_usage(src).free / 1024**3
    if free_gib < minimum:
        raise ValueError(
            f"not enough free space for Chromium GTests: {free_gib:.1f} GiB < {minimum} GiB"
        )

    env = execution_env()
    gn = tool("gn", env)
    autoninja = tool("autoninja", env)
    state = report / "result.json"
    result: dict[str, object] = {
        "startedAt": now(),
        "status": "preflight",
        "productHead": product_head,
        "productTree": product_tree,
        "chromiumVersion": chromium_version,
        "chromiumPin": chromium_pin,
        "chromiumHead": chromium_head,
        "chromiumTree": chromium_tree,
        "v8Base": v8_base,
        "v8Head": v8_head,
        "v8Tree": v8_tree,
        "argsSha256": sha256(ARGS_FILE),
        "patchSeriesSha256": sha256(PATCH_DIR / "series"),
        "v8PatchSeriesSha256": sha256(V8_PATCH_DIR / "series"),
        "outDir": str(out),
        "freeGiBAtStart": round(free_gib, 2),
        "targetCount": 0,
        "testCount": 0,
        "targets": [],
    }
    write_json(state, result)

    lock = acquire_lock(src)
    try:
        result["status"] = "generating"
        write_json(state, result)
        args_text = ARGS_FILE.read_text(encoding="utf-8")
        run_logged(
            [gn, "gen", str(out), f"--args={args_text}", "--check"],
            cwd=src,
            env=env,
            log=report / "gn.log",
        )

        result["status"] = "testing"
        for target in TARGETS:
            item = run_target(
                target,
                src=src,
                out=out,
                report=report,
                env=env,
                autoninja=autoninja,
                jobs=options.jobs,
            )
            result["targets"].append(item)
            result["targetCount"] = len(result["targets"])
            result["testCount"] = sum(int(row["tests"]) for row in result["targets"])
            write_json(state, result)

        result["status"] = "PASS"
        return report
    except Exception as error:
        result["status"] = "FAIL"
        result["error"] = str(error)
        raise
    finally:
        result["finishedAt"] = now()
        write_json(state, result)
        write_summary(result, report)
        lock.rmdir()


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser(description=__doc__)
    value.add_argument("--out", help="isolated Chromium output directory")
    value.add_argument("--report-dir", help="evidence output directory")
    value.add_argument(
        "--jobs",
        type=int,
        default=int(os.environ.get("AEGIS_CHROMIUM_TEST_JOBS", "2")),
        help="Chromium test launcher jobs",
    )
    value.add_argument(
        "--min-free-gib",
        type=int,
        default=int(os.environ.get("AEGIS_CHROMIUM_MIN_FREE_GIB", "30")),
        help="minimum free GiB required before building",
    )
    return value


def main() -> int:
    options = parser().parse_args()
    if options.jobs <= 0 or options.min_free_gib <= 0:
        raise SystemExit("--jobs and --min-free-gib must be positive")
    try:
        report = execute(options)
    except Exception as error:
        print(f"FAIL: {error}", file=os.sys.stderr)
        return 1
    print(f"PASS: fixed Chromium Access GTests; evidence: {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
