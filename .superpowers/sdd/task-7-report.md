# Task 7 实施报告

## 状态

已实现 watchdog 配置边界、terminal retention Admin API、真实 shutdown intent snapshot，以及公共 terminal metrics 聚合与 JSON 输出。

## TDD 与验证

- RED：新增 tuning/config 测试后，编译因 `TuningOverrideTerminalWatchdogSeconds` 不存在而失败。
- GREEN：默认值为 5；显式 5、30 可解析并进入计算结果；4、31 返回 `invalid tuning.terminal_watchdog_seconds`；仅非零 override 序列化。
- Admin：server 测试创建真实 retention ledger，验证组合 filter、`stream_id`、真实 `shutdown_intent`、armed watchdog 和非法 filter 400；router 测试验证 endpoint schema、非法 phase 及公共 metrics 字段。
- 并发 snapshot 的锁外行为由 Task 3 的 `terminal_convergence_test` 覆盖；本实现仅在 `TqSnapshotTerminalRetentions()` 返回后序列化。

执行命令：

```text
rtk cmake --build build --target tcpquic_tuning_test tcpquic_config_router_test tcpquic_server_admin_test tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_tuning_test
rtk build/bin/Release/tcpquic_config_router_test
rtk build/bin/Release/tcpquic_server_admin_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

四个测试进程均退出 0。

## 注意事项

- `shutdown_intent` 已进入 ledger snapshot，Admin 只通过 `TqTerminalShutdownIntentName()` 序列化真实事实；`last_stream_event` 只通过 `TqTerminalEventName()` 序列化。
- 为让 server Admin 测试使用现有 `CreateForTest()` 安全构造 retention，给该测试目标增加了 `TQ_UNIT_TESTING`。
- 工作区原有文档、脚本和临时文件改动未暂存、未修改。
- 发布 runner 对 timeout/violation 的失败门禁按计划延后到 Task 9；Task 7 仅保证字段与覆盖测试存在。

## 接续补充（2026-07-12）

- 将 server admin 与 router runtime 的 retention filter parser 和 schema serializer 抽取为 `terminal_convergence` 公共 helper；严格拒绝重复/未知 filter、零值、带符号及溢出 ID，两端共享同一 JSON schema 实现。
- production 的三处 client/server owner factory 均传播 `Config.Tuning.TerminalWatchdogSeconds`；ledger snapshot 记录实际 watchdog 秒数，测试确定性覆盖 5/30。
- scheduler 使用 fake clock 覆盖 retention oldest age `>5s` warning、`>30s` critical 各一次；扫描先复制 registry ledger，状态计算与日志均在 registry 锁外，terminal release 后清除 per-stream once 状态。
- Task 9 runner failure gate 仍按计划 defer；Task 7 未修改 runner。

## Review 修订（2026-07-12）

- scheduler 在 task heap 为空时仍以 1 秒低频轮询 retention diagnostics；真实 worker 测试使用可控 poll interval/diagnostic clock，覆盖跨越 warning/critical 阈值及 terminal release 清理。
- each-once 状态 key 扩展为 connection id、generation、stream id、role、backend 完整 identity；相同 stream id 的两个连接可独立告警和释放。
- `shutdown_status` 直接序列化 ledger 的真实 `ShutdownStatus`，稳定名称覆盖 success、pending、out_of_memory、invalid_state 和 unknown numeric。
- retention age/deadline 继续使用 steady clock；公开 submitted/terminal 时间戳改为 system clock Unix 毫秒，并验证 epoch 范围与先后顺序。
- 独立 speed-control production handler 接收 `TqConfig` 并传播实际 watchdog；dispatcher speed-control 路径覆盖 override 30。补齐受 terminal convergence 影响的测试 target 源清单后，无 target 全量构建通过。

## 异常安全复审修订（2026-07-12）

- retention diagnostics poll 的 snapshot、identity key/age map、state entry、log collection 分配均由最外层异常边界保护；任何异常只精确增加一次 `SchedulerFailure`，不会逃出 `noexcept`。
- once-only 告警采用 reservation→锁外 emit→commit 协议：所有可能分配的对象和 state entry 在 reservation 前完成；并发 poll 看到 reservation 会跳过，日志形成/emit 前失败不会消费 warning/critical bit。
- 测试分别注入 snapshot、state、log 三个分配失败点，验证不终止、failure +1、bit 未消费，下一次 poll 仍能正常各发一次且后续不重复。
