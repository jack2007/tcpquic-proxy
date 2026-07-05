# Client Name Handshake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 client 增加可配置名称，并在 QUIC connection 建立后立即通过 HELLO 控制 stream 上报给 server，让 server Admin Console 优先展示有意义的 client 名称。

**Architecture:** 在现有 `control_protocol` 中新增连接级 `TQ_CMD_CLIENT_HELLO` 帧，client connection connected 后发送一次 HELLO，server 收到后更新 server connection registry。配置层增加 `client_name`，admin API 和 console 展示优先使用 `client_name`，为空时保留当前 remote address fallback。

**Tech Stack:** C++17、msquic C++ wrapper、现有 control frame 编解码、nlohmann/json、嵌入式 Admin Console JS、现有单元测试二进制。

---

## 文件结构

- Modify: `src/config/config.h`
  - `TqConfig` 与 `TqPeerConfig` 增加 `ClientName`。
- Modify: `src/config/config.cpp`
  - 增加 client name 校验 helper。
  - CLI 增加 `--client-name`。
  - client/router/runtime peer JSON 增加 `client_name`。
  - 单 peer 与多 peer fallback 规则落地。
- Modify: `src/runtime/client_peer_runtime.h`
  - `TqMakePrimaryPeerConfig()` 和 `TqMakePeerRuntimeConfig()` 传递 client name。
- Modify: `src/protocol/control_protocol.h`
  - 新增 HELLO command、常量、结构体、encode/decode 声明。
- Modify: `src/protocol/control_protocol.cpp`
  - 实现 HELLO 编解码和名称校验。
- Modify: `src/protocol/quic_session.h`
  - `TqServerConnectionSnapshot` 增加 `ClientName`。
  - 声明 `TqSetServerConnectionClientName()`。
  - `QuicClientSession` 增加发送 HELLO 的私有 helper。
- Modify: `src/protocol/quic_session.cpp`
  - server connection registry 保存 client name。
  - client connected 后发送 HELLO。
- Modify: `src/tunnel/tcp_tunnel.cpp`
  - server stream open 读取首帧时识别 HELLO，更新 connection metadata，不注册 tunnel。
- Modify: `src/runtime/server_admin.cpp`
  - `/server/connections` JSON 输出 `client_name`。
- Modify: `src/runtime/admin_console.cpp`
  - server overview/peers/connections 优先按 `client_name` 展示和聚合。
- Modify: tests
  - `src/unittest/control_protocol_test.cpp`
  - `src/unittest/config_router_test.cpp`
  - `src/unittest/admin_http_test.cpp`
  - `src/unittest/quic_session_reconnect_test.cpp`
- Modify: docs
  - `README.md`
  - `docs/config_guide_cn.md`
  - `docs/server-admin-console.md`

---

### Task 1: 给 HELLO 帧写失败测试

**Files:**
- Modify: `src/unittest/control_protocol_test.cpp`
- Modify: `src/protocol/control_protocol.h`

- [ ] **Step 1: 在测试中加入 HELLO 编解码断言**

在 `src/unittest/control_protocol_test.cpp` 的 `TqOpenResponse` 测试后添加：

```cpp
    TqClientHello hello{};
    hello.ClientName = "office-a";
    std::vector<uint8_t> hbuf;
    if (!TqEncodeClientHello(hello, hbuf)) {
        return 201;
    }
    TqClientHello hdec{};
    if (!TqDecodeClientHello(hbuf.data(), hbuf.size(), hdec)) {
        return 202;
    }
    if (hdec.ClientName != "office-a") {
        return 203;
    }

    TqClientHello emptyHello{};
    if (TqEncodeClientHello(emptyHello, hbuf)) {
        return 204;
    }
    TqClientHello badHello{};
    badHello.ClientName = "bad name";
    if (TqEncodeClientHello(badHello, hbuf)) {
        return 205;
    }
    badHello.ClientName = std::string(65, 'a');
    if (TqEncodeClientHello(badHello, hbuf)) {
        return 206;
    }

    std::vector<uint8_t> invalidHello = {
        TQ_MAGIC_0, TQ_MAGIC_1, TQ_VERSION, TQ_CMD_CLIENT_HELLO, 0, 3, 'a', ' ', 'b'};
    if (TqDecodeClientHello(invalidHello.data(), invalidHello.size(), hdec)) {
        return 207;
    }
    if (TqDecodeClientHello(hbuf.data(), hbuf.size() - 1, hdec)) {
        return 208;
    }
```

