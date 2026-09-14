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

# Temporary same-emulator control: prove whether the helper fixtures fail before JaCoCo is present.
set +e
node apps/browser/scripts/android-agent-ui.mjs self-test \
  --adb "$adb" \
  --serial "$serial" \
  --driver-build "$RUNNER_TEMP/aegis-driver-normal" \
  --output "$EVIDENCE_DIR/normal-self-test.json"
normal_status=$?
set -e

helper='app.gcsa.aegis.qa.driver'
inventory="$("$adb" -s "$serial" shell pm list packages "$helper" | tr -d '\r')"
if [[ "$inventory" == "package:$helper" ]]; then
  installed="$("$adb" -s "$serial" shell pm path "$helper" | tr -d '\r')"
  installed="${installed#package:}"
  expected="$(node --input-type=module -e 'import fs from "node:fs"; console.log(JSON.parse(fs.readFileSync(process.argv[1])).apkSha256)' \
    "$RUNNER_TEMP/aegis-driver-normal/build.json")"
  actual="$("$adb" -s "$serial" shell sha256sum "$installed" | awk '{print $1}')"
  if [[ "$actual" != "$expected" || "$("$adb" -s "$serial" uninstall "$helper" | tr -d '\r')" != 'Success' ]]; then
    echo 'normal helper identity could not be removed from the dedicated emulator' >&2
    exit 1
  fi
elif (( normal_status == 0 )); then
  echo 'normal helper disappeared after its successful probe' >&2
  exit 1
fi

set +e
node apps/browser/scripts/android-agent-ui.mjs self-test \
  --adb "$adb" \
  --serial "$serial" \
  --driver-build "$RUNNER_TEMP/aegis-driver-coverage" \
  --coverage-output "$EVIDENCE_DIR/coverage" \
  --output "$EVIDENCE_DIR/self-test.json"
coverage_status=$?
set -e

if (( normal_status != 0 || coverage_status != 0 )); then
  echo "dedicated emulator probe status: normal=$normal_status coverage=$coverage_status" >&2
  # This emulator contains only the dedicated fixture helper; retain its crash class and stack for CI diagnosis.
  "$adb" -s "$serial" logcat -d -v brief 'AndroidRuntime:E' '*:S' | tail -n 120 >&2 || true
fi
(( normal_status == 0 && coverage_status == 0 ))
