# Relay stream wrapper terminal 生命周期设计

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

## 背景

`MsQuicStream` 不是 relay worker 拥有的对象。生产 tunnel 创建 stream 时使用
`CleanUpAutoDelete`，而 MsQuic C++ wrapper 在处理
`QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 时执行以下顺序：

```text
计算 DeleteOnExit
  -> 调用应用 StreamCallback(MsQuicStream*, Context, Event)
  -> callback 返回
  -> delete MsQuicStream
```

因此，在**当前 `CleanUpAutoDelete` 实现中**，`SHUTDOWN_COMPLETE` callback 是应用
最后一次可以合法访问该
`MsQuicStream` wrapper 的执行区间。callback 返回以后，即使 worker 中保存的
`MsQuicStream*` 非空、其 `Handle` 看起来非空，也不能再读取 wrapper 字段或调用
任何 stream API。

`docs/superpowers/specs/2026-07-09-linux-relay-stream-lifetime-design.md`
修复了生产 core 中已经确认的 Linux 二次 abort 路径，但其 detach 动作仍由 worker
在消费异步 shutdown-complete 事件时执行。这个时点晚于 wrapper 自动析构，不能作为
跨平台最终生命周期契约。

当前三端均存在 callback 到 worker 的异步边界：

- Linux：MsQuic callback 投递 `TqLinuxRelayEvent`，worker 与 epoll event 一起消费。
- Windows：MsQuic callback 投递 IOCP operation，worker 随后处理 completion。
- Darwin：MsQuic callback 投递 `TqDarwinRelayEvent`，worker 与 kqueue event 一起消费。

epoll、IOCP、kqueue 都允许已经产生的 TCP event/completion 晚于 QUIC terminal
callback 被处理。平台 API 不提供 stream wrapper 生命周期保护。

本设计依据的本地 MsQuic 契约包括：

- `third_party/msquic/src/inc/msquic.hpp`：auto-delete wrapper 在 terminal callback
  返回后执行 `delete pThis`。
- `third_party/msquic/docs/Execution.md`：同一 stream 不会并行 upcall，但 callback 不应
  阻塞；callback 内 down call 可能优先于其它线程正在进行或排队的 down call。
- `third_party/msquic/docs/api/SetCallbackHandler.md`：callback/context 没有内部同步，
  跨线程修改必须由应用提供同步。
- `third_party/msquic/docs/Streams.md` 与 `api/StreamStart.md`：本地 stream 的 start 会产生
  `START_COMPLETE`；start 失败不会再产生 `SHUTDOWN_COMPLETE`，因此 start-failure 是独立的
  可关闭终态，不能等待一个永远不会到来的 terminal callback。

## 已确认问题与潜在问题

### Linux 已确认崩溃

旧 Linux TCP hard-error 路径会执行：

```text
late EPOLLERR / SO_ERROR
  -> AbortRelayAndRelease(..., abortStream=true)
  -> relay->Stream->Shutdown(...)
  -> stale wrapper / stale HQUIC
```

现有 `HasAbortableStream()` 和 `TcpWriteClosed` guard 阻止了现场中的二次 abort，
但 shutdown-complete worker 路径仍可能通过 `DetachRelayStreamBinding()` 或
`RetiringStream` 读取已经自动删除的 wrapper。

### Windows 潜在 UAF

Windows worker 不在 TCP hard-error path 调用 `Stream::Shutdown()`，所以不存在与
Linux core 完全相同的调用链。但是 `ProcessQuicShutdownComplete()` 最终进入
`CloseRelay()`，后者可能读取 `relay->Stream->Context`。该 IOCP operation 在
terminal callback 返回后执行，此时 wrapper 已经可能被删除。

### Darwin 潜在 UAF

Darwin callback 已经在 terminal event 中将 `binding->Active` 置为 false，方向正确；
但随后执行的 `CloseRelay()` / `RetireRelay()` / `ClearRetiredStreamCallbackIfSafe()`
仍可能读取 `relay->Stream->Context` 或 `binding->RetiredStream` 指向的 wrapper。

因此本设计不声称 Windows/Darwin 已发生生产事故，但将三端统一视为同一类裸指针
生命周期风险。

### 原方案评审发现的 TOCTOU

仅在 terminal callback 中设置原子 `Closing/Active/Terminal`，仍不足以保证 wrapper
安全。以下时序仍然成立：

```text
relay worker: 检查 binding active
relay worker: 准备调用 stream API
MsQuic thread: terminal callback 设置 binding terminal
MsQuic thread: callback 返回，CleanUpAutoDelete 删除 wrapper
relay worker: 解引用刚才检查过的 stream pointer
```

这是 check-then-use 竞态。增加更多状态检查只能缩小窗口，不能关闭窗口。

也不能让 terminal callback 获取一个被 worker 持有、且 worker 持锁调用 MsQuic API 的
互斥锁。MsQuic 要求 callback 不得等待由另一个正在调用 MsQuic 的线程持有的锁，否则
可能死锁。让 callback 自旋等待 `ApiRefs == 0` 具有相同问题，并会阻塞 MsQuic execution
thread。

因此，本设计采用的完整方案不是“`CleanUpAutoDelete` + callback seal”，而是：

1. relay-capable stream 从创建时使用 `CleanUpManual`。
2. 由一个引用计数 lifetime owner 持有 wrapper 和稳定 callback router。
3. wrapper 的 callback/context 在启动前设置一次，之后不再改写；handoff 只切换 router target。
4. 每次非 callback stream API 调用持有一份 owner lease。
5. terminal callback 先取得强 callback guard，再在 owner 中发布 terminal、解除 terminal
   retention 并投递 cleanup；它不等待 lease 排空。
6. owner 处于可关闭 phase，且最后一份 callback/target/API/operation 强引用释放后，才 delete
   wrapper。

callback-time seal 仍然必要，但它负责阻止新 lease，不负责单独保证对象存活。

## 核心安全不变量

### 不变量 1：relay 期间禁止使用 `CleanUpAutoDelete`

stream 启动前必须完成显式所有权初始化：

- wrapper 创建时就是 `CleanUpManual`；不在运行中转换 cleanup mode。
- wrapper 进入引用计数的 `TqStreamLifetime`（暂定名）。
- wrapper callback/context 指向 lifetime 内稳定的 callback router。
- tunnel/dispatcher 和 relay 对“谁负责最终 delete”有唯一、可测试的交接结果。
- 注册失败也必须有明确 owner；不能恢复为无人约束的裸指针。

`SHUTDOWN_COMPLETE` callback 返回后，wrapper 不再被 C++ adapter 自动删除，但其
stream API capability 已 terminal。worker 处理 terminal event 时不得读取
`Handle/Context/Callback` 或调用 stream API；仅允许已经在 terminal 发布前成功取得的
API lease 完成其在途调用。

最终 delete wrapper 必须同时满足：

- owner phase 为 `CreatedNotStarted`、`StartFailed` 或 `TerminalPublished`；
- started-stream terminal retention 已解除；
- 所有当前 router callback guard/ref（包括 start/terminal/send completion）已释放；
- 所有已取得的 API lease 释放；
- 所有已成功提交、尚未收到 `SEND_COMPLETE` 的 send completion retention 已解除；
- 所有仍强持 owner 的 target、binding 或 claimed operation 已完成或 detach。

不访问 wrapper 的 terminal worker event 或 completion payload 不必为了 wrapper 析构而完成，
但它们也不得持有裸 wrapper capability。若它们选择强持 owner，引用计数会自然推迟析构。
owner、router、registry record、binding 之间禁止形成强引用环；析构不是打破环的机制。

这个约束同样适用于为了等待 send-complete accounting 而保留的 retired state：可以
保留 binding、relay id、generation、send operation 和 buffer owner，但不能保留一个
无 owner、未来还会被解引用的 retired stream pointer。

### 不变量 2：terminal phase 由 lifetime owner 唯一表示

terminal 不能只记录在 Linux/Windows/Darwin binding 中，因为 callback target 会经历
dispatcher -> tunnel -> relay handoff。公共 lifetime owner 只表达 wrapper 生命周期，不把
QUIC 双向 half-close 混入同一个 phase：

```text
CreatedNotStarted -> Starting
Starting -- START_COMPLETE(success) --> Started
Starting -- START_COMPLETE(failure) --> StartFailed
Starting | Started -- SHUTDOWN_COMPLETE --> TerminalPublished
CreatedNotStarted | StartFailed | TerminalPublished --> Closed
```

- `StreamStart` 或含 START 的首次 Send 调用前进入 `Starting`，避免 API 返回前已经发生的
  callback 看到错误的未启动状态。
- `START_COMPLETE` 是已提交 start 的成功/失败事实源。API 同步返回本身不能覆盖 callback
  已发布的状态；只有 API 在提交前同步拒绝、且 MsQuic 契约保证不会再回调时，调用线程才可
  调用 `PublishStartFailureAndTakeTarget()`。在下述 managed-factory flag约束下，失败后不会等待
  `SHUTDOWN_COMPLETE`。
- start 成功 callback 可在 API 返回前把 `Starting` 推进到 `Started`，并可继续重入到
  `TerminalPublished`；API 返回路径只能做幂等确认，不能把 terminal 状态改回 active。
- start failure通过 `PublishStartFailureAndTakeTarget()`（暂定名）在同一 control协议中进入
  `StartFailed`、关闭 API/route admission并取走当前业务 target；在锁外通知 open/factory失败，
  再解除 start retention。它不发布 terminal；已注册 `SEND(START)` 的 `SEND_COMPLETE`仍由
  owner registry特殊分支处理，不依赖已清空的 current target。
- accepted raw stream owner 从 factory 返回前即处于 `Started` 并启用 terminal retention。
- 无论当前 target 是 dispatcher、tunnel 还是 relay，`SHUTDOWN_COMPLETE` 都在同一 owner
  上 release-publish `TerminalPublished`。
- 只有 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 发布 owner terminal；peer send/receive abort、
  send-shutdown-complete、TCP half-close都不是 wrapper终态，只能触发 shutdown请求或本地状态。
- owner 只有在 `CreatedNotStarted`、`StartFailed` 或 `TerminalPublished` 时才允许进入
  `Closed` 并 close/delete wrapper。
- relay registration failure 不等于 stream 未启动。对已启动 tunnel stream，注册失败必须
  request shutdown 并等待 terminal，不能直接析构 owner。

本状态模型要求 managed outgoing factory禁止
`QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL`。本地 MsQuic会在该 flag的 start failure后继续交付
`SHUTDOWN_COMPLETE`，与“`StartFailed` 可直接 Closed”契约不同；当前生产使用
`SEND(QUIC_SEND_FLAG_START)`/默认 start flags，不需要该 flag。若未来必须支持，应增加
`StartFailedAwaitingTerminal`（或允许 `StartFailed -> TerminalPublished`并保留 terminal
retention），不能复用本设计的直接 close路径。

shutdown 请求由 owner 内独立的方向性 ledger 表示，而不是 owner phase：

```text
local send:    Open -> GracefulRequested -> SendComplete
               Open | GracefulRequested -> AbortRequested -> SendComplete
