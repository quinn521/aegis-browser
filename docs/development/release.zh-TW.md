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

# 使用正式目的地前，先產生並上傳新的 Release-profile 包。
./release ios submit --profile release --destination app-store --version 0.8.0 --build 2 \
  --run-id ios-release-2 --manifest-sha256 <retained-release-package-digest>

# Android 原生版本與 versionCode 必須已寫入原始碼並提交。
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

本地打包入口會在 archive 前檢查 Safari manifest 圖示；archive/IPA 驗證還會核對實際打入的 PNG 資源和尺寸。缺少圖示（App Store 錯誤 90849）會在本地上傳前失敗。

iOS 可在本次原始碼副本中覆盖數字 version/build。Chromium 的版本及 versionCode 必須事先寫入並提交原生输入，不能只改包名；目前 `0.8.0 (1)` 是 iOS 測試身份。`CHROMIUM_ROOT` 需要獨佔、已同步並應用修補的工作區，腳本不重置或拉取共用原始碼。

TF 內外测要求精確的已有群組名、符合的群組類型，以及處理完成且未過期的 build。外测會請求 Beta App Review，預設不通知外部測試者。上傳成功僅紀錄 `uploaded_processing_unverified`。ASC 正式送審要求預先準備對應版本、商店資料、隱私/審核資料和合規回答；腳本不猜測這些答案。它显式設定手動發布，提交精確 build，並回讀審核狀態，不自動發布通過審核的 App Store 版本。

Play 預設產生使用上傳金鑰簽章的 AAB；`--format apk` 保留現有側載路徑。upload 只建立 draft 並保留原 completed 版本；distribute/submit 提交选定軌道並請求審核，Google 可能仍要求 Console 操作。edits API 不能證明審核通過或測試者可用。腳本拒絕隱式改动其他 draft/正在進行的 rollout，也不會自動重試成另一種送審語義。正式 submit 必須显式指定 `--play-release-policy managed`（先在 Console 核對受管理發布）或 `automatic`（接受審核後自動發布）；這個參數紀錄操作者選擇，不代表 API 已驗證或修改该設定。首次 Play 應用及包可能需要先在 Console 完成初始化。

## 本地配置

使用程序環境或被忽略的 Fastlane `.env`，不要把憑證作為 lane 參數或寫入清單。使用 Ruby 3.3.6、Bundler 2.5.22 和鎖定依赖；使用 vendor bundle 時統一設定 `BUNDLE_PATH=vendor/bundle`。以下參數名與英文文件一致：

| 變數 | 用途 |
| --- | --- |
| `AEGIS_APPLE_TEAM_ID`, `AEGIS_IOS_PROFILES` | 簽名團隊，以及三個 bundle ID 到已安裝 App Store profile 名稱的 JSON 對應 |
| `AEGIS_ASC_KEY_ID`, `AEGIS_ASC_ISSUER_ID` | ASC API 身分 |
| `AEGIS_ASC_KEY_PATH` or `AEGIS_ASC_KEY_BASE64` | 私有 `.p8` 檔案（權限 600）或 Base64 secret；僅上傳、分發、送審和查詢使用 |
| `CHROMIUM_ROOT`, `DEPOT_TOOLS_DIR`, `DEVELOPER_DIR` | 已準備的原生建置工作區和工具鏈 |
| `AEGIS_AAPT2`, `AEGIS_APKSIGNER` | APK 驗證工具，預設從 PATH 尋找 |
| `AEGIS_BUNDLETOOL_JAR`, JDK 17+ on PATH | AAB 身分驗證；提供官方 bundletool JAR |
| `AEGIS_ANDROID_KEYSTORE`, `AEGIS_ANDROID_KEY_ALIAS`, `AEGIS_ANDROID_CERT_SHA256` | 私有 Play 上傳 keystore、別名和預期憑證指紋 |
| `AEGIS_ANDROID_STORE_PASSWORD`, `AEGIS_ANDROID_KEY_PASSWORD` | 透過環境傳給 Java 的密碼，不放入命令列參數 |
| `AEGIS_PLAY_JSON_KEY` | 私有 Play 服務帳號 JSON（權限 600），僅授予所需應用程式和軌道權限 |
| `GH_TOKEN` | 上游准入查詢使用讀取權限；僅所選 GitHub Draft 儲存庫需要 Contents 寫入權限 |

