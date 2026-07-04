# Linux relay 控制面 wait 和 snapshot 持锁收敛设计

## 背景

`docs/relay_linux.md` 当前仍有三类相关问题：

- `3.4 runtime snapshot 持锁范围偏大`：`TqLinuxRelayRuntime::Snapshot()` 和 `SnapshotWorkers()` 在持有 `Runtime::Lock` 时逐个调用 `worker->Snapshot()`。worker snapshot 会投递同步 event 并等待 worker 完成，admin 请求可能间接阻塞 `PickWorker()`。
- `3.5 ControlLock 持锁等待 worker command`：`RegisterRelayWithId()`、`UnregisterRelay()`、`Snapshot()` 等控制面入口在持有 `ControlLock` 后 enqueue command，并等待 command `Cv`。worker 忙或 event queue 满时，同 worker 的 stop/snapshot/register 会被串行放大。
- `3.8` 的剩余部分：Linux queue 侧 capacity/CAS/multi-producer 观测已补齐，但仍缺 `ControlLock`、`Runtime::Lock`、command wait 的等待时间和超时指标。

这三项属于同一条控制面链路：admin snapshot、建链 register、拆链 unregister 和 worker command wait 互相影响。只加指标无法降低阻塞，只缩锁又缺少生产证据。因此本阶段把“缩小持锁范围”和“补齐控制面 wait 观测”作为一个完整工作单元。

## 目标

1. `TqLinuxRelayRuntime::Snapshot()` 和 `SnapshotWorkers()` 不在持有 `Runtime::Lock` 时等待 worker snapshot。
2. `TqLinuxRelayWorker::RegisterRelayWithId()`、`UnregisterRelay()`、`Snapshot()` 不在持有 `ControlLock` 时等待 command `Cv`。
3. register/snapshot enqueue 失败路径使用 bounded retry，不再一次失败就静默返回空结果。
4. 同步 command wait 增加 timeout，避免 worker 卡住时调用方无限等待。
5. Linux relay metrics/admin JSON 暴露控制面等待和 timeout 指标。
6. 保持现有数据面行为不变：TCP/QUIC 转发、背压阈值、event queue 语义、active relay 明细输出不改变。

## 非目标

- 不修改 QUIC receive hysteresis。
- 不处理 MsQuic fake FIN abort。
- 不改 Linux event queue 数据结构或 wake 合并策略。
- 不重写 test helper 的所有同步 command；本阶段只覆盖生产路径和直接支撑测试的 helper。
- 不把 worker lifetime 改成 `shared_ptr`。第一阶段通过 runtime lock 内复制 raw pointer 列表并保持 `Stop()` 与 snapshot 串行来控制生命周期。

## 设计

### Runtime snapshot 解锁

当前实现：

```cpp
TqLinuxRelayWorkerSnapshot TqLinuxRelayRuntime::Snapshot() const {
    std::lock_guard<std::mutex> guard(Lock);
    TqLinuxRelayWorkerSnapshot total{};
    for (const auto& worker : Workers) {
        const auto snapshot = worker->Snapshot();
        ...
    }
    return total;
}
```

目标实现：

```cpp
std::vector<TqLinuxRelayWorker*> TqLinuxRelayRuntime::CopyWorkersForSnapshot() const {
    auto guard = AcquireRuntimeLockForMetrics();
    std::vector<TqLinuxRelayWorker*> workers;
    workers.reserve(Workers.size());
    for (const auto& worker : Workers) {
        workers.push_back(worker.get());
    }
    return workers;
}
```

`Snapshot()` 和 `SnapshotWorkers()` 先复制 worker 指针，再释放 `Runtime::Lock`，随后逐 worker 调 `Snapshot()`。`Stop()` 仍持有 `Runtime::Lock` 并清空 `Workers`；因此 snapshot 复制列表期间不会与清空并发。复制完成后的 raw pointer 生命周期由 runtime-level snapshot in-flight 计数保护：

```cpp
mutable std::mutex SnapshotLock;
mutable std::condition_variable SnapshotCv;
mutable uint32_t RuntimeSnapshotsInFlight{0};
```

流程：

- `AcquireSnapshotWorkers()` 在 `Runtime::Lock` 内复制 worker 指针，并在释放 `Runtime::Lock` 前增加 `RuntimeSnapshotsInFlight`。
- `ReleaseSnapshotWorkers()` 减少 in-flight 并 notify。
- `Stop()` 在清空 `Workers` 前等待 `RuntimeSnapshotsInFlight == 0`。

这样不需要把 worker 改为 `shared_ptr`，也能让 `PickWorker()` 不被每个 worker snapshot 的等待时间拖住。

### Worker ControlLock 解锁

生产路径只需要 `ControlLock` 保护：

- `Running` / `WorkerThreadId` 检查和 direct local fallback 判定。
- `Start()` / `Stop()` 对 fd、thread 生命周期的结构性修改。

不需要持 `ControlLock` 等待 command 完成。

新增配置和 helper：

```cpp
uint32_t ControlCommandTimeoutMs{5000};
```

`ControlCommandTimeoutMs` 只放在 `TqLinuxRelayWorkerConfig`，不新增 CLI/JSON 配置；单元测试可把它设成几十毫秒来覆盖 timeout 路径，生产默认值 5 秒。

```cpp
struct TqLinuxRelayControlState {
    bool Running{false};
    bool IsWorkerThread{false};
};

TqLinuxRelayControlState TqLinuxRelayWorker::ControlState() const;
```

