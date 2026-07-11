# Relay Runtime SnapshotWorkers Cross-Platform Admin Implementation Plan

> **For agentic workers:** 按任务顺序执行，并使用 checkbox（`- [ ]`）跟踪进度。每个任务先补失败测试，再做最小实现，最后运行该任务列出的回归命令；不要把三平台改造堆到最后一次验证。

**Goal:** 按 `docs/superpowers/specs/2026-07-10-relay-runtime-snapshot-workers-admin-design.md`
实现 Linux、macOS、Windows 统一的 runtime snapshot lease、bounded snapshot 与真实 per-worker
Admin 上报，使 `/api/v1/relay/workers` 单次采样即可返回 `aggregate + platform-N`，并对 timeout、
partial、Stop 并发和恢复提供可测试、可观测的契约。

**Architecture:** 新增 header-only `TqRelayRuntimeSnapshotSupport`、move-only lease、平台 snapshot
result 和可跨线程终结的 `TqRelayRuntimeSnapshotExecutionGate`。三平台 Runtime 在同一 runtime
lock 下线性化 Start/Stop/acquire；Linux 保留并发 snapshot，Darwin/Windows 用 execution permit
限制 outstanding command。worker snapshot 共用一次请求的绝对 deadline，异步 command 使用共享
所有权；metrics 层对同一批 per-worker 结果做 aggregate reduce，再把 complete/identity 状态传到
Admin API 与 console。

**Tech Stack:** C++17、`std::mutex` / `condition_variable` / `atomic` / `shared_ptr`、Linux epoll、
Darwin kqueue、Windows IOCP、nlohmann/json、CMake、GitHub Actions、k6、Python 3 recovery harness。

**Source Design:** `docs/superpowers/specs/2026-07-10-relay-runtime-snapshot-workers-admin-design.md`

---

## 实施规则与不变量

- C++17 没有 `std::latch`；确定性并发测试使用 mutex + condition variable/test hook，不使用固定
  sleep 判断竞态。
- `Stop()` 必须持续持有 runtime lock，按 `WaitForIdle -> worker Stop/reset -> Workers.clear`
  顺序执行；不得在 lease 未归零时销毁 worker。
- Start 在 `Running` 时保持幂等；从 `Stopped` 启动时在局部 staged vector 构建完整一代，失败
  回滚后仍满足 `Workers.empty()`。
- Snapshot 总 deadline 默认 5 秒，覆盖 execution permit、runtime lock 和所有 worker command；
  不允许每个 worker 各等待 5 秒。
- Darwin/Windows timeout 后 request 可以 detach，但 shared command 必须持有 execution permit，
  直到 Completed/Cancelled；outstanding late command 始终不超过 1。
- Windows worker/Admin 的 TCP read/write 保持 active-only 语义，不改用累计 `TcpSendBytes`。
- **relay worker index 是端到端 identity，不只是 workers snapshot 字段。**每个平台在
  `PickWorker()` 选中 worker 后，必须把稳定 slot 写入 `TqRelayHandle`，并原样传递到 tunnel
  registry metadata、trace `relay_started`/`relay_stopping` 和 `/tunnels` JSON。不得从 worker
  指针重新推断，也不得让 `WorkerIndex{0}` 的默认值作为非 aggregate relay 的回退值。
  Linux 现场曾出现 workers API 正确显示 8 个 worker、active relays 实际分散，但所有 tunnel
  因 handle/registry 未赋值而显示 `worker_index:0`；Darwin 和 Windows 必须在实现前防止同类
  control-plane observability 假象。
- helper 必须 header-only；若实现者改成 `.cpp`，必须先审计所有直接编译平台 worker 源文件的
  executable target，不能只接主程序。
- 本计划不扩大 Stop 对 `PickWorker()` 返回裸指针或 relay handle 的保护范围；上层 shutdown
  quiesce 前提保持不变。
- 工作区可能已有无关改动；实现时只修改本计划列出的文件，不覆盖或清理其他变更。

## File Structure

### New files

- Add: `src/tunnel/relay_runtime_snapshot.h`
  - 定义 runtime state、deadline、worker ref、move-only lease、snapshot result、stats、lifetime
    support 与 execution gate/permit。
- Add: `src/unittest/relay_runtime_snapshot_test.cpp`
  - 平台无关 fake worker 测试：lease、Stop barrier、异常展开、permit 跨线程完成和 late command
    上界。
- Add: `tests/k6/relay_workers.js`
  - Admin workers list/detail 的 baseline、peak、spike、soak 与 breaking-point 场景。
- Add: `tests/scripts/run_relay_runtime_snapshot_recovery.py`
  - 参数化 start/stop/kill/freeze 命令，验证 T0+60s graceful Stop 与 T0+90s health/CONNECT 恢复。
- Add: `tests/scripts/test_run_relay_runtime_snapshot_recovery.py`
  - 使用 fake command/health server 验证 recovery harness 的成功、超时和命令失败状态机。
- Add: `.github/workflows/relay-runtime-snapshot-nightly.yml`
  - sanitizer、100 轮 Start/Stop/Snapshot 与 30 分钟 soak。

### Modified source and tests

- Modify: `src/CMakeLists.txt`
  - 添加公共 helper test target，并纳入 `TCPQUIC_TEST_TARGETS`。
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/relay.h`
- Modify: `src/tunnel/relay.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/unittest/relay_backend_selection_test.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`
  - Linux helper 迁移、deadline、staged Start、Stop barrier、identity 与 legacy metrics 回归。
- Modify: `src/tunnel/darwin_relay_event_queue.h`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - Darwin shared command、真实 queue capacity、execution gate、runtime state 和故障测试。
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - Windows shared IOCP command、active-only 字段、execution gate、runtime state 和故障测试。
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/darwin_relay_metrics_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
  - neutral stats、single-source aggregate、三平台转换和 complete/partial JSON。
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/unittest/server_admin_test.cpp`
  - worker detail 的 Ok/NotFound/SnapshotUnavailable 三态路由。
