# Windows Relay 错误处理对齐 — 剩余工作 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Linux 侧错误处理对齐（commit `833e75e`）已基本完成的前提下，修复 Windows relay worker 中仍存在的 receive 背压计数 bug，补齐 Windows 专属单元测试，并在 Windows build host 上通过完整验证矩阵。

**Architecture:** 实现代码（`FailRelayFatal`、`CloseRelay(mode)`、`PostQuicSend` 背压重试、`QueueDeferredQuicReceive` 暂停 receive）已在 `windows_relay_worker.cpp` 落地；剩余工作集中在 `SetQuicReceiveEnabled` 与调用方的原子状态协调，以及 `windows_relay_worker_test.cpp` 中对 fatal/graceful/backpressure 路径的定向覆盖。参考基线：`docs/relay-lifecycle-review.md` §9–§10、`docs/superpowers/plans/2026-06-18-relay-error-handling-alignment.md` Task 6–7。

**Tech Stack:** C++17, MsQuic, Windows IOCP/WSARecv/WSASend, `build-win-verify` CMake 树, `tcpquic_windows_relay_worker_test`.

---

## 现状摘要（对照 Linux）

| 能力 | Linux | Windows 实现 | Windows 单测 | 备注 |
| --- | --- | --- | --- | --- |
| `TqResetSocket()` | ✅ | ✅ | ✅ platform_socket_test | |
| 共享 `relay_error.h` | ✅ | ✅ | compile-only | |
| TCP 读/写硬错误 → fatal reset | ✅ + 单测 | ✅ 代码 | ❌ | Windows 缺 IOCP/WSA 定向单测 |
| QUIC receive 队列满 → 背压 | ✅ + 单测 | ⚠️ 有 bug | ⚠️ 失败 exit 64 | 见 Task 1 |
| stream peer abort → fatal reset | ✅ 部分单测 | ✅ 代码 | ❌ | 缺 PEER_SEND/RECEIVE_ABORTED 单测 |
| connection shutdown → fatal | ✅ 代码 | ✅ 代码 | ❌ | |
| graceful FIN → drain + TCP shutdown | ✅ 代码 | ✅ 代码 | ❌ | 缺 GracefulRelayDrains 断言 |
| QUIC send OOM → 背压 + retry 队列 | ✅ 代码 | ✅ 代码 | ❌ 双端 | 缺 transient/fatal 单测 |
| admin metrics JSON | ✅ | ✅ | router_runtime_test | |
| Windows 编译/运行验证 | N/A | ⚠️ | ⚠️ test exit 64 | 本机已复现 |

**当前阻塞：** `tcpquic_windows_relay_worker_test.exe` 在 receive 背压用例（`return 64`）失败，根因是 `QueueDeferredQuicReceive()` 在调用 `SetQuicReceiveEnabled(relay, false)` 前已 `exchange(true)`，导致 `SetQuicReceiveEnabled` 认为"已暂停"而提前 return，既不递增 `QuicReceivePausedCount_` 也不调用 `StreamReceiveSetEnabled`。

---

## File Structure

- Modify: `src/tunnel/windows_relay_worker.cpp` — 修复 receive 暂停双重 exchange（Task 1）
- Modify: `src/unittest/windows_relay_worker_test.cpp` — 补齐 fatal/graceful/backpressure/TCP 硬错误单测（Task 2–6）
- Reference: `src/unittest/linux_relay_worker_io_test.cpp` — Linux 对应用例模板（TCP read hard error、peer abort、queue-full backpressure）
- Reference: `docs/relay-lifecycle-review.md` §10 — 完成后更新 Remaining Follow-ups

---

