# Server Client-Choice Encryption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 server 默认改为按 client 协商决定每条 QUIC 连接是否启用 1-RTT 加密，同时保留 client `--enable-encrypt`，并在 Admin API/Console 展示每条 server connection 的实际加密状态。

**Architecture:** server 在 `NEW_CONNECTION` 阶段宽容尝试 `QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION`：成功表示该连接 `encryption = "disabled"`，`QUIC_STATUS_INVALID_STATE` 表示 client 选择正常加密并记录 `encryption = "enabled"`，其他错误仍拒绝连接。配置展示层把 server 策略表达为 `encryption_policy = "client-choice"`，运行时 connection snapshot 单独输出实际 `encryption` 状态。

**Tech Stack:** C++17、MsQuic C++ wrapper、nlohmann/json、嵌入式 Admin Console HTML/JS、现有 CMake 单元测试目标。

---

## 文件结构

- Modify: `src/config/config.cpp`
  - server 模式拒绝 `--enable-encrypt`。
  - server JSON 配置继续接受但忽略旧字段 `proto.disable_1rtt_encryption`。
  - config dump 中 server 输出 `encryption_policy: "client-choice"`，client 继续输出 `disable_1rtt_encryption`。
- Modify: `src/runtime/admin_config.cpp`
  - server admin config quic JSON 输出 `encryption_policy`。
  - client admin config 保留 `disable_1rtt_encryption`。
- Modify: `src/runtime/router_runtime.cpp`
  - client runtime config JSON 保留 `disable_1rtt_encryption`。
- Modify: `src/protocol/quic_session.h`
  - `TqServerConnectionSnapshot` 增加 `Encryption`。
  - 测试可见 helper 暴露 server disable-1RTT 状态分类。
  - server connection 注册入口接受 `encryption` 参数，测试入口可直接注册指定状态。
- Modify: `src/protocol/quic_session.cpp`
  - server connection registry 保存 `Encryption`。
  - 新增 server connection callback context，跨 `NEW_CONNECTION` / `CONNECTED` 传递 `Encryption`。
  - server `ListenerCallback` 使用 client-choice 逻辑。
  - `TqSnapshotServerConnections()` 输出 `Encryption`。
- Modify: `src/runtime/trace.h`
  - `TqTraceQuicConnected()` 增加可选 `encryption` 参数，默认空字符串以兼容 client 调用。
- Modify: `src/runtime/trace.cpp`
  - server connected trace 输出 `encryption=enabled|disabled`。
- Modify: `src/runtime/server_admin.cpp`
  - `/server/connections` JSON 增加 `encryption`。
- Modify: `src/runtime/admin_console.cpp`
  - server connections 表增加 `encryption` 列。
  - `renderServerConnections()` 渲染字段列表增加 `encryption`。
  - disabled 状态显示风险文本或样式。
- Modify: `docs/config_guide_cn.md`
  - 说明 server `encryption_policy: client-choice`。
  - 说明 client `--enable-encrypt` / `disable_1rtt_encryption: false` 不需要 client cert/key，只需要 CA。
- Modify: `scripts/test-tcpquic-proxy.sh`
  - loopback smoke 覆盖默认 client 的 `encryption=disabled` 和 `--enable-encrypt` client 的 `encryption=enabled`。
- Test: `src/unittest/config_router_test.cpp`
- Test: `src/unittest/server_admin_test.cpp`
- Test: `src/unittest/admin_http_test.cpp`
- Test: `src/unittest/quic_session_reconnect_test.cpp`
- Test: `src/unittest/router_runtime_test.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

---

### Task 1: 配置语义红灯测试

**Files:**
- Modify: `src/unittest/config_router_test.cpp`
- Test target: `tcpquic_config_router_test`

- [ ] **Step 1: 写 server 拒绝 `--enable-encrypt` 的失败测试**

在 `src/unittest/config_router_test.cpp` 已有 server CLI 测试附近，替换当前允许 server `--enable-encrypt` 的断言。测试代码目标形态：

```cpp
    {
        TqConfig cfg;
        std::string err;
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--listen",
            "0.0.0.0:4433",
            "--allow-targets",
            "127.0.0.1/32",
            "--enable-encrypt",
            "--cert",
            "a.crt",
            "--key",
            "a.key"};
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 142;
        if (err.find("--enable-encrypt is client-only") == std::string::npos) return 143;
    }
