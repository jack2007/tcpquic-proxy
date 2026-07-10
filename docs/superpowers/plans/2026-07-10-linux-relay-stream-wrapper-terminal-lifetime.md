# Linux Relay Stream Wrapper Terminal Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 保证 Linux relay 在 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` callback 返回后不再解引用自动析构的 `MsQuicStream` wrapper，同时保留 pending send completion accounting 和 active TCP hard-error abort 行为。

**Architecture:** relay-capable stream 从创建时使用 `CleanUpManual`、公共 lifetime owner 和 stable callback router；wrapper callback/context 启动前设置一次。router target 从 tunnel 安全切换到 Linux binding。所有 worker stream API 持有 owner lease；terminal callback 在 owner 发布终态并 seal target，不等待 worker/API lease。

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, MsQuic C++ wrapper, Linux epoll, lock-free relay event queue, CMake unit tests.

---

## File Structure

- Add or Modify: `src/tunnel/stream_lifetime.h` / `.cpp`（名称可按项目惯例调整）
  - 提供 manual-cleanup owner、唯一 phase、terminal retention、API lease 和 stable callback router。
- Modify as needed: `src/CMakeLists.txt`
  - 注册公共 lifetime `.cpp` 和新增测试 source；如果实现为 header-only 则无需改动。
- Modify: `src/tunnel/relay.h` / `relay.cpp`
  - 将 relay registration 从裸 pointer 契约升级为明确的 owner handoff。
- Modify: `src/tunnel/tcp_tunnel.cpp`
  - 将 client tunnel和 server incoming dispatcher wrapper改为 manual owner/stable router。
  - 更新 `TqTunnelContext`、dispatcher target转交、pending bytes显式移交及失败回收。
- Modify: `src/runtime/speed_test.h` / `speed_test.cpp`
  - server speed-control attach接受 accepted stream的同一 owner/router target，不再改 wrapper handler。
- Modify: `src/tunnel/linux_relay_worker.h`
  - 接收 router target/binding owner，声明 terminal logical detach helper。
  - 调整 retiring state，不再保存 terminal `MsQuicStream*`。
  - 增加必要的 test hooks/counters。
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - 将现有 `StreamCallback` 拆成 router target dispatch，不再安装/清除 wrapper handler。
  - terminal dispatch 在 owner 发布终态并 seal binding。
  - 重写 `ProcessQuicShutdownComplete()` cleanup 顺序。
  - 移除 terminal path 对 retired stream 的访问。
  - 审计所有 stream API call capability。
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
  - 增加 callback-time seal、失效 wrapper、pending completion 和 late epoll error 测试。
- Modify: `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/client_tunnel_open_test.cpp`
  - 覆盖 manual owner 创建、pre-relay shutdown、dispatcher handoff 和 registration failure。
- Modify: `src/unittest/speed_test_test.cpp`
  - 覆盖 dispatcher -> speed-control target handoff和 terminal owner回收。
- Modify as needed: `src/runtime/relay_metrics.*`
  - 仅在新增生产诊断计数时贯通 metrics；不改变已有字段语义。
- Modify: `src/docs/thread_model_cn.md`, `docs/relay_linux.md`
  - 更新 stable router、owner phase、三阶段激活和 shutdown sink线程/所有权模型。

## Task 1: 建立公共 lifetime owner、router 和 ownership handoff

