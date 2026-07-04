# Darwin send completion 锁出清设计

## 背景

`docs/relay_macos.md` 中排名 1、2 的 `RelayState::Mutex` 和 `TqDarwinRelayWorker::RelayMutex` 已经从稳定转发热路径中移走。剩余最热的同步点集中在 TCP->QUIC send operation 生命周期：

- `KnownSendMutex`：worker 级全局 known operation map，覆盖注册、提交标记、完成注销、stop drain 和测试辅助。
- `CompletionState::Mutex`：stream binding 级 known operation map，覆盖 SEND_COMPLETE claim、detached completion、late completion 和 retired stream cleanup 判定。

这两把锁属于同一条 send completion ownership 链路。若只移除 `KnownSendMutex`，callback 仍会进入 `CompletionState::Mutex` 高频查找；若只改 `CompletionState::Mutex`，worker 正常路径仍会被 worker 级 map 锁串行化。因此本阶段应合并处理排名 3、4 的 send completion 锁热点，但不同时处理 receive callback 失败语义、fake FIN abort、metrics 命名或 event queue wake 策略。

## 目标

1. active worker TCP->QUIC send 注册、提交、完成路径不再持有 `KnownSendMutex`。
2. 正常 SEND_COMPLETE callback 不再通过 `CompletionState::Mutex` 查 per-binding unordered_map。
3. 保留同步 SEND_COMPLETE、异步 SEND_COMPLETE、late completion、worker stop 后 detached completion 的可判定性。
4. 保证每个 `TqDarwinRelaySendOperation` 只完成一次、释放一次，且 in-flight accounting、FIN completion、pending send retry 语义不变。
5. fallback/retired/detached completion 可以继续使用同步结构，但必须从 active worker 主路径移走。
6. 增加测试指标证明 active worker send completion 不触发 `KnownSendMutex` / `CompletionState::Mutex`。

## 非目标

- 不重写 `RelayState::Mutex` / `RelayMutex` 边界；上一阶段已经完成热路径出清。
- 不改变 MsQuic `Stream::Send()` 调用时机、FIN 提交语义、backpressure 阈值或 pending send retry 策略。
- 不处理 QUIC receive callback 队列满返回 `QUIC_STATUS_SUCCESS` 的语义。
- 不处理 fake FIN `std::abort()`。
- 不新增平台中性 metrics 字段。
- 不改 Linux/Windows relay。

## 当前 send operation 生命周期

当前 send operation 正常路径大致如下：

1. worker `TrySubmitQuicSendOperation()` 创建 `TqDarwinRelaySendOperation`。
2. worker 调用 `RegisterKnownSendOperationLocal()`，但该 local 版本仍持有 `KnownSendMutex`。
3. `RegisterKnownSendOperationLocal()` 同时把 operation 放入 worker `KnownSendOperations` 和 binding `CompletionState::KnownSendOperations`。
4. `Stream::Send()` 可能同步完成，也可能之后由 MsQuic callback 完成。
5. `MarkKnownSendOperationSubmittedLocal()` 标记 submitted，仍持有 `KnownSendMutex` 和 `CompletionState::Mutex`。
6. SEND_COMPLETE callback 调用 `TryClaimKnownSendCompletionEvent()`，在 `CompletionState::Mutex` 下查 operation 并设置 `CompletionEventClaimed`。
7. callback 若能入队，则 worker event 调用 `CompleteQuicSend()`；否则 fallback 直接调用 `CompleteQuicSend()`。
8. `CompleteQuicSend()` 在 worker 路径调用 `UnregisterKnownSendOperationLocal()`，仍持有 `KnownSendMutex`，并从 `CompletionState` 中删除 operation。

瓶颈来自两个 map 同时承担“正常 worker bookkeeping”和“callback/retired fallback 判定”。本设计把这两类责任拆开。

## 方案选择

推荐方案：operation 内状态机 + worker-local active set + fallback registry。

- active worker 路径使用 worker-only active set，不持有 `KnownSendMutex`。
- `TqDarwinRelaySendOperation` 自带原子状态位和必要 metadata，callback 可直接 claim operation，不必查 `CompletionState` map。
- `CompletionState` 保留 binding lifetime、known operation 计数和 retired cleanup 判定，但不再用 unordered_map 作为正常 completion claim 的入口。
- fallback registry 只覆盖 worker 未运行、operation 已 detached、callback 无法投递、retired stream cleanup 等边界。

不推荐完全删除 fallback 同步结构。MsQuic callback 可以在 worker stop、stream retired 或 synchronous completion 竞态中迟到；没有 fallback 判定会放大 double delete、operation 泄漏和 retired binding 提前释放风险。

