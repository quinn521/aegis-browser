#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

paths=(scripts/ci/coverage-impact.sh)
case "${1:-}" in
  ios)
    paths+=(
      .github/workflows/ios-coverage.yml
      apps/ios
      apps/browser/overlay/chrome/browser/resources/aegis/enhancements_shared
      packages/core
      scripts/release/build-ios-access-core.sh
      scripts/release/ios_embedded_core.py
      scripts/release/prepare-ios-access-core.sh
      scripts/release/prepare-ios-package-core.sh
      scripts/release/record-ios-access-core-digests.sh
    )
    ;;
  android)
    paths+=(.github/workflows/android-java-coverage.yml apps/browser packages/core)
    ;;
  *)
    echo 'Expected coverage platform ios or android' >&2
    exit 2
    ;;
esac

cd "$(git rev-parse --show-toplevel)"
test "$(git rev-parse HEAD)" = "$GITHUB_SHA"
base=''
pr_base=''
pr_head=''
case "$GITHUB_EVENT_NAME" in
  pull_request)
    parents="$(git show -s --format=%P HEAD)"
    read -r pr_base pr_head extra <<< "$parents"
    [[ -n "$pr_base" && -n "$pr_head" && -z "$extra" ]]
    test "$pr_head" = "$EVENT_PR_HEAD"
    [[ "$EVENT_PR_BASE" =~ ^[0-9a-f]{40}$ ]]
    git merge-base --is-ancestor "$EVENT_PR_BASE" "$pr_base"
    base="$pr_base"
    ;;
  push)
    [[ "$EVENT_PUSH_BEFORE" =~ ^[0-9a-f]{40}$ ]]
    if [[ "$EVENT_PUSH_BEFORE" != '0000000000000000000000000000000000000000' ]]; then
      git merge-base --is-ancestor "$EVENT_PUSH_BEFORE" "$GITHUB_SHA"
      base="$EVENT_PUSH_BEFORE"
    fi
    ;;
  workflow_dispatch)
    ;;
  *)
    echo "Unsupported coverage event: $GITHUB_EVENT_NAME" >&2
    exit 2
    ;;
esac

run=true
if [[ -n "$base" ]]; then
  # Compare Git objects directly so path selection sees the entire tested diff.
  if git diff --quiet --no-ext-diff --no-textconv --no-renames "$base" "$GITHUB_SHA" -- "${paths[@]}"; then
    run=false
  else
    diff_status=$?
    test "$diff_status" -eq 1 || exit "$diff_status"
  fi
fi
printf 'run=%s\npr_base=%s\npr_head=%s\n' "$run" "$pr_base" "$pr_head" >> "$GITHUB_OUTPUT"
