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
- server admin 本身不初始化 MsQuic，仅为链接 scheduler 依赖的 stream helper 提供空 API 指针；该测试不执行 stream API。