也不推荐把 operation ownership 改成 `shared_ptr`。send operation 当前由 MsQuic client context 裸指针传回，强行 shared ownership 会引入更复杂的生命周期图，并不解决 callback claim 的核心问题。

## 设计

### Operation 状态机

给 `TqDarwinRelaySendOperation` 增加原子状态和 claim helpers。状态需要覆盖以下事实，而不是表示严格线性阶段：

```cpp
enum class TqDarwinSendOperationState : uint32_t {
    Created = 0,
    Registered = 1,
    Submitted = 2,
    CompletionClaimed = 3,
    Completed = 4,
    Detached = 5,
};
```

推荐使用 `std::atomic<uint32_t> State`，并提供 helper：

```cpp
bool TryMarkRegistered();
bool TryMarkSubmitted();
bool TryClaimCompletion();
bool TryMarkCompleted();
bool MarkDetached();
bool IsSubmitted() const;
bool IsCompletionClaimed() const;
```

关键规则：

- 只有首次 `TryClaimCompletion()` 成功的线程拥有 completion event 处理权。
- 只有首次 `TryMarkCompleted()` 成功的线程可以删除 operation。
- synchronous SEND_COMPLETE 可以在 `Submitted` 前发生；状态机必须允许 callback 先 claim，worker 后提交时识别 completion 已 claim。
- `Detached` 表示 operation 已从 active worker ownership 脱离，只允许 fallback cleanup。

### Worker-local active set

把 worker 正常路径的 operation 存储从 `KnownSendOperations` 拆出：

```cpp
std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> ActiveSendOperations;
```

访问规则：

- 只能在 worker 线程访问。
- `RegisterKnownSendOperationLocal()` 改为 worker-only，无 `KnownSendMutex`。
- `MarkKnownSendOperationSubmittedLocal()` 改为 worker-only，无 `KnownSendMutex`。
- `UnregisterKnownSendOperationLocal()` 改为 worker-only，无 `KnownSendMutex`。
- 非 worker 路径不得访问 `ActiveSendOperations`。

为了避免大规模重命名，现有 public/fallback `RegisterKnownSendOperation()`、`MarkKnownSendOperationSubmitted()`、`UnregisterKnownSendOperation()` 可保留为 fallback registry API。

### CompletionState 的新职责

`CompletionState` 不再在正常 SEND_COMPLETE callback 中提供 map 查找。它保留：

- `KnownSendOperationCount`：retired binding 是否还能释放的粗粒度计数。
- `DetachedOperations` 或 fallback map：仅用于 worker stop 后、operation detached 后、callback 无法投递时的 cleanup 判定。
- `Mutex`：只保护 fallback registry，不进入 active worker completion 主路径。

注册 active operation 时递增 `KnownSendOperationCount`；operation 最终完成或 detached cleanup 时递减。计数必须在所有路径 exactly once。

### Callback claim

SEND_COMPLETE callback 不再在 `CompletionState::Mutex` 下查 map。推荐流程：

1. 从 `event->SEND_COMPLETE.ClientContext` 取 operation。
2. 校验 operation magic。
3. 调用 `operation->TryClaimCompletion()`。
4. 若 claim 失败，说明同步/迟到 completion 已被处理，返回 success 并记录异常计数。
5. 从 operation metadata 读取 `RelayId`、`BindingOwner`、`TotalBytes`、`Fin`。
6. 如果 worker running 且可入队，投递 `QuicSendComplete` event。
7. 如果不能入队，走现有 queue-full retry/worker-exit 语义；不得在 worker 仍可能运行时 inline 完成。
8. worker 已退出或 operation detached 时，走 fallback completion。

为了支持第 5 步，`TqDarwinRelaySendOperation` 需要保存当前 `KnownSendOperationInfo` 中 callback 需要的字段，或者保存一个只读 `Info` 成员。该 metadata 在注册后不可变。

### Worker completion

worker event 收到 `QuicSendComplete` 后调用 `CompleteQuicSend()`：

- worker 线程先从 `ActiveSendOperations` 删除 operation，取出 info。
- 调用 `operation->TryMarkCompleted()`。
- 若已经 completed，记录错误并返回，不 double delete。
- 更新 relay in-flight counters、FIN 状态、backpressure 和 pending send retry。
- 删除 operation。
- 递减 binding `KnownSendOperationCount`。

active worker completion 不调用 `KnownSendMutex`，不调用 `CompletionState::Mutex`。

### Fallback / detached completion

保留 fallback 路径，但边界必须清晰：

