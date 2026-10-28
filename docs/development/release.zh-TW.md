[English](release.md) | [简体中文](release.zh-CN.md) | [繁體中文](release.zh-TW.md)

# 本機 Fastlane 與上游發佈工作流程

根目錄 `./release` 統一產生 iOS IPA、Android arm64 APK/AAB、macOS arm64 DMG，參考 HomeGuard iOS 分離打包與上傳的做法，沿用 Aegis 自己的工程、建置腳本及簽署身分。`plan` 只需要 Ruby 與 Git；執行 lane 需要 Ruby 3.3+、Bundler 及 Gemfile.lock，先執行 `bundle install`。CI 固定 Ruby 3.3.6、Bundler 2.5.22。

| 平台 | 應用程式識別碼 | 建置主機 | 上傳目的地 |
| --- | --- | --- | --- |
| iOS | `com.gcsa.aegis` | macOS / Xcode | TestFlight、ASC/App Review 或 GitHub 候選草稿 |
| Android | `com.gcsa.aegis` | Linux x86-64 / 已準備的 Chromium 工作區 | Play 或 GitHub 候選草稿 |
| macOS | `com.gcsa.aegis` | Apple Silicon / 已準備的 Chromium 工作區 | GitHub 候選草稿 |

iOS 延伸功能使用 `com.gcsa.aegis.safari`、`com.gcsa.aegis.share`。保留 App Group `group.com.gcsa.aegis.ios.share-inbox` 與 Keychain service，新 profile 需要相應 App Group 能力。識別碼不同於歷史開發套件，尚未證明舊資料遷移或覆蓋升級。Android 獨立 QA driver 與 Windows 識別碼不變。

## 手動腳本：第一階段驗收

在終端執行 `./release` 可使用選單，也可使用命令列。`--profile alpha|beta|release` 與目的地獨立選擇。三種 profile 都使用最佳化的 Release 建置，不啟用 Debug 測試掛鉤；iOS 把 `AegisReleaseProfile` 寫入主應用和擴充，Android/macOS 將 profile 綁定到產物清單，不虛構功能差異。Alpha 原包不能靠修改清單變成 Release 包。

| 階段 | 行為 |
| --- | --- |
| `plan` | 只顯示原始碼、profile、版本和目的地 |
| `package` | 本地打包、簽章、驗證，不需要商店上傳憑證 |
| `upload` | 重用原包；Apple 只上傳，Play 只建立 draft |
| `release` | package + upload，不隱式分發或送審 |
| `distribute` | 將精確版本分配到已有 TF 群組，或提交 Play 測試軌道 |
| `submit` | 明確的正式送審步驟，必須使用 release profile |
| `status` | 唯讀商店狀態及本地階段紀錄 |

| 平臺 | 目的地 |
| --- | --- |
| iOS | `artifacts`、`testflight`（只上傳）、`testflight-internal`、`testflight-external`、`asc`（只上傳）、`app-store`、`github-draft` |
| Android | `artifacts`、`play-internal`、`play-closed`（預設 alpha，可用 `--track` 指定已有封閉軌道）、`play-open`（beta）、`play-production`、`github-draft` |
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

`upload/distribute/submit` 必須帶打包時獨立保留的 manifest SHA-256，並保持原始碼、profile、version、build、format 一致。產物目錄為 `.artifacts/release/<platform>/<run-id>/`，新打包拒絕覆盖既有目錄。直接 Fastlane 也支持這些階段；CLI 的 macos 對應 Fastlane 的 mac。

每次外部寫入先写 `*-started.json`，回讀成功再写 `*-result.json`。程序鎖防止同一包被兩個終端同時操作。完成且參數相同的階段可幂等重用；結果未知時禁止自動重試。`status` 不刪除標記，也不自動授權再次上傳。先在 ASC/Play 核對真實狀態，不要靠删 intent 绕過檢查。

本地手動腳本接受操作者選擇的乾淨已提交分支，包括 fork 測試分支。GitHub Actions 管理自己的上游品質准入；上傳上游 GitHub Draft 也保留该准入。用 `--repository owner/fork` 显式選擇 fork 草稿演練，不改变上游正式發行策略。Draft 必須對應已存在且指向包原始碼 SHA 的 `candidate/<platform>/...` tag；腳本不會移动 tag、公開草稿或覆盖已有資產，重試前回讀遠端摘要。

iOS 可在本次原始碼副本中覆盖數字 version/build。Chromium 的版本及 versionCode 必須事先寫入並提交原生输入，不能只改包名；目前 `0.8.0 (1)` 是 iOS 測試身份。`CHROMIUM_ROOT` 需要獨佔、已同步並應用修補的工作區，腳本不重置或拉取共用原始碼。

TF 內外测要求精確的已有群組名、符合的群組類型，以及處理完成且未過期的 build。外测會請求 Beta App Review，預設不通知外部測試者。上傳成功僅紀錄 `uploaded_processing_unverified`。ASC 正式送審要求預先準備對應版本、商店資料、隱私/審核資料和合規回答；腳本不猜測這些答案。它显式設定手動發布，提交精確 build，並回讀審核狀態，不自動發布通過審核的 App Store 版本。

