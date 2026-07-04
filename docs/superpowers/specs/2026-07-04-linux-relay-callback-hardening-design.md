# Linux relay callback hardening 设计

`docs/relay_linux.md` 剩余问题中，`3.6 收到 MsQuic fake FIN 时直接 abort 进程` 是当前优先级最高的生产风险：单个 stream 的异常 receive 形态会导致整个进程退出。`3.9 relay id lookup 已优化，但 fd/stream fallback 仍有线性扫描` 与同一条 callback/stream 诊断链路相关：主路径已经通过 `StreamRelayBinding` 和 relay id 避免扫描，但 fallback 扫描只停留在 worker snapshot 内，admin metrics 还无法直接观察。

本阶段合并处理这两项，形成一组 Linux relay callback hardening 工作：fake FIN 从进程级 fail-fast 改为单 relay fatal reset，并把 stream lookup fallback 扫描计数贯通到 admin metrics。`3.3 QUIC receive 背压没有 hysteresis` 本轮不处理；`3.7 metrics 原子内存序偏保守` 范围更大，另行排期。

## 目标

1. Linux relay callback 收到 MsQuic fake FIN receive 时，不再执行 `assert(false)` / `std::abort()` 终止进程。
2. fake FIN 仍被视为 relay fatal error：记录 trace、计数、关闭当前 relay/stream，不能静默当作正常 FIN。
3. admin metrics 暴露 stream lookup fallback 扫描次数，便于判断 `FindRelayIdByStream()` 是否仍在生产路径被触发。
4. 更新 `docs/relay_linux.md`，让主文档保留粗颗粒度问题和推进状态，详细设计/计划沉到 `docs/superpowers/`。

## 方案选择

推荐方案：**单 relay fatal reset + metrics 贯通**。

- fake FIN 分支保留 `TraceRelayStreamEvent(..., "receive_fake_fin", ...)`，随后记录 `FakeFinReceiveCount`，调用 `FailRelayFatal(relay, "quic_receive_fake_fin", true)`，并返回 `QUIC_STATUS_SUCCESS`。
- 返回 `QUIC_STATUS_SUCCESS` 的原因是 fake FIN 分支不保存 MsQuic receive buffer，也不会调用 `ReceiveComplete()`；不能返回 `QUIC_STATUS_PENDING` 让 MsQuic 认为 buffer 被 relay 持有。
- `FailRelayFatal()` 会增加 `FatalRelayResets`、记录错误日志、reset TCP fd、abort stream，并通过已有 binding detach/lifetime 流程隔离当前 relay。
- 额外暴露 `linux_relay_fake_fin_receive_count`，避免只看 `fatal_relay_resets` 时无法区分 fake FIN 与其它 fatal relay reset。
- 把已有 `TqLinuxRelayWorkerSnapshot::StreamLookupScanCount` 聚合到 `TqRelayMetricsSnapshot`，输出 `linux_relay_stream_lookup_scan_count`。

不选方案：

- **继续 fail-fast**：能强提示 MsQuic 语义异常，但生产 blast radius 太大，已不符合 Linux relay 单连接隔离目标。
- **把 fake FIN 当正常 FIN**：会掩盖 receive offset/total size 异常，可能让数据完整性问题被误判为正常关闭。
- **本轮新增 `RelaysByStream` map**：可以消除 fallback 扫描，但当前扫描不在主路径；先暴露指标判断真实触发频率，再决定是否加 map。

## 组件和数据流

### Fake FIN callback

当前入口是 `TqLinuxRelayWorker::OnStreamEventWithBinding()`：

```text
MsQuic RECEIVE callback
  -> StreamRelayBinding 校验
  -> TqIsMsQuicFakeFinReceive(...)
  -> TraceRelayStreamEvent("receive_fake_fin")
  -> FakeFinReceiveCount++
  -> FailRelayFatal(relay, "quic_receive_fake_fin", abortStream=true)
  -> QUIC_STATUS_SUCCESS
```

该路径不进入 `QueueDeferredQuicReceive()`，不向 event queue 投递 receive view，也不调用 `ReceiveComplete()`。relay 关闭继续使用现有 `FailRelayFatal()` / `AbortRelayAndRelease()` 生命周期路径。

### Stream lookup fallback metrics

`FindRelayIdByStream()` 当前每次扫描前已经更新 `StreamLookupScanCount`。本阶段只贯通指标：

```text
FindRelayIdByStream()
  -> StreamLookupScanCount++
  -> SnapshotLocal().StreamLookupScanCount
  -> Runtime::Snapshot() 聚合
  -> TqCollectRelayMetrics()
  -> TqAppendRelayMetricsJson()
  -> linux_relay_stream_lookup_scan_count
```

`StreamLookupScanCount > 0` 表示存在 fallback 或旧路径仍按 stream 扫描 relay。它不是错误计数，但排障时应结合 trace 或后续调用栈审计判断来源。

## 错误处理

- fake FIN 属于当前 relay fatal error，不终止进程。
- callback 返回 `QUIC_STATUS_SUCCESS`，避免 MsQuic 等待 relay 归还一个并未被保留的 receive buffer。
- `FailRelayFatal()` 的 `abortStream=true` 保持对端可见的异常关闭语义。
- 如果 relay 已经 closing，则 fake FIN 分支仍只记录 count/trace 后按现有 closing guard 返回 success，不重复 reset。

## 测试策略

1. `tcpquic_linux_relay_worker_io_test` 增加 fake FIN 用例：注册一个 fake stream，构造 `AbsoluteOffset=UINT64_MAX`、`TotalBufferLength=0`、`BufferCount=0`、`Flags=QUIC_RECEIVE_FLAG_FIN` 的 receive event，验证 callback 返回 `QUIC_STATUS_SUCCESS`，进程不退出，snapshot 中 `FakeFinReceiveCount == 1`、`FatalRelayResets == 1`、relay 进入 closing。
2. 同一测试确认 fake FIN 不增加 `DeferredReceiveCompletes`，因为没有 receive buffer 被持有。
3. `tcpquic_router_runtime_test` 增加 admin metrics JSON 字段检查：`linux_relay_fake_fin_receive_count` 和 `linux_relay_stream_lookup_scan_count` 必须存在。
4. 文档检查使用 `rtk git diff --check`。

## 范围外

- 不修改 Windows/macOS fake FIN 行为；相关文档已分别说明平台策略。
- 不实现 QUIC receive hysteresis。
- 不做全量 metrics `memory_order_relaxed` 改造。
- 不新增 `RelaysByStream` 或 `RelaysByFd` map；只有当新指标显示 fallback 扫描在生产中持续出现时再排期。

## 自检

- 无占位项；本阶段目标、错误语义、指标名和测试入口均已明确。
- 方案没有把 fake FIN 降级为正常关闭；仍保留 fatal relay reset 信号。
- 范围聚焦 Linux relay callback 和 stream lookup 观测，不与 QUIC receive 背压或全局 metrics 优化混在一起。
