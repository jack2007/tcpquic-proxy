# Windows / Linux 日志对齐设计

## 目标

统一 Windows 与 Linux 的控制台启动摘要、trace 文件事件与 relay fatal 错误格式，便于跨平台排障与脚本 grep。

## 方案

采用方案 B：统一打印层 + Windows relay trace 对齐，保留 `tcpquic-proxy tuning:` / `tcpquic-proxy runtime:` 前缀。

## 变更摘要

1. **启动 stderr**：`TqPrintTuning` 增加 `relay_workers`；Windows 增加 `win_pending_recv_cap`；恢复 `relay memory budget`；新增 `relay backend` 行。
2. **runtime 采样**：`TqMaybeLogRuntimeObservationsLocked` 在 trace 启用时同步写入 `client.log`。
3. **Windows relay trace**：`event=relay_* backend=windows` 对齐 Linux 生命周期事件。
4. **周期 stats**：`event=stats_relay` 输出 `TqSnapshotRelayMetrics()` 核心字段。
5. **fatal 错误**：`TqTraceRelayFatalError` 同时写 stderr（spdlog）与 trace 文件。

## 兼容性

- Linux 保留 `linux_relay_*` trace 事件名。
- Admin JSON 保留 `linux_relay_*` 字段；Windows 填充可对齐字段。
