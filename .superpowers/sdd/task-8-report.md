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
