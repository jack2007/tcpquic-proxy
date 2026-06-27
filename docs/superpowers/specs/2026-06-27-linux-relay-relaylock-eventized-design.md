# Linux relay RelayLock 优化与事件化设计

## 背景

当前 Linux relay worker 使用 `RelayLock` 保护 `Relays`、`RetiredRelays` 和 `NextRelayId`。注册、注销、snapshot 以及按 relay id 查找都会持有这把 worker 级互斥锁。

在数据面上，TCP epoll 事件、QUIC receive view、QUIC send complete、QUIC abort/shutdown/ideal buffer 事件都会通过 `FindRelayById()` 找到 `RelayState`。`FindRelayById()` 当前在 `Relays` 和 `RetiredRelays` 上做线性扫描，因此每个相关事件都可能付出：

```text
mutex lock + O(active relays) scan + shared_ptr copy
```

当单 worker 承载大量 relay、QUIC 小包事件频繁、metrics snapshot 频繁或短连接注册/注销压力高时，这个设计会让 `RelayLock` 和线性扫描成为转发事件的共享热点。

## 目标

1. 阶段一：在不改变现有线程模型和同步 API 的前提下，增加 O(1) relay id 索引，去掉 `FindRelayById()` 的线性扫描。
2. 阶段二：把 register、unregister、snapshot 等会读写 relay 容器的操作事件化，让 `Relays` 和 `RelaysById` 成为 worker 线程私有状态，最终移除 `RelayLock` 对热路径的影响。
3. 保持现有 relay 生命周期语义：outstanding QUIC send、stream binding、retired relay 延迟释放、TCP fd 关闭和 `TqRelayHandle` 状态不能回退。
4. 每个阶段都必须能单独测试、单独上线，便于压测对比。

## 非目标

- 不在本设计中重写 Linux relay 的 TCP/QUIC 背压策略。
- 不改变 MsQuic callback 返回 `QUIC_STATUS_PENDING` 和 `ReceiveComplete()` 的语义。
- 不改变 `TqRelayStart()` / `TqRelayStop()` 对上层 tunnel 的同步语义；阶段二内部可通过 completion 等待 worker 完成。
- 不把 event queue 改成多 shard；如果阶段二后 event queue 成为瓶颈，再另开设计。

## 当前机制

### Relay 容器

`TqLinuxRelayWorker` 当前持有：

```cpp
mutable std::mutex RelayLock;
std::deque<std::shared_ptr<RelayState>> Relays;
std::deque<std::shared_ptr<RelayState>> RetiredRelays;
uint64_t NextRelayId{1};
```

注册时：

```text
RegisterRelayWithId()
  -> lock RelayLock
  -> relay->Id = NextRelayId++
  -> Relays.push_back(relay)
  -> unlock
  -> epoll_ctl(ADD)
  -> 安装 StreamRelayBinding
```

注销时：

```text
UnregisterRelay(relayId)
  -> lock RelayLock
  -> 线性扫描 Relays
  -> erase
  -> unlock
  -> close fd / detach binding / clear queues
  -> lock RelayLock
  -> RetiredRelays.push_back(removed)
```

查找时：

```text
FindRelayById(relayId)
  -> lock RelayLock
  -> 线性扫描 Relays
  -> 线性扫描 RetiredRelays
  -> 返回 shared_ptr
```

### 热路径入口

以下路径会频繁进入 `FindRelayById()`：

- `ProcessTcpEvents()`：每个 TCP epoll readiness。
- `ProcessQuicReceiveViewEvent()`：每个 QUIC receive view。
- `CompleteQuicSend()`：每个 QUIC send complete。
- `HandleQuicIdealSendBuffer()`、peer abort、shutdown complete 等 stream 事件。
- test helper `DispatchTcpEventsForTest()`、`FindRelayByFd()`。

因此当前 `RelayLock` 不只是连接生命周期锁，也被事件级转发路径使用。

## 总体方案

分两个阶段完成。

### 阶段一：保留锁，增加 O(1) 索引

新增 `RelaysById` 和 `RetiredRelaysById`，由现有 `RelayLock` 保护：

```cpp
#include <unordered_map>

std::deque<std::shared_ptr<RelayState>> Relays;
std::deque<std::shared_ptr<RelayState>> RetiredRelays;
std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RelaysById;
std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RetiredRelaysById;
```

查找变成：

```text
FindRelayById(relayId)
  -> lock RelayLock
  -> RelaysById.find(relayId)
  -> RetiredRelaysById.find(relayId)
  -> 返回 shared_ptr
```

注册、注销和 retired purge 同步维护索引：

```text
Register:
  Relays.push_back(relay)
  RelaysById.emplace(relay->Id, relay)

Unregister:
  removed = RelaysById[relayId]
  从 Relays erase
  RelaysById.erase(relayId)
  ...
  RetiredRelays.push_back(removed)
  RetiredRelaysById.emplace(removed->Id, removed)

PurgeRetiredRelaysIfIdle:
  删除 RetiredRelays 中 idle relay
  同步 RetiredRelaysById.erase(id)
```

