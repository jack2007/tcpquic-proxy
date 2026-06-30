# Client Admin Stream Metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 client admin overview/peers 的 `active_streams`、`total_streams` 始终为 0，并补齐 client tunnel 的 peer/connection 归属。

**Architecture:** client peer 的 `active_streams` 从 admin snapshot 时的 connection `active_tunnels` 聚合得到；`total_streams` 在 tunnel 创建成功后用 peer runtime 内的 relaxed atomic 单调递增。`peer_id` 和 `connection_id` 在 tunnel 创建边界写入 context，relay 启动后复制到现有 tunnel registry metadata；relay 数据转发循环不新增 mutex。

**Tech Stack:** C++17, MsQuic C++ wrapper, existing `TqClientPeerRuntime`, `QuicClientSession`, `TqTunnelRegistry`, CMake unit test executables.

---

## File Structure

- Modify: `src/protocol/quic_session.h`
  增加 `TqClientPickedConnection` 和 `QuicClientSession::PickConnectionWithId()`。
- Modify: `src/protocol/quic_session.cpp`
  实现连接选择时同步返回 `conn-N`，保留现有 `PickConnection()` 兼容。
- Modify: `src/runtime/client_peer_runtime.h`
  增加 peer runtime 的 `std::atomic<uint64_t> TotalStreams`。
- Modify: `src/runtime/client_peer_runtime.cpp`
  使用 `PickConnectionWithId()` 启动 tunnel；成功后递增 total；snapshot 聚合 active。
- Modify: `src/tunnel/client_tunnel_open.h`
  增加 `TqClientTunnelMetadata` 和带 metadata 的 async tunnel 启动 overload；保留现有 5 参数签名。
- Modify: `src/tunnel/tcp_tunnel.cpp`
  `TqTunnelContext` 保存 client metadata；`UpdateRegistryMetadata()` 对 client role 写入 peer/connection。
- Test: `src/unittest/quic_session_reconnect_test.cpp`
  覆盖 `PickConnectionWithId()`。
- Test: `src/unittest/client_peer_runtime_test.cpp`
  覆盖 peer metrics active/total 语义。
- Test: `src/unittest/tunnel_registry_test.cpp` 或 `src/unittest/tcp_tunnel_test.cpp`
  覆盖 client metadata 进入 tunnel snapshot。
- Test: `src/unittest/router_runtime_test.cpp`
  覆盖 `/api/v1/peers` 输出 live stream 指标。

---

### Task 1: Return Connection Id With Picked Client Connection

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Test: `src/unittest/quic_session_reconnect_test.cpp`

- [ ] **Step 1: Write the failing test**

Add this test in `src/unittest/quic_session_reconnect_test.cpp` after `TestConnectionSnapshotAndSlotControls()`:

```cpp
static int TestPickConnectionWithIdReturnsSlotId() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(2);

    MsQuicConnection first;
    MsQuicConnection second;
    session.MarkSlotConnectedForTest(0, &first);
    session.MarkSlotConnectedForTest(1, &second);

    const auto picked = session.PickConnectionWithId();
    if (picked.Connection != &first && picked.Connection != &second) return 130;
    if (picked.Connection == &first && picked.ConnectionId != "conn-0") return 131;
    if (picked.Connection == &second && picked.ConnectionId != "conn-1") return 132;

    session.Stop();
    return 0;
}

static int TestPickConnectionWithIdReturnsEmptyWhenDisconnected() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);

    const auto picked = session.PickConnectionWithId();
    if (picked.Connection != nullptr) return 140;
    if (!picked.ConnectionId.empty()) return 141;

    session.Stop();
    return 0;
}
```

Wire both tests into `main()`:

```cpp
result = TestPickConnectionWithIdReturnsSlotId();
if (result != 0) return result;
result = TestPickConnectionWithIdReturnsEmptyWhenDisconnected();
if (result != 0) return result;
```

