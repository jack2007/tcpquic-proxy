# Darwin HTTP CONNECT：浏览器关闭后 tunnel/relay 不收敛

**日期：** 2026-07-11  
**平台：** macOS / Darwin kqueue relay（client）  
**入口：** HTTP CONNECT ingress（本例 `amazon-sgp` → `0.0.0.0:18080`）  
**状态：** 已定位根因缺口，尚未合入修复

相关代码：

- `src/tunnel/darwin_relay_worker.cpp`（`DrainTcpReadable` / `ProcessPeerSendShutdown` / `FlushTcpWrites` / `ProcessSendShutdownComplete`）
- `src/tunnel/linux_relay_worker.cpp`（`MaybeStopFullyClosedRelay` / `SetRelayStop`）
- 对照文档：`docs/relay_macos.md`、`docs/superpowers/plans/2026-07-10-darwin-relay-stream-wrapper-terminal-lifetime.md`

---

## 1. 问题现象

Client 通过 HTTP CONNECT 为浏览器提供代理。浏览器关闭（或标签页/进程退出）后：

1. Admin `GET /api/v1/tunnels` 与 `GET /api/v1/metrics` 仍显示大量 **active** tunnel / `active_streams`。
2. `GET /api/v1/relay/metrics` / `GET /api/v1/relay/workers` 中 `active_relays` 与 tunnel 数一致，长期不降。
3. 进程仍持有大量监听口上的 TCP FD；`lsof` 显示多数已是 **`CLOSED`**，少数仍为 `ESTABLISHED`。
4. 偶有 tunnel 在数百秒后以 `reason=reaper` 关闭；多数可挂十几分钟以上仍不消失。

### 1.1 现场快照（2026-07-11，本机 client）

| 观测项 | 值（约） |
|--------|----------|
| peer | `amazon-sgp`（HTTP CONNECT `0.0.0.0:18080`） |
| `active_streams` / `active_relays` | ~29–30 |
| `:18080` FD：`CLOSED` | ~25–26 |
| `:18080` FD：`ESTABLISHED` | ~3–4 |
| `closing_relays` | 0 |
| `tcp_read_armed_relays` | ~5（多数 read 已 disarm） |
| `outstanding_quic_sends` / `pending_bytes` | 0 |
| tunnel `duration_ms` | 多为 800s+ |
| 偶发关闭 | `event=relay_stopping` … `reason=reaper`，age 约 700–800s |

典型残留 target：`gpt.raysync.cloud:443`、`*.stripe.com`、`cursor.com`、`hcaptcha.com` 等 HTTPS CONNECT 隧道。

`CLOSED` FD 数 ≈ 卡住的 relay 数，`ESTABLISHED` ≈ 仍存活的连接：浏览器侧 TCP 已死，进程未 `close()` FD，relay 未从 worker map 移除。

---

## 2. 预期半关闭路径

浏览器关闭 CONNECT 套接字后，client 侧应大致经历：

```text
浏览器 TCP FIN/RST
  → Darwin DrainTcpReadable: read()==0
  → TcpReadClosed=true，向 QUIC 提交 FIN（SubmitTcpBatchToQuic(..., fin=true)）
  → 对端（server relay / 目标）处理后再发 QUIC FIN
  → ProcessPeerSendShutdown → TcpWriteShutdownQueued
  → FlushTcpWrites: shutdown(fd, SHUT_WR)，TcpWriteClosed=true
  → 本地 TCP 进入 CLOSED（FD 仍打开，直到 RetireRelay）
  → 收敛：SignalStop / reaper 或 SHUTDOWN_COMPLETE → CloseRelay → close(fd)
```

Linux 在「双向 TCP 已关 + 本地 QUIC FIN 已提交且完成 + 队列排空」时调用 `MaybeStopFullyClosedRelay` → `SetRelayStop`（`SignalStop`），由 tunnel reaper 收敛，不单纯依赖 MsQuic `SHUTDOWN_COMPLETE` 何时到达。

---

## 3. 根因分析

### 3.1 结论

