# macOS Darwin relay 锁边界

本文记录 Darwin kqueue relay 在 Phase 1-4 后的锁边界和仍需验证的原型项。

## 当前实现状态

Phase 1 后，QUIC receive callback 只通过 `StreamBinding` 上的原子计数预留接收预算并投递队列事件，不再在 callback 内直接修改 relay 的 pending-byte / pending-queue 状态。relay 的 `PendingQuicReceiveBytes` 和 `PendingQuicReceives` 只在 worker 事件路径中推进。

Phase 2 后，`SEND_COMPLETE` 和 QUIC terminal callback 已事件化。`SEND_COMPLETE` callback 只 claim completion 并投递 `QuicSendComplete` 事件；detached / late callback 仍通过 completion state 兜底清理。terminal callback 同步设置 close-pending marker，阻止后续 TCP->QUIC 继续推进，然后投递 worker 事件执行关闭。

Phase 3 后，register / unregister / snapshot 通过同步 control event 进入 worker，调用方等待命令完成；队列入队失败时保留唤醒重试语义，避免 command 栈对象在 worker 使用前失效。

Phase 4 后，本 Linux-only 原型采取保守边界：保留 worker-local relay lookup / known-send helper 的结构，但不在当前提交中让 `Relays`、`RetiredRelays`、`KnownSendOperations` 这类 worker-owned 容器进入无锁读写。原因是 callback fallback、test helper、stop / retire / destructor 仍会跨线程观察或修改这些容器；在没有 macOS TSAN / kqueue / MsQuic callback 验证前，正常 worker 事件路径继续通过带锁 wrapper 查找 relay，known-send local helper 也继续使用 `KnownSendMutex`。Phase 4 已把 callback SEND_COMPLETE 的 claim 迁移到每个 binding 的 `CompletionState::Mutex`，并保留同一 known-send 锁域，避免 `RelayMutex` 扩大到 send completion 跟踪。

## 仍保留的锁

`RelayMutex` 仍保留在外部 wrapper、stop / retire / destructor、retired binding 清理、snapshot / metrics、以及 callback 线程需要安全查找 relay 的 fallback 路径中。`PurgeQueuedEventsForStop()` 没有改为 local lookup，因为它可在 stop / destructor 路径由非 worker 线程调用；此处继续使用带锁 lookup 更保守。

`KnownSendMutex` 仍保留在 worker local known-send helper、callback fallback、test helper、stop / drain helper 中。它保护 `KnownSendOperations` 的全部跨线程访问；后续若要移除它，必须先消除 callback / test / stop 对 worker map 的直接访问或改为生命周期安全的 worker-owned handoff。

`CompletionState::Mutex` 仍保留在 SEND_COMPLETE callback claim、worker register / submit-mark / unregister 同步 completion state、以及 detached / late callback 清理中。它是 per-binding 锁，用于 callback 不读取 worker map 的 claim 和 detached cleanup。

`RelayState::Mutex` 仍保留在 `ProcessTcpEvents()` 下游的 `DrainTcpReadable()`、`SubmitTcpBatchToQuic()`、`RetryPendingQuicSends()`、`ProcessQuicReceiveViewEvent()` 及 TCP write / QUIC receive 辅助函数中。虽然这些函数大多由 worker 正常数据路径调用，但同一批状态也会被 test helper、snapshot、stop / retire、callback close marker 和 late completion fallback 观察或修改；在当前 Linux 环境无法运行 Darwin 压测前，不继续移除这些 per-relay 锁。

## macOS 原型待验证项

当前环境是 Linux，无法运行 Darwin kqueue / MsQuic callback 组合测试；本阶段只做静态检查和可用的 CMake 尝试。后续需要在 macOS 上验证：

- Darwin relay unit / IO tests 覆盖 register / unregister / snapshot control event 的唤醒和栈对象生命周期。
- QUIC receive callback 高并发下，callback 原子预算与 worker pending-byte 状态不出现泄漏或重复 release。
- `SEND_COMPLETE` 在同步完成、异步完成、queue full、retired binding、late callback 下不重复 delete send operation，`CompletionState::KnownSendOperationCount` 能归零。
- terminal callback 设置 close-pending marker 后，worker 不再提交新的 TCP->QUIC send，并能完成 retire / destructor 清理。
- Phase 4 后正常数据事件中的 `RelayMutex` 热点下降；剩余 `RelayState::Mutex` 是否可继续缩小，需要以 macOS perf / TSAN / 压测数据为准。
