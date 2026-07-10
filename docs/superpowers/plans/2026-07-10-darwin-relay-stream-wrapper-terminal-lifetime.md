# Darwin Relay Stream Wrapper Terminal Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 Darwin relay 在 terminal callback 返回后由 kqueue worker、retired relay 或 send completion cleanup 读取自动析构 `MsQuicStream` wrapper 的风险。

**Architecture:** relay-capable stream 使用公共 manual owner和 stable callback router；wrapper callback/context 启动前设置一次。router target 安全切换到 Darwin binding。kqueue stream API 调用持有 owner lease；terminal callback 在 owner发布终态并 seal target，不等待 worker/API lease；retire 不再依赖 `RetiredStream`。

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, Darwin kqueue, MsQuic C++ wrapper, shared/weak binding ownership, CMake Darwin relay tests.

**Prerequisite:** 平台中立 lifetime owner、stable router 与三个 tunnel target handoff 路径已经按 Linux
计划 Task 1（或等价的独立公共变更）实现；本计划只接入 Darwin，不复制公共 owner。

---

## File Structure

- Add or Modify: `src/tunnel/stream_lifetime.h` / `.cpp`
  - 复用平台中立 manual owner/phase/terminal retention/router/API lease，不创建 Darwin 私有副本。
- Modify: `src/tunnel/relay.h` / `relay.cpp`, `src/tunnel/tcp_tunnel.cpp`
  - 复用 client tunnel、server dispatcher及其 tunnel/speed-control target的 manual owner handoff。
- Modify: `src/tunnel/darwin_relay_worker.h`
  - 增加 terminal close disposition/helper 和测试接口。
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - 将现有 `StreamCallback` 改为 router target dispatch，不安装或撤销 wrapper handler。
  - router callback 内发布 owner terminal并 seal target。
  - terminal retire 不读取 stream。
  - 从 terminal send-completion cleanup 移除 `RetiredStream` 依赖。
- Modify: `src/tunnel/darwin_relay_event_queue.h`
  - 如需增加 terminal 标志，只保存逻辑 owner/payload，不保存 stream pointer。
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - 增加 wrapper 失效、pending send、late kqueue event 和 fallback close 测试。
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
  - 覆盖 terminal event queue full/stop purge ownership（如适合）。
- Modify as needed: `src/runtime/relay_metrics.*`, `src/docs/thread_model_cn.md`, `docs/relay_macos.md`
  - 输出 retention/router/registry指标并更新 Darwin kqueue生命周期说明。

## Task 1: 接入公共 lifetime owner

- [ ] Darwin registration 接收 owner，`RelayState` / `StreamBinding` 持有 owner，不再以裸 pointer 表达所有权。
- [ ] `RelayState` / `StreamBinding` 持有 owner和 route target adapter；所有非 callback API 通过 lease。
- [ ] owner phase 是 terminal 唯一事实源；started owner由 terminal retention保活，terminal后拒绝新 lease。
- [ ] 复用公共 outgoing/accepted factory 和 `Starting` 状态；不得在 Darwin registration 中重新包装 raw stream。
- [ ] terminal callback 不等待 lease，不获取 worker 调用 MsQuic 时可能持有的锁。
- [ ] owner/lease 不授权 kqueue worker 或 retired cleanup 跨线程写已启动 stream 的 callback/context。
- [ ] 明确 register failure、queue fallback、worker stop 和 retired purge 的 owner 回收路径。
- [ ] client tunnel和 server accepted wrapper从创建时使用 manual cleanup；禁止在 terminal callback 内转换，因为 adapter 已提前缓存 `DeleteOnExit`。
- [ ] 验证同步/异步 client open 与 server incoming dispatcher 均传递同一公共 owner，而不是重新 adopt 裸 pointer。
- [ ] Darwin registration 只发布 router target；采用 prepare/publish/commit 或等价事务避免 registration failure 的半提交路由。
- [ ] Darwin target adapter不保存无保护的 `RelayState*` / worker pointer；callback snapshot使用安全 owner/ref guard和deferred retire。
- [ ] target publish 与 terminal take共享公共线性化协议；terminal 后 Darwin target publish必须失败。
- [ ] Darwin relay prepare保持 kqueue/TCP数据面 inactive；target publish成功后 commit才安装filter/启动数据面，并重新校验 owner phase/generation。
- [ ] prepare只返回内部 token，不写 public handle/`RelayStarted`；commit后一次性发布，rollback/terminal按 token清理。
- [ ] prepared target用有界 precommit queue接收 copied bytes和publish/commit间 RECEIVE；不安装filter或写 TCP。
- [ ] publish前失败可 rollback，publish后 activation失败 request shutdown并清理 queue，不恢复旧 target。
- [ ] 将现有 operation lookup接到公共 owner级 registry语义；router把 context仅作为 opaque key claim typed record，不按当前 Darwin target cast。
- [ ] 已启动 stream registration failure request shutdown并等待 terminal；仅 StreamStart失败/从未启动可直接 close。
- [ ] 复用公共 shutdown begin/call/commit/fail状态机；API failure不释放 retention，并发 shutdown只提交一次。
- [ ] worker stop在 kqueue thread退出前把 route切到不引用 worker的 shutdown sink；sink/owner registry等待 terminal，不以 timeout强制 close。