Play 預設產生使用上傳金鑰簽章的 AAB；`--format apk` 保留現有側載路徑。upload 只建立 draft 並保留原 completed 版本；distribute/submit 提交选定軌道並請求審核，Google 可能仍要求 Console 操作。edits API 不能證明審核通過或測試者可用。腳本拒絕隱式改动其他 draft/正在進行的 rollout，也不會自動重試成另一種送審語義。正式 submit 必須显式指定 `--play-release-policy managed`（先在 Console 核對受管理發布）或 `automatic`（接受審核後自動發布）；這個參數紀錄操作者選擇，不代表 API 已驗證或修改该設定。首次 Play 應用及包可能需要先在 Console 完成初始化。

## 本地配置

使用程序環境或被忽略的 Fastlane `.env`，不要把憑證作為 lane 參數或寫入清單。使用 Ruby 3.3.6、Bundler 2.5.22 和鎖定依赖；使用 vendor bundle 時統一設定 `BUNDLE_PATH=vendor/bundle`。以下參數名與英文文件一致：

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

## GitHub Actions

`.github/workflows/release.yml` 僅手動執行，限定 `gcsagroup/aegis-browser`、儲存庫 ID `1348409663` 的 `main`，以 `github.sha` 固定來源，沒有任意分支/SHA 輸入。儲存庫變數 `AEGIS_RELEASE_ENABLED=true` 才啟用。

建立限制 `main`、要求人工審核的受保護 environments：

| Environment | 設定 |
| --- | --- |
| `aegis-release-ios` | 變數 `AEGIS_APPLE_TEAM_ID`、`AEGIS_IOS_PROFILES`、`AEGIS_MATCH_GIT_URL`、選用 `AEGIS_DEVELOPER_DIR`；secrets `MATCH_PASSWORD`、`MATCH_GIT_BASIC_AUTHORIZATION` |
| `aegis-release-macos` | 變數 `AEGIS_CHROMIUM_ROOT`、`AEGIS_DEPOT_TOOLS_DIR`、選用 `AEGIS_DEVELOPER_DIR` |
| `aegis-release-android` | 變數 `AEGIS_CHROMIUM_ROOT`、`AEGIS_DEPOT_TOOLS_DIR`、`AEGIS_AAPT2`、`AEGIS_APKSIGNER` |
| `aegis-testflight` | 三個 `AEGIS_ASC_*` secrets |
| `aegis-github-draft` | 審核擁有 Contents 寫入權限的上傳 job |

iOS 在 `macos-26` 使用暫時 CI keychain，以 `match(readonly: true)` 讀取既有簽署材料；不建立 app ID、profile 或憑證。啟用前另外準備簽署儲存庫與 ASC app，Match 憑證僅授予簽署儲存庫讀取權限。

Chromium 專用 runner 標籤為 `[self-hosted, macOS, ARM64, aegis-release]`、`[self-hosted, Linux, X64, aegis-release]`；須具備足夠磁碟、Git、Python 3.11+、Node、Ruby 3.3.6、Bundler 2.5.22、原生工具鏈及已準備原始碼。不可復用 PR runner 或活動中的開發工作區。實際驗證後設定 `AEGIS_CHROMIUM_RELEASE_RUNNERS_READY=true`。YAML 不證明主機或環境保護已設定。

無簽署憑證的 preflight 先執行；打包 job 僅 Contents 讀取。上傳 job 只下載同次執行的 artifact，以打包 job 直接提供的 manifest 摘要驗證。僅 GitHub 上傳 job 有 Contents 寫入。白名單排除 keychain、profile、建置工作目錄及憑證。

## 驗證邊界

合入前可推送 `codex/fastlane-release-*` 分支執行 `Release Branch Validation`。流程可在 fork 或上游分支執行無憑據回歸，並編譯 iOS `0.8.0 (1)` 的未簽名 archive，核對主 App 與兩個擴充功能的識別資訊。它不存取發布 Environment 或簽名 Secrets；結果不代表 IPA、TestFlight 或三端發布驗收。正式簽名打包、上傳及後續 Android → DMG 驗收仍須分別完成。

本機真實打包及上傳、Actions、候選 tag 草稿、商店狀態和真機驗收分別紀錄。模擬通過不能替代真實建置；Chromium 主機、Play 初始化與上傳金鑰、Developer ID/公證仍需對應環境。

```bash
bundle exec ruby fastlane/tests/stores_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml
```

回歸使用暫存倉庫、Apple/Play/GitHub 模擬回應，並以臨時金鑰實際簽章及驗證合成 bundle；不會上傳真實應用或建立實機證據。macOS 需設定 JAVA_HOME 指向完整 JDK 17+。參考：[Fastlane build_app](https://docs.fastlane.tools/actions/build_app/)、[TestFlight 上傳](https://docs.fastlane.tools/actions/upload_to_testflight/)、[GitHub 環境憑證](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)。
