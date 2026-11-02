# Android Go runtime native control implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Distinguish the locked compiler/input pair's native Linux-x64 behavior
from the preserved ARM64 Linux/QEMU runtime failure.

**Architecture:** Dedicated unsigned fork push branch, compact public input
fixture and byte-pinned public compiler download. A GitHub-hosted x64 job runs
64 calls and publishes diagnostic outcomes only. Both2926 delivery checkouts and
all release gates remain unchanged.

**Tech Stack:** Python standard library, pinned checkout/upload-artifact actions,
GitHub-hosted Ubuntu24.04. No package/signing/Play workflow or self-hosted runner.

**Spec:** User-authorized DevTools root-cause investigation and preserved task
evidence/devtools-root-diagnosis-11,13,14. All provenance/hashes are in
diagnostics/android-tsgo/provenance.json and fixture-manifest.json.

## Global constraints

- Branch codex/android-tsgo-native-control only, fork repoID1363949860; no default
  branch, upstream, source-pin, readiness, secret/grant or shared runner changes.
- Original compilerSHA4f2de678286401759b3fb4475bafe35b8f32b4b3a07d92642bbf37eadc9b34a4.
- Original P1/noEmit options; no new runtime mitigation, source/tool replacement,
  cache clearing, sync/hooks, product attempt, signing or store/release action.
- Core0 and minimal compiler child environment; no service/signing/GitHub token
  is forwarded to the compiler. Root is fresh and owned on the disposable hosted
  machine; local invocation refuses before touching the real product path.
- Treat native success as a bounded execution-path control, never a unique
  upstream root-cause fix or any product/delivery acceptance.

## Review focus

- Changed archives or compiler bytes must refuse before execution.
- Archive absolute/traversal/symlink entries must refuse before extraction.
- Missing native executable observation prevents a passing control claim.
- Existing non-Android push workflows must not match this diagnostic branch.
- Artifact selection is restricted to public compiler outcomes, with no cores,
  runner diagnostics, private keys, environment dumps or download credentials.

## Task1: Input-bound unsigned native control

- [x] Reduce the real noEmit failure to312 files/2,357,908 bytes; reproduce29/64
  runtime faults with35 successful type checks under the unchanged local setup.
- [x] Verify upstream tracked source hashes and scan for credential markers;
  exclude unread reference source files and binaries from the fixture archive.
- [x] Write archive/environment boundary tests; observe RED before the new module
  exists, then implement and pass4 local tests.
- [x] Add the branch-only hosted workflow, exact compiler/archive/input hashes,
 64-call outcome recording and per-child native `/proc/PID/exe` observations.
- [x] Independently review scope and extraction/runtime/artifact boundaries.
- [ ] Commit and push this diagnostic branch once, after fresh owner/source/ref
  checks. On unknown push outcome GET reconcile, never force or blindly repeat.
- [ ] Read back the exact fresh run/attempt/jobs, then GET-download its artifact
  and verify server digest, bytes and summary source/run/input/compiler hashes.
- [ ] Record native success/failure separately from all AAB/signature/Console/
  Actions product/Tag/device acceptance states.