## Task 2: 增加 terminal wrapper 失效测试

- [ ] 注册 relay 后通过固定 router callback 分派 `SHUTDOWN_COMPLETE`。
- [ ] router callback 返回后、terminal event 尚未 drain 时断言 owner terminal、`binding->Active=false`、wrapper handler仍为 router且 target 已清空。
- [ ] callback 返回后释放 tunnel 侧 owner，通过 weak owner/析构计数确认 binding/lease 正确保活 wrapper。
- [ ] 驱动 event queue/kqueue worker执行 `CloseRelay()`、`RetireRelay()` 和 retired purge，断言不访问 wrapper。
- [ ] terminal event 前后注入 `EV_ERROR` 或 TCP read/write failure，断言只执行一次本地 close。
- [ ] 构造 known send operation 尚未由 worker消费的情况，确认 completion state 能在无 stream 下完成。
- [ ] 增加 lease/terminal 交错测试，确认 callback 不等待、terminal 后无新 lease、最后 lease 释放后只析构一次。
- [ ] 直接调用 router callback 的测试必须与 manual cleanup/owner析构计数组合；它本身不经过 private wrapper adapter。
- [ ] 增加 tunnel -> Darwin target handoff 与 terminal 并发测试，terminal只能由一个完整 target处理。
- [ ] 复用 START 返回前 terminal 与 accepted factory 半构造防护测试。
- [ ] 在 Darwin注入 accepted owner/router allocation failure，验证 emergency C callback在 terminal后 close一次。
- [ ] 复用 shutdown API failure、重复请求和 terminal-wins测试。
- [ ] 增加 target detach与 callback并发，以及 kqueue worker退出后 terminal sink回收测试。
- [ ] 增加 tunnel send completion晚于 Darwin target切换测试，以及 prepare/publish/commit边界 terminal注入。

## Task 3: 完成 callback-time terminal seal

- [ ] 在 shutdown/peer-abort callback 分支区分真正 `SHUTDOWN_COMPLETE` 与非 terminal peer abort；只有 shutdown complete 采用 wrapper terminal 语义。
- [ ] router callback 使用 `PublishTerminalAndTakeTarget()` 原子发布 owner terminal并取走唯一 Darwin target snapshot。
- [ ] release-store `binding->Active=false`，必要时增加 `binding->Terminal=true`，避免普通 unregister 与 auto-delete terminal 混淆。
- [ ] router target切为空 terminal target；不得修改 `stream->Callback` / `stream->Context`。
- [ ] `EnqueueRelayCloseFromCallback()` 继续携带 `shared_ptr<RelayState>` owner，但不得携带 stream。
- [ ] queue full、worker stopped fallback close 必须传入 terminal disposition，不得把 stream 保存到 retired state。
- [ ] fallback 必须保留 owner/binding 回收路径，不能在 callback 中同步等待 worker close。

## Task 4: 重写 terminal retire

- [ ] 为 `CloseRelay()` / `RetireRelay()` 增加明确的 `TerminalLogicalDetach` disposition。
- [ ] terminal retire 直接清空 `relay->Binding` 和 `relay->Stream`，禁止读取 `Stream->Context`。
- [ ] terminal path 不设置 `binding->RetiredStream`。
- [ ] `RetiredRelays` / `RetiredStreamBindings` 继续延长 relay/binding 生命周期，释放条件只依赖 callback refs、known send operations 和 receive owners。
- [ ] `ClearRetiredStreamCallbackIfSafe()` 不得由 terminal completion path 调用；逐项迁移调用者，能够删除 `RetiredStream` 时直接删除。
- [ ] 彻底删除 `RetiredStream` 和跨线程 `ClearRetiredStreamCallbackIfSafe()`；stable router不再需要该兼容路径。
- [ ] 普通 close 采用 active shutdown并等待 terminal，不以 `CallbackRefs==0` 瞬时值证明 wrapper handler可写。
- [ ] 已启动 stream 不得 abort 后立即析构 owner；只有 StreamStart失败或从未启动才可直接 close。

