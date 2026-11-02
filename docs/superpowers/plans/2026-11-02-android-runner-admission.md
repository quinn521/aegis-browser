# Android runner admission implementation plan

> Execute inline under the existing user authorization; retain AAB10 and failed evidence.

**Goal:** Remove the Android-specific fork source and runner blockers without changing other platforms or service-account grants.

**Spec:** ../specs/2026-11-02-android-runner-admission-design.md

## Constraints

- Current owner 01a19ada-b833-7bf0-9309-d860785b13fe; check before own process control.
- AAB10 f36c447 remains running; do not sync, restart, clear caches or reuse its evidence for a new SHA.
- 40 GiB build entry, 15 GiB external and 8 GiB internal reserves.
- No main/default-branch change, merge, public release, production review or shared VM change.
- No Play account permission expansion or additional PAT.

## Task 1: Exact-source Android rehearsal admission

- [ ] Add behavioral tests to scripts/ci/tests/android-rehearsal.test.mjs; observe the current iOS-bound admission fail for a valid Android candidate.
- [ ] Add Android-only fork admission and android-release-validation.yml (hosted contracts + unsigned native-admission). Keep generic upstream and other-platform rules unchanged.
- [ ] Validate latest run/attempt, complete jobs, repository/source/ref identity and before/after ref stability. Test skipped/duplicate/racing/wrong-source evidence and opt-in failures.
- [ ] Run focused Node tests, actionlint, and full locked fork contracts on the new clean candidate.

## Task 2: Dedicated runner boundary

- [ ] Add admission hook behavioral tests: exact pin accepted; external repository/PR, stale source, wrong workflow/ref, incomplete config and wrong worker rejected.
- [ ] Implement a host-pinned hook with a verified owning-worker abort; retain rejected-job evidence.
- [ ] Download and hash the official Linux ARM64 runner, test runtime in an owned bounded container, and qualify positive/negative jobs before enabling release readiness.
- [ ] Preserve all existing runners, VM settings and AAB10. No signing material for unsigned admission.

## Task 3: Real acceptance sequence

- [ ] Finish AAB10, independently verify Upload certificate, every signed entry, application identity and digest; upload only via Console Alpha internal.
- [ ] After its lock releases, run a real Actions package at the final clean candidate, then the existing candidate Tag→private Draft and download/hash every asset.
- [ ] Record manual-button default-branch registration as incomplete until separately authorized integration; keep all three route receipts separate.

## Review focus

- A rerun starting during admission must invalidate the earlier attempt.
- A skipped native job cannot admit signing.
- A branch or tag advancing during GETs must invalidate admission.
- Rejection must prevent checkout and always-upload steps, not merely return a failed hook status.
- Native probe success is never a product or store acceptance claim.
