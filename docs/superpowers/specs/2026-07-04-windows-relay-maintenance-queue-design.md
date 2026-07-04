# Windows Relay Maintenance Queue Design

## 背景

`docs/relay_win.md` 目前指出三个相互关联的问题：

- `DrainPerRelayMaintenance()` 在 Windows worker 每次 IOCP wait 前和每次 event 后扫描该 worker 上的所有 relay。
- `FinishReceiveView()` 在正常按队首 drain 的路径里仍用 `std::find()` 删除 `PendingReceives` 中的 view。
- 现有指标能看到 worker lock 和 snapshot 成本，但看不到 maintenance 扫描成本，也看不到 receive view 完成时是否走了异常线性查找。

这些问题都属于 Windows relay 数据面热路径的可扩展性问题。目标不是改变 relay 语义，而是把常规路径从全量扫描和线性查找收敛到事件驱动，同时补齐能验证优化效果的指标。

## 目标

1. `DrainPerRelayMaintenance()` 不再在每个 IOCP event 前后无条件扫描所有 active relay。
2. 常规维护改为事件驱动：只有状态变化后确实需要 retry、resume read、drain close 或 retire 的 relay 进入 maintenance queue。
3. 每轮 maintenance drain 有预算，避免单个 IOCP event 后处理过多 relay。
4. `FinishReceiveView()` 正常路径使用 `PendingReceives.front() == view` 后 `pop_front()`。
5. 非队首完成或 missing view 仍保留线性查找/诊断路径，并记录指标。
6. 新增 Windows worker 级指标，能观测 maintenance 扫描次数、耗时、处理 relay 数，以及 receive view 异常线性查找。
7. 更新 `docs/relay_win.md`，把 3.2、3.3、3.8 的对应部分标为已处理或状态更新。

## 非目标

- 不处理 `WindowsRelayWorkerCount` 命名问题。
- 不处理 receive callback 复制前预算控制。
- 不清理所有 per-relay 残留 mutex；本轮只在触及的队列路径补必要注释。
- 不改变 fake FIN fail-fast 行为。
- 不引入新的线程或 timer thread。低频兜底扫描通过现有 IOCP worker loop 中的轻量计数触发。

## 设计

### Maintenance Queue

在 `TqWindowsRelayWorker` 内新增 worker-owned maintenance queue：

- `std::deque<std::shared_ptr<RelayContext>> MaintenanceQueue_`
- `std::unordered_set<uint64_t> MaintenanceQueuedRelayIds_`
- `uint32_t MaintenanceFullScanCountdown_`

这些字段只在 worker 线程访问，不使用额外 mutex。跨线程 callback 仍通过 IOCP 投递 operation；worker 解析 operation 后再调用 `ScheduleRelayMaintenance()`。

`ScheduleRelayMaintenance(relay)` 的语义：

- relay 为空或已经 `StopPublished` 时不入队。
- 同一 relay 在队列中只保留一份。
- 入队只表示“需要重新检查维护条件”，不代表一定有 work。

### Maintenance Drain

新增常量：

```cpp
constexpr size_t kWindowsRelayMaintenanceBudget = 64;
constexpr uint32_t kWindowsRelayMaintenanceFullScanInterval = 256;
```

`DrainPerRelayMaintenance()` 改为：

1. 记录开始时间。
2. 从 `MaintenanceQueue_` 中最多弹出 `kWindowsRelayMaintenanceBudget` 个 relay。
3. 对每个 relay 执行现有维护动作：`RetryPendingQuicSends()`、`MaybePostTcpRecv()`、`CloseRelayIfDrained()`、`TryRetireRelay()`。
4. 如果处理后 relay 仍有 pending retry、TCP read backlog pause、close-after-drained、closing-but-not-retired 等条件，则重新入队。
5. 每处理固定数量 IOCP event 后做一次 full-scan fallback：复制 `Relays_`，但只把需要维护的 relay 入队，不立即对所有 relay 执行维护。

这样保留异常兜底能力，但常规 event 不再 O(active relay count)。

### 入队触发点

以下路径应调用 `ScheduleRelayMaintenance()`：

- relay register 完成后，用于投递初始 TCP recv 和检查初始状态。
- QUIC ideal send buffer 更新后，用于 retry pending QUIC sends 和恢复 TCP read。
- QUIC send complete 后，用于 retry/resume/close。
- TCP recv/send completion 后，用于 close-after-drained、retry 和 retire。
- `EnqueueDeferredQuicReceiveView()`、`FinishReceiveView()`、`MaybeResumeQuicReceive()` 相关路径，用于继续 receive drain。
- `CloseRelay()`、`CloseRelayIfDrained()`、`ScheduleRelayReceiveDrainOrFail()` 后，用于 retire。

