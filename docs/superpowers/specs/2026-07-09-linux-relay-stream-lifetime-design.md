# Linux relay stream lifetime hardening 设计

> 状态说明：本文记录 2026-07-09 事故的最小修复设计。后续实现已由 `docs/superpowers/plans/2026-07-10-linux-relay-stream-wrapper-terminal-lifetime.md` 扩展为统一的 `TqStreamLifetime` owner、稳定 callback 路由、类型化 send completion、shutdown 账本和 Linux managed relay `prepare -> publish -> commit` 模型。本文中的 callback/context 重写和裸 wrapper detach 描述不再代表当前 managed 生产路径。

## 背景

`2026-07-09 19:44:42 +0800` 的 client 崩溃由 Crashpad 和 core 同时捕获：

- Crashpad minidump：`build/bin/Release/crashpad/pending/75e85b9b-c67b-4fde-8c2f-4f4980163963.dmp`
- 完整 core：`/tmp/core.raypx2.4092411.1783597482`
- 崩溃线程：`4092542`
- 信号：`SIGSEGV`
- 触发配置：multi-peer client，`trace.enabled=true`，本地 HTTP 入口 `0.0.0.0:18080`

符号化后的关键调用链：

```text
MsQuicStreamClose
TqLinuxRelayWorker::AbortRelayAndRelease(...)
TqLinuxRelayWorker::ProcessTcpEvents(...)
TqLinuxRelayWorker::DispatchEncodedEpollEvent(...)
TqLinuxRelayWorker::Run()
```

对应源码位置：

- `third_party/msquic/src/core/api.c:943`
- `src/tunnel/linux_relay_worker.cpp:879`
- `src/tunnel/linux_relay_worker.cpp:3466`

现场日志显示 relay 在收到 QUIC `receive_fin` 后完成 TCP write shutdown，随后同一个 TCP fd 又收到 `SO_ERROR=104`：

```text
event=linux_relay_tcp_read ... result=eof
event=linux_relay_stream_event ... stream_event=receive_fin
event=linux_relay_stop_condition ... trigger=tcp_write_shutdown_complete
event=linux_relay_tcp_so_error ... so_error=104
event=relay_fatal ... reason=tcp_socket_error
```

`AbortRelayAndRelease(relay, "tcp_socket_error", true)` 随后尝试执行 `relay->Stream->Shutdown(...)`。core 中被 MsQuic 当作 stream handle 的寄存器值是 `0xe3d26dc9f5bc`，该地址不属于任何有效映射，也不在 dump 捕获的内存范围内。也就是说，Linux relay 保存的 `MsQuicStream*` 或其 `Handle` 已经越过有效生命周期；`relay->Stream != nullptr && relay->Stream->Handle != nullptr` 不能证明 handle 仍可调用 MsQuic API。

## 目标

1. Linux relay 在 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 或任何确认 stream 生命周期结束的路径后，不再保留可被 late TCP error 使用的 stream 指针。
2. TCP late `EPOLLERR/SO_ERROR` 仍能关闭 TCP fd、清理 pending buffers、设置 relay stop；但在 `relay->Closing`、`StreamShutdownComplete` 或 `TcpWriteClosed/tcp_write_shutdown_complete` 之后不能再调用 `MsQuicStream::Shutdown()`。
3. `AbortRelayAndRelease()` 对 stream abort 的判断从“裸指针非空”升级为“relay 仍拥有 active stream binding，且未进入任何本地或 QUIC stream 终态”。
4. 增加回归测试覆盖：`receive_fin -> tcp_write_shutdown_complete -> tcp_socket_error` 不崩溃，并且不会调用 fake MsQuic `StreamShutdown`。
5. 增加最小诊断信号，便于确认生产中是否仍出现 shutdown-complete 后的 late TCP error。

## 非目标

- 不修改 Windows / Darwin relay 生命周期。
- 不改变 MsQuic C++ wrapper `MsQuicStream` 的所有权模型。
- 不把 TCP `SO_ERROR=104` 当作正常关闭静默吞掉；它仍应记录为 relay 级错误。
- 不重构整个 Linux relay worker 或把 `MsQuicStream*` 全面替换成 shared ownership。
- 不改变 admin API 的已有字段语义；新增指标只做补充。

## 方案选择

推荐方案：**shutdown complete 立即 detach stream binding，late TCP error 仅关闭 relay 本地资源**。

当前 `AbortRelayAndRelease()` 的问题不是单个 `if` 条件不够复杂，而是 relay 在 stream 已结束后仍把 `relay->Stream` 视作可调用对象。修复应把“stream 是否仍归 relay 拥有”变成显式状态：

