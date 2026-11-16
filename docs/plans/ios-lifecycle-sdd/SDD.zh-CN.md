# iOS 窗口展示、导航尝试与页面助手生命周期 SDD

日期：2026-11-16。实现基线：`8120c4ae30af82f6b325e0e32bf7f70d47389f1a`。范围：iOS；不改变部署目标、公开 BrowserTab/BrowserSession API、现有功能文案或远端协议。

## 需求与边界

| ID | 需求 | 验收边界 |
| --- | --- | --- |
| A1 | 根窗口的标签、助手、资料、设置、当前网站、下载、查找、阅读库、Markdown、窗口管理只有一个展示所有者 | 一个窗口只有一个当前根 sheet；新请求以 FIFO 保留，在旧 sheet 完成消失后交接；旧回调不能清除新请求 |
| A2 | 下载/Markdown 输入成为有身份的展示事件 | 消费事件时清除原 pending 值；下载上下文由路由值保留，关闭旧下载不清除后来输入 |
| A3 | 快捷键服从展示状态和窗口焦点 | 所有根展示和外链确认期间不提供浏览器命令；空分组仍可新建标签；阅读库和 Markdown 期间 Cmd-W 不关闭页面 |
| A4 | 保留外链普通模式确认、VoiceOver、iPad/多窗口和现有 sheet 内容 | 每个 BrowserSession/根视图独立路由，无全局展示单例 |
| B1 | 导航尝试有独立、明确的状态所有者 | 身份创建、FIFO 绑定、重定向合并、失败授权和退休由组件维护，WebKit 访问仍在 MainActor BrowserTab |
| B2 | 保留主框架的实际方法和不安全重放面 | POST/body/stream/Authorization 不被重写为可安全 GET；重定向不能清除 unsafe 位；历史确认保留 WebKit 方法 |
| B3 | 旧 start/failure/finish 不能污染更新的导航 | WKNavigation 对象身份映射与 allow 顺序一起判定；退休和 store replacement 释放相应身份 |
| C1 | 助手选择、读取、确认、发送和取消从 View 提取 | View 只负责布局、焦点、导出与子界面；feature model 持有操作和状态 |
| C2 | 同意绑定实际文档、profile、窗口和请求配置 | tab UUID + instanceID + navigationEpoch + webViewGeneration + URL + profile；发送前及异步回调重新验证；窗口 inactive/close/profile change 撤销 |
| C3 | 不合作异步任务不能复活旧结果或持久化完成记录 | 代次和 lifetime 双闸；取消先撤销身份再 cancel；仅当前有效请求可发布或 finish(completed) |

非目标：新 UI、新依赖/框架、Access 协议或代理策略变更、模型服务账户体系重构、签名/商店发布。Access 账户不是当前模型服务的身份来源；模型配置和凭据在本次确认/发送边界采样，不把 Access token 带入助手。

## 所有权与不变量

应用共享服务保持原状。每个 SwiftUI Window 的 `BrowserSession.windowID` 对应一个 `BrowserPresentationCoordinator`。每个 `BrowserTab` 对应一个 `MainFrameNavigationState`。每次助手 sheet 对应一个 `PageAssistantModel` 与既有 `AssistantOperationLifetime`。

1. 展示当前项与待交接项都带 UUID；dismissal 的 token 必须匹配 retiring 项，才可推进。输入不再依赖多个 Bool 组合。
2. WebKit delegate、store 切换、document termination 和 publish 属性仍由 `BrowserTab` 执行；导航状态组件不持有 WebView，也不调用 host coordinator。
3. 一个 allow 只绑定一个导航；映射值记录 allowSequence，旧回调只退休自己的行。
4. 同意的是已读取的文档集合和模型目标，不是同 URL 的后续文档。tab 关闭/恢复同 UUID、同 URL reload、后台/前台来回都会使旧同意失效。
5. 任何 await 后都检查 lifetime、generation、窗口、profile、文档和取消状态；检查失败不会更新 output、busy、error 或任务完成状态。
6. 私密任务默认不落盘；中断不自动保存正文；外发仍需显式预览和确认。

## 状态转换

### A：窗口展示

`idle → presented(request)`；`presented(A) + request(B) → presented(A, queued:B)`；`dismiss(A.id) → dismissing(A, queued:B)`；`dismissed(A.id) → presented(B)`；旧 `dismissed` 忽略。用户下拉关闭只结束当前请求。profile 切换撤销敏感展示；后台撤销助手和查找。外链确认与 sheet 共用展示排他边界，确认后才切普通模式并开标签。

### B：主框架尝试

`begin → unbound(allowSequence)`；app load 返回对象或 `didStart → bound(WKNavigation identity)`；重定向只更新已确认同链；新 policy action 创建新 attempt。`commit/finish/fail/stop/store teardown → retire matching identity`。安全重试仅消费仍匹配的 failureID/recoveryOperationID，不因提取改变现有重试资格或 host-coordinator 调用顺序。

### C：页面助手

`selection → reading(captured identities) → preview → consent(captured config, goal, sources, lifetime) → sending → completed`。选择变化、文档变化、profile/窗口失效、close/cancel 转入 revoked/idle；读取和模型 task 的晚到回调被忽略。重试重新建立 consent；完成结果对应发送时的 goal/sources，保留现有保存/导出确认。

## 实施切片与任务