- Modify: `src/runtime/admin_console.cpp`
- Modify: `src/unittest/admin_http_test.cpp`
  - Relay 页单请求和 incomplete 可见状态。
- Modify: `.github/workflows/ci.yml`
  - PR 三平台原生构建并执行 feature targets。

### Modified docs

- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`
- Modify: `docs/relay_macos.md`
- Modify: `docs/relay_win.md`
- Modify: `docs/server-admin-console.md`

## 任务依赖

```text
Task 1 公共原语
  -> Task 2 Linux
  -> Task 3 Darwin worker -> Task 4 Darwin runtime
  -> Task 5 Windows worker -> Task 6 Windows runtime
  -> Task 7 Metrics/JSON
  -> Task 8 Admin 路由
  -> Task 9 Console
  -> Task 10 文档
  -> Task 11 k6/恢复/CI
  -> Task 12 全量验证
```

## Task 1: 实现公共 snapshot lifetime 与 execution gate

**Files:**
- Add: `src/tunnel/relay_runtime_snapshot.h`
- Add: `src/unittest/relay_runtime_snapshot_test.cpp`
- Modify: `src/CMakeLists.txt`

- [x] **Step 1: 先添加会编译失败的公共 helper 测试 target**

在 `src/CMakeLists.txt` 增加 `tcpquic_relay_runtime_snapshot_test`，源文件只包含新单测，链接
`Threads::Threads`，调用 `tcpquic_target_include_dirs()`，并加入 `TCPQUIC_TEST_TARGETS`。

在单测先引用以下尚不存在的类型，覆盖 move-only 与默认失败语义：

```cpp
TqRelayRuntimeSnapshotSupport support;
TqRelayRuntimeSnapshotExecutionGate gate;
TqRelayRuntimeSnapshotResult<FakeSnapshot> result;
static_assert(!std::is_copy_constructible_v<
    TqRelayRuntimeSnapshotLease<FakeWorker>>);
```

Run:

```bash
rtk cmake --build build --target tcpquic_relay_runtime_snapshot_test -j$(nproc)
```

Expected: 编译失败，提示 `relay_runtime_snapshot.h` 或上述类型不存在。

- [x] **Step 2: 实现 header-only 基础类型**

在 `relay_runtime_snapshot.h` 中实现：

- `TqRelayRuntimeState { Stopped, Starting, Running, Stopping }`；
- `TqRelaySnapshotWorkerRef<WorkerT>`，identity 使用 runtime slot；
- `TqRelayRuntimeSnapshotResult<SnapshotT>`，默认 `SnapshotComplete=false`、
  `IdentitiesComplete=false`；
- `TqRelayRuntimeSnapshotStats`；
- move-only `TqRelayRuntimeSnapshotLease<WorkerT>`；
- `TqRelayRuntimeSnapshotSupport::AcquireWorkersLocked()`、私有 release、
  `WaitForIdleLocked()`、stats/failure 记录。

约束：先分配 worker ref vector，再在 `SnapshotLock` 下增加 in-flight；lease move assignment 必须
先正确释放旧 ownership；release counter 为 0 时 assert，禁止静默吞掉 double release。

- [x] **Step 3: 添加 lease 与 Stop barrier 的确定性测试**

至少覆盖：

- 空 Workers 也能返回 identity-complete 的空 lease；
- lease move construct/move assign 只 release 一次；
- fake Snapshot 抛异常后 lease 析构，`InFlight==0`；
- Stop 线程持 runtime lock 调用 `WaitForIdleLocked()`，在 lease release 前不返回且 fake worker
  destructor count 为 0；release 后 Stop 完成；
- worker ref vector 分配/构造失败发生在 in-flight 增加前。

- [x] **Step 4: 实现跨线程 execution permit**

`TqRelayRuntimeSnapshotExecutionGate` 使用内部 mutex/Cv/Busy/Epoch，`TryAcquire(deadline)` 返回
shared permit state。permit 可同时由 request 与 async command 持有，最后一个 token 在任意线程
释放时才清 Busy 并 notify；不能用 `std::timed_mutex` 跨线程 unlock。

stats 至少包含 busy/outstanding、wait count/nanos、deadline timeout、detached late command。

- [x] **Step 5: 添加 execution gate 测试**

用两个请求线程和一个 fake completion 线程验证：

- request A 获取 permit 后 command token 保活；
- A deadline 后 detach，gate 仍 busy；
- request B 到 deadline 只得到失败，不创建第二 command；
- completion/cancel 在线程 C 释放最后 token 后，request C 才能成功 acquire；
- outstanding 始终 `<= 1`，没有固定 sleep。

- [x] **Step 6: 运行公共单测**

```bash
rtk cmake --build build --target tcpquic_relay_runtime_snapshot_test -j$(nproc)
rtk build/bin/Release/tcpquic_relay_runtime_snapshot_test
```

Expected: build 成功，测试退出 0。

## Task 2: 迁移 Linux Runtime 并建立行为基线

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: 写 Linux 失败测试**

在现有 runtime 测试附近增加 `TQ_UNIT_TESTING` one-shot hooks，并先写以下断言：

- `SnapshotWorkers(deadline)` 启动 2 个 worker 时返回两个唯一 slot `0/1`；
- 用至少两个实际 tunnel 覆盖 `PickWorker -> TqRelayHandle -> TqTunnelRegistryMetadata ->
  /tunnels`：每条 tunnel 的 `worker_index` 必须等于创建它的 relay slot，不能因为 metadata 默认值
  全部展示为 0；同时验证 `relay_started` 与 `relay_stopping` trace 使用同一 index；
- 非 0 worker timeout/fallback 后仍为 `linux-1`，不能退化为重复 `linux-0`；
- 共享 absolute deadline，两个 blocked worker 的总耗时不接近 `2 * 5s`；
- lease acquire 后阻塞，Stop 已进入但 worker 未析构；释放后 Stop 完成；
- 第 k 个 worker Start 返回 false/抛异常后 Runtime 为 Stopped、Workers 为空，重试得到完整 N；
- runtime lock 在 deadline 前拿不到时 `IdentitiesComplete=false`，不得无锁读 Workers；
- `RuntimeLockAcquireCount` 等 legacy 指标仍增长。

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
```

