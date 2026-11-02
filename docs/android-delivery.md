# Android Delivery

[中文说明](android-delivery.zh-CN.md)

The Actions entrypoint uses the same `./release android` Fastlane package, Upload-key signature verification, Play draft and GitHub Draft implementation. The local CLI is unchanged. This branch is an Android handoff, not permission to merge or publish.

Two thin manual entries share `android-delivery-backend.yml`; the local CLI is unchanged.

All three entry modes remain source-bound:

| Entry | Result | Acceptance still required |
|---|---|---|
| Local manual `./release android package` | Real Upload-key AAB and manifest | Independent signature, identity and digest checks; then authorized Console internal upload |
| GitHub Actions Simple / Full | Same package backend and downloadable artifact with run/attempt/source/hash pins | Default-branch workflow registration, trusted source/quality and a qualified Android runner; green contracts alone do not build a package |
| `candidate/android/<version>-<profile>.<versionCode>` tag | Existing candidate workflow builds an AAB and creates a private Draft | Exact tag/source readback, qualified runner, Draft asset download and digest verification |

For example, the current native identity maps to `candidate/android/6.1.0.266-alpha.2006000266` only after the final clean source has been admitted. Adding this branch does not create or push a tag. After Android Draft upload, a separate read-only verifier downloads every AAB, identity, manifest and checksum asset and checks its bytes, size and SHA-256. It checks the tag, source, asset IDs and private Draft state before and after download. Failures preserve the existing Draft/upload record; retry only read-only verification. Android's tag job forwards only the two Upload-key password secrets explicitly; it receives no Play or Apple secret. The shared backend declares its existing credential interface without changing the iOS/macOS jobs.

Manual dispatch must first exist on the repository default branch ([GitHub documentation](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)). A workflow on this candidate branch with green push contracts is not proof that the default-branch Run workflow entry has been installed or that privileged packaging is ready.

The currently selected operational path is **package → independent verification → Play Console Alpha internal**, using `tools@gcsa.org`. The existing service account retains its HomeGuard-only application grant; Aegis API Status/Wait returned 403 and remain unqualified. A successful Package summary provides the artifact name, AAB hash, Upload certificate hash, manifest hash, exact source SHA/tree and producer run/attempt for this manual handoff. It never records Console upload/distribution as completed merely because packaging succeeded.

Simple has four fields: **Operation, Version, Build (Android versionCode), Product**.
Its bilingual operations are Package, Package and upload, Status, and Wait; **Status is the default**.
Package requires the exact committed native identity. Status/Wait ignore Product and query the explicit versionCode on the internal track with GET only, without creating or aborting a Play edit. They can inspect an older versionCode without claiming it came from the current query source.

