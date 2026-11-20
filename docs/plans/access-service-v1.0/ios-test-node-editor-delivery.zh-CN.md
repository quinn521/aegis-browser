# iOS 内部测试节点编辑器 · 首个实施切片

本切片实现 [#208 的 S1](https://github.com/gcsagroup/aegis-browser/issues/208#issuecomment-7039980352) 中可独立交付的普通测试 Profile 编辑流程。设计采用 fork main `c5b4257a` 与 [精确 SDD `26e8bc97`](https://github.com/quinn521/aegis-browser/blob/26e8bc9793787e23c5d310c15d28dbbadc907d43/docs/plans/access-service-v1.0/ios-app-inbound-sdd.zh-CN.md)。完整 O1–O6 继续开放。

## 用户流程

可信 DEV / 内部 Alpha 从原生“设置 → 隐私保护 → Access 连接设置”或内部当前网站面板进入普通测试 Profile。主动输入或粘贴一条 VLESS/TCP/REALITY/Vision 分享链接，或主动打开相机扫描同一节点二维码。整个捕获/输入只接受一个节点，设备本地解析后显示通用安全别名与协议；原 URI 的 fragment 不作为显示名。

预览只创建当前窗口草稿。Apply 比较打开编辑器时的安全存储版本，再保存规范化节点；Discard 不写入；移除已保存节点需要确认。两窗口同时编辑时后提交者得到冲突。相机拒绝、不可用或取消仍可继续文本输入。离开 Profile、取消或开启新扫描使旧回调失效，新扫描清除旧待应用草稿。

首片只有一个普通固定测试 Profile。界面明确提示“节点已保存，网页连接暂不可用”。本片不把保存操作接入 publisher、DataStore 或订阅，既有网页及用量仍由 `AccessThreeXUIWebEntry` 和 `AccessSubscriptionUsageSession` 负责。

## 边界与实现

- `AccessInternalOptions` 同时检查包 ID、Access channel、release profile 和显式能力；Firebase 不参与。内部 Alpha 可使用 Release 编译。Beta、Release、unknown、矛盾身份和私密作用域从原生仓储方法拒绝读、写及恢复，隐藏界面不是唯一防线。
- 新 Options 入口复用 `AccessLocalV2RayConfigLoader.parseShareURI` 和 `AccessRealityVisionParameters`，增加 8192 字节上限、整次单条、查询字段白名单及别名冲突检查。旧文件/订阅解析合同保持原行为。
- `VisionKit.DataScannerViewController` 只识别 QR；点击扫描后才请求相机授权。捕获 payload 与文本进入相同校验。二维码图像不持久化；自动化只使用合成节点。
- 普通测试节点按渠道保存到独立、本设备、不参与同步且解锁后可读的 Keychain 项。普通偏好和导出没有节点秘密，存储记录有版本及随机 revision；未知或损坏版本拒绝恢复。
- 该片没有手动节点激活或文件/深链导入通路。受限渠道仓储拒绝与包身份单测不等同于完整的 Beta/Release 包、覆盖安装、直接命令与恢复矩阵验收。旧高级面板的渠道迁移仍属后续 O1。

## 验证清单与证据归属

| 检查 | 可证明的范围 |
| --- | --- |
| `AccessLocalV2RayConfigTests` | 整串单节点、字段/别名拒绝、旧文件及渠道资源兼容性 |
| `AccessTestNodeEditorTests` | 渠道与普通/私密仓储拒绝、Apply/Discard、双编辑器冲突、真实 Keychain 往返、扫码取消/迟到回调、损坏版本、合成二维码图像经 Vision 本地解码与文本等价 |
| `SettingsLayoutUITests/testInternalNodeEditorKeepsTextEntryAvailableAfterCameraUnavailable` | iPhone/iPad Simulator 原生设置、最大字体、不可用相机后文本预览、脱敏、Discard、Apply 保存与确认移除 |
| `AccessThreeXUIWebEntryTests`、订阅用量定向回归 | 既有签入订阅授权、额度与失效阻断语义；不计为手动节点实际出站 |
| `AccessHostRecoveryWebKitTests/testFailedEndpointKeepsTheProxyAndDisallowsDirectFailover` | 现有真实 WebKit HTTP 回环路径保留失败端点及禁止直连 failover |
| Alpha 生成器测试、构建/内嵌核心链接、本地化、完整质量门与最终候选独立复审 | 分别绑定候选源码、实际输入与退出码；由 PR 和 #208 记录原始回执与最终状态 |

首次 `fda9ab1c` 运行的 iPhone 26 通过/1 失败、iPad 1/1，以及较早候选的质量门失败原地保留。WebKit 用例失败于首次路由收件等待；未改源码的定向复跑通过。本片为该用例补齐已有同类测试的内容规则预热，使路由等待不包含无关冷编译。仓库 `blind-protocol.test.ts` 的首次超时和子进程状态失败同样保留，不放宽断言或超时以取得绿灯。最终候选结果另记，不覆盖 FIRST。

合成 QR 解码不证明实体相机，模拟器的相机不可用不证明系统授权拒绝。真机扫码、VoiceOver 实际使用、iPad 真双窗与受限发行包矩阵仍待验。网络 publisher、核心 owner 隔离、规则仓储事务、DataStore 绑定/文档退役、HTTP O3、SOCKS OS 准入、O6 真机和托管服务均不在本片的完成声明中。

## 后续与回滚

按 S2→S3→S4→S5 的依赖接入稳定 Profile/规则事务、上游所有权、真实端点及文档退役，再完成固定 HTTP 的 O3。当前进程全局核心不因新增 Swift publisher 自动获得隔离；S3 之前不消费此安全存储激活手动节点。SOCKS 与 HTTP 均无本地认证、仅绑定前协议能力 fallback、上游独立授权及默认未准入约束继续适用。托管账户/套餐/额度等服务端同事交付接口与环境。

撤回本片可通过普通代码 revert 或关闭内部能力。已保存 Keychain 项不会自行激活，不进入受限渠道；重新开放时仍需可信能力和完整版本校验。移除只针对该渠道的手动测试记录，不修改签入订阅、产品网站意图或用量。后续迁移不能把仅 host/port 或旧版本记录当成有效运行登记。