Expected: 编译失败，因为 deadline overload/result/state hooks 尚未实现。

- [ ] **Step 2: 把 Linux runtime lifetime 字段迁入 helper**

删除 Runtime 自有 `AcquireSnapshotWorkers()` / `ReleaseSnapshotWorkers()`、Snapshot mutex/Cv 与
in-flight counter，新增 `TqRelayRuntimeSnapshotSupport SnapshotSupport` 和
`TqRelayRuntimeState State{Stopped}`。

保留 `AcquireRuntimeLockForMetrics()`；新增 deadline-aware 版本，使用 `try_lock` + 有界退避，
同时累计 wait/acquire/timeout。helper 必须接收已持有的 unique_lock，不能自行绕过指标加锁。

- [ ] **Step 3: 改造 Linux worker Snapshot deadline 与异常边界**

- `TqLinuxRelayWorker::Snapshot(deadline)` 把剩余时间传给 command wait；无参 overload 使用
  `now + 5s`。
- `SnapshotCommand` 继续由现有 `ControlOwner` shared ownership 保活。
- worker event dispatch 捕获 `SnapshotLocal()` 构造/分配/result move 异常，以默认 incomplete
  result 完成并 notify，异常不能逃出 worker thread。
- `SnapshotComplete` 默认 false，只有成功 `SnapshotLocal()` 显式置 true；timeout counters 保持。

- [ ] **Step 4: 改造 Linux Runtime Start/Stop/Snapshot**

- Start 持 runtime lock：Running 直接 true；Stopped 才 staged build；失败显式 Stop 已启动 worker
  并恢复 Stopped；成功 swap 后 Running。
- Stop 持 runtime lock，State=Stopping，`WaitForIdleLocked()` 后才 clear/reset，最后 Stopped。
- `Snapshot(deadline)` / `SnapshotWorkers(deadline)` 使用 lease；后者返回
  `TqRelayRuntimeSnapshotResult<TqLinuxRelayWorkerSnapshot>`。
- 逐 worker 使用同一 absolute deadline；无论 worker result 是否完整，都用 lease slot 覆盖
  `WorkerIndex`。
- Stopped 空集合的 aggregate 与 workers result 显式 complete。
- 在 `relay.h` 为 Linux 增加 `LinuxWorkerIndex`（Windows 已有等价字段；Darwin 也必须补齐）；
  `TqRelayStart*()` 写入 `worker->WorkerIndex()`，`TqRelayStop()` 清零。`tcp_tunnel.cpp` 更新
  registry metadata 与 trace 时必须读取 handle 中保存的 index，不能遗漏 Linux 分支。

- [ ] **Step 5: 保留 Linux compatibility metrics**

把 helper stats 映射回 `RuntimeSnapshotInFlightMax`，保证
`linux_relay_runtime_snapshot_inflight_max` 不消失；现有 runtime lock wait/acquire 字段继续按
相同或更严格语义累计。

- [ ] **Step 6: 运行 Linux 回归**

```bash
rtk cmake --build build --target \
  tcpquic_relay_runtime_snapshot_test \
  tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_relay_runtime_snapshot_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: 两个测试退出 0，TSAN/ASan 留到 Task 12。

## Task 3: 改造 Darwin worker snapshot command

**Files:**
- Modify: `src/tunnel/darwin_relay_event_queue.h`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: 写 shared-command/deadline 失败测试**

先覆盖：

- worker queue 被 test hook 阻塞时 `Snapshot(deadline)` 在总 deadline 返回 incomplete；
- caller 返回后再释放 worker，迟到 completion 不访问栈对象；
- 连续 snapshot timeout 不产生悬空 `SnapshotCommand*`；
- snapshot dispatch 中注入 allocation/build throw，worker thread 继续存活且返回 incomplete；
- 配置非 2 次幂 queue capacity 时 snapshot 上报 `EventQueue.Capacity()` 的归一化值。

Expected: 初始编译失败或现有无限等待测试被 watchdog 判失败。

- [ ] **Step 2: 给 Darwin control event 增加 shared owner**

在 `TqDarwinRelayEvent` 增加 `std::shared_ptr<void> ControlOwner`。Snapshot event 必须从 owner
恢复 `shared_ptr<SnapshotCommand>`；`Control` raw pointer 只为尚未迁移的其他 control event 保留，
不能作为 snapshot lifetime 的唯一来源。

- [ ] **Step 3: 重写 Darwin SnapshotCommand 完成协议**

- command 具有 Completed/Cancelled/Detached once-only 状态、Cv、result 与 execution permit token；
- `CompleteSnapshotCommand()` 幂等，result 构造异常时完成默认 incomplete；
- `Snapshot(deadline, permit)` 用 `wait_until`，timeout 只 detach，不销毁 command；
- worker Stop/purge 必须 cancel queued snapshot、notify waiter、释放 permit token；
- `SnapshotLocal()` 设置 `WorkerIndex`、`EventQueue.Capacity()` 与 `SnapshotComplete=true`。

- [ ] **Step 4: 在 worker event dispatch 捕获异常**

Snapshot 分支独立 try/catch；不能让 `SnapshotLocal()` 中 vector/string allocation 异常逃出
`Run()`。catch 路径增加 failure counter，并通过 shared command 返回 incomplete。

- [ ] **Step 5: 运行 Darwin worker tests**

在 macOS：

```bash
rtk cmake --build build --target \
  tcpquic_darwin_relay_worker_queue_test \
  tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.logicalcpu)