local receive: Open -----------------------> AbortRequested
peer send:     Open -> GracefulEof | Aborted
peer receive:  Open -> Aborted
```

- `RequestShutdown(intent)` 在 owner control 临界区内合并 desired intent、为尚未提交的方向
  预留一次 down call 和 API lease，再在锁外调用 MsQuic。`ABORT_SEND` 覆盖同方向的
  `GRACEFUL`，`ABORT_RECEIVE` 独立合并，`IMMEDIATE` 只能伴随 abort。
- 同一方向、同一强度的并发请求只提交一次；graceful send 之后允许因 hard error 升级为
  abort send，另一方向也可独立 abort。这里允许必要的方向升级，禁止的是重复提交相同 abort。
- down call 失败时清除对应 submitted reservation、保留 desired intent 和错误，供显式策略
  重试；不得解除 terminal retention。terminal 并发发布后不再开始重试，已预留 lease 可以
  返回并释放。
- 带 `QUIC_SEND_FLAG_FIN` 的 send 成功提交后，ledger 记录 graceful send 已请求；其失败、
  `SEND_COMPLETE(Canceled=true)` 和 `SEND_SHUTDOWN_COMPLETE` 分别更新提交、buffer ownership
  和方向完成事实，均不发布 wrapper terminal。

事件和 half-close/cancel 语义必须按下表实现：

| 输入 | 方向语义与允许动作 | 发布 owner terminal |
| --- | --- | --- |
| TCP EOF | TCP peer 不再发送；排空 TCP->QUIC 数据后提交一次 QUIC FIN，反方向继续 | 否 |
| QUIC `RECEIVE(FIN)` / `PEER_SEND_SHUTDOWN` | QUIC peer graceful EOF；已接受 receive 排空后对 TCP 执行 `SHUT_WR`，TCP->QUIC 可继续 | 否 |
| `SEND_SHUTDOWN_COMPLETE` | 本地 QUIC send 方向完成；只更新方向状态和 fully-closed 判定 | 否 |
| `PEER_SEND_ABORTED` | peer abort 其 send；终止 QUIC->TCP 方向、一次性回收 pending receive，并按 relay policy 请求本地 abort | 否 |
| `PEER_RECEIVE_ABORTED` | peer 不再接收本地 send；停止/取消 TCP->QUIC 方向，send buffer 仍由 `SEND_COMPLETE` 逐个归还 | 否 |
| TCP hard error、admin/worker stop、业务 cancel | 通过 ledger 请求相应方向或双向 abort，保留 owner 等待 terminal | 否 |
| `SEND_COMPLETE(Canceled=true)` | 只归还该 send operation/buffer 并更新 accounting | 否 |
| `CANCEL_ON_LOSS` | callback 内同步填写稳定的应用 error code并记录 cancel；可请求 abort，但不 seal router | 否 |
| `SHUTDOWN_COMPLETE`（含 connection shutdown） | wrapper 唯一终态；seal lease admission/router，后续只做逻辑 cleanup | **是** |

各平台 binding 的 `Closing/Active/TerminalSeen` 只表示路由和 relay 本地状态，不能作为
wrapper 最终析构的唯一依据。

### 不变量 3：active stream capability 由 owner 与 binding 共同表示

非空裸指针不是 capability。非 terminal 路径必须通过 lifetime owner 获取 lease；只有
同时满足以下条件才允许开始新的 stream API 调用：

```text
relay active
AND binding active
AND binding 仍指向同一 relay/generation
AND owner phase 允许该类 API
AND 成功取得 TqStreamLifetime lease
```

terminal callback 必须首先在 owner 发布 terminal，再 seal 当前 route target/binding，使
TCP worker、queued retry 和并发 control path 立即失去获取新 lease 的资格。terminal
发布前已经取得的 lease 可以完成；owner 保证 wrapper 不会在调用中析构。

`TryAcquireStreamApi()` 不能实现成“先 load phase，再构造一个 `shared_ptr` lease”。第一版
必须让 lease admission、shutdown-ledger reservation、`PublishTarget()` 和
`PublishTerminalAndTakeTarget()` 通过同一个短 `ControlMutex` 线性化（或使用有证明和等价
测试的 phase+lease-count 原子协议）：

- `TryAcquire` 在临界区内检查 phase/API kind/binding generation，并在返回前建立强 owner
  lease；它的线性化点是 admission 成功。
- terminal publication 在同一协议内先把 phase 改为 `TerminalPublished` 并关闭 admission；
  此后任何新 lease 必须失败。
- terminal 不等待已经 admitted 的 lease。该 lease 可能在 terminal publication 后才进入或
  返回 MsQuic down call，但 wrapper/handle 在 lease 释放前不会 close；调用结果只能完成本次
  operation 的同步收敛，不能再派生新 stream API work。
- 临界区只做状态、计数、route snapshot 和 operation reservation，不调用 MsQuic、不等待
  callback/worker，也不执行析构。

该规则覆盖 relay 前后的所有非 callback调用者，不只覆盖平台 worker：
`TqTunnelContext::SendBytes/Abort/Drain`、server dispatcher、server speed-control context、
relay worker、admin/connection shutdown 都必须通过 owner API/lease。当前 MsQuic callback
内部可以使用本次 callback参数，但仍需通过 owner完成 phase转换和 operation注册。

### 不变量 4：wrapper callback 路由稳定

`MsQuicStream::Callback` 和 `Context` 是普通字段，MsQuic 不提供跨线程同步。relay worker
注册时不得继续直接执行：

```cpp
stream->Callback = PlatformRelayCallback;
stream->Context = binding;
```

优先设计是：

- wrapper 启动前设置一次 `TqStreamCallbackRouter::Callback` 和稳定 router context。
- router 的 target 使用 C++17 可安全发布的 route snapshot。snapshot 持有 callback target
  adapter，adapter 再通过 weak owner/明确 ref guard 访问 dispatcher、tunnel 或 relay binding，
  避免 router-target-lifetime 形成强引用环。
- callback 开始时取得 route snapshot，整个 callback 期间保持 snapshot/target 存活。
- handoff 原子替换 route snapshot；不修改 wrapper callback/context。
- event 按 ownership 分类，而不是一律交给“当前 target”：`START_COMPLETE` 和 terminal 由
  owner处理，`SEND_COMPLETE` 由 operation registry处理，`RECEIVE` 由本次 snapshot或 fail-safe
  receive sink处理，`CANCEL_ON_LOSS` 必须同步填写 error code。只有纯提示类 event（例如没有
  consumer 的 ideal-send-buffer 通知）才可在 target失效时丢弃。
- target 已失效或 generation 不匹配时，`RECEIVE` 不得静默返回 `PENDING` 或假装已消费。
  router 必须同步选择有 owner 的 sink；默认 fail-safe 是接受 0 bytes、禁用后续 receive并在
  锁外请求 abort。任何返回 `PENDING` 的分支都必须先转交 receive view/buffer ownership。

route snapshot 不得保存无保护的 `TqTunnelContext*`、`RelayState*` 或 worker pointer。target
adapter 必须是 callback-safe owner：

- snapshot 强持有 adapter；adapter 通过 shared owner或 `TryEnter/Leave` ref guard保证实际
  target在调用期间存在。
- target detach 后禁止新 `TryEnter`，已有 callback可以完成；target destruction采用 deferred
  retire，不能在另一个线程等待 MsQuic callback。
- adapter 中的 worker pointer在 worker销毁前置空，并由 generation/active状态保护。
- callback 内允许业务 context在返回路径安排 deferred delete，但 adapter在 method返回后不得
  再访问已经自删除的 raw context。优先逐步改为 shared target而不是维持自删除 raw pointer。

推荐的无环引用方向是：terminal-retention registry、tunnel或worker relay state强持 stream
owner；owner内 router强持 adapter；adapter只弱引用/`TryEnter` platform binding或业务 target；
worker map强持 relay/binding，relay可强持 stream owner。callback入口已经持有 stream owner
guard；route snapshot取得过程必须与 target detach串行化，并在离开 control协议前通过 adapter
建立独立 target guard，保证已取得的旧 snapshot可完整处理本次 callback。若进入失败，router
在 callback内选择有 owner的 fail-safe sink。若 binding选择强持 stream owner，
adapter到 binding就必须是弱边；禁止出现
`stream owner -> router -> adapter -> binding -> stream owner`。shutdown sink也遵循同一方向。

router 必须给 handoff、start failure和 terminal提供同一个线性化协议，例如：

```text
PublishTarget(expected_generation, new_target)
PublishStartFailureAndTakeTarget(status)
PublishTerminalAndTakeTarget(event_payload)
```

- 三者与 API lease admission 通过同一个短 control mutex 或等价 CAS state machine串行化。
- `PublishTarget` 在临界区内检查 owner phase仍允许业务路由（`Starting`/`Started`）和 expected
  generation匹配，并递增 route generation；`StartFailed`同样拒绝 publish。
- `PublishStartFailureAndTakeTarget` 仅从 `Starting` once-only进入 `StartFailed`，取走 target并
  关闭 lease/route admission；锁外通知失败，不能吞掉 registry处理的 `SEND_COMPLETE`。
- `PublishTerminalAndTakeTarget` 在同一临界区发布 owner terminal、取走当前 target并将 router
  置为 terminal-empty；之后任何 `PublishTarget` 必须失败。
- terminal callback 在临界区外调用 target/投递 worker event。
- 临界区内不得调用 MsQuic、等待 worker、等待 API lease或执行 relay cleanup。
- route snapshot必须携带 generation；stale callback/worker event不能命中新 target。

### 不变量 5：wrapper detach 与逻辑 detach 分离

定义三个不同动作：

1. **callback-time seal**：在 terminal callback 内执行。
   - 在 lifetime owner 发布 terminal。
   - 将当前 route target/binding 标记 terminal/closing/inactive。
   - 将 router target 切换为空 terminal target；不写 wrapper callback/context。
   - 捕获后续 worker cleanup 所需的 relay id、generation、error/status。
2. **worker-time logical detach**：worker 消费 terminal event 时执行。
   - 标记 relay stream shutdown complete。
   - 将 relay 中的 `Stream` / `Binding` 清空。
   - 清理 TCP、pending receive/write 和 stop 状态。
   - 退役 binding 和 operation owner。
   - 不接收、读取或调用 `MsQuicStream*`。

3. **owner-time destruction**：owner phase 可关闭、terminal retention 已解除，且 callback
   refs、API leases 和 operation/route/binding 强 owner 都释放后执行。
   - `TqStreamLifetime` 析构并 delete `CleanUpManual` wrapper。
   - wrapper 析构调用 `StreamClose()`。
   - 不由 worker terminal event 根据裸指针直接 delete。
   - 不访问 wrapper 的 queued payload 可以晚于 wrapper 析构，但必须只持有独立 control/
     completion owner。

### 不变量 6：callback/context 只在启动前初始化

`MsQuicStream::Callback` 和 `Context` 是普通字段。lifetime lease 只能保证 wrapper 内存
存在，不能消除字段的数据竞争。

对于已经启动并可能收到 upcall 的 stream：

- terminal callback、普通 worker close、TCP error、admin stop 和 retired cleanup 都不写
  wrapper callback/context。
- 非 terminal close 应发布 binding closing，使用 lease 发起 shutdown，然后保留
  route target/owner 直到 terminal callback seal。
- 已启动 stream 不得在 worker 发起 abort 后立即 `StreamClose`。MsQuic 要求先收到
  `SHUTDOWN_COMPLETE`；只有从未成功启动的 stream 才可直接 close。
- `CallbackRefs == 0` 的瞬时观察不能作为跨线程改写 callback/context 的充分条件；在检查
  后仍可能开始新的 callback。

stream 尚未启动且 callback 不可能并发时初始化 callback/router context。之后的 handler
handoff 只修改 router target。

### 不变量 7：send completion 不依赖 retired wrapper

`SHUTDOWN_COMPLETE` 是 MsQuic stream terminal event，但 callback 已投递到 worker 的
send-complete operation 仍可能排在 terminal worker event 前后。send completion 必须仅依赖：

- send operation 自身；
- relay id / generation；
- binding/completion-state owner；
- buffer owner 和 accounting 字段。

它不得通过 retired stream pointer 清 callback，也不得在 completion 结束时访问 wrapper。

router 对 `SEND_COMPLETE` 不得按“当前 target”分派。handoff 前发起的 tunnel send 可能在
target 切到 relay 后才完成；把其 `ClientContext` reinterpret 成平台 relay operation 会造成
类型混淆和 UAF。

所有 managed stream send context 必须先注册到 lifetime owner级 completion registry，例如：

```text
ClientContext pointer key
  -> operation kind
  -> completion function / typed record owner
  -> once-only claimed/completed state
