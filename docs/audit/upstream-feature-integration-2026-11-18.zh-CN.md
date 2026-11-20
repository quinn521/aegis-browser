# 2026-11-18 上游候选与功能基线续作

原草稿 PR #174 的 Chromium 157 候选继续整合真实远端 `main`，冻结主线为 `e99b5ff830daed5542cf501daef28f4106a36b59`。保留已合入的截图、裁剪、本地及服务器 OCR、MCP/DevTools、移动菜单和设置，以及 iOS 独立 WebKit 交付改动。候选准备版本为 Ver 7.4 (340)，本轮未开始真实编译或打包。

## 源码与验证

- 桌面仍为 157.0.8103.79，Android 仍为 157.0.8103.78；514/346 项补丁从各平台既有已验证序列继续逐项回放，新增 19 项按真实前序树重新导出。752 个桌面覆盖文件与回放树一致；Android 保持其独立序列，不能用桌面覆盖文件一致性替代平台验收。
- Android 保留上游新增的图片菜单项，OCR 追加独立编号并同步全部映射；DevTools 保留导航提交后访问重验、对象生命周期和导航通知。
- 完整 `quality:fast` 已通过；新增实际页面逻辑的 7 项截图/OCR边界测试、MCP 定向回归、OCR 服务 4 项回归通过。过程中的补丁摘要和四端登记失败已修正并重跑，未降低检查。
- 功能合同新增截图/OCR、MCP 隔离，并同步四端目录；真实图像识别、保存分享、确认上传和 MCP 原生操作仍是本次候选的必需验收项。

## 新上游与发行边界

本轮官方渠道发现桌面 158.0.8127.37、Android 158.0.8127.42。158 正在同一候选流程内适配，不能把当前 157 源码当成已完成最新升级。158 的两套 V8 补丁各 3 项回放通过，但不等于安全回归运行通过。

GitHub CLI 的既有身份已具备仓库写权限，可读取 runner；连接器账号的权限与 CLI 分别核对。Mac 固定证书仍报告吊销，未发现正式 Developer ID 与有效公证收据。Windows 既有远程凭据本轮认证被拒绝，不据此断言机器健康或沿用旧验收状态。Android 尚无接通的原生 Linux x64 构建机和 ARM64 真机。

未创建正式 tag、三平台资产或 Release，未执行旧客户端更新安装验收。已利用漏洞的实际产物验证仍未解除；源码、脚本测试及补丁回放不替代真实平台收据。原工作区未提交现场和固定 Chromium 源码未被覆盖。

本轮本地证据：`.artifacts/chromium-upstream/heartbeat-20261118/`。

## 2026-11-20：PR #174 审查修复

以候选 `f2b391b37e73ef8f158cfddbc227cf3112566073` 为基线修复两项问题；本次未更新 Chromium pin、版本号或晋升基线。

- Android #0334 原文件是没有邮件头的裸 diff，`git apply --stat` 成功不能证明生产入口 `git am --3way` 可用。使用固定 Android Chromium 源码及前序补丁重建涉及的 17 个文件，从真实的定向提交导出邮件头；原 diff 的 SHA-256 保持 `4c228dedf59c7c0c03acba75ffbacc7cecb41588c298403364d644e8117a0832`，未改变 hunk 或 blob identity。邮件头的提交身份属于此次定向重建，不代表完整 Chromium 提交树。
- 新增桌面 #0515，将整页截图的 IDC 接入 Chromium 157 默认启用的 Action 映射和回调，并声明对应 GN 依赖。调用时读取当前活动标签页，由既有 `CaptureFullWebpage` 重新校验 URL 和禁止截图策略；保留旧命令分发路径。桌面序列现为 515 项，Android 仍为 346 项。
- `patch-series-format_test.sh` 默认覆盖桌面、Android 及两套 V8 序列，检查 format-patch commit header 和 Git 实际解析出的作者、邮箱、日期、主题及 diff。回归夹具使用真实 `git format-patch` / `git am`，并拒绝裸 diff、缺失字段和空作者。
- 新增 `ActionsAndLegacy/WebpageCaptureCommandBrowserTest.*`，在两种命令分发模式下验证菜单截图生成真实 PNG、菜单创建后禁止截图策略生效，以及内部页面不可截图；纳入 `//chrome/browser/aegis:browser_tests`。

