#!/usr/bin/env bash
# The Actions adapter uses the same public CLI as a local release.
set -euo pipefail
for name in DEVELOPER_DIR DEPOT_TOOLS_DIR AEGIS_AAPT2 AEGIS_APKSIGNER; do
  [[ -n "${!name:-}" ]] || unset "$name"
done
stage="${1:?package or upload is required}"
case "$stage" in package|upload|plan) ;; *) exit 2 ;; esac
args=("${RELEASE_PLATFORM:?}" "$stage" --destination "${RELEASE_DESTINATION:?}" --run-id "${RELEASE_RUN_ID:?}")
[[ -z "${RELEASE_VERSION:-}" ]] || args+=(--version "$RELEASE_VERSION")
[[ -z "${RELEASE_BUILD:-}" ]] || args+=(--build "$RELEASE_BUILD")
[[ -z "${RELEASE_TAG:-}" ]] || args+=(--tag "$RELEASE_TAG")
if [[ "$stage" == upload ]]; then
  args+=(--manifest-sha256 "${RELEASE_MANIFEST_SHA256:?}")
fi
exec ./release "${args[@]}"