- [ ] 实现 `TqStreamLifetime`（暂定名），唯一持有 `CleanUpManual` wrapper和 phase：`CreatedNotStarted`、`Starting`、`Started`、`ShutdownRequesting`、`ShutdownRequested`、`TerminalPublished`、`Closed`。
- [ ] 提供 outgoing/accepted 两个 factory：先建立 shared owner+router再打开/包装 stream，不允许运行期 raw-wrapper adopt。
- [ ] outgoing START 使用 begin/commit/fail 转换，允许 callback 在 API 返回前从 `Starting` 发布 terminal；accepted factory 返回前进入 `Started`。
- [ ] accepted owner/router allocation failure安装无分配 emergency C callback，abort后在 terminal close raw HQUIC；禁止直接 close已启动 raw stream。
- [ ] outgoing进入 `Starting`、accepted安装 handler前启用 terminal retention；只有确认未启动/无 callback或 terminal已发布的 owner才能 delete wrapper。
- [ ] 实现轻量 API lease；lease 持有 shared owner，禁止只返回无 owner 的裸 pointer。
- [ ] lease 根据 owner phase/API kind判断；terminal 发布前已经取得的 lease允许完成，callback 不等待它。
- [ ] 实现 stable callback router：wrapper callback/context 启动前设置一次，router 使用安全 route snapshot/target adapter完成 dispatcher、tunnel、relay handoff。
- [ ] router target不得与 lifetime owner形成无法在 terminal/failure 路径打破的强引用环；callback snapshot 必须保活当前 target。
- [ ] route snapshot不保存裸 `TqTunnelContext*` / relay / worker；target adapter用 shared owner或 `TryEnter/Leave` + deferred retire保证 callback期间存活。
- [ ] `PublishTarget` 与 `PublishTerminalAndTakeTarget` 使用同一个短临界区/CAS协议线性化；terminal 后 publish必须失败，临界区内不调用 MsQuic或等待 worker。
- [ ] 增加 owner级 send completion registry；router把 `ClientContext` 仅作为 opaque key once-only claim typed record，不按当前 target cast或读取未知 pointer。
- [ ] registry record不反向强持 owner形成引用环；claim后需要异步 owner时由 callback显式转交，terminal registry非空进入诊断和once-only cleanup。
- [ ] 不得用 `StreamOpLock` 包住 worker MsQuic API 再让 callback 获取同一锁；为该禁令增加注释/断言。
- [ ] owner/lease 不授权 worker修改已启动 stream 的 callback/context；handler handoff 只切换 router target。
- [ ] client tunnel和 server accepted wrapper从创建时使用 `CleanUpManual`；禁止在 terminal callback 内转换，因为 adapter 已提前缓存 `DeleteOnExit`。
- [ ] 覆盖同步/异步 client tunnel、server incoming dispatcher、dispatcher -> tunnel和 dispatcher -> speed-control target；client hello/structured-error由 dispatcher stable target完成 terminal。
- [ ] `TqTunnelContext` / dispatcher 的 pre-relay shutdown、self-delete 和 relay handoff 都使用同一 owner；任何 target都能在 owner 发布 terminal。
- [ ] 审计 `TqTunnelContext::SendBytes/Abort/Drain`、dispatcher和 server speed-control的非 callback API调用，全部通过 owner lease/phase；callback内调用也通过 owner注册 send和转换状态。
- [ ] handoff 采用 prepare/publish/commit 或等价事务：relay registration和新 target都成功后才释放旧 target，失败不能留下半提交路由。
- [ ] `PrepareRelay` 创建 inactive relay且不 arm epoll fd/提交 QUIC send；`PublishTarget` 成功后 `CommitRelay` 才激活，并重新校验 owner phase和 route generation。
- [ ] prepare返回内部 token且不写 `TqRelayHandle`/`RelayStarted`；commit成功才发布 public handle，rollback/terminal用 token清理。
- [ ] prepared target提供有界 precommit receive/pending-bytes queue；publish与commit之间不写 TCP或调用其它 stream API。
- [ ] 删除 `DispatchPendingRelayRx()` 对 wrapper callback/context 的读取和人工回调；copied pending bytes进入 precommit queue。
- [ ] publish前失败可恢复旧 target；publish后 activation失败不可回切，必须 request shutdown并清理 precommit ownership。
- [ ] 明确注册成功、注册失败、worker stop 三种情况下 owner 归属，增加析构一次/无泄漏测试。
- [ ] 已启动 stream 的 relay registration failure request shutdown并等待 terminal；只有 StreamStart失败/从未启动才能直接 close。
- [ ] `RequestShutdown` 用 begin/call/commit/fail转换合并并发请求；API failure不释放 retention，terminal并发时 terminal wins。
- [ ] 定义 Linux worker stop target：停止新 handoff、request shutdown、关闭本地 fd、把 router切到不引用 worker的 shutdown sink，再退出线程；不得超时强制 close started owner。

## Task 2: 建立会暴露 UAF 的回归测试

