# Access 的 GeoSite 分类、macOS DEV 双入口与 Hysteria 2 实验

基线为 `quinn521/aegis-browser` 的 `develop@e18106b84c56589a69faa1b0965e696caea2cb8c`。iOS 先回答一个问题：给地址栏的顶层 HTTPS 导航分类后，WebKit 实际把连接交给哪个出口？首个完整产品范围是 macOS DEV 的手动模式和显式启用的自动分流。Hysteria 2（HY2）另行验证出站恢复。实施顺序、模拟器回执和复跑命令见 [TDD 实施计划](../plans/2026-11-21-access-geosite.md)。

## 规则来源与准入

GeoSite 提供域名集合，用户所选模式和浏览器策略决定动作。首期使用预编译、可审计的小分组。订阅仍按 A41 只导入节点，第三方列表名称无法授予访问、放宽防护或直接生成 REJECT。[Access 产品合同](../../plans/access-service-v1.0/spec.zh-CN.md)

| 用途 | 核对结果 | 处理 |
|---|---|---|
| 运行时格式候选、iOS 试件 | V2Fly `dlc.dat`，版本 `20261121064510`，2,408,868 字节，1,557 组。完整 `DEBIAN` 只有 `domain:debian.org` 和 `domain:dotdeb.org`，均无属性 | 两条均通过固定 Chromium 的 ICANN 与 PRIVATE 后缀检查。拟作为 macOS DEV 的首个可选小组 |
| 被否决的候选 | `WIKIMEDIA` 的 18 条中含 `toolforge.org`、`wmcloud.org`、`wmflabs.org`，三者本身都是该版本的私有公共后缀 | 拒绝整组，防止自动覆盖其他租户 |
| 已有能力测试语料 | 上游 #98 的 XTLS **标准版** `20261117015116`，`blocked-in-cn` 有 4,389 条唯一规则、4,376 个字面主机候选 | 沿用现有顺序，架构和界面就绪后先进行 iPhone 真机及原 100 站冒烟，再单独评估此语料。保留受限样本及分母；该标签不表示广告拦截 |

