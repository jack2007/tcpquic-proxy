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

## Review 修订：connection lifetime pin 与 identity 直传

Review 后追加以下修订：

- client slot owner 从 `unique_ptr` 改为 `shared_ptr<MsQuicConnection>`。terminal operation 在 state lock 内完成二次 key 校验时同步复制 connection pin，锁外只通过该 pin down-call；barrier 测试覆盖 operation 与 slot erase 并发，wrapper 在 operation 返回前不会析构。
- server accepted wrapper 改用 `CleanUpManual` 和明确的 shared owner。server context 与 accepted record 持有同一 owner；controller 在 registry lock 内校验并复制 pin，锁外 down-call；shutdown-complete 先取得 callback local pin、erase record、`Close()`，回调返回后才释放最后 pin。测试覆盖 record erase 与在途 down-call 的析构 barrier。
- `TqClientPickedConnection` 在同一次 slot state lock 选择中携带 connection owner、numeric id、generation；随后生成 immutable escalation token。client runtime 使用 picked overload 直接把 identity/token/owner 传给 tunnel factory。
- legacy handle side table 仅在 pick 返回后重新锁定并确认 slot incarnation 仍完全一致时发布，迟到 pick 不能覆盖 reconnect 后的新 incarnation。
- production terminal binding 不再生成 counter fallback。client raw lookup 或 server accepted record lookup 失败时，factory 在 callback 安装、stream start 或 retention handoff 前直接失败。
- 新增测试覆盖迟到 picked token 在 reconnect 后被 generation gate 拒绝、lookup failure 不安装 accepted callback、client/server owner erase/down-call lifetime barrier，以及既有 handle reuse/weak expiry。

## 复审修订：server callback trampoline 与 side-table TOCTOU

- server `SHUTDOWN_COMPLETE` 应用 callback 不再调用 `Close()` 或释放最后 owner。callback 只 erase record、清 context owner，并把 local pin 投递到专用 cleanup queue；后台线程等待 `ShutdownCompleteEvent`（由 MsQuic 外层 trampoline 在应用 callback 返回后设置）后才释放 wrapper。
- cleanup queue 使用嵌入 `serverContext` 的 intrusive node，callback push 不分配且无容量上限。worker 早运行时等待 trampoline、晚运行时直接消费已完成 signal；线程启动失败时节点仍留在同一 intrusive queue，`QuicServerSession::Stop()` 会 stop/drain queue。
- test hook 使用 outer-return barrier 验证 worker 早运行不会提前析构，并覆盖 delayed release、stop/drain，以及禁用 worker 后连续 retention 256 个 owner 再全部 drain；不存在固定 emergency 槽耗尽后的孤儿 context。
- 删除 client legacy handle side-table 的存储、发布与 lookup；`TqLookupClientTerminalConnection()` 对 raw legacy 路径固定失败。production client runtime 继续通过 `TqClientPickedConnection` 直接传 immutable key/token/owner，因此不存在 old pick 校验后覆盖 new incarnation 的发布窗口。
- reconnect 与 handle reuse 后 legacy lookup 同样保持失败；server accepted record 仍在 registry lock 内直接取得 immutable key/token。
