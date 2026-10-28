[English](release.md) | [简体中文](release.zh-CN.md) | [繁體中文](release.zh-TW.md)

# 本地 Fastlane 与上游发布工作流

根目录 `./release` 统一生成 iOS IPA、Android arm64 APK、macOS arm64 DMG。参考 HomeGuard iOS，将打包与上传分开，但使用 Aegis 自己的工程、构建脚本和签名身份。`plan` 只需要 Ruby 和 Git；执行 lane 需要 Ruby 3.3+、Bundler 和已提交的 Gemfile.lock。CI 固定 Ruby 3.3.6、Bundler 2.5.22，先执行 `bundle install`。

| 平台 | 应用标识 | 构建宿主 | 上传目的地 |
| --- | --- | --- | --- |
| iOS | `com.gcsa.aegis` | macOS / Xcode | TestFlight 或 GitHub 候选草稿 |
| Android | `com.gcsa.aegis` | Linux x86-64 / 已准备的 Chromium 工作区 | GitHub 候选草稿 |
| macOS | `com.gcsa.aegis` | Apple Silicon / 已准备的 Chromium 工作区 | GitHub 候选草稿 |

iOS 扩展为 `com.gcsa.aegis.safari`、`com.gcsa.aegis.share`。保留现有 App Group `group.com.gcsa.aegis.ios.share-inbox` 和 Keychain service；新应用及扩展的 profile 需具有对应 App Group 能力。标识不同于历史开发包，尚未证明旧应用资料迁移或覆盖升级。Android 独立 QA driver 与 Windows 标识保持现状。

## 本地使用

```bash
./release ios plan --destination testflight --version 3.2 --build 70
./release ios package --version 3.2 --build 70 --run-id ios-70
./release ios upload --destination testflight --version 3.2 --build 70 \
  --run-id ios-70 --manifest-sha256 <package输出的摘要>

./release macos plan --destination github-draft --tag candidate/macos/2.2.0.144
./release macos release --destination github-draft \
  --tag candidate/macos/2.2.0.144 --run-id mac-candidate-144
./release android release --destination github-draft \
  --tag candidate/android/2.2.0.144 --run-id android-candidate-144
```

`package` 默认只保存本地产物；`release` 依次打包和上传；`upload` 复用原包，必须提供独立保留的 manifest SHA-256。直接 Fastlane 入口例如 `bundle exec fastlane ios package version:3.2 build:70 run_id:ios-70`，macOS lane 名为 `mac`。

产物位于 `.artifacts/release/<platform>/<run-id>/`。打包拒绝复用已有目录，失败现场保留，修复后使用新 run ID。清单绑定干净已提交源码的 SHA/tree、版本、build、产物大小与 SHA-256，上传前重新核对。不能把不可信下载附带的摘要直接当作预期摘要。

iOS 可显式传入数字 version/build，签名配置只修改本次目录中的受跟踪源码副本。Chromium 版本必须与已提交的原生源码一致，不能靠文件名改版本。`CHROMIUM_ROOT` 必须是独占、已同步并应用补丁的工作区；脚本不重置或拉取共享源码，保留现有源码校验、打包保护及发布锁。

## 本地配置

凭据通过进程环境或密钥管理工具注入，不作为 lane 参数。Fastlane 也支持其标准本地 `.env` 文件；这些文件须保持忽略，不能提交凭据。

| 变量 | 用途 |
| --- | --- |
| `AEGIS_APPLE_TEAM_ID` | IPA 签名的开发者 Team |
| `AEGIS_IOS_PROFILES` | 三个 iOS bundle ID 到已安装 **App Store** profile 名称的 JSON 映射 |
| `DEVELOPER_DIR` | 可选，选择适用的 Xcode 26 |
| `CHROMIUM_ROOT`、`DEPOT_TOOLS_DIR` | 独占 Chromium 工作区与 depot_tools |
| `AEGIS_AAPT2`、`AEGIS_APKSIGNER` | Android SDK 工具路径；也可在 PATH 提供 `aapt2`、`apksigner` |
| `GH_TOKEN` | 上游源码及 Actions 读权限；GitHub Draft 另需 Contents 写权限 |
| `AEGIS_ASC_KEY_ID`、`AEGIS_ASC_ISSUER_ID`、`AEGIS_ASC_KEY_BASE64` | 仅 TestFlight 上传使用的 ASC API Key ID、Issuer 和 Base64 `.p8` |

profile 映射示例，须替换为真实已安装的名称：

```json
{"com.gcsa.aegis":"match AppStore com.gcsa.aegis","com.gcsa.aegis.safari":"match AppStore com.gcsa.aegis.safari","com.gcsa.aegis.share":"match AppStore com.gcsa.aegis.share"}
```

两个外部目的地都要求包的源码等于当前上游 `main`，并核对该 SHA 最新 `quality.yml` push run/attempt 的 `quality`、`quality-gate` 均成功。main 推进后需重新绑定候选，本地测试不能代替此准入。

GitHub Draft 还要求上游已存在指向同一 SHA 的 `candidate/<platform>/...` tag。脚本不创建或移动 tag、不公开草稿、不修改已公开 Release、不覆盖资产。中断后重试会先核对远端已有资产摘要，再补传缺项。候选 tag 与现有 `vX.Y.Z.N` 正式三平台发行门禁分开。

TestFlight 上传关闭 external distribution 和 submission。成功记录为 `uploaded_processing_unverified`，ASC 处理结果、测试者可见性和真机状态仍需独立回读。上传中断时先检查 ASC，不能盲目重传同一 build。

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

当前 macOS DMG 为 **临时签名、未公证**；Android 沿用现有 Chromium 构建签名，**未获正式分发资格**。草稿明确记录这些状态。Developer ID/公证、Android 正式签名连续性、Play/AAB、真机验收和公开发布仍需独立完成；本入口不改动正式发布验证器或把未通过的门禁标为通过。

```bash
bundle exec ruby fastlane/tests/release_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml
```

测试使用临时 Git 仓库、ZIP 和模拟 GitHub 响应，不执行真实签名、archive、上传或真机操作。参考：[Fastlane build_app](https://docs.fastlane.tools/actions/build_app/)、[TestFlight 上传](https://docs.fastlane.tools/actions/upload_to_testflight/)、[GitHub 环境凭据](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)。
