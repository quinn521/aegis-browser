> **2026-11-24 · 安全更新源码候选**　桌面 `158.0.8127.89`、Android `158.0.8127.92`。包含官方 Variations 输入校验修复 CVE-2026-134401；未标注已被利用。原 Ver 7.5 (344) 产物仍对应旧内核及原构建提交，不作为本候选验收。新版本编译、三平台签名和实机升级验收尚未完成；下次真实编译前递增产品构建号。

[English](./android.md) | [简体中文](./android.zh-CN.md) | [**繁體中文**](./android.zh-TW.md)

# Android 狀態：No-Go

本文只涵蓋 Android 瀏覽器目標。iOS 和 WebView 外殼不在目前產品範圍內。

## 目前證據邊界

- Android 目前的核心版本與提交見 `ci-pins/android.json`，補丁輸入見 `patches-android/`。
- 預留 application ID 為 `com.gcsa.aegis`；預留不證明已經形成有效套件或 Play 身分。
- 2026-10-09 原始碼候選為 Ver 2.2 (144)，versionCode `2002000144`，含276個Chromium補丁與3個V8補丁。完整Chromium重放通過，與已驗證桌面原始碼僅有官方 `chrome/VERSION` 差異；共享Agent修復和桌面手勢的平台限制均保留，不會在Android開啟桌面手勢。APK建置、簽章、套件身分及實機驗收仍待完成。
- 目前沒有綁定身分的 APK 或 AAB。即使存在 `$HOME/Desktop/GCSA-aegis.apk` 之類的歷史檔案，也不能對應到目前原始碼，更不是 RC。
- v2 原始碼會解析全頁 Agent 分頁背後的公開網頁，並把目前頁面工作綁定到該文件。頁面擷取、去識別化、導覽失效和結果仍需實機驗收。
- v2 原始碼將處理程序層級遠端偵錯 latch 置於 Android DevTools HTTP/socket 啟動之前，並涵蓋延遲啟動。無痕 Profile 一旦觸發 latch，本處理程序內待處理和後續啟動都會被拒絕；實機驗證仍未完成。

在完成目前原始碼建置和真機驗收前，Android 維持 **No-Go**。

## 受支援的建置環境

S1 開發前期，Android APK 使用上游 `aegis-android-linux-arm64` runner 建置。QEMU 執行候選版本固定的 Linux x86-64 工具鏈，目標 ABI 為 `arm64-v8a`。這是過渡建置方式，產出的 APK 可直接進入裝置驗收。後續使用原生 x86-64 Linux 試建同一候選，核對產物與裝置結果後切換預設建置機。[runner 遷移](https://github.com/quinn521/aegis-browser/issues/383)

獨立 Linux 原始碼目錄需要符合以下條件。

- 至少 200 GB 可用磁碟空間；
- 足夠完成 Chromium 建置的記憶體；
- 精確固定的 Chromium 基線與補丁輸入；以及
- 不把現有 macOS checkout 重複用作 Android 建置樹。

## 建置 S1 的三個 APK

在 runner 上將 `CHROMIUM_ROOT` 指向已備妥的 Android 原始碼目錄，其中包含 `src/` 與 `.gclient`。依照 `apps/browser/ci-pins/android.json` 同步依賴並套用其 Android 與 V8 補丁序列，接著在產品儲存庫執行下列命令。

```bash
set -e
export CHROMIUM_ROOT="${CHROMIUM_ROOT:?Set the dedicated Android checkout path}"
export AEGIS_ANDROID_ALLOW_ARM64_HOST=1
bash apps/browser/scripts/build-android.sh
bash apps/browser/scripts/build-android.sh --internal-access-options alpha

out="$CHROMIUM_ROOT/src/out"
sha256sum "$out/AegisAndroid/apks/ChromePublic.apk" \
  "$out/AegisAndroidInternalAlpha/apks/ChromePublic.apk" \
  "$out/AegisAndroidInternalAlpha/apks/AegisAccessOptionsTest.apk"
```

一般 APK 與 App Bundle 關閉 Access Options。內部 Alpha 設定同時建置產品 APK 與專用測試 APK。內部 DEV 使用 `bash apps/browser/scripts/build-android.sh --internal-access-options debug`。

預設產品用 `pnpm --filter @gcsa-aegis/browser package:android` 產生側載套件 `apps/browser/dist/GCSA-aegis.apk`，啟動器名稱為 `GCSA-aegis`。內部設定使用 `out/AegisAndroidInternalAlpha` 或 `out/AegisAndroidInternalDebug`。

`AegisAccessOptionsTest.apk` 會取代已安裝的 `com.gcsa.aegis`，因此在專用裝置上執行 instrumentation。隨後用對應的產品 APK 在 arm64 實機驗證 A210-01/02，每份結果都記錄裝置、系統與 APK 摘要。

## 驗收條件

1. 依 Android pin 與對應補丁目錄中的兩套序列核對完整原始碼樹及產品版本。
2. 為三個 APK 各自記錄儲存庫提交、核心 pin、補丁樹、runner 架構、QEMU 與編譯器版本、GN profile、應用程式 ID、版本、ABI 及 SHA-256。
3. 驗證最終套件名稱、版本、啟動器名稱、圖示、權限、原生程式庫和簽署結構。
4. 在專用實機保留舊版資料，以原套件名稱和簽章安裝升級套件，核對資料相容、正常網頁、`chrome://aegis`與核心保護。首次安裝另用隔離裝置驗證，不透過解除安裝舊版規避升級相容問題。
5. 在實機驗證頁面擷取、文件綁定、去識別化、確認、導覽失效和結果處理。
6. 完成啟動、前後台、當機、儲存、升級和網路驗收，不能留下殘留程序或無法解釋的出站，並驗證延遲 DevTools 啟動不能繞過無痕處理程序 latch。
7. 內部候選通過與 Play 可發布仍是兩道獨立門禁。

## Play Store 邊界

Play 交付使用獨立的 AAB 工作流程。恢復 CD 後以 `0.8.0` 驗證 `internal`，開發結束後以 `1.0.0` 發布到 `production`。簽署、上傳與安裝各自記錄實際結果。[Android CD 工單](https://github.com/gcsagroup/aegis-browser/issues/276) · [Play Store 準備情況](./play-store.zh-TW.md)

## 刻意不做

- iOS 或 WebView 外殼
- 把 CDP 作為 Android 產品功能
- 把本機模型 sidecar 寫成 Android 承諾
- 在現有 macOS checkout 中建置 Android

相關公開文件：[Browser README](../README.zh-TW.md) 和 [fork 架構](./fork-architecture.zh-TW.md)。
