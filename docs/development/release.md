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

Run `./release` in a terminal for the menu, or use the same explicit CLI in scripts. `--profile alpha|beta|release` is independent of destination. All profiles build optimized release binaries; they do not enable Debug test hooks. iOS embeds `AegisReleaseProfile` in the app and extensions. Native profiles identify the package/manifest without inventing different application behavior. Changing a manifest profile cannot promote an Alpha package into a Release package.

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

`upload`, `distribute` and `submit` require the independently retained manifest SHA-256 and the same clean source/profile/version/build/format. Output lives in `.artifacts/release/<platform>/<run-id>/`. A new package never overwrites an existing attempt. Interrupted store writes retain `*-started.json`; only a completed and verified stage gets `*-result.json`. A process lock prevents concurrent writes for the same package. A completed stage is idempotent for identical parameters; an unknown result blocks automatic retry. `status` does not remove markers or silently authorize another upload. Reconcile ambiguous results with ASC/Play before recovery; do not delete an intent to bypass this check.

Local manual lanes accept the operator's clean committed branch, including a fork test branch. GitHub Actions owns its upstream quality admission. Upstream GitHub Draft uploads also retain that admission. `--repository owner/fork` explicitly selects a fork for draft rehearsals; it never changes the upstream formal-release policy. A Draft requires an existing `candidate/<platform>/...` tag resolving to the exact package SHA. It never moves tags, publishes drafts or overwrites existing assets; retries read back remote hashes first.

iOS overrides numeric version/build only in an attempt-owned tracked-source copy. Native builds require the committed product version/versionCode; `0.8.0 (1)` is currently the iOS test identity, not a filename override for Chromium. `CHROMIUM_ROOT` must be a dedicated, synchronized, patched workspace. No shared checkout reset or fetch is performed.

TF distribution requires exact existing group names of the correct internal/external type and a processed unexpired build. External distribution requests Beta App Review, with tester notification disabled. Upload success alone is `uploaded_processing_unverified`. ASC production submission requires a prepared matching App Store version, listing, privacy/review information and compliance; the script does not guess these answers. It explicitly sets manual publication, submits the exact build and reads back the review state. Neither workflow publishes an approved App Store version.

Play uses an upload-key signed AAB by default; `--format apk` keeps the existing sideload path. Upload creates only a draft and preserves existing completed releases. Distribution/submission commits the selected track and requests review; Google may additionally require Console action. The edits API does not prove approval or tester availability. It refuses unrelated drafts/active rollouts and never silently retries with different review semantics. Production `submit` requires `--play-release-policy managed` after verifying managed publishing in Console, or `automatic` if publication after approval is intended. This option records the operator's policy; the API does not verify or change managed publishing. The first Play app upload/setup may require Console setup.

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

Use actual installed profile names. Package-only needs signing material, but no ASC/Play publishing credentials. Android verifies every signed payload entry with the expected upload certificate, then verifies the final AAB identity. Its native build-identity file describes the pre-signing snapshot; the release manifest separately binds the final signed bytes. APK retains the Chromium signature and is not Play-qualified. DMG remains ad-hoc signed and unnotarized.

## GitHub Actions configuration

`.github/workflows/release.yml` is manual-only and accepts only upstream repository ID `1348409663`, `gcsagroup/aegis-browser`, on `main`. Source comes from `github.sha`; there is no arbitrary branch/SHA input. It stays disabled until repository variable `AEGIS_RELEASE_ENABLED=true` is set.

Create protected environments restricted to `main`, with required reviewers:

| Environment | Configuration |
| --- | --- |
| `aegis-release-ios` | Variables `AEGIS_APPLE_TEAM_ID`, `AEGIS_IOS_PROFILES`, `AEGIS_MATCH_GIT_URL`, optional `AEGIS_DEVELOPER_DIR`; secrets `MATCH_PASSWORD`, `MATCH_GIT_BASIC_AUTHORIZATION` |
| `aegis-release-macos` | Variables `AEGIS_CHROMIUM_ROOT`, `AEGIS_DEPOT_TOOLS_DIR`, optional `AEGIS_DEVELOPER_DIR` |
| `aegis-release-android` | Variables `AEGIS_CHROMIUM_ROOT`, `AEGIS_DEPOT_TOOLS_DIR`, `AEGIS_AAPT2`, `AEGIS_APKSIGNER` |
| `aegis-testflight` | The three `AEGIS_ASC_*` secrets above |
| `aegis-github-draft` | Approval boundary for the job's Contents-write `GITHUB_TOKEN` |

The iOS job runs on `macos-26`, creates a temporary CI keychain and reads existing signing assets with `match(readonly: true)`. It does not create app IDs, profiles or certificates. Set up the signing repository and ASC app separately before enabling the workflow. The Match credential must have read-only access to that signing repository.

Chromium requires dedicated runners labeled `[self-hosted, macOS, ARM64, aegis-release]` and `[self-hosted, Linux, X64, aegis-release]`, with sufficient disk, Git, Python 3.11+, Node, Ruby 3.3.6, Bundler 2.5.22, native toolchains and the prepared checkout. Do not assign these labels to PR runners or an active development workspace. Set repository variable `AEGIS_CHROMIUM_RELEASE_RUNNERS_READY=true` only after validating those hosts. Runner setup and environment protection are service-side prerequisites, not proven by YAML.

The preflight uses only a read token and precedes protected packaging. Package jobs have Contents read; TestFlight and GitHub uploads are separate jobs that download only this run's artifact and verify the manifest digest passed directly from the package job. Only the GitHub upload job has Contents write. Uploaded artifacts exclude keychains, profiles, build work directories and credential files.

## Qualification and validation

Before merge, pushing a `codex/fastlane-release-*` branch runs `Release Branch Validation` on a fork or upstream branch. It runs credential-free regressions and compiles an unsigned iOS `0.8.0 (1)` archive, checking the app and both extension identities. It has no release environment or signing secrets. These diagnostics do not qualify an IPA, TestFlight upload, or the complete three-platform flow; signed packaging/upload and the subsequent Android then DMG acceptance remain separate.

Real local packaging/upload, Actions execution, candidate-tag Draft, store state and device acceptance are distinct evidence. A green simulation or unsigned archive cannot close the remaining real-build or upload checks. Developer ID/notarization, Chromium build hosts and Play account/upload-key setup must be available for their corresponding real tests.

```bash
bundle exec ruby fastlane/tests/stores_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml
```

Regression tests use disposable repositories, simulated Apple/Play/GitHub responses and real JDK signing of a synthetic bundle. They verify profile/identity freezing, corruption, wrong groups, duplicate/unknown uploads, track transitions, manual publication, locks and signature failures. They do not upload a production app or establish device evidence. Ensure JAVA_HOME selects a complete JDK on macOS. References: [Fastlane build_app](https://docs.fastlane.tools/actions/build_app/), [TestFlight upload](https://docs.fastlane.tools/actions/upload_to_testflight/), [GitHub environment secrets](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets).