### Task 1: 修复 QUIC Receive 背压暂停的双重 Exchange Bug

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp:874-877`

- [x] **Step 1: 确认失败用例**

Run:

```powershell
cd c:\src\tcpquic-proxy\build-win-verify
cmake --build . --target tcpquic_windows_relay_worker_test --config Release
.\bin\Release\tcpquic_windows_relay_worker_test.exe
echo $LASTEXITCODE
```

Expected（修复前）: exit code `64`（`snapshot.QuicReceivePausedCount != 1`）。

- [x] **Step 2: 移除冗余 exchange，统一由 SetQuicReceiveEnabled 管理状态**

在 `QueueDeferredQuicReceive()` 中，将：

```cpp
        if (pendingBytes >= maxPendingBytes &&
            !relay->QuicReceivePaused.exchange(true, std::memory_order_acq_rel)) {
            SetQuicReceiveEnabled(relay, false);
        }
```

替换为：

```cpp
        if (pendingBytes >= maxPendingBytes &&
            !relay->QuicReceivePaused.load(std::memory_order_acquire)) {
            SetQuicReceiveEnabled(relay, false);
        }
```

`SetQuicReceiveEnabled(relay, false)` 内部已通过 `QuicReceivePaused.exchange(true)` 保证幂等并递增 `QuicReceivePausedCount_`。

- [x] **Step 3: 验证背压用例通过**

Run:

```powershell
cmake --build . --target tcpquic_windows_relay_worker_test --config Release
.\bin\Release\tcpquic_windows_relay_worker_test.exe
echo $LASTEXITCODE
```

Expected: exit code `0`；背压用例中 `QuicReceivePausedCount == 1`、`g_StreamReceiveSetEnabledCalls == 1`、`g_LastStreamReceiveEnabled == FALSE`、`handle.Stop == false`。

- [x] **Step 4: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp
git commit -m "fix(windows): avoid double exchange on quic receive pause"
```

---

### Task 2: 补齐 Stream Peer Abort Fatal Reset 单测

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加 PEER_RECEIVE_ABORTED 失败用例**

在 `#if defined(_WIN32)` 的 `main()` 中、receive 背压用例之后追加：

```cpp
    {
        TqWindowsRelayWorker receiveWorker;
        assert(receiveWorker.Start());
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 150;
        }
        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
        (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &aborted);
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets == 0) {
            receiveWorker.Stop();
            return 151;
        }
        receiveWorker.Stop();
    }
```

- [x] **Step 2: 添加 PEER_SEND_ABORTED 对称用例**

复制 Step 1 块，改 `aborted.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED`，return code 使用 `152`/`153`。

- [x] **Step 3: 运行测试**

```powershell
cmake --build . --target tcpquic_windows_relay_worker_test --config Release
.\bin\Release\tcpquic_windows_relay_worker_test.exe
echo $LASTEXITCODE
```

Expected: exit `0`；两次 abort 均 `FatalRelayResets >= 1` 且 `handle.Stop == true`。

- [x] **Step 4: Commit**

```bash
git add src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): cover stream peer abort fatal reset"
```

---

### Task 3: 补齐 Connection Shutdown Fatal Reset 单测

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加 SHUTDOWN_COMPLETE.ConnectionShutdown 用例**

```cpp
    {
        TqWindowsRelayWorker receiveWorker;
        assert(receiveWorker.Start());
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 160;
        }
        QUIC_STREAM_EVENT shutdown{};
        shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        shutdown.SHUTDOWN_COMPLETE.ConnectionShutdown = TRUE;
        (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &shutdown);
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets == 0) {
            receiveWorker.Stop();
            return 161;
        }
        receiveWorker.Stop();
    }
```

- [x] **Step 2: 添加非 connection shutdown 走 graceful drain 的对照用例**

同一注册流程，设 `ConnectionShutdown = FALSE`，断言 `FatalRelayResets == 0`（graceful 路径不应 fatal）。若 relay 尚未 drain 完成，`handle.Stop` 可为 false。

- [x] **Step 3: 运行测试并 commit**

```bash
git add src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): cover connection shutdown fatal vs graceful drain"
```

---

### Task 4: 补齐 QUIC Send Backpressure 与 Fatal 单测

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 扩展 FakeStreamSend 支持可注入 status**

在 anonymous namespace 顶部、`FakeStreamSend` 之前添加：