V2Fly 输入 SHA-256 为 `b1e803c6b8d02def0865b1b4953ee391c562478d8ff7ee3e8b621a2d15fb7ed2`。Chromium `155.0.8059.80`，提交 `43a195105613ac83c21d3f14de279f4a045c7da0` 的 PSL 文件 SHA-256 为 `4155611645690529d1bbea0cfe653b0c45f63b028e0ba039de29a97edc3204ca`。两条 `DEBIAN` 规则按字符排序，每条写作 `domain:<host>` 并以 LF 结尾，36 字节的 SHA-256 为 `d865d67c829d0f13bf508e152ef402d5d9634227ed12a8b49434d231baf5a643`。这个摘要标识本次提取，打包时另计资产摘要并保留 MIT 声明。[V2Fly 发行版](https://github.com/v2fly/domain-list-community/releases/tag/20261121064510) · [分组源文件](https://github.com/v2fly/domain-list-community/blob/ffb513217b9cb22835eb74a39a907d77742ce68f/data/debian) · [许可证](https://github.com/v2fly/domain-list-community/blob/ffb513217b9cb22835eb74a39a907d77742ce68f/LICENSE) · [Chromium PSL](https://chromium.googlesource.com/chromium/src/+/43a195105613ac83c21d3f14de279f4a045c7da0/net/base/registry_controlled_domains/effective_tld_names.dat) · [#98 语料记录](https://github.com/gcsagroup/aegis-browser/issues/98#issuecomment-6947679716)

离线编译器读取已展开 DAT，以平台 URL 解析器生成的规范主机比较。`full` 仅命中精确主机，`domain` 命中主机及 DNS 标签边界上的子域。比较忽略 ASCII 大小写和单个末尾根点，IDNA 使用规范化 ASCII 主机。首期分类只接受受支持网络 scheme 的 DNS 名，排除 IP、环回、本地域、空标签及公共后缀。属性、keyword、regexp、未知类型、截断、超限或完整性失败均拒绝整组。输入文件、来源、许可、版本、编译器、PSL、规则数量及输出摘要写入清单。首版随 DEV 构建装入明确获准的清单，浏览器请求路径只读不可变索引。

今后更新先在暂存区完整验证签名、允许的来源、范围、数量和大小，再编译并原子发布。坏包保留仍获授权的有效旧版。关闭自动分流或回退有效旧内容时发布新的 generation；撤销、过期或授权变化使旧决定失效。保留手动设置和用户已同意的范围，新增域名超出已同意快照时需确认后采用。诊断记录模式、组、版本、决策来源、generation 和脱敏原因，常规日志不采集完整浏览历史。

## iOS 顶层导航验证

XCTest 把分类器接到 `BrowserTab` 正常的 `prepareProxyNavigation` 调用。在独立试件内，命中组时将当前 host 映射到现有精确主机意图；未命中交给现有策略。生产 `AccessHostStoreCoordinator` 负责准备、租用及选择 store，`AccessHostDataStoreRegistry` 绑定实际 `WKWebsiteDataStore`。同一标签页通过 `loadFromAddressBar` 依次访问四个 `.geosite.invalid` 镜像。原设置 store 和受管 store 各连一个不可故障转移的回环 CONNECT 监听器，监听器记录 authority 后终止。对同一目标要求预期出口有记录、另一出口为零。镜像保留标签关系，真实域名只做离线分类。[浏览器入口](../../../apps/ios/BrowserKit/BrowserSession.swift) · [协调器](../../../apps/ios/AegisApp/AccessHostStoreCoordinator.swift) · [store 注册表](../../../apps/ios/AccessKit/AccessHostDataStoreRegistry.swift) · [Apple 的 store 代理 API](https://developer.apple.com/documentation/webkit/wkwebsitedatastore/proxyconfigurations)

模拟器已验证用户发起的顶层 GET 分类和 CONNECT 选择，也覆盖了命中组但入口准备失败时回原设置的次数为零。原设置可包含用户的系统代理。WebKit 页面内资源随已绑定 store；既有受信 landing 还有来源及 generation 约束。产品级 iOS 自动开关、已提交页面的跳转与历史、iframe、worker、独立下载、TLS/页面完成及真机另行取证。现有位置保护、手动选择、精确成员恢复组保持各自语义。[iOS 精确主机设计](../../plans/access-service-v1.0/ios-exact-host-store-sdd.zh-CN.md) · [模拟器测试](../../../apps/ios/Tests/AegisTests/AccessHostRecoveryWebKitTests.swift)

## macOS DEV 的两种入口

| 入口 | 用户动作 | 浏览器行为 |
|---|---|---|
| 手动，默认 | 当前网站「使用服务」「使用原设置」「清除并继承」 | 按既有 site、website、Profile 所有权处理显式决定；显式使用原设置能覆盖更宽的类别，清除后恢复继承 |
| 自动分流，按 Profile 主动启用 | 选择准入组并确认覆盖范围，可随时关闭 | 无显式覆盖时按当前请求目标 host 命中组选用已授权 Access；未命中保留原生代理配置。空分组、初次安装及旧版本迁移默认手动 |

企业和浏览器安全决策先行。保留 `EvaluateAccessPolicy` 现有 site → website → Profile 优先级、冲突及失败语义，显式 Profile 同样高于类别。只在手动层确实未作决定时查分类索引，类别的 `ProtectionOverride` 固定为 `kNone`。命中需要可用的同 owner 入口；准备、验证或授权失败时有界等待或明确失败。每个请求、重定向逐跳按可信请求所有者、目标、Profile 和 partition 重判，不能由渲染端指定 owner。普通与 OTR/Guest 配置、generation 和连接池遵守既有隔离。发布使用既有准备、精确 ACK、持久提交、完成顺序；迟到 ACK、重启和回滚不可扩大访问。[现有求值器](../../../apps/browser/overlay/components/aegis_access/access_policy_evaluator.cc) · [请求与发布合同](../../plans/access-service-v1.0/w1a-request-routing-design.zh-CN.md)

GeoSite 决定是否使用 Access。若结果为 `preserve_native`，原生 Options 的 Auto Switch 才能选 System、PAC 或 Fixed 基础代理；“使用原设置”因此可能经过原有代理。本地 HTTP/SOCKS 入站选择和远端节点健康轮换分别发生在各自层。后端须从可信构建身份确认 macOS Debug/DEV、无冲突、Internal Options 已授权，再绑定实际 BrowserContext、实例与分区。现有 Options 也接受 Alpha，新能力须分别保护展示、读取、写入、导入、恢复及激活。Alpha、Beta、Stable、unknown、冲突身份均不可启用。#209 的现有 owner、FIRST、预算及当前 Options 交付顺序继续生效。[构建能力](../../../apps/browser/overlay/components/aegis_access/access_options_capability.h) · [Options 设计](../../plans/access-service-v1.0/macos-access-options-sdd.zh-CN.md) · [#209](https://github.com/gcsagroup/aegis-browser/issues/209) · [独立 Options PR](https://github.com/quinn521/aegis-browser/pull/400) · [后期公共管理入口 #142](https://github.com/gcsagroup/aegis-browser/issues/142)

## Hysteria 2 的独立受控实验

HY2 可以经客户端至节点的 QUIC/UDP 承载浏览器 TCP CONNECT。它不改变 GeoSite 决策，也不授予页面通用 UDP。固定 Xray `v26.11.9` 注册了 hysteria v2；Aegis iOS 的出站类型目前只接受 `reality_vision`，Go 桥按 TCP 目的地址 dial，单实例 `Stop` 会关闭已有流。macOS 的 `SelectedOutbound` 和控制协议当前构造 VLESS/REALITY，服务计量补丁的身份绑定在 VLESS 入站。原生支持该协议只解决了可行性的一部分。[协议](https://hysteria.network/docs/developers/Protocol/) · [固定 Xray 配置](https://github.com/XTLS/Xray-core/blob/676eaf66f8f05d4d558ee7a74b4736623d3feeb9/infra/conf/hysteria.go) · [iOS 出站](../../../apps/ios/AccessKit/AccessOutboundProfile.swift) · [Go 桥](../../../apps/ios/EmbeddedCore/bridge/bridge.go) · [macOS 选择器](../../../apps/browser/overlay/chrome/browser/aegis/access/access_local_core_manager.cc) · [控制协议](../../../apps/browser/overlay/chrome/browser/aegis/proxy/access_core_control.proto) · [计量补丁](../../../services/access_account/xray_meter/xray.patch)

实验默认关闭。先用可控时钟和回环端点验证调度，再用自有 QUIC 服务、同账户签名候选、可追踪租约和自有 marker 验证实传。可复用浏览器既有入口。仅主传输故障经既有两轮确认后允许尝试 HY2，单纯网站 403/429/5xx、证书、认证、配额、撤销、本机离线或用户锁定不能触发跨协议切换。沿用单候选、Profile 与浏览器共享预算及冷却。新 generation 经确认并持久化后，只接管新连接，旧流排空或明确失败，不重放请求。可选 iOS 试件要证明旧流生命周期独立；单实例全局 Stop/Start 无法提供这种保证。只有 QUIC 往返、目标 marker、双向字节与账户额度/撤销都能对应，才能评价服务 fallback。模拟器 GeoSite 的通过记录与此实验分别登记。
