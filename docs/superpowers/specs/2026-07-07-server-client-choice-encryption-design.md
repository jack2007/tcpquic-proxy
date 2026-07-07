# Server Client-Choice Encryption 设计

## 背景

`tcpquic-proxy` 当前用 `proto.disable_1rtt_encryption` 控制是否禁用 QUIC 1-RTT 加密。默认值是 `true`，client 和 server 都会在连接建立早期调用 `QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION`。当 client 使用 `--enable-encrypt` 或配置 `proto.disable_1rtt_encryption: false` 时，client 不会协商禁用 1-RTT 加密；server 仍按默认配置尝试禁用，MsQuic 会返回 `QUIC_STATUS_INVALID_STATE`，POSIX 下表现为 `0x1`，于是打印：

```text
tcpquic-proxy: failed to disable QUIC 1-RTT encryption on server connection, 0x1
```

这个日志在“server 默认禁用、client 选择加密”的场景下是预期结果，但它暴露出当前配置模型的问题：server 需要预先决定所有入站连接都加密或都不加密，无法让不同 client 按自己的配置选择。

目标是把 server 端默认改成按连接协商：server 不再预先配置 enabled/disabled，而是允许 client 在每条连接上决定是否请求禁用 1-RTT 加密。client 端保留现有 `--enable-encrypt` 参数，用于请求正常 QUIC 加密。

## 目标

- server 默认采用 `client-choice` 策略。
- server 不需要 `--enable-encrypt` 参数语义；server CLI 上使用该参数应被拒绝。
- client 继续保留 `--enable-encrypt`，作为 `proto.disable_1rtt_encryption = false` 的 CLI 入口。
- client 使用正常加密时不需要配置 client cert/key，只需要 `tls.ca` 或 `--ca` 验证 server 证书。
- 每条 server runtime connection 保存实际加密状态，Admin API 和 Admin Console 展示：

```json
{
  "encryption": "enabled"
}
```

或：

```json
{
  "encryption": "disabled"
}
```

- server config/admin config 输出表达策略，而不是复用单连接结果：

```json
{
  "encryption_policy": "client-choice"
}
```

- 行为面向 Windows、Linux、macOS 三个平台。不能写只依赖 POSIX 数值 `0x1` 的逻辑，必须使用 `QUIC_STATUS_INVALID_STATE` 常量判断。

## 非目标

- 不修改 MsQuic、QUIC transport parameter 格式或 TLS 证书验证模型。
- 不引入 client certificate authentication。
- 不把单连接 `encryption` 状态写回 config 文件。
- 不让 server 在业务层收到 stream 后再决定加密状态，因为 1-RTT 加密禁用是握手 transport parameter 协商项，必须在 `NEW_CONNECTION` / `SetConfiguration` 前完成。

## 推荐方案

server 第一版不新增可配置的加密策略枚举。server 端策略固定为 `client-choice`，只在 config/admin 输出层用字符串表达：

```json
{
  "encryption_policy": "client-choice"
}
```

这样满足“server 不预先配置 enabled/disabled”的要求，也避免为了单一取值增加无效抽象。

client 端继续使用现有 bool：

```cpp
bool QuicDisable1RttEncryption;
```

client 行为保持：

- `true`：调用 `QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION`，请求禁用 1-RTT 加密。
- `false`：不调用该参数，使用正常 QUIC 加密。
- `--enable-encrypt`：仅 client 模式允许，设置 `QuicDisable1RttEncryption = false`。

server 行为改为“宽容尝试”：

1. 收到 `QUIC_LISTENER_EVENT_NEW_CONNECTION`。
2. 包装 `MsQuicConnection`。
3. 调用 server 专用 helper `TqTryDisable1RttEncryptionForServerNewConnection()` 尝试设置 `QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION`。
4. 如果成功，说明 client 也协商了禁用 1-RTT 加密，记录该连接 `encryption = "disabled"`。
5. 如果返回 `QUIC_STATUS_INVALID_STATE`，说明 client 没有协商禁用 1-RTT 加密，server 继续 `SetConfiguration`，记录该连接 `encryption = "enabled"`，不打印 error。
6. 如果返回其他失败状态，记录错误并拒绝连接。

`QUIC_STATUS_INVALID_STATE` 只在上述 helper 内、且只在 server `NEW_CONNECTION` 阶段、`SetConfiguration` 之前解释为“client 选择加密”。该状态码不能被通用 helper 在其他连接阶段复用为 `enabled`，因为 MsQuic 也可能在非法调用时机返回同一个状态。

### NEW_CONNECTION 到 CONNECTED 的状态传递

当前 server connection registry 在 `QUIC_CONNECTION_EVENT_CONNECTED` 中注册连接，而 client-choice 判定发生在 listener `NEW_CONNECTION` 阶段。实现必须显式跨回调传递 `encryption` 状态，避免 disabled 连接在注册时落回默认 `enabled`。

