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
