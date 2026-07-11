# Task 5 实施报告

## 状态

已接入 Linux terminal handoff、epoll generation ownership 与 reaper 三事实门禁；focused build 全部成功，四个 focused 进程中三个通过。`tcpquic_linux_relay_worker_io_test` 仍在既有 queue-full terminal fallback 用例返回 `5028`（进程码 164），发生于新增 fatal barrier 用例之后，表现为 owner reset 后 wrapper close 计数仍为 0。

## 实现摘要

- `TqTerminalHandoffControl` 共享 owner 唯一 ledger，并以 acquire/release 发布三事实。
- Linux managed registration 预先发布 handoff control；fatal 路径 freeze epoll admission 后创建唯一 sink，调用 `BeginTerminalShutdown()`，完成本地清理后发布三事实。
- worker stop、TCP fatal、peer abort、fake FIN 统一进入 terminal handoff；正常 TCP EOF/FIN 与 peer FIN half-close 保持原路径。
- epoll payload 携带 relay id 与 control generation；真实 tag generation 不匹配时只计 late cleanup，不进入 relay 数据面。
- reaper 仅在 `DataPlaneStopped && TerminalHandoffComplete && LocalOperationOwnershipTransferredOrDrained` 时删除 tunnel，且自身不调用 owner API。

## TDD 与验证

- RED：truth-table 首次编译明确缺少 `TqTerminalReleaseReady`。
- GREEN：`tcpquic_tunnel_reaper_test` 通过。
- Barrier：shutdown hook 内观察 release predicate 为 false；返回 `PENDING` 后同一 ledger 的三事实为 111。
- Build：四个 focused targets 构建成功。
- Run：reaper、queue、tcp_tunnel 通过；linux IO 在既有 queue-full close assertion 返回 5028。

## 疑虑

- epoll tag 为兼容既有 63-bit relay-id allocator，仅在实际注册 tag 中编码低 31-bit relay id 与 32-bit generation；当前 runtime relay id 递增且远低于该边界，但这是显式容量约束。
- 未实现 Task 6 的真实 connection escalation controller；本任务传入空 escalation。

## 后续测试修复

系统化诊断确认 queue-full `5028` 在基线 `413bfc8` 同样复现，根因是测试局部
`registration.StreamOwner` 在 `owner.reset()` 后仍保留唯一强引用，并非 production
handoff 回归。测试现已在 close-count 断言前释放该引用。fatal barrier 也在验证
`PENDING` handoff 后注入晚到 `SHUTDOWN_COMPLETE`，并断言 terminal retention registry
回到进入用例前的基线，避免污染后续用例。