推荐做法是在 server connection callback context 中保存状态：

```cpp
struct TqServerConnectionContext {
    QuicServerSession* Session{nullptr};
    std::string Encryption{"enabled"};
};
```

`NEW_CONNECTION` 中：

1. 创建 `TqServerConnectionContext`。
2. 根据 `TqTryDisable1RttEncryptionForServerNewConnection()` 结果写入 `context->Encryption`。
3. 把该 context 传给 `MsQuicConnection` callback。

`QUIC_CONNECTION_EVENT_CONNECTED` 中：

1. 从 callback context 读取 `Encryption`。
2. 调用带 encryption 参数的注册入口：

```cpp
uint32_t TqRegisterServerConnection(MsQuicConnection* connection, const std::string& encryption);
```

3. registry 保存该值并用于 snapshot/admin 输出。

`QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE` 或连接对象清理路径负责释放 context，保持现有连接生命周期语义。若实现不方便改变 context 类型，可用 `HQUIC -> encryption` 临时表作为替代，但必须在 `CONNECTED` 注册时消费并删除，避免泄漏或旧状态污染复用。

## 数据模型

server connection registry 扩展：

```cpp
struct TqServerConnectionRecord {
    uint32_t Id{0};
    MsQuicConnection* Connection{nullptr};
    std::string ClientName;
    std::string RemoteAddress;
    std::string State{"connected"};
    std::string Encryption{"enabled"};
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    uint64_t ActiveTunnels{0};
    std::string LastError;
};
```

snapshot 扩展：

```cpp
struct TqServerConnectionSnapshot {
    std::string ConnectionId;
    std::string ClientName;
    std::string RemoteAddress;
    std::string State;
    std::string Encryption;
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    uint64_t ActiveTunnels{0};
    std::string LastError;
};
```

helper 建议：

```cpp
enum class TqDisable1RttEncryptionResult {
    Disabled,
    PeerDidNotOffer,
    Error,
};

struct TqDisable1RttEncryptionOutcome {
    TqDisable1RttEncryptionResult Result{TqDisable1RttEncryptionResult::Error};
    QUIC_STATUS Status{QUIC_STATUS_SUCCESS};
};
```

`PeerDidNotOffer` 必须由 `status == QUIC_STATUS_INVALID_STATE` 判断，不使用平台相关数值。

该 helper 只能服务 server `NEW_CONNECTION` 阶段。client 侧继续使用严格 helper；client 主动要求 disabled 时，任何失败都表示配置无法满足，应保留现有重试/错误行为。

## API 和 Console

`/api/v1/server/connections` 和 `/api/v1/server/connections/{id}` 每条 connection 增加：

```json
{
  "encryption": "enabled"
}
```

Admin Console：

- server overview peer 聚合保留现有 peer/client_name/remote_address 聚合逻辑。
- server connections 表新增 `encryption` 列。
- `encryption = "disabled"` 使用明确的风险样式或文本提示，标识这是不安全实验室状态。
- 如果连接对象缺失 `encryption`，前端显示空字符串，保持对旧 API 的宽容。
- 第一版 server overview/server peers 聚合表不强制展示单个 `encryption` 字段，因为同一个 client_name 聚合行可能同时包含 enabled 和 disabled 连接。后续如需展示，应使用 `enabled_connections` / `disabled_connections` 计数。

server config/admin config：

```json
{
  "quic": {
    "encryption_policy": "client-choice"
  }
}
```

client config/admin config 继续输出：

```json
{
  "proto": {
    "disable_1rtt_encryption": false
  }
}
```

## 错误处理

- server `QUIC_STATUS_INVALID_STATE` 不再作为错误日志输出，因为它是 client 选择加密的正常信号。
- server 其他失败状态继续输出错误，格式保留状态码，便于跨平台诊断。
- client 调用禁用 1-RTT 加密失败时仍保留现有重试/错误行为，因为 client 主动要求 disabled，失败表示无法满足配置。
- server CLI 使用 `--enable-encrypt` 返回配置解析错误，错误信息说明该参数只适用于 client。
- 旧 server JSON 配置中的 `proto.disable_1rtt_encryption` 继续接受但忽略，避免已有配置启动失败；生成配置和 admin config 不再输出该字段，只输出 `encryption_policy: "client-choice"`。
- server accepted 日志和 trace 连接事件应包含 `encryption=enabled|disabled`，便于不访问 Admin API 时确认协商结果。

## 跨平台要求

