#!/usr/bin/env bash
# 在 Linux 上编 chrome_public_apk。macOS 会立刻退出并说明原因。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

SRC="$CHROMIUM_ROOT/src"
OUT="${OUT_DIR:-$SRC/out/AegisAndroid}"
ARGS_FILE="$ROOT_DIR/args/aegis-android.gn"
case "${AEGIS_ANDROID_FORMAT:-apk}" in
  apk) build_target=chrome_public_apk; package_file=ChromePublic.apk ;;
  aab) build_target=chrome_public_bundle; package_file=ChromePublic.aab ;;
  *) echo 'AEGIS_ANDROID_FORMAT must be apk or aab' >&2; exit 2 ;;
esac

need_gb="${AEGIS_ANDROID_MIN_FREE_GB:-80}"

fail_host() {
  cat <<EOF
无法在本机编 Android APK。

Chromium $VERSION_PIN 写明：Building the Android client on Windows or Mac is not supported.

这台 Mac 可以当工作机，编译需要使用现有受支持的 x86-64 Linux 环境。
磁盘建议至少空余 ${need_gb}GB（Android 依赖 + out/AegisAndroid）。

下一步见 apps/browser/docs/android.md
EOF
  exit 2
}

VERSION_PIN="$(python3 "$ROOT_DIR/scripts/chromium_inputs.py" --platform android --field version)"

if [[ "$(uname -s)" != "Linux" ]]; then
  fail_host
fi

case "$(uname -m)" in
  x86_64) ;;
  aarch64|arm64)
    if [[ "${AEGIS_ANDROID_EXPERIMENTAL_ARM64:-false}" != true ]]; then
      echo "Linux ARM64 试验需显式设置 AEGIS_ANDROID_EXPERIMENTAL_ARM64=true。" >&2
      exit 2
    fi
    echo "Experimental Linux ARM64 host; pinned x86-64 tools require working emulation."
    ;;
  *) echo "不支持的 Android Linux 构建宿主架构。" >&2; exit 2 ;;
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
if [[ "$OUT" != "$SRC/out/AegisAndroid" || -L "$SRC/out" || -L "$OUT" ]]; then
  echo "Android 只使用固定的实际输出目录：$SRC/out/AegisAndroid" >&2
  exit 1
fi
if [[ -f "$OUT/build.ninja" ]] && ! gn_args_match "$ARGS_FILE" "$OUT/args.gn"; then
  echo "已有 Android 构建参数与当前配置不同；请先核对并重新生成，禁止沿用旧构建图。" >&2
  exit 1
fi
ensure_depot_tools_on_path

cd "$SRC"
if [[ ! -f "$OUT/build.ninja" ]]; then
  echo "Generating $OUT with aegis-android.gn…"
  gn gen "$OUT" --args="$(cat "$ARGS_FILE")"
fi

echo "Building $build_target at $OUT…"
autoninja -C "$OUT" "$build_target"
echo "Package: $OUT/apks/$package_file"
ls -lh "$OUT/apks/$package_file"