```cpp
bool RelayOwnsAbortableStream(const RelayState& relay) {
    return !relay.Closing &&
           !relay.StreamShutdownComplete &&
           !relay.TcpWriteClosed &&
           relay.Stream != nullptr &&
           relay.StreamBinding != nullptr &&
           !relay.StreamBinding->Closing.load(std::memory_order_acquire) &&
           relay.Stream->Handle != nullptr;
}
```

`TcpWriteClosed` 是 Linux relay 对 `tcp_write_shutdown_complete` 的本地终态标记。它不等价于 QUIC stream shutdown complete，但它表示本 relay 已经完成向 TCP 方向的写关闭；此后同一个 TCP fd 再报告 `EPOLLERR/SO_ERROR` 时，不能把该错误解释成仍需要 abort QUIC stream 的 active-stream 错误。

在收到 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 时，Linux relay 应：

1. 标记 `relay->StreamShutdownComplete = true`。
2. 设置 binding closing，并清空 binding 内 `Relay` / `Handle`。
3. 将 `MsQuicStream::Callback` 改为 `NoOpCallback`，`Context` 置空。
4. 清空 `relay->Stream` 和 `relay->StreamBinding`。
5. 之后调用 `MaybeStopFullyClosedRelay(...)` 或已有 stop 条件处理。

`AbortRelayAndRelease(..., abortStream=true)` 只在 relay 仍拥有 active stream 时 abort stream；如果 relay 已 closing、stream 已 shutdown complete、TCP write 已 shutdown complete，或 stream 已 detach，则记录计数后跳过 stream abort。

### 与 Windows / Darwin 的一致性

Windows relay 已经通过 `CloseRelay(...)`、`Closing`、`TcpWriteClosed` 和 binding closing 把 stream 生命周期收敛到单一关闭路径；Darwin relay 也通过本地 `Closing` / binding active 状态阻止 late callback 继续驱动已关闭 relay。Linux 修复不改动 Windows / Darwin 代码，但需要采用同一约束：**只有 active relay + active binding 可以调用 stream API；任何关闭确认路径都必须先撤销 relay 对 stream 的可调用所有权**。

因此 Linux 的实现不应在各个错误分支重复拼接 `relay->Stream != nullptr` 条件，而应把可 abort 判断集中到 `HasAbortableStream()`，并让 `ProcessQuicShutdownComplete()` / `tcp_write_shutdown_complete` / `Closing` 这些终态共同使该判断返回 false。

### 不选方案

**仅在 `AbortRelayAndRelease()` 中检查 `relay->Closing`。**  
这无法覆盖现场：日志显示 `stream_shutdown_complete` 后的 late `tcp_socket_error` 仍进入 fatal path，而 `relay->Closing` 和 stream 生命周期不是同一个概念。即使 closing 已设置，未来其它路径仍可能持有非空裸 stream 指针。

**把 `relay->Stream->Handle` 置空但保留 `relay->Stream`。**  
这降低崩溃概率，但仍要求 relay 访问一个可能已析构的 C++ wrapper 对象；如果 `MsQuicStream` 本体已被 tunnel context 释放，读取 `Handle` 本身就是 use-after-free。

**让 relay 通过 shared ownership 延长 `MsQuicStream` 生命周期。**  
这能从模型上解决裸指针，但会扩大改动到 tunnel context、client/server stream dispatch、send complete 和 receive complete 路径。当前事故可用更小的 detach 契约修复。

## 数据流

### 正常关闭路径

```text
TCP read EOF
  -> FinishTcpToQuic / submit FIN
MsQuic RECEIVE FIN
  -> QueueDeferredQuicReceive(...)
  -> FlushDeferredQuicReceives(...)
  -> TCP write shutdown complete
MsQuic SHUTDOWN_COMPLETE
  -> ProcessQuicShutdownComplete(...)
  -> DetachRelayStreamBinding(relay, stream, binding)
  -> MaybeStopFullyClosedRelay("stream_shutdown_complete")
```

detach 后，relay 本地仍可保留到 reaper unregister，但 `relay->Stream == nullptr`。后续 TCP fd 如果再上报 `EPOLLERR`，只会走本地资源清理。

### 事故路径修复后

```text
stream_shutdown_complete
  -> relay->StreamShutdownComplete = true
  -> relay->Stream = nullptr
  -> relay->StreamBinding = nullptr

late EPOLLERR / SO_ERROR=104
  -> ProcessTcpEvents(...)
  -> LogLinuxRelayError("tcp_socket_error")
  -> AbortRelayAndRelease(relay, "tcp_socket_error", abortStream=true)
       -> relay has no abortable stream
       -> skip Stream::Shutdown()
       -> reset TCP fd
       -> clear pending queues
       -> Stop=true
```

### callback 与 binding

