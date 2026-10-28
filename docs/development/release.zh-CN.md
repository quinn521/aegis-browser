[English](release.md) | [简体中文](release.zh-CN.md) | [繁體中文](release.zh-TW.md)

# 本地 Fastlane 与上游发布工作流

根目录 `./release` 统一生成 iOS IPA、Android arm64 APK/AAB、macOS arm64 DMG。参考 HomeGuard iOS，将打包与上传分开，但使用 Aegis 自己的工程、构建脚本和签名身份。`plan` 只需要 Ruby 和 Git；执行 lane 需要 Ruby 3.3+、Bundler 和已提交的 Gemfile.lock。CI 固定 Ruby 3.3.6、Bundler 2.5.22，先执行 `bundle install`。

| 平台 | 应用标识 | 构建宿主 | 上传目的地 |
| --- | --- | --- | --- |
| iOS | `com.gcsa.aegis` | macOS / Xcode | TestFlight、ASC/App Review 或 GitHub 候选草稿 |
| Android | `com.gcsa.aegis` | Linux x86-64 / 已准备的 Chromium 工作区 | Play 或 GitHub 候选草稿 |
| macOS | `com.gcsa.aegis` | Apple Silicon / 已准备的 Chromium 工作区 | GitHub 候选草稿 |

iOS 扩展为 `com.gcsa.aegis.safari`、`com.gcsa.aegis.share`。保留现有 App Group `group.com.gcsa.aegis.ios.share-inbox` 和 Keychain service；新应用及扩展的 profile 需具有对应 App Group 能力。标识不同于历史开发包，尚未证明旧应用资料迁移或覆盖升级。Android 独立 QA driver 与 Windows 标识保持现状。

## 手动脚本：第一阶段验收

在终端运行 `./release` 可使用菜单，也可使用命令行。`--profile alpha|beta|release` 与目的地独立选择。三种 profile 都使用优化的 Release 构建，不启用 Debug 测试钩子；iOS 把 `AegisReleaseProfile` 写入主应用和扩展，Android/macOS 将 profile 绑定到产物清单，不虚构功能差异。Alpha 原包不能靠修改清单变成 Release 包。

| 阶段 | 行为 |
| --- | --- |
| `plan` | 只显示源码、profile、版本和目的地 |
| `package` | 本地打包、签名、校验，不需要商店上传凭据 |
| `upload` | 复用原包；Apple 只上传，Play 只创建 draft |
| `release` | package + upload，不隐式分发或送审 |
| `distribute` | 将精确版本分配到已有 TF 组，或提交 Play 测试轨道 |
| `submit` | 明确的正式送审步骤，必须使用 release profile |
| `status` | 只读商店状态及本地阶段记录 |

| 平台 | 目的地 |
| --- | --- |
| iOS | `artifacts`、`testflight`（只上传）、`testflight-internal`、`testflight-external`、`asc`（只上传）、`app-store`、`github-draft` |
| Android | `artifacts`、`play-internal`、`play-closed`（默认 alpha，可用 `--track` 指定已有封闭轨道）、`play-open`（beta）、`play-production`、`github-draft` |
| macOS | `artifacts`、`github-draft` |

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

`upload/distribute/submit` 必须带打包时独立保留的 manifest SHA-256，并保持源码、profile、version、build、format 一致。产物目录为 `.artifacts/release/<platform>/<run-id>/`，新打包拒绝覆盖既有目录。直接 Fastlane 也支持这些阶段；CLI 的 macos 对应 Fastlane 的 mac。

每次外部写入先写 `*-started.json`，回读成功再写 `*-result.json`。进程锁防止同一包被两个终端同时操作。完成且参数相同的阶段可幂等重用；结果未知时禁止自动重试。`status` 不删除标记，也不自动授权再次上传。先在 ASC/Play 核对真实状态，不要靠删 intent 绕过检查。

本地手动脚本接受操作者选择的干净已提交分支，包括 fork 测试分支。GitHub Actions 管理自己的上游质量准入；上传上游 GitHub Draft 也保留该准入。用 `--repository owner/fork` 显式选择 fork 草稿演练，不改变上游正式发行策略。Draft 必须对应已存在且指向包源码 SHA 的 `candidate/<platform>/...` tag；脚本不会移动 tag、公开草稿或覆盖已有资产，重试前回读远端摘要。

iOS 可在本次源码副本中覆盖数字 version/build。Chromium 的版本及 versionCode 必须事先写入并提交原生输入，不能只改包名；当前 `0.8.0 (1)` 是 iOS 测试身份。`CHROMIUM_ROOT` 需要独占、已同步并应用补丁的工作区，脚本不重置或拉取共享源码。

TF 内外测要求精确的已有组名、匹配的组类型，以及处理完成且未过期的 build。外测会请求 Beta App Review，默认不通知外部测试者。上传成功仅记录 `uploaded_processing_unverified`。ASC 正式送审要求预先准备对应版本、商店资料、隐私/审核资料和合规回答；脚本不猜测这些答案。它显式设置手动发布，提交精确 build，并回读审核状态，不自动发布通过审核的 App Store 版本。