```cpp
QUIC_STATUS g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
```

修改 `FakeStreamSend` 末尾：

```cpp
    if (QUIC_FAILED(g_FakeStreamSendStatus)) {
        return g_FakeStreamSendStatus;
    }
    return QUIC_STATUS_SUCCESS;
```

并在每个用例开始前重置：`g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;`

- [x] **Step 2: Transient backpressure（relay 保持存活）**

复用现有 TCP→QUIC 压缩/非压缩 send 路径（参考 `return 83+` 段），设置：

```cpp
g_FakeStreamSendStatus = QUIC_STATUS_OUT_OF_MEMORY;
```

向已注册 relay 的 TCP socket 写入数据，等待 worker 处理。断言：

```cpp
if (snapshot.QuicSendBackpressureEvents == 0 ||
    handle.Stop.load(std::memory_order_acquire) ||
    snapshot.FatalRelayResets != 0) {
    return 170;
}
```

- [x] **Step 3: Fatal send（relay stop + fatal counter）**

```cpp
g_FakeStreamSendStatus = QUIC_STATUS_INVALID_STATE;
```

再次驱动 TCP 数据。断言：

```cpp
if (snapshot.QuicSendFatalErrors == 0 ||
    !handle.Stop.load(std::memory_order_acquire) ||
    snapshot.FatalRelayResets == 0) {
    return 171;
}
```

- [x] **Step 4: 运行测试并 commit**

```bash
git add src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): classify quic send backpressure and fatal failures"
```

---

### Task 5: 补齐 TCP 硬错误 Fatal Reset 单测

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`
- Reference: 已有 `EmptyOutputCompressor` + `TestCloseRelayTcpSocketForPostRecvFailure` 模式（约 line 906）

- [x] **Step 1: IOCP/WSA 投递失败路径**

在 TCP recv 完成路径中，通过 `TestCloseRelayTcpSocketForPostRecvFailure` 使后续 `PostTcpRecv` 失败，或直接在 recv 完成 handler 触发 `RecordTcpHardErrorAndFail`。断言：

```cpp
if (snapshot.TcpHardErrors == 0 ||
    snapshot.FatalRelayResets == 0 ||
    !handle.Stop.load(std::memory_order_acquire)) {
    return 180;
}
```

- [x] **Step 2: 确认 graceful 路径不 increment TcpHardErrors**

对正常 FIN drain（Task 6）断言 `TcpHardErrors == 0`。

- [x] **Step 3: 运行测试并 commit**

```bash
git add src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): cover tcp hard error fatal reset"
```

---

### Task 6: 补齐 Graceful FIN Drain 单测

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: Stream FIN 无 payload → GracefulRelayDrains**

注册带有效 `TqSocketPair` 的 relay，dispatch `QUIC_STREAM_EVENT_RECEIVE` 且 `QUIC_RECEIVE_FLAG_FIN`、零长度 buffer。断言：

```cpp
if (snapshot.GracefulRelayDrains == 0 || snapshot.FatalRelayResets != 0) {
    return 190;
}
```

- [x] **Step 2: Stream FIN 有 payload → drain 完成后 shutdown**

发送带 FIN 的 receive view，等待 TCP 侧收到数据且 pending receive 归零，再 dispatch `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`（非 connection shutdown）。断言 `GracefulRelayDrains >= 1` 且 `FatalRelayResets == 0`。

- [x] **Step 3: 运行测试并 commit**

```bash
git add src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): cover graceful fin drain without fatal reset"
```

---

### Task 7: Windows 完整验证矩阵

**Files:**
- Verify only

- [x] **Step 1: 构建并运行 Windows relay 相关测试**

```powershell
cd c:\src\tcpquic-proxy\build-win-verify
cmake --build . --target `
  tcpquic_platform_socket_test `
  tcpquic_windows_relay_worker_test `
  --config Release
.\bin\Release\tcpquic_platform_socket_test.exe
.\bin\Release\tcpquic_windows_relay_worker_test.exe
echo "platform=$LASTEXITCODE windows_relay=$LASTEXITCODE"
```