- [ ] **Step 2: Run test and verify it fails to compile**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_quic_session_reconnect_test -j 8
```

Expected: compile fails because `QuicClientSession::PickConnectionWithId()` is not declared.

- [ ] **Step 3: Add the public result type and method declaration**

In `src/protocol/quic_session.h`, after `struct TqServerConnectionSnapshot`, add:

```cpp
struct TqClientPickedConnection {
    MsQuicConnection* Connection{nullptr};
    std::string ConnectionId;
};
```

In `class QuicClientSession`, near `PickConnection()`, add:

```cpp
TqClientPickedConnection PickConnectionWithId();
```

- [ ] **Step 4: Implement `PickConnectionWithId()` and preserve `PickConnection()`**

In `src/protocol/quic_session.cpp`, replace the body of `QuicClientSession::PickConnection()` with:

```cpp
MsQuicConnection* QuicClientSession::PickConnection() {
    return PickConnectionWithId().Connection;
}
```

Add this implementation next to `PickConnection()`:

```cpp
TqClientPickedConnection QuicClientSession::PickConnectionWithId() {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (State->Slots.empty()) {
        return {};
    }
    const size_t count = State->Slots.size();
    const size_t start = State->NextPickIndex % count;
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t index = (start + offset) % count;
        auto& slot = State->Slots[index];
        if (auto* connection = PickableConnectionLocked(slot)) {
            State->NextPickIndex = (index + 1) % count;
            TqClientPickedConnection picked;
            picked.Connection = connection;
            picked.ConnectionId = slot.ConnectionId.empty() ? MakeConnectionId(index) : slot.ConnectionId;
            return picked;
        }
    }
    return {};
}
```

- [ ] **Step 5: Run the reconnect test**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_quic_session_reconnect_test -j 8
rtk ./build-msquic-perf/src/tcpquic_quic_session_reconnect_test
```

Expected: exit code 0.

- [ ] **Step 6: Commit this task**

```bash
git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp
git commit -m "admin: expose client connection id when picking tunnel connection"
```

---

### Task 2: Track Client Peer Total Streams Without Hot-Path Locks

**Files:**
- Modify: `src/runtime/client_peer_runtime.h`
- Modify: `src/runtime/client_peer_runtime.cpp`
- Test: `src/unittest/client_peer_runtime_test.cpp`

- [ ] **Step 1: Write the failing peer metrics aggregation test**

Add a test-only helper declaration to `src/runtime/client_peer_runtime.h` inside `class TqClientPeerRuntime`, under `#if defined(TQ_UNIT_TESTING)`:

```cpp
void IncrementTotalStreamsForTest(uint64_t count = 1);
```

Add this test in `src/unittest/client_peer_runtime_test.cpp` near other peer runtime tests:

```cpp
static int TestSnapshotPeerMetricsIncludesStreamTotals() {
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:0";
    cfg.QuicConnections = 1;

    TqClientIngressReactor ingress;
    auto runtime = std::make_shared<TqClientPeerRuntime>("peer-streams", cfg, &ingress);
    runtime->IncrementTotalStreamsForTest(3);

    const TqPeerMetrics metrics = runtime->SnapshotPeerMetrics();
    if (metrics.PeerId != "peer-streams") return 2101;
    if (metrics.TotalStreams != 3) return 2102;
    if (metrics.ActiveStreams != 0) return 2103;
    return 0;
}
```

Wire it into `main()`:

```cpp
result = TestSnapshotPeerMetricsIncludesStreamTotals();
if (result != 0) return result;
```

- [ ] **Step 2: Run test and verify it fails to compile**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_client_peer_runtime_test -j 8
```

Expected: compile fails because `IncrementTotalStreamsForTest()` and `TotalStreams` do not exist.

- [ ] **Step 3: Add the atomic counter field**

In `src/runtime/client_peer_runtime.h`, include atomic if not already available:

```cpp
#include <atomic>
```

Add this private field to `TqClientPeerRuntime`:

```cpp
std::atomic<uint64_t> TotalStreams{0};
```

Under `#if defined(TQ_UNIT_TESTING)`, define the helper declaration:

```cpp
void IncrementTotalStreamsForTest(uint64_t count = 1);
```

- [ ] **Step 4: Implement total stream snapshot and test helper**

In `src/runtime/client_peer_runtime.cpp`, update `SnapshotPeerMetrics()`:

```cpp
TqPeerMetrics TqClientPeerRuntime::SnapshotPeerMetrics() const {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    TqPeerMetrics metrics;
    metrics.PeerId = PeerId;
    metrics.QuicPeer = Config.QuicPeer;
    metrics.SocksListen = Config.SocksListen;
    metrics.HttpListen = Config.HttpListen;
    metrics.PortForwards = Config.PortForwards;
    metrics.ConnectionCount = Quic ? Quic->ConnectionCount() : 0;
    metrics.ConnectedConnections = Quic ? Quic->ConnectedConnectionCount() : 0;
    metrics.State = metrics.ConnectedConnections > 0 ? "healthy" : "connecting";
    if (Quic) {
        for (const auto& connection : Quic->SnapshotConnections()) {
            metrics.ActiveStreams += connection.ActiveTunnels;
        }
    }
    metrics.TotalStreams = TotalStreams.load(std::memory_order_relaxed);
    return metrics;
}
```

Add the test helper:

