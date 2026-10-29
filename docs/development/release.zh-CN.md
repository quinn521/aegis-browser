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

在终端运行 `./release` 可使用菜单，也可使用命令行。`--profile alpha|beta|release` 选择包类型。Alpha 的商店分发只允许 TestFlight 内测组或 Play 内部测试；在打包或读取发布凭据前拒绝 TestFlight 外测、Play 封闭/公开测试及正式渠道。iOS Alpha 导出始终设置 `testFlightInternalTestingOnly=true`，包 only 和 GitHub Draft 也一样，Apple 因而限制其外测及 App Store 送审；Beta/Release 设为 false。Beta 可内外测但禁止正式送审；Release 可用于所有列出的目的地，上传、分发和送审仍是分别显式执行的阶段。默认目的地仍为 `artifacts`（不上传）；`testflight` 只上传，分发必须显式指定已有内测组名称。外测需重新打 Beta 或 Release 包并使用新 build。参见 [Apple 分发选项](https://developer.apple.com/documentation/xcode/distributing-your-app-for-beta-testing-and-releases)。三种 profile 都使用优化的 Release 构建，不启用 Debug 测试钩子；iOS 把 `AegisReleaseProfile` 写入主应用和扩展，Android/macOS 将 profile 绑定到产物清单，不虚构功能差异。Alpha 原包不能靠修改清单变成 Release 包。

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

# 使用正式目的地前，先生成并上传新的 Release-profile 包。
./release ios submit --profile release --destination app-store --version 0.8.0 --build 2 \
  --run-id ios-release-2 --manifest-sha256 <retained-release-package-digest>

# Android 原生版本与 versionCode 必须已写入源码并提交。
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

本地打包入口会在 archive 前检查 Safari manifest 图标；archive/IPA 验证还会核对实际打入的 PNG 资源和尺寸。缺少图标（App Store 错误 90849）会在本地上传前失败。

iOS 可在本次源码副本中覆盖数字 version/build。Chromium 的版本及 versionCode 必须事先写入并提交原生输入，不能只改包名；当前 `0.8.0 (1)` 是 iOS 测试身份。`CHROMIUM_ROOT` 需要独占、已同步并应用补丁的工作区，脚本不重置或拉取共享源码。

Android 的源码校验、包名和原生构建身份读取 `apps/browser/ci-pins/android.json` 及其补丁目录。桌面仍使用 `CHROMIUM_VERSION`、`CHROMIUM_COMMIT` 和 `patches/`。桌面工作区不能替代 Android 的独立基线。Android 验收要求 x86-64 Linux 和至少 200 GB 可用空间；ARM64 VM 中的 x86-64 仿真容器不满足该门禁。

TF 内外测要求精确的已有组名、匹配的组类型，以及处理完成且未过期的 build。外测会请求 Beta App Review，默认不通知外部测试者。上传成功仅记录 `uploaded_processing_unverified`。ASC 正式送审要求预先准备对应版本、商店资料、隐私/审核资料和合规回答；脚本不猜测这些答案。它显式设置手动发布，提交精确 build，并回读审核状态，不自动发布通过审核的 App Store 版本。

Play 默认生成使用上传密钥签名的 AAB；`--format apk` 保留现有侧载路径。upload 只创建 draft 并保留原 completed 版本；distribute/submit 提交选定轨道并请求审核，Google 可能仍要求 Console 操作。edits API 不能证明审核通过或测试者可用。脚本拒绝隐式改动其他 draft/正在进行的 rollout，显式使用 ERROR_IF_IN_REVIEW 避免取消其他审核，也不会自动重试成另一种送审语义。正式 submit 必须显式指定 `--play-release-policy managed`（先在 Console 核对受管理发布）或 `automatic`（接受审核后自动发布）；这个参数记录操作者选择，不代表 API 已验证或修改该设置。首次 Play 应用及包可能需要先在 Console 完成初始化。

分发和送审会先检查该 build 的出口合规声明。缺少声明时停止，由应用所有者在 ASC 完成填写；脚本不默认填写“无非豁免加密”。TestFlight 分发显式指定 iOS，无需交互输入，也不重复进入 Pilot 的处理等待。

## 本地配置

使用进程环境或被忽略的 Fastlane `.env`，不要把凭据作为 lane 参数或写入清单。使用 Ruby 3.3.6、Bundler 2.5.22 和锁定依赖；使用 vendor bundle 时统一设置 `BUNDLE_PATH=vendor/bundle`。以下参数名与英文文档一致：

| 变量 | 用途 |
| --- | --- |
| `AEGIS_APPLE_TEAM_ID`, `AEGIS_IOS_PROFILES` | 签名团队，以及三个 bundle ID 到已安装 App Store profile 名称的 JSON 映射 |
| `AEGIS_ASC_KEY_ID`, `AEGIS_ASC_ISSUER_ID` | ASC API 身份 |
| `AEGIS_ASC_KEY_PATH` or `AEGIS_ASC_KEY_BASE64` | 私有 `.p8` 文件（权限 600）或 Base64 secret；仅上传、分发、送审和查询使用 |
| `CHROMIUM_ROOT`, `DEPOT_TOOLS_DIR`, `DEVELOPER_DIR` | 已准备的原生构建工作区和工具链 |
| `AEGIS_AAPT2`, `AEGIS_APKSIGNER` | APK 验证工具，默认从 PATH 查找 |
| `AEGIS_BUNDLETOOL_JAR`, JDK 17+ on PATH | AAB 身份验证；提供官方 bundletool JAR |
| `AEGIS_ANDROID_KEYSTORE`, `AEGIS_ANDROID_KEY_ALIAS`, `AEGIS_ANDROID_CERT_SHA256` | 私有 Play 上传 keystore、别名和预期证书指纹 |
| `AEGIS_ANDROID_STORE_PASSWORD`, `AEGIS_ANDROID_KEY_PASSWORD` | 通过环境传给 Java 的密码，不放入命令行参数 |
| `AEGIS_PLAY_JSON_KEY` | 私有 Play 服务账号 JSON（权限 600），仅授予所需应用和轨道权限 |
| `GH_TOKEN` | 上游准入查询使用读权限；仅所选 GitHub Draft 仓库需要 Contents 写权限 |

```json
{"com.gcsa.aegis":"Aegis AppStore","com.gcsa.aegis.safari":"Aegis Safari AppStore","com.gcsa.aegis.share":"Aegis Share AppStore"}
```

请使用实际安装的 profile 名称。仅打包需要签名材料，无需 ASC/Play 发布凭据。Android 用预期上传证书核验每个已签名文件，再核对最终 AAB 身份。原生 build-identity 描述签名前快照，release manifest 绑定最终签名字节。APK 保留 Chromium 签名，未取得 Play 发布资格。DMG 签名遵循 `apps/browser/scripts/sign-chromium-app.sh`。发布入口先真实签名小型 Mach-O，再启动长时间构建；最终 DMG 必须通过同一证书、Apple 根锚和团队校验。需解锁已配置的钥匙串，不导出私钥。这不代表 Developer ID 或公证验收。

## GitHub Actions：第二阶段验收

`release.yml` 支持手动触发及供 tag 工作流复用。选择平台、profile、目的地和最后阶段（package/upload/distribute/submit），默认只打包。`store_options` 只接受字符串 groups、track、play_release_policy，例如 `{"groups":"Existing Internal Group"}`，不能传凭据。上游仍要求仓库 ID 1348409663、当前 main 的精确 SHA、最新质量检查成功及 `AEGIS_RELEASE_ENABLED=true`。

无签名凭据的 preflight 先核对源码和路由；打包 job 只有 Contents 读权限。上传只接收同一次运行的 artifact，并使用打包 job 直接传递的 manifest SHA-256 和 run ID。分发/送审还要下载成功的上传记录，放在独立受保护 job。Apple 最多等待处理 30 分钟，始终选择精确版本。只有 GitHub Draft job 有 Contents 写权限。产物白名单不包含 profile、keychain、凭据或 work 目录。

受保护 environments 应设置审核人，并限制 main 和计划使用的 candidate tags：

| Environment | 配置 |
| --- | --- |
| `aegis-release-ios` | 变量 `AEGIS_APPLE_TEAM_ID`、`AEGIS_IOS_PROFILES`、`AEGIS_MATCH_GIT_URL`，可选 `AEGIS_DEVELOPER_DIR`；secrets `MATCH_PASSWORD`、`MATCH_GIT_BASIC_AUTHORIZATION` |
| `aegis-release-macos` | 变量 `AEGIS_CHROMIUM_ROOT`、`AEGIS_DEPOT_TOOLS_DIR`、`AEGIS_LOCAL_MACOS_BUNDLE_PATH`、`AEGIS_CIPD_CACHE_DIR`、`AEGIS_VPYTHON_VIRTUALENV_ROOT`、`AEGIS_NATIVE_TMPDIR`，可选 `AEGIS_DEVELOPER_DIR`；使用已安装的原生签名身份和钥匙串；不接收 ASC/Play/Android 商店 Secrets |
| `aegis-release-android` | Chromium/depot_tools、bundletool、上传 keystore 路径/别名/证书 SHA-256 变量；secrets `AEGIS_ANDROID_STORE_PASSWORD`、`AEGIS_ANDROID_KEY_PASSWORD`；runner 安装 SDK/JDK |
| `aegis-testflight` | ASC key ID、issuer 和 Base64 私钥 secrets；仅上传 |
| `aegis-play` | Secret `AEGIS_PLAY_JSON_KEY_DATA`；仅上传草稿 |
| `aegis-ios-distribute`, `aegis-ios-submit` | ASC secrets；测试分发与 App Review 单独审核 |
| `aegis-android-distribute`, `aegis-android-submit` | Play JSON secret；测试与正式送审单独审核 |
| `aegis-github-draft` | 审核该 job 的 Contents 写权限使用 |

iOS 使用 `match(readonly: true)` 将已有签名材料安装到临时 CI keychain；应先准备签名仓库、三个 profiles 和 ASC 应用。CI 不创建新证书或应用标识。Play JSON 仅写入本次私有临时目录，子进程退出后删除，并从子进程环境移除 JSON 内容。

Chromium 专用 runner 标签为 `[self-hosted, macOS, ARM64, aegis-release]` / `[self-hosted, Linux, X64, aegis-release]`。需要独占工作区、足够磁盘、Git、Python 3.11+、Node 22.23.1、Ruby 3.3.6、Bundler 2.5.22、SDK/JDK 等工具。分别验证后设置 `AEGIS_MACOS_RELEASE_RUNNERS_READY=true` 或 `AEGIS_ANDROID_RELEASE_RUNNERS_READY=true`。兼容旧的 `AEGIS_CHROMIUM_RELEASE_RUNNERS_READY`，但显式的平台 `false` 优先；启用 Mac 不会启用 Android。YAML 不代表服务端配置已就绪。

macOS self-hosted 路径检查预装运行时、同 SHA 干净源码及各工作区/缓存目录的所有权和写权限，复用固定 Bundler 缓存，不运行 hosted 工具缓存安装。先通过真实小型 DMG 的挂载/签名/损坏回归，再调用相同的本地打包入口。最终 DMG 必须通过只读挂载、镜像内 `com.gcsa.aegis` 产品版本、arm64 和深度签名验证，并把已验证镜像的摘要写入 release manifest。该检查不建立浏览器运行或公证证据。

fork 分支打包还需按平台显式启用 `AEGIS_IOS_BRANCH_REHEARSAL=true` 或 `AEGIS_MACOS_BRANCH_REHEARSAL=true`；它们不影响候选 tag 入口。Android 暂缓时保持其 ready 为 `false`，macOS 可独立完成本地 → Actions → candidate tag/Draft 验收。

Xcode 26 的 Metal 编译器可能需要单独安装：`xcodebuild -downloadComponent MetalToolchain`。本地打包和 macOS Actions 都先运行 `python3 scripts/release/verify-macos-toolchain.py`，实际把最小 shader 编译成 AIR；`xcrun --find metal` 只能找到占位入口，不能证明组件可用。缺少组件或编译失败会在创建打包目录和启动 Chromium 编译前停止。

fork 演练需显式设置 `AEGIS_RELEASE_ENABLED=true`、`AEGIS_RELEASE_REHEARSAL=true`、`AEGIS_RELEASE_TEST_REPOSITORY_ID=<fork 数字 ID>`，并准备该 fork 自己的受保护签名环境。API 核对 parent 为上游、同 SHA 最新分支验证 attempt 的 contracts/ios-archive 成功，只允许 codex/fastlane-release-* 分支或 candidate tag。目的地限包 only、TF 上传/内测、Play 内测、该 fork 自己的 Draft；拒绝正式版及外测。分支验证可在两个无凭据检查完成后调用真正的打包工作流，无需先合并到默认分支。未启用时 package rehearsal 为 skipped，不能算打包通过；fork_rehearsal_only 也不等于上游准入。

Actions rerun 可以重打包，或通过远端摘要验证续传 GitHub 草稿；不会在 attempt 1 之后重放 Apple/Play 写操作，避免 runner 在商店接受请求后丢失回执导致重复上传。先核对保存的阶段记录和商店状态，再本地处理或使用新 build/run。原生版本约束仍然适用。

fork 的 iOS 打包/Draft 演练可额外设置 `AEGIS_IOS_LOCAL_REHEARSAL=true`。通过同一无签名凭据的 preflight 后，只选择 `[self-hosted, macOS, ARM64, aegis-ios-release-rehearsal]`，拒绝上游、其他分支和商店目的地。为单个 job 注册任务独占的一次性 Runner，不给共享质量 Runner 添加发布标签。受保护的 `aegis-release-ios` 环境提供现有团队/profile 映射和任务自有缓存 `AEGIS_LOCAL_IOS_BUNDLE_PATH`。提前安装固定版本的 Ruby/Node/Bundler 和签名材料，并在本地解锁钥匙串。该路径不导出私钥，也不接收 ASC/Play/Match 凭据。默认仍使用 hosted Match 路由；不能仅填写其他仓库地址就读取其 Secrets。

## Candidate tag → Draft Release：第三阶段验收

`release-candidate-tag.yml` 解析平台专属候选 tag，再调用相同 release 工作流，固定 github-draft + upload。它不响应正式 v* tag，不上传商店，不公开 Release。上游仍要求 tag 指向当前 main 且质量检查成功；fork 走独立演练准入。

```bash
ruby scripts/release/tag.rb candidate/ios/0.8.0-alpha.1
ruby scripts/release/tag.rb candidate/android/6.1.0.266-beta.2006000266
ruby scripts/release/tag.rb candidate/macos/6.1.0.266-release.266
```

这些命令只做本地模拟，不创建 tag 或上传。tag 编码平台、profile、产品版本与 build/versionCode；本地和 Actions 对应平台实测通过后才创建/推送。脚本回读 tag SHA 与每个远端资产摘要，保留 draft/prerelease，不覆盖已有资产。

Draft 创建后使用 REST 响应中的 release ID，并在上传资产前保存创建意图和结果；后续按 ID 回读，不依赖新草稿立即出现在列表中。创建结果未知时禁止再次 POST；Actions 重跑可恢复已有草稿，但在既无可见草稿又无创建记录时不能新建替代草稿。CI 在失败时也保留创建记录，恢复时不覆盖既有 Tag 或资产。

## 验证范围

合入前可推送 `codex/fastlane-release-*` 分支运行 `Release Branch Validation`。前两个 job 在 fork 或上游分支运行无凭据回归测试，并编译 iOS `0.8.0 (1)` 的未签名 archive，核对主应用和两个扩展的 Bundle ID、version、build。它们不访问发布 Environment 或签名 Secrets；按平台启用的 fork rehearsal 可在其后执行真实打包。前两项诊断不属于 IPA、TestFlight 或三端完整发布验收；每个平台的本地真包、Actions 和候选 Draft 分别验收。

独立 iOS 验证使用 `codex/fastlane-release-ios-*` 分支。推送只运行无凭据检查，不排队平台实包；本地验收后，对同一分支手动触发 `release-validation.yml`，设置 `platform=ios`、`ios_version` 和 `ios_build`，只打 iOS 包且不上传商店。发布并发按仓库和平台区分，独立的 macOS 流程不会占住 iOS 队列。原有 fork 显式启用、精确源码准入及受保护签名环境门禁继续生效。

本地真包及上传、Actions、候选 tag 草稿、商店状态和真机验收分别记录。模拟通过不能替代真实构建；Chromium 宿主、Play 初始化与上传密钥、Developer ID/公证仍需对应环境。

```bash
bundle exec ruby fastlane/tests/tag_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml .github/workflows/release-validation.yml .github/workflows/release-candidate-tag.yml
```

回归使用临时仓库、Apple/Play/GitHub 模拟响应，并用临时密钥实际签名和验证合成 bundle；不会上传真实应用或建立真机证据。macOS 需设置 JAVA_HOME 指向完整 JDK 17+。参考：[Fastlane build_app](https://docs.fastlane.tools/actions/build_app/)、[TestFlight 上传](https://docs.fastlane.tools/actions/upload_to_testflight/)、[GitHub 环境凭据](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)。