```

- [ ] **Step 2: 写 client 保留 `--enable-encrypt` 的断言**

保留或新增 client 侧断言：

```cpp
    {
        TqConfig cfg;
        std::string err;
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--peer",
            "127.0.0.1:14444",
            "--socks",
            "127.0.0.1:11080",
            "--enable-encrypt",
            "--ca",
            "ca.crt"};
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 144;
        if (cfg.QuicDisable1RttEncryption) return 145;
    }
```

- [ ] **Step 3: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: FAIL，server `--enable-encrypt` 当前仍被接受，返回码命中 Step 1。

- [ ] **Step 4: 实现最小配置解析变更**

在 `src/config/config.cpp` 中 `--enable-encrypt` 分支加入 mode 判断。实现目标：

```cpp
        } else if (std::strcmp(arg, "--enable-encrypt") == 0) {
            if (cfg.Mode == TqMode::Server) {
                err = "--enable-encrypt is client-only";
                return false;
            }
            cfg.QuicDisable1RttEncryption = false;
```

`TqParseArgs()` 已在扫描普通选项前通过 `argv[1]` 设置 `cfg.Mode`，因此这里可以直接判断 mode，不需要延后到 validation 阶段。

- [ ] **Step 5: 运行测试确认通过**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/config/config.cpp src/unittest/config_router_test.cpp
git commit -m "config: keep enable-encrypt client-only"
```

---

### Task 2: server admin config 输出策略字段

**Files:**
- Modify: `src/unittest/config_router_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
- Modify: `src/runtime/admin_config.cpp`
- Modify: `src/config/config.cpp`
- Test target: `tcpquic_config_router_test`
- Test target: `tcpquic_router_runtime_test`

- [ ] **Step 1: 写 server config dump 失败测试**

在 server config JSON dump 测试附近加入断言，目标是 server 输出 `encryption_policy`，不输出误导性的 `disable_1rtt_encryption`：

```cpp
    {
        TqConfig cfg;
        std::string err;
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--listen",
            "0.0.0.0:4433",
            "--cert",
            "server.crt",
            "--key",
            "server.key"};
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 146;
        const auto root = nlohmann::json::parse(RuntimeConfigJson(cfg));
        if (root["proto"].value("encryption_policy", "") != "client-choice") return 147;
        if (root["proto"].contains("disable_1rtt_encryption")) return 148;
    }
```

- [ ] **Step 2: 写旧 server 配置兼容断言**

在 `src/unittest/config_router_test.cpp` 增加旧配置兼容测试，确保 server JSON 配置继续接受 `proto.disable_1rtt_encryption` 但生成配置不再输出该字段：

```cpp
    {
        const std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"server.crt","key":"server.key"},
            "server":{"proto_listen":"0.0.0.0:4433"},
            "proto":{"disable_1rtt_encryption":true}
        })json");
        TqConfig cfg;
        std::string err;
        const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 149;
        const auto root = nlohmann::json::parse(RuntimeConfigJson(cfg));
        if (root["proto"].value("encryption_policy", "") != "client-choice") return 150;
        if (root["proto"].contains("disable_1rtt_encryption")) return 151;
    }
```

- [ ] **Step 3: 写 admin config JSON 失败测试**

在 `src/unittest/router_runtime_test.cpp` 的 `TqServerRuntimeConfigJson(cfg, resolvedListens, false)` 测试块中加入：

```cpp
        if (json["quic"].value("encryption_policy", "") != "client-choice") return 964;
        if (json["quic"].contains("disable_1rtt_encryption")) return 965;
```

- [ ] **Step 4: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
rtk cmake --build build --target tcpquic_router_runtime_test -j2
rtk ./build/bin/Release/tcpquic_router_runtime_test
```

Expected: FAIL，server JSON/admin config 仍输出 `disable_1rtt_encryption` 且缺少 `encryption_policy`。

- [ ] **Step 5: 修改 config JSON 生成**

在 `src/config/config.cpp` 的 proto JSON 生成处拆分 client/server 输出。目标逻辑：

```cpp
    nlohmann::json proto{
        {"profile", QuicProfileName(cfg.QuicProfile)},
        {"connection_stream_count", cfg.QuicConnectionStreamCount},
        {"keepalive_ms", cfg.QuicKeepAliveIntervalMs},
    };
    if (cfg.Mode == TqMode::Server) {
        proto["encryption_policy"] = "client-choice";
    } else {
        proto["disable_1rtt_encryption"] = cfg.QuicDisable1RttEncryption;
        proto["connections"] = cfg.QuicConnections;
    }
```