Expected: 两者均 exit `0`。

- [x] **Step 2: 确认 Linux 侧无回归（可选，CI 或 Linux host）**

```bash
rtk cmake --build build-glibc --target \
  tcpquic_platform_socket_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_linux_relay_worker_io_test \
  -j
rtk ./build-glibc/src/tcpquic_platform_socket_test
rtk ./build-glibc/src/tcpquic_linux_relay_worker_queue_test
rtk ./build-glibc/src/tcpquic_linux_relay_worker_io_test
```

Expected: 均 exit `0`。

- [x] **Step 3: 更新 lifecycle 文档**

修改 `docs/relay-lifecycle-review.md` §10，移除已完成项，保留：

- Windows QUIC send retry 在长期资源紧张下的行为观察（见 Task 8）
- 如需，补充 Linux 侧仍缺的 `PEER_RECEIVE_ABORTED` / QUIC send 背压单测（与 alignment plan Task 7 对齐）

- [x] **Step 4: Commit 文档**

```bash
git add docs/relay-lifecycle-review.md
git commit -m "docs: mark windows relay error-handling verification complete"
```

---

### Task 8（Follow-up）: QUIC Send Retry 长期背压行为验证

**Goal:** 确认 `PendingQuicSends` + `QuicSendRetry` IOCP 事件在持续 `QUIC_STATUS_OUT_OF_MEMORY` 下不会永久停滞，且 recovery 后 TCP read 恢复。

- [ ] **Step 1: 添加 fake send hook 交替 OOM/SUCCESS 的 integration 测试**

在 `windows_relay_worker_test.cpp` 中用原子计数器让 `FakeStreamSend` 前 N 次返回 OOM、之后 SUCCESS；驱动大 payload TCP read，断言最终 `QuicSendBackpressureEvents > 0`、pending send 队列清空、`handle.Stop == false`。

- [ ] **Step 2: 可选 stress 脚本**

多 relay 并发 + 人为限制 MsQuic send 成功率（测试 hook），运行 30s 后检查 admin metrics 中 `windows_relay_quic_send_backpressure_events` 增长但 `fatal_relay_resets` 不异常飙升。

---

## Verification Matrix（全部 Task 完成后）

| 场景 | 期望 counter | 期望 handle.Stop |
| --- | --- | --- |
| receive pending 超限 | `QuicReceivePausedCount++`，relay 存活 | false |
| peer abort / conn shutdown | `FatalRelayResets++` | true |
| graceful FIN / shutdown complete | `GracefulRelayDrains++` | true（drain 后） |
| QUIC send OOM | `QuicSendBackpressureEvents++` | false |
| QUIC send invalid state | `QuicSendFatalErrors++`, `FatalRelayResets++` | true |
| TCP IOCP/WSA 硬错误 | `TcpHardErrors++`, `FatalRelayResets++` | true |

---

## Self-Review

- **Spec coverage:** 覆盖 `relay-lifecycle-review.md` §8–§10 中所有 Windows 待验证项；Task 1 修复当前 CI/本地失败根因。
- **Placeholder scan:** 无 TBD；Task 8 标为 follow-up，有明确验证目标。
- **Type consistency:** counter 名称与 `TqWindowsRelayWorkerSnapshot` / `relay_metrics.h` 一致。
- **Risk note:** Task 1 修改后需确认 `MaybeResumeQuicReceive` 在 pending bytes 降至 `maxPendingBytes/2` 以下时正确 resume（现有逻辑不变，仅修复 pause 路径）。

---

## 执行方式

Plan 已保存至 `docs/superpowers/plans/2026-06-18-windows-relay-error-handling-remaining.md`。

**1. Subagent-Driven（推荐）** — 每个 Task 派发独立 subagent，Task 间 code review

**2. Inline Execution** — 当前 session 按 Task 1→7 顺序执行，Task 7 作为 checkpoint