阶段一不移除 `RelayLock`，只降低持锁时间和查找复杂度。这样风险较低，现有 API、测试和生命周期不需要整体重写。

### 阶段二：注册/注销/snapshot 事件化，移除 RelayLock

阶段二把 `Relays`、`RetiredRelays`、`RelaysById` 和 `RetiredRelaysById` 的读写都收敛到 worker 线程：

```text
外部线程
  -> Enqueue control event
  -> Wake worker
  -> 等待 completion 或读取异步结果

worker 线程
  -> DrainEvents()
  -> ProcessRegisterRelayEvent()
  -> ProcessUnregisterRelayEvent()
  -> ProcessSnapshotEvent()
  -> 直接读写 relay 容器，无 RelayLock
```

注册、注销、snapshot 不再直接修改 relay 容器。外部线程只负责投递命令和等待结果。

## 阶段一详细设计

### 数据结构

`Relays` 和 `RetiredRelays` 继续存在，避免大范围重写 snapshot 遍历和 retired purge 逻辑。新增 map 只负责查找。

```cpp
std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RelaysById;
std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RetiredRelaysById;
```

`RelaysById` 是权威查找索引；`Relays` 是遍历顺序容器。二者必须在同一个 `RelayLock` 临界区内更新，避免 snapshot 或 lookup 看到不一致状态。

### 注册失败回滚

当前 `RegisterRelayWithId()` 在 `epoll_ctl(ADD)` 或 binding 分配失败时，会持锁从 `Relays` erase。阶段一必须同步 erase `RelaysById`：

```text
if epoll_ctl(ADD) fails:
  erase relay from Relays
  RelaysById.erase(relay->Id)

if binding allocation fails:
  epoll_ctl(DEL)
  erase relay from Relays
  RelaysById.erase(relay->Id)
```

### retired 生命周期

`RetiredRelays` 用于延迟保存已经从 active 中移除、但可能仍有 outstanding send 或 stream binding 引用的 relay。阶段一不能把 retired relay 直接释放。

`FindRelayById()` 保持可查 retired relay，因为 `CompleteQuicSend()` 可能在注销后收到 send complete，需要通过 retired relay 扣减 outstanding 并 detach binding。

### 复杂度变化

阶段一前：

```text
FindRelayById = O(active relays + retired relays)
```

阶段一后：

```text
FindRelayById = O(1) average
```

仍然存在 `RelayLock` 竞争，但持锁时间不再随 relay 数线性增长。

## 阶段二详细设计

### 事件类型

扩展 `TqLinuxRelayEventType`：

```cpp
RegisterRelay,
UnregisterRelay,
Snapshot,
```

`Shutdown` 当前已用于 `UnregisterRelay(event.RelayId)`。阶段二应避免混淆 worker stop 和 relay unregister，建议：

```cpp
StopWorker,
UnregisterRelay,
```

若为了减少改动保留 `Shutdown`，必须在文档和代码中明确它表示 relay unregister event，而不是 worker stop。

### control event payload

现有 `TqLinuxRelayEvent` 主要承载数据面事件。阶段二需要承载注册请求和 snapshot 请求。为了避免把 `TqLinuxRelayEvent` 塞得过大，可以新增小型 control command：

```cpp
struct TqLinuxRelayRegisterCommand {
    TqLinuxRelayRegistration Registration;
    TqLinuxRelayRegistrationResult Result;
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct TqLinuxRelaySnapshotCommand {
    TqLinuxRelayWorkerSnapshot Result;
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};
```

`TqLinuxRelayEvent` 新增：

```cpp
void* Control{nullptr};
```

注册同步 API 保持：

```text
RegisterRelayWithId(registration)
  -> 创建 TqLinuxRelayRegisterCommand
  -> event.Type = RegisterRelay
  -> event.Control = &command
  -> Enqueue + Wake
  -> 等待 command.Done
  -> return command.Result
```

worker 处理：

```text
ProcessRegisterRelayCommand(command)
  -> 在 worker 线程创建 RelayState
  -> 分配 id
  -> 插入 Relays / RelaysById
  -> epoll_ctl(ADD)
  -> 安装 binding
  -> 写 Result
  -> signal Done
```

### 为什么这能移除 RelayLock

阶段二后，以下操作全部在 worker 线程内执行：

- `Relays.push_back`
- `RelaysById.emplace`
- active 到 retired 的移动
- retired purge
- snapshot 遍历
- TCP/QUIC event lookup

外部线程不再读写 relay 容器，只通过 event queue 请求 worker 执行。因此 relay 容器不再是多线程共享可变状态，`RelayLock` 可以移除。

`StreamRelayBinding` 仍然需要 atomic，因为 MsQuic callback 仍可能从 MsQuic 线程进入；event queue 和 completion 仍然是跨线程同步边界。

### 防止 deadlock

阶段二必须避免以下死锁：