```

send API调用前注册 record；API同步失败则 unregister并归还 ownership。router 的
`SEND_COMPLETE` 分支只把 `ClientContext` 当作不解引用的 key，从 registry once-only claim
typed record，再调用 operation 自身 completion；不读取当前 route target。重复、未知或
generation不匹配的 key进入诊断/fail-safe，禁止先读取 pointer magic或盲目 cast。现有 tunnel
send、Linux/Windows/Darwin relay send operation 都要接入该 registry或等价安全 lookup。

`ClientContext` 虽然类型是 pointer，但只能作为 opaque token。若直接使用 operation地址，
同一 owner内不得在 duplicate/late callback仍可能到达时把该地址复用给新 record，否则旧 callback
会误 claim新 operation。优先使用经三端 ABI/static assertion和 round-trip测试验证的 owner内
非零单调 opaque token（只比较、不解引用，并检测计数器耗尽）；否则保留 tombstone直到 owner
关闭，或使用有容量/背压的不复用 envelope arena。record中的 generation无法单独修复“相同 key
已指向新 record”的问题。测试必须强制地址/token复用尝试并验证旧 completion不能命中新 record。

registry lookup临界区必须短，不调用 MsQuic或worker cleanup。claim后的 record由 callback
或 queued worker event独占；terminal/stop cleanup只能处理仍在 registry中的未完成 record，
并与 callback claim竞争一次性 ownership。

owner内嵌 registry时，registered record不得反向强持 owner形成
`owner -> registry -> record -> owner` 环，也不得通过强 binding 间接形成
`owner -> record -> binding -> owner`。callback本身先持有 owner guard；record 从 registry
once-only claim 后，才允许 queued worker envelope 显式转交所需的 binding/completion owner，
完成时释放。

但 registered send仍必须保证未来 callback入口的 router/owner存活。send注册时同时在
connection/runtime级 retention registry取得独立 completion-retention token；`StreamSend`
同步失败时 unregister record并释放 token，成功提交后由 `SEND_COMPLETE` callback先建立 owner
guard、once-only claim record，再解除 token。这个外部 token不能放进 owner内 registry record
形成 self-cycle，且必须计入 retained-owner指标。

terminal publication 会 seal 新 send registration，但此刻 registry 不保证立即为空：terminal
可能与一个已 admitted、尚未从 `StreamSend` 返回的 lease 交错。terminal callback 不等待；
最后一个 in-flight send lease 返回后，deferred finalizer 将仍未 claim 的 record 按同步失败/
canceled 路径 once-only 收敛。已经由 `SEND_COMPLETE` claim 并投递给 worker 的 record 不再属于
registry，只依赖自己的 payload/binding owner，且不得访问 wrapper。registry 非空年龄和
terminal reconcile 次数必须可诊断，但“terminal 瞬间非空”本身不是契约错误。

### 不变量 8：异步 receive ownership 不保存裸 stream capability

返回 `QUIC_STATUS_PENDING` 的 receive view 可能晚于 callback 被 worker消费。view 不得只保存
`MsQuicStream*`：

- active receive completion 通过 binding/lifetime owner 获取 lease，再调用
  `ReceiveComplete()`。
- view 可以持有 owner 或稳定的 binding/completion owner，但不能让裸 pointer 单独逃逸。
- terminal 发布后不再开始新的 `ReceiveComplete()` / `ReceiveSetEnabled()` 调用。
- terminal/abort cleanup 释放 view、本地 buffer owner、budget 和 accounting；如果 stream
  已 abort/terminal，不为了“补完成”而调用 stream API。

MsQuic receive 契约要求应用最终完成已接受 bytes，或者 stream 被 abort。实现必须区分
active drain 与 terminal discard，不能复用一个无条件调用 `ReceiveComplete()` 的 helper。
graceful peer FIN 不是 discard：FIN 前已返回 `PENDING` 的 bytes 必须先完成/排空，再发布
TCP `SHUT_WR`；只有 receive abort 或 wrapper terminal 才走本地 discard。

### 不变量 9：relay target handoff 是三阶段激活

router target 切换与 relay 数据面启动必须组成事务：

```text
PrepareRelay(owner)
  -> PublishTarget(expected_old_generation, prepared_target)
  -> CommitRelay(prepared_target)
