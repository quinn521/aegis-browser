# Aegis iOS 1.0.1 build 14 补丁版本送审文案

记录日期：2026-11-04（Asia/Shanghai）。应用：GCSA Aegis，Apple App ID：`6827988433`。

送审目标为 iOS / iPadOS **1.0.1 补丁版本、build 14**，沿用现有 Aegis Beta 外测组并替换 build 12。本文件承载该版本的三语商店开源说明和送审状态记录。

## 更新状态

本文件保存待追加到 App Store 对应语言介绍的段落。本次文档更新没有写入 App Store Connect，也没有执行 TestFlight 构建替换或 Beta 提审。操作前应读取线上现有内容，保留其他有效介绍并避免重复追加。

| 项目 | 本次已核验状态 |
| --- | --- |
| build 14 源码 | `18d167c5a81eeae0cd80ca1de3f4897fef718916` |
| GitHub 打包与 Draft | [运行 41718146298](https://github.com/gcsagroup/aegis-browser/actions/runs/41718146298) 的 admission、ios-package、ios-draft 成功 |
| Apple 上传和处理 | 该运行的 upload-new、status-query、store 等阶段跳过；不能据此判断其他路径是否已上传或处理完成 |
| 外测组 build 14 替换 build 12 | 待读取 App Store Connect 实时状态并完成、回读核验 |
| TestFlight Beta 审核 | 待读取实时状态并完成提交、回读核验 |
| 商店开源介绍 | 以下三语文案已准备；尚未由本次更新在线保存 |

商店说明以仓库的 [README 许可范围](../../README.md#license-and-acknowledgements)、[LICENSE](../../LICENSE) 和[第三方许可说明](../../THIRD_PARTY_NOTICES.md)为依据。源码可供审查不等于已经通过独立安全审计；以下文案不作安全认证或数据零收集承诺。

## 简体中文（zh-Hans）

```text
开源透明，可供审查
Aegis 公开源代码，欢迎开发者和安全研究人员查看实现、报告问题并参与改进。GCSA 编写的源码采用 Apache-2.0 许可，第三方组件遵循各自许可。
源码与许可说明：https://github.com/gcsagroup/aegis-browser
```

## 繁體中文（zh-Hant）

```text
開源透明，可供審查
Aegis 公開原始碼，歡迎開發者與安全研究人員查看實作、回報問題並參與改進。GCSA 編寫的原始碼採用 Apache-2.0 授權，第三方元件遵循各自授權。
原始碼與授權說明：https://github.com/gcsagroup/aegis-browser
```

## English（en-US）

```text
Open source and open to review
Aegis publishes its source code for developers and security researchers to inspect, report issues, and contribute improvements. GCSA-authored source is licensed under Apache-2.0; third-party components retain their own licenses.
Source and license information: https://github.com/gcsagroup/aegis-browser
```

## 完成后的记录

保存商店介绍后回读对应语言内容；更换外测构建及提交 Beta 审核后记录实际 build、测试组、审核状态与核验时间。再更新三语 changelog 的待核验记录，并保留 build 12、13 的历史交付记录。Beta 提审不等于审核通过，也不等于 App Store 正式版本发布。