两份修复补丁已在精确文件前序上通过 `git am --3way` 及 ordered exact lineage v2 检查；这是定向源码回放，不是完整 Chromium 回放或构建。四套补丁格式和邮件回归检查、仓库合同及 ShellCheck 通过。新增原生浏览器测试尚未编译、运行，完整 Chromium 157 GN/构建、平台设备、签名和发行验收继续保持未完成；不得以脚本结果替代这些门槛。

## 2026-11-20：主线冲突整合与 158 目标确认

以审查修复提交 `f2f68aed43231607a7523a4d3ee8ee1b3db46c67` 整合主线 `8f3a2576515fcc613e0e08cdd557352c0332f8f8`，保留两个父提交。用户确认继续升级到本 PR 已登记的桌面 158.0.8127.37、Android 158.0.8127.42；本段记录的主线整合仍以当前 157 pin 为过渡基线，不表示 158 已完成。

主线新增的桌面 0496–0501 按候选前序树重新生成 0516–0521，Android 0330 重新生成 0347；原邮件补丁保存在各平台 `archive/main-before-pr174-integration-20261120/`。桌面活动序列为 521 项，Android 为 347 项。157 的 omnibox GN 列表和 feature 邻接上下文已适配；Access BUILD.gn 通过可核对的旧 blob 三方合并，保留截图测试与平台依赖。其余主线实现、iOS 与 CI 改动保留。

新增 6/1 项补丁在各自精确文件前序树上通过邮件回放和 ordered exact lineage v2；两套完整活动序列的相邻 blob 身份及 overlay 对照无不一致。这些是定向源码验证；完整 Chromium 回放、GN/编译、原生浏览器测试、最终提交质量门与独立复查另行登记，尚不构成发行验收。原生 runner 缺少齐备的受信平台路由，不降低现有准入门槛。

## 2026-11-20：Chromium 158 源码适配候选 341

主线文档继续整合至 `167f61b52ac7c6d3bbf1714fd949dc73a462b967`；保留之前的失败与暂停记录。本轮按用户范围只在本机进行 Mac 开发和验证，Windows 执行继续暂停，Android 仅准备独立源码；平台状态不互相替代。

