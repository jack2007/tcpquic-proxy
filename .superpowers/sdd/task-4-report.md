# Task 4 实施报告：公共 terminal scheduler

## 状态

已实现公共 `TqTerminalScheduler`：生产路径使用单线程、单 mutex/CV 和按到期时间排序的最小堆；测试路径使用同步 fake clock。scheduler task 只弱持 owner，并共享持有 ledger/escalation，不持有 sink，也不形成 owner 强引用环。

## TDD 证据

- RED：先加入 retry/watchdog/cancel 边界测试，执行 `rtk cmake --build build --target tcpquic_terminal_convergence_test -j$(nproc)`，编译失败并明确报告 `TqTerminalScheduler has not been declared`。
- GREEN：实现后 focused target 构建和两个测试二进制退出码均为 0。
- 确定性边界覆盖：失败后的 retry 分别在 10/50/250 ms 到期；第 4 次同步失败只 escalation 一次；成功提交后 4999 ms 不 escalation、5000 ms escalation；再过 30 秒进入 `TerminalTimeout`；terminal callback 后推进 40 秒不 escalation 且无 pending timeout。

## 修改文件

- `src/tunnel/terminal_convergence.h/.cpp`：scheduler API、heap/worker/fake clock、retry/watchdog/timeout/cancel、相关 metrics。
- `src/tunnel/stream_lifetime.h/.cpp`：scheduler friend/helper，BeginTerminalShutdown 接线，terminal cancel，illegal phase escalation。
- `src/unittest/terminal_convergence_test.cpp`：确定性时间边界与取消测试。
- `src/CMakeLists.txt`：直接编译 convergence 的独立测试补齐 stream lifetime；server admin 提供局部 MsQuic API 定义。

## 自审

- escalation 均在 owner、scheduler、ledger 锁外调用。
- Cancel 先将 ledger watchdog 标为 Canceled，再从 heap/index 删除 task；已弹出的并发 task仍由 ledger 状态阻止 escalation。
- `ResetForTest` 先 Stop/join worker，再清 heap/index 并启用 fake clock，避免跨测试线程或任务泄漏。
- 未接入 Linux worker 或 connection runtime，保持 Task 5/6 边界。
- 保留并未暂存工作区原有文档、脚本和未跟踪文件。

## 疑虑

- `Start()` 当前由首次 Schedule/Arm 懒启动；后续若 Task 5 在进程生命周期显式 Start/Stop，语义兼容。
- server admin 本身不初始化 MsQuic；专用测试 stub 仅满足 stream helper 链接，该测试不执行 stream API。

## Review FAIL 修复（2026-07-11）

### RED / GREEN

- RED：先加入 terminal 位于 owner commit 与 scheduler arm/schedule 之间、task 已出堆后 Cancel、allocation failure、索引归零等测试；首次构建因新增 scheduler test API 未实现而链接失败。
- GREEN：上述测试以及 retry allocation failure、thread-start failure、旧 worker 已 `Running=false` 但仍 joinable 的 Start/Stop 交错测试全部通过。

### 不可逆屏障与锁序

- ledger 的 `TerminalObserved/Closed` phase 与 `Canceled` watchdog 是唯一不可逆屏障。terminal event 在同一个 ledger 临界区内写 phase 和 Canceled；Arm、retry enqueue、attempt-4 escalation 均在 ledger 锁下复核。
- schedule 的顺序为 `ledger -> scheduler task mutex`；Cancel 先取得 ledger 引用、释放 task mutex，再标记 ledger Canceled，最后持 task mutex 原地删 heap/index。外部 escalation 不持 owner、ledger、task 或 lifecycle 锁。
- task 出堆后仍保留 stream index，执行完成/owner expired/watchdog timeout 后自然清理；Cancel 因而可以标记已出堆 task 的 ledger，执行侧 CAS/gate 会拒绝 escalation。

### Start/Stop 与异常策略

- 独立 lifecycle mutex 串行化旧 thread join 与新 thread create；join 时不持 scheduler task mutex。Stop join 后清空 heap/index，Start 在复用 thread 对象前必先 join 旧 joinable worker。
- heap 使用可原地 erase/re-heapify 的 vector-backed min-heap，Cancel 不分配。heap/map 入队是事务式操作；所有容器分配与 thread 创建均在 `noexcept` 边界内捕获。
- retry 入队失败返回 false，由 owner 走 attempt-4 ledger gate 做一次锁外 escalation；watchdog 入队/thread-start 失败由 scheduler 锁外 escalation，并累计 `SchedulerFailure`。测试注入验证旧任务不受损且进程不终止。

### CMake 修复

- 移除 `TQ_DEFINE_MSQUIC_API` target-wide compile define；新增专用 `src/unittest/msquic_api_stub.cpp`，只为需要链接 `stream_lifetime.cpp` 的 server admin 测试提供单一 `MsQuic` 定义，避免重复符号。

## 第三轮复审修复（2026-07-11）

- RED：新增 enqueue 与 worker start 之间并发 Stop、Stop join 期间 retry/arm、重复 terminal/cancel、container/thread failure 精确计数测试；新 lifecycle hook 首次构建因未实现而链接失败。
- lifecycle 使用持久 `Ready/Running/Stopping/Stopped` 状态。schedule/arm 在同一个 lifecycle 临界区完成 ledger gate、入队和 worker start；Stop 原子切换到 Stopping 后释放 lifecycle 锁再 join，因此 join 期间的新 schedule/arm 明确失败并执行既定 escalation，不能进入随后 drain 的 heap。
- Stop 由独立 Stop mutex 串行化；join 不持 task mutex。Stop drain 前把所有 indexed ledger 显式、once-only 转为 Canceled，成功入队的 task 不再被无声清除。
- terminal event 首次把 watchdog 转为 Canceled 时直接累计 `WatchdogCanceled`；heap/index 清理不参与该 metric。重复 terminal、Cancel 或 Stop 不重复累计。
- `SchedulerFailure` 只在 container allocation/injection、thread creation/injection 的底层失败点累计；调用方只做 rollback/escalation 策略。container 与 thread 两类注入测试均严格断言增量为 1。

## 最终复审修复（2026-07-11）

- RED：扩展 Stop/join 窗口测试，要求 retry 与 arm fallback escalation 各保留 timeout obligation；最初 generation 为 0 且 pending task 为 0，测试按预期失败。测试随后还捕获 arm fallback timeout 错误地从原 watchdog due（5 秒）再加 30 秒，而非从 escalation 时刻计 30 秒。
- scheduler 内部现在严格只有一个 mutex 和一个 CV；移除了 LifecycleMutex/StopMutex。`LifecycleGeneration` 与 `Ready/Running/Stopping/Stopped` 在同一 state/task mutex 下串行化 Start、Stop、enqueue。
- Stop 在锁内切 Stopping、递增 generation、move 出旧 worker/heap/index，随后解锁做 cancel/join；join 完成后按相同 generation 在锁内完成 Stopped 或 restart 转换。并发 Stop 使用同一个 CV 等待 state 转换，不需要第二把 mutex。
- Stopping/join 窗口发生的 escalation 可将 30 秒 timeout 入新 heap，并设置 `RestartRequested`；Stop join 后启动新 worker 接管。terminal cancel 仍可删除该 obligation。测试推进 30 秒后 retry 与 arm ledger 均进入 `TerminalTimeout`。
- timeout due 统一以实际 escalation 时刻为基准，避免 arm fallback 产生 35 秒期限。