```

- `PrepareRelay` 创建 relay/binding，但保持 inactive：不 arm TCP read/write、不提交 QUIC send、
  不让 maintenance/retry 调用 stream API。
- prepare 返回内部 `PreparedRelayToken`（暂定名），包含 worker/relay id/generation/binding
  owner；此阶段不得写 public `TqRelayHandle`，不得设置 tunnel `RelayStarted=true`。
- `PublishTarget` 原子替换旧 target；发布失败可 rollback prepared relay，旧 target继续负责
  stream。发布成功是 callback ownership不可逆提交点，不再恢复旧 target。
- `CommitRelay` 再次检查 owner 非 terminal、router generation/target仍匹配，才激活数据面。
- commit成功后才一次性发布 public handle和 `RelayStarted`；其它线程不能观察到 prepared relay。
- prepared target在 publish与commit之间收到 RECEIVE时只进入有界 precommit queue并保留
  receive ownership，不写 TCP、不调用其它 stream API；其它 ownership event按各自 owner处理。
- terminal 若发生在 publish 与 commit 之间，commit 必须失败并走 terminal logical cleanup。
- publish后的 activation failure不恢复旧 target；它 request shutdown，清理 prepared relay和
  precommit receive queue，向 tunnel报告启动失败。
- rollback/commit/terminal 使用 prepared token幂等竞争，不能留下 active map entry、已 arm fd、
  半发布 public handle或重复 owner release。
- tunnel context 把 copied pending relay bytes放入 prepared target的有界 precommit queue；
  publish前失败可回滚，publish后失败走 shutdown。不能再调用
  `stream->Callback(stream->Context, ...)`。

RECEIVE callback 总是由取得的 route snapshot完整处理。旧 target snapshot在 handoff 后仍可
完成当前 callback；其 pending receive/send ownership 不能依赖 target仍是 router current。

### 不变量 10：worker/runtime stop 不提前释放 callback 基础设施

stop 顺序必须为：

```text
停止新 relay/handoff
  -> active owner RequestShutdown
  -> 关闭本地 TCP/data-plane并把 router target切到安全 shutdown sink或可退役 target
  -> worker thread退出
  -> terminal callback由 router本身发布 owner terminal
  -> terminal sink/owner registry完成最终回收
