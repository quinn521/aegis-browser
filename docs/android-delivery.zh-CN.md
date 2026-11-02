# Android 交付入口

[English](android-delivery.md)

Simple 与 Full 是两个薄入口，共用 `android-delivery-backend.yml` 和原有 `./release android` 后台。本分支不授权合并、公开发布或正式送审。

最终同时支持三种来源绑定的入口：

| 方式 | 交付结果 | 仍须完成的真实验收 |
|---|---|---|
| 本地手动 `./release android package` | Upload 密钥签名的真实 AAB 和 manifest | 独立验签、身份与摘要核对，再通过已授权 Console 上传 internal |
| GitHub Actions Simple / Full | 共用后台，下载产物附 run/attempt、来源和摘要 | 默认分支注册入口、同源质量门禁、合格 Android runner；契约 CI 不等于打包 |
| `candidate/android/<version>-<profile>.<versionCode>` Tag | 既有 candidate 工作流打包并创建私有 Draft | Tag/SHA 读回、合格 runner、Draft 资产下载验哈希 |

当前产品身份对应的 Tag 示例为 `candidate/android/6.1.0.266-alpha.2006000266`，必须等待最终干净来源完成准入再使用。本次修改不创建或推送 Tag。Android Tag 作业只显式传递两项 Upload 密钥密码，不接收 Play 或 Apple secret；共享后台仅补齐既有凭据接口声明，iOS/macOS 作业不变。

GitHub 的手动入口须先存在于[默认分支](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow)。候选分支 push 契约通过，不代表默认分支已接入 Run workflow，也不代表有权执行真实打包。

当前实际交付路线为：**打包 → 独立验签 → `tools@gcsa.org` 的 Play Console Alpha internal**。服务账号继续仅保留 HomeGuard 应用授权，Aegis API Status/Wait 的 403 仍作为未通过项。Package 成功摘要会列出下载产物名、AAB/证书/manifest 摘要、准确 SHA/tree、producer run/attempt；打包成功不写成已上传或已分发。

Simple 只有 operation、version、build、product 四项。中英文操作为 Package、Package and upload、Status、Wait，默认 Status。打包必须匹配提交中的真实版本和 Android versionCode；查询忽略 Product，只在 internal 轨道查询明确的 versionCode，可查询旧版本，不由当前工作流源码推断远端包来源。

Simple Status/Wait 只发 GET，不创建或撤销 Play edit。官方[轨道摘要接口](https://developers.google.com/android-publisher/api-ref/rest/v3/applications.tracks.releases/list)最多返回 20 个非废弃 release，不含 versionName、AAB 摘要或源码身份。NOT_OBSERVED 不能证明 versionCode 从未使用。Wait 最多十分钟，拒审报错，需开发者操作与已发布分别标示。查询不证明测试者可用性或真机验收。

**“打包并上传”目前明确阻断在签名和上传之前。** 权威的全历史未使用 versionCode 核验尚未建立，不能用轨道查不到版本代替。新打包也拒绝通过 Actions rerun 重放。

Package 使用独立 Upload 密钥签署真实 AAB。默认入口要求 40 GiB 可用空间；本地控制器继续保留外置 15 GiB、内置 8 GiB 运行储备。必须使用就绪的 Android 专用 Linux runner、固定来源与锁定工具。APK、签名夹具或工具链探针均不能代替真实 AAB。

Full 保留 Product、Version or source 完整交付凭据及阶段 JSON。整次运行成功后，复制 `aegis-android-v1:...` 凭据；后续 Upload/Status/Distribute/Submit 使用最新成功凭据，保留原生产运行、attempt、manifest 摘要和源码身份。不能仅凭版本号认定二进制来源。Full 的原凭据 Status 仍是旧 edit/abort 对账路径，不标作 GET-only。

Full JSON 仅接受字符串字段 `operation`、`destination`、`track`、`tag`、`play_release_policy`，并需明确选择 Full。例如：

```json
{"operation":"upload","destination":"play-closed","track":"qa"}
```

```json
{"operation":"distribute","destination":"play-internal"}
```

默认 Upload 只创建 Play internal 草稿，不分发或生产送审。Alpha 仅 internal；Beta 可以内外测试但禁止 production；Release 也必须明确选择阶段。分发要求同一 manifest 和轨道的成功上传记录，可能请求测试轨道审核；生产提交另需明确授权，本次没有该授权。

Tag → 私有 Draft 继续使用既有 `release-candidate-tag.yml`，不增加 Android tag 触发器。Full 可为已验收 AAB 明确提供现有 Android candidate tag 来创建 Draft，并下载验哈希；不会公开发布。

后台在加载凭据前核验原操作者和 rerunner 当前有效的仓库 Admin 权限、仓库/运行/attempt/源码及最新质量门禁。已有产物还核验原 producer 和最新成功状态、manifest 摘要、所有下载字节、AAB 格式、证书和应用身份。凭据只固定身份，不代替授权。源码改变必须使用新的干净候选重新验收。

两个入口逐项显式转发 secret，不使用 inherit。签名仅在 `aegis-release-android`，GET 查询在 `aegis-play` 且只接受明确提供的 Aegis 授权服务账号。Full 变更在导入和复验后才读取 Play 凭据，分发/提交继续使用 `aegis-android-distribute` / `aegis-android-submit`。只有 Draft 后台任务请求 contents write；查询、打包、store 后台任务保持仓库只读。不会把 HomeGuard 等其他应用的服务账号当作 Aegis 权限证明。

变更操作不允许 Actions rerun 重放。上传结果未知时先保留 intent/result，再读服务端对账，不能盲目重试。失败运行的产物仅用于诊断，不能成为成功交付来源。

分支 push 只运行无凭据 contracts，不启动打包/Play/Draft，也不启用 runner readiness 或环境。fork 特权演练和上游 main 的同源质量门禁保持原约束；绿色契约 CI 不等于真实包、商店或设备验收。
