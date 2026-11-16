#!/usr/bin/env bash
set -euo pipefail
sdk="$ANDROID_HOME"
export PATH="$sdk/platform-tools:$PATH"
image="system-images;android-25;google_apis;x86_64"
"$sdk/cmdline-tools/latest/bin/sdkmanager" "emulator" "platform-tools" "$image"
echo no | "$sdk/cmdline-tools/latest/bin/avdmanager" create avd -n access25 -k "$image" -d "Nexus 5" --force
cd "$RUNNER_TEMP"
curl --retry 2 -fsSL https://dl.google.com/android/repository/emulator-linux_x64-11237101.zip -o emulator.zip
mv "$sdk/emulator" "$RUNNER_TEMP/current-emulator"
unzip -q emulator.zip -d "$sdk"
export ANDROID_SERIAL=emulator-5554
adb start-server
"$sdk/emulator/emulator" -version
"$sdk/emulator/emulator" -avd access25 -port 5554 -accel on -no-window -no-audio -no-snapshot -gpu swiftshader_indirect -memory 2048 -cores 2 -no-boot-anim -camera-back none -verbose > "$RUNNER_TEMP/emulator25.log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; tail -40 "$RUNNER_TEMP/emulator25.log"' EXIT
for i in {1..60}; do
  kill -0 "$pid"
  if [[ "$(timeout -k 1 4 adb shell getprop ro.build.version.sdk | tr -d '\r')" == 25 && "$(timeout -k 1 4 adb shell getprop ro.product.cpu.abilist)" == *x86_64* ]]; then
    cd "$GITHUB_WORKSPACE"
    python3 harness/.github/access-minapi/check-device.py 25
    exit
  fi
  sleep 2
done
exit 1