rtk build/bin/Release/tcpquic_darwin_relay_worker_queue_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: 两个测试退出 0，无 timeout hang。

## Task 4: 接入 Darwin Runtime state、lease 与 execution gate

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
- Modify: `src/unittest/darwin_relay_metrics_test.cpp`

- [ ] **Step 1: 写 Darwin Runtime 失败测试**

使用 `tuning.RelayWorkerCount=2`、非默认 `RelayEventQueueCapacity`，断言：

- `SnapshotWorkers(deadline)` 返回 N 个连续唯一 index 和真实 normalized capacity；
- 启动至少两个 Darwin relay 后，`/tunnels` 的每条 `worker_index` 与 `darwin-N`/worker snapshot
  对应；不得因 handle 或 registry metadata 未赋值而全部为 0；
- aggregate capacity 取 max，Stopped 两条 snapshot 路径都 complete；
- Snapshot A timeout 后 permit 由 late command 持有，Snapshot B 不 enqueue，outstanding `<=1`；
- Snapshot 与 Stop 并发时 Stop 在 lease release 前不调用任何 worker Stop；
- Start index k 失败后无部分 Workers，重试仍为 `[0,N)`；
- snapshot 期间 `PickWorker()` 不再被整个 worker command wait 阻塞。

- [ ] **Step 2: 增加 Runtime state/helper/gate 成员**

替换 `Started` bool 为明确 state；增加 `SnapshotSupport`、`SnapshotExecutionGate` 与
deadline-aware `TryAcquireRuntimeLockForSnapshot()`。gate 内部 mutex 不得与 Runtime mutex 或
SnapshotLock 同时持有。

- [ ] **Step 3: staged Start 并接入真实 queue capacity**

持 Runtime mutex 构造局部 workers，给每个 config 写入
`tuning.RelayEventQueueCapacity`。全部 Start 成功后 swap；失败显式 Stop 局部 worker 并恢复
Stopped。Running 重复 Start 仍立即 true。

- [ ] **Step 4: 实现 Snapshot/SnapshotWorkers/Stop**

- `Snapshot(deadline)` 和 `SnapshotWorkers(deadline)` 先取 permit，再取 runtime lease，锁外逐
  worker snapshot；command 持 permit token。
- request timeout 后 permit 由 command 持续保活；新请求只等待 gate，不重复 enqueue。
- Stop 不等待 permit，但持 Runtime mutex 等 lease 归零，再 worker Stop/clear；queued command
  必须由 worker Stop cancel。
- slot identity 由 lease 覆盖；runtime lock deadline 前失败时 identity incomplete。
- 为 `TqRelayHandle` 增加 Darwin worker index，Darwin relay start/sink start 写入该 slot，stop 清零；
  `tcp_tunnel.cpp` registry metadata 和 trace 读取该字段，并通过 client/server tunnel API 回归测试。

- [ ] **Step 5: 运行 Darwin Runtime/metrics tests**

```bash
rtk cmake --build build --target \
  tcpquic_darwin_relay_worker_io_test \
  tcpquic_darwin_relay_metrics_test -j$(sysctl -n hw.logicalcpu)
rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test
rtk build/bin/Release/tcpquic_darwin_relay_metrics_test
```

Expected: 测试退出 0；metrics 完整接线将在 Task 7 补齐后再跑一次。

## Task 5: 改造 Windows worker IOCP snapshot command

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 写 IOCP shared-command 失败测试**

复用 `TqWindowsRelayWorkerQueueBlockForTest`，先覆盖：

- block 进入 worker queue 后 Snapshot 在 deadline 返回 incomplete；
- caller detach 后释放 block，迟到 completion 不访问栈地址；
- 下一 Snapshot 不在第一个 command terminal 前 post 第二 command；
- `PostQueuedCompletionStatus` failure 与 BuildSnapshot throw 都返回显式 incomplete，worker 存活；
- active relay 的 read/write/pending 由 ActiveRelayStates 求和，relay 退出后允许下降。

- [x] **Step 2: 给 IO operation 增加 shared control owner**

在内部 `IoOperation` 增加 `std::shared_ptr<void> ControlOwner`。Snapshot operation 在 post 前同时
设置 raw `Control`（兼容 dispatch）和 owner；IOCP dispatch 从 owner 恢复 command，不能只解引用
caller stack。

- [x] **Step 3: 重写 Windows SnapshotCommand wait/completion**

- command 保存 once-only state、result、Cv 与 execution permit token；
- `Snapshot(deadline, permit)` 使用 `wait_until`，timeout detach；
- `PostQueuedCompletionStatus` 失败直接完成 incomplete 并释放 token；
- worker Stop/drain 取消剩余 command 并释放 token；
- dispatch 捕获 `BuildSnapshotLocal()`/move 异常，不能逃出 worker thread。

