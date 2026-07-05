# Client 名称上报设计

## 背景

server Admin Console 当前把 server 端的 peer 视为运行时聚合概念，名称由 QUIC 连接的 `remote_address` 推导。例如 `peer-192.0.2.10:53124`。这在连接池、多客户端、多路径或 NAT 场景下不够可读，也无法表达部署语义。

client 侧已经有 `peer_id`，但它目前只在 client 本地用于 router、listener、connection/tunnel 展示和日志，不会传到 server。单 peer CLI 模式还固定生成 `peer_id = "primary"`，即使直接复用也不能满足生产可读性。

目标是在 client 中增加可配置的 client 名称，并尽早在连接生命周期内提供给 server，使 server console/API 能优先展示有意义的名称，同时保持老版本 client/server 兼容。

## 约束

- 不修改 msquic、quictls 或 QUIC/TLS 握手实现。
- client 名称只作为展示身份，不作为认证、授权或 ACL 判断依据。
- 老 client 不发送名称时，server 继续显示当前 `remote_address` 推导名称。
- 名称更新应是连接级属性，而不是每条 tunnel stream 的属性。
- 多连接池中，同一 client 的每条 QUIC connection 都应能携带同一个名称，server console 按名称聚合。
- 文档、API 和 console 文案默认使用中文。

## 推荐方案

新增一个轻量连接级 HELLO 控制消息：client 在 `QUIC_CONNECTION_EVENT_CONNECTED` 后立即打开一条独立控制 stream，发送 `TQ_CMD_CLIENT_HELLO`，server 收到后把 `client_name` 记录到对应 `TqServerConnectionRecord`。

该方案不是 TLS 握手的一部分，但发生在 QUIC 连接建立完成后立即执行。对 Admin Console 来说，它能在首条业务 tunnel 建立前完成，比把名称塞进 `TQ_CMD_OPEN` 更早、更符合连接级语义。

### 协议格式

在 `src/protocol/control_protocol.h` 中新增：

```cpp
static constexpr uint8_t TQ_CMD_CLIENT_HELLO = 0x20;
static constexpr size_t TQ_CLIENT_HELLO_MIN_SIZE = 6;
static constexpr size_t TQ_CLIENT_HELLO_MAX_NAME_LEN = 64;

struct TqClientHello {
    std::string ClientName;
};
```

编码格式：

```text
0..1   magic: 'T' 'Q'
2      version: TQ_VERSION
3      cmd: TQ_CMD_CLIENT_HELLO
4..5   client_name_len: uint16 big-endian
6..N   client_name bytes, UTF-8/ASCII-compatible display text
```

名称校验：

- 长度 1..64 字节。
- 允许 ASCII 可打印字符中的字母、数字、点、下划线、短横线、冒号。
- 禁止控制字符、空白、斜杠、引号、尖括号等容易污染日志或 HTML 的字符。

## 配置模型

新增字段：

- `TqConfig::ClientName`
- `TqPeerConfig::ClientName`

配置优先级：

1. peer 级 `client_name` 优先。
2. 顶层/CLI `ClientName` 次之。
3. 多 peer JSON 中未配置时 fallback 到 `peer_id`。
4. 单 peer CLI 未配置时 fallback 到 `primary`，但文档建议生产显式设置 `--client-name`。

CLI 新增：

```bash
--client-name <name>
```

client runtime JSON peer 支持：

```json
{
  "peer_id": "office-a",
  "client_name": "office-a",
  "quic_peer": "proxy.example.com:443",
  "socks_listen": "127.0.0.1:1080"
}
```

runtime schema 别名 `id` / `proto_peer` 路径也接受 `client_name`。

## 数据流

1. `TqParseArgs()` / `TqLoadClientConfig()` 解析 client name。
2. `TqMakePeerRuntimeConfig()` 把 peer 级名称下发到每个 `QuicClientSession` 的 `TqConfig`。
3. client connection 进入 `QUIC_CONNECTION_EVENT_CONNECTED` 后调用 helper 打开 control stream 并发送 HELLO。
4. server `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED` 仍进入现有 stream handler，但 handler 先读取首帧：
   - 如果是 `TQ_CMD_CLIENT_HELLO`，只更新 server connection metadata，然后关闭该 control stream。
   - 如果是 `TQ_CMD_OPEN` 或 speed test command，继续走现有 tunnel/speed test 流程。
5. `TqSnapshotServerConnections()` 返回 `client_name`。
6. Admin API `/server/connections` 输出 `client_name`。
7. Admin Console 的 server peer 聚合优先使用 `client_name`，为空时仍用 `peer-${remote_address}`。

## API 和 Console 行为

`TqServerConnectionSnapshot` 增加：

```cpp
std::string ClientName;
```

`/api/v1/server/connections` 每条 connection 增加：

```json
{
  "connection_id": "srv-1",
  "client_name": "office-a",
  "remote_address": "192.0.2.10:53124"
}
```

Console 行为：

- server overview、server peers、server connections 的 `peer` 列优先显示 `client_name`。
- 同一 `client_name` 的多条 connection 聚合到一个 peer 行。
- `remote_address` 仍保留，便于排障区分 NAT/端口/路径。
- 未收到 HELLO 时保持当前展示行为。

## 错误处理

- client name 为空时不发送 HELLO，server fallback 到 remote address。
- client HELLO 编码失败时只记录日志，不影响 QUIC 连接和业务 tunnel。
- server 收到非法 HELLO：
  - 不更新 client name。
  - 关闭该 control stream。
  - 不关闭 QUIC connection，避免因为展示字段导致可用性回退。
- server 收到重复 HELLO：
  - 相同名称幂等接受。
  - 不同名称建议接受最后一次并记录 debug 日志；第一版也可以选择忽略后续不同值，避免控制面被频繁改名。

## 测试策略

- `tcpquic_control_test` 覆盖 HELLO encode/decode、非法长度、非法字符、错误 cmd/version。
- `tcpquic_config_router_test` 覆盖 CLI `--client-name`、client config `client_name`、runtime peer shape `client_name`、fallback 到 `peer_id`。
- `tcpquic_quic_session_reconnect_test` 或新增 session 单测覆盖 server registry 更新 client name 后 snapshot 返回。
- `tcpquic_admin_http_test` 覆盖 console JS 中 server peer 聚合优先 `client_name`。
- 端到端脚本可后续补充：启动 server/client 后检查 `/api/v1/server/connections` 返回 `client_name`。

## 风险和处理

- **时序短暂 fallback**：连接刚 accepted 到 HELLO 到达之间，console 可能短暂显示 remote address。自动刷新后会变成 client name；这是可接受的。
- **名称不是安全身份**：该字段由 client 自报，不能作为 ACL 或审计真实性依据。文档和代码命名避免使用 `authenticated_identity`。
- **控制 stream 与业务 stream 混用风险**：server stream handler 必须先识别 HELLO，再进入 tunnel OPEN；HELLO stream 不注册 tunnel，不计入 tunnel registry。
- **兼容性**：新 client 对老 server 发送 HELLO 时，老 server 可能把该 stream 当非法 OPEN 并关闭 stream。业务 tunnel 后续仍应可用；如果老 server 行为会关闭连接，需要在 client 侧通过 ALPN/feature 协商规避，但第一版先以当前 server 行为验证为准。