入队应尽量集中在现有 helper 中，避免每个调用点重复判断。例如：

- `SetTcpReadBackpressure()` 在 pause/resume 状态变化后入队。
- `ScheduleRelayReceiveDrain()` 已有 coalesce 语义，保留它专门负责 receive drain operation；finish 后入队维护 close/retire。
- `MarkRelayCloseReason()` 不入队，真正 close 动作处入队。

### Receive View Finish

`FinishReceiveView()` 改为优先队首删除：

```cpp
bool removed = false;
if (!relay->PendingReceives.empty() && relay->PendingReceives.front() == view) {
    relay->PendingReceives.pop_front();
    removed = true;
} else {
    const uint64_t searchStartNanos = NowSteadyNanos();
    const auto it = std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view);
    ReceiveViewFinishLinearSearchNanos_.fetch_add(
        NowSteadyNanos() - searchStartNanos,
        std::memory_order_relaxed);
    ReceiveViewFinishLinearSearchCount_.fetch_add(1, std::memory_order_relaxed);
    if (it != relay->PendingReceives.end()) {
        ReceiveViewFinishNotFrontCount_.fetch_add(1, std::memory_order_relaxed);
        relay->PendingReceives.erase(it);
        removed = true;
    }
}
```

如果 `removed == false`，保留当前 `finish_already_drained` / `finish_missing` trace 和返回行为。

这明确表达 Windows QUIC->TCP 是单队首 drain 模型：正常 completion 应该完成队首 view；非队首 completion 是诊断事件。

### 新增指标

在 `TqWindowsRelayWorkerSnapshot`、`TqRelayMetricsSnapshot`、Windows snapshot 聚合、admin JSON 和 trace summary 中增加：

- `WindowsMaintenanceDrainCount`
- `WindowsMaintenanceDrainNanos`
- `WindowsMaintenanceRelaysProcessed`
- `WindowsMaintenanceFullScanCount`
- `WindowsMaintenanceFullScanRelaysScanned`
- `WindowsReceiveViewFinishLinearSearchCount`
- `WindowsReceiveViewFinishLinearSearchNanos`
- `WindowsReceiveViewFinishNotFrontCount`

命名统一使用 `windows_relay_...` JSON 字段，例如：

- `windows_relay_maintenance_drain_count`
- `windows_relay_maintenance_drain_nanos`
- `windows_relay_maintenance_relays_processed`
- `windows_relay_maintenance_full_scan_count`
- `windows_relay_maintenance_full_scan_relays_scanned`
- `windows_relay_receive_view_finish_linear_search_count`
- `windows_relay_receive_view_finish_linear_search_nanos`
- `windows_relay_receive_view_finish_not_front_count`

## 测试策略

### 单元测试

在 `src/unittest/windows_relay_worker_test.cpp` 增加测试：

1. maintenance 初始状态指标为 0。
2. 注册 relay 后，不依赖全表扫描也能投递初始 TCP recv。
3. 多 relay 场景中，只入队的 relay 被 maintenance drain 处理；未入队 relay 不增加 processed 计数。
4. maintenance budget 生效：入队数量超过预算时，单轮只处理预算数量，后续轮继续处理。
5. `FinishReceiveView()` 队首路径不增加 linear-search 指标。
6. 人工构造非队首完成时增加 `ReceiveViewFinishLinearSearchCount` 和 `ReceiveViewFinishNotFrontCount`。

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

- 不能丢失 close/retire 机会。full-scan fallback 必须保留，直到压力测试证明所有维护触发点覆盖完整。
- 不能在 callback 线程直接访问 worker-owned maintenance queue。
- 不能把 receive drain operation 和 maintenance queue 混为一体；receive drain 仍用现有 IOCP operation 保持 TCP write 顺序。
- 新指标是累计值，不用于控制逻辑。

## 文档更新

实现后更新 `docs/relay_win.md`：

- 3.2 改为“已改为事件驱动维护队列，保留低频兜底扫描”。
- 3.3 改为“队首完成走 O(1)，异常才线性查找并计数”。
- 3.8 删除已补齐的 maintenance/linear-search 指标缺口，只保留 callback copy 指标等未处理项。
- 优先级表中对应项标为已完成。