Play 默认生成使用上传密钥签名的 AAB；`--format apk` 保留现有侧载路径。upload 只创建 draft 并保留原 completed 版本；distribute/submit 提交选定轨道并请求审核，Google 可能仍要求 Console 操作。edits API 不能证明审核通过或测试者可用。脚本拒绝隐式改动其他 draft/正在进行的 rollout，也不会自动重试成另一种送审语义。正式 submit 必须显式指定 `--play-release-policy managed`（先在 Console 核对受管理发布）或 `automatic`（接受审核后自动发布）；这个参数记录操作者选择，不代表 API 已验证或修改该设置。首次 Play 应用及包可能需要先在 Console 完成初始化。

## 本地配置

使用进程环境或被忽略的 Fastlane `.env`，不要把凭据作为 lane 参数或写入清单。使用 Ruby 3.3.6、Bundler 2.5.22 和锁定依赖；使用 vendor bundle 时统一设置 `BUNDLE_PATH=vendor/bundle`。以下参数名与英文文档一致：

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

## GitHub Actions 接入

`.github/workflows/release.yml` 仅手动触发，限定上游 `gcsagroup/aegis-browser`、仓库 ID `1348409663` 的 `main`，以 `github.sha` 固定源码。没有任意分支/SHA 输入。仓库变量 `AEGIS_RELEASE_ENABLED=true` 才启用。

先建立限制 `main` 且要求人工审核的受保护 environments：

| Environment | 配置 |
| --- | --- |
| `aegis-release-ios` | 变量 `AEGIS_APPLE_TEAM_ID`、`AEGIS_IOS_PROFILES`、`AEGIS_MATCH_GIT_URL`、可选 `AEGIS_DEVELOPER_DIR`；secrets `MATCH_PASSWORD`、`MATCH_GIT_BASIC_AUTHORIZATION` |
| `aegis-release-macos` | 变量 `AEGIS_CHROMIUM_ROOT`、`AEGIS_DEPOT_TOOLS_DIR`、可选 `AEGIS_DEVELOPER_DIR` |
| `aegis-release-android` | 变量 `AEGIS_CHROMIUM_ROOT`、`AEGIS_DEPOT_TOOLS_DIR`、`AEGIS_AAPT2`、`AEGIS_APKSIGNER` |
| `aegis-testflight` | 上述三个 `AEGIS_ASC_*` secrets |
| `aegis-github-draft` | 审核具有 Contents 写权限的上传 job |

iOS 使用 `macos-26` 临时 CI keychain，通过 `match(readonly: true)` 读取既有签名材料。它不创建 app ID、profile 或证书；启用前需单独准备签名仓库与 ASC 应用，Match 凭据仅授予签名仓库读权限。

Chromium 专用 runner 标签为 `[self-hosted, macOS, ARM64, aegis-release]` 和 `[self-hosted, Linux, X64, aegis-release]`。需足够磁盘、Git、Python 3.11+、Node、Ruby 3.3.6、Bundler 2.5.22、原生工具链和已准备源码。不能复用 PR runner 或正在使用的开发工作区。实际验收后再设置仓库变量 `AEGIS_CHROMIUM_RELEASE_RUNNERS_READY=true`。YAML 不证明 runner 或环境保护已配置。

无签名凭据的 preflight 先运行，打包 job 只有 Contents 读权限。上传为独立 job，只下载同一次运行的 artifact，并用打包 job 直接传递的 manifest 摘要核对。仅 GitHub 上传 job 有 Contents 写权限。artifact 白名单排除 keychain、profile、构建工作目录和凭据文件。

## 验证范围

合入前可推送 `codex/fastlane-release-*` 分支运行 `Release Branch Validation`。该流程在 fork 或上游分支运行无凭据回归测试，并编译 iOS `0.8.0 (1)` 的未签名 archive，核对主应用和两个扩展的 Bundle ID、version、build。它不访问发布 Environment 或签名 Secrets，输出不属于 IPA、TestFlight 或三端完整发布验收。正式签名打包、上传及 Android → DMG 的顺序验收仍需单独执行。

本地真包及上传、Actions、候选 tag 草稿、商店状态和真机验收分别记录。模拟通过不能替代真实构建；Chromium 宿主、Play 初始化与上传密钥、Developer ID/公证仍需对应环境。

```bash
bundle exec ruby fastlane/tests/stores_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml
```

回归使用临时仓库、Apple/Play/GitHub 模拟响应，并用临时密钥实际签名和验证合成 bundle；不会上传真实应用或建立真机证据。macOS 需设置 JAVA_HOME 指向完整 JDK 17+。参考：[Fastlane build_app](https://docs.fastlane.tools/actions/build_app/)、[TestFlight 上传](https://docs.fastlane.tools/actions/upload_to_testflight/)、[GitHub 环境凭据](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)。
