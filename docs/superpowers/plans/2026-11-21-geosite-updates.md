# GeoSite 更新的 RED/GREEN 实施计划

实施基线为 `24c3b9a1d4ff80c16966883de43976bc780f1afc`。按 U1 至 U5 先写失败用例，再实现并验证，记录模拟器、构建和真实网络各自的结果。[本次 SDD](../specs/2026-11-21-geosite-updates-design.md) · [原 SDD](../specs/2026-11-21-access-geosite-design.md) · [原计划](2026-11-21-access-geosite.md) · [#212](https://github.com/gcsagroup/aegis-browser/issues/212)

## U1：固定 wire 与严格准入

**RED**：新增 `apps/ios/Tests/AegisTests/AccessGeoSitePackageTests.swift`，用一次性私有签名 key 验证原字节覆盖：逐项更改来源、组、渠道、sequence、期限、action、scope、pins 或规则必拒绝；再测错 key、无签名、同序列异内容、重复 JSON key、错误 UTF-8、乱序/重复/混入非法规则、上限及空组。`AccessGeoSiteAdmissionTests.swift` 覆盖真实 PSL hash、PRIVATE、通配/例外、IDNA、多个根点、标签混淆、父域包含嵌套租户边界及 full→domain 扩权，不能只用玩具 PSL。

**GREEN**：新增 `apps/ios/AegisPolicyKit/GeoSitePackage.swift`、`GeoSiteAdmission.swift`；在 `RegisteredDomain.swift` 提供严格查询但保留既有 API。新增 `packages/core/src/access/geosite-update-v1.vectors.json` 作为平台无关逻辑向量，记录输入字节、期望判定及 scope 包含关系，不放私钥。将 `apps/ios/AccessKit/AccessHostRoutingPolicy.swift` 改为接受已准入的 immutable typed index，默认仍为两根域。参照独立 Mac 编译器输出合同，不复制/移植 JS。回归 `WebKitProtectionTests` 的 PSL/landing 及 `AccessHostStorePolicyTests`、既有 GeoSite 分类向量。

## U2：持久化、撤销与可中断恢复

**RED**：新增 `AccessGeoSiteCacheTests.swift`、`AccessGeoSitePublicationTests.swift`：分别在临时写、flush、rename、安全记录提交前后中断；破坏/删除 active、留旧包、恢复备份偏好、缺安全记录、Keychain 锁定/写失败。覆盖干净安装基线、升级迁移、离线重启、刚到期、时钟后退、撤销包到期、收窄后删 cache、较高序列 rollback、待确认候选高于仍有效 active。断言无半包发布，无旧范围复活，也无“无法恢复=original”的降级。

**GREEN**：新增 `apps/ios/AegisApp/GeoSiteCache.swift`、`GeoSiteSecurityLedger.swift`、`GeoSiteUpdateCoordinator.swift`。注入文件系统、墙钟、Keychain 与故障点；真实持久化适配器也要经历进程终止/重启，内存 fake 不能替代冷启动证据。实现 source 水位与 owner 提交记录、原子文件、候选 ID、期限终态和串行提交。回归旧版未到期时坏包不影响页面；记录每个中断点的文件/记录状态，不清历史失败。

## U3 条件 HTTPS 与后台刷新

**RED**。用 `AccessGeoSiteUpdateTransportTests.swift` 让真实 URLSession 访问自有 HTTPS 服务。无订阅节点时也应收到 200，持久化后下次携带 If-None-Match 并接受 304，重复 304 不延长签名期限。覆盖错证书、错签名、重定向、HTTP 206、分块超限、错误长度、压缩、慢流超时、取消后的回调、无有效本地表示或冲突 ETag 的 304。更新源记录请求，页面服务出口记录更新请求数为零。

**GREEN**。在 `GeoSiteUpdateTransport.swift`、`GeoSiteUpdateConfiguration.swift` 接入包内固定的更新源和公钥、独立临时 URLSession、有界流式接收、条件请求、退避及单次在途合并。保留系统和企业网络配置。普通前台窗口向协调器报告生命周期。浏览器请求路径继续只读取当前快照。

独立 `AccessGeoSiteEligibilityTests.swift` 从 UI 和后端直接调用 foreground、enable、choose、restore、activate：手动/禁用、private、非 DEV、unknown、无配置时更新器网络计数必须为零；先关再开不能重置退避。两普通窗口同时前台/启用只发一次；最后普通窗口离开、切私密、切账户、旧 generation 回调均不得发布。普通已配置 DEV App 使用不可变 bundle 配置走同一传输实现，不能只把 fetch 注入测试后宣称产品已接通。

测试私钥在测试进程内生成，或置于仓库及 App/XCTest 产物之外的私有临时目录；不提交、不打包、不打印。受控服务可持有该 key，只把公钥交给测试配置。TLS 测试信任仅限该测试 session 的回环端点/证书，错误 host/key 仍拒绝；不修改机器 trust、DNS、代理或全局 ATS。普通 DEV 使用团队已有 HTTPS origin/key；未提供时保留 bundle 离线能力，不虚构地址，也不把回环证据当作生产 feed/密钥运维验收。

## U4：owner 快照、确认与真实 WebKit 切换

**RED**：新增 `AccessGeoSiteSwitchingTests.swift`，扩展现有 `AccessHostRecoveryWebKitTests.swift`，覆盖新增、撤回、未变三种域及 active/retiring lease。交错下载完成与手动/取消、confirm 与下一候选、两窗口 exact 编辑、准备与新 epoch、发送与到期、失效后离线重启；旧同意令牌不能批准新 candidate。同 scope 更新应保留等价操作，失去授权的文档不再产生原连接流量；POST/长连接中断不能自动重发，不能用清空代理退场。

**GREEN**：在 `AccessHostStoreCoordinator.swift` 的 `changeWebsitePolicy`/`policyEpoch` 上发布不可变快照；按依赖精确取消/换发令牌，接入 `AccessHostDataStoreRegistry.swift`、`BrowserKit/BrowserSession.swift` 的实际文档退场与 consumer 生命周期。`BrowserSettings.swift` 只增加一次性扩围确认/必要失败反馈，保持两模式与简短提示；`SettingsLayoutUITests` 新增确认候选变更、取消、8 条上限及 private/release 无入口用例。

以下既有 #402 方法必须原样保留并实际复跑，再增加热更新交错版本，不能只测新 reducer：

- `testGeoSiteReviewReloadRepreparesAuthorizedLanding`
- `testGeoSiteReviewSiteChoicePreservesUnrelatedPreparation`
- `testGeoSiteReviewSmartPreservesCommittedLegacyLanding`

## U5：最小 DEV 闭环与交付门

最小功能阶段必须连通 **普通 iOS DEV 设置启用 → 原生控制面 HTTPS 200 → 验签/确认 → 持久提交 → 多窗口真实 WebKit 路由 → 304 → 离线重启 → 撤销/到期不复活**。缺 origin 的独立路径证明零更新 egress 与内置基线可用。实际受控页返回独有 marker，并记录两侧请求直到文档 inert；仅 CONNECT 建连、mock、编译或源码审查均不足以验收。后续真机普通 DEV 安装验证另记 exact App hash，不能从模拟器推定。

每个 U 先执行可暴露缺陷的测试并保存 RED，再做最小实现、重跑同一选择记录 GREEN；构建失败/测试未发现不是行为 RED。将新文件注册到 `apps/ios/project.yml`/生成工程；拟用现有 `Aegis-Debug` 入口按类筛选：

```bash
# 以下仅为后续命令模板；RESULT_ROOT、SIM_UDID 与 CORE_PRODUCTS 由实施阶段核定。
xcodebuild test -project apps/ios/Aegis.xcodeproj -scheme Aegis-Debug \
  -configuration Debug -destination "platform=iOS Simulator,id=$SIM_UDID" \
  -derivedDataPath "$RESULT_ROOT/DerivedData" -parallel-testing-enabled NO \
  -onlyUsePackageVersionsFromResolvedFile \
  "AEGIS_EMBEDDED_CORE_PRODUCTS=$CORE_PRODUCTS" \
  -resultBundlePath "$RESULT_ROOT/$RUN_ID.xcresult" \
  -only-testing:"AegisTests/$TEST_CLASS"
```

对 U1–U4 全部拟增类（均在 `apps/ios/Tests/AegisTests/`）及受影响旧类逐一执行，核对预期方法实际运行、零跳过；U4 加跑整个 `AccessHostRecoveryWebKitTests`、`AccessHostWebKitRouteTests` 和设置 UI 回归。产品包构建/签名检查沿用[原计划的 iOS DEV 入口](2026-11-21-access-geosite.md#2-ios-dev-的网站加速)，另验证普通 DEV bundle 的配置、PSL/规则/公钥 hash、无测试私钥/测试 trust，以及非 DEV 包不能激活更新。受控传输、模拟器、真机、最终 SHA CI、签名与生产发布分开登记。

完成代码、构建、集成和功能测试后，对最终提交的完整差异及原始验证记录运行独立只读审查。修复或更新基线后复审，再运行适用仓库门和 PR 检查。[CI 指南](../../development/ci.zh-CN.md)

## 后续平台

macOS 在其独立静态规则/路由/UI 可交付后接续：消费同一 envelope、typed scope、候选 ID、水位/期限及确认向量；在 `access_policy_evaluator_unittest.cc`、Options 服务测试和 `access_browser_request_adapter_browsertest.cc` 增加真实 owner/partition、OTR、跨请求切换与控制面更新的 RED/GREEN。平台 PSL/admission 必须匹配合同，不重新解释签名字段。由 [#209](https://github.com/gcsagroup/aegis-browser/issues/209) 原 owner 按 FIRST 和原预算安排 GN/浏览器验证，本补丁不构建 macOS。

Android 随后由 [#210](https://github.com/gcsagroup/aegis-browser/issues/210) 原 owner 接入同一逻辑格式及 native Profile/Incognito、持久安全记录、条件 HTTPS 与设备网络见证，不借用 iOS/macOS 成绩，不改变现有 ARM 环境前提或预算。本补丁不构建 Android；HY2 继续独立。