```

- shutdown sink 不持有已销毁 worker/raw relay，只负责 terminal 本地 accounting、public stop
  和 owner回收；SEND_COMPLETE 仍由 operation envelope处理。
- worker event post失败不能阻止 owner terminal publication。
- connection/registration teardown 必须晚于 started stream terminal，或由 MsQuic connection
  shutdown保证所有 stream terminal callback已经交付。
- timeout 只能报告/升级 stuck terminal owner，不能通过提前 `StreamClose` “解决”超时。
- terminal retention数量、最老 age和 stop时剩余 owner应可观测，防止安全修复变成静默泄漏。

### 不变量 11：late cleanup 不持有裸 public relay handle

当前 `TqRelayHandle` 嵌在 `TqTunnelContext` 中。terminal sink、post/enqueue fallback、retired
operation 和 worker stop 可能晚于 tunnel context 析构，不能保存或解引用裸
`TqRelayHandle*`。

- prepare 阶段创建引用计数的 `TqRelayControl`（暂定名），其中保存 once-only stop 状态、
  immutable relay id/generation 和安全的 worker dispatch token；public handle只发布该 control
  的 snapshot/owner。
- callback target、terminal event 和 fallback只携带 control owner/weak owner与 generation，
  不携带 public handle地址。发布 stop前必须匹配 generation，stale event不能修改新 relay。
- tunnel context销毁只撤销 public handle对 control的引用；late cleanup仍可安全完成 control
  accounting，但不得回调已经销毁的 tunnel context。
- worker/runtime销毁前使 dispatch token失效；`TqRelayStop()` 通过安全 control snapshot路由，
  不依赖无保护的 worker raw pointer。
- control不能反向强持 tunnel context、stream lifetime owner或 worker形成环。

## 统一状态模型

建议三端共享 `TqStreamLifetime`，并让 dispatcher、tunnel context、router target 和
各平台 binding 按阶段持有 `std::shared_ptr<TqStreamLifetime>`。owner phase 为：

```text
CreatedNotStarted
  -> Starting
       -> Started
       -> StartFailed
       -> TerminalPublished
Started -> TerminalPublished
CreatedNotStarted | StartFailed | TerminalPublished -> Closed
```

`Starting` 的 start result由 `START_COMPLETE`（或有明确 no-callback保证的同步提交失败）推进；
shutdown desired/submitted和四个方向状态保存在独立 ledger中，不再扩张 owner phase。

relay 本地状态另行表达
`Prepared -> Active -> Closing -> TerminalSealed -> LogicalDetached -> Retired`，不能反向改变
owner phase。worker 已停止或 enqueue 失败时可以合并 logical detach/retire，但已启动 wrapper
不能跳过 `TerminalPublished` 就进入 `Closed`。

binding 至少需要一个 acquire/release 可见的 route closing 状态。已有字段可以复用：

- Linux：`StreamRelayBinding::Closing`，必要时增加语义更明确的 `TerminalSeen`。
- Windows：`CallbackBinding::Closing`，同时保留 relay id / generation 用于 IOCP event。
- Darwin：`StreamBinding::Active=false`，必要时增加 `Terminal=true` 区分普通 unregister。

### Lifetime owner 与 lease

建议公共抽象提供以下能力，而不是向各平台暴露 owner 内部裸指针或外部 terminal 原子：

```cpp
class TqStreamLifetime;
class TqStreamLease;

std::shared_ptr<TqStreamLifetime> TqOpenOutgoingManagedStream(...);
std::shared_ptr<TqStreamLifetime> TqWrapAcceptedManagedStream(HQUIC stream, ...);
TqStreamLease TryAcquireStreamApi(
    const std::shared_ptr<TqStreamLifetime>& owner,
    TqStreamApiKind api);
```

具体 API 可以调整，但必须满足：

- factory 先创建 shared owner/router，再构造 `CleanUpManual` wrapper并安装稳定 handler；不提供
  运行期 raw-wrapper adopt。
- owner 析构负责 delete wrapper。
- owner 持有受 control线性化协议保护的唯一 phase，并提供 start begin/complete、
  `PublishStartFailureAndTakeTarget()`、`RequestShutdown()` 和
  `PublishTerminalAndTakeTarget()` 等幂等状态转换；不得另设绕过 router target take的
  phase-only failure/terminal publication入口。
- lease 持有 owner，从而保证整个 MsQuic API 调用期间 wrapper 存活。
- `TryAcquire` 根据 owner phase、API kind和 route generation判断，并与 terminal publication
  共用 control线性化协议；terminal 前 admitted 的多个 lease均可完成，terminal 后新 lease为零。
- terminal callback 不等待 lease，也不持有 worker mutex。
- wrapper pointer 只能通过有效 lease 或当前 router callback 参数取得。
- owner/binding 必须保证 wrapper 不会在 callback ref 尚未释放时析构。
- outgoing owner进入 `Starting`、accepted owner调用 `SetCallbackHandler`之前，就必须进入
  connection/runtime级 terminal-retention registry；不能只依赖 caller“记得保留 shared_ptr”，
  也不建议用 owner内部 self-cycle隐藏 retention。
- router context依赖 retention保证在 callback入口仍有效；callback第一步必须从该稳定 context
  建立强 owner guard。terminal publication只返回/解除 registry retention token，wrapper最早
  也只能在 callback guard、API lease和其它强 owner全部退出后析构。
- `START_COMPLETE(failure)` 使用相同 callback guard调用
  `PublishStartFailureAndTakeTarget()`并解除 retention；如果 start在提交前同步失败且保证无
  callback，调用线程走同一方法。两条路径以 once-only token竞争，不能双重 target take、
  retention erase或失败通知。
- 若 start由带 `QUIC_SEND_FLAG_START` 的首个 send触发且 send已成功排队，MsQuic会先交付
  `START_COMPLETE(failure)`，再交付该 send的 `SEND_COMPLETE(Canceled=true)`。前者只发布
  `StartFailed`并解除 start retention；start-send record、buffer、accounting和 completion
  retention必须留到后一个 callback once-only claim后归还。只有 `StreamSend`同步失败才直接
  unregister并释放，不能把 direct `StreamStart` 的“failure是唯一事件”套到 Send(START)。
- owner 析构对 started-but-not-terminal 状态 fail-fast；release 构建也不得静默执行未定义
  `StreamClose`。
- outgoing start 使用 begin/callback-complete/synchronous-reject 状态转换；accepted factory 在
  安装 handler前准备好 owner/router target和 retention，不能留下 callback 可见的半构造对象。
- shutdown request 使用 desired/reserved/submitted ledger；测试注入 API failure、graceful到
  abort升级、双方向并发和 terminal-wins，确保 retention不提前释放且相同 abort只提交一次。
- registry record、route target、binding和 relay control不得直接或间接形成回到 owner的强环；
  debug构建记录各类强 owner来源，便于定位 terminal后不归零。

`TqTunnelContext::StreamOpLock` 不能替代该 owner/lease。该锁可继续串行化 tunnel 控制
操作，但不能在 worker 调用 MsQuic 时持有并要求 terminal callback 获取。

注意：不能在 `SHUTDOWN_COMPLETE` 应用 callback 内才把 cleanup mode 从 auto 改成
manual。C++ adapter 在调用应用 callback **之前**已经计算并缓存 `DeleteOnExit`，此时修改
来不及阻止 callback 返回后的 delete。为消除 handoff 竞态，本设计禁止运行期 adopt。

## Terminal callback 顺序

三端应遵循相同顺序：

```text
1. 依靠 terminal retention保证 router context有效，并立即建立强 owner callback guard
2. `PublishTerminalAndTakeTarget()` 在 control线性化临界区关闭 lease/route admission、发布
   `TerminalPublished`、取走 route snapshot和 retention token，并将 router置为 terminal-empty
