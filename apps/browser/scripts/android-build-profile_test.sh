#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BROWSER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ARGS_FILE="$BROWSER_DIR/args/aegis-android.gn"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/aegis-android-build-profile.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT

# shellcheck source=android-build-profile.sh
source "$SCRIPT_DIR/android-build-profile.sh"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

has_line() {
  grep -qx "$2" <<< "$1"
}

count_matches() {
  grep -c "$2" <<< "$1" || true
}

# 普通 APK 与 App Bundle 都直接取自基础参数，必须保持关闭。
grep -q 'aegis_android_' "$ARGS_FILE" && fail "基础 Android 参数开启了 Access Options"
default_args="$(aegis_android_gn_args "$ARGS_FILE" default)"
[[ "$default_args" == "$(cat "$ARGS_FILE")" ]] || fail "default 配置改动了基础参数"
has_line "$default_args" 'android_channel = "default"' || fail "default 配置改动了渠道"

check_internal() {
  local profile="$1" channel="$2" product="$3" args
  args="$(aegis_android_gn_args "$ARGS_FILE" "internal-$profile")"
  has_line "$args" "android_channel = \"$channel\"" || fail "$profile 未使用 $channel 渠道"
  [[ "$(count_matches "$args" '^android_channel =')" == 1 ]] || fail "$profile 渠道不唯一"
  has_line "$args" "aegis_android_access_product_channel = \"$product\"" || \
    fail "$profile 未声明 $product 产品渠道"
  has_line "$args" 'aegis_android_internal_access_options = true' || fail "$profile 未显式开启"
  has_line "$args" 'chrome_public_manifest_package = "com.gcsa.aegis"' || fail "$profile 改变了包名"
  [[ "$(count_matches "$args" '^chrome_public_manifest_package =')" == 1 ]] || \
    fail "$profile 包名不唯一"
  has_line "$args" 'target_cpu = "arm64"' || fail "$profile 改变了目标架构"
}
check_internal alpha canary alpha
check_internal debug dev debug
[[ "$(aegis_android_internal_out_name alpha)" == AegisAndroidInternalAlpha ]] || fail "alpha 输出目录"
[[ "$(aegis_android_internal_out_name debug)" == AegisAndroidInternalDebug ]] || fail "debug 输出目录"

expect_profile_failure() {
  local label="$1" file="$2" profile="$3"
  if aegis_android_gn_args "$file" "$profile" > /dev/null 2>&1; then
    fail "$label"
  fi
}
expect_profile_failure "接受了 beta 内部配置" "$ARGS_FILE" internal-beta
expect_profile_failure "接受了 release 内部配置" "$ARGS_FILE" internal-release
aegis_android_internal_out_name release > /dev/null 2>&1 && fail "release 有内部输出目录"

enabled_base="$TEST_ROOT/enabled.gn"
{ cat "$ARGS_FILE"; echo 'aegis_android_internal_access_options = true'; } > "$enabled_base"
expect_profile_failure "基础参数已开启时仍生成 default" "$enabled_base" default
expect_profile_failure "基础参数已开启时仍生成内部配置" "$enabled_base" internal-alpha

no_channel="$TEST_ROOT/no-channel.gn"
grep -v '^android_channel = ' "$ARGS_FILE" > "$no_channel"
expect_profile_failure "缺少 default 渠道时仍生成内部配置" "$no_channel" internal-alpha

two_channels="$TEST_ROOT/two-channels.gn"
{ cat "$ARGS_FILE"; echo 'android_channel = "stable"'; } > "$two_channels"
expect_profile_failure "重复渠道时仍生成内部配置" "$two_channels" internal-debug

other_package="$TEST_ROOT/other-package.gn"
sed 's/^chrome_public_manifest_package = .*/chrome_public_manifest_package = "org.chromium.chrome"/' \
  "$ARGS_FILE" > "$other_package"
expect_profile_failure "非产品包名时仍生成内部配置" "$other_package" internal-alpha

two_packages="$TEST_ROOT/two-packages.gn"
{ cat "$ARGS_FILE"; echo 'chrome_public_manifest_package = "org.chromium.chrome"'; } > "$two_packages"
expect_profile_failure "重复包名赋值时仍生成 alpha 内部配置" "$two_packages" internal-alpha
expect_profile_failure "重复包名赋值时仍生成 debug 内部配置" "$two_packages" internal-debug

# 入口只接受显式组合；宿主检查保持不变，任何宿主都不会在测试根目录留下输出。
for bad in "--internal-access-options beta" "--internal-access-options release" \
           "--internal-access-options" "--internal-access-options alpha --app-bundle alpha"; do
  # shellcheck disable=SC2086
  if output="$(CHROMIUM_ROOT="$TEST_ROOT/absent" bash "$SCRIPT_DIR/build-android.sh" $bad 2>&1)"; then
    fail "build-android.sh 接受了 $bad"
  fi
  [[ "$output" == *"Usage: build-android.sh"* ]] || fail "$bad 未在参数检查处失败：$output"
done
if CHROMIUM_ROOT="$TEST_ROOT/absent" bash "$SCRIPT_DIR/build-android.sh" \
    --internal-access-options alpha > /dev/null 2>&1; then
  fail "缺少源码时内部构建仍成功"
fi
[[ ! -e "$TEST_ROOT/absent" ]] || fail "内部构建在缺少源码时创建了目录"

printf 'PASS: Android build profiles keep Access Options internal-only\n'