- [x] **Step 4: 补齐 Windows worker snapshot 字段**

增加 `WorkerIndex`、`TcpReadBytes`、`TcpWriteBytes`、`PendingBytes`、`SnapshotComplete`。
`BuildSnapshotLocal()` 在现有 ActiveRelayStates 循环内求 read/write；pending 使用
`PendingQuicReceiveBytes + RelayBufferBytesInUse`。禁止把累计 `TcpSendBytes` 直接映射到 Admin
`TcpWriteBytes`。

- [x] **Step 5: 运行 Windows worker test**

在 Windows PowerShell：

```powershell
rtk cmake --build build --config Release --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test.exe
```

Expected: 测试退出 0，Application Verifier 留到 Task 12。

## Task 6: 接入 Windows Runtime state、lease 与 execution gate

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 写 Windows Runtime 失败测试**

必须设置 `tuning.WindowsRelayWorkerCount=2`，断言：

- per-worker result size/index/identity 正确；
- 启动至少两个 Windows relay 后，`/tunnels` 中每条 `worker_index` 与 `windows-N` 和 handle
  中保存的 `WindowsWorkerIndex` 一致；不得因 registry metadata 漏传而回退为 0；
- Stop 等待 lease，且 lease release 前 worker destructor count 为 0；
- Snapshot A timeout 后 B 不产生第二 IOCP snapshot operation；
- Start index k failure/throw 完整回滚，Running 重复 Start 不替换一代；
- Stopped 返回 identity-complete 的空 worker 集合和 complete aggregate；
- active-only read/write/pending 的 runtime sum 等于 worker sum。

- [x] **Step 2: 替换 RuntimeWorkerLifetimeLock**

增加 State、SnapshotSupport、SnapshotExecutionGate 与 deadline-aware Runtime lock acquire；删除
`RuntimeWorkerLifetimeLock()` 及 Snapshot/Stop 调用点。execution gate 只负责 snapshot 串行，
lease 只负责 worker lifetime。

- [x] **Step 3: staged Start 与 bounded Snapshot**

Start 在 Runtime lock 下 staged 构造 `TqWindowsRelayWorker(i)`，全部成功后 swap；失败停止局部
workers 并恢复 Stopped。实现 `Snapshot(deadline)` / `SnapshotWorkers(deadline)`，identity 永远
来自 slot，所有 worker 共用绝对 deadline。

审计 `TqRelayStart*()`、receive-sink start、`TqRelayStop()`、tunnel registry metadata 和 trace：
现有 `WindowsWorkerIndex` 必须在所有路径写入、使用和清零，不能只修 workers Admin snapshot。

- [x] **Step 4: 改造 Stop**

Stop 持 Runtime lock 设置 Stopping，等待 lease idle，再 clear Workers/重置 NextWorker，最后
Stopped。Stop 不等待 execution permit；worker destructor/Stop 负责 cancel command，让 permit
终态释放。

- [x] **Step 5: 运行 Windows Runtime test**

```powershell
rtk cmake --build build --config Release --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test.exe
```

Expected: 测试退出 0。

## Task 7: 实现 neutral metrics、单次采样和三平台转换

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/darwin_relay_metrics_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [x] **Step 1: 先写结果模型与 JSON 失败断言**

增加平台条件测试，覆盖：

- `TqSnapshotRelayWorkers()` 返回 result 而非裸 vector；
- list 顶层与每行有 `snapshot_complete`；
- Running N worker 返回 `aggregate + N`，Stopped 只有 complete aggregate；
- 完整响应 sum/max 不变量成立；
- `/relay/workers` 一次请求每 worker 只执行一次 Snapshot；
- 同一批运行中的多 worker relay 同时请求 `/relay/workers` 与 `/tunnels` 时，非 aggregate
  tunnel 的 `worker_index` 必须能对应其 handle/trace 的 slot；允许负载不均，但不允许所有
  tunnel 因默认值集中显示为 0；
- Darwin actual capacity、Windows active-only 字段、Linux prefix 均正确；
- `/relay/metrics` 有 `snapshot_complete` 与 neutral runtime snapshot metrics；
- runtime lock deadline 前失败时 `IdentitiesComplete=false`，空 aggregate 不能 vacuous true。

- [x] **Step 2: 扩展 neutral snapshot structs**

在 `relay_metrics.h` 增加：

- `TqRelayWorkersSnapshotResult`；
- `TqRelayWorkerLookupStatus { Ok, NotFound, SnapshotUnavailable }`；
- `TqRelayWorkerSnapshot::SnapshotComplete`；
- `TqRelayMetricsSnapshot::SnapshotComplete` 与 neutral `RelayEventQueueCapacity`；
- neutral helper/gate stats 字段：in-flight/current max、acquire/failure、Stop wait、execution
  busy/wait/timeout/detached count。

保留 `LinuxRelayEventQueueCapacity` 与 `LinuxRelayRuntimeSnapshotInFlightMax` 等 legacy 字段。

- [x] **Step 3: 添加 Darwin/Windows converters**

与现有 Linux converter 并列实现 `ConvertDarwinRelayWorkerSnapshot()` 和
`ConvertWindowsRelayWorkerSnapshot()`。backend/id 分别为 `darwin-N`、`windows-N`；Windows
capacity 为 0，byte/pending 使用 Task 5 已固定口径。

- [x] **Step 4: 删除 workers endpoint 的双重采样**

