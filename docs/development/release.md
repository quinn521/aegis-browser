[English](release.md) | [简体中文](release.zh-CN.md) | [繁體中文](release.zh-TW.md)

# Local Fastlane and upstream release workflow

The root `./release` command packages iOS IPA, Android arm64 APK and macOS arm64 DMG. It follows HomeGuard iOS's separation of preparation, packaging and upload while using Aegis's own projects, build drivers and signing identities. `plan` needs only Ruby and Git; executing lanes requires Ruby 3.3+, Bundler and the checked-in Gemfile.lock. CI uses Ruby 3.3.6 and Bundler 2.5.22. Install dependencies with `bundle install`.

| Platform | Application ID | Host | Upload destination |
| --- | --- | --- | --- |
| iOS | `com.gcsa.aegis` | macOS with Xcode | TestFlight or GitHub candidate draft |
| Android | `com.gcsa.aegis` | Linux x86-64; prepared Chromium workspace | GitHub candidate draft |
| macOS | `com.gcsa.aegis` | Apple Silicon; prepared Chromium workspace | GitHub candidate draft |

iOS extensions use `com.gcsa.aegis.safari` and `com.gcsa.aegis.share`. The existing App Group `group.com.gcsa.aegis.ios.share-inbox` and keychain service names are retained. Provision the new app and extension IDs with the existing App Group capability. These IDs differ from historical development builds; existing app data and in-place updates are **not** claimed to migrate. The Android QA driver's independent ID and Windows product ID are unchanged.

## Local stages

```bash
./release ios plan --destination testflight --version 3.2 --build 70
./release ios package --version 3.2 --build 70 --run-id ios-70
./release ios upload --destination testflight --version 3.2 --build 70 \
  --run-id ios-70 --manifest-sha256 <digest-printed-by-package>

./release macos plan --destination github-draft --tag candidate/macos/2.2.0.144
./release macos release --destination github-draft \
  --tag candidate/macos/2.2.0.144 --run-id mac-candidate-144
./release android release --destination github-draft \
  --tag candidate/android/2.2.0.144 --run-id android-candidate-144
```

`package` defaults to local artifacts. `release` runs package then upload; `upload` reuses an existing package and requires its independently retained manifest SHA-256. Direct lanes are also available, e.g. `bundle exec fastlane ios package version:3.2 build:70 run_id:ios-70`. Fastlane uses `mac`, while the CLI uses `macos`.

Output is `.artifacts/release/<platform>/<run-id>/`. A fresh package refuses an existing attempt directory. A failed build retains its diagnostics; use a new run ID after fixing the cause. Packaging requires a clean committed checkout and binds SHA, tree, version, build, artifact sizes and SHA-256 digests. Upload checks this binding again. Never use a manifest hash taken from an untrusted download as the expected value.

iOS accepts explicit numeric version/build overrides without modifying the checked-in project. Signing adjustments happen only in an attempt-owned copy of tracked iOS inputs. Chromium version/build values come from committed native source and cannot be changed by renaming a package. `CHROMIUM_ROOT` must point to a dedicated, already synchronized and patched workspace; the lane does not reset or fetch a shared checkout. Existing build-source checks, package guards and a release lock remain in effect.

## Local configuration

Configure secrets in the process environment or a secret manager. Fastlane also supports its standard local `.env` files; keep them ignored and never commit credentials. Never pass secrets as lane arguments.

| Name | Purpose |
| --- | --- |
| `AEGIS_APPLE_TEAM_ID` | Apple Developer team for IPA signing |
| `AEGIS_IOS_PROFILES` | JSON map of the three iOS bundle IDs to installed **App Store** profile names |
| `DEVELOPER_DIR` | Optional selected Xcode; the project requires an appropriate Xcode 26 installation |
| `CHROMIUM_ROOT`, `DEPOT_TOOLS_DIR` | Dedicated prepared Chromium workspace and depot_tools |
| `AEGIS_AAPT2`, `AEGIS_APKSIGNER` | Android SDK tool paths, or `aapt2`/`apksigner` on PATH |
| `GH_TOKEN` | Read access to upstream source and Actions; additionally Contents write for GitHub Draft |
| `AEGIS_ASC_KEY_ID`, `AEGIS_ASC_ISSUER_ID`, `AEGIS_ASC_KEY_BASE64` | App Store Connect API key, issuer and Base64-encoded `.p8`, used only for TestFlight upload |

Example profile mapping (use actual installed profile names):

```json
{"com.gcsa.aegis":"match AppStore com.gcsa.aegis","com.gcsa.aegis.safari":"match AppStore com.gcsa.aegis.safari","com.gcsa.aegis.share":"match AppStore com.gcsa.aegis.share"}
```

Both uploads require the package commit to equal current upstream `main` and its latest `quality.yml` push run/attempt to contain successful `quality` and `quality-gate` jobs. A main advance requires rebinding the candidate; local checks do not replace this admission.

GitHub Draft additionally requires an **existing upstream tag** `candidate/<platform>/...` resolving to that exact commit. The script never creates/moves tags, publishes a draft, alters a public release, or overwrites assets. Repeating an interrupted upload verifies existing remote asset digests before filling missing assets. Candidate tags are separate from the existing formal `vX.Y.Z.N` release pipeline and its three-platform qualification requirements.

TestFlight uses Fastlane's API-key upload with external distribution and submission disabled. Its receipt states `uploaded_processing_unverified`: App Store Connect processing, tester availability and device acceptance require separate readback. If the process stops during upload, inspect App Store Connect before retrying the same build number.

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

The macOS driver currently produces an **ad-hoc signed, unnotarized** DMG. Android retains the existing Chromium build signature and is **not distribution-qualified**. The draft records these facts. Developer ID signing/notarization, production Android signing continuity, Play/AAB delivery, device acceptance and public release remain separate work. This entrypoint does not change the formal release validator or mark these gates passed.

```bash
bundle exec ruby fastlane/tests/release_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml
```

The tests use disposable repositories, ZIP fixtures and simulated GitHub responses; they do not sign, archive, upload or establish device evidence. References: [Fastlane build_app](https://docs.fastlane.tools/actions/build_app/), [TestFlight upload](https://docs.fastlane.tools/actions/upload_to_testflight/), [GitHub environment secrets](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets).
