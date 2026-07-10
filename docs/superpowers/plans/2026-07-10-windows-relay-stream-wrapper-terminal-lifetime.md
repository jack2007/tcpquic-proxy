# Windows Relay Stream Wrapper Terminal Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 Windows relay 在 `SHUTDOWN_COMPLETE` callback 返回后由 IOCP close/maintenance 路径读取自动析构 `MsQuicStream` wrapper 的风险。

**Architecture:** relay-capable stream 使用公共 manual owner和 stable callback router；wrapper callback/context 启动前设置一次。router target 安全切换到 Windows binding。IOCP stream API 调用持有 owner lease；terminal callback 在 owner发布终态并 seal target，不等待 IOCP/API lease。

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, Windows IOCP, MsQuic C++ wrapper, CMake Windows relay tests.

**Prerequisite:** 平台中立 lifetime owner、stable router 与三个 tunnel target handoff 路径已经按 Linux
计划 Task 1（或等价的独立公共变更）实现；本计划只接入 Windows，不复制公共 owner。

---

## File Structure

- Add or Modify: `src/tunnel/stream_lifetime.h` / `.cpp`
  - 复用平台中立 manual owner/phase/terminal retention/router/API lease，不创建 Windows 私有副本。
- Modify: `src/tunnel/relay.h` / `relay.cpp`, `src/tunnel/tcp_tunnel.cpp`
  - 复用 client tunnel、server dispatcher及其 tunnel/speed-control target的 manual owner handoff。
- Modify: `src/tunnel/windows_relay_worker.h`
  - 增加 terminal seal/helper 和 close stream disposition。
  - 增加 test hooks/counters（如需要）。
- Modify: `src/tunnel/windows_relay_worker.cpp`
  - 将现有 `StreamCallback` 改为 router target dispatch，不安装或撤销 wrapper handler。
  - 允许 sealed binding 投递唯一 terminal IOCP operation。
  - terminal `CloseRelay()` 不读取 stream。
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - 增加 wrapper 失效、late IOCP completion、pending operation 和 active close 对照测试。
- Modify as needed: `src/runtime/relay_metrics.*`, `src/docs/thread_model_cn.md`, `docs/relay_win.md`
  - 输出 retention/router/registry指标并更新 Windows IOCP生命周期说明。

## Task 1: 接入公共 lifetime owner

- [ ] Windows relay registration 接收 owner，不再只保存无所有权的 `MsQuicStream*`。
- [ ] `RelayContext` / `CallbackBinding` 持有 owner和 route target adapter；API helper 返回持有 owner 的 lease。
- [ ] owner phase 是 terminal 唯一事实源；started owner由 terminal retention 保活，terminal 后拒绝新 lease。
- [ ] 复用公共 outgoing/accepted factory 和 `Starting` 状态；不得在 Windows registration 中重新包装 raw stream。
- [ ] owner/lease 不授权 IOCP worker 跨线程写已启动 stream 的 callback/context。
- [ ] 明确注册失败、IOCP post 失败、worker stop、runtime stop 时 owner 的唯一回收路径。
- [ ] client tunnel和 server accepted wrapper从创建时使用 manual cleanup；禁止在 terminal callback 内转换，因为 adapter 已提前缓存 `DeleteOnExit`。
- [ ] 验证同步/异步 client open 与 server incoming dispatcher 均传递同一公共 owner，而不是重新 adopt 裸 pointer。
- [ ] Windows registration 只发布 router target；采用 prepare/publish/commit 或等价事务避免 registration failure 的半提交路由。
- [ ] Windows target adapter不保存无保护的 `RelayContext*` / worker pointer；callback snapshot使用安全 owner/ref guard和deferred retire。
- [ ] target publish 与 terminal take共享公共线性化协议；terminal 后 Windows target publish必须失败。
- [ ] Windows relay prepare保持 IOCP/TCP数据面 inactive；target publish成功后 commit才 post receive/maintenance，并重新校验 owner phase/generation。
- [ ] prepare只返回内部 token，不写 public handle/`RelayStarted`；commit后一次性发布，rollback/terminal按 token清理。
- [ ] prepared target用有界 precommit queue接收 copied bytes和publish/commit间 RECEIVE；不 post TCP send/receive。
- [ ] publish前失败可 rollback，publish后 activation失败 request shutdown并清理 queue，不恢复旧 target。
- [ ] 接入公共 owner级 send completion registry；router把 context仅作为 opaque key claim typed record，不按当前 Windows target cast。
- [ ] 已启动 stream registration failure request shutdown并等待 terminal；仅 StreamStart失败/从未启动可直接 close。
- [ ] 复用公共 shutdown begin/call/commit/fail状态机；API failure不释放 retention，并发 shutdown只提交一次。
- [ ] runtime stop在 IOCP worker退出前把 route切到不引用 worker的 shutdown sink；sink/owner registry等待 terminal，不以 timeout强制 close。

