#!/usr/bin/env bash
# 在 Linux 上编 chrome_public_apk。macOS 会立刻退出并说明原因。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "$ROOT_DIR/scripts/android-build-profile.sh"

SRC="$CHROMIUM_ROOT/src"
OUT="${OUT_DIR:-$SRC/out/AegisAndroid}"
ARGS_FILE="$ROOT_DIR/args/aegis-android.gn"

bundle_profile=""
internal_profile=""
if [[ "$#" -ne 0 ]]; then
  if [[ "$#" == 2 && "$1" == --app-bundle && "$2" =~ ^(alpha|beta|release)$ ]]; then
    bundle_profile="$2"
  elif [[ "$#" == 2 && "$1" == --internal-access-options && "$2" =~ ^(alpha|debug)$ ]]; then
    internal_profile="$2"
  else
    echo "Usage: build-android.sh [--app-bundle alpha|beta|release | --internal-access-options alpha|debug]" >&2
    exit 2
  fi
fi

host_arch="$(uname -m)"
if [[ -z "$internal_profile" ]]; then
  default_args="$(aegis_android_gn_args "$ARGS_FILE" default "$host_arch")"
fi

need_gb="${AEGIS_ANDROID_MIN_FREE_GB:-80}"

fail_host() {
  cat <<EOF
无法在本机编 Android APK。

Chromium $VERSION_PIN 写明：Building the Android client on Windows or Mac is not supported.

请在 aegis-android-linux-arm64 上通过 QEMU 运行固定版本的 Linux x86-64 工具链，或使用原生 x86-64 Linux。
磁盘建议至少空余 ${need_gb}GB（Android 依赖 + out/AegisAndroid）。

下一步见 apps/browser/docs/android.md
EOF
  exit 2
}

VERSION_PIN="$(read_pinned_value "$VERSION_FILE")"

if [[ "$(uname -s)" != "Linux" ]]; then
  fail_host
fi

case "$host_arch" in
  x86_64) ;;
  aarch64|arm64)
    if [[ "${AEGIS_ANDROID_ALLOW_ARM64_HOST:-}" != 1 &&
          ( -z "$bundle_profile" || "${AEGIS_ANDROID_EXPERIMENTAL_ARM64:-}" != true ) ]]; then
      echo "Linux ARM64 APK builds require QEMU and AEGIS_ANDROID_ALLOW_ARM64_HOST=1." >&2
      exit 2
    fi
    echo "Linux ARM64 build host using the pinned Linux x86-64 toolchain via QEMU." >&2
    ;;
  *)
    echo "Android builds require Linux ARM64 with QEMU or native x86-64 Linux." >&2
    exit 2
    ;;
esac

free_gb="$(df -Pk "$CHROMIUM_ROOT" 2>/dev/null | awk 'NR==2 {print int($4/1024/1024)}')"
if [[ -n "$free_gb" && "$free_gb" -lt "$need_gb" ]]; then
  echo "磁盘只剩 ${free_gb}GB，Android 构建需要约 ${need_gb}GB。先腾空间。"
  exit 2
fi

if [[ ! -d "$SRC" ]]; then
  echo "Chromium src missing — 在 Linux 上先跑 fetch，并给 .gclient 加上 target_os = ['android']"
  exit 1
fi

if ! grep -q "android" "$CHROMIUM_ROOT/.gclient" 2>/dev/null; then
  echo ".gclient 没有 target_os android。先："
  echo "  bash $ROOT_DIR/scripts/enable-android-gclient.sh"
  echo "  pnpm --filter @gcsa-aegis/browser sync"
  exit 1
fi