按现有函数结构保留其他字段名和顺序约定，不改变无关 JSON。`ParseProtoConfig()` 可继续读取 `disable_1rtt_encryption` 到 `cfg.QuicDisable1RttEncryption`；server 生成路径忽略该值即可。

- [ ] **Step 6: 修改 admin config JSON 生成**

在 `src/runtime/admin_config.cpp` 的 `QuicJsonValue()` 中按 role 区分。server 输出：

```cpp
{"encryption_policy", "client-choice"}
```

client 仍输出：

```cpp
{"disable_1rtt_encryption", cfg.QuicDisable1RttEncryption}
```

- [ ] **Step 7: 运行测试确认通过**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
rtk cmake --build build --target tcpquic_router_runtime_test -j2
rtk ./build/bin/Release/tcpquic_router_runtime_test
```

Expected: PASS。

- [ ] **Step 8: Commit**

```bash
git add src/config/config.cpp src/runtime/admin_config.cpp src/unittest/config_router_test.cpp src/unittest/router_runtime_test.cpp
git commit -m "config: show server encryption policy"
```

---

### Task 3: server connection snapshot 增加 encryption 字段

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/unittest/server_admin_test.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`
- Test target: `tcpquic_server_admin_test`
- Test target: `tcpquic_tcp_tunnel_test`

- [ ] **Step 1: 写 server admin JSON 失败测试**

在 `src/unittest/server_admin_test.cpp` 的 `TqSnapshotServerConnections()` stub 中设置：

```cpp
    connection.Encryption = "disabled";
```

在 `/server/connections` list 和 get 断言附近增加：

```cpp
    if (list.find("\"encryption\":\"disabled\"") == std::string::npos) return 773;
    if (get.find("\"encryption\":\"disabled\"") == std::string::npos) return 774;
```

- [ ] **Step 2: 先给 header 加目标字段**

在 `src/protocol/quic_session.h` 的 `TqServerConnectionSnapshot` 中加入：

```cpp
    std::string Encryption;
```

同时把注册入口改为可接收状态，保留默认值避免现有调用点必须一次性全改：

```cpp
uint32_t TqRegisterServerConnection(
    MsQuicConnection* connection,
    const std::string& encryption = "enabled");

#if defined(TQ_UNIT_TESTING)
uint32_t TqRegisterServerConnectionForTest(
    HQUIC handle,
    MsQuicConnection* connection = nullptr,
    const std::string& encryption = "enabled");
#endif
```

- [ ] **Step 3: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
rtk ./build/bin/Release/tcpquic_server_admin_test
```

Expected: FAIL，JSON 尚未输出 `encryption`，或编译失败提示注册入口实现尚未匹配新签名。

- [ ] **Step 4: 实现 registry 和 JSON 输出**

在 `src/protocol/quic_session.cpp` 的 `TqServerConnectionRecord` 增加默认值：

```cpp
    std::string Encryption{"enabled"};
```

把内部注册函数改为：

```cpp
uint32_t TqRegisterServerConnectionByHandle(
    HQUIC handle,
    MsQuicConnection* connection,
    const std::string& encryption) {
    if (handle == nullptr) {
        return 0;
    }
    const uint32_t id = g_nextServerConnId.fetch_add(1);
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    TqServerConnectionRecord record;
    record.Id = id;
    record.Connection = connection;
    record.RemoteAddress = TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS);
    record.Encryption = encryption == "disabled" ? "disabled" : "enabled";
    g_serverConnIds[handle] = std::move(record);
    return id;
}
```

更新 public/test 包装函数，把 encryption 传进去：

```cpp
uint32_t TqRegisterServerConnection(MsQuicConnection* connection, const std::string& encryption) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    return TqRegisterServerConnectionByHandle(connection->Handle, connection, encryption);
}

#if defined(TQ_UNIT_TESTING)
uint32_t TqRegisterServerConnectionForTest(
    HQUIC handle,
    MsQuicConnection* connection,
    const std::string& encryption) {
    return TqRegisterServerConnectionByHandle(handle, connection, encryption);
}
#endif
```

在 `TqSnapshotServerConnections()` 复制：

```cpp
        snapshot.Encryption = item.second.Encryption;
```

在 `src/runtime/server_admin.cpp` 的 `ServerConnectionJsonValue()` 增加：

```cpp
        {"encryption", connection.Encryption},
