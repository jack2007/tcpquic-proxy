# Linux Active Relay Detail Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Linux relay 后端补齐 active relay 逐连接明细，让 `/api/v1/relay/active-relays` 和 `/api/v1/relay/active-relays/{id}` 能返回 Linux relay 的可排障状态。

**Architecture:** Linux relay 的逐 relay 状态仍只在 owner worker 线程内读取，`TqLinuxRelayWorker::SnapshotLocal()` 把私有 `RelayState` 投影成公开 snapshot 结构；runtime 只聚合 worker snapshot，admin JSON 层只做平台无关转换和输出。这样不暴露 `RelayState`，也不在 MsQuic callback 或 epoll 热路径上增加新锁。

**Tech Stack:** C++17, Linux epoll relay worker, nlohmann/json, CMake, 现有 `tcpquic_linux_relay_worker_io_test` 和 `tcpquic_router_runtime_test`。

---

## File Structure

- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

## Design

### Current State

`TqSnapshotActiveRelays()` 只在 `_WIN32` 分支读取 `TqWindowsRelayWorkerSnapshot::ActiveRelayStates`。Linux 分支虽然在 `TqLinuxRelayWorker::SnapshotLocal()` 中遍历了 `Relays` 并计算了 `HotRelay*` 聚合字段，但没有把每个 relay 的状态保存到 `TqLinuxRelayWorkerSnapshot`，所以 admin API 只能返回空列表和 `not_supported`。

Linux 需要保持的边界：

- `RelayState` 是 `TqLinuxRelayWorker` 私有类型，不能让 runtime 或 `relay_metrics.cpp` 直接访问。
- `Relays`、`PendingQuicReceives`、`PendingTcpWrites`、`TcpReadArmed`、`TcpWriteArmed` 等数据面字段只应在 owner worker 线程内读取。
- `CallbackPendingQuicReceives` 是 callback 降级队列，读取 depth 时必须短暂持有 `CallbackPendingQuicReceiveLock`。

### API Contract

Linux active relay 使用现有公共字段，并追加 Linux 排障字段。公共字段仍由 `TqActiveRelayJsonValue()` 输出：

| JSON 字段 | Linux 来源 | 用途 |
| --- | --- | --- |
| `backend` | 固定 `"linux"` | 标识 relay 后端。 |
| `worker_index` | `TqLinuxRelayWorkerConfig::WorkerIndex` | 定位 owner worker。 |
| `relay_id` / `relay_id_numeric` | `RelayState::Id` | 列表显示和单 relay 查询键。 |
| `in_flight_quic_sends` | `OutstandingQuicSends + PendingQuicSendRetries.size()` | QUIC send operation 数。 |
| `pending_quic_receive_bytes` | `RelayState::PendingQuicReceiveBytes` | QUIC->TCP 背压主指标。 |
| `pending_quic_receive_queue_depth` | `RelayState::PendingQuicReceives.size()` | 判断 pending 是大包还是队列积压。 |
| `callback_pending_quic_receive_depth` | `CallbackPendingQuicReceives.size()` | event queue full 降级路径是否触发。 |
| `outstanding_quic_send_bytes` | `OutstandingQuicSendBytes + retry.TotalBytes` | TCP->QUIC 发送积压。 |
| `tcp_read_bytes` / `tcp_write_bytes` | `TcpReadBytes` / `TcpWriteBytes` | 单 relay 吞吐。 |
| `last_tcp_write_errno` | `RelayState::LastTcpWriteErrno` | TCP 写错误定位。 |
| `closing` | `RelayState::Closing` | 是否正在 drain/teardown。 |
| `tcp_read_closed` / `tcp_write_closed` | `TcpReadClosed` / `TcpWriteClosed` | TCP 双向关闭状态。 |
| `tcp_read_paused_by_quic_backlog` | `RelayState::TcpReadPausedByQuicBacklog` | TCP read 是否因 QUIC backlog 暂停。 |
| `quic_send_fin_submitted` / `quic_send_fin_completed` | `QuicSendFinSubmitted` / `QuicSendFinCompleted` | QUIC FIN 生命周期。 |
| `stop_published` | `Handle != nullptr && Handle->Stop.load(std::memory_order_acquire)` | 控制面是否发布停止。 |
| `stream_detached` | `RelayState::StreamBinding == nullptr` | stream callback binding 是否已解绑。 |

