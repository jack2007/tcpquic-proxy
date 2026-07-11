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