**Darwin 缺少 Linux 的 `MaybeStopFullyClosedRelay` 等价收敛。**

半关闭数据面步骤（EOF → QUIC FIN、peer FIN → `SHUT_WR`）在 Darwin 上基本存在，但在 **双向 TCP 已关且本地 QUIC FIN 完成后不会 `SignalStop`**。之后只能等待 MsQuic `SHUTDOWN_COMPLETE`；对 keep-alive / 对端迟迟不拆流的 HTTPS 目标，该事件经常长期不来，于是：

- relay 仍留在 worker `Relays` map → `active_relays` 不降；
- tunnel 仍为 `active`；
- TCP 状态机已到 `CLOSED`，但 FD 未 `close()` → `lsof` 僵尸 FD。

### 3.2 与 Linux 的代码对照

| 步骤 | Linux | Darwin |
|------|--------|--------|
| TCP EOF | `FinishTcpToQuic` 提交 FIN；失败则 `SetRelayStop` | `SubmitTcpBatchToQuic(..., true)`；**不** `SignalStop` |
| Peer QUIC FIN | 排队 TCP write shutdown | `ProcessPeerSendShutdown` 设 `TcpWriteShutdownQueued` |
| `SHUT_WR` 完成 | `TcpWriteClosed=true` 后 **`MaybeStopFullyClosedRelay("tcp_write_shutdown_complete")`** | `FlushTcpWrites` 仅 `TcpWriteClosed=true`，**无 stop** |
| 本地 QUIC FIN 完成 | `MaybeStopFullyClosedRelay("quic_send_fin_completed")` 等 | `CompleteQuicSend` / `ProcessSendShutdownComplete` 只更新标志，**无 stop** |
| 双向都关且队列空 | `SetRelayStop` → reaper | 无对应逻辑；依赖 `QuicShutdownComplete` → `CloseRelay(TerminalLogicalDetach)` |

Linux 收敛条件（`MaybeStopFullyClosedRelay`）摘要：

- `TcpReadClosed && TcpWriteClosed`
- `QuicSendFinSubmitted && QuicSendFinCompleted`
- 无 outstanding QUIC send / pending TCP write / pending QUIC receive
- 然后 `SetRelayStop` → `StopControl->SignalStop(generation)`

Darwin 在 `FlushTcpWrites` 完成 `SHUT_WR` 后没有调用任何等价函数。

### 3.3 为何会看到 `CLOSED` FD 仍被占用

1. 已读到浏览器 EOF → 本地处于可 `SHUT_WR` 的半关闭态；
2. 随后 peer QUIC FIN 触发 `SHUT_WR` → 内核 TCP 进入 `CLOSED`；
3. `RetireRelay` / `CloseRelayTcpFdOnce` 未跑 → 用户态仍持有 FD；
4. Admin 仍计为 active relay/tunnel。

这与「浏览器已关但 tunnels/relays 不关」的用户观感一致。

### 3.4 次要假设（未单独证实，可并存）

若 TCP read 因 QUIC send backpressure 被 `EV_DISABLE`，而 peer 在 disarm 期间 RST/关闭，可能更晚或无法通过 `EVFILT_READ` 观察到 EOF。现场多数 FD 已是 `CLOSED` 且与 active 数对齐，主路径仍更符合「半关闭做完、缺 SignalStop」。修复 `MaybeStopFullyClosedRelay` 后若仍有残留，再查 read interest / `EV_EOF` 漏读。

---

## 4. 次要现象

1. **Tunnel 字节计数为 0**  
   `GET /api/v1/tunnels` 中多条 `tcp_read_bytes` / `tcp_write_bytes` 恒为 0，但 relay 聚合 `tcp_read_bytes` / `tcp_write_bytes` 有真实流量。属 Darwin → tunnel 指标未正确回填，**易误判为“从未传过数据”**，与泄漏正交。

2. **`linux_relay_*` 指标名**  
   Darwin 快照仍大量以 `linux_relay_*` legacy key 输出；`tcp_read_closed_relays` 等在 Darwin `SnapshotLocal` 中未必按同名字段填充，**不能单靠该字段判断是否已 EOF**。