```cpp
#if defined(TQ_UNIT_TESTING)
void TqClientPeerRuntime::IncrementTotalStreamsForTest(uint64_t count) {
    TotalStreams.fetch_add(count, std::memory_order_relaxed);
}
#endif
```

- [ ] **Step 5: Run the client peer runtime test**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_client_peer_runtime_test -j 8
rtk ./build-msquic-perf/src/tcpquic_client_peer_runtime_test
```

Expected: exit code 0.

- [ ] **Step 6: Commit this task**

```bash
git add src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp src/unittest/client_peer_runtime_test.cpp
git commit -m "admin: expose client peer stream counters"
```

---

### Task 3: Attach Peer And Connection Metadata To Client Tunnels

**Files:**
- Modify: `src/tunnel/client_tunnel_open.h`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/runtime/client_peer_runtime.cpp`
- Test: `src/unittest/client_tunnel_open_test.cpp`
- Test: `src/unittest/tunnel_registry_test.cpp`

- [ ] **Step 1: Add the failing async metadata overload signature test**

In `src/unittest/client_tunnel_open_test.cpp`, add this signature alias next to `StartAsyncSignature`:

```cpp
using StartAsyncWithMetadataSignature = TqClientTunnelOpenHandle* (*)(
    MsQuicConnection*,
    const TunnelRequest&,
    TqSocketHandle,
    const TqConfig&,
    TqClientTunnelOpenComplete,
    TqClientTunnelMetadata);
```

Add this static assertion after the existing `TqStartClientTunnelAsync` stable signature assertion:

```cpp
static_assert(
    std::is_same_v<
        decltype(static_cast<StartAsyncWithMetadataSignature>(&TqStartClientTunnelAsync)),
        StartAsyncWithMetadataSignature>,
    "TqStartClientTunnelAsync metadata overload must be available");
```

- [ ] **Step 2: Run client tunnel open test and verify it fails to compile**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_client_tunnel_open_test -j 8
```

Expected: compile fails because `TqClientTunnelMetadata` and the 6 parameter overload do not exist.

- [ ] **Step 3: Add the metadata expectation to registry test**

`src/unittest/tunnel_registry_test.cpp` already checks `PeerId` and `ConnectionId` when metadata is supplied directly. Add a second connection case that mirrors client role metadata overwrite:

```cpp
    auto* clientConnection = reinterpret_cast<MsQuicConnection*>(0x2);
    int clientContext = 0;
    TqRegisterConnectionTunnel(clientConnection, &clientContext, &Abort, &Drain);
    TqTunnelRegistryMetadata clientMetadata;
    clientMetadata.PeerId = "primary";
    clientMetadata.ConnectionId = "conn-0";
    clientMetadata.Target = "www.google.com:443";
    clientMetadata.Role = "client";
    clientMetadata.Ingress = "socks";
    clientMetadata.Compress = "zstd";
    clientMetadata.RelayBackend = "linux";
    TqUpdateConnectionTunnelMetadata(clientConnection, &clientContext, clientMetadata);

    auto clientTunnels = TqSnapshotTunnels();
    auto clientIt = std::find_if(clientTunnels.begin(), clientTunnels.end(), [](const TqTunnelSnapshot& tunnel) {
        return tunnel.Target == "www.google.com:443";
    });
    if (clientIt == clientTunnels.end()) return 27;
    if (clientIt->PeerId != "primary") return 28;
    if (clientIt->ConnectionId != "conn-0") return 29;
    TqUnregisterConnectionTunnel(clientConnection, &clientContext);
```

Add the include:

```cpp
#include <algorithm>
```

- [ ] **Step 4: Run registry test and verify it still passes**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_tunnel_registry_test -j 8
rtk ./build-msquic-perf/src/tcpquic_tunnel_registry_test
```

Expected: exit code 0. This confirms registry already supports the fields and the remaining work is passing metadata from client tunnel creation.

- [ ] **Step 5: Add client tunnel metadata type and async overload**

In `src/tunnel/client_tunnel_open.h`, after `using TqClientTunnelOpenComplete`, add:

```cpp
struct TqClientTunnelMetadata {
    std::string PeerId;
    std::string ConnectionId;
};
```

Add `#include <string>` to the header.

Declare the metadata overload after the existing 5 parameter declaration:

```cpp
TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete,
    TqClientTunnelMetadata metadata);
```

Keep the existing 5 parameter declaration unchanged so `client_tunnel_open_test.cpp` continues to validate the stable public signature.

- [ ] **Step 6: Store metadata in tunnel context**

In `src/tunnel/tcp_tunnel.cpp`, add fields to `TqTunnelContext`:

```cpp
std::string PeerId;
std::string ConnectionId;
```

