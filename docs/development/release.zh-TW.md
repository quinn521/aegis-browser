[English](release.md) | [简体中文](release.zh-CN.md) | [繁體中文](release.zh-TW.md)

# 本機 Fastlane 與上游發佈工作流程

根目錄 `./release` 統一產生 iOS IPA、Android arm64 APK、macOS arm64 DMG，參考 HomeGuard iOS 分離打包與上傳的做法，沿用 Aegis 自己的工程、建置腳本及簽署身分。`plan` 只需要 Ruby 與 Git；執行 lane 需要 Ruby 3.3+、Bundler 及 Gemfile.lock，先執行 `bundle install`。CI 固定 Ruby 3.3.6、Bundler 2.5.22。

| 平台 | 應用程式識別碼 | 建置主機 | 上傳目的地 |
| --- | --- | --- | --- |
| iOS | `com.gcsa.aegis` | macOS / Xcode | TestFlight 或 GitHub 候選草稿 |
| Android | `com.gcsa.aegis` | Linux x86-64 / 已準備的 Chromium 工作區 | GitHub 候選草稿 |
| macOS | `com.gcsa.aegis` | Apple Silicon / 已準備的 Chromium 工作區 | GitHub 候選草稿 |

iOS 延伸功能使用 `com.gcsa.aegis.safari`、`com.gcsa.aegis.share`。保留 App Group `group.com.gcsa.aegis.ios.share-inbox` 與 Keychain service，新 profile 需要相應 App Group 能力。識別碼不同於歷史開發套件，尚未證明舊資料遷移或覆蓋升級。Android 獨立 QA driver 與 Windows 識別碼不變。

## 本機階段

```bash
./release ios plan --destination testflight --version 3.2 --build 70
./release ios package --version 3.2 --build 70 --run-id ios-70
./release ios upload --destination testflight --version 3.2 --build 70 \
  --run-id ios-70 --manifest-sha256 <package輸出的摘要>
./release macos release --destination github-draft \
  --tag candidate/macos/2.2.0.144 --run-id mac-candidate-144
./release android release --destination github-draft \
  --tag candidate/android/2.2.0.144 --run-id android-candidate-144
```

`package` 預設只保存本機產物；`release` 依序打包並上傳；`upload` 復用原套件，必須提供獨立保留的 manifest SHA-256。可直接使用 `bundle exec fastlane ios package version:3.2 build:70 run_id:ios-70`，macOS lane 名稱為 `mac`。

產物位於 `.artifacts/release/<platform>/<run-id>/`，拒絕覆寫已有嘗試。失敗保留現場，修正後使用新 run ID。清單繫結乾淨已提交原始碼的 SHA/tree、版本、build、大小與 SHA-256；上傳前重新核對。不可將不可信下載附帶的摘要當作預期值。

iOS 的數字 version/build 可明確指定；簽署設定僅修改本次目錄中的受追蹤原始碼副本。Chromium 版本須與已提交原生原始碼相符，不能靠檔名改版本。`CHROMIUM_ROOT` 必須是獨占、已同步並套用補丁的工作區；流程不重設或擷取共享原始碼，保留來源檢查、打包保護及發佈鎖。

## 設定與上傳

透過程序環境或密鑰管理工具注入憑證，不作為 lane 參數。Fastlane 也支援標準本機 `.env` 檔案；這些檔案須保持忽略，不可提交憑證。

| 變數 | 用途 |
| --- | --- |
| `AEGIS_APPLE_TEAM_ID` | IPA 簽署 Team |
| `AEGIS_IOS_PROFILES` | 三個 iOS bundle ID 至已安裝 App Store profile 名稱的 JSON 對應 |
| `DEVELOPER_DIR` | 選用適當的 Xcode 26 |
| `CHROMIUM_ROOT`、`DEPOT_TOOLS_DIR` | 獨占 Chromium 工作區與 depot_tools |
| `AEGIS_AAPT2`、`AEGIS_APKSIGNER` | Android SDK 工具路徑，或在 PATH 提供 `aapt2`、`apksigner` |
| `GH_TOKEN` | 上游原始碼及 Actions 讀取；GitHub Draft 另需 Contents 寫入 |
| `AEGIS_ASC_KEY_ID`、`AEGIS_ASC_ISSUER_ID`、`AEGIS_ASC_KEY_BASE64` | 僅 TestFlight 使用的 ASC Key ID、Issuer 及 Base64 `.p8` |

```json
{"com.gcsa.aegis":"match AppStore com.gcsa.aegis","com.gcsa.aegis.safari":"match AppStore com.gcsa.aegis.safari","com.gcsa.aegis.share":"match AppStore com.gcsa.aegis.share"}
```

兩個上傳目的地皆要求套件原始碼等於目前上游 `main`，且該 SHA 最新 `quality.yml` push run/attempt 的 `quality`、`quality-gate` 均成功。main 前進後需重新繫結候選；本機測試不能替代準入。

GitHub Draft 另要求上游已存在指向相同 SHA 的 `candidate/<platform>/...` tag。流程不建立或移動 tag、不公開草稿、不修改已公開 Release、不覆寫資產。重試先核對遠端摘要再補傳，候選 tag 與既有 `vX.Y.Z.N` 正式三平台發佈門檻分開。

TestFlight 停用 external distribution 與 submission，成功記錄為 `uploaded_processing_unverified`。ASC 處理結果、測試者可見性及實機驗收需獨立確認。上傳中斷後先檢查 ASC，不要盲目重傳同一 build。

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

目前 macOS DMG 為臨時簽署、未公證；Android 沿用 Chromium 建置簽署，尚未取得正式散布資格。草稿明確記錄。Developer ID/公證、Android 正式簽署連續性、Play/AAB、實機驗收及公開發佈仍須獨立完成；本入口不變更正式發佈驗證器。

```bash
bundle exec ruby fastlane/tests/release_test.rb
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/release -p 'test_*.py'
pnpm run ci:test
shellcheck release scripts/release/action.sh
actionlint .github/workflows/release.yml
```

測試採用暫時 Git、ZIP 及模擬 GitHub 回應，不執行真實簽署、archive、上傳或實機操作。參考：[Fastlane build_app](https://docs.fastlane.tools/actions/build_app/)、[TestFlight 上傳](https://docs.fastlane.tools/actions/upload_to_testflight/)、[GitHub 環境憑證](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets)。
