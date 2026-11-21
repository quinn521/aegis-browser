# GeoSite 热更新、冷缓存与切换 SDD

首期在 iOS DEV 验证，基线为 `24c3b9a1d4ff80c16966883de43976bc780f1afc`。本方案承接 [iOS PR #402](https://github.com/quinn521/aegis-browser/pull/402) 的顶层选路和简洁界面，随后由 macOS、Android 接入同一更新合同。Hysteria 2 独立验证。[网站加速 SDD](2026-11-21-access-geosite-design.md) · [原实施计划](../plans/2026-11-21-access-geosite.md) · [更新实施计划](../plans/2026-11-21-geosite-updates.md) · [工作单 #212](https://github.com/gcsagroup/aegis-browser/issues/212)

## 1. 不变项与复用边界

当前普通 DEV App 已编入 Swift 分类器，`AccessWebsiteRoutePolicy` 的应用签名内置基线是 `DEBIAN` 的 `domain:debian.org`、`domain:dotdeb.org`；不把它描述为已实现远端签名包。保留默认手动、exact 选择优先、未选中走原设置、选中而 transport 失败则关闭请求，以及独立位置保护、恢复与已授权 landing。多个 BrowserWindow 共用协调器，但实际 WebKit store 属于各文档；现有 `changeWebsitePolicy`、`policyEpoch` 是切换入口，不另起一套状态机绕过它们。[路由](../../../apps/ios/AccessKit/AccessHostRoutingPolicy.swift) · [协调器](../../../apps/ios/AegisApp/AccessHostStoreCoordinator.swift)

沿用「设置 → 国内加速 → 网站加速 → 手动／智能」及简短所选模式提示。精确根域、租户风险和协议放在本规格；界面只在待确认扩围或不可用时展示必要的临时说明，不增工程设置区。更新触发、候选选择、缓存恢复、确认、激活及异步回调均验证 ordinary owner 与可信 DEV 身份；private、Guest、其他渠道、unknown/冲突身份不能借直接调用启用，也不能由私密窗口的前台事件触发普通更新。

复用 [RegisteredDomain](../../../apps/ios/AegisPolicyKit/RegisteredDomain.swift) 的 ICANN+PRIVATE PSL 数据和匹配语义；资源 SHA-256 固定为 `ba7d836c0ea57a8bcf9f1fcb868066a7554bed029b441ecc6204ad8494a32612`。现有 `normalizedHost` 会去除多余首尾点，`of` 在空 PSL 时仍有默认后缀行为，**不能直接作为准入判定**。新增严格、可失败的 PSL 查询，显式暴露资源有效性、后缀及后缀祖先；不改变既有调用者行为。复用 [SecurityRules](../../../apps/ios/BrowserKit/SecurityRules.swift) 的 CryptoKit Ed25519 原字节验签方式，不复用其 `allowExpired`、大小上限或签名 key。[Apple 验签 API](https://developer.apple.com/documentation/cryptokit/curve25519/signing/publickey/isvalidsignature(_:for:))

## 2. 小包与共同准入合同

独立分支 `codex/macos-dev-geosite-rules` 的 `d49749e77e140db313fd7219da519ecf71ecb293:scripts/dev/compile-geosite.mjs` 只作静态编译合同参照，不复制 JS、不合并该分支。保留其 catalogue 的 `schemaVersion=1`、`compiler`、`inputs` 来源 pins、`group`、`ruleCount`、排序的 `{type,value}` 与 `payloadSha256`。摘要为按 type、value ASCII 升序的 `type:value\n` UTF-8 字节 SHA-256。其 Chromium PSL pin 与本 iOS PSL 不同：各平台的准入版本及向量须显式绑定所用 PSL，不能声称两份资产等价。运行时只接收已编译 JSON，不下载 DAT/protobuf。

传输信封固定为 `{keyId, payloadBase64, signatureBase64}`。`B` 是解码后的原始 UTF-8 JSON 字节，签名精确覆盖 `UTF8("AEGIS-GEOSITE-UPDATE-V1\n") || B`；不重新序列化后验签。`candidateID=SHA256(同一签名输入)`，64 字节签名由包内配置的 32 字节 Ed25519 公钥验证；内外 `keyId` 必须一致。`B` 必填：

```text
updateSchemaVersion=1, admissionVersion=1, keyId, sourceId, group=DEBIAN,
channel=dev, sequence, issuedAt, expiresAt, action, scope, catalogue
replace / rollback: scope={ownerClass:ordinary, unit:topLevelDocument,
                           route:requireService, rulesSha256:catalogue.payloadSha256}
rollback 另含 rollbackOfPayloadSha256；revoke 不含 catalogue，
scope={ownerClass:ordinary, unit:topLevelDocument, extent:wholeGroup}, revokesThrough
```

上述字段、catalogue 内全部来源 pins、typed rules、数量及摘要都在签名内。`sourceId` 必须映射到应用包固定的 HTTPS URL、公钥及允许的来源项目；签名中的 group 与请求的唯一已启用组一致。pins 记录来源 revision、版本、输入摘要、许可和 PSL；其可追溯性由离线签发流程复现，运行时不把一个第三方摘要当作授权。未知字段/版本、重复 JSON key、错误类型、非法 UTF-8、摘要不符及任一坏规则均拒绝整组；禁止验签后“清洗”部分规则再发布。

v1 上限是信封 128 KiB、解码载荷 96 KiB、JSON 深度 8、非空组 128 条、DNS 主机 253 字节/标签 63 字节；先限流累计字节再解码，不能 `data(for:)` 全量读完才检查。拒绝非 identity 压缩、HTTP 206 部分内容及声明或实际超限，未知长度也逐块计数；不信任 `Content-Length`。规则只接受 canonical 小写 ASCII/IDNA A-label，禁止重复与非排序输入。请求 URL 可经平台 IDNA 转换、去除一个根点后严格验证；规则输入本身必须已规范化。IP、单标签、本地域、空标签、URL 分隔符、转义、属性、keyword/regexp/未知类型均拒绝。`full` 精确命中，`domain` 仅按标签边界覆盖子域；两者都拒绝公共后缀，`domain` 还拒绝覆盖嵌套 ICANN/PRIVATE 边界的祖先。`github.io`、`toolforge.org` 等共享后缀不能获准；具体租户也不能扩成父域，PSL 未列出的共享托管风险仍须签发方审核和用户扩围确认。PSL 缺失/坏 hash 拒绝远端准入，不能以 eTLD+1 猜测补救。

`sequence` 是无前导零的 UInt64 十进制字符串，网络包从 1 开始；版本名称不参与排序。UTC 秒整数满足 `issuedAt <= now+300`、`issuedAt < expiresAt <= issuedAt+7天` 且 `now < expiresAt`。新候选必须高于已准入水位；同序列只允许同一 candidateID 的恢复/重传，异内容拒绝。回滚必须以更高序列重签完整旧内容、新期限及 rollback 目标，重新检查范围；revoke 以更高序列撤销该组所有 `<= revokesThrough=sequence-1` 的版本，包括内置序列 0。已持久化撤销不随撤销包到期失效，恢复须更高序列及重新授权。缺签名、坏签名、过期、低序列或 unsolicited group 从不发布。

## 3. 异步刷新与真实传输

应用签名内的可选 `GeoSiteUpdateConfiguration` 固定 DEV origin、路径、sourceId、公钥与组白名单；缺配置时零更新网络请求，使用下节允许的内置离线路径。设置、订阅、远端响应和环境变量不能改源或换 key；不提供生产域名、付费服务或新部署。生产 feed、密钥保管/轮换、撤销运维与 rollout 另行交付。

只在普通前台 owner 已选智能时由启用/前台事件触发；冷启动先读本地，不等 HTTP。同 owner/group 单 flight，多个窗口和相邻事件合并；导航只读快照，不拉取。成功后在 `max(now+60秒, min(now+24小时, expiresAt-300秒))` 再检查；失败按 1 分钟、5 分钟、30 分钟、6 小时退避，加 0–20% 抖动但封顶 6 小时，持久化 nextAttempt，开关/重启不重置。仅合资格前台期间允许定时重试。切手动、取消、失去最后一个普通前台窗口或 owner/channel 改变，撤销更新代次并取消任务；迟到响应不能落地激活。

规则文件属于浏览器控制面。使用临时 `URLSession` 向包内指定的 HTTPS 地址发条件 GET，遵守系统证书、系统代理和企业网络策略。更新独立于页面与订阅节点，首次启动和节点不可用时也可从已配置的更新源取得签名规则。会话禁用 cookie、持久凭据和 URLCache，拒绝重定向；请求仅携带协议必需字段及已验证版本的 ETag。它不发送页面 URL、浏览历史或账户 token，不为更新源递归运行 GeoSite，也不触碰标签页的连接租约。整个操作最多 30 秒。[Access 控制面合同](../../plans/access-service-v1.0/spec.zh-CN.md) · [Apple 流式接收](https://developer.apple.com/documentation/foundation/urlsessiondatadelegate)

只保存强 ETag，最多 256 字节且无控制字符，与已完整验证的 `(origin/path, source, group, candidateID, bytes)` 绑定。只有该表示仍可读取、重新验证且未到期，才发 `If-None-Match`。304 无载荷，仅表示可复用先前表示；使用同一条件请求绑定的已验证表示，再次检查期限、水位和 owner 代次；响应附带 ETag 时必须匹配。304、Date、Cache-Control 均不能修改签名 expiresAt 或授权范围；无可用表示/ETag 不匹配的 304 失败，下次受退避约束的请求不带条件，不立即循环补拉。[RFC 9110 §15.4.5](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.4.5)

## 4. 冷缓存、时间与防复活

可清理 blob 缓存只留 active 与一个 staged candidate，文件以 candidateID 命名，保存原信封及绑定的 ETag。另以本机 Keychain 小记录保存 source/group/channel 的最高准入 `H`、撤销水位 `D`、内置是否已退休，以及每 owner 的已提交 `P/id/expiresAt/acceptedScopeDigest/mode/状态`、clockFloor；普通偏好镜像不作安全权威。记录与 cache 生命周期独立，删除坏包、退出账户、切换模式和清理网站数据均不能清掉水位。Keychain 使用本机不可同步属性；访问失败不能解释成首次安装。[Apple 本机 Keychain 属性](https://developer.apple.com/documentation/security/ksecattraccessibleafterfirstunlockthisdeviceonly)

写入顺序：后台完整验签/准入 → 同目录临时文件有界写入、flush/fsync、rename 并同步目录 → 主协调器检查 candidate/owner/模式/代次/期限 → 无 await 的短提交区原子更新安全记录 → 替换不可变内存快照。待确认候选只推进 H，不改 P/accepted scope；获准发布才提交 P。所有提交、手动切换、撤销在同一生命周期入口串行化；异步文件完成不能直接改变路由。文件阶段可取消；提交区是线性化点，之后到来的取消作为下一次切换处理。持久化失败不宣布生效；关闭动作先在内存停止新发送并报保存失败，不能声称重启已保存；撤销记录写失败同样不能报告持久撤销成功。

重启按安全记录找**精确的** active id，重新验证签名、大小、来源、PSL、scope digest、期限和水位，再建立快照；`P<H` 只允许恢复此前已经提交且仍有效的同一 active，不能把低版本作为新候选。崩溃在安全记录提交前使用旧提交；提交后 blob 缺失/损坏则智能不可用，绝不扫描目录找一个较旧“好包”。首次确认为干净安装（无安全记录且无既存安装/迁移状态）先初始化记录，再使用应用内置基线；已有版本升级可从已知固定基线迁移。只要发布过网络版或收到有效撤销，就永久退休自动内置回退。既存安装标记与安全记录不一致时同样关闭智能；删除缓存不能复活已撤销、到期或被新版移除的范围。

期限以墙钟检查，不用“最近下载时间+TTL”。启动、回前台、发布及到期时维护持久 clockFloor，判断使用 `max(Date(), clockFloor)`；倒退超过 300 秒暂停更新/激活，已观察到的到期成为持久终态，不因调回时钟复活。前台到期定时器不因回拨延后，每次授权/发送前另检查本地期限，后台恢复先检查再放行。此保证覆盖 cache 丢失及应用中断，不声称抵御应用数据、Keychain 与系统时钟一起被外部回滚。

| 情况 | 发布/离线选择 | 页面决定 |
|---|---|---|
| 干净安装，无 origin/key | 内置两根域，仍默认手动 | 智能命中才要求服务 |
| 坏更新/离线/合法 304 | 保留仍有效且获准的 active；不延寿 | 不改变原决定 |
| 有效新版，范围相等或收窄 | 完整提交新版及收窄后的 accepted scope | 新动作按新版；被撤回的在途动作暂停 |
| 新版扩大 typed 覆盖范围 | staged，确认前保持有效旧版 | 不提前准备新增主机 |
| 到期/revoke/已发布后 cache 丢失 | 持久关闭对应版本；无自动内置回退 | 智能无法确定授权时暂停，不能当作未命中直连 |

## 5. 同意、不可变快照与在途切换

accepted scope 是 owner 已接受的 **typed 覆盖集合**，不是组名或“永远接受 Debian”。覆盖判断保留 full/domain 差别：父 domain 可覆盖其子规则，full 不能授权 domain；无法证明子集就视作扩围。旧版收窄后接受集合同步收窄，不保留历史授权并集。恢复撤销/到期版本也须重新确认。一次确认最多展示 8 条新增/扩大的规则，显示“仅此网站／含子域”、新增数量及同页第三方资源随文档出口的说明，列全、不截断；超过上限整候选不激活，需签发更小候选。不能缩取半组发布。确认令牌绑定 `(owner, candidateID, previousSnapshotID, acceptedScopeDigest, policyRevision, deltaDigest)`；任一变化、到期、取消或切手动使其失效，新版必须重问。

协调器持有 `DecisionSnapshot(ownerID, mode, candidateID/sequence, policyEpoch, acceptedScope, immutableIndex, expiresAt, manualChoices+hostRevisions, independentGrants)`。owner 来自可信普通 Profile/scope；窗口和 tab 是消费者，不各自下载或覆盖共享快照。每次导航/准备还固定 tab 实例、navigation/document ID、host、依赖的规则/手动 revision 与 transport generation；在 await 返回、绑定、发送、受信重定向及续租时再次比较。更新代次、策略 epoch 与 transport generation 分开，不以新 transport 证明旧授权仍有效。

- **新增/改变决定**：先关闭受影响请求的发送入口，再经 `changeWebsitePolicy` 提交快照；新动作才可申请新 lease。已在原 store 中加载、现在应受服务保护的文档也须暂停/终止，不能放任脚本继续原连接。所有共享协调器的窗口共同过屏障，失败的消费者保持暂停。
- **未变决定**：在同 owner、范围、手动 revision、独立授权和 transport 都有效时，可继续旧文档/lease；准备中的操作须显式验证等价并换发新快照令牌，不能无条件豁免旧 epoch。续租受新有效期约束。编辑 A 的 exact choice 只处理依赖 A 的操作，保留 B 的准备；模式变化仍保留独立 legacy landing，不把它推广给冷标签。
- **撤回/到期/撤销/切手动**：失去唯一授权的准备操作取消；依赖它的文档先停止并使 realm inert，终止相关流，保留 retiring store 的代理配置及 lease 到 inert 回执。随后释放**自己的** consumer；禁止先清 `proxyConfigurations`、先释放共享 transport 或把旧请求转往 original。无法确认 inert 则保持封闭，禁止复用该 store；独立仍有效授权不随组退休。
- **动作安全**：发布不批量重载。所有被中断的请求都不自动迁移重发，尤其 POST/有正文/下载/长连接；提交结果不确定须告知用户。用户再次打开时，仅既有安全规则允许的 GET/HEAD 可重新求值；POST 必须用户重新提交。新版明确收窄后，新的用户导航可按未选中走原设置；到期、cache 损坏或 transport 失败不等于这种主动收窄。显式要求服务的 exact/legacy 决定始终不回落直连。

实现须保留 #402 三项定向 P2：已授权 landing 失去入口后 reload 能重新准备；修改 A 不取消 B 的准备；切智能保留已提交的 legacy landing，而同 host 冷标签不继承。WebKit 按文档共用 store，不能仅凭 reducer 或 `stopLoading()` 声称所有子资源/worker 已静默；集成验收必须观察真实文档退场及两侧网络，失败则不开放更新激活。诊断仅记录版本、代次、状态与脱敏原因，不上传主机浏览轨迹。