```json
{"com.gcsa.aegis":"Aegis AppStore","com.gcsa.aegis.safari":"Aegis Safari AppStore","com.gcsa.aegis.share":"Aegis Share AppStore"}
```

請使用實際安裝的 profile 名稱。僅打包需要簽名材料，無需 ASC/Play 發布憑據。Android 用預期上傳憑證核驗每個已簽名檔案，再核對最終 AAB 身分。原生 build-identity 描述簽名前快照，release manifest 綁定最終簽名字節。APK 保留 Chromium 簽名，未取得 Play 發布資格。DMG 仍採用 ad-hoc 簽名，尚未公證。

## GitHub Actions：第二階段驗收

`release.yml` 支援手動觸發及供 tag 工作流程重用。選擇平台、profile、目的地和最後階段（package/upload/distribute/submit），預設只打包。`store_options` 只接受字串 groups、track、play_release_policy，例如 `{"groups":"Existing Internal Group"}`，不能傳入憑證。上游仍要求倉庫 ID 1348409663、目前 main 的精確 SHA、最新品質檢查成功及 `AEGIS_RELEASE_ENABLED=true`。

無簽章憑證的 preflight 先核對原始碼及路由；打包 job 只有 Contents 讀取權限。上傳只接收同一次執行的 artifact，並使用打包 job 直接傳遞的 manifest SHA-256 和 run ID。分發/送審還要下載成功的上傳紀錄，放在獨立受保護 job。Apple 最多等待處理 30 分鐘，始終選擇精確版本。只有 GitHub Draft job 有 Contents 寫入權限。產物白名單不包含 profile、keychain、憑證或 work 目錄。

受保護 environments 應設定審核人，並限制 main 和計畫使用的 candidate tags：

| Environment | 設定 |
| --- | --- |
| `aegis-release-ios` | 變數 `AEGIS_APPLE_TEAM_ID`、`AEGIS_IOS_PROFILES`、`AEGIS_MATCH_GIT_URL`，選填 `AEGIS_DEVELOPER_DIR`；secrets `MATCH_PASSWORD`、`MATCH_GIT_BASIC_AUTHORIZATION` |
| `aegis-release-macos` | 變數 `AEGIS_CHROMIUM_ROOT`、`AEGIS_DEPOT_TOOLS_DIR`，選填 `AEGIS_DEVELOPER_DIR` |
| `aegis-release-android` | Chromium/depot_tools、bundletool、上傳 keystore 路徑/別名/憑證 SHA-256 變數；secrets `AEGIS_ANDROID_STORE_PASSWORD`、`AEGIS_ANDROID_KEY_PASSWORD`；runner 安裝 SDK/JDK |
| `aegis-testflight` | ASC key ID、issuer 和 Base64 私鑰 secrets；僅上傳 |
| `aegis-play` | Secret `AEGIS_PLAY_JSON_KEY_DATA`；僅上傳草稿 |
| `aegis-ios-distribute`, `aegis-ios-submit` | ASC secrets；測試分發與 App Review 個別審核 |
| `aegis-android-distribute`, `aegis-android-submit` | Play JSON secret；測試與正式送審個別審核 |
| `aegis-github-draft` | 審核該 job 的 Contents 寫入權限使用 |

iOS 使用 `match(readonly: true)` 將已有簽章材料安裝到暫時 CI keychain；應先準備簽章倉庫、三個 profiles 和 ASC 應用。CI 不建立新憑證或應用識別碼。Play JSON 僅寫入本次私有暫存目錄，子程序退出後刪除，並從子程序環境移除 JSON 內容。