The official [track release summary API](https://developers.google.com/android-publisher/api-ref/rest/v3/applications.tracks.releases/list) excludes obsolete releases and returns at most 20 releases per track. It has no versionName, AAB digest or source identity. A missing versionCode is `NOT_OBSERVED`, never proof that the code is unused. Wait is bounded to ten minutes; a review rejection fails, and states requiring developer action are reported separately from publication. Publication alone does not prove tester availability or physical-device acceptance.

**Package and upload is currently blocked before signing or upload.** An authoritative all-history unused-versionCode check has not been established; this entry never substitutes track absence for that check. New package attempts also reject Actions reruns.

Full preserves the original Product, complete delivery receipt in Version or source, and explicit stage/destination/track/tag/publishing-policy JSON. Full receipt Status retains the legacy edit/abort reconciliation path; it is not labelled GET-only. Credentials come only from the protected stage environments, never form JSON. Both wrappers forward each allowed secret explicitly to the shared backend.

Package produces an AAB signed by the independent Upload key. It requires the dedicated Linux runner, prepared pinned source and at least 40 GiB available before the build by default (`AEGIS_ANDROID_MIN_FREE_GB`). The current local Android controller retains runtime reserves of 15 GiB external and 8 GiB internal. An APK is a separate local sideload product and cannot be uploaded through this form. A package fixture or toolchain object probe is not a product AAB.

After the **whole run succeeds**, copy its `aegis-android-v1:...` receipt from the summary. Open Full, select Upload, keep Product on Auto, and paste the receipt into Version or source. Upload creates a Play **draft on the internal track**; it does not distribute or request production review. Full receipt Status uses the same receipt and checks the exact versionCode. Simple Status/Wait need only the explicit version and versionCode and do not emit a mutation receipt. Use the latest successful receipt for the next stage and retain the original package receipt.

Full mode accepts these string fields only: `operation`, `destination`, `track`, `tag`, `play_release_policy`. It supports package, upload, status, distribute and submit. Examples (Version or source remains the successful receipt):

```json
{"operation":"upload","destination":"play-closed","track":"qa"}
```

```json
{"operation":"distribute","destination":"play-internal"}
```

Distribution requires the completed upload record for the same manifest and track. Play can request testing-track review during distribution; tester availability, review approval and device acceptance remain separate evidence. Beta may select internal/closed/open testing, but cannot select production. Production submission requires Release, an explicit production destination and publishing policy, and separate user authorization; this handoff does not authorize it.

Tag → Draft retains the existing `release-candidate-tag.yml` route, with no second Android tag trigger. Use an existing source-bound `candidate/android/<version>-<profile>.<versionCode>` candidate. Full upload to `github-draft` can also reuse an admitted AAB with an explicit Android candidate tag. The backend keeps the result private, prerelease and draft, and verifies downloaded asset digests. It never publishes a release.

Admission checks the current repository Admin grants of both the original operator and rerunner, exact repository/source and latest quality. Later stages also verify the successful original package run, latest package and state attempts, producer job, original manifest digest, all downloaded bytes, format, certificate evidence and application identity. The receipt is an identity pin, not authorization. A source change requires a new package from the new clean candidate. Metadata API failures stop admission.

Store mutations cannot be replayed by rerunning Actions. If an upload result is uncertain, inspect retained intent/result records and read Play status before retrying. Failed-run artifacts are diagnostic evidence and cannot become a successful delivery source.

The fork branch push job runs uncredentialed contracts only. It cannot start the package/Play/Draft jobs, consume another platform's runner or replace real AAB/Play/Tag acceptance. Privileged fork rehearsal still requires the existing explicit fork identity, exact-source release-validation and runner readiness gates; upstream operation requires main and green exact-source quality. No readiness flag or repository environment is enabled by adding this workflow.

Required environments and variables remain those of the existing release backend: `aegis-release-android` for Upload signing, `aegis-play` for the Aegis-authorized Play identity, and `aegis-github-draft` for Draft. Signing secrets are loaded only for package; Play credentials only after receipt import and revalidation, or after Admin/query/source checks for Simple GET queries. The GET credential loader accepts an explicitly supplied Aegis-authorized service account; it does not fall back to ambient credentials. The caller permits the shared backend’s existing Draft write scope; executed query/package/store jobs retain read-only repository access, and only the Draft job requests contents write. Do not reuse another app's service account as proof of Aegis permission. Workflow UI options are bilingual; local CLI options remain unchanged.

The generic workflow no longer offers Android in its manual platform dropdown; its reusable backend remains for tags and existing callers. Full distribution and submission retain the `aegis-android-distribute` / `aegis-android-submit` stage environments.

Android fork admission uses the fixed `codex/android-release-acceptance` branch and `android-release-validation.yml`. Explicit opt-in, repository ID and exact source SHA are required. The latest exact run/attempt must pass both hosted contracts and the unsigned native-admission job; skipped, duplicate, incomplete, changed-source or racing evidence is rejected. This Android chain does not run iOS archives or alter upstream main/quality admission. The dedicated runner uses a read-only installed pre-checkout hook pinned to one run/attempt, actor, workflow, ref and source, terminating only its verified owning worker on rejection. Release readiness remains false until real positive and negative qualification succeeds. AAB10 retains its original process and lock.
