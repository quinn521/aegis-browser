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

## Task 4: Protected Android package token and fresh admission

- [ ] Reproduce missing package token/fresh source preflight with workflow-boundary tests; preserve all non-Android package permissions and effective token values.
- [ ] Add `scripts/ci/android-read-token.mjs` and behavioral tests for exact public fork/source/run/attempt/Admin and required GET access, rejecting unavailable APIs, stale metadata, private/foreign repositories and ref changes.
- [ ] Add an unsigned hosted `read-token-admission` job with only `contents: read` to Android validation. Native waits for it; fork source admission requires contracts/read-token/native success together at the exact latest source/attempt.
- [ ] Supply the ephemeral token only to Android package checks; repeat Admin/source immediately before the credential-bearing package step. Do not enable release flags, install secrets, or change other platform permissions.
- [ ] Run focused checks/actionlint and full same-candidate fork contracts, then independent review/fix regression. New clean source still needs actual hosted read-token and fresh native qualification; no old source proof is reused.

## Review focus

- A rerun starting during admission must invalidate the earlier attempt.
- A skipped native job cannot admit signing.
- A branch or tag advancing during GETs must invalidate admission.
- Rejection must prevent checkout and always-upload steps, not merely return a failed hook status.
- Native probe success is never a product or store acceptance claim.

## Execution ledger

- Task4 implementation: Node52/52 focused checks, locked Ruby3.3.6/minitest5.25.4 signing/routing3/20, actionlint and diff check passed. Workflow/source/read-token requirements and actual post-build signing denial were observed RED before repair. A fresh independent Astra Ultra review of the8de delta plus new probe files found no Critical/Important/Minor findings, and independently ran38/38 checks. Full same-candidate CI, actual contents-only API capability, fresh native and signed package remain pending; no release/readiness flag or service-account grant was enabled. Keep the existing shared package permission at contents:read; a denied real token probe must stay closed rather than broadening shared permissions.
- Task4 candidate4bfc25e fork contracts41437323182/1/job125250291497 passed175/175 (Ruby65/297, Status10/74, Python68/skip6); delivery was skipped. Further live settings reads found shared Draft environment limited to four exact iOS tags. Android-only Draft environment routing reproduced RED and passed52 focused checks/actionlint; separate independent review found no issues and ran22/22. The final environment-routing source requires fresh CI and native/token qualification. Shared global release/rehearsal flags were already true since October28/29; preserve them, correcting the previous unverified assumption that they were false.

- Protected-release follow-up after unsigned qualification: introduce `AEGIS_ANDROID_RELEASE_ENABLED` in `release-preflight.mjs`, `android-rehearsal.mjs`, `release.yml`, and `android-delivery-backend.yml`; retain the exact repository/source/ref pins and native prerequisites. Update `scripts/release/action.rb` so Android candidate Draft plans select only the admitted fork without shared flags. First observe failing source-admission, evaluated workflow-expression and real Ruby request tests; then implement, run focused tests/full contracts/actionlint, and request a fresh review before any enablement. No main change, environment secret installation, signing readiness enablement or AAB10 interruption is part of this code task.
- Protected opt-in verification: Node35/35 focused checks, locked Ruby3.3.6/minitest5.25.4 request test1/13, actionlint and locked Linux full contracts167/167 passed (Ruby63/290, Status10/74, Python68 with6 expected skips). First full-suite fixture mounted the checkout read-only at `/repo` and failed8 checks because tests create temporary files and the linked Git common directory was absent; retained as fixture failure, corrected real-path writable test checkout/read-only common Git mount passed. No product was built by these checks.
- Fresh independent review found an admitted fixed-branch push Draft silently selecting upstream in the Ruby request. Reproduced RED, aligned push/dispatch predicates, GREEN. No Critical or Minor findings. Two pre-existing Tag prerequisites remain explicitly unresolved before release enablement: generic Android package needs a read token for its Admin check, and fresh source/native admission immediately before signing. This opt-in change is not protected signing/package acceptance and enables no repository flag.

- Source gate red: valid Android fork source rejected by iOS-bound branch/validation path; original failure retained in session. Dedicated Android admission now has an independent validation workflow and opt-in/source pin.
- Local initial tests: focused Node39/39, hook3/3, actionlint passed. Initial candidate 7f5499b fork contracts run41415524313/1 passed162/162; delivery skipped.
- Independent read-only review found three defects: nonterminal workflow accepted; source ref could move during final listing; unittest skips could qualify native admission. Each reproduced RED, then repaired with terminal-success checks, a final ref read, and an actual Linux ARM64/no-skips/eight-tests guard.
- Ruling: GitHub GETs do not provide an atomic source/run snapshot. Recheck before and after metadata and again immediately before signing; pin one runner job/attempt and source. A post-check external change remains observable only at the next check, so immutable source identity is retained in every product receipt.
- Unsigned native admission also exercises the original/current Admin check using its own read-only Actions token. Its fixed validation workflow/ref is the only new accepted operator context.
- Runner archive direct download timed out after12MiB; partial retained. Existing SOCKS route tested HTTP206 and used without proxy configuration changes; full official archive140231317bytes verified SHA256670adb2f268fe21dcbb58b3db0e0aa9d5b3bddfe2c651481336e561ff86de945.