Chromium 專用 runner 標籤為 `[self-hosted, macOS, ARM64, aegis-release]` / `[self-hosted, Linux, X64, aegis-release]`。需要獨佔工作區、足夠磁碟、Git、Python 3.11+、Node 22.23.1、Ruby 3.3.6、Bundler 2.5.22、SDK/JDK 等工具。實際驗證後才設定 `AEGIS_CHROMIUM_RELEASE_RUNNERS_READY=true`；YAML 不代表服務端設定已就緒。

fork 演練需明確設定 `AEGIS_RELEASE_ENABLED=true`、`AEGIS_RELEASE_REHEARSAL=true`、`AEGIS_RELEASE_TEST_REPOSITORY_ID=<fork 數字 ID>`，並準備該 fork 自己的受保護簽章環境。API 核對 parent 為上游、同 SHA 最新分支驗證 attempt 的 contracts/ios-archive 成功，只允許 codex/fastlane-release-* 分支或 candidate tag。目的地限包 only、TF 上傳/內測、Play 內測、該 fork 自己的 Draft；拒絕正式版及外測。分支驗證可在兩個無憑證檢查完成後呼叫真正的打包工作流程，無需先合併到預設分支。未啟用時 package rehearsal 為 skipped，不能算打包通過；fork_rehearsal_only 也不等於上游准入。

Actions rerun 可以重新打包，或透過遠端摘要驗證續傳 GitHub 草稿；不會在 attempt 1 之後重播 Apple/Play 寫入操作，避免 runner 在商店接受請求後遺失回執而重複上傳。先核對儲存的階段紀錄及商店狀態，再本機處理或使用新 build/run。原生版本約束仍然適用。

## Candidate tag → Draft Release：第三階段驗收

`release-candidate-tag.yml` 解析平台專屬候選 tag，再呼叫相同 release 工作流程，固定 github-draft + upload。它不回應正式 v* tag，不上傳商店，不公開 Release。上游仍要求 tag 指向目前 main 且品質檢查成功；fork 走獨立演練准入。

```bash
ruby scripts/release/tag.rb candidate/ios/0.8.0-alpha.1
ruby scripts/release/tag.rb candidate/android/2.2.0.144-beta.2002000144
ruby scripts/release/tag.rb candidate/macos/2.2.0.144-release.144
```

這些命令只做本機模擬，不建立 tag 或上傳。tag 編碼平台、profile、產品版本與 build/versionCode；本機和 Actions 對應平台實測通過後才建立/推送。腳本回讀 tag SHA 與每個遠端資產摘要，保留 draft/prerelease，不覆寫已有資產。

## 驗證邊界

合入前可推送 `codex/fastlane-release-*` 分支執行 `Release Branch Validation`。前兩個 job 可在 fork 或上游分支執行無憑據回歸，並編譯 iOS `0.8.0 (1)` 的未簽名 archive，核對主 App 與兩個擴充功能的識別資訊。它們不存取發布 Environment 或簽名 Secrets；個別啟用的 fork rehearsal 可在其後執行簽名打包。前兩項診斷不代表 IPA、TestFlight 或三端發布驗收。正式簽名打包、上傳及後續 Android → DMG 驗收仍須分別完成。

本機真實打包及上傳、Actions、候選 tag 草稿、商店狀態和真機驗收分別紀錄。模擬通過不能替代真實建置；Chromium 主機、Play 初始化與上傳金鑰、Developer ID/公證仍需對應環境。

```bash
bundle exec ruby fastlane/tests/tag_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml .github/workflows/release-validation.yml .github/workflows/release-candidate-tag.yml
```

回歸使用暫存倉庫、Apple/Play/GitHub 模擬回應，並以臨時金鑰實際簽章及驗證合成 bundle；不會上傳真實應用或建立實機證據。macOS 需設定 JAVA_HOME 指向完整 JDK 17+。參考：[Fastlane build_app](https://docs.fastlane.tools/actions/build_app/)、[TestFlight 上傳](https://docs.fastlane.tools/actions/upload_to_testflight/)、[GitHub 環境憑證](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)。