`ControlState()` 短暂持 `ControlLock`，复制必要状态后释放。`RegisterRelayWithId()`、`UnregisterRelay()`、`Snapshot()` 根据该状态决定本地执行或投递 command。

command 等待统一使用 helper：

```cpp
template <typename Command>
bool WaitForCommandDone(
    Command& command,
    std::chrono::milliseconds timeout,
    std::atomic<uint64_t>& waitNanosCounter,
    std::atomic<uint64_t>& timeoutCounter);
```

第一阶段落成三个专用 helper，内部复用一个 `.cpp` 内部局部函数累计 wait 指标：

- `WaitRegisterCommand(RegisterRelayCommand&)`
- `WaitUnregisterCommand(UnregisterRelayCommand&)`
- `WaitSnapshotCommand(SnapshotCommand&)`

要求：

- wait 前不持 `ControlLock`。
- 使用 `wait_until()`。
- 成功时累计 wait nanos。
- 超时时累计 timeout counter，并返回失败/空 snapshot。
- timeout 后 command 仍必须保持有效，worker 晚到访问不会触发 use-after-return。

为避免 timeout 后 use-after-return，生产 command 需要从栈对象改为 `std::shared_ptr<...Command>`，event.Control 改为持有裸指针仍不够安全。推荐扩展 `TqLinuxRelayEvent` 增加：

```cpp
std::shared_ptr<void> ControlOwner;
```

投递时：

```cpp
auto command = std::make_shared<SnapshotCommand>();
event.Control = command.get();
event.ControlOwner = command;
```

worker event 处理结束后释放 `ControlOwner`。调用方 timeout 后丢弃自己的 `shared_ptr` 不会释放 command，避免 worker 晚到访问悬空栈对象。

### Enqueue retry 和 timeout

新增常量：

```cpp
constexpr uint32_t kLinuxRelayControlEnqueueRetries = 8;
```

生产路径语义：

- `RegisterRelayWithId()`：event queue full 时 bounded retry + `Wake()` + `yield()`；超过重试返回空 `TqLinuxRelayRegistrationResult`，记录 enqueue failure。
- `UnregisterRelay()`：event queue full 时 bounded retry；超过重试记录 timeout/failure 并返回。unregister 是 best-effort，不抛异常。
- `Snapshot()`：event queue full 或 timeout 返回当前空 snapshot，并记录 failure/timeout。admin 层可继续返回部分/空指标，排障时通过 timeout counter 诊断。

timeout 值来自 `TqLinuxRelayWorkerConfig::ControlCommandTimeoutMs`，默认 5000 ms。该字段只供代码和测试使用，不进入命令行、配置文件或 admin JSON。

### Metrics

新增到 `TqLinuxRelayWorkerSnapshot`：

```cpp
uint64_t ControlLockWaitNanos{0};
uint64_t ControlLockAcquireCount{0};
uint64_t ControlCommandWaitNanos{0};
uint64_t ControlCommandWaitCount{0};
uint64_t ControlCommandTimeouts{0};
uint64_t ControlCommandEnqueueFailures{0};
uint64_t SnapshotCommandWaitNanos{0};
uint64_t SnapshotCommandWaitCount{0};
uint64_t SnapshotCommandTimeouts{0};
```

新增到 runtime aggregate snapshot：

```cpp
uint64_t RuntimeLockWaitNanos{0};
uint64_t RuntimeLockAcquireCount{0};
uint64_t RuntimeSnapshotInFlightMax{0};
```

JSON 输出字段：

- `linux_relay_control_lock_wait_nanos`
- `linux_relay_control_lock_acquire_count`
- `linux_relay_control_command_wait_nanos`
- `linux_relay_control_command_wait_count`
- `linux_relay_control_command_timeouts`
- `linux_relay_control_command_enqueue_failures`
- `linux_relay_snapshot_command_wait_nanos`
- `linux_relay_snapshot_command_wait_count`
- `linux_relay_snapshot_command_timeouts`
- `linux_relay_runtime_lock_wait_nanos`
- `linux_relay_runtime_lock_acquire_count`
- `linux_relay_runtime_snapshot_inflight_max`

计数只用于观测，使用 `memory_order_relaxed`。

### 测试策略

单元测试覆盖：

- runtime snapshot 不持 `Runtime::Lock` 等待 worker snapshot：构造一个 worker snapshot 阻塞点或高频 snapshot，并发 `PickWorker()` 应可返回。
- worker `Snapshot()` 不持 `ControlLock` 等待 command：并发 snapshot 和 register/unregister 不应互相长时间阻塞。
- command timeout 不造成 use-after-return：投递 snapshot command 后模拟 worker 不 drain，调用方 timeout 返回，之后 worker drain 晚到不崩溃。
- register enqueue full 使用 bounded retry：小容量 event queue 下能重试成功或明确失败并增加 failure counter。
- metrics JSON 包含新增字段。
- 正常 register/unregister/snapshot 现有行为不回归。

验证命令：

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

## 风险

- timeout 后 command lifetime 是最大风险，必须先引入 `ControlOwner` 或等价 shared ownership，再启用 timeout。
- runtime worker raw pointer snapshot 需要 in-flight guard；否则 `Stop()` 清空 worker 后 snapshot 可能访问释放对象。
- 缩小 `ControlLock` 后 direct local fallback 必须只在安全状态执行，不能让非 worker 线程在 worker running 时直接改 worker-local state。
- 指标增加不能进入数据面热路径；只在控制面和 snapshot 路径更新。