- worker 未运行或 worker thread 已退出后，callback 可以 inline fallback completion。
- operation 已从 worker active set 移入 fallback registry 后，callback 使用 fallback registry 完成 cleanup。
- retired relay 或 binding fallback 可以持有 `CompletionState::Mutex` 和必要 lifecycle 锁。
- fallback completion 只做 accounting/cleanup，不重新提交 pending sends，不恢复 live TCP read interest。

### Stop / retire / purge

worker stop 时：

- active worker set 中未完成 operation 进入 detached/fallback registry，或者等待 known operation drain。
- 已 claim 但未被 worker event drain 的 completion 必须可由 stop purge 或 callback fallback 完成。
- retired binding cleanup 仍以 `KnownSendOperationCount == 0` 为释放条件。

这一部分必须保留现有 “worker 可能停止但 MsQuic callback 迟到” 的安全性。

## 测试策略

### Characterization 和计数测试

在 `TCPQUIC_TESTING` 下增加计数器：

- `KnownSendLockedCountForTest()`
- `CompletionStateLockedCountForTest()`
- `ActiveSendLocalRegisterCountForTest()`
- `ActiveSendLocalCompleteCountForTest()`
- `FallbackSendCompletionCountForTest()`

先写 characterization tests，证明当前 active worker send path 会触发锁计数；最终改为断言 active worker path 不再触发锁计数。

### 必须覆盖的功能场景

1. active worker async SEND_COMPLETE：
   - TCP read -> Stream::Send success -> callback 入队 -> worker complete。
   - 断言 operation 删除、known count 归零、in-flight bytes 归零、无 fallback。
   - 断言 `KnownSendLockedCountForTest()` / `CompletionStateLockedCountForTest()` 不增加。

2. synchronous SEND_COMPLETE：
   - fake `Stream::Send()` 在 submitted 标记前触发 callback。
   - worker 后续 submitted 逻辑识别 completion 已 claim。
   - 不 double complete，不泄漏 operation。

3. callback enqueue full while worker still running：
   - callback 不能 inline 完成。
   - worker drain 后 completion event 被处理。
   - pending send retry 语义保持。

4. running=false but worker thread not exited：
   - callback 等待 worker exit 前不 inline。
   - worker exit 后 fallback 只做 accounting cleanup。

5. worker stopped / detached late completion：
   - operation 不在 active set。
   - fallback registry 完成 cleanup。
   - retired binding 在 count 归零后可释放。

6. corrupt magic / unknown operation：
   - 记录错误，不 crash，不 double delete。

### 系统 QA 验证

功能目标：

- TCP->QUIC bytes 在正常、backpressure、stop/late callback 下不丢 accounting。
- FIN send completion 只标记一次。
- worker stop 后没有 operation 泄漏或 double free。

性能目标：

- active worker SEND_COMPLETE 主路径不进入 `KnownSendMutex` / `CompletionState::Mutex`。
- 小包高频 send completion 下锁计数保持 0，fallback 计数只在故障/stop 场景增长。

异常和恢复：

- event queue full：callback retry 或等待 worker，不 inline 破坏 worker ownership。
- worker stop：late callback 由 fallback cleanup 接管。
- retired relay：retired storage 只在 operation count 归零后释放。

macOS 必跑：

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Linux 开发机 sanity：

```bash
rtk cmake --build build --target tcpquic_relay_buffer_test tcpquic_relay_alloc_test -j$(nproc)
rtk proxy ./build/bin/Release/tcpquic_relay_buffer_test
rtk proxy ./build/bin/Release/tcpquic_relay_alloc_test
```

## 风险

- synchronous SEND_COMPLETE 是最大风险点；必须用状态机明确 callback 先到和 worker submitted 后到的顺序。
- operation metadata 从 map 迁到 operation 本体后，必须保证注册后不可变，避免 callback 读到半初始化状态。
- `KnownSendOperationCount` 递增/递减必须 exactly once，否则 retired binding 可能泄漏或提前释放。
- fallback registry 仍保留锁，必须确保 active worker 路径不会误入 fallback。
- 测试环境 Linux 无法运行 Darwin kqueue 测试；实际执行阶段必须在 macOS runner 或本机 macOS 上跑 Darwin IO tests。

## 验收标准

1. active worker TCP->QUIC send register/submit/complete 路径不再持有 `KnownSendMutex`。
2. 正常 SEND_COMPLETE callback claim 不再持有 `CompletionState::Mutex`。
3. queue-full、running=false、worker-exit、late completion、retired relay cleanup 的现有行为不回退。
4. `docs/relay_macos.md` 更新 rank 3/4 状态，明确剩余 fallback 锁边界。
5. Darwin IO tests 覆盖上述场景并通过；Linux sanity tests 通过。