1. **SDD 与基线**：核对现有 lifetime、公开 API 与测试；建立本文件与验收矩阵。
2. **A 路由**：新增窗口内 coordinator；替换根 Bool；统一 sheet 分发；将窗口管理接入；下载/Markdown 事件即时捕获；把快捷键资格放到单一 policy；测试交接/旧 dismiss、多窗口、空组、阅读和 Markdown。
3. **B 导航**：提取 attempt 数据与状态操作；BrowserTab 保留副作用；用有意义接口处理 begin/adopt/bind/retire/current/failure；保留现有历史/重试/store 回归；新增组件测试与真实 WK lifecycle 测试。
4. **C 助手**：新增 feature model 和注入式 snapshot/stream 接缝；复用 AssistantOperationLifetime/window binding；精确来源身份和 consent；View 接入；新增 revoke-before-send、同 URL 导航、关闭恢复、后台切换和逆序响应测试。
5. **收尾**：xcodegen 最小工程差异；静态/本地快速检查；执行原生单测/UI smoke；修复实际失败；最终候选 fresh read-only built-in review；复审任何修复。

## 验证矩阵与交付

| 层 | 检查 | 结果记录规则 |
| --- | --- | --- |
| 源码 | `git diff --check`、项目生成差异、公开 API/本地化/隐私检查 | 绑定 baseline + 完整 diff/file hash |
| 单测 | presentation transition/keyboard policy、navigation state、PageAssistantModel lifetime | 运行 AegisTests，保存原始日志与候选 SHA |
| 集成 | 真实 WK provisional/start/redirect/late callback、历史与 popup POST；实际窗口页面/阅读库/Markdown 快捷键 | 保留现有 feature tests；失败注明首个实际失败与环境 |
| UI | 原有 root sheets、页面助手读取/确认/取消、无标签组恢复、iPad/多窗口 | 模拟器/设备证据分别记录，不用源码或编译替代 |
| 审查 | 最终候选的独立只读审查 | 独立提供需求/设计/原始测试；失败/不可用为 open gate |
| CI/设备/发布 | final SHA CI、真实设备、签名、商店 | 本实现不自动视作完成；另列实际结果 |

具体测试对应：`BrowserPresentationCoordinatorTests` 覆盖 FIFO、同 URL 事件、旧 dismiss、实际窗口命令与下载权限；`MainFrameNavigationStateTests` 覆盖绑定/退休及历史重定向重放面；`PageAssistantModelTests` 覆盖撤销前零发送、同 URL 新文档、凭据变化、不合作异步任务、读取中关闭和窗口隔离。`AegisUITests.testIPadCommandsDoNotCloseUnderlyingTabInReadingOrMarkdown` 通过真实窗口触发阅读库和 Markdown 交接，验证 Cmd-W 不穿透。

## 并发设计

全部新增 UI/功能模型和 WebKit 状态组件隔离在 `MainActor`。页面读取和模型流使用 `async/await`；Task 仅连接同步用户动作到异步操作，并由 `AssistantOperationLifetime` 明确拥有和取消。每次 await、流片段、最终结果和持久化前复核操作身份。没有新增 GCD、detached Task 或 unchecked Sendable。弹层交接依据 SwiftUI onDismiss，原生 alert 依据 UIKit dismiss completion，不用延时或轮询猜测关闭顺序。

受保护根展示通过窗口级 focused scene 值保持 ⌘W 的消费路径；Markdown 的 `BrowserPresentationWebView` 注册优先级原生 key command 并在展示层消费，防止禁用浏览器关闭动作后落入 iPadOS 的关闭窗口行为。根阅读库、根 Markdown 与子 Markdown 的真实键盘验收均检查应用前台状态、窗口数、底层网址、DOM 和标签数。

下载展示回执以事件 ID 保留，最多 32 个，超限最旧回执失效而非扩大权限。`ownsAccessDownload` 在执行时复核 window/profile/privateSessionID/tab instance/navigationEpoch/Access selection/exact host；展示冻结值不是授权。关闭旧弹层只释放自己的回执，不清除较新的 pending 下载。

可执行的原生验收入口：

```sh
xcodegen generate --spec apps/ios/project.yml
xcodebuild test -project apps/ios/Aegis.xcodeproj -scheme Aegis-Debug \
  -destination 'platform=iOS Simulator,id=<SIMULATOR_UDID>' \
  -only-testing:AegisTests/BrowserPresentationCoordinatorTests \
  -only-testing:AegisTests/MainFrameNavigationStateTests \
  -only-testing:AegisTests/PageAssistantModelTests \
  -only-testing:AegisTests/AccessNavigationRecoveryTests \
  -only-testing:AegisTests/NavigationPolicyIntegrationTests \
  -only-testing:AegisTests/TabLifecycleTests
```

iPad 集成另运行 `AegisUITests/testIPadKeyboardShortcutsNavigateAndManageTabs`、`testIPadCommandsDoNotCloseUnderlyingTabInReadingOrMarkdown`；助手 fixture 运行 `testRealPageModelAndCitation`、`testPrivateAssistantSendsOnlyAfterConsentAndSavesOnlyOnRequest` 和 `testStreamingDraftCannotBeSavedAfterCancellation`。需要按仓库开发文档准备嵌入式核心和 fixture 服务；模拟器、设备与签名使用各自证据。

## 回滚

各切片是独立本地 commit，可按 C/B/A 逆序 revert。没有存储 schema 变化，无数据迁移；持久化接口不变。回滚不清理用户数据、不删除失败日志、不重写既有证据。若真实 WK 行为发现不一致，先修复到基线语义，再重跑受影响测试与独立审查。

## 验收状态

本文列出验收要求，不以源码提取或工程生成替代原生执行结果。构建、测试、独立 review、CI、真实设备、签名和发布应分别绑定最终候选 SHA，附原始结果；未执行、失败或待完成项仍为 open gate。
