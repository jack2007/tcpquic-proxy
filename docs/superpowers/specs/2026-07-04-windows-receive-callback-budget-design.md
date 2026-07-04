# Windows Receive Callback Budget Design

## 背景

`docs/relay_win.md` 目前还剩两个相关问题：

- 3.4：Windows `StreamCallback(RECEIVE)` 先调用 `BuildDeferredQuicReceiveView()` 复制 MsQuic buffers，再由 worker 线程的 `EnqueueDeferredQuicReceiveView()` 检查 `WindowsRelayMaxPendingQuicReceiveBytesPerRelay`。
- 3.8：maintenance 和 receive-finish 指标已补齐，但 callback 复制字节/耗时仍没有直接指标。

这两个问题属于同一条路径。当前代码能在 worker 入队时暂停 QUIC receive，但不能阻止 callback 线程先复制一个大 receive view，也不能直接观测 callback copy 的字节数和耗时。

## 目标

1. 在 `StreamCallback(RECEIVE)` 复制 MsQuic buffers 之前做轻量预算判断。
2. 预算判断只读取 callback-safe 状态，不直接访问 worker-owned 队列。
3. 当 relay pending bytes 已达上限或本次 receive 会明显超过上限时，优先在 callback 线程暂停 QUIC receive，并跳过复制。
4. 保留 worker 线程 `EnqueueDeferredQuicReceiveView()` 的预算检查，作为并发竞态兜底。
5. 新增 callback receive copy 和预算拒绝指标，并接入 worker snapshot、relay metrics JSON 和 trace summary。
6. 更新 `docs/relay_win.md`，把 3.4 标为已处理，并关闭 3.8 中 callback copy 指标缺口。

## 非目标

- 不改 MsQuic receive ownership 模型；本轮仍复制 buffers 到 `OwnedBuffer`。
- 不引入分片处理。
- 不修改 fake FIN fail-fast 行为。
- 不改变 worker maintenance queue 或 receive-finish 逻辑。
- 不更改 `WindowsRelayMaxPendingQuicReceiveBytesPerRelay` 的配置语义。

## 设计

### Callback-Safe Budget Check

新增 helper：

```cpp
uint64_t ComputeReceiveEventBytes(const QUIC_STREAM_EVENT& event);
bool ShouldRejectReceiveInCallback(
    const CallbackBinding& binding,
    const QUIC_STREAM_EVENT& event,
    uint64_t receiveBytes);
```

`ComputeReceiveEventBytes()` 只遍历 `event.RECEIVE.Buffers`，累加 `Length`，并对 `BufferCount != 0 && Buffers == nullptr`、`Length != 0 && Buffer == nullptr`、size overflow 返回特殊失败值或让调用方记录错误。

`ShouldRejectReceiveInCallback()` 读取：

- `binding.RelayHint.load(std::memory_order_acquire)`
- `binding.Generation`
- `relay->Generation`
- `relay->Closing`
- `relay->PendingQuicReceiveBytes`
- `relay->QuicReceivePaused`
- `relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay`

如果 hint 为空、generation 不匹配、relay closing 或 limit 为 0，则不做预算拒绝，让既有 worker 解析/stale/drop 逻辑兜底。

如果 `pendingBytes >= limit` 或 `receiveBytes > limit - pendingBytes`，callback 线程调用已有 MsQuic API：

```cpp
worker->SetQuicReceiveEnabledFromCallback(binding, relay, false);
```

该 helper 只做 `StreamReceiveSetEnabled(FALSE)` 和 atomic flag/metric 更新，不操作 worker queue。它需要用 `compare_exchange` 或 `exchange` 避免重复 pause。

### Callback Return Semantics

预算拒绝发生在复制前。callback 不接管 MsQuic receive ownership，因此返回 `QUIC_STATUS_SUCCESS`。这表示本次 receive 不会 pending，MsQuic 可以继续按正常语义释放本次 callback buffer。暂停 receive 只阻止后续 receive delivery。

如果 receive 带 FIN，预算拒绝仍不应静默丢 FIN。设计要求：

- `fin && receiveBytes == 0` 走现有 fake FIN 检查或正常 FIN 路径，不走预算拒绝。
- `fin && receiveBytes > 0` 允许复制并投递，因为 FIN 是状态边界；worker 入队后继续按现有 drain/close 处理。

