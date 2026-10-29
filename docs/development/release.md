[English](release.md) | [简体中文](release.zh-CN.md) | [繁體中文](release.zh-TW.md)

# Local Fastlane and upstream release workflow

The root `./release` command packages iOS IPA, Android arm64 APK/AAB and macOS arm64 DMG. It follows HomeGuard iOS's separation of preparation, packaging and upload while using Aegis's own projects, build drivers and signing identities. `plan` needs only Ruby and Git; executing lanes requires Ruby 3.3+, Bundler and the checked-in Gemfile.lock. CI uses Ruby 3.3.6 and Bundler 2.5.22. Install dependencies with `bundle install`.

| Platform | Application ID | Host | Upload destination |
| --- | --- | --- | --- |
| iOS | `com.gcsa.aegis` | macOS with Xcode | TestFlight, ASC/App Review, or GitHub candidate draft |
| Android | `com.gcsa.aegis` | Linux x86-64; prepared Chromium workspace | Play or GitHub candidate draft |
| macOS | `com.gcsa.aegis` | Apple Silicon; prepared Chromium workspace | GitHub candidate draft |

iOS extensions use `com.gcsa.aegis.safari` and `com.gcsa.aegis.share`. The existing App Group `group.com.gcsa.aegis.ios.share-inbox` and keychain service names are retained. Provision the new app and extension IDs with the existing App Group capability. These IDs differ from historical development builds; existing app data and in-place updates are **not** claimed to migrate. The Android QA driver's independent ID and Windows product ID are unchanged.

## Manual stages (first acceptance stage)

