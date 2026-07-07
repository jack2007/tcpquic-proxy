# Windows relay worker review

## 1. 背景

参考 `docs/linux_relay_worker_cpu_loop_error.md` 的 Linux relay worker CPU 忙循环问题，对 Windows relay worker 做同类风险排查。

Linux 问题的最小根因是 `epoll_event.data` 混用了 wake fd 和 relay id：当 wake fd 数值与某个 relay id 碰撞时，TCP fd 的 HUP / RDHUP 事件会被误判成 wake event，导致真正的 TCP 关闭处理路径不执行。由于 fd readiness 条件仍然存在，`epoll_wait()` 会持续立即返回，形成单 worker 线程 CPU 忙循环。

Windows relay worker 使用 IOCP，事件模型不同。当前未发现与 Linux 同类的 wake / relay id 数值碰撞问题。

## 2. Windows maintenance queue 的相似表象

排查中看到一个表象相似但机制不同的点：Windows relay worker 有 per-relay maintenance queue。

相关路径位于 `src/tunnel/windows_relay_worker.cpp`：

- `ScheduleRelayMaintenance()` 会把 relay 放入 `MaintenanceQueue_`。
- `RelayNeedsMaintenance()` 判断 relay 是否仍需维护。
- `DrainPerRelayMaintenance()` 每轮处理一批 maintenance relay。
- `Run()` 在等待 IOCP completion 前后都会调用 `DrainPerRelayMaintenance()`。

如果某个 relay 处在以下状态之一，`RelayNeedsMaintenance()` 可能持续返回 true：

- `Closing=true`
- `CloseAfterDrained=true`
- `TcpReadPausedByQuicBacklog=true`
- `PendingQuicSendRetries` 非空
- `PendingQuicReceiveQueueDepth != 0`
- QUIC receive 被暂停且仍需恢复检查

此时 `DrainPerRelayMaintenance()` 处理完该 relay 后，可能再次调用 `ScheduleRelayMaintenance()` 将其重新排队。这看起来像“同一个 relay 反复进入 worker 处理路径”。

## 3. 为什么它不是 Linux 同类 CPU loop

这个 maintenance queue 行为不等价于 Linux epoll 忙循环，关键差异是 worker 的阻塞点不同。

Windows worker 主循环中，maintenance drain 后会进入：

```cpp
GetQueuedCompletionStatus(Iocp_, &bytes, &key, &overlapped, INFINITE)
```

也就是说，如果没有新的 IOCP completion、控制命令或 callback posted operation，worker 会阻塞等待。maintenance queue 本身不会创建一个总是立即返回的 OS readiness 条件。

Linux 问题中，TCP fd 的 HUP / RDHUP readiness 条件持续存在，且被误分发为 wake event 后没有清理 relay，因此 `epoll_wait()` 会立即再次返回同一个事件。Windows maintenance queue 即使反复 requeue，也需要 worker 从真实 IOCP completion 返回后才会继续推进下一轮主循环；在没有新 completion 时不会独立自旋。

## 4. 关闭路径上的防御

Windows TCP read half-close 也有明确防御：

- `HandleTcpRecv()` 在 TCP recv completion `bytes == 0` 时调用 `HandleTcpReadClosed()`。
- `HandleTcpReadClosed()` 将 `TcpRecvClosed` 置为 true，并向 QUIC 提交 FIN。
- `MaybePostTcpRecv()` 开头检查 `TcpRecvClosed`，已关闭后不会再投递新的 `WSARecv()`。
- IOCP teardown / stale completion 路径会记录 errno、降级为 graceful close 或丢弃 stale completion，不会把 completion 误分发成另一类事件。

因此 Windows 不存在 Linux 中 `tcp_read_closed=true` 但仍然因为 fd readiness 持续唤醒并绕过关闭处理的同类机制。

## 5. 后续观察指标

如果现场怀疑 Windows relay worker CPU 异常，可以重点观察这些指标，而不是直接套用 Linux wake fd / relay id 碰撞结论：

- `WindowsRelayMaintenanceDrainCount`
- `WindowsRelayMaintenanceRelaysProcessed`
- `WindowsRelayMaintenanceFullScanCount`
- `WindowsRelayMaintenanceFullScanRelaysScanned`
- `WindowsCallbackIocpPostCount`
- `WindowsPostedCallbackStaleDropCount`
- `IocpCompletionDowngraded`
- `IocpStaleCompletionDropped`
- active relay 中的 `Closing`、`CloseAfterDrained`、`InFlightTcpRecvs`、`InFlightTcpSends`、`InFlightQuicSends`、`PendingQuicReceiveQueueDepth`、`OutstandingQuicSendBytes`

如果 maintenance drain / processed 指标随 CPU 高占用快速增长，同时 IOCP completion / callback post 也快速增长，需要继续排查是哪类 completion 或 callback 在高频投递。若 maintenance 指标不增长，而某个 relay 长期卡在 close-after-drained 或 pending backlog，则更可能是资源回收或完成事件缺失问题，而不是 worker 空转忙循环。

## 6. 当前结论

Windows maintenance queue 可能呈现“同一 relay 反复被维护”的表象，但当前实现每轮 maintenance 后会阻塞等待 `GetQueuedCompletionStatus(..., INFINITE)`。没有新的 IOCP completion 时，worker 不会因为 maintenance queue 本身持续占用 CPU。

因此该点不是 Linux epoll wake fd / relay id 碰撞的同类 CPU loop。它更适合作为 Windows relay 卡住或完成事件缺失时的观察线索。
