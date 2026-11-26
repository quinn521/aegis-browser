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

default_args="$(aegis_android_gn_args "$ARGS_FILE" default)"
[[ "$default_args" == "$(cat "$ARGS_FILE")" ]] || fail "default 配置改动了基础参数"
has_line "$default_args" 'android_channel = "default"' || fail "default 配置改动了渠道"

commented_base="$TEST_ROOT/commented.gn"
{
  sed 's/^android_channel = "default"$/&  # aegis_android_access_product_channel = "debug"/' \
    "$ARGS_FILE"
  printf '%s\n' \
    '  # aegis_android_internal_access_options = true' \
    'assert(true, "escaped quote: \" and hash: #") # aegis_android_access_product_channel' \
    'assert(true, "aegis_android_internal_access_options and aegis_android_access_product_channel")'
} > "$commented_base"
default_with_comments="$(aegis_android_gn_args "$commented_base" default)" ||
  fail "default 配置拒绝了只在注释或字符串里出现的内部参数"
[[ "$default_with_comments" == "$(cat "$commented_base")" ]] ||
  fail "default 配置改动了含注释的基础参数"
for profile in alpha debug; do
  case "$profile" in
    alpha) channel=canary ;;
    debug) channel=dev ;;
  esac
  internal_with_comments="$(aegis_android_gn_args "$commented_base" "internal-$profile")" ||
    fail "$profile 配置拒绝了只在注释或字符串里出现的内部参数"
  grep -Fxq "android_channel = \"$channel\"  # aegis_android_access_product_channel = \"debug\"" \
    <<< "$internal_with_comments" || fail "$profile 未改写频道或保留行末注释"
  grep -Fxq "aegis_android_access_product_channel = \"$profile\"" \
    <<< "$internal_with_comments" || fail "$profile 未设置实际产品渠道"
done

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

nested_base="$TEST_ROOT/nested.gn"
{
  cat "$commented_base"
  printf '%s\n' 'if ("\"#tag" == "\"#tag") { aegis_android_access_product_channel = "alpha" } # valid comment'
} > "$nested_base"
expect_profile_failure "接受了含引号和井号的块内渠道设置" "$nested_base" default
expect_profile_failure "接受了含引号和井号的块内渠道设置" "$nested_base" internal-debug
expect_profile_failure "接受了缺失的基础参数文件" "$TEST_ROOT/missing.gn" default

check_default_build_rejects_enabled_args() {
  local output
  if output="$(CHROMIUM_ROOT="$fixture/chromium" bash "$fixture/scripts/build-android.sh" "$@" 2>&1)"; then
    fail "build-android.sh 接受了开启内部功能的基础参数"
  fi
  [[ "$output" == *"Android 基础参数不得开启内部 Access Options。"* ]] ||
    fail "普通构建没有在 profile 校验处失败：$output"
}

fixture="$TEST_ROOT/build"
fixture_out="$fixture/chromium/src/out/AegisAndroid"
mkdir -p "$fixture/scripts" "$fixture/args" "$fixture_out"
cp "$SCRIPT_DIR/"{build-android.sh,android-build-profile.sh,common.sh} "$fixture/scripts/"
cp "$enabled_base" "$fixture/args/aegis-android.gn"
check_default_build_rejects_enabled_args
check_default_build_rejects_enabled_args --app-bundle alpha
check_default_build_rejects_enabled_args --app-bundle release
cp "$enabled_base" "$fixture_out/args.gn"
touch "$fixture_out/build.ninja"
check_default_build_rejects_enabled_args

