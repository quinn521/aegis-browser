# Access 共享核心验收矩阵

## Android 最低 API 回归（2026-11-16）

基准 `ab7a5630` 的 x86_64 编译目标是 API 26，但 NDK r30 的旧版 CMake 工具链将 `CMAKE_SYSTEM_VERSION` 设为 `1`。修复后，CTest 使用工具链为编译器解析出的最低 API。以下作业均显式检出[修复提交 `84f17768`](https://github.com/quinn521/aegis-browser/commit/84f17768d0c1bf176b53783434dc0d7e1acb5ea9)，使用 NDK 30.0.16248370 和仓库注册的 `aegis_access_contract`。

| x86_64 模拟器 | 实际结果 | 证据 |
| --- | --- | --- |
| API 25 | 通过部署前拒绝，CTest 按预期返回 8 并报告最低 API 26。adb 仅查询序列号、ABI 和 SDK，未创建设备目录或推送程序 | [adb 轨迹与 CTest 日志](https://github.com/quinn521/aegis-browser/actions/runs/43829939299/job/133172573182) |
| API 26 | 通过，CTest 1/1，892 项检查 | [API 26 作业](https://github.com/quinn521/aegis-browser/actions/runs/43828675871/job/133168431490) |
| API 36 | 通过，CTest 1/1，892 项检查 | [API 36 作业](https://github.com/quinn521/aegis-browser/actions/runs/43828675871/job/133168431405) |

x86_64 编译目标为 `x86_64-none-linux-android26`，CTest 注册参数为 `26`。API 26 和 36 运行的 ELF SHA-256 均为 `49add3b40acf1b23ceb81be2e580ce020c978f5511c731ba48ffeecb01475b53`。覆盖 arm64 预设为 `ANDROID_PLATFORM=android-30` 时，编译目标与 CTest 注册参数均为 30。该 arm64 用例只完成配置核对。

API 25 首轮验证在创建 AVD 时缺少 SDK 模拟器包，后续启动还遇到 adb 路径、主机库和 AVD 路径问题。这些作业均未运行 CTest。上表记录修正环境后在模拟器报告 API 25 时执行的拒绝检查。

本次修复的 Windows、Android arm64 设备执行、物理真机及浏览器产品验收均未执行。基准提交的平台运行记录见 [PR #359](https://github.com/quinn521/aegis-browser/pull/359)。

## 初始实现验收

基准证据为 `7d47f940` 的 macOS 独立脚本 892 项检查。下表记录实现阶段的验证。最终提交的报告随 PR 登记。

| 需求 | 用例 | 结果 |
| --- | --- | --- |
| R1、R2 | macOS CMake 构建清单与实际编译源一致，独立编译 | 通过，11 个生产单元 |
| R3、R4 | macOS CTest、脚本、增量重建及向量生成 | 通过，892 项检查，覆盖率分母保持 11 个单元 |
| R1、R3、R4 | 桌面 Chromium 及 V8 完整索引重放 | 同步 develop `da37e3ec` 后，从桌面固定版本依次重放 483 个 Chromium 补丁及 3 个 V8 补丁；Access 为 `0483` |
| R1、R3、R4 | Android Chromium 及 V8 完整索引重放、Access 源码投影 | 从 Android 固定版本依次重放 321 个 Chromium 补丁及 3 个 V8 补丁；Access 为 `0321`，共享清单、源码和向量与 overlay 一致 |
| R3、R4 | Chromium 原生测试与浏览器集成 | 分平台记录，GN 生成和编译证据分别登记 |
| R2、R3 | Windows x64 编译及运行 | 未执行，缺少本次可用的 Windows 工具链及主机 |
| R2、R3 | Android NDK 编译 | 未执行，NDK 下载超时 |
| R2、R3 | Android 模拟器或设备执行 | 未执行，设备运行器只有本地模拟 adb 的证据 |
| R5 | 三端真实浏览器产品闭环 | T4–T7 |
| R6 | 仓库检查、独立 Review、PR CI | 按最终提交和 PR merge 候选分别登记 |

候选记录包括提交或完整差异、清单和向量摘要、编译器与 SDK/NDK、平台和 ABI、构建参数、产物摘要、实际用例及退出码。缺失、跳过和失败的项目分别保留。