- [ ] **Step 2: 在 header 先声明预期接口**

在 `src/protocol/control_protocol.h` 中先加入接口声明，让测试表达目标 API：

```cpp
static constexpr uint8_t TQ_CMD_CLIENT_HELLO = 0x20;
static constexpr size_t TQ_CLIENT_HELLO_MIN_SIZE = 6;
static constexpr size_t TQ_CLIENT_HELLO_MAX_NAME_LEN = 64;

struct TqClientHello {
    std::string ClientName;
};

bool TqEncodeClientHello(const TqClientHello& msg, std::vector<uint8_t>& out);
bool TqDecodeClientHello(const uint8_t* data, size_t len, TqClientHello& out);
bool TqIsValidClientName(const std::string& name);
```

- [ ] **Step 3: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_control_test -j2
rtk ./build/bin/Release/tcpquic_control_test
```

Expected: 编译失败，缺少 `TqEncodeClientHello` / `TqDecodeClientHello` / `TqIsValidClientName` 实现。

---

### Task 2: 实现 HELLO 编解码

**Files:**
- Modify: `src/protocol/control_protocol.cpp`
- Modify: `src/protocol/control_protocol.h`
- Test: `src/unittest/control_protocol_test.cpp`

- [ ] **Step 1: 实现名称校验 helper**

在 `src/protocol/control_protocol.cpp` 匿名 namespace 后或文件末尾添加：

```cpp
bool TqIsValidClientName(const std::string& name) {
    if (name.empty() || name.size() > TQ_CLIENT_HELLO_MAX_NAME_LEN) {
        return false;
    }
    for (const unsigned char ch : name) {
        const bool ok =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '.' || ch == '_' || ch == '-' || ch == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 2: 实现 encode/decode**

继续添加：

```cpp
bool TqEncodeClientHello(const TqClientHello& msg, std::vector<uint8_t>& out) {
    if (!TqIsValidClientName(msg.ClientName)) {
        return false;
    }
    out.clear();
    out.reserve(TQ_CLIENT_HELLO_MIN_SIZE + msg.ClientName.size());
    out.push_back(TQ_MAGIC_0);
    out.push_back(TQ_MAGIC_1);
    out.push_back(TQ_VERSION);
    out.push_back(TQ_CMD_CLIENT_HELLO);
    WriteU16BE(out, static_cast<uint16_t>(msg.ClientName.size()));
    out.insert(out.end(), msg.ClientName.begin(), msg.ClientName.end());
    return out.size() == TQ_CLIENT_HELLO_MIN_SIZE + msg.ClientName.size();
}

bool TqDecodeClientHello(const uint8_t* data, size_t len, TqClientHello& out) {
    if (data == nullptr || len < TQ_CLIENT_HELLO_MIN_SIZE) {
        return false;
    }
    if (data[0] != TQ_MAGIC_0 || data[1] != TQ_MAGIC_1) {
        return false;
    }
    if (data[2] != TQ_VERSION || data[3] != TQ_CMD_CLIENT_HELLO) {
        return false;
    }
    const uint16_t nameLen = ReadU16BE(data + 4);
    if (len != TQ_CLIENT_HELLO_MIN_SIZE + nameLen) {
        return false;
    }
    TqClientHello msg{};
    msg.ClientName.assign(
        reinterpret_cast<const char*>(data + TQ_CLIENT_HELLO_MIN_SIZE),
        static_cast<size_t>(nameLen));
    if (!TqIsValidClientName(msg.ClientName)) {
        return false;
    }
    out = std::move(msg);
    return true;
}
```

- [ ] **Step 3: 运行 control protocol 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_control_test -j2
rtk ./build/bin/Release/tcpquic_control_test
```

Expected: PASS。

- [ ] **Step 4: Commit**

```bash
git add src/protocol/control_protocol.h src/protocol/control_protocol.cpp src/unittest/control_protocol_test.cpp
git commit -m "feat: add client hello control frame"
```

---

### Task 3: 配置层增加 client_name

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/runtime/client_peer_runtime.h`
- Test: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: 写配置解析失败测试**

在 `src/unittest/config_router_test.cpp` usage 断言中加入：

```cpp
        if (usage.find("--client-name <name>") == std::string::npos) return 401;
```

在现有 client config parse 用例后加入：

```cpp
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"agent-b","client_name":"office-a","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 402;
        if (cfg.Router.Peers.size() != 1) return 403;
        if (cfg.Router.Peers[0].ClientName != "office-a") return 404;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--ca", "ca.crt", "--client-name", "edge-a"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 405;
        if (cfg.ClientName != "edge-a") return 406;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--ca", "ca.crt", "--client-name", "bad name"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 407;
        if (err.find("client-name") == std::string::npos) return 408;
    }
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: 编译失败或测试失败，因为 `ClientName` 字段和解析尚未实现。

- [ ] **Step 3: 增加配置字段**

在 `src/config/config.h` 中添加：

```cpp
struct TqPeerConfig {
    std::string PeerId;
    std::string ClientName;
    std::string QuicPeer;
    ...
};

struct TqConfig {
    ...
    std::string ClientName;
    std::string QuicPeer;
    ...
};
```

- [ ] **Step 4: 更新 usage 和 CLI 解析**

在 `TqPrintUsage()` client specific 区块加入：

```cpp
        "  --client-name <name>        Display name sent to server console\n"
```

在 `TqParseArgs()` client options 中加入：

```cpp
        } else if (GetOptionValue(arg, "--client-name", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--client-name", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.ClientName = value;
            if (!TqIsValidClientName(cfg.ClientName)) {
                err = "invalid value for --client-name";
                return false;
            }
```

同时在文件顶部 include：

```cpp
#include "control_protocol.h"
```

- [ ] **Step 5: 更新 JSON peer 解析**

在 `ParsePeer()` 和 `ParseRuntimePeer()` 中都加入：

```cpp
            } else if (key == "client_name") {
                if (!ReadString(item.value(), peer.ClientName) ||
                    (!peer.ClientName.empty() && !TqIsValidClientName(peer.ClientName))) {
                    return Error("invalid client_name");
                }
```

- [ ] **Step 6: 落地 fallback 规则**

在 `TqValidateRouterConfig()` 检查单个 peer 时加入：

```cpp
        if (!peer.ClientName.empty() && !TqIsValidClientName(peer.ClientName)) {
            err = "invalid client_name for " + peer.PeerId;
            return false;
        }
```

在 `TqMakePrimaryPeerConfig()` 设置：

```cpp
    peer.ClientName = cfg.ClientName.empty() ? peer.PeerId : cfg.ClientName;
```

在 `TqMakePeerRuntimeConfig()` 设置：

```cpp
    cfg.ClientName = peer.ClientName.empty() ? peer.PeerId : peer.ClientName;
```

- [ ] **Step 7: 运行配置测试**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: PASS。

- [ ] **Step 8: Commit**

```bash
git add src/config/config.h src/config/config.cpp src/runtime/client_peer_runtime.h src/unittest/config_router_test.cpp
git commit -m "feat: configure client display name"
```

---

### Task 4: server registry 保存 client_name

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Test: `src/unittest/quic_session_reconnect_test.cpp`

- [ ] **Step 1: 写 registry 单测**

在 `src/unittest/quic_session_reconnect_test.cpp` 中新增测试函数：

```cpp
static int TestServerConnectionClientNameSnapshot() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x2300));
    if (TqRegisterServerConnection(conn) == 0) return 230;
    if (!TqSetServerConnectionClientName(conn, "office-a")) return 231;
    const auto snapshots = TqSnapshotServerConnections();
    bool found = false;
    for (const auto& snapshot : snapshots) {
        if (snapshot.ClientName == "office-a") {
            found = true;
        }
    }
    TqUnregisterServerConnection(conn);
    return found ? 0 : 232;
}
```

在 `main()` 调用该测试：

```cpp
    if ((rc = TestServerConnectionClientNameSnapshot()) != 0) return rc;
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: 编译失败，缺少 snapshot 字段和 setter。