3. **`quic_send_ops=0` 与 `tcp_read_batches>0` 并存**  
   stats 中 Darwin 路径可能未递增与 Linux 同名的 `quic_send_ops` 计数；不要据此推断“从未 StreamSend”。以 `InFlightQuicSends` / 实际 FD 与 stop 事件为准。

4. **偶发超长延迟后 reaper 成功**  
   说明 stop/reaper 链路本身可用；缺的是半关闭完成后的 **及时 SignalStop**，而不是 reaper 完全失效。

---

## 5. 修复建议

### 5.1 主修复

在 Darwin 增加与 Linux 语义对齐的 `MaybeStopFullyClosedRelay`（或私有同名 helper）：

**触发点（至少）：**

1. `FlushTcpWrites` 在 `shutdown(SHUT_WR)` 且 `TcpWriteClosed=true` 之后；
2. `CompleteQuicSend`（或等价路径）在 `QuicSendFinCompleted=true` 之后；
3. 若还有其它将 `TcpReadClosed` / `TcpWriteClosed` 置位的路径，同样调用。

**条件（对齐 Linux）：**

- `!Closing`
- `TcpReadClosed && TcpWriteClosed`
- `QuicSendFinSubmitted && QuicSendFinCompleted`
- `InFlightQuicSends == 0`，无 `PendingQuicSends` / `PendingTcpWrites` / pending QUIC receive 压力
- `StopControl != nullptr` → `SignalStop(ControlGeneration)`

**不要**用盲目 timeout 强关已 started wrapper 来掩盖未收到 terminal 的问题（与 stream-lifetime 计划约束一致）；这里是在双向半关闭已完成后补 Linux 已有的 stop 信号。

### 5.2 测试

先写会失败的 Darwin 确定性测试（建议落在 `darwin_relay_worker_io_test.cpp`）：

1. 模拟 TCP EOF → 本地 QUIC FIN 完成；
2. 再模拟 peer send shutdown → `SHUT_WR` / `TcpWriteClosed`；
3. 断言在无注入 `SHUTDOWN_COMPLETE` 的情况下，`StopControl` 已被 signal（或 public handle / active map 可观察收敛）；
4. 再实现 helper，使测试通过。

可选：系统/手工回归 — HTTP CONNECT 打开若干 HTTPS，关浏览器，确认 `active_relays` 与 `:18080` FD 在短时间内归零。

### 5.3 顺带（非阻断）

- 修复 Darwin tunnel 快照的 `tcp_read_bytes` / `tcp_write_bytes` 回填，避免 admin 误导。
- 在 Darwin snapshot 中显式统计 `tcp_read_closed` / `tcp_write_closed` / `quic_send_fin_*`，便于下次排障。

### 5.4 非目标

- 不在本修复中改协议 framing 或强制 abort 所有 half-close。
- 不把 Windows/Linux worker 一并大改；仅保证 Darwin 与 Linux 的「双向关完即 SignalStop」语义对齐。

---

## 6. 排障命令备忘

```bash
# Admin（Bearer token 来自 --admin-token-file）
curl -sS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:2345/api/v1/metrics
curl -sS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:2345/api/v1/tunnels
curl -sS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:2345/api/v1/relay/metrics
curl -sS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:2345/api/v1/relay/workers

# 进程 FD 与 TCP 状态
PID=$(pgrep -f 'raypx2 client' | head -1)
lsof -nP -p "$PID" | awk '/TCP/ && /18080/ {print $NF}' | sort | uniq -c

# 日志
rg 'event=(stats_active_tunnel|relay_stopping|stream_closed)' build/bin/Release/log/client.log
```

---

## 7. 记录

| 项 | 内容 |
|----|------|
| 发现方式 | 用户报告 + 运行中 client admin / `lsof` / `client.log` |
| Gortex memory | `mem02b3ed1c4252c55c`（gotcha）、session note `nt25833deff6279173`（decision） |
| 修复状态 | 文档已记录；代码与失败测试待做 |
