#!/usr/bin/env bash
# The unsigned archive supervisor launches this worker with private tool output.
set -euo pipefail

test "$PWD" = "${IOS_CI_SOURCE:?IOS_CI_SOURCE is required}" || exit 1
: "${IOS_CI_ROOT:?IOS_CI_ROOT is required}"

unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_IMPLICIT_WORK_TREE GIT_INDEX_FILE \
  GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_QUARANTINE_PATH \
  GIT_GRAFT_FILE GIT_REPLACE_REF_BASE GIT_NO_REPLACE_OBJECTS GIT_SHALLOW_FILE \
  GIT_NAMESPACE GIT_ATTR_SOURCE
export MISE_TRUSTED_CONFIG_PATHS="${MISE_TRUSTED_CONFIG_PATHS:-$PWD}"
if command -v mise >/dev/null 2>&1; then
  GOROOT="$(mise where go@1.27.3 || true)"
  if [[ -n "${GOROOT:-}" ]]; then
    export GOROOT
    export PATH="$GOROOT/bin:${GOPATH:-$HOME/go}/bin:$PATH"
  fi
fi
export GOTOOLCHAIN="${GOTOOLCHAIN:-local}"
command -v go >/dev/null || exit 2
go_version="$(go version)"
[[ "$go_version" == "go version go1.27.3 "* ]] || exit 2

if ! command -v gomobile >/dev/null 2>&1; then
  go install golang.org/x/mobile/cmd/gomobile@v0.0.0-20261011034528-197770444867
  export PATH="${GOPATH:-$HOME/go}/bin:$PATH"
  gomobile init
fi

# Both the bridge and its receipt must use the job-private checkout.
AEGIS_EMBEDDED_CORE_DIGEST_MODE=ci-rebuild \
  bash scripts/release/prepare-ios-access-core.sh \
    "$PWD/apps/ios/EmbeddedCore/embedded-core-lock.json" \
    "$IOS_CI_ROOT/EmbeddedCore"
test -d "$IOS_CI_ROOT/EmbeddedCore/AegisEmbeddedCore.xcframework"
