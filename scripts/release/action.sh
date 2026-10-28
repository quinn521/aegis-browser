#!/usr/bin/env bash
# The Actions adapter uses the same public CLI as a local release.
set -euo pipefail
for name in DEVELOPER_DIR DEPOT_TOOLS_DIR AEGIS_AAPT2 AEGIS_APKSIGNER; do
  [[ -n "${!name:-}" ]] || unset "$name"
done
stage="${1:?stage is required}"
case "$stage" in package|upload|plan|finish|status) ;; *) exit 2 ;; esac
exec ruby scripts/release/action.rb "$stage"