```

在 `src/unittest/tcp_tunnel_test.cpp` 的测试替身函数签名同步增加第三个参数，并写入测试 snapshot：

```cpp
uint32_t TqRegisterServerConnectionForTest(
    HQUIC handle,
    MsQuicConnection* connection,
    const std::string& encryption) {
    ...
    snapshot.Encryption = encryption == "disabled" ? "disabled" : "enabled";
    ...
}
```

- [ ] **Step 5: 运行测试确认通过**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j2
rtk ./build/bin/Release/tcpquic_server_admin_test
rtk cmake --build build --target tcpquic_tcp_tunnel_test -j2
rtk ./build/bin/Release/tcpquic_tcp_tunnel_test
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/runtime/server_admin.cpp src/unittest/server_admin_test.cpp src/unittest/tcp_tunnel_test.cpp
git commit -m "admin: expose server connection encryption state"
```

---

### Task 4: server client-choice 握手逻辑

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/runtime/trace.h`
- Modify: `src/runtime/trace.cpp`
- Modify: `src/unittest/quic_session_reconnect_test.cpp`
- Test target: `tcpquic_quic_session_reconnect_test`

- [ ] **Step 1: 写 server 专用状态分类失败测试**

在 `src/unittest/quic_session_reconnect_test.cpp` 中加入断言，验证状态分类不依赖平台数值。不要直接比较 `const char*` 指针，使用 `std::strcmp`：

```cpp
#include <cstring>

    if (std::strcmp(TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS_SUCCESS), "disabled") != 0) return 601;
    if (std::strcmp(TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS_INVALID_STATE), "enabled") != 0) return 602;
    if (std::strcmp(TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS_OUT_OF_MEMORY), "error") != 0) return 603;
```

- [ ] **Step 2: 在 header 暴露测试 helper**

在 `src/protocol/quic_session.h` 的 `#if defined(TQ_UNIT_TESTING)` 区域加入：

```cpp
const char* TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS status);
```

- [ ] **Step 3: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: 编译失败，缺少 `TqClassifyServerNewConnectionDisable1RttStatusForTest`。

- [ ] **Step 4: 实现 server 专用状态分类 helper**

在 `src/protocol/quic_session.cpp` 中新增：

```cpp
enum class TqServerNewConnectionDisable1RttResult {
    Disabled,
    PeerDidNotOffer,
    Error,
};

TqServerNewConnectionDisable1RttResult TqClassifyServerNewConnectionDisable1RttStatus(
    QUIC_STATUS status) {
    if (QUIC_SUCCEEDED(status)) {
        return TqServerNewConnectionDisable1RttResult::Disabled;
    }
    if (status == QUIC_STATUS_INVALID_STATE) {
        return TqServerNewConnectionDisable1RttResult::PeerDidNotOffer;
    }
    return TqServerNewConnectionDisable1RttResult::Error;
}

const char* TqEncryptionStateFromServerNewConnectionDisable1RttResult(
    TqServerNewConnectionDisable1RttResult result) {
    return result == TqServerNewConnectionDisable1RttResult::Disabled ? "disabled" : "enabled";
}
```

在测试区域实现：

```cpp
#if defined(TQ_UNIT_TESTING)
const char* TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS status) {
    switch (TqClassifyServerNewConnectionDisable1RttStatus(status)) {
    case TqServerNewConnectionDisable1RttResult::Disabled:
        return "disabled";
    case TqServerNewConnectionDisable1RttResult::PeerDidNotOffer:
        return "enabled";
    case TqServerNewConnectionDisable1RttResult::Error:
        return "error";
    }
    return "error";
}
#endif
```

- [ ] **Step 5: 新增 server connection callback context**

在 `src/protocol/quic_session.cpp` 匿名 namespace 中新增 context：

```cpp
struct TqServerConnectionContext {
    QuicServerSession* Session{nullptr};
    std::string Encryption{"enabled"};
};
```

把 `QuicServerSession::ConnectionCallback()` 开头从：

```cpp
auto* session = static_cast<QuicServerSession*>(context);
```

改成：

```cpp
auto* serverContext = static_cast<TqServerConnectionContext*>(context);
QuicServerSession* session = serverContext != nullptr ? serverContext->Session : nullptr;
```

在 `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE` 分支末尾、`TqUnregisterServerConnection(connection);` 之后释放 context：

```cpp
delete serverContext;
```

只在 shutdown complete 分支释放，避免其他事件后续还需要 context。