固定桌面 [158.0.8127.37](https://github.com/chromium/chromium/commit/b8c2be851f62f6a394ec57955d7f31335d7cb6d4) 与 Android [158.0.8127.42](https://github.com/chromium/chromium/commit/52bdd21723d5db0c9ab11716d38feac74828cf84)，V8 官方前序为 `b601a2347f005e8bfadd32f285df97ffba5cf1bd`。原有 521/347 项 Chromium 补丁及两套各 3 项 V8 补丁均已在各自完整 Git 树上逐项重放；解决冲突后重新导出邮件，未跳过原有功能补丁。新增兼容补丁后桌面为 522 项、Android 为 348 项；786 份桌面及 V8 overlay 与对应已提交源码对象同步。

适配保留 158 的 Actor surface 身份、预取校验、CORS 预检句柄生命周期、DevTools 同文档权限重验和当前 GN 目标结构；移除的 tab_helpers 入口不恢复，现有防护、手势及页面增强在 TabFeatures 的初始化和页面重建入口保留。Actor 请求通过官方转换接口从授权标签取得 surface，并增加两种 ID 模式下的真实导航回归。设置页入口迁移到 Lit，同时保留 Aegis 分类卸载时清理监听器和子页的行为，新增分类切换回归；新版 Gin 绑定与窗口打开枚举一并适配。

完整产品版本准备为 `7.4.0.341` / `Ver 7.4 (341)`，Android versionCode 为 `2007000341`，与打包默认值和两套补丁版本头保持一致。完整补丁 lineage、最终产品质量门、Mac GN/编译、浏览器与功能回归、最终独立复查和最终 SHA 的 CI 结果按后续收据登记；本段不宣称这些门槛已经通过。未执行签名、公证、安装、tag 或 Release。源码回退应通过保留的 157 基线和原邮件历史形成可审查的新候选，不直接覆盖用户安装或改写主线。

### 本机 GN 适配补充

首次真实 GN 检查发现 Agent WebUI 仍引用已移除的 `browser_public_dependencies`，两处截图入口缺少直接依赖，以及 Access Options 测试引用了 158 已限制可见性的 `Browser` 头文件。新增桌面 0523、Android 0349：WebUI 明确依赖 `browser_process`，截图实现拆为独立目标供入口引用；桌面测试使用 `BrowserWindowInterface`。现行 Chromium 序列为 523/349 项，原有补丁和失败记录保留。

修复后的 macOS `gn gen --check` 已通过，生成 37,510 个目标；该诊断绑定原生前序 `773216cbe5647e754d971128905878e6970038ef` 与完整改动，随后提交为 `b4fc62e58db0ec4f7fabde1d87aa387bbafba59a`。编译和运行测试尚未开始；新候选的完整 lineage、质量门、独立复查及最终 SHA 的 CI 仍须另行核对。Android 本轮仅同步共用源码修复，不宣称 Android 编译或设备验收。

### 整合上游 #207

将上游 `8af307ce1284d4c2e0cf01bb5edf4024a1991747` 合入本地候选。原桌面 0502–0505 移入同一历史归档，按 158 的前序树重新导出为 0524–0527。截图菜单沿用 0523 已接入的独立目标，保留各平台条件和上游 Windows 启动修复。共用的 `StoragePartition` 完整类型修复同步为 Android 0350。活动序列现在是桌面 527 项、Android 350 项。当前验证在本机 macOS 进行。

### 汇合 #174 的 158 草稿候选

保留远端草稿 `2116509fb99cef0f51ce42e3956b9ec651716a67` 与本机候选 `2bd46a685c521c548a8d518c3252b165758b0240` 的双亲历史。接入远端的 Windows Access 和 Mac Access Options 验收清单、四端说明、Lit 生命周期测试及可执行夹具。

独立对照两端最终源码。保留已通过本机 GN 的截图目标、Agent WebUI 依赖和 Access Options 测试写法，继续采用 Chromium 158 的 Actor surface 及新头文件路径，并保留原生导航与设置回归。保留 Chromium 158 官方上下文菜单统计编号，把 Aegis 记录放在新编号。两套 V8 补丁在同一官方基线回放后的变更内容一致，采用已按 158 树重新导出的邮件。

合并候选使用 `7.5.0.342` / `Ver 7.5 (342)` 和 Android versionCode `2007000342`，与远端 341 记录区分。桌面和 Android 序列分别为 528、351 项。前一候选的工具启动失败、临时文件错误与质量门失败保留在本机收据。远端 341 的验证记录见 [该候选审计](upstream-158-2026-11-20.zh-CN.md)。

### 整合上游 #201 与 #174 的后续 GN 修正

将上游 `c5b4257a221b7d4434a113d2c13d870139b8d6fa` 和 #174 草稿 `0d53f45328aad1087893e37ca966b17063ec2ca5` 依次合入候选。保留上游的 Android 模拟器失败记录和 macOS DMG 交付校验，以及草稿对固定源码的诊断记录。

合并说明与补丁清单时保留 342 候选的 528 项桌面补丁和 351 项 Android 补丁。两套序列已具备等效的 GN 依赖修复，继续使用截图实现的独立目标。将四份策略生成覆盖文件的执行位与 Chromium 原生树对齐。`generate_policy_source.py` 保留官方源码的执行位。桌面与 Android 的 Chromium 和 V8 补丁序列、编译参数以及产品版本保持一致。macOS 本机验证继续使用原生提交 `207a7e45e541ea37d2a41fdb71bdb1508a6658df`。本段合并未触发发行或远端 CI。
