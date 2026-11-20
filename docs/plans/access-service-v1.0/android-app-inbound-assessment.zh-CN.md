# Android 评估 · Aegis Chromium 的应用级回环代理

2026-11-20 · 基于 [fork main `c5b4257a`](https://github.com/quinn521/aegis-browser/tree/c5b4257a221b7d4434a113d2c13d870139b8d6fa)。**建议采用双无认证的应用级入站设计，当前产品默认保持未准入。** 与桌面共用 Chromium 的请求归属思路，Android 需要自己的监听所有权、进程重建和设备证据。[共同合同](app-inbound-contract.zh-CN.md)

## 当前能复用什么

Android 产品目标是完整 Chromium 浏览器。自己的 [pin](../../../apps/browser/ci-pins/android.json) 为 155.0.8059.79 / `a49e2e3d4addfd84e765b52afe230bedb81096db`，[补丁序列](../../../apps/browser/patches-android/series) 在基线列至 0330。桌面为 155.0.8059.80，桌面 0350 的存在不证明 Android 产品包含该接线。先查 Android 实际重放后的源码、GN 目标及调用者，再确定可共享的 adapter、解析、所有权检查和 wire 合同。[Android 产品边界](../../../apps/browser/docs/android.zh-CN.md) · [Chromium 代理说明](https://chromium.googlesource.com/chromium/src/+/refs/heads/lkgr/net/docs/proxy.md)

共享 C++ 核心已有 Android NDK ELF 和模拟器 CTest 记录，另有 arm64 真机未完成的条目。这些回执绑定各自提交。现有仓库证据尚未证明本方案的 Android 浏览器 APK 能经两种入站到达指定出口。WebView 的进程级代理覆盖、系统代理设置和 VpnService 均不作为本产品路径。[核心证据与真机状态](../access-build-portability/acceptance.md) · [可移植范围](../access-build-portability/spec.md)

## 合理性与 Android 特有成本

| 设计因素 | 判断与约束 |
| --- | --- |
| 协议与作用域 | Chromium 原生支持无认证 SOCKS5，HTTP 可作为绑定前的备选。选择按受信 Profile/普通或无痕、StoragePartition、路由组和核心代次发布。单条受保护请求只使用一个端点，其他应用及原生路由不纳入本设计 |
| 同机可达性 | Android 的 UID 沙箱隔离应用资源，TCP 回环地址本身没有连接方 UID 认证语义。接受应用内归属与撤销的部署适用双无认证；要求只准特定本地客户端消费出口的部署维持未准入。[Android 应用沙箱](https://source.android.com/docs/security/app-sandbox) |
| 生命周期 | 后台或缓存进程可能被系统回收，onDestroy 回调没有保证。进程重建时从持久策略恢复保护意图，旧端口和旧授权失效，在新监听就绪前拦下受保护的首包。避免靠常驻前台服务维持一个无业务的监听。[进程生命周期](https://developer.android.com/guide/components/activities/process-lifecycle) |
| 网络和资源 | Wi-Fi/蜂窝切换、断网恢复或系统 VPN 变化需要重验受影响上游与网络代次。旧探测结果不能覆盖新网络；切换期间禁止直接试目标或重放业务请求。按活跃授权引用启动/回收监听，测准备延时、持续内存、打开 socket 和后台耗电。[Android 网络状态](https://developer.android.com/develop/connectivity/network-ops/reading-network-state) |
| 实施经济性 | 省去本地密码生成、407 提示和认证缓存。登记快照、双向转发、前后台取消及跨 Profile 隔离仍有成本。以同一 APK 中的 HTTP 与 SOCKS 冷启动及持续负载对照确定性能收益 |

## 建议的验证顺序

| 阶段 | 需要的证据 | 通过后的结论 |
| --- | --- | --- |
| 源码与构建 | 核对独立 pin/补丁、可用核心资产、GN 图和浏览器真实入口，在项目支持的 Linux 构建环境形成可追溯 APK；保存源码、APK 摘要、ABI、版本和构建身份 | 可进入 Android 运行验证 |
| 集成运行 | 在产品实际支持的最低与当前系统上验证普通/无痕、双 Profile、HTTP forward/CONNECT、SOCKS5 CONNECT、单端点发布。用受控出口和独立原连接对照检查标记 | 仅为受测系统和协议记录能力。SOCKS 未准入时可继续独立验证无认证 HTTP |
| 物理设备 | 同一 APK 检查前后台、进程被回收后的首次请求、Wi-Fi/蜂窝切换、端口复用、关闭私密、授权/额度撤销、控制面失效。保护路径在失效时原出口新增收件为零，其他有效 owner 的请求仍能完成 | 只对有实测证据的配置开放候选准入，发布和签名按独立流程判断 |

Android 本轮仅交付可行性评估。现有 Android CI 和共享核心移植工单继续按它们自己的关闭条件执行。