- Windows、Linux、macOS 均通过 MsQuic `QUIC_STATUS_INVALID_STATE` 常量判断 peer 未协商场景。
- 日志和测试不能假设 POSIX 下 `QUIC_STATUS_INVALID_STATE == 1`。
- 现有 `TqCredentialConfig` 已保证 client 使用 `QUIC_CREDENTIAL_TYPE_NONE`，只设置 CA；实现不应新增 client cert/key 要求。
- macOS/Windows 下 client CA 验证路径继续沿用现有 `QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION` 处理。
- 发布门禁至少包括 Linux 单测运行、Windows 编译或单测运行、macOS 编译或单测运行。
- 源码搜索检查不得出现用 `0x1` 判断 `QUIC_STATUS_INVALID_STATE` 的逻辑。

## 测试策略

- `tcpquic_config_router_test`
  - client `--enable-encrypt` 仍可解析并设置 `QuicDisable1RttEncryption = false`。
  - server `--enable-encrypt` 解析失败，错误信息包含 client-only 语义。
  - server 默认 config/admin 输出 `encryption_policy: "client-choice"`。
  - server JSON 配置包含旧字段 `proto.disable_1rtt_encryption` 时仍可解析，但生成配置/admin config 不再输出该字段。
- `tcpquic_server_admin_test`
  - server connection JSON 输出 `encryption`。
  - 单连接查询同样输出 `encryption`。
- `tcpquic_admin_http_test`
  - Admin Console server connections 表含 `encryption` 列。
  - JS 渲染 server connection rows 使用 `encryption` 字段。
  - disabled 状态有风险样式或文本提示。
- `tcpquic_quic_session_reconnect_test` 或新增 unit seam
  - server helper 把 `QUIC_STATUS_SUCCESS` 映射为 `disabled`。
  - server helper 把 `QUIC_STATUS_INVALID_STATE` 映射为 `enabled`。
  - helper 使用 `QUIC_STATUS_INVALID_STATE` 常量，不使用 `0x1`。
- `tcpquic_server_connection_context_test` 或既有 session test 扩展
  - `NEW_CONNECTION` 阶段得到的 `"disabled"` 状态能传递到 `CONNECTED` 注册后的 snapshot。
  - `NEW_CONNECTION` 阶段得到的 `"enabled"` 状态能传递到 `CONNECTED` 注册后的 snapshot。
- 脚本化 smoke test
  - client 默认连接 server：server `/server/connections` 显示 `encryption: "disabled"`。
  - client `--enable-encrypt` 连接 server：server `/server/connections` 显示 `encryption: "enabled"`，stderr 无“failed to disable QUIC 1-RTT encryption on server connection, 0x1”。
- 发布门禁
  - Linux 运行相关单测。
  - Windows 编译或运行相关单测。
  - macOS 编译或运行相关单测。
  - `rg` 检查没有用 `0x1` 作为状态判断。

## 风险和处理

- **安全默认变化**：server 默认接受 client 选择，允许未加密连接。文档必须明确 `encryption: disabled` 是不安全实验室模式。
- **配置语义迁移**：旧 server 配置里的 `proto.disable_1rtt_encryption` 不再表达 server 策略。server 继续接受该字段但忽略，并在生成配置/admin config 中只输出 `encryption_policy: "client-choice"`，避免启动失败影响旧配置。
- **可观测性误解**：策略和结果必须分开命名。策略叫 `encryption_policy`，单连接状态叫 `encryption`。
- **跨平台状态码差异**：只比较 MsQuic 常量，不比较数值。
- **跨回调状态丢失**：client-choice 结果产生于 `NEW_CONNECTION`，registry 注册发生在 `CONNECTED`。实现必须通过 connection context 或临时表传递状态，并用测试覆盖，否则 admin 可能错误展示默认 `enabled`。

## 评审采纳记录

- 已明确不新增单取值 `TqQuicEncryptionPolicy`，server 策略固定为 `client-choice`，仅在 config/admin 输出层用字符串表达。
- 已明确 `NEW_CONNECTION` 到 `CONNECTED` 的状态传递方案：推荐使用 server connection callback context 保存 `Encryption`，注册 connection 时传入 registry；临时表只能作为替代方案，且必须消费后删除。
- 已限定 `QUIC_STATUS_INVALID_STATE` 的解释范围：只在 server `NEW_CONNECTION` 阶段、`SetConfiguration` 之前、server 专用 helper 中解释为 client 选择正常加密。
- 已固定旧 server 配置兼容策略：server JSON 配置继续接受 `proto.disable_1rtt_encryption` 但忽略，生成配置和 admin config 只输出 `encryption_policy: "client-choice"`。
- 已把 server accepted 日志和 trace 中携带 `encryption=enabled|disabled` 纳入设计。
- 已把跨平台发布门禁、`0x1` 搜索检查、脚本化 smoke test 纳入测试策略。
- 已要求 Admin Console 对 `encryption: "disabled"` 使用风险样式或文本提示。
- 已明确 server overview/server peers 聚合表第一版不强制展示单个 `encryption` 字段；后续如需展示，使用 `enabled_connections` / `disabled_connections` 计数。
