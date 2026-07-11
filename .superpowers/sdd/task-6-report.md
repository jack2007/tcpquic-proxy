# Task 6 实施报告：generation-safe connection escalation

## 状态

已完成 generation-safe connection escalation：client slot 与 server accepted connection 都使用不可变的 numeric connection id + generation；terminal escalation 只弱持 controller 与不可变 key，不保存 `MsQuicConnection*`。

## 实现摘要

- client slot、picked connection 与 snapshot 增加 `SlotIndex`、`NumericConnectionId`、`Generation`。
- reconnect 时同时推进 generation 并分配新的 process-unique numeric id，旧 escalation 不能命中新 slot incarnation。
- client controller 在 state lock 内校验 slot/key/closing，并预留 exactly-once shutdown operation；operation 执行时再次按 key 校验当前 slot，随后在锁外调用 `Shutdown(0, SILENT)`。
- server accepted record 注册时分配 process-unique numeric id，generation 固定为该 record incarnation 的 `1`；record erase 后 escalation 的 weak controller 自动失效，同一 handle 的新 record 使用新的 numeric id。
- duplicate、closing 与 generation mismatch 分别抑制并计数。
- outgoing/accepted stream factory 之前分配非零 stream/tunnel identity，并从实际 client slot 或 server accepted record 绑定 connection key。
- Linux shared terminal handoff control 携带同一 `shared_ptr<TqTerminalEscalation>`，`BeginTerminalHandoff()` 将其传给 `BeginTerminalShutdown()`；Windows/Darwin 路径未改变，空 token 保持兼容。

## TDD 证据

RED：首次构建 `tcpquic_quic_session_reconnect_test` 按预期因缺少 `MakeTerminalEscalation`、`NumericConnectionId`、closing/duplicate diagnostics 等接口而编译失败。

GREEN 覆盖：

- client old generation 不 shutdown 新 slot。
- client duplicate escalation exactly-once。
- client closing escalation suppressed。
- server correct key shutdown once、duplicate suppressed、closing suppressed。
- server record erase 后 weak escalation 失效；同一 handle 新 record 不受旧 key 影响。
- Linux handoff 保留并发布与 stop control 相同的 escalation token。

## 验证

以下命令退出码均为 0：

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test tcpquic_tcp_tunnel_test tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test -j$(nproc)
rtk build/bin/Release/tcpquic_quic_session_reconnect_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
rtk cmake --build build --target tcpquic_terminal_convergence_test tcpquic_stream_lifetime_test -j$(nproc)
rtk build/bin/Release/tcpquic_terminal_convergence_test
rtk build/bin/Release/tcpquic_stream_lifetime_test
rtk git diff --check
```

## 已知边界

- client 优先通过现有 zero-delay session scheduler 执行 shutdown operation；scheduler 不可用时同步执行 operation，但仍保持二次 key 校验与锁外 down-call。
- server registry 当前没有独立 owner task queue，因此 controller 预留后同步在 registry lock 外执行 down-call；接口与 operation 边界已隔离，后续可把该 operation 投递到 server connection owner queue，而无需改变 escalation/key 协议。
- `tcpquic_client_peer_runtime_test` 当前链接目标缺少 `stream_lifetime.cpp` / `terminal_convergence.cpp` 等既有实现对象，出现大量既有 undefined reference；production `tcpquic-proxy` 与本任务相关测试均可构建。
