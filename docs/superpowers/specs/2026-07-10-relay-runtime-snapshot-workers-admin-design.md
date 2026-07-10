# Relay Runtime SnapshotWorkers 三平台 Admin 上报设计

## 背景

Admin `/api/v1/relay/workers` 与 client admin-console 的 Workers 表依赖
`TqSnapshotRelayWorkers()`。

当前行为：

1. 始终返回一行跨平台聚合项：`worker_id=aggregate`。
2. 仅在 Linux 下追加 per-worker 行：`linux-0` … `linux-N`。
3. Darwin / Windows 即使已按 CPU 数启动多个 worker，admin 也只看到 `aggregate`，
   容易误判为“只有 1 个 worker”。

平台现状差异：

| 平台 | Runtime | Per-worker Snapshot API | Aggregate Snapshot 锁策略 |
|------|---------|-------------------------|---------------------------|
| Linux | `TqLinuxRelayRuntime` | 已有 `SnapshotWorkers()` | Acquire 复制指针 → 释放 runtime 锁 → 再 `worker->Snapshot()`；`Stop()` 等 in-flight |
| Darwin | `TqDarwinRelayRuntime` | 无 | 持 `Mutex` 调用可阻塞的 `worker->Snapshot()`（与 Linux 已修问题同类） |
| Windows | `TqWindowsRelayRuntime` | 无 | 已复制指针后在 `Lock_` 外 Snapshot，但用粗粒度 `RuntimeWorkerLifetimeLock()` 保活 |

目标是让三平台在 admin 上都能看到真实 worker 列表，并把 snapshot 期间的 worker
指针保活语义统一到同一 helper。

## 目标

1. 抽出公共 `TqRelayRuntimeSnapshotSupport` helper（组合，非完整 Runtime 基类），统一：
   - `AcquireWorkers`
   - `ReleaseWorkers`
   - `WaitForIdle`（`Stop()` 清空 Workers 前）
2. Linux / Darwin / Windows Runtime 都提供 `SnapshotWorkers()`，并用 helper 保护
   aggregate `Snapshot()` 与 `Stop()`。
3. `TqSnapshotRelayWorkers()` 在对应平台追加：
   - `linux-N`
   - `darwin-N`
   - `windows-N`
4. 保留首行 `aggregate`；`per_worker_active_relays` 仍为 `false`。
5. 更新 admin API / 平台 relay 文档与必要单测。

## 非目标

- 不合并三平台 `Start` / `RegisterRelay` / `PickWorker` 业务逻辑。
- 不引入完整 `TqRelayRuntimeBase` 拥有 `Workers` 的继承体系。
- 不实现 per-worker active-relays 明细（`relays` 数组继续为空）。
- 不改 admin-console UI 列定义（现有表格已能渲染新行）。
- 不改变数据面转发语义与 worker 数量默认策略（仍由 tuning / CPU 检测决定）。

## 已确认决策

1. 抽象形态：**B1 helper/mixin**（`TqRelayRuntimeSnapshotSupport`），三平台 Runtime
   **组合**使用。
2. `worker_id` 前缀固定为：`linux-` / `darwin-` / `windows-`。
3. 范围覆盖 **Linux + macOS + Windows** 三平台适配。

## 设计方案

### 1. 公共 Snapshot 生命周期 helper

新增：

- `src/tunnel/relay_runtime_snapshot.h`
- 如实现需要，可配 `src/tunnel/relay_runtime_snapshot.cpp`

建议 API 形态（示意）：

```cpp
class TqRelayRuntimeSnapshotSupport {
public:
    template <typename WorkerT>
    std::vector<WorkerT*> AcquireWorkers(
        std::mutex& runtimeLock,
        const std::vector<std::unique_ptr<WorkerT>>& workers) const;

    void ReleaseWorkers() const;
    void WaitForIdle() const;

    uint64_t InFlightMax() const;

private:
    mutable std::mutex SnapshotLock;
    mutable std::condition_variable SnapshotCv;
    mutable uint32_t SnapshotsInFlight{0};
    mutable std::atomic<uint64_t> SnapshotInFlightMax{0};
};
```