# 旧实现会先覆盖 overlay，再直接复用旧 build.ninja，可能把 151 缓存
# 和 153 产品输入混在一起。构建只接受已完成迁移且可重放的干净源码。
python3 "$ROOT_DIR/scripts/verify-build-source.py" --source "$SRC" --platform android
if [[ -n "$bundle_profile" ]]; then
  if [[ ! "${GITHUB_RUN_ID:-}" =~ ^[1-9][0-9]{0,15}$ || "${GITHUB_RUN_ATTEMPT:-}" != 1 ||
        ! "${GITHUB_SHA:-}" =~ ^[0-9a-f]{40}$ ||
        "$OUT" != "$SRC/out/AegisAndroidDelivery-$GITHUB_RUN_ID-1" ||
        -L "$SRC/out" || -e "$OUT" || -L "$OUT" ]]; then
    echo "Android App Bundle requires a new output directory for this producer run." >&2
    exit 1
  fi
  ensure_depot_tools_on_path
  cd "$SRC"
  mkdir -p out
  mkdir "$OUT"
  printf '%s\n%s\n' "$GITHUB_RUN_ID" "$GITHUB_SHA" > "$OUT/.aegis-producer"
  args="$default_args"
  if [[ ! "${AEGIS_ANDROID_VERSION_NAME:-}" =~ ^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}$ ||
        ! "${AEGIS_ANDROID_VERSION_CODE:-}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Android App Bundle requires committed versionName and versionCode." >&2
    exit 1
  fi
  args="$(sed -E "s/^android_override_version_name = .*/android_override_version_name = \"$AEGIS_ANDROID_VERSION_NAME\"/;
    s/^android_override_version_code = .*/android_override_version_code = \"$AEGIS_ANDROID_VERSION_CODE\"/" <<< "$args")"
  if [[ "$bundle_profile" == release ]]; then
    args="$(sed -E 's/^symbol_level = [0-9]+$/symbol_level = 1/' <<< "$args")"
    if ! grep -qx 'symbol_level = 1' <<< "$args"; then
      echo "Android Release requires effective native symbol generation." >&2
      exit 1
    fi
  fi
  args="$(printf '%s\n%s' "$args" 'debuggable_apks = false')"
  args="$(gn format --stdin <<< "$args")"
  gn gen "$OUT" --args="$args"
  aegis_android_verify_gn_args "$OUT" default
  autoninja -C "$OUT" chrome_public_bundle
  python3 "$ROOT_DIR/scripts/verify-build-source.py" --source "$SRC" --platform android
  exit 0
fi
if [[ -n "$internal_profile" ]]; then
  # 内部 DEV / Alpha 构建使用独立输出目录，不进入 package:android 产物路径。
  OUT="$SRC/out/$(aegis_android_internal_out_name "$internal_profile")"
  if [[ -n "${OUT_DIR:-}" || -L "$SRC/out" || -L "$OUT" ]]; then
    echo "内部 Access Options 构建只使用固定的实际输出目录：$OUT" >&2
    exit 1
  fi
  internal_args="$(aegis_android_gn_args "$ARGS_FILE" "internal-$internal_profile" "$host_arch")"
  ensure_depot_tools_on_path
  cd "$SRC"
  internal_args="$(gn format --stdin <<< "$internal_args")"
  if [[ -f "$OUT/build.ninja" ]] &&
      ! aegis_android_gn_args_match <(printf '%s\n' "$internal_args") "$OUT/args.gn"; then
    echo "已有内部 Android 构建参数与当前配置不同；请先核对并重新生成，禁止沿用旧构建图。" >&2
    exit 1
  fi
  echo "Generating $OUT with internal $internal_profile Access Options…"
  gn gen "$OUT" --args="$internal_args"
  aegis_android_verify_gn_args "$OUT" "internal-$internal_profile"
  autoninja -C "$OUT" chrome_public_apk aegis_access_options_test_apk
  echo "Internal APK: $OUT/apks/ChromePublic.apk"
  echo "Instrumentation APK: $OUT/apks/AegisAccessOptionsTest.apk"
  exit 0
fi
if [[ "$OUT" != "$SRC/out/AegisAndroid" || -L "$SRC/out" || -L "$OUT" ]]; then
  echo "Android 只使用固定的实际输出目录：$SRC/out/AegisAndroid" >&2
  exit 1
fi
ensure_depot_tools_on_path
cd "$SRC"
# GN formats args.gn as it writes it. Use that same form for the graph and cache check.
default_args="$(gn format --stdin <<< "$default_args")"
if [[ -f "$OUT/build.ninja" ]] &&
    ! aegis_android_gn_args_match <(printf '%s\n' "$default_args") "$OUT/args.gn"; then
  echo "已有 Android 构建参数与当前配置不同；请先核对并重新生成，禁止沿用旧构建图。" >&2
  exit 1
fi
# Re-evaluate imports and their environment before the effective profile check.
# Equivalent args.gn text alone cannot identify the configuration in build.ninja.
echo "Generating $OUT with aegis-android.gn…"
gn gen "$OUT" --args="$default_args"
aegis_android_verify_gn_args "$OUT" default

echo "Building chrome_public_apk at $OUT…"
autoninja -C "$OUT" chrome_public_apk
echo "APK: $OUT/apks/ChromePublic.apk"
ls -lh "$OUT/apks/ChromePublic.apk"
