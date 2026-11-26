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

# 未指定渠道时逐行输出记号，供守卫和参数比较使用。指定渠道时只改写
# 原有 android_channel 的字符串字面量，保留它和包名赋值附近的排版与注释。
aegis_android_gn_scan() {
  local channel="$1"
  shift
  LC_ALL=C awk -v channel="$channel" '
    function record(token, position) {
      if (channel == "") {
        print token
        return
      }
      if (token == "=") {
        if (previous == "android_channel") channels++
        if (previous == "chrome_public_manifest_package") packages++
      }
      if (previous == "=") {
        if (name == "android_channel" && token == "\"default\"") {
          channel_line = NR
          channel_column = position
        }
        if (name == "chrome_public_manifest_package" && token == "\"com.gcsa.aegis\"") {
          has_package = 1
        }
      }
      name = previous
      previous = token
    }
    {
      line = $0
      column = 1
      if (channel != "") source[NR] = line
      while (1) {
        match(line, /^[[:space:]]*/)
        column += RLENGTH
        line = substr(line, RLENGTH + 1)
        if (line == "" || line ~ /^#/) break
        match(line, /^("([^"\\]|\\.)*"|[[:alpha:]_][[:alnum:]_]*|-?[0-9]+|[-+=!<>&|][=|&]|.)/)
        width = RLENGTH
        record(substr(line, 1, width), column)
        column += width
        line = substr(line, width + 1)
      }
    }
    END {
      if (channel != "") {
        if (channels != 1 || packages != 1 || !channel_line || !has_package) exit 1
        line = source[channel_line]
        source[channel_line] = substr(line, 1, channel_column - 1) "\"" channel "\"" \
            substr(line, channel_column + length("\"default\""))
        for (i = 1; i <= NR; i++) print source[i]
      }
    }
  ' "$@"
}

aegis_android_gn_args_match() {
  local expected actual
  expected="$(aegis_android_gn_scan "" "$1")" || return 1
  actual="$(aegis_android_gn_scan "" "$2")" || return 1
  [[ "$expected" == "$actual" ]]
}

# 用法：aegis_android_gn_args <args-file> default|internal-alpha|internal-debug
aegis_android_gn_args() {
  local args_file="$1"
  local profile="$2"
  local args tokens channel product
  args="$(cat "$args_file")" || return 1
  tokens="$(aegis_android_gn_scan "" <<< "$args")" || return 1
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
  if ! args="$(aegis_android_gn_scan "$channel" <<< "$args")"; then
    echo "Android 基础参数必须只有一个 default 渠道与 com.gcsa.aegis 包名。" >&2
    return 1
  fi
  printf '%s\n%s\n%s\n' "$args" \
    "aegis_android_access_product_channel = \"$product\"" \
    'aegis_android_internal_access_options = true'
}

aegis_android_verify_gn_args() {
  local out_dir="$1" profile="$2" gn_bin="${3:-gn}"
  local effective_args enabled product channel
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
  # GN print() diagnostics share stdout with the listing unless GN is quiet.
  if ! effective_args="$("$gn_bin" args "$out_dir" --list --short --json -q)"; then
    echo "无法读取 Android $profile 构建的生效 GN 参数。" >&2
    return 1
  fi
  python3 -c '
import json
import sys

def unique_keys(pairs):
    record = dict(pairs)
    if len(record) != len(pairs):
        raise ValueError("duplicate JSON key")
    return record

try:
    records = json.load(sys.stdin, object_pairs_hook=unique_keys)
    if not isinstance(records, list):
        raise ValueError("expected an argument list")
    effective = {}
    for record in records:
        name = record["name"]
        value = record.get("current", record["default"])["value"]
        if not isinstance(name, str) or not isinstance(value, str) or name in effective:
            raise ValueError("invalid or duplicate argument")
        effective[name] = value
except (KeyError, TypeError, ValueError):
    sys.exit("Android 生效 GN 参数的 JSON 输出无效。")

profile = sys.argv[1]
for setting in sys.argv[2:]:
    name, expected = setting.split(" = ", 1)
    if effective.get(name) != expected:
        sys.exit(f"Android {profile} 构建的生效 GN 参数必须包含：{setting}")
  ' "$profile" "${expected_args[@]}" <<< "$effective_args"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  set -euo pipefail
  if [[ "$#" == 2 && "$1" == --validate-template ]]; then
    aegis_android_gn_args "$2" default >/dev/null
  elif [[ "$#" == 4 && "$1" == --verify ]]; then
    aegis_android_verify_gn_args "$2" "$3" "$4"
  else
    echo "用法：android-build-profile.sh --validate-template <args-file> | --verify <out-dir> <profile> <gn>" >&2
    exit 2
  fi
fi
