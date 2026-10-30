# Android Delivery

[中文说明](android-delivery.zh-CN.md)

The Actions entrypoint uses the same `./release android` Fastlane package, Upload-key signature verification, Play draft and GitHub Draft implementation. The local CLI is unchanged. This branch is an Android handoff, not permission to merge or publish.

The form has four fields:

| Field | Use |
|---|---|
| Operation | Package, Upload Play draft, Status; select Full for other stages. |
| Product | Auto defaults to Alpha for Package and inherits the receipt for later stages. Alpha is internal only; Beta permits testing but never production; Release still requires an explicit stage. |
| Version or source | Package may leave this blank to use the committed Android version/versionCode, or enter `6.1.0.266(2006000266)`. Later stages require the entire successful delivery receipt. |
| Full options JSON | Leave `{}` unless Operation is Full. Never enter credentials here. |

Package produces an AAB signed by the independent Upload key. It requires the dedicated Linux runner, prepared pinned source and at least 80 GiB available before the build. An APK is a separate local sideload product and cannot be uploaded through this form. A package fixture or toolchain object probe is not a product AAB.

After the **whole run succeeds**, copy its `aegis-android-v1:...` receipt from the summary. Select Upload, keep Product on Auto, and paste the receipt into Version or source. Upload creates a Play **draft on the internal track**; it does not distribute or request production review. Status uses the same receipt and checks the exact versionCode. Use the latest successful receipt for the next stage and retain the original package receipt.

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

Required environments and variables remain those of the existing release backend: `aegis-release-android` for Upload signing, `aegis-play` for the Aegis-authorized Play identity, and `aegis-github-draft` for Draft. Signing secrets are loaded only for package; Play credentials only after import and revalidation. Do not reuse another app's service account as proof of Aegis permission. Workflow UI options are bilingual; local CLI options remain unchanged.

The generic workflow no longer offers Android in its manual platform dropdown; its reusable backend remains for tags and existing callers. Full distribution and submission retain the `aegis-android-distribute` / `aegis-android-submit` stage environments.