## Task 2: 增加 terminal wrapper 失效测试

- [ ] 注册 Windows relay，通过固定 router callback 投递真实 `QuicShutdownComplete` IOCP operation。
- [ ] router callback 返回后、terminal IOCP operation 尚未处理时断言 owner terminal、binding closing、`RelayHint=null`、wrapper handler仍为 router且 target 已清空。
- [ ] callback 返回后释放 tunnel 侧 owner，通过 weak owner/析构计数确认 wrapper 由 binding/lease 正确保活。
- [ ] 驱动 IOCP worker处理 terminal operation，断言 active relay 最终退役且无访问异常。
- [ ] 在 terminal operation 前后各注入一个 TCP send/recv teardown completion，验证 completion 被 downgrade/cleanup。
- [ ] 增加 queued send completion 与 terminal operation 的两种顺序测试，断言 operation 只释放一次。
- [ ] 增加 lease/terminal 交错测试；terminal callback 必须立即返回，lease 释放后 wrapper 才析构。
- [ ] 直接调用 router callback 的测试必须与 manual cleanup/owner析构计数组合；它本身不经过 private wrapper adapter。
- [ ] 增加 tunnel -> Windows target handoff 与 terminal 并发测试，terminal只能由一个完整 target处理。
- [ ] 复用 START 返回前 terminal 与 accepted factory 半构造防护测试。
- [ ] 在 Windows注入 accepted owner/router allocation failure，验证 emergency C callback在 terminal后 close一次。
- [ ] 复用 shutdown API failure、重复请求和 terminal-wins测试。
- [ ] 增加 target detach与 callback并发，以及 IOCP worker退出后 terminal sink回收测试。
- [ ] 增加 tunnel send completion晚于 Windows target切换测试，以及 prepare/publish/commit边界 terminal注入。

## Task 3: 实现 callback-time seal

- [ ] router callback 使用 `PublishTerminalAndTakeTarget()` 原子发布 owner terminal并取走唯一 Windows target snapshot。
- [ ] terminal operation 的 post helper 不得因为即将设置的 `binding->Closing` 而拒绝该唯一事件；可新增专用 `PostTerminalCallbackOperationById()`，避免放宽普通 callback post。
- [ ] 成功取得投递所需数据后 release-store `Closing=true`、清除 `RelayHint` 并把 router target切为空 terminal target。
- [ ] 不得修改 `stream->Callback` / `stream->Context`。
- [ ] 投递 terminal IOCP operation；operation 不保存 stream pointer。
- [ ] post 失败时增加已有 callback-post failure 指标，并发布只操作 relay/handle 的 fallback close 请求；不得访问或 shutdown stream。
- [ ] post 失败必须保留一条 owner/binding 回收路径，不能只置 `Closing` 后遗留 active map entry。

## Task 4: 区分 active close 与 terminal close

- [ ] 为 `CloseRelay()` 增加明确的 stream disposition，例如 `ActiveShutdown` / `TerminalLogicalDetach`，不要依赖 reason 字符串推断。
- [ ] `ProcessQuicShutdownComplete()` 总是使用 `TerminalLogicalDetach`。
- [ ] terminal close 允许设置 `relay->Stream=nullptr`，但禁止检查 `relay->Stream->Context` 或修改 wrapper callback/context。
- [ ] active TCP error、admin stop、worker stop 等路径发起 active shutdown 并等待 terminal；stream API 调用必须持有 owner lease，且不得跨线程清 callback/context。
- [ ] 已启动 stream 不得 abort 后立即析构 owner；只有 StreamStart失败或从未启动才可直接 close。
- [ ] `TryRetireRelay()` 不得在 terminal logical detach 后再次尝试清 wrapper callback/context。
- [ ] `RetiredCallbacks_` 只延长 binding 生命周期，不保存 wrapper capability。

