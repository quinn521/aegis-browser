#!/usr/bin/env bash
set -euo pipefail
sdk="$ANDROID_HOME"
export PATH="$sdk/platform-tools:$PATH"
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends libpulse0 libgl1 libnss3 libx11-6 libxcb1 libxcomposite1 libxcursor1 libxi6 libxrandr2 libxrender1 libxtst6 libxkbcommon0 libxkbcommon-x11-0
export ANDROID_AVD_HOME="$RUNNER_TEMP/access-avds"
mkdir -p "$ANDROID_AVD_HOME"
image="system-images;android-25;google_apis;x86_64"
"$sdk/cmdline-tools/latest/bin/sdkmanager" "emulator" "platform-tools" "$image"
echo no | "$sdk/cmdline-tools/latest/bin/avdmanager" create avd -n access25 -k "$image" -d "Nexus 5" -p "$ANDROID_AVD_HOME/access25.avd" --force
test -s "$ANDROID_AVD_HOME/access25.avd/config.ini"
printf 'avd.ini.encoding=UTF-8\npath=%s/access25.avd\ntarget=android-25\n' "$ANDROID_AVD_HOME" > "$ANDROID_AVD_HOME/access25.ini"
cd "$RUNNER_TEMP"
curl --retry 2 -fsSL https://dl.google.com/android/repository/emulator-linux_x64-11237101.zip -o emulator.zip
mv "$sdk/emulator" "$RUNNER_TEMP/current-emulator"
unzip -q emulator.zip -d "$sdk"
export ANDROID_SERIAL=emulator-5554
adb start-server
"$sdk/emulator/emulator" -list-avds | grep -Fx access25
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