- [ ] **Step 3: 增加 snapshot 字段和 setter 声明**

在 `src/protocol/quic_session.h` 中：

```cpp
struct TqServerConnectionSnapshot {
    std::string ConnectionId;
    std::string ClientName;
    std::string RemoteAddress;
    ...
};

bool TqSetServerConnectionClientName(MsQuicConnection* connection, const std::string& clientName);
```

- [ ] **Step 4: 实现 registry 存储**

在 `TqServerConnectionRecord` 增加：

```cpp
std::string ClientName;
```

实现 setter：

```cpp
bool TqSetServerConnectionClientName(MsQuicConnection* connection, const std::string& clientName) {
    if (connection == nullptr || connection->Handle == nullptr || !TqIsValidClientName(clientName)) {
        return false;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    auto it = g_serverConnIds.find(connection->Handle);
    if (it == g_serverConnIds.end()) {
        return false;
    }
    it->second.ClientName = clientName;
    return true;
}
```

在 `TqSnapshotServerConnections()` 填充：

```cpp
snapshot.ClientName = item.second.ClientName;
```

- [ ] **Step 5: 运行 session 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp
git commit -m "feat: track server connection client names"
```

---

### Task 5: client connected 后发送 HELLO

**Files:**
- Modify: `src/protocol/quic_session.cpp`

- [ ] **Step 1: 增加发送 helper**

在 `QuicClientSession` private 区域声明：

```cpp
static void SendClientHello(MsQuicConnection* connection, const std::string& clientName);
```

在 `quic_session.cpp` 实现：

```cpp
void QuicClientSession::SendClientHello(MsQuicConnection* connection, const std::string& clientName) {
    if (connection == nullptr || connection->Handle == nullptr || clientName.empty()) {
        return;
    }
    TqClientHello hello{clientName};
    std::vector<uint8_t> payload;
    if (!TqEncodeClientHello(hello, payload)) {
        std::fprintf(stderr, "tcpquic-proxy: invalid client name, skip hello\n");
        return;
    }
    auto* stream = new (std::nothrow) MsQuicStream(
        *connection,
        QUIC_STREAM_OPEN_FLAG_NONE,
        CleanUpAutoDelete,
        nullptr,
        nullptr);
    if (stream == nullptr || !stream->IsValid()) {
        delete stream;
        return;
    }
    QUIC_BUFFER buffer{};
    buffer.Buffer = payload.data();
    buffer.Length = static_cast<uint32_t>(payload.size());
    const QUIC_STATUS status = stream->Send(&buffer, 1, QUIC_SEND_FLAG_FIN, nullptr);
    if (QUIC_FAILED(status)) {
        stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
}
```

如果 `MsQuicStream` 构造函数必须带 callback，则增加一个最小 callback，处理 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 后让 `CleanUpAutoDelete` 清理。

- [ ] **Step 2: connected 事件触发发送**

在 `QUIC_CONNECTION_EVENT_CONNECTED` 中，`OnSlotConnected` 后加入：

```cpp
        {
            std::string clientName;
            QuicClientSession* session = AcquireLiveSession(slotContext->Gate);
            if (session != nullptr) {
                std::lock_guard<std::mutex> guard(session->ConfigLock);
                clientName = session->Config.ClientName;
                ReleaseLiveSession(slotContext->Gate);
            }
            QuicClientSession::SendClientHello(connection, clientName);
        }
```

实现时注意 `AcquireLiveSession()` 后必须在所有路径调用 `ReleaseLiveSession()`；如不方便，可把 `ClientName` 放入 `ClientConnContext`，在 `StartSlot()` 创建 context 时复制，避免 callback 再拿 session。

- [ ] **Step 3: 构建 quic session 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: PASS。

- [ ] **Step 4: Commit**

```bash
git add src/protocol/quic_session.h src/protocol/quic_session.cpp
git commit -m "feat: send client hello after connect"
```

---

### Task 6: server 识别 HELLO control stream

**Files:**
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

- [ ] **Step 1: 增加 server open 识别逻辑**

在 server 处理首帧 OPEN 的路径中，找到调用 `TqDecodeOpenRequest(...)` 前的首帧数据检查位置。加入：

```cpp
        TqClientHello hello{};
        if (TqDecodeClientHello(data, len, hello)) {
            if (TqSetServerConnectionClientName(QuicConn, hello.ClientName)) {
                std::fprintf(stderr,
                    "tcpquic-proxy: QUIC server client hello name=%s conn=%u\n",
                    hello.ClientName.c_str(),
                    TqLookupServerConnectionId(QuicConn));
            }
            ShutdownComplete = true;
            StateChanged.notify_all();
            return;
        }
```

实际变量名以 `tcp_tunnel.cpp` 中 server OPEN 首帧 buffer 为准；要求是 HELLO stream 不进入 dial reactor、不调用 `RegisterWithConnectionIfNeeded()`、不写 tunnel registry。

- [ ] **Step 2: 对非法非 OPEN 帧保持现有行为**

确认 HELLO decode 失败后继续走原有 `TqDecodeOpenRequest()`，非法 stream 仍按现有错误路径关闭。

- [ ] **Step 3: 添加单元测试或最小回归测试**

如果 `tcp_tunnel_test.cpp` 已有 server OPEN 首帧测试，新增一个 HELLO 首帧用例：

```cpp
TqClientHello hello{"office-a"};
std::vector<uint8_t> payload;
if (!TqEncodeClientHello(hello, payload)) return 601;
// 构造 server-side tunnel context，喂入 payload，断言不会产生 dial request，
// 且 TqSetServerConnectionClientName 后 snapshot 可见 office-a。
```

如果现有测试工具不容易构造该路径，本任务允许只跑端到端脚本覆盖，但必须在 commit message 或后续任务记录测试缺口。

- [ ] **Step 4: 运行 tunnel 相关测试**

Run:

```bash
rtk cmake --build build --target tcpquic_tunnel_test -j2
rtk ./build/bin/Release/tcpquic_tunnel_test
```

Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git add src/tunnel/tcp_tunnel.cpp src/unittest/tcp_tunnel_test.cpp
git commit -m "feat: accept client hello on server streams"
```

---

### Task 7: Admin API 和 Console 展示 client_name

**Files:**
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/runtime/admin_console.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: server admin JSON 增加字段**

在 `ServerConnectionJsonValue()` 中加入：

```cpp
        {"client_name", connection.ClientName},
```

- [ ] **Step 2: console JS 优先使用 client_name**

把 `groupServerPeers()` 中 key 计算改为：

```javascript
        const key = connection.client_name || peerNameFromRemote(connection.remote_address);
```

把 item 增加字段：

```javascript
          client_name: connection.client_name || '',
```

把 `renderServerConnections()` 的 row 映射改为：

```javascript
        peer: row.client_name || peerNameFromRemote(row.remote_address),
```

- [ ] **Step 3: console 表格列增加 client_name**

server overview / server peers 可保持 `peer` 和 `remote_address` 两列；server connections 表头改为：

```html
<th>connection_id</th><th>peer</th><th>client_name</th><th>remote_address</th>...
```

render columns 改为：

```javascript
['connection_id','peer','client_name','remote_address','state','active_streams','total_streams_opened','active_tunnels','last_error']
```

- [ ] **Step 4: 添加 admin console 静态断言**

在 `src/unittest/admin_http_test.cpp` console HTML/JS 断言区加入：

```cpp
        if (html.find("<th>client_name</th>") == std::string_view::npos) return 701;
        if (js.find("connection.client_name || peerNameFromRemote(connection.remote_address)") == std::string_view::npos) return 702;
        if (js.find("peer: row.client_name || peerNameFromRemote(row.remote_address)") == std::string_view::npos) return 703;
```

- [ ] **Step 5: 运行 admin 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/runtime/server_admin.cpp src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp
git commit -m "feat: show client names in server console"
```

---

### Task 8: 文档和端到端验证

**Files:**
- Modify: `README.md`
- Modify: `docs/config_guide_cn.md`
- Modify: `docs/server-admin-console.md`

- [ ] **Step 1: README CLI 参数表增加 client name**

在 client 参数表加入：

```markdown
| `--client-name` | client | `primary` / `peer_id` | 上报给 server console 的展示名称；只用于展示，不作为认证身份 |
```

- [ ] **Step 2: config guide 增加 JSON 示例**

在 client peer JSON 示例中加入：

```json
{
  "peer_id": "office-a",
  "client_name": "office-a",
  "quic_peer": "proxy.example.com:443",
  "socks_listen": "127.0.0.1:1080"
}
```

并写明合法字符和 fallback：

```markdown
`client_name` 允许字母、数字、`.`、`_`、`-`、`:`，最长 64 字节。未配置时多 peer 模式使用 `peer_id`，单 peer CLI 模式使用 `primary`。
```

- [ ] **Step 3: server console 文档说明展示行为**

在 `docs/server-admin-console.md` server peers/connections 部分加入：

```markdown
server peer 名称优先来自 client 上报的 `client_name`。未收到上报或老版本 client 连接时，console 回退到 `peer-${remote_address}`。`client_name` 是展示字段，不是认证身份。
```

- [ ] **Step 4: 运行完整相关测试**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy tcpquic_control_test tcpquic_config_router_test tcpquic_quic_session_reconnect_test tcpquic_admin_http_test tcpquic_tunnel_test -j2
rtk ./build/bin/Release/tcpquic_control_test
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build/bin/Release/tcpquic_admin_http_test
rtk ./build/bin/Release/tcpquic_tunnel_test
```

Expected: 全部 PASS。

- [ ] **Step 5: 运行端到端脚本**

Run:

```bash
CLIENT_NAME=office-a rtk ./scripts/test-tcpquic-proxy.sh
```

Expected: 脚本 PASS；如果脚本暂不支持 `CLIENT_NAME`，先用现有脚本确认无回归，再增加脚本参数传递。

- [ ] **Step 6: Commit**

```bash
git add README.md docs/config_guide_cn.md docs/server-admin-console.md scripts/test-tcpquic-proxy.sh
git commit -m "docs: document client names in server console"
```

---

## Self-Review

- Spec coverage: 配置、协议、server registry、Admin API、console 展示、兼容和测试均有对应任务。
- Placeholder scan: 计划没有留下未完成说明；Task 6 的测试部分明确允许现有测试工具不足时记录缺口，但实现路径和验证命令已给出。
- Type consistency: 文档统一使用 `ClientName` C++ 字段、`client_name` JSON 字段、`TQ_CMD_CLIENT_HELLO` 协议命令。

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-05-client-name-handshake.md`. Two execution options:

1. Subagent-Driven (recommended) - dispatch a fresh subagent per task, review between tasks, fast iteration

2. Inline Execution - execute tasks in this session using executing-plans, batch execution with checkpoints