Run `./release` in a terminal for the menu, or use the same explicit CLI in scripts. `--profile alpha|beta|release` selects the package profile. Alpha store distribution is restricted to TestFlight internal groups or Play internal testing; external TestFlight, Play closed/open testing and production destinations are rejected before packaging or credential access. iOS Alpha exports always set `testFlightInternalTestingOnly=true`, including package-only and GitHub Draft builds, so Apple also prevents external testing and App Store submission. Beta/Release exports set it to false. Beta supports internal and external testing but cannot submit for production review. Release supports all listed destinations; upload, distribution and submission remain separate explicit stages. The default destination remains `artifacts` (no upload); `testflight` is upload-only, and distribution requires explicit existing internal group names. External testing requires a new Beta or Release package and build number. See [Apple distribution options](https://developer.apple.com/documentation/xcode/distributing-your-app-for-beta-testing-and-releases). All profiles build optimized release binaries; they do not enable Debug test hooks. iOS embeds `AegisReleaseProfile` in the app and extensions. Native profiles identify the package/manifest without inventing different application behavior. Changing a manifest profile cannot promote an Alpha package into a Release package.

| Stage | Effect |
| --- | --- |
| `plan` | Print source, profile, identity and destination; no build or upload |
| `package` | Build and verify locally, without store credentials |
| `upload` | Reuse the verified package; Apple transport only, Play draft only |
| `release` | Convenience for package + upload; no distribution or review submission |
| `distribute` | Reuse the exact processed build for selected TF groups or a Play testing track |
| `submit` | Explicit production review stage, requiring profile `release` |
| `status` | Read current store state and local stage records; no upload |

| Platform | Destinations |
| --- | --- |
| iOS | `artifacts`, `testflight` (upload only), `testflight-internal`, `testflight-external`, `asc` (upload only), `app-store`, `github-draft` |
| Android | `artifacts`, `play-internal`, `play-closed` (default `alpha`, override with `--track`), `play-open` (`beta`), `play-production`, `github-draft` |
| macOS | `artifacts`, `github-draft` |

```bash
./release ios plan --profile alpha --destination testflight-internal --version 0.8.0 --build 1
./release ios package --profile alpha --version 0.8.0 --build 1 --run-id ios-alpha-1
./release ios upload --profile alpha --destination testflight-internal --version 0.8.0 --build 1 \
  --run-id ios-alpha-1 --manifest-sha256 <retained-package-digest>
./release ios status --profile alpha --destination testflight-internal --version 0.8.0 --build 1 --run-id ios-alpha-1
./release ios distribute --profile alpha --destination testflight-internal --groups 'Existing Internal Group' \
  --version 0.8.0 --build 1 --run-id ios-alpha-1 --manifest-sha256 <retained-package-digest>

# Build a new Release-profile package before using production destinations.
./release ios submit --profile release --destination app-store --version 0.8.0 --build 2 \
  --run-id ios-release-2 --manifest-sha256 <retained-release-package-digest>

# Android's native version and versionCode must already be committed in source.
./release android package --profile beta --format aab --run-id android-beta
./release android upload --profile beta --destination play-closed --run-id android-beta \
  --manifest-sha256 <retained-package-digest>
./release android distribute --profile beta --destination play-closed --run-id android-beta \
  --manifest-sha256 <retained-package-digest>
./release macos package --profile release --run-id mac-release
```

`upload`, `distribute` and `submit` require the independently retained manifest SHA-256 and the same clean source/profile/version/build/format. Output lives in `.artifacts/release/<platform>/<run-id>/`. A new package never overwrites an existing attempt. Interrupted store writes retain `*-started.json`; only a completed and verified stage gets `*-result.json`. A process lock prevents concurrent writes for the same package. Distribution/review also requires the completed upload record for that package and track. A completed stage is idempotent for identical parameters; an unknown result blocks automatic retry. `status` does not remove markers or silently authorize another upload. Reconcile ambiguous results with ASC/Play before recovery; do not delete an intent to bypass this check.

Local manual lanes accept the operator's clean committed branch, including a fork test branch. GitHub Actions owns its upstream quality admission. Upstream GitHub Draft uploads also retain that admission. `--repository owner/fork` explicitly selects a fork for draft rehearsals; it never changes the upstream formal-release policy. A Draft requires an existing `candidate/<platform>/...` tag resolving to the exact package SHA. It never moves tags, publishes drafts or overwrites existing assets; retries read back remote hashes first.

The local package entrypoint checks Safari manifest icons before archiving, and archive/IPA validation checks the packaged PNG resources and their dimensions. Missing icons (App Store error 90849) fail locally before upload.

iOS overrides numeric version/build only in an attempt-owned tracked-source copy. Native builds require the committed product version/versionCode; `0.8.0 (1)` is currently the iOS test identity, not a filename override for Chromium. `CHROMIUM_ROOT` must be a dedicated, synchronized, patched workspace. No shared checkout reset or fetch is performed.

Android source verification, package names and native identity use `apps/browser/ci-pins/android.json` and its patch directory. Desktop inputs remain in `CHROMIUM_VERSION`, `CHROMIUM_COMMIT` and `patches/`. A desktop checkout cannot substitute for Android's independent base. The supported Android acceptance environment requires x86-64 Linux and at least 200 GB available space; an ARM64 VM running an emulated x86-64 container does not satisfy that gate.

TF distribution requires exact existing group names of the correct internal/external type and a processed unexpired build. External distribution requests Beta App Review, with tester notification disabled. Upload success alone is `uploaded_processing_unverified`. ASC production submission requires a prepared matching App Store version, listing, privacy/review information and compliance; the script does not guess these answers. It explicitly sets manual publication, submits the exact build and reads back the review state. Neither workflow publishes an approved App Store version.

Play uses an upload-key signed AAB by default; `--format apk` keeps the existing sideload path. Upload creates only a draft and preserves existing completed releases. Distribution/submission commits the selected track and requests review; Google may additionally require Console action. The edits API does not prove approval or tester availability. It refuses unrelated drafts/active rollouts and explicitly uses ERROR_IF_IN_REVIEW, so it cannot cancel another review. It never silently retries with different review semantics. Production `submit` requires `--play-release-policy managed` after verifying managed publishing in Console, or `automatic` if publication after approval is intended. This option records the operator's policy; the API does not verify or change managed publishing. The first Play app upload/setup may require Console setup.

Distribution and review require the exact build's export compliance declaration. If it is missing, the app owner must complete it in ASC; the script does not assume an exemption. TestFlight distribution explicitly selects iOS without prompting and does not enter a second Pilot processing wait.

## Local configuration

Use process environment or ignored Fastlane `.env` files. Never put secrets in lane arguments, receipts or committed files. Install the locked bundle with Ruby 3.3.6/Bundler 2.5.22; when using a local vendor bundle, set `BUNDLE_PATH=vendor/bundle` consistently.

| Variables | Purpose |
| --- | --- |
| `AEGIS_APPLE_TEAM_ID`, `AEGIS_IOS_PROFILES` | Signing team and JSON mapping of three bundle IDs to installed App Store profile names |
| `AEGIS_ASC_KEY_ID`, `AEGIS_ASC_ISSUER_ID` | ASC API identity |
| `AEGIS_ASC_KEY_PATH` or `AEGIS_ASC_KEY_BASE64` | Private `.p8` file (mode 600), or Base64 secret; upload/distribute/submit/status only |
| `CHROMIUM_ROOT`, `DEPOT_TOOLS_DIR`, `DEVELOPER_DIR` | Prepared native workspace and toolchain |
| `AEGIS_AAPT2`, `AEGIS_APKSIGNER` | APK verification tools, defaulting to PATH |
| `AEGIS_BUNDLETOOL_JAR`, JDK 17+ on PATH | AAB identity verification; provide the official bundletool JAR |
| `AEGIS_ANDROID_KEYSTORE`, `AEGIS_ANDROID_KEY_ALIAS`, `AEGIS_ANDROID_CERT_SHA256` | Private Play upload keystore, alias, expected certificate fingerprint |
| `AEGIS_ANDROID_STORE_PASSWORD`, `AEGIS_ANDROID_KEY_PASSWORD` | Passwords passed to Java via environment, never command-line values |
| `AEGIS_PLAY_JSON_KEY` | Private Play service-account JSON file (mode 600), granted only the needed app/track permissions |
| `GH_TOKEN` | Upstream read admission; Contents write only for the selected GitHub Draft repository |

```json
{"com.gcsa.aegis":"Aegis AppStore","com.gcsa.aegis.safari":"Aegis Safari AppStore","com.gcsa.aegis.share":"Aegis Share AppStore"}
```

Use actual installed profile names. Package-only needs signing material, but no ASC/Play publishing credentials. Android verifies every signed payload entry with the expected upload certificate, then verifies the final AAB identity. Its native build-identity file describes the pre-signing snapshot; the release manifest separately binds the final signed bytes. APK retains the Chromium signature and is not Play-qualified. DMG signing follows `apps/browser/scripts/sign-chromium-app.sh`. The release entrypoint signs a small Mach-O before the long build, then checks the final DMG against the same certificate, Apple anchor and team. The configured keychain must be unlocked; no private key is exported. This is not Developer ID/notarization qualification.

## GitHub Actions (second acceptance stage)

`.github/workflows/release.yml` supports manual dispatch and reuse by the candidate-tag workflow. Choose platform, profile, destination and the last operation (`package`, `upload`, `distribute`, `submit`). The default is package-only. `store_options` is JSON with only string `groups`, `track`, `play_release_policy` fields, for example `{"groups":"Existing Internal Group"}`; it never accepts credentials. Upstream requires repository ID `1348409663`, exact current `main`, the latest successful exact-source quality attempt, and `AEGIS_RELEASE_ENABLED=true`.

The uncredentialed preflight checks routing/source first. Protected package jobs only have Contents read. Upload jobs download this run's artifact and use the manifest digest and run ID passed directly from the package job. Store distribution/review is a further protected job that also downloads the successful upload record. Apple processing is bounded to 30 minutes and always selects the exact version/build, never the latest build. Only GitHub Draft has Contents write. Artifacts whitelist packages/manifests/stage records, excluding profiles, keychains, secrets and work directories.

Create protected environments with required reviewers. Restrict upstream sources to `main` and the intended `candidate/*` tags:

| Environment | Configuration |
| --- | --- |
| `aegis-release-ios` | Variables `AEGIS_APPLE_TEAM_ID`, `AEGIS_IOS_PROFILES`, `AEGIS_MATCH_GIT_URL`, optional `AEGIS_DEVELOPER_DIR`; secrets `MATCH_PASSWORD`, `MATCH_GIT_BASIC_AUTHORIZATION` |
| `aegis-release-macos` | Variables `AEGIS_CHROMIUM_ROOT`, `AEGIS_DEPOT_TOOLS_DIR`, `AEGIS_LOCAL_MACOS_BUNDLE_PATH`, `AEGIS_CIPD_CACHE_DIR`, `AEGIS_VPYTHON_VIRTUALENV_ROOT`, `AEGIS_NATIVE_TMPDIR`, optional `AEGIS_DEVELOPER_DIR`; preinstalled native signing identity/keychain; no ASC/Play/Android store secrets |
| `aegis-release-android` | Variables for Chromium/depot_tools, bundletool, upload keystore path/alias/certificate SHA-256; secrets `AEGIS_ANDROID_STORE_PASSWORD`, `AEGIS_ANDROID_KEY_PASSWORD`; SDK/JDK installed on the runner |
| `aegis-testflight` | ASC key ID, issuer and Base64 key secrets; uploads only |
| `aegis-play` | Secret `AEGIS_PLAY_JSON_KEY_DATA`; uploads a draft only |
| `aegis-ios-distribute`, `aegis-ios-submit` | ASC secrets; separate approval for tester distribution or App Review |
| `aegis-android-distribute`, `aegis-android-submit` | Play JSON secret; separate approval for testing or production submission |
| `aegis-github-draft` | Approval boundary for the job's Contents-write token |

The hosted iOS package job installs existing signing assets using `match(readonly: true)` into an ephemeral CI keychain. Prepare the signing repository, the three profiles and ASC application separately. No new certificates or identifiers are generated by CI. Play JSON is materialized in a private task-owned temporary directory, removed after the child exits, and omitted from the child environment; it is not an artifact.

Chromium runners need `[self-hosted, macOS, ARM64, aegis-release]` or `[self-hosted, Linux, X64, aegis-release]`, sufficient disk and a dedicated prepared checkout. They need Git, Python 3.11+, Node 22.23.1, Ruby 3.3.6, Bundler 2.5.22 and native toolchains. Set `AEGIS_MACOS_RELEASE_RUNNERS_READY=true` or `AEGIS_ANDROID_RELEASE_RUNNERS_READY=true` separately after verification. The legacy `AEGIS_CHROMIUM_RELEASE_RUNNERS_READY` remains a fallback; an explicit platform `false` takes precedence. Enabling Mac cannot enable Android. YAML does not prove any runner or protected environment is configured.

The macOS self-hosted path checks preinstalled runtimes, the clean admitted source SHA and owned workspace/cache permissions, and reuses the pinned Bundler cache without installing into hosted tool caches. It first exercises real miniature DMG mounting, signing and corruption regressions, then invokes the same local package entrypoint. The final DMG must pass read-only mounting, app ID/product version, arm64 and deep signature verification. Its verified digest is bound into the release manifest. This establishes neither browser runtime acceptance nor notarization.

Fork branch packaging also requires the corresponding `AEGIS_IOS_BRANCH_REHEARSAL=true` or `AEGIS_MACOS_BRANCH_REHEARSAL=true`; candidate tag entrypoints do not depend on these branch switches. Keep Android readiness `false` while deferred. Mac can independently complete local packaging, Actions and candidate tag/Draft acceptance.

Xcode 26 may require a separately installed Metal compiler: `xcodebuild -downloadComponent MetalToolchain`. Local packaging and macOS Actions first run `python3 scripts/release/verify-macos-toolchain.py`, which compiles a minimal shader into an AIR object. `xcrun --find metal` can find the stub without the component. A missing component or compiler failure stops before creating the package directory or starting Chromium compilation.

For an isolated **fork rehearsal**, explicitly set `AEGIS_RELEASE_ENABLED=true`, `AEGIS_RELEASE_REHEARSAL=true` and `AEGIS_RELEASE_TEST_REPOSITORY_ID` to that fork's numeric repository ID, and prepare its own protected signing environment. The API verifies that the repository is a fork of upstream and that the same SHA passed `contracts` and `ios-archive` in the latest branch validation attempt. Only `codex/fastlane-release-*` branches and candidate tags are accepted. Only package-only, TF upload/internal testing, Play internal testing and a Draft **in that fork** are allowed. Production and external-testing destinations are refused. This returns `fork_rehearsal_only`, never upstream admission. The branch validation workflow can call the real package workflow after its two uncredentialed checks, avoiding a merge just to gain workflow_dispatch. Without the opt-in variables, that package rehearsal stays skipped; a skipped job is not packaging evidence.

Actions reruns may rebuild packages or resume a GitHub Draft by verifying remote hashes. They cannot replay Apple/Play write stages after attempt 1, because a runner can disappear after the store accepts a write but before its receipt is saved. Inspect retained stage records and live store state, then reconcile locally or start a fresh build/run. Do not blindly re-upload the same version/build. Native source/version rules still apply.

Fork iOS package/Draft rehearsals may additionally set `AEGIS_IOS_LOCAL_REHEARSAL=true`. This selects only `[self-hosted, macOS, ARM64, aegis-ios-release-rehearsal]` after the same uncredentialed preflight, and rejects upstream, other branches and store destinations. Register a task-owned ephemeral runner for one job; do not add release labels to shared quality runners. Its protected `aegis-release-ios` environment supplies the existing team/profile mapping and `AEGIS_LOCAL_IOS_BUNDLE_PATH` for an owned bundle cache. Preinstall the pinned Ruby/Node/Bundler versions and signing assets, and unlock the keychain locally. This path exports no private key and receives no ASC/Play/Match credentials. The hosted Match route remains the default. Neither route can consume another repository's Secrets merely by naming it as a Match backend.

## Candidate tag to Draft Release (third acceptance stage)

`.github/workflows/release-candidate-tag.yml` parses a platform-specific candidate tag and calls the same workflow with destination `github-draft` and operation `upload`. It never responds to formal `v*` tags, uploads to stores, or publishes a Release.

```bash
# Local simulation, without creating a tag or uploading:
ruby scripts/release/tag.rb candidate/ios/0.8.0-alpha.1
ruby scripts/release/tag.rb candidate/android/6.1.0.266-beta.2006000266
ruby scripts/release/tag.rb candidate/macos/6.1.0.266-release.266
```

The tag encodes platform, profile, exact product version and build/versionCode. Native versions must match committed inputs. Create/push the tag only after the corresponding local and Actions package tests pass. Upstream still requires that tag's commit to be current `main` with successful quality checks. Fork tags follow the separate opt-in rehearsal admission. Draft creation checks the existing tag's exact SHA, preserves draft/prerelease state, verifies each remote asset SHA-256 and refuses replacement. Formal release validation and publication remain separate.

Draft creation uses the REST response's release ID and saves its intent/result before uploading assets. Subsequent checks read that ID directly, so a newly created draft need not immediately appear in the releases inventory. An unknown creation outcome blocks another POST; Actions reruns can resume a known draft but cannot create a replacement when neither a remote draft nor its creation receipt is available. CI retains creation records even when the job fails. Existing tags and assets are never overwritten during recovery.

## Qualification and validation

Before merge, pushing a `codex/fastlane-release-*` branch runs `Release Branch Validation` on a fork or upstream branch. Its first two jobs run credential-free regressions and compile an unsigned iOS `0.8.0 (1)` archive, checking the app and both extension identities. Those jobs have no release environment or signing secrets. The separately enabled platform rehearsals can then run real packaging. These diagnostics do not qualify an IPA, TestFlight upload, or the complete three-platform flow. Each platform separately requires local package, Actions and candidate Draft acceptance.

Real local packaging/upload, Actions execution, candidate-tag Draft, store state and device acceptance are distinct evidence. A green simulation or unsigned archive cannot close the remaining real-build or upload checks. Developer ID/notarization, Chromium build hosts and Play account/upload-key setup must be available for their corresponding real tests.

```bash
bundle exec ruby fastlane/tests/tag_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml .github/workflows/release-validation.yml .github/workflows/release-candidate-tag.yml
```

Regression tests use disposable repositories, simulated Apple/Play/GitHub responses and real JDK signing of a synthetic bundle. They verify profile/identity freezing, corruption, wrong groups, duplicate/unknown uploads, track transitions, manual publication, locks and signature failures. They do not upload a production app or establish device evidence. Ensure JAVA_HOME selects a complete JDK on macOS. References: [Fastlane build_app](https://docs.fastlane.tools/actions/build_app/), [TestFlight upload](https://docs.fastlane.tools/actions/upload_to_testflight/), [GitHub environment secrets](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets).

Store API references: [Apple upload and distribution](https://docs.fastlane.tools/actions/upload_to_testflight/), [Google edit commit and review behavior](https://developers.google.com/android-publisher/api-ref/rest/v3/edits/commit).
