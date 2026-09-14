#!/usr/bin/env bash
set -euo pipefail

: "${ANDROID_SDK_ROOT:?ANDROID_SDK_ROOT is required}"
: "${RUNNER_TEMP:?RUNNER_TEMP is required}"
: "${EVIDENCE_DIR:?EVIDENCE_DIR is required}"

if [[ "$EVIDENCE_DIR" != /* || -e "$EVIDENCE_DIR/coverage" || -e "$EVIDENCE_DIR/self-test.json" ]]; then
  echo "coverage evidence paths must be new and absolute" >&2
  exit 1
fi

adb="$ANDROID_SDK_ROOT/platform-tools/adb"
serial="emulator-5554"
"$adb" -s "$serial" wait-for-device

if [[ "$("$adb" -s "$serial" shell getprop ro.kernel.qemu | tr -d '\r')" != "1" ]]; then
  echo "coverage self-test requires the dedicated emulator" >&2
  exit 1
fi
if [[ "$("$adb" -s "$serial" shell am get-current-user | tr -d '\r')" != "0" ]]; then
  echo "coverage self-test requires Android user 0" >&2
  exit 1
fi

"$adb" -s "$serial" shell input keyevent KEYCODE_WAKEUP
"$adb" -s "$serial" shell wm dismiss-keyguard
"$adb" -s "$serial" shell input keyevent 82

node apps/browser/scripts/android-agent-ui.mjs self-test \
  --adb "$adb" \
  --serial "$serial" \
  --driver-build "$RUNNER_TEMP/aegis-driver-coverage" \
  --coverage-output "$EVIDENCE_DIR/coverage" \
  --output "$EVIDENCE_DIR/self-test.json"