Add a setter:

```cpp
void SetClientMetadata(TqClientTunnelMetadata metadata) {
    PeerId = std::move(metadata.PeerId);
    ConnectionId = std::move(metadata.ConnectionId);
}
```

In `UpdateRegistryMetadata(TqCompressAlgo algo)`, set client fields:

```cpp
if (Role == TqTunnelRole::ClientOpen) {
    metadata.PeerId = PeerId;
    metadata.ConnectionId = ConnectionId;
} else if (Role == TqTunnelRole::ServerOpen) {
    const uint32_t serverConnId = TqLookupServerConnectionId(QuicConn);
    if (serverConnId != 0) {
        metadata.ConnectionId = "srv-" + std::to_string(serverConnId);
    }
}
```

- [ ] **Step 7: Pass metadata through async tunnel creation**

Change the `TqStartClientTunnelAsync()` definition in `src/tunnel/tcp_tunnel.cpp` to accept `TqClientTunnelMetadata metadata`.

After `context->SetStream(stream);`, add:

```cpp
context->SetClientMetadata(std::move(metadata));
```

Keep the sync `TqStartClientTunnel()` path unchanged; it can use empty metadata.

Add the 5 parameter wrapper in `src/tunnel/tcp_tunnel.cpp`:

```cpp
TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete) {
    return TqStartClientTunnelAsync(
        conn,
        req,
        clientTcpFd,
        cfg,
        std::move(onComplete),
        TqClientTunnelMetadata{});
}
```

- [ ] **Step 8: Pass peer and connection ids from peer runtime**

In `src/runtime/client_peer_runtime.cpp`, update `StartTunnel()` to use the picked id:

```cpp
TqClientPickedConnection picked;
{
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    picked = Quic ? Quic->PickConnectionWithId() : TqClientPickedConnection{};
    if (picked.Connection == nullptr) {
        if (LogMode == TqClientPeerLogMode::Primary) {
            std::fprintf(stderr, "tcpquic-proxy: no connected QUIC peer available for tunnel\n");
        } else {
            std::fprintf(stderr,
                "tcpquic-proxy: peer %s has no connected QUIC connection for tunnel\n",
                PeerId.c_str());
        }
        return static_cast<TqClientTunnelOpenHandle*>(nullptr);
    }
}

TqClientTunnelMetadata metadata;
metadata.PeerId = PeerId;
metadata.ConnectionId = picked.ConnectionId;
auto* handle = TqStartClientTunnelAsync(
    picked.Connection,
    req,
    fd,
    Config,
    std::move(onComplete),
    std::move(metadata));
if (handle != nullptr) {
    TotalStreams.fetch_add(1, std::memory_order_relaxed);
}
return handle;
```

- [ ] **Step 9: Run tunnel registry, tunnel open, and client peer tests**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_tunnel_registry_test tcpquic_client_tunnel_open_test tcpquic_client_peer_runtime_test -j 8
rtk ./build-msquic-perf/src/tcpquic_tunnel_registry_test
rtk ./build-msquic-perf/src/tcpquic_client_tunnel_open_test
rtk ./build-msquic-perf/src/tcpquic_client_peer_runtime_test
```

Expected: both executables exit 0.

- [ ] **Step 10: Commit this task**

```bash
git add src/tunnel/client_tunnel_open.h src/tunnel/tcp_tunnel.cpp src/runtime/client_peer_runtime.cpp src/unittest/client_tunnel_open_test.cpp src/unittest/tunnel_registry_test.cpp
git commit -m "admin: attach client tunnel peer metadata"
```

---

### Task 4: Verify Router/Admin JSON Surfaces Live Stream Metrics

**Files:**
- Modify: `src/unittest/router_runtime_test.cpp`
- Inspect: `src/runtime/router_runtime.cpp`
- Inspect: `src/runtime/admin_console.cpp`

- [ ] **Step 1: Add router runtime test adapter metrics**

In `src/unittest/router_runtime_test.cpp`, find the fake adapter implementing `SnapshotPeerMetrics()`. Add a test case that sets:

```cpp
adapter.Metrics["primary"].ActiveStreams = 2;
adapter.Metrics["primary"].TotalStreams = 5;
```

Then call:

```cpp
std::string peersJson = runtime.HandleAdmin(Request("GET", "/peers")).Body;
if (peersJson.find("\"active_streams\":2") == std::string::npos) return 2301;
if (peersJson.find("\"total_streams\":5") == std::string::npos) return 2302;
```

Use the local `Request()` helper already present in this test file.

- [ ] **Step 2: Run router runtime test**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_router_runtime_test -j 8
rtk ./build-msquic-perf/src/tcpquic_router_runtime_test
```