语义（对齐现有 Linux）：

1. `AcquireWorkers`：
   - 持 `runtimeLock` 复制非空 `worker.get()` 到 `vector<WorkerT*>`
   - 在 `SnapshotLock` 下 `++SnapshotsInFlight`，更新 `SnapshotInFlightMax`
   - 释放 `runtimeLock` 后返回指针列表
2. 调用方在锁外对每个指针调用 `worker->Snapshot()`（允许阻塞等待 worker 线程）
3. `ReleaseWorkers`：`SnapshotsInFlight--`；降到 0 时 `notify_all`
4. `WaitForIdle`：在 `Stop()` 清空 / 销毁 Workers 前等待 `SnapshotsInFlight == 0`

RAII：各 Runtime 可用本地 `SnapshotReleaseGuard` 调用 `ReleaseWorkers()`，与 Linux
现状一致。

### 2. 三平台 Runtime 改造

#### Linux

- 将现有 `AcquireSnapshotWorkers` / `ReleaseSnapshotWorkers` /
  `RuntimeSnapshotsInFlight` / `SnapshotLock` / `SnapshotCv` 迁到 helper 成员。
- 保留公开 `SnapshotWorkers()`；内部改走 helper。
- `Snapshot()` / `Stop()` 行为保持不变（只换实现载体）。
- 现有 Linux metrics 字段（如 `RuntimeSnapshotInFlightMax`）继续从 helper 读取。

#### Darwin

- `TqDarwinRelayRuntime` 嵌入 helper。
- 新增：

```cpp
std::vector<TqDarwinRelayWorkerSnapshot> SnapshotWorkers() const;
```

- `Snapshot()`：改为 Acquire → 锁外逐 worker Snapshot → 再聚合（修复当前持
  `Mutex` 等待 worker 的问题）。
- `Stop()`：在清空 `Workers` 前 `WaitForIdle()`。
- `TqDarwinRelayWorkerSnapshot` 增加：
  - `uint32_t WorkerIndex`
  - `uint64_t EventQueueCapacity`（来自 `Config.EventQueueCapacity`）
- `SnapshotLocal()` 填入上述字段。

#### Windows

- `TqWindowsRelayRuntime` 嵌入 helper。
- 新增 `SnapshotWorkers()`。
- `Snapshot()` / `Stop()` 改用 helper；删除
  `RuntimeWorkerLifetimeLock()`（当前仅 Snapshot/Stop 使用）。
- `TqWindowsRelayWorkerSnapshot` 增加 `uint32_t WorkerIndex`；
  `BuildSnapshotLocal()` 写入 `WorkerIndex_`。
- Admin 映射所需字节字段：
  - `TcpWriteBytes` ← `TcpSendBytes`（或显式别名填充）
  - `TcpReadBytes` ← 从 `ActiveRelayStates` 求和，或在
    `BuildSnapshotLocal()` 增加累计字段
  - `PendingBytes` ← `PendingQuicReceiveBytes + RelayBufferBytesInUse`
    （与现有 aggregate metrics 口径一致）

### 3. Metrics / Admin 接线

修改 `src/runtime/relay_metrics.cpp::TqSnapshotRelayWorkers()`：

```text
workers = [aggregate]
#if __linux__
  append ConvertLinux(...) for SnapshotWorkers()
#elif __APPLE__
  append ConvertDarwin(...) for SnapshotWorkers()
#elif _WIN32
  append ConvertWindows(...) for SnapshotWorkers()
#endif
```

转换约定：

| 字段 | Linux | Darwin | Windows |
|------|-------|--------|---------|
| `Backend` | `"linux"` | `"darwin"` | `"windows"` |
| `WorkerId` | `linux-{index}` | `darwin-{index}` | `windows-{index}` |
| `WorkerIndex` | snapshot | snapshot | snapshot |
| `ActiveRelays` | snapshot | snapshot | snapshot |
| `PendingBytes` | snapshot | snapshot | 见上节映射 |
| `TcpReadBytes` / `TcpWriteBytes` | snapshot | snapshot | 见上节映射 |
| `Errors` | snapshot | snapshot | snapshot |
| `EventQueueCapacity` | snapshot | snapshot | `0`（Windows 无等价 ring capacity 时填 0） |