Linux 扩展字段：

| JSON 字段 | Linux 来源 | 用途 |
| --- | --- | --- |
| `tcp_fd` | `RelayState::TcpFd` | 与 `ss`、`lsof`、trace 对齐。 |
| `tcp_read_armed` / `tcp_write_armed` | `TcpReadArmed` / `TcpWriteArmed` | epoll interest 状态。 |
| `pending_quic_send_retries` | `PendingQuicSendRetries.size()` | MsQuic backpressure retry 数。 |
| `ideal_send_bytes` | `CurrentRelayIdealSendBytes(relay)` | 与 outstanding bytes 对比。 |
| `tcp_write_eagain_count` | `TcpWriteEagainCount` | TCP 写端背压强度。 |
| `epoll_out_events` | `EpollOutEvents` | writable 唤醒频率。 |
| `pending_tcp_write_queue_depth` | `PendingTcpWrites.size()` | 解压输出等待写 TCP 的队列深度。 |
| `pending_tcp_write_bytes` | `PendingTcpWriteBytes(PendingTcpWrites)` | 解压输出等待写 TCP 的字节数。 |
| `relay_buffer_bytes_in_use` | `PendingBufferBytes + retry bytes + pending tcp write bytes` | 单 relay buffer budget 占用。 |
| `local_address` / `peer_address` | `GetSocketNameString(TcpFd, false/true)` | TCP 本地和对端地址。 |

Capability 行为：

- Linux: `active_relay_detail:true`、`worker_detail:true`、`per_worker_active_relays:false`。当前计划只补齐全局 active relay 明细列表，不在 `/relay/workers/{worker_id}` 内嵌逐 relay 明细。
- Windows: 保持 `active_relay_detail:true`，`per_worker_active_relays` 可以继续按当前实现返回。
- macOS/unsupported: 继续返回 `active_relay_detail:false`。

### Cost Control

`/active-relays` 是 O(active relays) snapshot。第一版实现完整列表，不新增分页参数；文档必须说明该接口面向排障，不应高频轮询。若后续生产连接数很高，再增加 `limit`、`worker_index`、`sort` 查询参数。

