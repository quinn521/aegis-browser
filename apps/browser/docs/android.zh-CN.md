> **2026-11-24 · 安全更新源码候选**　桌面 `158.0.8127.89`、Android `158.0.8127.92`。包含官方 Variations 输入校验修复 CVE-2026-134401；未标注已被利用。原 Ver 7.5 (344) 产物仍对应旧内核及原构建提交，不作为本候选验收。新版本编译、三平台签名和实机升级验收尚未完成；下次真实编译前递增产品构建号。

[English](./android.md) | [**简体中文**](./android.zh-CN.md) | [繁體中文](./android.zh-TW.md)

# Android 状态：No-Go

本文只覆盖 Android 浏览器目标。iOS 和 WebView 壳不在当前产品范围内。

## 当前证据边界

- Android 的当前内核版本和提交见 `ci-pins/android.json`，补丁输入见 `patches-android/`。
- 预留 application ID 为 `com.gcsa.aegis`；预留不证明已经形成有效包或 Play 身份。
- 2026-10-09 源码候选为 Ver 2.2 (144)，versionCode `2002000144`，含276个Chromium补丁与3个V8补丁。完整Chromium重放通过，与已验证桌面源码仅有官方 `chrome/VERSION` 差异；共享Agent修复和桌面手势的平台限制均保留，不会在Android开启桌面手势。APK构建、签名、包身份及真机验收仍待完成。
- 当前没有绑定身份的 APK 或 AAB。即使存在 `$HOME/Desktop/GCSA-aegis.apk` 之类的历史文件，也不能映射到当前源码，更不是 RC。
- v2 源码会解析全页 Agent 标签背后的公开网页，并把当前页面任务绑定到该文档。页面采集、脱敏、导航失效和结果仍需真机验收。
- v2 源码将进程级远程调试 latch 置于 Android DevTools HTTP/socket 启动之前，并覆盖延迟启动。无痕 Profile 一旦触发 latch，本进程内待处理和后续启动都会被拒绝；真机验证仍未完成。

在完成当前源码构建和真机验收前，Android 维持 **No-Go**。

## 受支持的构建环境

Android APK 在上游 `aegis-android-linux-arm64` runner 构建。QEMU 执行候选固定的 Linux x86-64 工具链，目标 ABI 为 `arm64-v8a`。原生 x86-64 Linux 可作为备用构建机。独立 Linux 源码目录需要满足以下条件。

- 至少 200 GB 可用磁盘空间；
- 足够完成 Chromium 构建的内存；
- 精确固定的 Chromium 基线与补丁输入；以及
- 不把现有 macOS checkout 复用为 Android 构建树。

## 构建 S1 的三个 APK

在 runner 上将 `CHROMIUM_ROOT` 指向已准备好的 Android 源码目录，其中包含 `src/` 和 `.gclient`。按 `apps/browser/ci-pins/android.json` 同步依赖并应用其 Android 与 V8 补丁序列，然后在产品仓库执行以下命令。

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

普通 APK 和 App Bundle 关闭 Access Options。内部 Alpha 配置同时构建产品 APK 和专用测试 APK。内部 DEV 使用 `bash apps/browser/scripts/build-android.sh --internal-access-options debug`。

默认产品用 `pnpm --filter @gcsa-aegis/browser package:android` 生成侧载包 `apps/browser/dist/GCSA-aegis.apk`，启动器名称为 `GCSA-aegis`。内部配置使用 `out/AegisAndroidInternalAlpha` 或 `out/AegisAndroidInternalDebug`。

`AegisAccessOptionsTest.apk` 会替换已安装的 `com.gcsa.aegis`，因此在专用设备运行 instrumentation。随后用对应产品 APK 在 arm64 真机验证 A210-01/02，每份结果记录设备、系统与 APK 摘要。

## 验收条件

1. 按 Android pin 和对应补丁目录中的两套序列核对完整源码树及产品版本。
2. 三份 APK 逐包记录仓库提交、内核 pin、补丁树、runner 与工具链版本、GN profile、应用 ID、版本、ABI 和 SHA-256。
3. 验证最终包名、版本、启动器名称、图标、权限、原生库和签名结构。
4. 在专用实机保留旧版资料，以原包名和签名安装升级包，核对资料兼容、正常网页、`chrome://aegis`与核心保护。首次安装另用隔离设备验证，不通过卸载旧版规避升级兼容问题。
5. 在真机验证页面采集、文档绑定、脱敏、确认、导航失效和结果处理。
6. 完成启动、前后台、崩溃、存储、升级和网络验收，不能留下残留进程或无法解释的出站，并验证延迟 DevTools 启动不能绕过无痕进程 latch。
7. 内部候选通过与 Play 可发布仍是两道独立门禁。

## Play Store 边界

Play 交付使用独立的 AAB 工作流。恢复 CD 后用 `0.8.0` 验证 `internal`，开发结束后用 `1.0.0` 发布到 `production`。签名、上传和安装各自记录实际回执。[Android CD 工单](https://github.com/gcsagroup/aegis-browser/issues/276) · [Play Store 准备情况](./play-store.zh-CN.md)

## 刻意不做

- iOS 或 WebView 壳
- 把 CDP 作为 Android 产品功能
- 把本地模型 sidecar 写成 Android 承诺
- 在现有 macOS checkout 中构建 Android

相关公开文档：[Browser README](../README.zh-CN.md) 和 [fork 架构](./fork-architecture.zh-CN.md)。
