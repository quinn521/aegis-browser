#!/usr/bin/env bash
# Compile the uncredentialed iOS archive from the job-private Source tree.
# Runner cancellation signals this entry process, then kills its process tree.
# Keep xcodebuild in the same process group and reap by PID before exiting so a
# cancelled archive cannot outlive the step or race the next checkout.
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
archive_pid=""
tee_pid=""

signal_pid() {
  local pid=$1 signal=$2
  [[ -n "$pid" ]] || return 0
  kill "-$signal" "$pid" 2>/dev/null || true
}

wait_pid() {
  local pid=$1
  [[ -n "$pid" ]] || return 0
  local _
  for _ in $(seq 1 8); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.25
  done
}

job_root_pids() {
  [[ -n "$IOS_CI_ROOT" && "$IOS_CI_ROOT" == /* ]] || return 0
  # ps is the portable enumerator; pgrep is preferred when the host allows it.
  if command -v pgrep >/dev/null 2>&1; then
    pgrep -f "$IOS_CI_ROOT" 2>/dev/null || true
  fi
  ps -axo pid=,command= 2>/dev/null | while IFS= read -r line; do
    case "$line" in
      *"$IOS_CI_ROOT"*)
        # shellcheck disable=SC2086
        set -- $line
        printf '%s\n' "$1"
        ;;
    esac
  done
}

reap_job_root() {
  local signal=$1 pid
  while IFS= read -r pid; do
    [[ -n "$pid" && "$pid" != "$$" ]] || continue
    kill "-$signal" "$pid" 2>/dev/null || true
  done < <(job_root_pids | sort -u)
}

reap_archive() {
  # Kill the pipeline subshell, its children, then any worker still bound to this
  # job root. Do not create a separate process group: Actions tree-kill must see
  # xcodebuild as part of the entry process tree.
  if [[ -n "$archive_pid" ]]; then
    pkill -TERM -P "$archive_pid" 2>/dev/null || true
  fi
  signal_pid "$archive_pid" TERM
  signal_pid "$tee_pid" TERM
  reap_job_root TERM
  wait_pid "$archive_pid"
  wait_pid "$tee_pid"
  if [[ -n "$archive_pid" ]]; then
    pkill -KILL -P "$archive_pid" 2>/dev/null || true
  fi
  signal_pid "$archive_pid" KILL
  signal_pid "$tee_pid" KILL
  reap_job_root KILL
  wait 2>/dev/null || true
  archive_pid=""
  tee_pid=""
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

# Stay in the runner entry process group so Actions tree-kill reaches xcodebuild.
# Capture status without set -e aborting before PIPESTATUS is read.
set +e
(
  set +e
  cd "$SOURCE"
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
archive_pid=$!
# tee is the only durable sibling once xcodebuild is running; locate it by log path.
sleep 0.1
tee_pid=$(pgrep -f "tee ${IOS_CI_ROOT}/xcodebuild.log" | head -n 1 || true)

wait "$archive_pid"
archive_status=$?
set -e
archive_pid=""
tee_pid=""
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