Expected before code changes in earlier tasks: this may already pass if router copies live fields. Expected after all tasks: exit code 0.

- [ ] **Step 3: Confirm admin console reads the existing fields**

Inspect `src/runtime/admin_console.cpp` and confirm these lines still use peer JSON fields:

```js
document.getElementById('client-overview-stream-count').textContent = sum(peerList, 'active_streams');
renderRows(document.getElementById('client-overview-peers'), peerList, ['peer_id','state','enabled','connection_count','connected_connections','active_streams','total_streams','reconnects','socks_listen','http_listen','last_error']);
renderRows(tbody, rows, ['peer_id','state','enabled','quic_peer','socks_listen','http_listen','connection_count','connected_connections','active_streams','total_streams','reconnects','last_error']);
```

No code change is needed if these lines remain unchanged.

- [ ] **Step 4: Run admin console regression test**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_admin_http_test -j 8
rtk ./build-msquic-perf/src/tcpquic_admin_http_test
```

Expected: exit code 0.

- [ ] **Step 5: Commit this task**

```bash
git add src/unittest/router_runtime_test.cpp
git commit -m "test: cover client admin stream metric json"
```

---

### Task 5: Full Verification And Runtime Probe

**Files:**
- No source edits.
- Runtime probe uses `/run/user/1000/raypx2/client-admin-2841985.json` when that process is running.

- [ ] **Step 1: Run focused test targets**

Run:

```bash
rtk cmake --build build-msquic-perf --target tcpquic_quic_session_reconnect_test tcpquic_client_peer_runtime_test tcpquic_tunnel_registry_test tcpquic_client_tunnel_open_test tcpquic_router_runtime_test tcpquic_admin_http_test -j 8
rtk ./build-msquic-perf/src/tcpquic_quic_session_reconnect_test
rtk ./build-msquic-perf/src/tcpquic_client_peer_runtime_test
rtk ./build-msquic-perf/src/tcpquic_tunnel_registry_test
rtk ./build-msquic-perf/src/tcpquic_client_tunnel_open_test
rtk ./build-msquic-perf/src/tcpquic_router_runtime_test
rtk ./build-msquic-perf/src/tcpquic_admin_http_test
```

Expected: all commands exit 0.

- [ ] **Step 2: Check for hot-path locking regressions by inspection**

Run:

```bash
rtk git diff -- src/relay src/tunnel src/runtime src/protocol | rg -n "std::lock_guard|std::unique_lock|std::scoped_lock|TqCountConnectionTunnels|TotalStreams|PickConnectionWithId"
```

Expected:

- New `TotalStreams.fetch_add()` appears in `TqClientPeerRuntime::StartTunnel()`, not relay read/write loops.
- New `TqCountConnectionTunnels()` usage appears only through admin snapshot path, not relay worker code.
- No new lock appears in relay worker TCP read/write or QUIC receive data loops.

- [ ] **Step 3: Probe a running client admin process**

If `/run/user/1000/raypx2/client-admin-2841985.json` exists and the process is the rebuilt binary, run:

```bash
TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /run/user/1000/raypx2/client-admin-2841985.json)
rtk curl -fsS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3080/api/v1/peers
rtk curl -fsS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3080/api/v1/tunnels
rtk curl -fsS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3080/api/v1/peers/primary/connections
```

Expected:

- `/api/v1/peers` includes `"active_streams":` equal to the sum of active connection tunnels for that peer.
- `/api/v1/peers` includes `"total_streams":` greater than 0 after new browser proxy tunnels are opened.
- `/api/v1/tunnels` includes `"peer_id":"primary"` and `"connection_id":"conn-0"` for client tunnels.

- [ ] **Step 4: Commit verification notes if a docs/test record is created**

If runtime probe output is recorded in a new docs/test file, commit only that file:

```bash
git add docs/test/client-admin-stream-metrics-20260630_cn.md
git commit -m "docs: record client admin stream metric validation"
```

If no runtime note file is created, skip this commit.

---

## Self-Review Checklist

- Spec coverage: Tasks cover `active_streams`, `total_streams`, client tunnel `peer_id`, client tunnel `connection_id`, console JSON surfacing, and no-hot-path-lock verification.
- Placeholder scan: The plan contains no `TBD`, no `TODO`, and no open-ended implementation steps.
- Type consistency: `TqClientPickedConnection`, `TqClientTunnelMetadata`, `PickConnectionWithId()`, and `TotalStreams` names are consistent across tasks.
- Performance check: The only new atomic increment is in tunnel creation; registry counting is restricted to admin snapshot.