- [ ] **Step 6: 新增无日志 server 尝试 helper**

保留现有 `TqSetDisable1RttEncryption(connection, "client")` 供 client 严格路径使用。新增 server 专用 helper，不在 `QUIC_STATUS_INVALID_STATE` 时打印 error：

```cpp
QUIC_STATUS TqTryDisable1RttEncryptionForServerNewConnection(MsQuicConnection* connection) {
    const uint8_t value = TRUE;
    return connection->SetParam(
        QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION,
        sizeof(value),
        &value);
}
```

- [ ] **Step 7: 修改 server `ListenerCallback`**

把 server `disable1RttEncryption = session->Config.QuicDisable1RttEncryption` 和后续条件禁用逻辑移除，改成无条件 client-choice 尝试。目标伪代码：

```cpp
    auto* serverContext = new (std::nothrow) TqServerConnectionContext();
    if (serverContext == nullptr) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    serverContext->Session = session;

    auto* connection = new (std::nothrow) MsQuicConnection(
        event->NEW_CONNECTION.Connection,
        CleanUpAutoDelete,
        QuicServerSession::ConnectionCallback,
        serverContext);
    if (connection == nullptr) {
        delete serverContext;
        return QUIC_STATUS_OUT_OF_MEMORY;
    }

    const QUIC_STATUS disableStatus =
        TqTryDisable1RttEncryptionForServerNewConnection(connection);
    const auto disableResult =
        TqClassifyServerNewConnectionDisable1RttStatus(disableStatus);
    serverContext->Encryption =
        TqEncryptionStateFromServerNewConnectionDisable1RttResult(disableResult);
    if (disableResult == TqServerNewConnectionDisable1RttResult::Error) {
        std::fprintf(stderr,
            "tcpquic-proxy: failed to negotiate server QUIC encryption mode, 0x%x\n",
            disableStatus);
        connection->Handle = nullptr;
        delete serverContext;
        delete connection;
        return disableStatus;
    }
```

注意分配顺序：`serverContext` 必须在 `MsQuicConnection` 构造前创建，并作为 callback context 传入构造函数。不能先用 `session` 构造 connection 后再补写 context，否则 `CONNECTED` callback 无法读取当前连接的 encryption 状态。

`SetConfiguration()` 失败或 session 已停止时，按同一清理路径释放 connection 和 context：

```cpp
    {
        std::lock_guard<std::mutex> guard(session->Lock);
        QUIC_STATUS configStatus = QUIC_STATUS_INVALID_STATE;
        if (!session->Stopping && session->Configuration) {
            configStatus = connection->SetConfiguration(*session->Configuration);
        }
        if (session->Stopping || !session->Configuration || QUIC_FAILED(configStatus)) {
            connection->Handle = nullptr;
            delete serverContext;
            delete connection;
            return QUIC_FAILED(configStatus) ? configStatus : QUIC_STATUS_INVALID_STATE;
        }
    }
```

- [ ] **Step 8: 在 `CONNECTED` 注册时保存 encryption 并输出日志/trace**

在 `QUIC_CONNECTION_EVENT_CONNECTED` 中从 context 读取 encryption：

```cpp
const std::string encryption =
    serverContext != nullptr && !serverContext->Encryption.empty()
        ? serverContext->Encryption
        : "enabled";
const uint32_t connId = TqRegisterServerConnection(connection, encryption);
```

accepted 日志加入 encryption：

```cpp
std::snprintf(
    line,
    sizeof(line),
    "tcpquic-proxy: QUIC server connection accepted from=%s conn=%u encryption=%s",
    TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
    connId,
    encryption.c_str());
```

修改 `src/runtime/trace.h` 和 `src/runtime/trace.cpp`，给 `TqTraceQuicConnected()` 增加 encryption 参数；默认值只写在 header 声明中，`.cpp` 定义不要重复默认参数：

```cpp
void TqTraceQuicConnected(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint32_t slot,
    const char* encryption = "");
```

trace 日志中仅当 `encryption` 非空时追加字段。server 调用传入：

```cpp
TqTraceQuicConnected(connection, connId, "server", 0, encryption.c_str());
```

client 调用保持现有四参数形式，依靠默认参数兼容。

- [ ] **Step 9: 保留 client 严格行为**

client `StartSlot()` 中：

```cpp
if (!scheduleRetry && configSnapshot.QuicDisable1RttEncryption &&
    !TqSetDisable1RttEncryption(newConnection.get(), "client")) {
```

