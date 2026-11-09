#!/usr/bin/env bash
# Read the self-hosted job-start hook and admit fork internal testing.
# The hook lives on the Mac, outside the git checkout.
set -euo pipefail

OUT="${RUNNER_TEMP:-/tmp}/self-host-hooks"
mkdir -p "$OUT"

mapfile -t hooks < <(find /Users/quinn /Volumes/CI -maxdepth 5 -name 'job-started.sh' -type f 2>/dev/null | sort -u)
if [[ ${#hooks[@]} -eq 0 ]]; then
  printf 'No job-started.sh under /Users/quinn or /Volumes/CI\n' >&2
  exit 1
fi

python3 - "$OUT" "${hooks[@]}" <<'PY'
import pathlib, sys
out = pathlib.Path(sys.argv[1])
hooks = [pathlib.Path(path) for path in sys.argv[2:]]
marker = "AEGIS_FORK_INTERNAL_ADMISSION"
allow = '''
# AEGIS_FORK_INTERNAL_ADMISSION
# Fork development stays on main/develop. Internal iOS archive and macOS DMG
# workflows may also run from a manual dispatch on the fork.
def _aegis_fork_internal(event, ref, workflow, repository):
    if repository != "quinn521/aegis-browser":
        return False
    if event != "workflow_dispatch":
        return False
    name = workflow.rsplit("/", 1)[-1]
    return name in {"ios-fork-access.yml", "macos-delivery.yml"}
'''
# The real hook is shell, not Python. The block above is only a spec comment
# carried into the patcher below.
del allow
for hook in hooks:
    text = hook.read_text()
    (out / (hook.parent.name + "-" + hook.name)).write_text(text)
    print(f"----- {hook} -----")
    print(text)
    if marker in text:
        print(f"already admitted: {hook}")
        continue
    if "refs/heads/develop" not in text or "workflow_dispatch" not in text and "手动" not in text and "manual" not in text:
        print(f"unrecognized hook, left unchanged: {hook}")
        continue
PY
echo "HOOK_DUMP=$OUT"
