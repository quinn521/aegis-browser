#!/usr/bin/env bash
# Compile the uncredentialed iOS archive from the job-private Source tree.
# Runner cancellation sends SIGINT then SIGTERM to this entry process. Reap the
# xcodebuild process group before exiting so the next job can check out safely.
set -euo pipefail

: "${IOS_CI_ROOT:?IOS_CI_ROOT is required}"

SOURCE="${IOS_CI_SOURCE:-$IOS_CI_ROOT/Source}"
if [[ "$SOURCE" != /* || ! -d "$SOURCE" ]]; then
  printf 'archive source must be an absolute directory under admitted storage\n' >&2
  exit 1
fi
if [[ ! -f "$SOURCE/apps/ios/Aegis.xcodeproj/project.pbxproj" ]]; then
  printf 'archive source is missing apps/ios/Aegis.xcodeproj\n' >&2
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=apps/browser/scripts/common.sh
source "$REPO_ROOT/apps/browser/scripts/common.sh"
configure_git_for_chromium_fetch

export TMPDIR="$IOS_CI_ROOT/tmp"
export CLANG_MODULE_CACHE_PATH="$IOS_CI_ROOT/ModuleCache"
mkdir -p "$TMPDIR" "$CLANG_MODULE_CACHE_PATH"

python3 -B "$REPO_ROOT/scripts/release/ios_embedded_core.py" verify --root "$SOURCE" \
  --source-lock "$SOURCE/apps/ios/EmbeddedCore/embedded-core-lock.json" \
  --products "$IOS_CI_ROOT/EmbeddedCore"

archive_status=0
archive_pgid=""

reap_archive() {
  local target="${archive_pgid:-}"
  [[ -n "$target" ]] || return 0
  if kill -0 -"$target" 2>/dev/null; then
    kill -TERM -"$target" 2>/dev/null || true
    local _
    for _ in $(seq 1 20); do
      kill -0 -"$target" 2>/dev/null || return 0
      sleep 0.25
    done
    kill -KILL -"$target" 2>/dev/null || true
  fi
  wait 2>/dev/null || true
}

on_signal() {
  local code=$1
  trap - EXIT INT TERM
  reap_archive
  exit "$code"
}

trap 'on_signal 143' TERM
trap 'on_signal 130' INT
trap 'status=$?; trap - EXIT; reap_archive; exit "$status"' EXIT

set -m
(
  # Keep set -e off inside the job group so a failed xcodebuild cannot skip
  # PIPESTATUS capture; the entry process still fails closed on that status.
  set +e
  cd "$SOURCE"
  # Keep Actions live output and the retained job log without orphaning xcodebuild
  # when the entry shell exits on cancellation.
  xcodebuild -scmProvider system -onlyUsePackageVersionsFromResolvedFile \
    -project apps/ios/Aegis.xcodeproj -scheme Aegis -configuration Release \
    -destination 'generic/platform=iOS' \
    -archivePath "$IOS_CI_ROOT/Aegis.xcarchive" \
    -derivedDataPath "$IOS_CI_ROOT/DerivedData" \
    -clonedSourcePackagesDirPath "$IOS_CI_ROOT/SourcePackages" \
    -packageCachePath "$IOS_CI_ROOT/PackageCache" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO \
    MARKETING_VERSION=1.0.0 CURRENT_PROJECT_VERSION=1 \
    AEGIS_EMBEDDED_CORE_PRODUCTS="$IOS_CI_ROOT/EmbeddedCore" \
    archive 2>&1 | tee "$IOS_CI_ROOT/xcodebuild.log"
  exit "${PIPESTATUS[0]}"
) &
archive_pgid=$!
set +m

set +e
wait "$archive_pgid"
archive_status=$?
set -e
archive_pgid=""
trap - EXIT INT TERM

if (( archive_status != 0 )); then
  exit "$archive_status"
fi

python3 "$REPO_ROOT/scripts/release/verify-ios-archive.py" \
  "$IOS_CI_ROOT/Aegis.xcarchive" 1.0.0 1
python3 -B "$REPO_ROOT/scripts/release/test_ios_linked_bundle.py" \
  "$IOS_CI_ROOT/Aegis.xcarchive/Products/Applications/Aegis.app" \
  "$IOS_CI_ROOT/EmbeddedCore" "$IOS_CI_ROOT/DerivedData" device
python3 -I -B "$REPO_ROOT/scripts/ci/ios-ci-storage.py" seal --root "$IOS_CI_ROOT"
