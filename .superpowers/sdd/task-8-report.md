# Task 8 实施报告

## 状态

已完成 Linux 确定性 terminal convergence 回归 target：`tcpquic_linux_terminal_convergence_test`。

## TDD 证据

1. RED：首次执行 `rtk cmake --build build --target tcpquic_linux_terminal_convergence_test -j$(nproc)`，构建系统返回 `No rule to make target`，退出码 2。
2. GREEN：增加 test-only hooks、八场景测试表与 CMake target 后，新 target 构建成功，八个 case 均逐项输出 `PASS`。
3. 回归：组合构建并运行新 target 与 `tcpquic_linux_relay_worker_io_test`，两个进程退出码均为 0。

## 实现摘要

- 在 `TQ_UNIT_TESTING` 下增加 terminal reserve、downcall、submit 三个无 worker/control mutex 的边界 hook。
- 增加 terminal callback queue-full 与 callback suppression 故障注入开关；production 预处理结果不保留这些字段和分支。
- 新测试通过真实 `TqLinuxRelayWorker`、`TqStreamLifetime` owner、handoff control、socketpair RST 与 worker TCP event API 驱动 fatal convergence。
- 每个 fatal 场景验证 `ABORT|IMMEDIATE`、handoff release facts、relay/terminal sink/timeout/send completion/retention 资源归零和 exactly-once 指标。
- half-close 负向场景验证 graceful peer FIN 不产生 `ABORT` 或 `IMMEDIATE`。
- 所有竞态推进均使用同步 hook、直接事件分派或 fake scheduler API；未使用 `sleep`。

## 验证命令

```bash
rtk cmake --build build --target tcpquic_linux_terminal_convergence_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_terminal_convergence_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk git diff --check
```

## 关注点

- 本任务未增加 production 行为；故障注入全部受 `TQ_UNIT_TESTING` 限制。
- 工作区原有其他未提交改动未触碰，也未纳入本任务提交。

## Reviewer 返工（2026-07-12）

原提交的七个 case 只是同一 `RunFatal` 的别名，且 worker 的三个 hook 均位于真实
`BeginTerminalShutdown` downcall 之外，不能证明 brief 中的竞态。本次返工删除该
fixture，八个入口分别驱动独立场景。

- terminal 边界 hook 移入 `TqStreamLifetime` 状态机：reserve 已提交、真实 shutdown
  downcall 内、submit 状态落定后；三个回调均在 `ControlMutex_` 外执行。
- 新增仅测试编译的 Linux tunnel fixture，直接构造 production `TqTunnelContext`、
  调用 `StartRelay` 并注册真实 reaper；`ReapReadyForTest` 与 reaper 线程共享同一个
  predicate/提取实现，实际调用 `TqReapTunnelContext`。精确 case 断言 release-ready
  前析构计数为 0，三项 handoff facts 成立后 reaper delete 恰好一次。
- 所有 TCP fixture 改用 `AF_INET/INADDR_LOOPBACK`。fd/relay generation 复用、queue-full
  owner release、四方 barrier、5s/30s fake-clock、重复事件和完整 half-close 均为独立测试。
- half-close case 由真实 worker 读取 TCP EOF、提交并完成 QUIC FIN，随后继续把反向
  payload 写回 TCP；peer FIN 最终触发 `SHUT_WR`，并断言无 abort/immediate/watchdog。
- 每个 case 结束后统一检查 terminal sink、timeout、send completion、terminal
  retention、exactly-once 指标回到基线；RAII 清理 MsQuic、scheduler、worker 和 socket。

返工 RED/GREEN 证据：先引用不存在的真实 owner boundary API，目标按预期编译失败；
实现边界后逐项推进运行期 RED，最终八项均打印 `PASS`。组合回归运行 Linux IO、
tunnel reaper、tcp tunnel 三个测试进程，退出码均为 0。