- [ ] 在 `linux_relay_worker_io_test.cpp` 增加 terminal router 测试。注册 relay 后，通过固定 router callback 分派 `SHUTDOWN_COMPLETE`，不要直接调用 `ProcessQuicShutdownComplete()` test helper。
- [ ] router callback 同步返回后、worker event 尚未 drain 时断言 owner phase terminal、binding closing，wrapper callback/context 仍为原 router且 target 已清空。
- [ ] callback 返回后释放 tunnel 侧 owner，通过 `weak_ptr`/析构计数确认 binding 或 API lease 仍保活 wrapper；然后 drain worker event。
- [ ] 断言 worker 能完成 logical detach、snapshot 显示 `StreamDetached`、没有 fake stream API 调用。
- [ ] 在同一失效 wrapper 状态下分派 `EPOLLERR | EPOLLHUP | EPOLLRDHUP`，断言只清理 relay/TCP，并保持 late-error metric。
- [ ] 增加 shutdown complete 前 active TCP hard error 对照，断言 fake `StreamShutdown` 仍恰好调用一次。
- [ ] 增加 API lease 与 terminal callback 交错测试：lease 释放前 wrapper 不析构，terminal 后不能取得新 lease。
- [ ] 先运行 focused test，确认现有实现因 terminal worker detach 读取 wrapper 而失败，或由新增结构性断言失败。
- [ ] 明确记录直接调用 router callback 不经过 private C++ wrapper adapter；该测试必须与 manual owner析构测试组合，不能单独作为验收证据。
- [ ] 增加 tunnel -> relay target handoff 与 terminal 并发测试，断言 terminal 只由一个完整 target处理。
- [ ] 增加 START API 返回前 terminal callback 测试，以及 accepted factory 安装 handler时无半构造对象测试。
- [ ] 注入 accepted owner/router/dispatcher allocation failure，断言 emergency handler只 abort/close一次且无泄漏。
- [ ] 注入 shutdown API failure、重复请求和 terminal-wins，断言只提交一次 abort且 owner不早删。
- [ ] 增加 target detach与正在执行 callback并发测试，旧 target deferred retire且 callback不访问已删除 context/worker。
- [ ] 增加 tunnel send completion晚于 target切换测试，确保原 `TqTunnelSendContext` 不会被 cast成 Linux relay send operation。
- [ ] 在 prepare/publish/commit 每个边界注入 terminal/failure，断言 fd未误 arm、relay不残留且 owner只释放一次。
- [ ] 在 publish/commit间注入 RECEIVE和activation failure，断言 receive ownership有界、只释放一次且旧 target不被错误恢复。
- [ ] 并发 snapshot/stop在 prepare阶段看不到 public active relay；commit后 handle字段一次性一致可见。
- [ ] 覆盖 dispatcher -> speed-control handoff，不发生 wrapper handler写入且 terminal仍发布到同一 owner。

## Task 3: 在 router terminal callback 内发布 owner 终态

- [ ] router callback 调用幂等 `PublishTerminalAndTakeTarget()`，在线性化临界区发布 owner terminal并取走唯一 route snapshot。
- [ ] 在临界区外从 snapshot 保存 relay id、handle 和 worker，release-store `binding->Closing=true` 并清除 binding route capability。
- [ ] router 已为 terminal-empty；不得写 `stream->Callback` / `stream->Context`。
- [ ] shutdown event 仅携带 relay id、error code、status；禁止加入 stream pointer。
- [ ] enqueue 失败时只发布 handle stop 或执行无 wrapper fallback，不得重新激活 binding，也不得调用 `Shutdown()`。
- [ ] enqueue 失败后仍将 owner/binding 接入 stop/reaper 回收路径，不能因 terminal event 丢失而泄漏。
- [ ] 确认 `SEND_COMPLETE` 特殊分支仍能交还已经完成的 send operation，且 terminal event 不会吞掉已排队 completion。
- [ ] send前注册、同步失败unregister；`SEND_COMPLETE` registry claim一次，unknown/duplicate key不得解引用、delete或 cast。

## Task 4: 将 worker cleanup 改为 logical detach

- [ ] `ProcessQuicShutdownComplete()` 设置 `StreamShutdownComplete=true` 后立即将 relay 的 `Stream` / `StreamBinding` 逻辑清空，但不读取 stream 字段。
- [ ] 将 sealed binding 放入 retired binding 容器；释放条件只依赖 callback refs、outstanding operation 和 lifetime owner/lease。
- [ ] 删除 terminal path 中 `DetachRelayStreamBinding(relay, stream, binding)` 形式的调用。
- [ ] 删除或重构 `RetiringStream`。若 pending sends 需要延迟 cleanup，只保留 `RetiringStreamBinding` 或独立 completion owner。
- [ ] `CompletePendingStreamDetach()` 不得从 terminal state 恢复或解引用 stream pointer。
- [ ] 普通 abort/unregister 不再从 worker 跨线程清 callback/context；已启动 stream 发起 shutdown 后保留 binding/owner，等待 terminal callback retire。
- [ ] 只有 StreamStart失败或从未启动的 stream 可以直接 close；relay registration failure 不能据此直接析构已启动 owner。
- [ ] 保持 pending receive discard、TCP close、stop publish、send byte accounting 的现有顺序和一次性语义。
- [ ] 将 pending receive view 中的裸 stream capability 替换为 owner/binding owner；active completion 通过 lease 调用 `ReceiveComplete()`。
- [ ] terminal cleanup 使用独立 discard helper，只释放 view/budget/accounting，不调用 `ReceiveComplete()` 或其它 stream API。

