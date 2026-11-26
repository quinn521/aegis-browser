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

# 每行输出一个 GN token。忽略注释和分隔空白，保留语句顺序和字符串原值。
aegis_android_gn_tokens() {
  LC_ALL=C awk '
    {
      line = $0
      while (1) {
        sub(/^[[:space:]]+/, "", line)
        if (line == "" || line ~ /^#/) break
        match(line, /^("([^"\\]|\\.)*"|[[:alpha:]_][[:alnum:]_]*|-?[0-9]+|[-+=!<>&|][=|&]|.)/)
        print substr(line, 1, RLENGTH)
        line = substr(line, RLENGTH + 1)
      }
    }
  ' "$@"
}

aegis_android_gn_args_match() {
  local expected actual
  expected="$(aegis_android_gn_tokens "$1")" || return 1
  actual="$(aegis_android_gn_tokens "$2")" || return 1
  [[ "$expected" == "$actual" ]]
}

# 用法：aegis_android_gn_args <args-file> default|internal-alpha|internal-debug
aegis_android_gn_args() {
  local args_file="$1"
  local profile="$2"
  local args tokens channel product
  local default_channel_pattern='^([[:blank:]]*android_channel[[:blank:]]*=[[:blank:]]*)"default"([[:blank:]]*(#.*)?)$'
  local package_pattern='^[[:blank:]]*chrome_public_manifest_package[[:blank:]]*=[[:blank:]]*"com[.]gcsa[.]aegis"[[:blank:]]*(#.*)?$'
  args="$(cat "$args_file")" || return 1
  tokens="$(aegis_android_gn_tokens <<< "$args")" || return 1
  if grep -Exq 'aegis_android_(internal_access_options|access_product_channel)' <<< "$tokens"; then
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
  if [[ "$(grep -Ec "$default_channel_pattern" <<< "$args")" != 1 ||
        "$(grep -c '^[[:space:]]*android_channel[[:space:]]*=' <<< "$args")" != 1 ||
        "$(grep -Ec "$package_pattern" <<< "$args")" != 1 ||
        "$(grep -c '^[[:space:]]*chrome_public_manifest_package[[:space:]]*=' <<< "$args")" != 1 ]]; then
    echo "Android 基础参数必须只有一个 default 渠道与 com.gcsa.aegis 包名。" >&2
    return 1
  fi
  args="$(sed -E "s/$default_channel_pattern/\1\"$channel\"\2/" <<< "$args")" ||
    return 1
  printf '%s\n%s\n%s\n' "$args" \
    "aegis_android_access_product_channel = \"$product\"" \
    'aegis_android_internal_access_options = true'
}

aegis_android_verify_gn_args() {
  local out_dir="$1" profile="$2" gn_bin="${3:-gn}"
  local effective_args enabled product channel setting
  local -a expected_args
  case "$profile" in
    default) enabled=false product=unknown ;;
    internal-alpha) enabled=true product=alpha channel=canary ;;
    internal-debug) enabled=true product=debug channel=dev ;;
    *) return 1 ;;
  esac
  expected_args=(
    "aegis_android_internal_access_options = $enabled"
    "aegis_android_access_product_channel = \"$product\""
  )
  if [[ "$enabled" == true ]]; then
    expected_args+=(
      "android_channel = \"$channel\""
      'chrome_public_manifest_package = "com.gcsa.aegis"'
    )
  fi
  if ! effective_args="$("$gn_bin" args "$out_dir" --list --short)"; then
    echo "无法读取 Android $profile 构建的生效 GN 参数。" >&2
    return 1
  fi
  for setting in "${expected_args[@]}"; do
    if ! grep -Fxq "$setting" <<< "$effective_args"; then
      printf 'Android %s 构建的生效 GN 参数必须包含：%s\n' "$profile" "$setting" >&2
      return 1
    fi
  done
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  set -euo pipefail
  if [[ "$#" != 4 || "$1" != --verify ]]; then
    echo "用法：android-build-profile.sh --verify <out-dir> <profile> <gn>" >&2
    exit 2
  fi
  aegis_android_verify_gn_args "$2" "$3" "$4"
fi