## Task 1: Add Linux Active Relay Snapshot Types

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/runtime/relay_metrics.h`

- [ ] **Step 1: Add a failing compile-time usage in the Linux worker test**

In `src/unittest/linux_relay_worker_io_test.cpp`, add this assertion in the existing test that registers two relays and checks `snapshot.ActiveRelays == 2`:

```cpp
if (snapshot.ActiveRelayStates.size() != 2) {
    std::fprintf(
        stderr,
        "expected 2 active relay states, got %zu\n",
        snapshot.ActiveRelayStates.size());
    return 3202;
}
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
```

Expected: compile fails because `TqLinuxRelayWorkerSnapshot` has no `ActiveRelayStates` member.

- [ ] **Step 2: Define Linux snapshot structs**

In `src/tunnel/linux_relay_worker.h`, add before `struct TqLinuxRelayWorkerSnapshot`:

```cpp
struct TqLinuxRelayActiveSnapshot {
    uint32_t WorkerIndex{0};
    uint64_t RelayId{0};
    int TcpFd{-1};
    uint32_t InFlightQuicSends{0};
    uint64_t PendingQuicReceiveBytes{0};
    uint64_t PendingQuicReceiveQueueDepth{0};
    uint64_t CallbackPendingQuicReceiveDepth{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t PendingQuicSendRetries{0};
    uint64_t IdealSendBytes{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t EpollOutEvents{0};
    uint64_t PendingTcpWriteQueueDepth{0};
    uint64_t PendingTcpWriteBytes{0};
    uint64_t RelayBufferBytesInUse{0};
    uint64_t LastTcpWriteErrno{0};
    bool Closing{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool TcpReadArmed{false};
    bool TcpWriteArmed{false};
    bool TcpReadPausedByQuicBacklog{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    bool StopPublished{false};
    bool StreamDetached{false};
    std::string LocalAddress;
    std::string PeerAddress;
};
```

Then add these fields to `TqLinuxRelayWorkerSnapshot`:

```cpp
uint64_t SnapshotActiveRelaysScanned{0};
std::vector<TqLinuxRelayActiveSnapshot> ActiveRelayStates;
```

In `src/runtime/relay_metrics.h`, extend `TqRelayActiveSnapshot` with Linux extension fields:

```cpp
int TcpFd{-1};
bool TcpReadArmed{false};
bool TcpWriteArmed{false};
uint64_t PendingQuicSendRetries{0};
uint64_t IdealSendBytes{0};
uint64_t TcpWriteEagainCount{0};
uint64_t EpollOutEvents{0};
uint64_t PendingTcpWriteQueueDepth{0};
uint64_t PendingTcpWriteBytes{0};
uint64_t RelayBufferBytesInUse{0};
std::string LocalAddress;
std::string PeerAddress;
```

- [ ] **Step 3: Verify the compile error moves forward**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
```

Expected: build reaches link or test execution; if it still fails, the failure is not “no member named `ActiveRelayStates`”.

## Task 2: Populate Linux Active Relay States in Worker Snapshot

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Strengthen the failing worker test**

In the `kRelayCount = 64` relay-index test in `src/unittest/linux_relay_worker_io_test.cpp`, add checks for relay identity and worker index after:

```cpp
assert(snapshot.ActiveRelays == kRelayCount);
assert(snapshot.TcpReadBytes >= sizeof(payload));
```

Add:

```cpp
bool sawFirst = false;
bool sawLast = false;
for (const auto& active : snapshot.ActiveRelayStates) {
    if (active.WorkerIndex != config.WorkerIndex) {
        std::fprintf(stderr, "unexpected worker index %u\n", active.WorkerIndex);
        return 3203;
    }
    if (active.RelayId == registrations.front().RelayId) {
        sawFirst = true;
    }
    if (active.RelayId == registrations.back().RelayId) {
        sawLast = true;
    }
}
if (!sawFirst || !sawLast) {
    std::fprintf(stderr, "active relay states missing registered relay ids\n");
    return 3204;
}
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: test fails because `ActiveRelayStates` is empty or fields are zero.

- [ ] **Step 2: Populate each relay snapshot in `SnapshotLocal()`**

In `TqLinuxRelayWorker::SnapshotLocal()`, before the `for (const auto& relay : Relays)` loop, reserve capacity:

```cpp
snapshot.ActiveRelayStates.reserve(Relays.size());
```

Inside the relay loop, after computing retry bytes and pending TCP write bytes, construct and append:

```cpp
uint64_t retryBytes = 0;
for (const auto& retry : relay->PendingQuicSendRetries) {
    if (retry != nullptr) {
        retryBytes += retry->TotalBytes;
    }
}
const uint64_t pendingTcpWriteBytes = PendingTcpWriteBytes(relay->PendingTcpWrites);
uint64_t callbackPendingDepth = 0;
{
    std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
    callbackPendingDepth = relay->CallbackPendingQuicReceives.size();
}

TqLinuxRelayActiveSnapshot active{};
active.WorkerIndex = Config.WorkerIndex;
active.RelayId = relay->Id;
active.TcpFd = relay->TcpFd;
active.InFlightQuicSends = static_cast<uint32_t>(
    relay->OutstandingQuicSends + relay->PendingQuicSendRetries.size());
active.PendingQuicReceiveBytes = relay->PendingQuicReceiveBytes;
active.PendingQuicReceiveQueueDepth = relay->PendingQuicReceives.size();
active.CallbackPendingQuicReceiveDepth = callbackPendingDepth;
active.OutstandingQuicSendBytes = relay->OutstandingQuicSendBytes + retryBytes;
active.PendingQuicSendRetries = relay->PendingQuicSendRetries.size();
active.IdealSendBytes = CurrentRelayIdealSendBytes(relay.get());
active.TcpReadBytes = relay->TcpReadBytes;
active.TcpWriteBytes = relay->TcpWriteBytes;
active.TcpWriteEagainCount = relay->TcpWriteEagainCount;
active.EpollOutEvents = relay->EpollOutEvents;
active.PendingTcpWriteQueueDepth = relay->PendingTcpWrites.size();
active.PendingTcpWriteBytes = pendingTcpWriteBytes;
active.RelayBufferBytesInUse =
    relay->PendingBufferBytes.load(std::memory_order_relaxed) + retryBytes + pendingTcpWriteBytes;
active.LastTcpWriteErrno = relay->LastTcpWriteErrno;
active.Closing = relay->Closing;
active.TcpReadClosed = relay->TcpReadClosed;
active.TcpWriteClosed = relay->TcpWriteClosed;
active.TcpReadArmed = relay->TcpReadArmed;
active.TcpWriteArmed = relay->TcpWriteArmed;
active.TcpReadPausedByQuicBacklog = relay->TcpReadPausedByQuicBacklog;
active.QuicSendFinSubmitted = relay->QuicSendFinSubmitted;
active.QuicSendFinCompleted = relay->QuicSendFinCompleted;
active.StopPublished =
    relay->Handle != nullptr && relay->Handle->Stop.load(std::memory_order_acquire);
active.StreamDetached = relay->StreamBinding == nullptr;
active.LocalAddress = GetSocketNameString(relay->TcpFd, false);
active.PeerAddress = GetSocketNameString(relay->TcpFd, true);
snapshot.ActiveRelayStates.push_back(std::move(active));
```

At the end of `SnapshotLocal()`, set:

```cpp
snapshot.SnapshotActiveRelaysScanned = snapshot.ActiveRelayStates.size();
```

When adding this code, avoid double-counting `retryBytes` in existing aggregate fields. If the existing loop already adds retry bytes to `snapshot.OutstandingQuicSendBytes`, reuse the same local `retryBytes` for both aggregate and active snapshot.

- [ ] **Step 3: Verify worker detail test passes**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: build succeeds and the test exits 0.

## Task 3: Aggregate Linux Active Relay States in Runtime

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add runtime aggregation assertions**

In the existing runtime snapshot test near `TqLinuxRelayRuntime::Instance().SnapshotWorkers()`, add:

```cpp
const auto aggregate = TqLinuxRelayRuntime::Instance().Snapshot();
if (aggregate.ActiveRelays != 1 ||
    aggregate.SnapshotActiveRelaysScanned != 1 ||
    aggregate.ActiveRelayStates.size() != 1 ||
    aggregate.ActiveRelayStates.front().RelayId != runtimeRelay.RelayId ||
    aggregate.ActiveRelayStates.front().WorkerIndex != 0) {
    std::fprintf(
        stderr,
        "aggregate active states mismatch active=%llu scanned=%llu states=%zu\n",
        static_cast<unsigned long long>(aggregate.ActiveRelays),
        static_cast<unsigned long long>(aggregate.SnapshotActiveRelaysScanned),
        aggregate.ActiveRelayStates.size());
    return 3298;
}
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: the new assertion fails if runtime aggregation does not copy worker `ActiveRelayStates`.

- [ ] **Step 2: Aggregate states in `TqLinuxRelayRuntime::Snapshot()`**

In `TqLinuxRelayRuntime::Snapshot()`, after each worker `snapshot` is collected, append:

```cpp
total.SnapshotActiveRelaysScanned += snapshot.SnapshotActiveRelaysScanned;
total.ActiveRelayStates.insert(
    total.ActiveRelayStates.end(),
    snapshot.ActiveRelayStates.begin(),
    snapshot.ActiveRelayStates.end());
```

Before iterating workers, reserve a small baseline:

```cpp
total.ActiveRelayStates.reserve(total.ActiveRelays);
```

If `total.ActiveRelays` is not known until after the loop in the current function, skip the reserve or reserve `Workers.size()`; correctness is more important than preallocation.

- [ ] **Step 3: Verify runtime aggregation**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: build succeeds and the test exits 0.

## Task 4: Convert Linux Active Relay Snapshots to Admin JSON

**Files:**
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Add a Linux-specific active relay API test**

In `src/unittest/router_runtime_test.cpp`, add a test helper or extend the existing `/relay/active-relays` test to parse the response and check capabilities. Use `nlohmann::json` because this test target already links `nlohmann_json::nlohmann_json`:

```cpp
TqHttpRequest activeRelays = Request("GET", "/relay/active-relays", "");
TqHttpResponse activeRelaysResponse = TqHandleRouterRuntime(activeRelays, state);
const auto activeRelaysJson = nlohmann::json::parse(activeRelaysResponse.Body);
#if defined(__linux__)
if (activeRelaysJson["capabilities"]["active_relay_detail"] != true) return 1321;
if (activeRelaysJson["capabilities"]["per_worker_active_relays"] != false) return 1322;
#endif
```

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected on Linux: test fails because capabilities still report `active_relay_detail:false`.

- [ ] **Step 2: Enable Linux capability**

In `src/runtime/relay_metrics.cpp`, change the supported constant:

```cpp
#if defined(_WIN32) || defined(__linux__)
constexpr bool kRelayActiveRelayDetailSupported = true;
#else
constexpr bool kRelayActiveRelayDetailSupported = false;
#endif
```

Update `TqRelayCapabilitiesJsonValue()`:

```cpp
nlohmann::json TqRelayCapabilitiesJsonValue() {
    return {
        {"active_relay_detail", kRelayActiveRelayDetailSupported},
        {"worker_detail", true},
        {"per_worker_active_relays", false},
    };
}
```

- [ ] **Step 3: Add Linux converter**

In `src/runtime/relay_metrics.cpp`, below `ConvertLinuxRelayWorkerSnapshot()`, add:

```cpp
TqRelayActiveSnapshot ConvertLinuxRelaySnapshot(const TqLinuxRelayActiveSnapshot& relay) {
    TqRelayActiveSnapshot active{};
    active.Backend = "linux";
    active.WorkerIndex = relay.WorkerIndex;
    active.RelayId = relay.RelayId;
    active.InFlightQuicSends = relay.InFlightQuicSends;
    active.PendingQuicReceiveBytes = relay.PendingQuicReceiveBytes;
    active.PendingQuicReceiveQueueDepth = relay.PendingQuicReceiveQueueDepth;
    active.CallbackPendingQuicReceiveDepth = relay.CallbackPendingQuicReceiveDepth;
    active.OutstandingQuicSendBytes = relay.OutstandingQuicSendBytes;
    active.EventQueueDepth = 0;
    active.TcpReadBytes = relay.TcpReadBytes;
    active.TcpWriteBytes = relay.TcpWriteBytes;
    active.LastTcpWriteErrno = relay.LastTcpWriteErrno;
    active.Closing = relay.Closing;
    active.TcpReadClosed = relay.TcpReadClosed;
    active.TcpReadPausedByQuicBacklog = relay.TcpReadPausedByQuicBacklog;
    active.TcpWriteClosed = relay.TcpWriteClosed;
    active.QuicSendFinSubmitted = relay.QuicSendFinSubmitted;
    active.QuicSendFinCompleted = relay.QuicSendFinCompleted;
    active.StopPublished = relay.StopPublished;
    active.StreamDetached = relay.StreamDetached;
    active.TcpFd = relay.TcpFd;
    active.TcpReadArmed = relay.TcpReadArmed;
    active.TcpWriteArmed = relay.TcpWriteArmed;
    active.PendingQuicSendRetries = relay.PendingQuicSendRetries;
    active.IdealSendBytes = relay.IdealSendBytes;
    active.TcpWriteEagainCount = relay.TcpWriteEagainCount;
    active.EpollOutEvents = relay.EpollOutEvents;
    active.PendingTcpWriteQueueDepth = relay.PendingTcpWriteQueueDepth;
    active.PendingTcpWriteBytes = relay.PendingTcpWriteBytes;
    active.RelayBufferBytesInUse = relay.RelayBufferBytesInUse;
    active.LocalAddress = relay.LocalAddress;
    active.PeerAddress = relay.PeerAddress;
    return active;
}
```

In `TqSnapshotActiveRelays()`, add the Linux branch:

```cpp
#if defined(__linux__)
    const auto snapshot = TqLinuxRelayRuntime::Instance().Snapshot();
    active.reserve(snapshot.ActiveRelayStates.size());
    for (const auto& relay : snapshot.ActiveRelayStates) {
        active.push_back(ConvertLinuxRelaySnapshot(relay));
    }
#elif defined(_WIN32)
    const auto snapshot = TqWindowsRelayRuntime::Instance().Snapshot();
    active.reserve(snapshot.ActiveRelayStates.size());
    for (const auto& relay : snapshot.ActiveRelayStates) {
        active.push_back(ConvertWindowsRelaySnapshot(relay));
    }
#endif
```

- [ ] **Step 4: Output Linux extension fields**

In `TqActiveRelayJsonValue()`, keep existing fields and append Linux extension fields only for Linux relay snapshots:

```cpp
if (std::string(relay.Backend) == "linux") {
    body["tcp_fd"] = relay.TcpFd;
    body["tcp_read_armed"] = relay.TcpReadArmed;
    body["tcp_write_armed"] = relay.TcpWriteArmed;
    body["pending_quic_send_retries"] = relay.PendingQuicSendRetries;
    body["ideal_send_bytes"] = relay.IdealSendBytes;
    body["tcp_write_eagain_count"] = relay.TcpWriteEagainCount;
    body["epoll_out_events"] = relay.EpollOutEvents;
    body["pending_tcp_write_queue_depth"] = relay.PendingTcpWriteQueueDepth;
    body["pending_tcp_write_bytes"] = relay.PendingTcpWriteBytes;
    body["relay_buffer_bytes_in_use"] = relay.RelayBufferBytesInUse;
    body["local_address"] = relay.LocalAddress;
    body["peer_address"] = relay.PeerAddress;
}
return body;
```

To support mutation, first rewrite the function from a direct braced return to:

```cpp
nlohmann::json body{
    {"relay_id", std::to_string(relay.RelayId)},
    {"relay_id_numeric", relay.RelayId},
    {"worker_index", relay.WorkerIndex},
    {"backend", relay.Backend},
    {"active_handlers", relay.ActiveHandlers},
    {"queued_worker_ops", relay.QueuedWorkerOps},
    {"in_flight_tcp_recvs", relay.InFlightTcpRecvs},
    {"in_flight_tcp_sends", relay.InFlightTcpSends},
    {"in_flight_quic_sends", relay.InFlightQuicSends},
    {"pending_quic_receive_bytes", relay.PendingQuicReceiveBytes},
    {"pending_quic_receive_queue_depth", relay.PendingQuicReceiveQueueDepth},
    {"callback_pending_quic_receive_depth", relay.CallbackPendingQuicReceiveDepth},
    {"outstanding_quic_send_bytes", relay.OutstandingQuicSendBytes},
    {"max_outstanding_quic_send_bytes", relay.MaxOutstandingQuicSendBytes},
    {"event_queue_depth", relay.EventQueueDepth},
    {"tcp_read_bytes", relay.TcpReadBytes},
    {"tcp_write_bytes", relay.TcpWriteBytes},
    {"last_tcp_write_errno", relay.LastTcpWriteErrno},
    {"last_tcp_recv_errno", relay.LastTcpRecvErrno},
    {"last_tcp_send_errno", relay.LastTcpSendErrno},
    {"last_iocp_completion_errno", relay.LastIocpCompletionErrno},
    {"last_iocp_operation", relay.LastIocpOperation},
    {"closing", relay.Closing},
    {"tcp_read_closed", relay.TcpReadClosed},
    {"tcp_read_paused_by_quic_backlog", relay.TcpReadPausedByQuicBacklog},
    {"tcp_write_closed", relay.TcpWriteClosed},
    {"close_after_drained", relay.CloseAfterDrained},
    {"quic_send_fin_submitted", relay.QuicSendFinSubmitted},
    {"quic_send_fin_completed", relay.QuicSendFinCompleted},
    {"stop_published", relay.StopPublished},
    {"stream_detached", relay.StreamDetached},
};
return body;
```

- [ ] **Step 5: Verify router API behavior**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: build succeeds and the test exits 0.

## Task 5: Document API and Linux Relay Status

**Files:**
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

- [ ] **Step 1: Update admin API docs**

In `docs/admin-api/interface.md`, replace the current active relay note with:

```markdown
- Windows 和 Linux 后端可通过 active relay snapshot 返回逐 relay 明细。
- macOS 或不支持逐 relay 明细的平台，`GET /api/v1/relay/active-relays/{relay_id}` 返回 `503 not_supported`。
- Linux 明细额外包含 `tcp_fd`、`tcp_read_armed`、`tcp_write_armed`、`pending_quic_send_retries`、`ideal_send_bytes`、`tcp_write_eagain_count`、`epoll_out_events`、`pending_tcp_write_queue_depth`、`pending_tcp_write_bytes`、`relay_buffer_bytes_in_use`、`local_address`、`peer_address`。
```

- [ ] **Step 2: Keep `docs/relay_linux.md` coarse-grained**

Keep section `3.1 Linux active relay 明细缺失` as a short problem statement that links to this plan:

```markdown
### 3.1 Linux active relay 明细缺失

现象：`TqSnapshotActiveRelays()` 当前只在 Windows 分支填充逐 relay 明细，Linux 下 `/api/v1/relay/active-relays` 只能返回空列表，`/api/v1/relay/active-relays/{id}` 返回 `503 not_supported`。

影响：排查单连接卡顿时只能看 `linux_relay_hot_relay_*` 和 worker 聚合计数，无法枚举所有 active relay 的 pending、backpressure、TCP/QUIC 方向状态和地址信息。

解决方向：在 Linux worker snapshot 中生成逐 relay 明细，由 runtime 聚合后复用 admin active relay API 输出。详细设计和开发计划见 `docs/superpowers/plans/2026-07-04-linux-active-relay-detail.md`。
```

- [ ] **Step 3: Verify Markdown diff**

Run:

```bash
rtk git diff --check -- docs/admin-api/interface.md docs/relay_linux.md docs/superpowers/plans/2026-07-04-linux-active-relay-detail.md
```

Expected: command exits 0 with no whitespace errors.

## Task 6: Final Verification

**Files:**
- Verify only.

- [ ] **Step 1: Build focused targets**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_router_runtime_test -j$(nproc)
```

Expected: both targets build successfully.

- [ ] **Step 2: Run focused tests**

Run:

```bash
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: both executables exit 0.

- [ ] **Step 3: Review final diff**

Run:

```bash
rtk git diff -- src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/linux_relay_worker_io_test.cpp src/unittest/router_runtime_test.cpp docs/admin-api/interface.md docs/relay_linux.md
```

Expected: diff only contains Linux active relay detail support, tests, and docs. No unrelated formatting churn.

---

## Self-Review

- Spec coverage: The plan covers moving detailed 3.1 design out of `docs/relay_linux.md`, preserving a coarse problem statement there, creating a Superpowers plan under `docs/superpowers/plans`, and defining the design and development tasks for Linux active relay detail.
- Placeholder scan: No placeholder tokens, generic “add tests”, or unspecified implementation steps remain; each task includes concrete files, snippets, commands, and expected outcomes.
- Type consistency: Linux worker-private fields are read only inside `SnapshotLocal()`; exported data flows through `TqLinuxRelayActiveSnapshot` and `TqRelayActiveSnapshot`.