保持不变。client 主动要求 disabled 时失败仍应重试并记录 `failed to disable 1-RTT encryption`。

- [ ] **Step 10: 运行测试确认通过**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: PASS。

- [ ] **Step 11: Commit**

```bash
git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/runtime/trace.h src/runtime/trace.cpp src/unittest/quic_session_reconnect_test.cpp
git commit -m "quic: let server follow client encryption choice"
```

---

### Task 5: Admin Console 展示 encryption

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Modify: `src/unittest/admin_http_test.cpp`
- Test target: `tcpquic_admin_http_test`

- [ ] **Step 1: 写 console 失败测试**

在 `src/unittest/admin_http_test.cpp` 的 server console 结构断言中加入：

```cpp
        if (html.find("<th>encryption</th>") == std::string_view::npos) return 568;
        if (js.find("['connection_id','peer','client_name','remote_address','state','encryption','active_streams','total_streams_opened','active_tunnels','last_error']") == std::string_view::npos) return 569;
        if (js.find("if (column === 'encryption')") == std::string_view::npos) return 570;
        if (js.find("disabled (insecure)") == std::string_view::npos) return 571;
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: FAIL，Console 尚未包含 `encryption` 列。

- [ ] **Step 3: 修改 server connections 表头**

在 `src/runtime/admin_console.cpp` 的 server connections table 中把表头改为包含 `encryption`：

```html
<tr><th>connection_id</th><th>peer</th><th>client_name</th><th>remote_address</th><th>state</th><th>encryption</th><th>active_streams</th><th>total_streams_opened</th><th>active_tunnels</th><th>last_error</th></tr>
```

- [ ] **Step 4: 修改渲染字段列表**

在 `renderServerConnections()` 中把字段列表改为：

```js
['connection_id','peer','client_name','remote_address','state','encryption','active_streams','total_streams_opened','active_tunnels','last_error']
```

- [ ] **Step 5: 为 disabled 加风险展示**

在 `src/runtime/admin_console.cpp` 的 `formatCell(column, value)` 中，在 state/status 分支后加入 encryption 分支：

```js
      if (column === 'encryption') {
        const normalized = String(value || '').toLowerCase();
        if (normalized === 'disabled') {
          return '<span class="state err">disabled (insecure)</span>';
        }
        if (normalized === 'enabled') {
          return '<span class="state ok">enabled</span>';
        }
        return escapeHtml(value);
      }
```

- [ ] **Step 6: 运行测试确认通过**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: PASS。

- [ ] **Step 7: Commit**

```bash
git add src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
git commit -m "admin-console: show server connection encryption"
```

---

### Task 6: 文档更新

**Files:**
- Modify: `docs/config_guide_cn.md`

- [ ] **Step 1: 更新 server 示例**

在 server 配置示例的 `proto` 块中使用：

```json
"encryption_policy": "client-choice"
```

并移除 server 示例里的 `disable_1rtt_encryption`。

- [ ] **Step 2: 更新配置项表**

把 `proto.disable_1rtt_encryption` 描述改成 client 专用：

```markdown
| `proto.disable_1rtt_encryption` | client | 不安全的 QUIC 1-RTT 加密禁用开关，默认 `true`；设为 `false` 或使用 `--enable-encrypt` 可启用正常 QUIC 包加密。client 启用加密时仍不需要配置 client cert/key，只需要 `tls.ca` 验证 server 证书。 |
```

新增 server 策略行：

```markdown
| `proto.encryption_policy` | server | server 端固定为 `client-choice`：每条入站连接按 client 是否协商禁用 1-RTT 加密决定实际 `encryption` 状态。 |
```

- [ ] **Step 3: 更新安全说明**

把原有 `proto.disable_1rtt_encryption: true` 安全提示改成：

```markdown
- client 默认 `proto.disable_1rtt_encryption: true` 会请求禁用 1-RTT 加密，不安全，只应用于隔离实验室测试。生产 client 应设置 `proto.disable_1rtt_encryption: false` 或使用 `--enable-encrypt`。
- server 默认 `proto.encryption_policy: "client-choice"`，实际连接状态可通过 Admin API/Console 的 `encryption` 字段查看，值为 `enabled` 或 `disabled`。
```

- [ ] **Step 4: 文档检查**

Run:

```bash
rtk rg -n "disable_1rtt_encryption|encryption_policy|--enable-encrypt|cert/key" docs/config_guide_cn.md
```

Expected: server 相关段落使用 `encryption_policy`，client 相关段落仍说明 `disable_1rtt_encryption` 和 `--enable-encrypt`。

- [ ] **Step 5: Commit**

```bash
git add docs/config_guide_cn.md
git commit -m "docs: describe client-choice encryption policy"
```

---

### Task 7: 全量相关验证

**Files:**
- No source edits unless a verification failure reveals a root cause.

- [ ] **Step 1: 构建相关测试目标**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_router_runtime_test tcpquic_server_admin_test tcpquic_admin_http_test tcpquic_quic_session_reconnect_test tcpquic_tcp_tunnel_test -j2
```