## Task 5: 审计 Linux 全部 stream API 调用点

- [ ] 使用 `rtk rg -n --fixed-strings -- '->Shutdown(' src/tunnel/linux_relay_worker.cpp` 列出 abort 点。
- [ ] 使用 `rtk rg -n "ReceiveComplete|ReceiveSetEnabled|Send\\(" src/tunnel/linux_relay_worker.cpp` 列出其它 API。
- [ ] callback 内调用只使用 router 提供的当前 callback 参数/owner snapshot；worker 使用 binding owner lease。
- [ ] worker 内调用必须由 active relay + active binding capability 保护，并全程持有 lifetime lease。
- [ ] `HasAbortableStream()` 增加/保留 binding terminal 检查；`TcpWriteClosed` 和 `StreamShutdownComplete` 继续返回 false。
- [ ] terminal worker path 和 retired cleanup 不得出现 `stream->`。
- [ ] 审计 `Callback` / `Context` 写入点；只允许 stream 启动前的 router初始化，terminal callback 也不得改写。

## Task 6: pending completion 与 stop/unregister 回归

- [ ] 构造 shutdown complete 到达时至少一个 send-complete event 已入队但尚未消费的测试。
- [ ] 先处理 terminal event再处理 send completion，以及反向顺序各覆盖一次。
- [ ] 断言 operation/buffer 只释放一次，outstanding sends/bytes 最终归零。
- [ ] 覆盖 worker stop 和 `UnregisterRelay()` 与 terminal event 相邻的情况，binding 不提前释放且不访问 wrapper。
- [ ] 覆盖 duplicate/stale terminal event，logical detach/retire 幂等且不重复释放 owner。
- [ ] 覆盖 started owner 的 worker stop/unregister，terminal retention 保证最后一个业务 owner释放后不会提前 `StreamClose`。
- [ ] 覆盖 worker已退出后的 terminal callback，由 shutdown sink完成 owner回收且不投递到失效 worker。
- [ ] 覆盖 API lease 已取得后 terminal 发布的情况，确认 callback 不等待且 wrapper 在 lease 释放后才析构。
- [ ] 保留 fake FIN、receive completion 和 queue-full 原有测试行为。
- [ ] 增加 active receive complete 一次、terminal receive discard 零次 stream API 的对照测试，并检查 pending bytes/budget 归零。

## Task 7: 验证

- [ ] 运行：

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_client_tunnel_open_test
rtk build/bin/Release/tcpquic_speed_test_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk git diff --check
```

- [ ] 在 ASan 构建运行 Linux relay IO test 和 owner handoff/lease test。
- [ ] 运行现有 Linux relay 性能/压力入口或最小 microbenchmark，记录 owner lease 对 worker CPU 和吞吐的影响。
- [ ] snapshot/metrics暴露 terminal-retained owner数量、最老 age和 stop剩余量，压力测试结束为零。
- [ ] 执行 System Test F01-F18 的 Linux矩阵、k6 short-tunnel baseline和30分钟 churn soak，保存规定证据。
- [ ] 静态确认 `RetiringStream` 已移除或 terminal path 永不使用。
- [ ] 更新旧 Linux lifetime 设计文档，注明本计划取代其“worker-time wrapper detach”部分，但保留 late TCP error guard 和指标。

## 验收标准

- relay 注册期间 wrapper 使用 manual cleanup，owner handoff 无双删/泄漏。
- owner phase 是 terminal/close 唯一事实源，started owner 由 terminal retention 保活。
- wrapper callback/context 只初始化一次；terminal callback 发布 owner终态并清 router target，不等待 worker/API lease。
- callback 返回后 worker、late epoll error、send completion、unregister 不通过裸 pointer 解引用 wrapper。
- worker stream API 调用全程持有 lifetime lease。
- worker/retired cleanup和 terminal callback都不改写已启动 stream 的 callback/context。
- pending receive view 不保存无 owner 的 stream capability，terminal discard 不调用 stream API。
- pending send accounting 正确，binding 生命周期覆盖 callback refs/operation owner。
- handoff 前后的 send completion由 operation owner处理，prepared relay只在 target publish后激活。
- active TCP hard error 仍调用一次 stream abort；terminal seal 后调用零次。
- 现有 Linux late TCP error 指标和回归测试继续通过。
- feature system-test release gates通过。