`StreamRelayBinding` 是 callback 到 relay 的安全桥。detach 后：

- `binding->Closing = true`
- `binding->Relay = nullptr`
- `binding->Handle = nullptr`
- stream callback/context 被置为 no-op/null
- binding 保留在 `RetiredStreamBindings`，继续吸收 late callback 引用，避免 callback 读到已释放 binding

这个行为沿用已有 `DetachRelayStreamBinding()` 设计，只把调用时机前移到 shutdown complete 路径，避免等到 `OutstandingQuicSends == 0` 的后续清理才 detach。

## 错误处理

- TCP `SO_ERROR` 非 0 仍记录 `RelayErrorKind::TcpWriteHard` 和 `FatalRelayResets`。
- 如果 relay 已 closing、stream 已 shutdown complete，或 TCP write 已 shutdown complete，`AbortRelayAndRelease()` 不再调用 `Stream::Shutdown()`；其中 stream shutdown complete 后的 late TCP error 增加 `LateTcpErrorAfterStreamShutdown` 计数。
- 如果 stream 未 shutdown complete 且 binding 仍 active，`AbortRelayAndRelease(..., true)` 保持原 abort 行为。
- 如果 `OutstandingQuicSends > 0`，不能释放 send context；但仍应 detach stream binding，send complete late callback 由现有 send-complete 路径按 context 释放。
- pending QUIC receives 在 abort path 继续 `CompleteAndDiscardQuicReceive()`，避免 MsQuic receive buffer 被长期持有。

## 诊断指标

新增 worker/runtime snapshot 字段：

```cpp
uint64_t LateTcpErrorAfterStreamShutdown{0};
```

当 `ProcessTcpEvents()` 在 `relay->StreamShutdownComplete == true` 后看到 `SO_ERROR != 0` 时递增。admin metrics JSON 增加：

```json
"linux_relay_late_tcp_error_after_stream_shutdown": 1
```

该指标不是用户可操作错误；它用于确认本次修复覆盖的生产模式是否仍在发生，以及发生频率。

## 测试策略

1. `tcpquic_linux_relay_worker_io_test` 新增 fake MsQuic API 测试：注册 relay 后构造 stream shutdown complete，再让 TCP peer reset，触发 `SO_ERROR=104` 或等价 hard error，断言进程不崩溃且 fake `StreamShutdown` 调用次数为 0。
2. 同一测试验证 snapshot 中 `LateTcpErrorAfterStreamShutdown == 1`，`FatalRelayResets >= 1`。
3. 增加 `tcp_write_shutdown_complete` 后 late TCP error 测试：即使 `StreamShutdownComplete` 尚未到达，只要 Linux relay 已标记 `TcpWriteClosed`，后续 TCP `SO_ERROR` 也不能调用 fake `StreamShutdown`。
4. 增加 active stream hard error 测试：stream 尚未 shutdown complete、relay 未 closing、TCP write 也未 closed 时，TCP hard error 仍会调用 fake `StreamShutdown` 一次，避免修复把有效 abort 行为删掉。
5. 对照 Windows / Darwin relay 的关闭路径，确认 Linux stream API 调用只保留在单一 active-stream guard 后面。
6. `tcpquic_router_runtime_test` 或 relay metrics 测试检查 admin metrics JSON 包含 `linux_relay_late_tcp_error_after_stream_shutdown`。
7. 回归运行：

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk git diff --check
```

## 验收标准

- `AbortRelayAndRelease()` 不再仅凭 `relay->Stream != nullptr && relay->Stream->Handle != nullptr` 调用 stream API。
- `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 后，`relay->Stream` 和 `relay->StreamBinding` 被清空。
- `relay->Closing`、`StreamShutdownComplete` 或 `TcpWriteClosed/tcp_write_shutdown_complete` 之后，`AbortRelayAndRelease(..., true)` 不会再次 abort stream。
- late TCP error 不会调用 `MsQuicStream::Shutdown()`，也不会访问失效 stream handle。
- active stream 的 TCP hard error 仍会 abort stream。
- Linux relay 的 stream API 调用点收敛到 `HasAbortableStream()` 这一类单一 active-stream 判断，行为约束与 Windows / Darwin relay 的关闭模型一致。
- 新指标在 worker snapshot、runtime aggregate 和 admin metrics JSON 中贯通。
- crash dump 中的事故路径可由单元测试稳定覆盖，不依赖真实 Crashpad/core。

## 自检

- 文档没有占位项；事故证据、目标、非目标、方案和测试入口均已明确。
- 方案没有把 TCP hard error 静默降级；只禁止 shutdown-complete 后访问失效 stream。
- 范围聚焦 Linux relay stream 生命周期；Windows/Darwin 仅作为关闭模型对照，不做代码改动。