(
  export CHROMIUM_ROOT="$fixture/chromium"
  export DEPOT_TOOLS_DIR="$fixture/no-depot-tools"
  export AEGIS_ANDROID_MIN_FREE_GB=0
  unset OUT_DIR
  cp "$BROWSER_DIR/CHROMIUM_VERSION" "$fixture/"
  printf 'target_os = ["android"]\n' > "$CHROMIUM_ROOT/.gclient"
  printf '%s\n' 'aegis_android_internal_access_options = true' \
    'aegis_android_access_product_channel = "alpha"' > "$CHROMIUM_ROOT/src/internal.gni"
  { cat "$commented_base"; echo 'import("//internal.gni")'; } > "$fixture/args/aegis-android.gn"
  cp "$fixture/args/aegis-android.gn" "$fixture_out/args.gn"

  uname() {
    case "$1" in
      -s) printf '%s\n' Linux ;;
      -m) printf '%s\n' x86_64 ;;
      *) return 1 ;;
    esac
  }
  python3() { :; }
  gn() {
    case "$1" in
      gen)
        mkdir -p "$2"
        printf '%s\n' "${3#--args=}" > "$2/args.gn"
        touch "$2/build.ninja"
        ;;
      args)
        [[ "$3" == --list && "$4" == --short ]] || return 1
        if grep -Fxq 'import("//internal.gni")' "$2/args.gn"; then
          cat "$CHROMIUM_ROOT/src/internal.gni"
        elif grep -Fxq 'aegis_android_internal_access_options = true' "$2/args.gn"; then
          grep -E '^(aegis_android_.*|android_channel) =' "$2/args.gn"
          if grep -Fxq 'chrome_public_manifest_package += ".internal"' "$2/args.gn"; then
            printf '%s\n' 'chrome_public_manifest_package = "com.gcsa.aegis.internal"'
          else
            grep '^chrome_public_manifest_package =' "$2/args.gn"
          fi
        else
          printf '%s\n' 'aegis_android_internal_access_options = false' \
            'aegis_android_access_product_channel = "unknown"'
        fi
        ;;
      *) return 1 ;;
    esac
  }
  autoninja() {
    touch "$CHROMIUM_ROOT/ninja-invoked"
    return 1
  }
  export -f uname python3 gn autoninja

  check_build_rejects_args() {
    local expected="$1" output
    shift
    if output="$(bash "$fixture/scripts/build-android.sh" "$@" 2>&1)"; then
      fail "build-android.sh 接受了不合规的 GN 参数"
    fi
    [[ "$output" == *"$expected"* ]] ||
      fail "GN 参数校验应报告 $expected，实际输出：$output"
    [[ ! -e "$CHROMIUM_ROOT/ninja-invoked" ]] || fail "参数不合规时调用了 autoninja"
  }

  clean_out="$CHROMIUM_ROOT/src/out/no-import"
  mkdir "$clean_out"
  cp "$ARGS_FILE" "$clean_out/args.gn"
  aegis_android_verify_gn_args "$clean_out" default
  aegis_android_gn_args "$ARGS_FILE" internal-alpha > "$clean_out/args.gn"
  aegis_android_verify_gn_args "$clean_out" internal-alpha
  default_error='生效 GN 参数必须包含：aegis_android_internal_access_options = false'
  check_build_rejects_args "$default_error"
  rm "$fixture_out/build.ninja"
  check_build_rejects_args "$default_error"
  GITHUB_RUN_ID=42 GITHUB_RUN_ATTEMPT=1 GITHUB_SHA=0123456789abcdef0123456789abcdef01234567 \
    OUT_DIR="$CHROMIUM_ROOT/src/out/AegisAndroidDelivery-42-1" \
    check_build_rejects_args "$default_error" --app-bundle alpha

  { cat "$ARGS_FILE"; echo 'chrome_public_manifest_package += ".internal"'; } > "$fixture/args/aegis-android.gn"
  package_error='生效 GN 参数必须包含：chrome_public_manifest_package = "com.gcsa.aegis"'
  check_build_rejects_args "$package_error" --internal-access-options alpha
  check_build_rejects_args "$package_error" --internal-access-options alpha

  { cat "$ARGS_FILE"; printf '%s\n' 'android_override_version_name += " test version"'; } \
    > "$fixture/args/aegis-android.gn"
  internal_out="$CHROMIUM_ROOT/src/out/AegisAndroidInternalAlpha"
  aegis_android_gn_args "$fixture/args/aegis-android.gn" internal-alpha |
    sed 's/" test version"/"testversion"/' > "$internal_out/args.gn"
  check_build_rejects_args \
    '已有内部 Android 构建参数与当前配置不同' --internal-access-options alpha

  { cat "$ARGS_FILE"; printf '%s\n' 'android_override_version_name += " # expected"'; } \
    > "$fixture/args/aegis-android.gn"
  sed 's/ # expected"/ # stale"/' "$fixture/args/aegis-android.gn" > "$fixture_out/args.gn"
  check_build_rejects_args '已有 Android 构建参数与当前配置不同'

  # Comment and spacing changes outside literals must still allow cache reuse.
  sed 's/^android_channel = "default"$/android_channel="default" # cached comment/' \
    "$fixture/args/aegis-android.gn" > "$fixture_out/args.gn"
  if bash "$fixture/scripts/build-android.sh" > /dev/null 2>&1; then
    fail "模拟 autoninja 应以失败状态结束"
  fi
  [[ -e "$CHROMIUM_ROOT/ninja-invoked" ]] || fail "只改注释和排版时未复用构建图"
)

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