## Task 5: send completion 与 callback 并发审计

- [ ] `KnownSendOperationInfo::BindingOwner` 和 `CompletionState` 足以完成 terminal 后 send accounting，不得回读 stream。
- [ ] tunnel send与 Darwin relay send都通过类型安全 operation owner完成；unknown/duplicate context不得盲目 cast。
- [ ] `CompleteDetachedQuicSend()` 只 unregister operation、更新计数并 delete operation。
- [ ] callback refs guard 覆盖 callback-time seal；retired binding 在 refs 清零前不释放。
- [ ] RECEIVE callback 在 `Active=false` 后立即返回，不再调用 receive API。
- [ ] `SetQuicReceiveEnabled()`、retry send、flush callback-pending receive 在 terminal binding 后不执行 stream API。
- [ ] `TqDarwinPendingQuicReceive` 不保存无 owner 的 stream capability；active completion 通过 lease，terminal cleanup 只释放本地 receive ownership/accounting。
- [ ] 所有非 callback wrapper 访问除状态检查外还必须持有 lifetime lease。
- [ ] 审计 `Callback` / `Context` 写入点；只允许启动前 router初始化，terminal callback 也不得写入。

## Task 6: 回归普通 close 和 peer abort

- [ ] peer send/receive aborted 不一定意味着 wrapper 已自动删除，不能错误套用 shutdown-complete 的“wrapper 已失效”事实。
- [ ] 为 peer abort event 保留 active close 所需语义，但所有 stream API 调用仍要求 active binding capability。
- [ ] 覆盖 unregister、worker stop、queue purge 和 callback fallback，确保锁顺序与现有 state-lock 设计一致。
- [ ] late kqueue event 找不到 active relay时应直接丢弃，不从 retired relay 恢复 stream capability。
- [ ] duplicate/stale terminal event 必须幂等丢弃，不重复 retire owner；event 自带 `RelayOwner` 必须与 active map entry 一致。
- [ ] worker stop/queue purge 测试证明 terminal retention保留 started owner，直到 terminal router callback 或连接 teardown完成。
- [ ] snapshot/metrics暴露 terminal-retained owner数量、最老 age和 stop剩余量，压力测试结束为零。

## Task 7: 验证

- [ ] 在 macOS 运行：

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test -j$(sysctl -n hw.ncpu)
rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_queue_test
rtk build/bin/Release/tcpquic_darwin_relay_metrics_test
rtk build/bin/Release/tcpquic_darwin_reactor_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_client_tunnel_open_test
rtk build/bin/Release/tcpquic_speed_test_test
rtk git diff --check
```

- [ ] 在 ASan 构建运行 owner handoff、terminal lifetime 和 pending send completion cases。
- [ ] 运行 Darwin relay 性能/压力入口，记录 lifetime lease 对 kqueue worker CPU 和吞吐的影响。
- [ ] 使用 `rtk rg -n "RetiredStream|ClearRetiredStreamCallbackIfSafe|Stream->Context" src/tunnel/darwin_relay_worker.cpp` 确认 terminal path 已无 wrapper 访问。
- [ ] 保持现有 callback queue-full backpressure、state/map lock 和 send completion 测试通过。
- [ ] 增加 active `ReceiveComplete` 一次与 terminal receive discard 零次 stream API 的对照测试。
- [ ] 执行 System Test Darwin矩阵、ASan、k6 baseline和worker-stop恢复场景。

## 验收标准

- relay 使用 manual cleanup，registration handoff 无双删/泄漏。
- owner phase 是 terminal/close 唯一事实源，started owner不被 worker stop提前析构。
- terminal callback 发布 owner终态、seal target且不等待 worker/API lease；wrapper handler保持稳定。
- terminal event、fallback close、retire、purge、send completion 不读取 wrapper。
- pending send operation 仅依赖 completion/binding owner并只释放一次。
- 非 callback stream API 调用全程持有 lifetime lease。
- wrapper callback/context 只初始化一次；kqueue worker/terminal/retired cleanup均不改写。
- pending receive terminal discard 不调用 stream API，receive budget/accounting 正确归零。
- late kqueue event 不能恢复 terminal stream capability。
- peer abort、普通 unregister、worker stop 的现有语义和锁边界不回归。
- prepared relay只在 target publish后激活，handoff前后的 send completion由 operation owner处理。
- feature system-test release gates通过。