`TqRelayWorkerDetailJson` 无需改路由：按 `worker_id` 查找即可命中
`darwin-N` / `windows-N`；`relays` 仍为空数组。

Capabilities：

- `worker_detail: true` 保持
- `per_worker_active_relays: false` 保持

### 4. CMake / 源文件接线

- 将 `relay_runtime_snapshot` 源文件加入三平台会编译到的目标
  （`tcpquic-proxy` 与相关 unit test targets）。
- Header-only 模板实现优先放在 `.h`，避免三平台重复链接问题；非模板部分可放
  `.cpp`。

### 5. 测试

最低覆盖：

1. **Darwin**（`darwin_relay_worker_io_test`）：
   - `Start` 多 worker → `SnapshotWorkers().size() == N`
   - 每项 `WorkerIndex` / 后续 metrics `darwin-N` 可转换
   - 并发 Snapshot 期间 `Stop()` 等待 in-flight（轻量测即可）
2. **Windows**（现有 windows relay test，若有 runtime 级用例则扩展；否则新增最小
   runtime snapshot 测）：
   - 同上 `SnapshotWorkers` size/index
3. **Linux**：
   - 现有 `SnapshotWorkers` 测保持通过；确认迁到 helper 后行为不变
4. **Metrics 层**（可平台条件编译）：
   - `TqSnapshotRelayWorkers()` 含 `aggregate` + 平台前缀行

### 6. 文档

更新：

- `docs/admin-api/interface.md`：`/relay/workers` 说明改为
  `aggregate` + 平台 `linux-N` / `darwin-N` / `windows-N`
- `docs/relay_macos.md`：补充 admin worker 列表与 snapshot 锁语义
- `docs/relay_linux.md`：注明 Acquire/Release 已抽到公共 helper（行为不变）
- `docs/relay_win.md`：补充 `SnapshotWorkers` 与去掉
  `RuntimeWorkerLifetimeLock` 的原因

## 风险与约束

1. **Stop 与 Snapshot 并发**：三平台都必须在销毁 Workers 前 `WaitForIdle`，否则
   helper 返回的裸指针会 UAF。
2. **Windows 字段口径**：worker 级 `TcpReadBytes` / `PendingBytes` 需与现有
   aggregate metrics 一致，避免 admin 行与总览卡数字对不上。
3. **Linux 回归**：helper 抽取必须保持现有 in-flight / Stop 等待语义，不能削弱
   已修复的 control-plane wait 设计。
4. **持锁顺序**：`AcquireWorkers` 内先 runtime lock 再 SnapshotLock；`Stop` 侧
   WaitForIdle 只拿 SnapshotLock。禁止在持 SnapshotLock 时再抢 runtime lock。

## 验收标准

1. macOS client admin-console Workers 表在 runtime 启动后显示
   `aggregate` + `darwin-0` … `darwin-(N-1)`，`N ==` 启动 worker 数。
2. Linux / Windows 同样分别显示 `linux-*` / `windows-*`。
3. Darwin / Windows aggregate `Snapshot()` 不再在持 runtime workers 锁时调用可阻塞
   的 `worker->Snapshot()`。
4. 相关单测通过；文档与实现一致。

## 实现顺序建议

1. 落地 `TqRelayRuntimeSnapshotSupport` + Linux 迁移（行为不变的重构）。
2. Darwin：`SnapshotWorkers` + Snapshot/Stop 接入 + snapshot 字段补齐。
3. Windows：`SnapshotWorkers` + 去掉 lifetime lock + 字段映射。
4. `relay_metrics.cpp` 三平台 Convert / 接线。
5. 单测 + 文档。
