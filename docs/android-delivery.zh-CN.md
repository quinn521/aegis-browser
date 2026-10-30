# Android 交付

[English](android-delivery.md)

Actions 表单沿用现有 `./release android` Fastlane 打包、独立 Upload 密钥签名验签、Play 草稿和 GitHub Draft 后端。本地 CLI 不扩展。此分支供 Android 交接，不包含合并、公开发布或正式送审授权。

表单只有四项：

| 字段 | 填法 |
|---|---|
| 操作 | 常用为仅打包、上传 Play 草稿、查询状态，其余选择完整模式。 |
| 目标产物 | 自动：打包默认 Alpha，后续继承凭据。Alpha 仅内测；Beta 可内外测但不能正式送审；Release 通用但每阶段仍需明确选择。 |
| 版本或来源 | 打包可留空使用源码中版本/versionCode，也可填 `6.1.0.266(2006000266)`；后续粘贴完整交付凭据。 |
| 完整模式配置 | 非完整模式保持 `{}`。只接受无凭据的 JSON 设置。 |

打包产物为独立 Upload 密钥签名的 AAB，仍要求专用 Linux 构建机、已验证的固定源码和构建前至少 80 GiB 空闲。APK 是本地侧载产物，不能经此表单上传 Play。工具链对象探针和测试夹具均不是真实 AAB。

等待**整次运行成功**，再从摘要复制 `aegis-android-v1:...` 整行凭据。下一次选“上传 Play 草稿”，目标产物保持自动，将凭据粘贴到“版本或来源”。上传默认在内测轨道建立 **draft**，不自动分发、不正式送审。查询状态使用同样凭据并核对确切 versionCode。后续操作使用最新成功凭据，同时保留最初打包凭据。

完整模式仅接受字符串字段 `operation`、`destination`、`track`、`tag`、`play_release_policy`；操作为 package、upload、status、distribute、submit。例如版本或来源仍填成功凭据：

```json
{"operation":"upload","destination":"play-closed","track":"qa"}
```

```json
{"operation":"distribute","destination":"play-internal"}
```

分发必须提供同一清单、同一轨道的已完成上传记录。Play 测试轨道分发可能申请测试审核；测试者可见、审核通过、设备验收应分别记录。Beta 只允许内测、封闭或开放测试；正式提交必须为 Release、明确生产目的地和发布策略，并另有用户授权，本轮不执行。

Tag → Draft 保留现有 `release-candidate-tag.yml`，不增加第二个 Android Tag 触发器。Tag 格式为 `candidate/android/<version>-<profile>.<versionCode>`，必须绑定已准入源码。完整模式也可对成功 AAB 凭据选 upload、`github-draft` 和明确的现有 Android candidate tag。后端保持私有 prerelease Draft，并下载核验远端字节摘要，不公开发布。

后端独立核验原操作者和重跑者的当前仓库 Admin 权限、确切仓库/源码和最新质量门禁。后续阶段还核验原打包生产运行、最新 package/state attempt、生产作业、原清单摘要、全部下载字节、AAB 包型、签名证据和应用身份。凭据只固定来源，不能替代授权；源码变化必须在新 clean SHA/tree 上重新打包。权限元数据不可读时停止。

商店写操作不能通过 Actions 重跑重复执行。上传结果未知时，先保留 intent/result 并回读 Play 状态，不能盲目重传。失败运行的附件仅用于诊断，不能作为成功交付来源。

fork 分支 push 仅运行无签名、无商店凭据的 contracts，不启动打包/Play/Draft，也不消耗其他平台 runner。该 CI 不替代真实 AAB、Play 或 Tag 验收。特权 fork 演练仍要求既有显式 fork 身份、同源 release-validation 和 Android runner readiness 门禁；上游要求当前 main 和同源质量通过。新增工作流不启用任何 readiness 或仓库环境。

沿用既有 `aegis-release-android`、`aegis-play`、`aegis-github-draft` 环境。签名密码仅打包时读取，Play 凭据在导入与重新校验后读取。其他应用的服务账号不直接证明 Aegis 权限。仅工作流界面选项双语，本地 CLI 保持原样。

旧通用工作流不再提供 Android 手工下拉选项，但保留供 Tag/既有流程调用的 reusable 后端。完整模式分发和提交分别沿用 `aegis-android-distribute` / `aegis-android-submit` 阶段环境，不能借精简表单绕过原阶段保护。