### Copy Metrics

在 `BuildDeferredQuicReceiveView()` 中记录复制前后的 steady nanos，并将成功复制的 `view->TotalLength` 计入：

- `CallbackReceiveCopyBytes_`
- `CallbackReceiveCopyNanos_`

如果函数因为非法 buffers、overflow、empty non-FIN 返回 nullptr，不增加 copy bytes；耗时只统计实际分配/复制路径。

### Budget Metrics

新增 worker snapshot 和 metrics 字段：

- `CallbackReceiveBudgetRejectedCount`
- `CallbackReceiveBudgetPausedCount`
- `CallbackReceiveCopyBytes`
- `CallbackReceiveCopyNanos`

公开 JSON 字段：

- `windows_relay_callback_receive_budget_rejected_count`
- `windows_relay_callback_receive_budget_paused_count`
- `windows_relay_callback_receive_copy_bytes`
- `windows_relay_callback_receive_copy_nanos`

trace summary 使用紧凑字段：

- `win_cb_recv_budget_rejected`
- `win_cb_recv_budget_paused`
- `win_cb_recv_copy_bytes`
- `win_cb_recv_copy_nanos`

### Worker-Side Backstop

`EnqueueDeferredQuicReceiveView()` 保持现有检查：入队后累计 `PendingQuicReceiveBytes`，超过 limit 时调用 `SetQuicReceiveEnabled(relay, false)`。callback 预算只是提前拒绝明显超限的 receive；并发竞态仍由 worker 侧检查兜底。

### Error Handling

- 非法 buffer 指针仍递增 `Errors_` 并返回 `QUIC_STATUS_SUCCESS`，保持现有行为。
- callback budget 拒绝不递增 `Errors_`，因为这是一种背压行为。
- 如果 `StreamReceiveSetEnabled(FALSE)` 失败，计入 `Errors_`，但仍跳过复制并返回 `QUIC_STATUS_SUCCESS`，避免 callback 线程继续放大内存压力。

## 测试策略

### Windows 单测

在 `src/unittest/windows_relay_worker_test.cpp` 增加：

1. 初始 snapshot 中新增 callback budget/copy 指标为 0。
2. pending bytes 已接近 limit 时触发 RECEIVE callback，不投递 `RelayReceiveReady`，`CallbackReceiveBudgetRejectedCount` 增加。
3. 第一次预算拒绝会调用 `StreamReceiveSetEnabled(FALSE)`，`CallbackReceiveBudgetPausedCount` 增加。
4. 预算允许时仍复制并投递 `RelayReceiveReady`，`CallbackReceiveCopyBytes` 增加。
5. 带 FIN 的非空 receive 不走预算拒绝，仍投递给 worker。
6. stale generation / closing binding 不崩溃，不直接访问 worker queue。

### 本地验证

Linux 环境可运行：

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
rtk cmake --build build --target tcpquic_platform_socket_test tcpquic_thread_pool_test -j$(nproc)
rtk proxy ./build/bin/Release/tcpquic_platform_socket_test
rtk proxy ./build/bin/Release/tcpquic_thread_pool_test
```

Windows 环境需运行：

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

## 风险和约束

- callback 线程只能读 atomic 状态和调用 MsQuic receive enable API，不能直接操作 `PendingReceives`、maintenance queue 或 relay map。
- budget 拒绝可能丢弃当前 receive payload，因此只能在后续 receive 已被暂停、且本次 receive 不含 FIN 的情况下使用。若产品语义要求不能丢弃任何 payload，应改为“复制但立即暂停后续 receive”，只保留 copy metrics。本 spec 选择按文档中“复制前预算拒绝路径”实现。
- `RelayHint` 是优化路径，不是权限来源。hint 为空或 generation 不匹配时必须退回现有 IOCP/stale 逻辑。

## 文档更新

实现后更新 `docs/relay_win.md`：

- 3.4 改为“已在 callback 复制前做轻量预算判断，超限时暂停后续 receive 并跳过复制”。
- 3.8 删除 callback copy 指标缺口。
- 优先级表中 `receive callback 复制前加入预算判断和指标` 标为已完成。
