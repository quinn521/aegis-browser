# iOS 内部测试节点支持 VLESS WebSocket/TLS

普通 DEV 和内部 Alpha 通过现有 Options 的文本或扫码入口导入一条 VLESS 分享。预览只显示协议，Apply 后沿用本 Profile 的本机 Keychain、监听端点与代次撤销。原 TCP/REALITY/Vision 格式仍可用。

WebSocket/TLS 连接由已锁定的内嵌 Xray 执行。客户端使用链接指定的上游地址、DNS 服务名称、WebSocket Host 和路径，保留常规证书校验。支持空 ALPN 或 HTTP/1.1。只生成一个 VLESS 出站，不接受透传配置、明文模式或跳过校验。本地 HTTP CONNECT／已准入 SOCKS5 入口沿用现有行为。签入订阅、账户托管出站以及 Beta / Release 的原生门禁维持现有合同。

旧 REALITY 记录继续用原 Keychain 编码。WebSocket/TLS 使用第二版编码，旧 App 会拒绝读取。需要降级时，可从新版 Options 显式删除该测试节点后再安装旧构建。
