# Windows IOCP Relay 问题调查

本文记录 2026-06-24 在 Windows x64 上运行 `tcpquic-proxy client`、通过浏览器 SOCKS5 访问 YouTube 时出现的 relay 失败与进程异常退出问题。

**环境：** Windows 10 x64，Release 构建（`build-x64/bin/Release/tcpquic-proxy.exe`），relay backend 为 `windows-iocp`（8 workers）。

---

## 1. 现象

### 用户侧

- 本机启动 client，浏览器配置 SOCKS5 代理后访问 YouTube（含视频页）。
- **约 30 秒内**代理不可用：表现为浏览器断连，或 **client 进程直接退出**（PowerShell 显示 `last_exit_code: 1`，部分场景为 `-1073741819`）。
- **`--compress off` 与 `--compress zstd` 均会复现**；关闭压缩不能规避。

### 典型启动命令

```powershell
.\build-x64\bin\Release\tcpquic-proxy.exe client `
  --socks-listen 0.0.0.0:11080 `
  --peer 52.74.45.234:8443 `
  --compress off `
  --trace --trace-interval 30 `
  --ca cert\ca.crt
```

终端可见 QUIC 与 SOCKS5 监听正常启动；问题出现在 **第一条或并发多条隧道进入 relay 阶段之后**。

### 日志侧（`build-x64/bin/Release/log/client.log`）

- QUIC 连接、SOCKS5 `stream_started`、`open_result ok=1`、`relay_started` 均正常。
- 随后大量 **`relay_fatal`**，无优雅 `relay_stopping` / `stream_closed` 序列（或极短即失败）：

```
event=relay_fatal backend=windows reason=iocp_completion_error relay_id=1 socket=624 ...
event=relay_fatal backend=windows reason=wsa_send_receive_view_failed relay_id=1 socket=708 ...
event=relay_fatal backend=windows reason=iocp_completion_error relay_id=1 socket=18446744073709551615 ...
```

其中 `18446744073709551615` 即 **`INVALID_SOCKET`（`(uint64_t)-1`）**，表示对已关闭/无效 socket 的 IO 操作。

- 约 14 秒内 12 条 YouTube 隧道全部失败，统计示例：

```
fatal_relay_resets=12  tcp_hard_errors=13  graceful_drains=0
```

- 单次 curl 小请求有时可成功；**浏览器式多连接 + 持续流量**（YouTube 视频页）稳定触发。

### 进程崩溃（Windows 事件查看器 Application Error 1000）

| 字段 | 值 |
|------|-----|
| 进程 | `tcpquic-proxy.exe` |
| 异常代码 | `0xc0000005`（Access Violation） |
| 故障模块 | `VCRUNTIME140.dll` |

说明部分运行会直接 **内存访问违规崩溃**，而非业务逻辑 `return 1` 退出。崩溃与 IOCP 上 **relay 生命周期 / pending OVERLAPPED** 的竞态一致（见下文）。

---

## 2. 复现条件

| 条件 | 结果 |
|------|------|
| 单次 `curl` + `--compress off` | 通常 OK |
| 5–12 并发 `curl` YouTube | 间歇性进程崩溃（`0xC0000005`） |
| 浏览器访问 YouTube 视频页 | **~30s 内必现** relay 全灭或进程退出 |
| `--compress zstd` | 同样失败（非 zstd 独有） |
| Linux client / server relay | 未观察到同等现象（对照平台行为） |

---

## 3. 根因（代码层）

问题集中在 **`src/tunnel/windows_relay_worker.cpp`** 的 IOCP relay 路径，与压缩无关。

### 3.1 IOCP 完成失败 → 连锁 `FailRelayFatal`

`Run()` 中 `GetQueuedCompletionStatus` 返回失败时，对 **仍持有 `op->Relay` 的 OVERLAPPED** 直接 fatal：

```cpp
if (!ok) {
    RecordTcpHardErrorAndFail(op->Relay, "iocp_completion_error");
    continue;
}
```

`FailRelayFatal` 会 `CloseRelay(..., AbortReset)`，**重置/关闭 TCP socket**。若该 socket 上仍有其他 pending 的 `WSARecv` / `WSASend`，会再次以错误完成投递到 IOCP，形成 **错误风暴**（日志中同一时段多条 `relay_fatal` / 不同 `socket=` 值）。

常见触发 reason：

- `iocp_completion_error` — IOCP 完成端口报告失败（socket 已 reset/close 后 completion 仍到达）
- `wsa_send_receive_view_failed` — QUIC→TCP 方向 `WSASend` 向浏览器侧写失败
- `tcp_send_receive_view_completion_error` — TCP send 完成字节数异常

### 3.2 Socket 关闭与 pending IO 未同步取消

`CloseRelay` 在 `AbortReset` 模式下调用 `TqResetSocket` / `TqCloseSocket`，并将 `TcpFd` 置为 `INVALID_SOCKET`，但 **未在关闭前 cancel 已投递的 overlapped IO**（对比 Linux/Darwin 的 epoll/kqueue unregister 语义）。

浏览器并发连接时，多路 relay 共享同一 IOCP worker；一路 fatal 关闭 socket 后，**stale completion** 仍可能被 worker 线程处理，进一步误伤其他 relay 或触发 UAF。

### 3.3 `Stop` 标志过早 → Reaper 与 worker 竞态

Windows 上 `TqRelayStop`（`src/tunnel/relay.cpp`）仅 **异步** `PostQueuedCompletionStatus(CloseRelay)`，随后 **立即** `handle->Stop.store(true)`。

Reaper（`src/tunnel/tunnel_reaper.cpp`）见 `Stop==true` 即 `delete` 对应 `TqTunnelContext`。Linux/Darwin 路径则 **同步** `UnregisterRelay`，在设置 `Stop` 前从 worker 移除 relay。

相关字段：

- `TqTunnelContext::Compressor` / `Decompressor` — `unique_ptr`，随 context 销毁
- `RelayContext::Compressor` / `Decompressor` — **裸指针**，worker 线程可能仍在使用

`CloseRelayIfDrained` 也会在 IO 未完全结束时设置 `PublicHandle->Stop = true`，加剧上述窗口。

### 3.4 与 zstd 的关系

早期怀疑 zstd 自定义分配器（`TqZstdAlloc` / mimalloc）导致 UAF；用户实测 **`--compress off` 仍失败**，且日志中 `compress=off` 的隧道同样 `relay_fatal`。**zstd 不是主因**；压缩路径仅额外增加 `DecompressInto` / `Flush` 调用，使竞态更容易暴露。

---

## 4. 与 Linux / Darwin 行为差异

| | Linux | Darwin | Windows（当前） |
|---|--------|--------|-----------------|
| Relay 停止 | `UnregisterRelay` 同步移除 | `UnregisterRelay` + `RetireRelay` | `StopRelay` 仅 POST `CloseRelay` 到 IOCP |
| `Stop` 置位时机 | Unregister 完成后 | Unregister 完成后 | **POST 后立即** |
| TCP fd 关闭 | worker 关闭已注册 fd | worker 关闭已注册 fd | `CloseRelay` reset/close，**pending OVERLAPPED 未显式取消** |
| 并发多连接 | 稳定 | 有独立 Darwin 问题（见 `docs/darwin-issues.md`） | **YouTube 类场景稳定失败** |

---

## 5. 建议修复方向（按优先级）

1. **关闭 socket 前取消 pending IO**
   - 关闭前对 relay 标记 `Closing`，忽略/吞掉后续 error completion（勿再 `FailRelayFatal`）。
   - 评估 `CancelIoEx` / 关闭前 drain in-flight 计数（`InFlightTcpSends`、`PendingReceives`）。

2. **对齐 Linux：`Stop=true` 仅在 relay 完全退役后**
   - `TqRelayStop` Windows 路径：等待 IOCP worker 完成 `CloseRelay` 后再 `handle->Stop.store(true)`。
   - 或 `CloseRelayIfDrained` 不在仍有 pending IO 时置 `Stop`。

3. **延长 compressor / tunnel context 生命周期（若仍启用压缩）**
   - `RelayContext` 以 `shared_ptr` 持有 compressor/decompressor，或 Stop 前 pin context，避免 worker 使用裸指针 UAF。

4. **错误 completion 降级**
   - 对已 `Closing` 的 relay，`iocp_completion_error` 走静默清理，避免 `AbortReset` 级联。

5. **测试**
   - 扩展 `windows_relay_worker_test`：并发 register/close、模拟 `GetQueuedCompletionStatus` 失败、INVALID_SOCKET completion。
   - 集成：client SOCKS5 + 多连接 YouTube 类 smoke（Windows CI）。

---

## 6. 临时规避

- Windows client 上 **不宜** 用 SOCKS5 代理浏览 YouTube 等多连接站点；单次简单 HTTP 请求可能可用。
- 生产环境在 Windows 上优先使用 **Linux client** 或待本 issue 修复后再部署 Windows client。
- 排查时启用 `--trace`，查看 `<exe>/log/client.log` 中 `relay_fatal` 的 `reason=` 与 `socket=`。

---

## 7. 修复状态

- **已修复**（2026-06-24）
- 修复要点：
  - Windows relay 关闭后不再立即发布 `RelayHandle.Stop=true`；`Stop` 现在表示 IOCP relay 已完全退役，reaper 可以安全删除 `TqTunnelContext`。
  - `TqRelayStop` Windows 路径改为两阶段：首次调用只向 worker 投递 close，请求完成后由 worker 在 pending TCP/QUIC/worker completion 全部回收后发布 `Stop`；reaper 再次调用时清理 handle 与 active relay 计数。
  - 对已进入 closing 的 relay，后续 stale IOCP error completion 只回收 in-flight 计数并尝试退役，不再连锁触发 `relay_fatal` / `AbortReset`。
  - `CloseRelayIfDrained` 不再在仍可能有 pending completion 时提前置 `Stop`，而是进入统一 `CloseRelay` / retirement 流程。
  - `PostQuicSend` 使用 `IoOperation::QuicBuffer`，避免把栈上 `QUIC_BUFFER` 传给 MsQuic 异步 send 后发生 UAF。
  - Windows TCP EOF 只标记读半边关闭，不再立即关闭整个 TCP socket；QUIC 回来的响应仍可写回浏览器。
  - `WSASend`/`WSARecv` post 失败会记录 `WSAGetLastError()` 到 trace 的 `tcp_write_errno`；本地 socket teardown（如浏览器取消请求导致的 reset/shutdown）按 `GracefulDrain` 处理，不再作为 `relay_fatal`。
  - QUIC receive -> TCP send 的 `WSABUF` 描述符改为挂在 `IoOperation` 上，生命周期与 overlapped send 对齐。
- 验证：
  - `MSBuild build-x64/src/tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false /m:1`
  - `build-x64/bin/Release/tcpquic_windows_relay_worker_test.exe`
  - `MSBuild build-x64/src/tcpquic-proxy.vcxproj /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false /m:1`
  - `build-x64/bin/Release/tcpquic-proxy.exe` 已重新生成。
- 调查依据：`build-x64/bin/Release/log/client.log`、Windows Application Error 1000、本地并发 curl / 浏览器复现

---

## 8. 关键代码位置

| 文件 | 符号 / 区域 |
|------|-------------|
| `src/tunnel/windows_relay_worker.cpp` | `Run()` — IOCP 循环与 `iocp_completion_error` |
| `src/tunnel/windows_relay_worker.cpp` | `CloseRelay` / `FailRelayFatal` / `StopRelay` |
| `src/tunnel/windows_relay_worker.cpp` | `CloseRelayIfDrained` — 过早 `Stop` |
| `src/tunnel/relay.cpp` | `TqRelayStop` — Windows 异步 stop |
| `src/tunnel/tunnel_reaper.cpp` | `TqReapTunnelContext` — context 删除 |
| `src/tunnel/tcp_tunnel.cpp` | `TqTunnelContext` — compressor 所有权 |