移除无参 `MakeAggregateRelayWorkerSnapshot()` 内对 `TqSnapshotRelayMetrics()` 的调用。新的流程：

```text
platformResult = Runtime::SnapshotWorkers(deadline)
rows = Convert(platformResult.Workers)
aggregate = Reduce(rows, compile-time backend, complete, identitiesComplete)
result.Workers = [aggregate] + rows
```

Stopped 空集显式 complete；identity 未取得的空集显式 incomplete。完整结果只对 additive 字段
求和，capacity 取 max。

- [x] **Step 5: 接入 `/relay/metrics` complete 与 neutral stats**

三平台 `TqSnapshotRelayMetrics()` 调用 bounded aggregate Snapshot；partial/fallback 设置
`SnapshotComplete=false`。neutral JSON 输出设计稿列出的 runtime snapshot stats；Linux 同时写
legacy alias，Darwin 的 `relay_event_queue_capacity` 来自实际 capacity，Windows 为 0。

- [x] **Step 6: 运行当前平台 metrics/router tests**

Linux：

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

macOS：

```bash
rtk cmake --build build --target \
  tcpquic_darwin_relay_metrics_test \
  tcpquic_router_runtime_test -j$(sysctl -n hw.logicalcpu)
rtk build/bin/Release/tcpquic_darwin_relay_metrics_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

Windows：

```powershell
rtk cmake --build build --config Release --target tcpquic_router_runtime_test -j
rtk build/bin/Release/tcpquic_router_runtime_test.exe
```

Expected: 对应测试全部退出 0。（Windows 已验证 `tcpquic_router_runtime_test` exit 0）

## Task 8: 传播 worker detail 三态到 client/server Admin 路由

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
- Modify: `src/unittest/server_admin_test.cpp`

- [x] **Step 1: 写 200/404/503 失败测试**

client/router 与 server handler 都覆盖：

- `aggregate` 和 `{platform}-0` complete 时 200，body 含正确 identity 与 `relays:[]`；
- identity 完整且 index 不存在时 404；
- runtime lock 未取得或目标 worker snapshot incomplete 时 503，错误码
  `snapshot_unavailable`；
- 连字符 ID 正常解码，空 segment/嵌套 slash 仍 404；
- capabilities 保持 `worker_detail:true`、`per_worker_active_relays:false`。

- [x] **Step 2: 用结果对象替换 found/supported bool 组合**

让 `TqRelayWorkerDetailJson()` 返回 body + `TqRelayWorkerLookupStatus`（或等价 struct）。不要把
SnapshotUnavailable 复用为 NotFound，也不要把 partial 数值作为 200 detail。

同步修改 `server_admin_test.cpp` 内 stub 签名和 JSON；避免只修真实实现导致 server test 链接或
编译失败。

- [x] **Step 3: 修改两套路由状态映射**

- Ok -> 200；
- NotFound -> 404 `not_found`；
- SnapshotUnavailable -> 503 `snapshot_unavailable`；
- backend 不支持的旧语义若仍保留，继续使用独立 `not_supported`，不要与 timeout 混合。

- [x] **Step 4: 运行 Admin route tests**

```bash
rtk cmake --build build --target \
  tcpquic_router_runtime_test \
  tcpquic_server_admin_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
rtk build/bin/Release/tcpquic_server_admin_test
```

Expected: 两个测试退出 0。macOS/Windows 在 Task 12 用原生命令重复。

## Task 9: 把 Admin-console Relay 页收敛为单请求

**Files:**
- Modify: `src/runtime/admin_console.cpp`
- Modify: `src/unittest/admin_http_test.cpp`

- [x] **Step 1: 写 console 静态失败断言**

在 `admin_http_test.cpp` 断言 `renderRelay()`：

- 只调用 `api('/relay/workers')`，不再在该函数内调用 `api('/relay/metrics')` 或 Promise.all；
- 从 `workers[0]` 且 `worker_id==='aggregate'` 的行填 Backend/Active/Pending/Errors 卡片；
- 表格仍渲染全部 rows，列定义不变；
- 顶层或 aggregate `snapshot_complete:false` 时显示可见错误/partial 文案，不把零值显示为健康；
- request failure 仍走现有 panel error。

- [x] **Step 2: 重写 `renderRelay()` 数据源**

保留现有 capability 与 table rendering helper。只做数据 wiring 和 incomplete 状态展示，不新增
Workers 列、不改变 3 秒 refresh cadence。

- [x] **Step 3: 运行 Admin HTTP/console tests**

```bash
rtk cmake --build build --target tcpquic_admin_http_test -j$(nproc)
rtk build/bin/Release/tcpquic_admin_http_test
rtk scripts/check-admin-console-v4.sh
```

Expected: 测试与 console 静态检查退出 0。

## Task 10: 更新 API、平台与 console 文档

**Files:**
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`
- Modify: `docs/relay_macos.md`
- Modify: `docs/relay_win.md`
- Modify: `docs/server-admin-console.md`

- [x] **Step 1: 更新 Admin API 契约**

写明：

- workers 顺序为 aggregate 后接平台 index 升序；
- `snapshot_complete` 顶层/每行语义；
- list partial 200、detail snapshot unavailable 503、unknown 404；
- aggregate sum/max 与顺序采样而非全局原子语义；
- `/relay/metrics` complete、neutral stats 与 legacy Linux aliases；
- `relays:[]` 和 capabilities 保持不变。

- [x] **Step 2: 更新三平台 relay 文档**