## Task 5: 审计 IOCP queued work

- [ ] 检查两个 IOCP dispatch 分支中 `QuicShutdownComplete` 的 relay resolve/generation 校验一致。
- [ ] duplicate/stale terminal IOCP operation 必须幂等丢弃，不能关闭新的 relay generation 或重复释放 owner。
- [ ] sealed binding 后普通 RECEIVE/IDEAL/ABORT callback 不再投递；SEND_COMPLETE 已完成事件仍按 operation owner 交还。
- [ ] unknown/duplicate send key不解引用并安全诊断；send同步失败正确unregister，不 reinterpret为 `TqWindowsQuicSendOperation`。
- [ ] `ShouldDowngradeTcpRecvCompletion()` / `ShouldDowngradeTcpSendCompletion()` 保持 terminal close 后降级行为。
- [ ] maintenance queue、receive drain、retry send 在 relay terminal/closing 后不得调用 stream API。
- [ ] `TqWindowsPendingQuicReceive` 不保存无 owner 的 stream capability；active completion 通过 lease，terminal cleanup 只释放本地 receive ownership/accounting。
- [ ] 使用 `rtk rg -n "Stream->|stream->" src/tunnel/windows_relay_worker.cpp` 人工分类所有 wrapper 访问点。
- [ ] 分类结果中所有非 callback wrapper 访问必须能追溯到 lifetime lease，不能只有原子状态检查。
- [ ] 审计 `Callback` / `Context` 写入点；只允许启动前 router初始化，terminal callback 也不得写入。
- [ ] worker stop/runtime stop 测试证明 terminal retention保留 started owner，直到 terminal router callback 或连接 teardown完成。
- [ ] snapshot/metrics暴露 terminal-retained owner数量、最老 age和 stop剩余量，压力测试结束为零。

## Task 6: 验证

- [ ] 在 Windows 运行：

```powershell
rtk cmake --build build --target tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test.exe
rtk build/bin/Release/tcpquic_windows_reactor_test.exe
rtk build/bin/Release/tcpquic_tcp_tunnel_test.exe
rtk build/bin/Release/tcpquic_client_tunnel_open_test.exe
rtk build/bin/Release/tcpquic_speed_test_test.exe
rtk build/bin/Release/tcpquic_router_runtime_test.exe
rtk git diff --check
```

- [ ] 使用 ASan for Windows 或 Application Verifier page heap 运行 owner/terminal lifetime case；保护页只用于独占 page 的专用测试。
- [ ] 运行 Windows relay 性能/压力入口，记录 lifetime lease 对 IOCP worker CPU 和吞吐的影响。
- [ ] 确认 terminal worker operation、`CloseRelay()`、`TryRetireRelay()` 路径不存在 wrapper 字段读取。
- [ ] 保留 active TCP hard-error reset、IOCP teardown downgrade 和 callback metrics 原有断言。
- [ ] 增加 active `ReceiveComplete` 一次与 terminal receive discard 零次 stream API 的对照测试。
- [ ] 执行 System Test Windows矩阵、Application Verifier、k6 baseline和worker-stop恢复场景。

## 验收标准

- relay 使用 manual cleanup，registration handoff 无双删/泄漏。
- owner phase 是 terminal/close 唯一事实源，started owner 不会被 runtime stop提前析构。
- `SHUTDOWN_COMPLETE` callback 发布 owner终态、seal target且不等待 IOCP/API lease；wrapper handler保持稳定。
- terminal IOCP operation 不携带 stream pointer。
- terminal close/retire 不读取 `relay->Stream->Context`。
- late TCP completion 和 pending send completion 正确退役且不访问 wrapper。
- 非 callback stream API 调用全程持有 lifetime lease。
- wrapper callback/context 只初始化一次；IOCP worker/terminal/maintenance/retire均不改写。
- pending receive terminal discard 不调用 stream API，receive budget/accounting 正确归零。
- active close 行为、IOCP generation 校验和维护队列语义不回归。
- prepared relay只在 target publish后激活，handoff前后的 send completion由 operation owner处理。
- feature system-test release gates通过。