1. worker 线程调用同步 `RegisterRelayWithId()` 后等待自己处理事件。
2. worker 持有 completion mutex 时执行可能回调外部的操作。
3. `Stop()` 清理 worker 时仍有外部线程等待 register/unregister completion。

规则：

- 如果 `RegisterRelayWithId()` / `UnregisterRelay()` 在 worker 线程内被调用，直接执行内部 helper，不走 enqueue+wait。
- completion 只在写结果和 `notify_one()` 时持有自己的 mutex。
- worker stop 时 drain 或 fail 所有未完成 control command，避免调用方永久等待。

### Snapshot 事件化

当前 `Snapshot()` 直接持 `RelayLock` 遍历 worker relay。阶段二改为同步 control event：

```text
Snapshot()
  -> 如果当前是 worker 线程，直接 BuildSnapshotLocal()
  -> 否则 enqueue Snapshot event
  -> 等待 Result
```

`TqLinuxRelayRuntime::Snapshot()` 仍可逐个 worker 调用 `worker->Snapshot()`。后续若 snapshot 影响转发延迟，可再改为 async cached snapshot。

### 查找接口

阶段二中保留接口名，但语义改为 worker-local：

```cpp
RelayState* FindRelayByIdLocal(uint64_t relayId);
std::shared_ptr<RelayState> FindRelayByIdForExternal(uint64_t relayId); // 仅测试或过渡期
```

数据面路径使用裸指针或 `shared_ptr` 均可。保守第一版建议仍让 map 保存 `shared_ptr` 并返回 `shared_ptr`，因为它能维持现有 retired send complete 生命周期。等阶段二稳定后再评估 slot/generation 和裸指针。

## 线程与生命周期

阶段二后，worker 拥有 relay 容器；外部线程拥有 control command 的栈对象或 `shared_ptr`。事件处理时必须保证 command 生命周期覆盖 worker 访问：

```text
同步 command:
  caller 栈上创建 command
  enqueue pointer
  caller wait Done
  worker signal Done 后不再访问 command
```

如果后续需要异步 unregister，可以改成 `std::shared_ptr<TqLinuxRelayControlCommand>` 放入事件，避免 caller 生命周期问题。

## 测试策略

阶段一测试重点：

- 注册多个 relay 后，snapshot active 数正确。
- 通过 `DispatchTcpEventsForTest()`、QUIC receive、send complete 能按 id 命中正确 relay。
- unregister 后 active map 删除，late send complete 仍能在 retired map 找到 relay。
- 注册失败回滚时 `RelaysById` 不残留。
- 并发注册/注销压力测试保持通过。

阶段二测试重点：

- `RegisterRelayWithId()` 返回前 relay 已加入 worker，epoll 和 stream callback 已安装。
- `UnregisterRelay()` 返回前 fd 已关闭、active relay 已移除。
- snapshot 通过 event 返回一致数据。
- 从 worker 线程内部触发 unregister 不死锁。
- worker stop 时没有未完成 completion 卡死。
- 现有 IO 转发测试全部通过。

## 风险和缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| map 和 deque 不一致 | 查找命中已删除 relay 或漏查 active relay | 所有增删在同一临界区；阶段一增加一致性断言测试 |
| retired map 忘记清理 | 内存泄漏，late event 命中过期 relay | `PurgeRetiredRelaysIfIdle()` 同步 erase map；测试 late completion 后 retired 回收 |
| 阶段二 completion 死锁 | `TqRelayStart()` 或 `TqRelayStop()` 卡住 | worker 线程内调用走 direct helper；stop fail pending commands |
| 注册事件化改变 API 时序 | 上层拿到 handle 但 relay 尚未可用 | `RegisterRelayWithId()` 必须等 worker 完成注册后才返回 |
| snapshot 事件阻塞 worker | metrics 请求影响转发延迟 | 初版接受同步 snapshot；后续可做 cached snapshot 或限频 |
| 事件队列满导致 control event 投递失败 | register/unregister 失败或 stop 不及时 | control event 入队失败时同步返回失败；unregister 失败必须保守设置 handle stop 并记录错误 |

## 上线顺序

1. 合入阶段一，运行单元测试和短连接/多 relay 压测，观察 `StreamLookupScanCount`、event latency、CPU profile。
2. 阶段一稳定后合入阶段二，重点压测 register/unregister/snapshot 并发和 worker stop。
3. 若阶段二仍有热点，再设计 slot/generation、epoll ptr 或 event queue shard。

## 验收标准

- 阶段一后，`FindRelayById()` 不再扫描 `Relays` / `RetiredRelays`。
- 阶段一后，所有现有 Linux relay queue/io 测试通过。
- 阶段二后，relay 容器只在 worker 线程读写，`RelayLock` 从 `TqLinuxRelayWorker` 中移除。
- 阶段二后，`TqRelayStart()`、`TqRelayStop()`、metrics snapshot 语义保持同步且可观测结果不变。
- 两阶段均不得引入 relay 泄漏、late send complete use-after-free、stream callback use-after-free。
