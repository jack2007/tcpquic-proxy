# macOS (Darwin) segmentation fault 调查

本文记录 2026-06-24 在 macOS arm64 上运行 `tcpquic-proxy client` 时出现 segmentation fault 的调查结论。

**环境：** macOS arm64，Release 构建，Darwin kqueue relay backend，QUIC peer `52.74.45.234:8443`。

---

## 1. 现象

客户端启动命令：

```bash
build/bin/Release/tcpquic-proxy client \
  --socks-listen 0.0.0.0:11080 \
  --peer 52.74.45.234:8443 \
  --trace \
  --trace-interval 30 \
  --ca cert/ca.crt
```

终端输出显示 QUIC 连接成功、SOCKS5 和 HTTP CONNECT 监听均已启动，随后进程被 `SIGSEGV` 终止：

```text
tcpquic-proxy relay backend: darwin-kqueue (8 workers)
tcpquic-proxy: trace enabled (interval=30s, log under <exe>/log/)
tcpquic-proxy: QUIC client connected peer=52.74.45.234:8443 slot=1
tcpquic-proxy: peer primary SOCKS5 listening on 0.0.0.0:11080
tcpquic-proxy: peer primary HTTP CONNECT listening on 127.0.0.1:8080
tcpquic-proxy: peer primary QUIC peer 52.74.45.234:8443 (1 connections)
tcpquic-proxy: QUIC peer 52.74.45.234:8443 (1 connections)
zsh: segmentation fault  build/bin/Release/tcpquic-proxy client ...
```

本次崩溃没有生成 `/cores/core.*`。调查时系统配置显示：

```text
ulimit -c: 0
launchctl limit core: soft=0 hard=unlimited
kern.coredump: 1
kern.corefile: /cores/core.%P
```

因此这次定位主要依赖 macOS CrashReporter 报告。

---

## 2. CrashReporter 证据

CrashReporter 文件：

```text
/Users/ry/Library/Logs/DiagnosticReports/tcpquic-proxy-2026-06-24-213112.ips
```

关键信息：

- `exception.type`: `EXC_BAD_ACCESS`
- `signal`: `SIGSEGV`
- `subtype`: `KERN_INVALID_ADDRESS`
- macOS 报告包含 `possible pointer authentication failure`
- `faultingThread`: Darwin relay worker 线程

崩溃线程栈：

```text
MsQuicStreamSend
TqDarwinRelayWorker::TrySubmitQuicSendOperation(...)
TqDarwinRelayWorker::SubmitTcpBatchToQuic(...)
TqDarwinRelayWorker::DrainTcpReadable(...)
TqDarwinRelayWorker::Run()
```

地址符号化结果：

```text
MsQuicStreamSend + 28
TqDarwinRelayWorker::TrySubmitQuicSendOperation(...) + 396
TqDarwinRelayWorker::SubmitTcpBatchToQuic(...) + 936
TqDarwinRelayWorker::DrainTcpReadable(...) + 3160
TqDarwinRelayWorker::Run() + 584
```

这说明崩溃发生在 Darwin relay worker 从 TCP 读取数据后，向 QUIC stream 提交 send 的路径上。

---

## 3. 源码路径分析

Darwin relay 在 `RelayState` 中保存的是裸 `MsQuicStream*`：

```cpp
MsQuicStream* Stream{nullptr};
```

`TrySubmitQuicSendOperation()` 在 `relay->Mutex` 内读取该指针，随后在锁外调用 `stream->Send()`：

```cpp
stream = relay->Stream;
...
const QUIC_STATUS status = TqDarwinRelayStreamSend(
    stream,
    raw->QuicBuffers.empty() ? nullptr : raw->QuicBuffers.data(),
    static_cast<uint32_t>(raw->QuicBuffers.size()),
    raw->Fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE,
    raw);
```

如果 QUIC stream 已经 shutdown、abort 或被 `CleanUpAutoDelete` 释放，而 Darwin relay 未及时停止 TCP 读取，就可能继续用旧的 `MsQuicStream*` 调 `Send()`。CrashReporter 的 pointer authentication failure 与这种 stale pointer / use-after-close 风险一致。

---

## 4. 平台行为对比

Linux relay callback 已处理 stream 关闭事件：

- `QUIC_STREAM_EVENT_PEER_SEND_ABORTED`：`FailRelayFatal(...)`
- `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`：记录状态并 `MaybeStopFullyClosedRelay(...)`
- `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED`：进入 relay 关闭路径

Windows relay callback 也处理了类似事件：

- `PEER_SEND_ABORTED` / `PEER_RECEIVE_ABORTED`：`FailRelayFatal(...)`
- `SHUTDOWN_COMPLETE`：`CloseAfterDrained = true`，随后 `CloseRelayIfDrained(...)`

Darwin relay callback 当前只处理：

- `QUIC_STREAM_EVENT_SEND_COMPLETE`
- `QUIC_STREAM_EVENT_RECEIVE`

其他事件，包括 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`、`QUIC_STREAM_EVENT_PEER_SEND_ABORTED`、`QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED`，会直接走默认返回 `QUIC_STATUS_SUCCESS`。这意味着 QUIC stream 已经进入关闭状态时，Darwin relay 未显式关闭 relay，也不会阻止后续 TCP readable 继续触发 TCP -> QUIC send。

---

## 5. 初步根因判断

最可能根因：

> Darwin relay worker 未处理 QUIC stream shutdown / abort 事件，导致 stream 关闭或释放后，kqueue worker 仍继续从 TCP 读取数据，并使用已失效的 `MsQuicStream*` 调用 `Send()`，最终在 `MsQuicStreamSend` 中触发 `SIGSEGV`。

这不是 zstd flush 问题。崩溃命令未启用 `--compress zstd`，CrashReporter 栈也落在 Darwin TCP -> QUIC send 提交路径，而不是压缩器或解压器路径。

---

## 6. 建议修复

1. 在 `TqDarwinRelayWorker::StreamCallback()` 中补齐以下事件处理：
   - `QUIC_STREAM_EVENT_PEER_SEND_ABORTED`
   - `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED`
   - `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`
2. 收到上述事件后，应将对应 relay 标记为 closing，关闭或注销 TCP kqueue filter，避免后续 `DrainTcpReadable()` 再提交 QUIC send。
3. 对 `SHUTDOWN_COMPLETE` 路径，应参考 Linux / Windows relay 的语义：允许已提交 send completion 安全回收，但禁止新 send。
4. 增加 Darwin relay 单元测试：
   - 注册 relay
   - 模拟 stream shutdown / peer abort callback
   - 再触发 TCP readable
   - 断言不会再调用 `StreamSend`
   - 断言 relay 被关闭或进入 stop 状态
5. 可选增强：评估 Darwin relay 的 `MsQuicStream*` send 路径是否需要与 `TqTunnelContext::StreamOpLock` 或等价 lifetime guard 串行化，避免 raw pointer 生命周期竞争。

---

## 7. 当前验证状态

- 已用 CrashReporter 定位崩溃栈，崩溃点明确在 `MsQuicStreamSend`。
- 已用 `atos` 对地址进行符号化，确认调用链来自 Darwin relay worker TCP -> QUIC send。
- 使用 `lldb` 运行同一命令数分钟，期间有 SOCKS 流量进入但未立即复现，说明该问题更可能依赖 stream close / abort 时序，而不是启动即崩。
- 尚未完成代码修复和回归测试。
