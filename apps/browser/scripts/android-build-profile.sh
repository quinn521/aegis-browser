#!/usr/bin/env bash
# Android GN 参数配置。普通 APK 与 App Bundle 继续关闭 Access Options；
# 只有显式的内部 DEV / Alpha 配置才打开，并同时设定一致的 Chromium 渠道。

aegis_android_internal_out_name() {
  case "$1" in
    alpha) printf 'AegisAndroidInternalAlpha\n' ;;
    debug) printf 'AegisAndroidInternalDebug\n' ;;
    *) return 1 ;;
  esac
}

# 用法：aegis_android_gn_args <args-file> default|internal-alpha|internal-debug
aegis_android_gn_args() {
  local args_file="$1"
  local profile="$2"
  local args channel product
  args="$(cat "$args_file")"
  if grep -Eq '^[[:space:]]*aegis_android_(internal_access_options|access_product_channel)[[:space:]]*=' <<< "$args"; then
    echo "Android 基础参数不得开启内部 Access Options。" >&2
    return 1
  fi
  case "$profile" in
    default)
      printf '%s\n' "$args"
      return 0
      ;;
    internal-alpha) channel=canary product=alpha ;;
    internal-debug) channel=dev product=debug ;;
    *)
      echo "未知 Android 构建配置：$profile" >&2
      return 1
      ;;
  esac
  if [[ "$(grep -cx 'android_channel = "default"' <<< "$args")" != 1 ||
        "$(grep -c '^[[:space:]]*android_channel[[:space:]]*=' <<< "$args")" != 1 ||
        "$(grep -cx 'chrome_public_manifest_package = "com.gcsa.aegis"' <<< "$args")" != 1 ]]; then
    echo "Android 基础参数必须只有一个 default 渠道与 com.gcsa.aegis 包名。" >&2
    return 1
  fi
  args="$(sed -E "s/^android_channel = \"default\"\$/android_channel = \"$channel\"/" <<< "$args")"
  printf '%s\n%s\n%s\n' "$args" \
    "aegis_android_access_product_channel = \"$product\"" \
    'aegis_android_internal_access_options = true'
}