3. 在临界区外解除 retention；callback guard保证 owner不会在当前 callback内析构
4. 从 snapshot读取 relay id / generation / relay control owner并 seal target/binding；不读取裸
   public handle或无保护 worker
5. wrapper callback/context 保持不变
6. 投递不含 stream pointer/public-handle pointer 的 terminal worker event
7. callback返回并释放 guard；`CleanUpManual` wrapper由尚存 lease/target/binding owner按需保活
8. worker logical detach 和 queued operation完成后释放 route/binding owner
```

如果第 6 步失败：

- 不得恢复 owner phase 或 binding active。
- 不得再次调用 stream API。
- 可以通过 generation校验后的 relay control发布 stop，或投递/执行只操作 relay本地状态的
  fallback close；不得解引用嵌在 tunnel context中的 public handle。
- fallback close API 必须显式知道 `streamAlreadyTerminal=true`，并禁止 wrapper cleanup。
- lifetime owner 必须进入可回收路径，不能因 terminal worker event 丢失而永久保留。

## 所有权交接

relay registration 不能继续只接收 `MsQuicStream*`。平台中立入口应传递 owner，或返回
明确的 ownership result。建议契约：

```text
调用前：tunnel/dispatcher 持有 stream owner
注册成功：relay binding 与 tunnel context 共享或完成转移
注册失败且 stream 已启动：owner 留在 tunnel/router，request shutdown 并等待 terminal
StreamStart 失败或从未启动：caller 可直接释放 owner/close
terminal：binding owner 保持到 callback refs 和 queued operations 清零
```

stream wrapper 创建时就使用 `CleanUpManual`，由 dispatcher/tunnel 持有 owner，再将 owner
共享给 relay route target。禁止在运行期改写 `CleanUpMode`，也禁止 relay registration
重新 adopt 同一个 raw pointer。

公共 factory 是 relay-capable wrapper 的唯一创建入口：outgoing factory负责 `StreamOpen`，
accepted factory负责包装 `HQUIC` 和安装稳定 MsQuic handler。factory 失败必须恢复/关闭尚未
启动的 handle，不能发布一个 router 已可回调但 owner 尚未完成初始化的对象。

accepted raw stream 已经启动，owner/router allocation failure不能直接 `StreamClose`。实现
必须提供一个不依赖动态分配的 emergency C callback（或经 MsQuic契约验证的等价拒绝路径）：

1. 给 raw HQUIC安装静态 emergency handler。
2. 发起带应用错误码的 abort shutdown。
3. emergency handler在 `SHUTDOWN_COMPLETE` 中执行最终 `StreamClose`。
4. 记录 emergency accepted-stream cleanup计数。

禁止在 terminal 前 close accepted raw handle。测试必须注入 owner、router和dispatcher分配
失败，验证进程不崩溃、handle只 close一次且没有 C++ wrapper UAF。

accepted factory 必须在 `PEER_STREAM_STARTED` connection callback 内同步完成包装。该设计依赖
本仓库 MsQuic `Execution.md` 的前提：单个 connection及其 streams不会并行 upcall，且普通
callback内 down call默认不会触发递归 callback。因此，在构造 `MsQuicStream(HQUIC, ...)`
内部调用 `SetCallbackHandler` 之前，owner、router、初始 target和 terminal-retention registry
entry必须全部可用；当前 connection callback返回后才允许 stream event命中 router。不得把
factory异步投递到另一个线程后再安装 handler。

实现和测试不能只把上述时序当作注释：fake MsQuic需在 `SetCallbackHandler` 前、调用期间和
返回后注入可控事件/连接 shutdown，验证事件要么按 MsQuic串行契约延后，要么只能观察到完整
owner/router；若升级后的 MsQuic不能维持该不并行/不递归前提，则必须改为先安装始终有效的
emergency C handler再做显式 handoff，不能继续使用半构造 C++ wrapper。

callback route handoff 必须返回明确结果：新 target 发布成功后旧 target才可释放；失败时
旧 target继续拥有路由并负责 shutdown。不能出现 router target 已切到 relay、但 relay
registration 又报告失败的半提交状态。

registration prepare结果还必须携带 `TqRelayControl` owner和 generation。commit成功时 public
handle原子发布 control snapshot；terminal/fallback只更新 control，不保存 public handle地址。
rollback在 public snapshot发布前回收 control，commit后 stale generation不能命中新 relay。

本项目的落地范围是：

- `tcp_tunnel.cpp` 中同步、异步 client tunnel stream；
- server incoming stream dispatcher 创建的 accepted wrapper；
- dispatcher 可能切换到的 tunnel target 和 server speed-control target；
- dispatcher 自己处理的 client hello/structured-error terminal 路径。

accepted wrapper 在协议识别前无法知道最终 target，因此不能只改 tunnel 分支而让
speed-control 分支继续直接改写 wrapper callback/context。`runtime/speed_test.cpp` 的
`TqAttachServerSpeedControlStream()` 需要接受同一 lifetime owner/router target。独立创建、
不经过 server dispatcher 的 speed-test data stream 不在本次范围内，除非实现复用公共
factory 更简单且其测试完整通过。

现有 `DispatchPendingRelayRx()` 通过读取 `stream->Callback/Context` 人工调用新 handler，
违反 stable-router 不变量。handoff 必须把已经复制的 opening/pending bytes 作为显式数据
交给 prepared target，或调用类型安全 target API；不得通过 wrapper 字段模拟 MsQuic callback。

## 平台设计

### Linux

- relay state 和 `StreamRelayBinding` 持有公共 lifetime owner；所有 worker stream API 通过
  lease 调用。
- stable router 将 relay target snapshot 安全转发到 `StreamRelayBinding`；registration 不改写
  wrapper callback/context。
- `SHUTDOWN_COMPLETE` callback 先在 owner 发布 terminal，再 seal relay target/binding。
- `QuicShutdownComplete` event 不携带 stream pointer。
- `ProcessQuicShutdownComplete()` 只清空 relay 的逻辑 stream/binding。
- 删除 terminal 路径对 `DetachRelayStreamBinding(stream, ...)` 的调用。
- `RetiringStream` 不再用于 terminal shutdown；pending send 只保留 retiring binding 或
  completion owner。
- `HasAbortableStream()` 继续作为 active TCP error 的防线，并要求 binding 未 terminal。
- `TcpWriteClosed` guard 保留为乱序和事故路径的 defense-in-depth。
- terminal event、fallback和retire只保存 `TqRelayControl` owner + Linux relay generation，
  不保存嵌在 tunnel context中的 `TqRelayHandle*`。

### Windows

- `RelayContext` / `CallbackBinding` 持有公共 lifetime owner；IOCP worker stream API 通过
  lease 调用。
- stable router target 保存 relay id/generation 和安全 binding owner；registration 不改写
  wrapper callback/context。
- terminal callback 在投递 IOCP operation 前后完成 binding seal；投递 helper 必须允许
  terminal binding 投递该唯一 terminal operation。
- terminal callback 和普通 IOCP close 都不改写 wrapper callback/context。
- `ProcessQuicShutdownComplete()` / `CloseRelay()` 增加“stream 已 terminal-sealed”的关闭
  语义，禁止读取 `relay->Stream->Context`。
- worker 将 `relay->Stream=nullptr` 仅作为逻辑状态更新。
- queued IOCP TCP completion 继续通过 `Closing`、`TcpWriteClosed` 和无效 socket 降级。
- `RetiredCallbacks_` 只保存 binding owner，不保存或访问 stream wrapper。
- IOCP callback operation和fallback只持有 `TqRelayControl` owner + Windows generation；
  `RelayContext::PublicHandle` 裸指针必须从 late cleanup契约移除。

### Darwin

- `RelayState` / `StreamBinding` 持有公共 lifetime owner；kqueue worker stream API 通过
  lease 调用。
- stable router target 使用安全的 `StreamBinding` owner；registration 不改写 wrapper
  callback/context。
- 保留 terminal callback 中 `binding->Active=false`，但不清 wrapper callback/context。
- `QuicShutdownComplete` event 继续携带 `shared_ptr<RelayState>` owner，但不携带 stream。
- `CloseRelay()` / `RetireRelay()` 接受 terminal-sealed 语义，直接逻辑清空
  `relay->Stream`，不得读取其字段。
- terminal shutdown 不写 `binding->RetiredStream`。
- 删除 `ClearRetiredStreamCallbackIfSafe()` 和 `RetiredStream`：stable router 不需要 retired
  cleanup 修改 wrapper handler。
- send completion 仅通过 `CompletionState` / `KnownSendOperationInfo::BindingOwner` 释放。
- `RelayState::PublicHandle` 改为安全 control owner/generation；kqueue retire和queue-full
  fallback不能访问 tunnel context内存。

## 与非 terminal close 的边界

本设计不禁止 active relay 因 TCP hard error 主动 abort QUIC stream。要求是：调用前必须
证明 binding 尚 active/非 terminal，并持有有效 lifetime lease。

普通 admin stop、worker stop、注册失败、TCP hard error 与 terminal shutdown 不同：这些
路径可能发生在 wrapper 尚有效时。实现不得用一个模糊的 `DetachStream()` 同时处理两类
情况。建议关闭 helper 显式区分：

```text
ActiveAbort       // 取得 lease 后允许调用 stream API
ActiveShutdown    // 通过 lease 发起 shutdown，保留 binding/owner 等 terminal
TerminalLogical  // 不取得新 lease，只清本地状态
```

禁止实现 worker-thread `ActiveDetach` 去写已启动 wrapper 的 callback/context。

## 测试策略

### 1. wrapper 失效边界测试

每个平台增加测试：

1. 使用 `CleanUpManual` wrapper 和可观察 lifetime owner 注册 relay。
2. 通过 wrapper 启动前固定安装的 `stream->Callback`/router context发送
   `SHUTDOWN_COMPLETE`；禁止绕过 router直接调用平台 worker callback。
3. 在 terminal publication解除 retention前确认 callback guard已建立；callback返回后释放
   tunnel侧 owner。若 binding/API lease仍强持 owner，wrapper不得析构；最后一份强 owner释放
   后，wrapper允许在 worker event尚未消费时析构，因为 event不得携带 wrapper capability。
4. 驱动 worker消费 terminal event、TCP late error、pending send completion和retire；分别覆盖
   wrapper仍被 lease保活和已经析构两种情况。
5. 通过 `weak_ptr`/析构计数断言 retention、callback guard、lease和binding各自的释放边界，
   wrapper不早删、不泄漏且只析构一次。

ASan 构建必须覆盖真实 owner 释放。保护页只能作为补充 death test：storage 必须独占
完整 page，测试结束前恢复访问权限，且不能让普通 allocator/destructor 接触仍受保护的
内存；不得对普通 heap object 所在 page 直接 `mprotect/VirtualProtect`。

### 2. stable router 与 terminal publish 测试

通过 wrapper 中固定的 router callback 分派 terminal event；同步返回后、worker 尚未消费
terminal event 时断言：

- owner phase 为 `TerminalPublished`；
- binding 已 terminal/closing/inactive；
- `stream->Callback` / `stream->Context` 仍是创建时安装的 router，不发生 handoff 写入；
- router target 已切换为空 terminal target；
- terminal worker event 已投递且不保存 stream pointer。
- cleanup mode 为 manual，callback 返回不会自动 delete wrapper。

直接调用 `stream->Callback` 不会经过 private `MsQuicStream::MsQuicCallback` adapter，
因此该测试只能证明 router/owner state transition。manual cleanup 和最终析构必须由
owner handoff、析构计数以及 ASan 测试分别证明。

另需覆盖 dispatcher -> tunnel -> relay target handoff 与 terminal 并发：要么旧 target
完整处理 callback，要么新 target完整处理；不能丢 terminal、同时投递两次或访问已释放
target。

start/accepted factory还需覆盖：

- `START_COMPLETE(success)` 在 start API返回前到达，最终 phase只能是 `Started` 或
  `TerminalPublished`，不得被返回路径回退。
- 不带 `QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL` 的 direct `StreamStart`，其
  `START_COMPLETE(failure)` 是唯一回调且没有
  `SHUTDOWN_COMPLETE`；failure publish取走业务 target、通知失败一次，callback guard退出后
  解除 start retention并只 close一次，后续 target publish/lease均失败。
- `SEND(START)` 成功排队后启动失败时，强制覆盖
  `START_COMPLETE(failure) -> SEND_COMPLETE(Canceled=true)` 顺序及两 callback之间 caller释放：
  start callback只推进 `StartFailed`/解除 start retention，completion retention继续保活
  owner/router，send callback再归还 record/buffer/accounting并解除 completion retention。
- start提交前同步拒绝的 no-callback路径也通过 `PublishStartFailureAndTakeTarget()`进入
  `StartFailed`，与 failure callback竞争时 target/notification/retention仍只收敛一次。
- accepted factory在 `SetCallbackHandler` 前/中/后注入事件，验证 MsQuic串行前提以及
  owner/router/retention不存在半构造可见窗口。
- managed factory拒绝/静态禁止 `QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL`；若实现选择支持，
  必须改用等待 terminal的扩展状态并增加 start-failure后 terminal测试。

### 3. pending operation 测试

覆盖 shutdown complete 到达时仍存在已投递但未消费的 send completion / TCP completion：

- accounting 正确归零；
- buffer/context 只释放一次；
- 不通过 retired stream 清 callback；
- binding 在 callback refs 和 operation owners 清零后才释放。
- API lease 与 terminal 发布交错时，wrapper 在 lease 释放前保持存活。

lease线性化测试使用 barrier强制覆盖两种顺序：

- `TryAcquire`先在线性化协议内成功，terminal随后发布：callback不等待，lease可完成且 wrapper
  到 lease释放后才析构。
- terminal先关闭 admission，`TryAcquire`随后执行：返回空 lease，fake stream API调用次数为 0。

仅做 phase load + shared_ptr构造的实现必须让该测试稳定失败，避免 TOCTOU 被压力概率掩盖。

另需覆盖 pending receive view：

- active path 通过 lease 调用一次 `ReceiveComplete()`；
- terminal/abort path 只释放本地 ownership，不调用 stream API；
- receive budget、pending bytes 和 buffer owner 归零且不重复释放。

handoff 测试还必须覆盖：

- tunnel send 在 target切换后才收到 `SEND_COMPLETE`，仍由原 operation completion处理；
- prepared relay 在 target publish前不 arm fd、不调用 stream API；
- terminal 发生在 prepare/publish/commit 每个边界时均能 rollback/cleanup；
- handoff 前开始的 RECEIVE callback由旧 snapshot完成，旧 target不会提前析构。

按事件语义表增加方向状态测试：TCP EOF只提交一次 FIN；QUIC FIN必须等待 pending receive
排空后才 `SHUT_WR`；graceful send可升级到 abort send；receive abort可独立合并；peer abort、
`SEND_COMPLETE(Canceled)`、`SEND_SHUTDOWN_COMPLETE` 和 `CANCEL_ON_LOSS` 都不能发布 owner
terminal。只有 `SHUTDOWN_COMPLETE` 使后续 lease数归零。

terminal/fallback与 tunnel context析构竞态测试必须在释放 public `TqRelayHandle` 所在内存后
驱动 late event，断言仅由匹配 generation的 `TqRelayControl` once-only发布 stop；stale
generation被丢弃，ASan/Application Verifier下无 public-handle UAF。

### 4. active-error 对照测试

保留 active stream TCP hard error 测试，确保加固没有删除正常 abort 行为。terminal seal 后
相同错误则只清本地资源，不调用 stream API。

## 可观测性

保留 Linux 已有 `linux_relay_late_tcp_error_after_stream_shutdown`。建议三端新增或复用测试/
诊断计数：

- terminal callback seal 次数；
- terminal worker logical detach 次数；
- terminal event enqueue/post failure 次数；
- terminal 后被抑制的 stream API 次数；
- retired binding 等待 callback refs / operation owner 的次数。
- terminal retention insert/release、最老 retention age和 start-failure release 次数；
- terminal 与 lease admission竞争的成功/拒绝次数；
- shutdown direction升级、API failure retry和 terminal registry reconcile次数；
- stale relay-control generation丢弃次数。

指标用于验证行为，不应通过指标分支决定正确性。

## 性能约束

stream send/receive completion 属于热路径。公共 lease 可以使用 `shared_ptr` 作为首个正确
实现，但必须测量每次 API 调用的原子引用计数成本。如果出现可观回归，可以在保持相同
语义和测试的前提下改为 intrusive owner/lease；不能为了性能退回裸 pointer + 状态检查。

至少比较改造前后的：

- 单连接与多连接吞吐；
- worker CPU；
- 每次 send/receive completion 的 owner ref 操作数量；
- terminal/retire 延迟和未回收 owner 数。

## 风险

- 在 terminal callback 中过早清 binding relay pointer，可能影响 terminal event 投递；必须
  先把 relay id/generation/owner 保存到局部变量。
- router target handoff 如果与 terminal callback 并发，必须由 route snapshot 决定唯一
  target；不能依赖 wrapper 字段写入顺序。
- 已启动 stream 的 active close 必须等待 terminal callback，不能把 owner 析构当作
  abort 后的快速 close。
- owner 解决对象生存期，stable router 解决 callback target handoff；两者不能互相替代。
- send-complete callback 当前可能绕过普通 binding 检查；改造时不能丢失已经由 MsQuic
  交还的 send operation。
- callback-time seal 与 worker-time close 并发，要求 binding 状态使用 release/acquire；
  owner/lease 保证对象存活，relay worker-only 字段仍只由 worker 修改。
- terminal callback 不得等待 API lease 或获取 worker 调用 MsQuic 时持有的锁。
- terminal event、fallback close 和 unregister 可能相邻或重复，logical detach/retire 必须
  幂等，并保持 relay id/generation/owner 校验。
- 测试 fake stream 不执行 private wrapper adapter，仅直接调用 router callback 不足以证明
  cleanup 行为。
- started owner 的 terminal retention 必须覆盖 worker stop/unregister；普通 caller 释放最后
  一个业务 owner不能触发 pre-terminal `StreamClose`。
- 在禁止 `QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL` 的 managed路径，start failure不会产生
  `SHUTDOWN_COMPLETE`；若只在 terminal释放 start retention会形成永久泄漏。引入该 flag而不扩展
  状态机则会反向造成提前 close。
- terminal时 registry可能暂存已 admitted但尚未从 send API返回的 record；立即断言为空会把
  合法交错误判为错误，提前释放则会与 API返回路径双重 cleanup。
- `TqRelayHandle` 当前嵌在 tunnel context中；terminal sink/late fallback保存其裸指针会把
  stream-wrapper UAF转化为 public-handle UAF，必须通过 control owner/generation消除。

## 实施顺序

1. 先实现一次平台中立 lifetime owner、stable callback router、lease 和 target handoff。
2. Linux 接入公共 owner，替换已经确认的事故路径并验证现有 late-error 回归。
3. Windows 接入同一 owner，修改 terminal IOCP close disposition。
4. Darwin 接入同一 owner，移除 terminal `RetiredStream` 依赖。
5. 三端完成后再收紧公共 API，禁止新的裸 `MsQuicStream*` relay registration。

三份平台计划中的公共 owner 工作不是三个不同实现。第一个执行的平台负责落地公共
抽象，后续平台只能复用并补测试，不能复制平台私有 owner。

## 验收标准

- relay 使用 `CleanUpManual` 和唯一、可测试的 lifetime owner。
- owner phase 是 stream terminal/close 的唯一事实源，started owner 保留到 terminal。
- start成功/失败由 `START_COMPLETE` 或有 no-callback保证的同步拒绝推进；失败路径不等待
  `SHUTDOWN_COMPLETE`，start retention只解除一次；成功排队的 START-bearing send仍等待
  `SEND_COMPLETE(Canceled=true)`归还 buffer和 completion retention。
- start failure与 target take线性化，open/factory失败只通知一次，失败后新 target publish和
  API lease均为零。
- managed factory拒绝 `QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL`；若未来支持，先扩展 failure后
  terminal状态和retention契约。
- wrapper callback/context 在 stream 启动前设置一次，后续 handoff 只切换安全 router target。
- 三端 terminal callback 返回后，没有生产代码通过裸指针解引用对应 wrapper。
- terminal worker event、close、retire、send completion 不需要 stream pointer。
- callback-time seal 发布 owner terminal 并清空 router target，不改 wrapper callback/context。
- 非 callback stream API 调用全程持有 lifetime lease。
- lease admission、shutdown reservation、target publish与terminal publish具有共同线性化协议；
  terminal后新 lease严格为零，terminal前 admitted lease不阻塞 callback。
- terminal callback 不阻塞等待 worker 或 API lease。
- 已启动 stream 的 callback/context 不再被任何 handoff/worker/retired cleanup 修改。
- pending receive view 不保存无 owner 的 stream capability，terminal discard 不调用 stream API。
- TCP EOF、QUIC FIN、peer abort、local cancel和send completion遵守方向语义表；half-close不误发
  owner terminal，graceful send可按策略升级abort且相同方向不重复提交。
- SEND_COMPLETE 先用 opaque pointer key从 owner registry once-only claim typed record，不按
  当前 router target cast，也不读取未知 pointer；completion token在同一 owner内无 ABA复用。
- owner/router/adapter/platform binding引用方向无强环，已取得 route snapshot持有独立 target guard。
- relay 只有在 target publish成功后才 commit并激活数据面。
- late TCP event 只操作 TCP、relay、binding 和 operation owner。
- duplicate/stale terminal event 不会重复 close、重复释放 owner 或命中新的 relay generation。
- terminal/late cleanup不保存裸 `TqRelayHandle*`；public control更新必须匹配 generation且
  tunnel context析构后仍可安全 once-only收敛。
- active TCP hard error 在 capability 有效时仍保持原 abort 语义。
- 保护页或 ASan 生命周期测试覆盖三端 terminal event 后的 worker cleanup。
- Linux 现有事故回归测试继续通过。