Expected: build succeeds。

- [ ] **Step 2: 运行相关测试**

Run:

```bash
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_server_admin_test
rtk ./build/bin/Release/tcpquic_admin_http_test
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build/bin/Release/tcpquic_tcp_tunnel_test
```

Expected: all PASS。

- [ ] **Step 3: 运行源码搜索检查**

Run:

```bash
rtk sh -c 'if rg -n "status == 0x1|QUIC_STATUS_INVALID_STATE == 1|failed to disable QUIC 1-RTT encryption on server connection" src; then exit 1; fi'
rtk rg -n "encryption_policy|\"encryption\"|disabled \\(insecure\\)" src docs/config_guide_cn.md
```

Expected:
- server 正常 client-choice 路径不再把 `QUIC_STATUS_INVALID_STATE` 作为 error。
- 没有用 `0x1` 或 `1` 判断 `QUIC_STATUS_INVALID_STATE`。
- 第一条反向断言命令无输出且返回 0。
- admin/server connection JSON 和 console 能找到 `encryption`。

- [ ] **Step 4: 扩展并运行脚本化 smoke test**

扩展现有 `scripts/test-tcpquic-proxy.sh`，复用它已有的临时证书、server/client 启停、`pick_free_tcp_port()`、`wait_log()` 和 `fail_with_logs()`。

在变量区增加：

```bash
ENCRYPT_CLIENT_PID=""
```

在 `cleanup()` 的 pid 列表中加入 `"$ENCRYPT_CLIENT_PID"`。

修改 `start_server()`，增加可选 admin listen/token file 参数：

```bash
start_server() {
    local listen_port=$1
    local allow_targets=$2
    local compress_mode=$3
    local log_file=$4
    local stdin_fifo=$5
    local admin_listen=${6:-}
    local admin_token_file=${7:-}
    local admin_args=()

    if [ -n "$admin_listen" ]; then
        admin_args+=(--admin-listen "$admin_listen")
    fi
    if [ -n "$admin_token_file" ]; then
        admin_args+=(--admin-token-file "$admin_token_file")
    fi

    mkfifo "$stdin_fifo"
    eval "exec {fd}<>\"$stdin_fifo\""
    "$BIN" server \
        --listen "127.0.0.1:${listen_port}" \
        --allow-targets "$allow_targets" \
        --cert "$TMP_DIR/server.crt" \
        --key "$TMP_DIR/server.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress "$compress_mode" \
        "${admin_args[@]}" \
        >"$log_file" 2>&1 <"$stdin_fifo" &
    echo "$! $fd"
}
```

修改 `start_client()`，删除 client `--cert` / `--key`，并增加可选加密参数：

```bash
start_client() {
    local quic_port=$1
    local http_port=$2
    local socks_port=$3
    local compress_mode=$4
    local log_file=$5
    local quic_connections=${6:-1}
    local encryption_mode=${7:-default}
    local encryption_args=()

    if [ "$encryption_mode" = "enabled" ]; then
        encryption_args+=(--enable-encrypt)
    fi

    "$BIN" client \
        --peer "127.0.0.1:${quic_port}" \
        --http-listen "127.0.0.1:${http_port}" \
        --socks-listen "127.0.0.1:${socks_port}" \
        --ca "$TMP_DIR/ca.crt" \
        --client-name "$CLIENT_NAME" \
        --connections "$quic_connections" \
        --compress "$compress_mode" \
        "${encryption_args[@]}" \
        >"$log_file" 2>&1 &
    echo "$!"
}
```

在启动 healthy server 前增加 admin 端口和 token file：

```bash
SERVER_ADMIN_PORT=$(pick_free_tcp_port)
SERVER_ADMIN_TOKEN_FILE="$TMP_DIR/server-admin-token.json"
```