- Linux：helper 迁移、并发 snapshot 不变、absolute deadline 与 legacy metrics；
- macOS：真实 normalized queue capacity、execution permit、Stop barrier；
- Windows：删除旧 lifetime lock、execution permit 与 active-only byte 口径。

- [x] **Step 3: 更新 console 文档**

说明 Relay 页只请求 workers endpoint、aggregate 卡片来源和 incomplete 显示。

- [x] **Step 4: 检查文档 diff**

```bash
rtk git diff --check -- \
  docs/admin-api/interface.md \
  docs/relay_linux.md \
  docs/relay_macos.md \
  docs/relay_win.md \
  docs/server-admin-console.md
```

Expected: 无输出，退出 0。

## Task 11: 落地 k6、恢复 harness 与分层 CI

**Files:**
- Add: `tests/k6/relay_workers.js`
- Add: `tests/scripts/run_relay_runtime_snapshot_recovery.py`
- Add: `tests/scripts/test_run_relay_runtime_snapshot_recovery.py`
- Modify: `.github/workflows/ci.yml`
- Add: `.github/workflows/relay-runtime-snapshot-nightly.yml`

- [x] **Step 1: 实现完整 k6 场景**

把设计稿骨架扩展为通过 `SCENARIO` 选择的：

- `console_baseline`：1 VU，每 3 秒 list，10m；
- `console_peak`：0->10 VU、稳态、ramp-down；
- `admin_mixed`：80% list、15% valid detail、5% expected 404，10 req/s；
- `admin_spike`：0->32->0；
- `admin_soak`：5 req/s，30m；
- `breaking_point`：每级 +16 req/s。

脚本必须：要求 `ADMIN_TOKEN`；支持 `BASE_URL`、`WORKER_IDS`、VU/rate/duration；用
`expectedStatuses(200,404)` 隔离预期 404；解析 `snapshot_complete`；输出 endpoint/scenario tags；
为 arrival-rate 设置 `preAllocatedVUs/maxVUs`；baseline 与 breaking point 使用独立 thresholds 和
summary。

- [x] **Step 2: 验证 k6 脚本可加载与短跑**

```bash
rtk k6 inspect tests/k6/relay_workers.js
rtk k6 run \
  -e SCENARIO=admin_mixed \
  -e BASE_URL=http://127.0.0.1:8080/api/v1 \
  -e ADMIN_TOKEN="$TOKEN" \
  -e WORKER_IDS=linux-0 \
  -e DURATION=30s \
  --summary-export artifacts/relay-workers-smoke.json \
  tests/k6/relay_workers.js
```

Expected: `inspect` 成功；有测试服务时 short run 满足 complete/error/dropped thresholds。
（本机未安装 k6 时跳过 short run；脚本已落地，CI/nightly 与 release-lab 使用。）

- [x] **Step 3: 实现跨平台 recovery harness**

Python 脚本接受：

- `--base-url`、`--token`；
- `--start-command`、`--stop-command`、`--kill-command`；
- 可选 `--freeze-worker-command` / `--unfreeze-worker-command`；
- `--graceful-timeout 60`、`--recovery-timeout 90`；
- 可选 CONNECT probe command。

它记录 T0、轮询 `/api/v1/health` 与 workers complete 状态，验证 Stop 未越过 lease、hard kill 后
health 和 10 个新 CONNECT 在 T0+90s 前恢复，并把 timeline/JSON/退出码写到 artifact dir。

- [x] **Step 4: 给 harness 添加自检**

使用 fake start/stop/health server 或 dry-run fixture，覆盖正常恢复、graceful timeout、health 永久
失败和命令非零退出；不要让 release-lab 首次发现脚本自身状态机错误。

```bash
rtk python3 tests/scripts/run_relay_runtime_snapshot_recovery.py --help
rtk python3 -m py_compile tests/scripts/run_relay_runtime_snapshot_recovery.py
rtk python3 tests/scripts/test_run_relay_runtime_snapshot_recovery.py
```

Expected: 参数帮助和语法检查成功；harness 自检覆盖成功恢复、60/90 秒边界的注入时钟、health
永久失败和子命令非零退出，测试退出 0。

- [x] **Step 5: 修改 PR CI**

在现有三平台 matrix 中，除 production binary 外构建并运行 feature targets：

- 所有平台：`tcpquic_relay_runtime_snapshot_test`、`tcpquic_router_runtime_test`、
  `tcpquic_server_admin_test`、`tcpquic_admin_http_test`；
- Linux：`tcpquic_linux_relay_worker_io_test`；
- macOS 每个实际运行架构至少运行 `tcpquic_darwin_relay_worker_io_test` 和
  `tcpquic_darwin_relay_metrics_test`；
- Windows：`tcpquic_windows_relay_worker_test`。

CI runner 未安装 RTK 时 workflow 使用原生命令；本地开发命令继续遵守 RTK 规则。

- [x] **Step 6: 新增 nightly workflow**

schedule + workflow_dispatch，至少执行：Linux ASan/TSAN、macOS ASan、Windows Application
Verifier/page heap 可用路径，100 轮 Snapshot/Stop/Start 和 30 分钟 soak。上传 sanitizer log、
snapshot stats 与 timeline；任一 UAF/deadlock/outstanding command >1/in-flight 不归零即失败。

- [x] **Step 7: 明确 release-lab 手动门禁**

在 workflow summary 或脚本 README 中记录：4,096 active relay、k6 baseline/peak/breaking point、
数据面并行吞吐、worker freeze、SIGKILL/TerminateProcess 与 supervisor restart 不在普通 PR job
执行，但三平台最近一次 release-lab 结果是发布前置条件。

## Task 12: 全量验证与交付