把 healthy server 启动改为：

```bash
read -r SERVER_PID SERVER_STDIN_FD < <(start_server "$HEALTHY_PEER_QUIC_PORT" "127.0.0.0/8" off "$TMP_DIR/proxy-server.log" "$TMP_DIR/server.stdin" "127.0.0.1:${SERVER_ADMIN_PORT}" "$SERVER_ADMIN_TOKEN_FILE")
```

在默认 client 连接并完成 HTTP/SOCKS happy path 后，读取 admin token 并检查 disabled 连接：

```bash
SERVER_ADMIN_TOKEN="$(python3 - "$SERVER_ADMIN_TOKEN_FILE" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["token"])
PY
)"

curl -fsS \
    -H "Authorization: Bearer ${SERVER_ADMIN_TOKEN}" \
    "http://127.0.0.1:${SERVER_ADMIN_PORT}/api/v1/server/connections" \
    -o "$TMP_DIR/server-connections-disabled.json" \
    >"$TMP_DIR/curl-server-connections-disabled.log" 2>&1 ||
    fail_with_logs "server connections query failed for default client"
grep -q '"encryption":"disabled"' "$TMP_DIR/server-connections-disabled.json" ||
    fail_with_logs "expected disabled encryption for default client"
```

再启动一个 `--enable-encrypt` client，使用不同 listener 端口，并检查 enabled 连接：

```bash
ENCRYPT_HTTP_PORT=$(pick_free_tcp_port)
ENCRYPT_SOCKS_PORT=$(pick_free_tcp_port)
ENCRYPT_CLIENT_PID=$(start_client "$HEALTHY_PEER_QUIC_PORT" "$ENCRYPT_HTTP_PORT" "$ENCRYPT_SOCKS_PORT" off "$TMP_DIR/proxy-client-encrypted.log" 1 enabled)
wait_for_open_port 127.0.0.1 "$ENCRYPT_HTTP_PORT"
wait_for_open_port 127.0.0.1 "$ENCRYPT_SOCKS_PORT"

curl -fsS \
    -H "Authorization: Bearer ${SERVER_ADMIN_TOKEN}" \
    "http://127.0.0.1:${SERVER_ADMIN_PORT}/api/v1/server/connections" \
    -o "$TMP_DIR/server-connections-enabled.json" \
    >"$TMP_DIR/curl-server-connections-enabled.log" 2>&1 ||
    fail_with_logs "server connections query failed for encrypted client"
grep -q '"encryption":"enabled"' "$TMP_DIR/server-connections-enabled.json" ||
    fail_with_logs "expected enabled encryption for --enable-encrypt client"
```

最后加入旧错误日志反断言：

```bash
if grep -q "failed to disable QUIC 1-RTT encryption on server connection" "$TMP_DIR/proxy-server.log"; then
    fail_with_logs "server logged old disable-1RTT error for client-choice"
fi
```

Run:

```bash
rtk bash scripts/test-tcpquic-proxy.sh
```

Expected: PASS，脚本同时覆盖默认 client 的 `encryption=disabled` 和 `--enable-encrypt` client 的 `encryption=enabled`。

- [ ] **Step 5: 记录跨平台门禁**

在 PR 或执行记录中保留以下结果：

```text
Linux: related tests PASS
Windows: build or related tests PASS
macOS: build or related tests PASS
```

如果当前机器只能运行 Linux，必须在最终交付说明中明确 Windows/macOS 尚需 CI 或对应机器补跑，不能宣称全平台已验证。

- [ ] **Step 6: Commit verification note if needed**

如果测试修复过程中有额外代码修改：

```bash
git add <changed-files>
git commit -m "test: cover server client-choice encryption"
```

如果没有额外修改，不创建空 commit。

---

## 自检记录

- Spec coverage: 计划覆盖 server 默认 client-choice、client 保留 `--enable-encrypt`、client 无 cert/key 要求、server config 策略输出、旧 server 配置兼容、server runtime connection `encryption = enabled|disabled`、跨回调状态传递、日志/trace、Admin API/Console 展示、disabled 风险提示、脚本化 smoke、Windows/Linux/macOS 常量判断和发布门禁。
- Completeness scan: 文档不包含未决条目或延后实现说明。
- Type consistency: 运行时字段统一为 `Encryption` / JSON `encryption`，取值统一为 `enabled` 或 `disabled`；策略字段统一为 JSON `encryption_policy`，取值 `client-choice`。