**Files:**
- 本计划涉及的全部文件。

- [ ] **Step 1: Linux 原生 build/test**

```bash
rtk cmake --build build --target \
  tcpquic_relay_runtime_snapshot_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_router_runtime_test \
  tcpquic_server_admin_test \
  tcpquic_admin_http_test -j$(nproc)
rtk build/bin/Release/tcpquic_relay_runtime_snapshot_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk build/bin/Release/tcpquic_server_admin_test
rtk build/bin/Release/tcpquic_admin_http_test
```

- [ ] **Step 2: macOS 原生 build/test**

```bash
rtk cmake --build build --target \
  tcpquic_relay_runtime_snapshot_test \
  tcpquic_darwin_relay_worker_queue_test \
  tcpquic_darwin_relay_worker_io_test \
  tcpquic_darwin_relay_metrics_test \
  tcpquic_router_runtime_test \
  tcpquic_server_admin_test \
  tcpquic_admin_http_test -j$(sysctl -n hw.logicalcpu)
rtk build/bin/Release/tcpquic_relay_runtime_snapshot_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_queue_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test
rtk build/bin/Release/tcpquic_darwin_relay_metrics_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk build/bin/Release/tcpquic_server_admin_test
rtk build/bin/Release/tcpquic_admin_http_test
```

- [x] **Step 3: Windows 原生 build/test**

```powershell
rtk cmake --build build --config Release --target `
  tcpquic_relay_runtime_snapshot_test `
  tcpquic_windows_relay_worker_test `
  tcpquic_router_runtime_test `
  tcpquic_server_admin_test `
  tcpquic_admin_http_test -j
rtk build/bin/Release/tcpquic_relay_runtime_snapshot_test.exe
rtk build/bin/Release/tcpquic_windows_relay_worker_test.exe
rtk build/bin/Release/tcpquic_router_runtime_test.exe
rtk build/bin/Release/tcpquic_server_admin_test.exe
rtk build/bin/Release/tcpquic_admin_http_test.exe
```

- [ ] **Step 4: sanitizer 与确定性 stress**

- Linux ASan/TSAN：公共 helper、Linux Runtime、router tests；
- macOS ASan：Darwin worker/runtime/metrics；
- Windows Application Verifier/page heap：Windows worker/runtime 与 router；
- 每个平台执行 100 轮 Start/Stop/Snapshot，确认无 crash、deadlock、counter 下溢、late command
  堆积或 worker UAF。

- [ ] **Step 5: k6、容量与数据面并行门禁**

执行设计稿第 9/10 节矩阵。保存 p50/p95/p99、error/check/dropped、CPU/RSS、helper/gate stats、
每请求 worker snapshot 次数和数据面吞吐。门禁：Admin p99 < 3s、A/B p95/p99 回归 <=5%、
数据面吞吐回归 <=3%、CPU 增幅 <=5%、payload 100% 正确、结束后 in-flight=0、outstanding
command=0。

- [ ] **Step 6: recovery 门禁**

三平台分别运行 recovery harness：正常 Stop、snapshot barrier、queue/post failure、worker freeze、
持续 3 秒 console polling 下重启、进程 hard kill。验证 T0+60/T0+90、health、完整 workers list、
10 个新 CONNECT 和 artifact timeline。

- [x] **Step 7: 最终静态检查**

```bash
rtk rg -n "RuntimeWorkerLifetimeLock|AcquireSnapshotWorkers|ReleaseSnapshotWorkers" \
  src/tunnel src/runtime
rtk git diff --check
```

Expected:

- Windows `RuntimeWorkerLifetimeLock` 和三平台旧手工 snapshot acquire/release 已删除；若搜索仍命中，
  逐项确认是否仅为文档/兼容测试，不得忽略生产路径；
- `git diff --check` 无输出；
- 所有 feature tests、system gates 和文档更新完成。
（Windows 本机：`src/tunnel`/`src/runtime` 无旧 lifetime/acquire-release 命中；文档 `--check` 通过。Linux/macOS 原生、AppVerifier 百轮、k6 lab、live recovery 属 release-lab / nightly，本环境未全跑。）

## 验收标准

- 三平台 Running N worker 时返回 `aggregate + N`，ID/index 连续唯一；Stopped 只返回 complete
  zero aggregate。
- 每次 `/relay/workers` 对每个 worker 最多 snapshot 一次；完整响应满足 additive sum 和
  capacity max。
- snapshot worker 调用期间不持 Runtime lock；Linux 并发语义与 legacy metrics 保持，
  Darwin/Windows outstanding late command 始终 `<=1`。
- Stop 在 lease 归零前不停止/析构 worker；Start 任一点失败不发布部分一代。
- 总 deadline 覆盖 gate/runtime/worker；partial list 显式标记，detail 正确区分 200/404/503，
  `/relay/metrics` 也不把 fallback 伪装为 complete。
- Darwin 上报 normalized `EventQueue.Capacity()`；Windows read/write/pending 保持 active-only。
- console 一次 refresh 只触发一轮 worker snapshot，并能显示 incomplete。
- workers 表与 Tunnels 表的 worker index 可交叉核对：workers 的 active relay 分布和 tunnels
  的 index 分布在同一稳定采样窗口内一致（聚合行 `aggregate` 的 index 0 不参与此比较）。
- neutral snapshot stats、legacy aliases、slow Stop/deadline 日志可查询，pressure 后全部归零。
- PR、nightly、release-lab 三层门禁通过，Admin/data-plane 性能和 60s/90s 恢复目标满足。
- 所有实现、Admin API、平台文档、console 文档与 source design 一致。
