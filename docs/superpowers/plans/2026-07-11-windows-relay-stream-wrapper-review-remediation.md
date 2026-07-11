# Windows Relay Stream Wrapper 评审问题修复开发计划

> **实施规则：** 按 Task 顺序执行；每个生命周期或并发修复必须先提交可稳定失败的
> 确定性测试，再实现修复。步骤使用 checkbox（`- [ ]`）跟踪。禁止用 sleep 猜测竞争窗口，
> 禁止通过删除测试、放宽归零断言或 timeout 强制析构 started wrapper 规避问题。

**Goal:** 修复
`docs/superpowers/plans/2026-07-10-windows-relay-stream-wrapper-terminal-lifetime.md`
末尾 2026-07-11 评审记录中的 3 个 P0 和 3 个 P1 问题，恢复 Windows 构建，重新建立
“只有真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 发布 wrapper terminal”的唯一状态迁移，
使 registration rollback、worker stop、IOCP command/operation drain、public control 和系统门禁
形成可线性化、可确定验证的闭环。

**Architecture:** `TqStreamLifetime` 的 phase 是 wrapper terminal 的唯一事实源；本地 relay
close/retire 与 wrapper terminal 分离。主动 close、registration activation failure 和 worker stop
只能通过 owner shutdown ledger 请求 shutdown，并将 router target切到不引用 worker的 sink；
不得人工调用 `PublishTerminalAndTakeTarget()`。started owner由公共 terminal retention保活，直到
stable router处理真实 `SHUTDOWN_COMPLETE`。Windows control generation构造后 immutable；
public handle不保存可解引用的 worker capability。IOCP control command改为 heap-owned typed envelope，
stop拒绝新 admission后继续完成或显式失败所有已提交 command和 `OVERLAPPED`，最后才关闭 IOCP。

**Source Review:**
`docs/superpowers/plans/2026-07-10-windows-relay-stream-wrapper-terminal-lifetime.md#2026-07-11-windows-实现评审记录`

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:**
`docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, Windows IOCP/Winsock overlapped IO, MsQuic C++ wrapper,
shared ownership, CMake/MSVC, ASan for Windows/Application Verifier, k6, fault-injection hooks.

---

## 1. 范围、非目标与发布阻断条件

### 1.1 本计划范围

- `TqRelayStopControl` immutable generation 与 Windows production/test 调用点。
- Windows active close、terminal callback、terminal cleanup record、shutdown sink和本地退役。
- prepare/publish/commit failure后的 owner shutdown、TCP cancellation和 IOCP drain。
- worker/runtime stop与已入队 register/snapshot/trace/close command的 ownership。
- Windows public handle/result中的裸 worker capability清理。
- 恢复被 `#if 0` 禁用的真实 Winsock/data-path teardown测试。
- Windows focused、Verifier/ASan、F01-F18、k6、吞吐和 churn/stop恢复门禁。

### 1.2 非目标

- 不重写 MsQuic 公共协议、payload framing或压缩格式。
- 不复制 Windows 私有的 stream owner/router；继续复用公共 `TqStreamLifetime`。
- 不改变 Linux/Darwin control语义；修改公共 header时必须保持三平台编译。
- 不以“worker已退出”为理由强制推进 started owner到 `TerminalPublished`。
- 不把 injected completion单测当作真实 Winsock cancellation/drain的替代品。

### 1.3 发布阻断条件

- Windows 任一 Release/Debug test target不能编译。
- `PublishTerminalAndTakeTarget()` 仍可从真实 terminal router和合法 start failure之外的路径调用。
- started owner在真实 terminal前从 terminal retention registry移除或 wrapper析构。
- publish后 activation failure没有提交 shutdown，或发布半完成 public handle。
- stop返回时仍有已提交 control command、`OVERLAPPED`、worker op或栈上 raw command pointer。
- production handle/result仍携带可解引用的 `TqWindowsRelayWorker*`。
- 任一原 `#if 0` crash/deadlock/hang用例仍未恢复。
- system/performance/Verifier证据缺失但计划被标为完成。

## 2. 评审问题到修复任务的映射

| 评审问题 | 根因 | 修复 Task | 核心证据 |
|---|---|---|---|
| P0-1 Windows不能编译 | immutable generation仍按 atomic访问 | Task 0 | Windows全量 build + 静态搜索零违规 |
| P0-2 active close提前terminal | 本地退役错误推进 owner phase | Task 1、2 | terminal barrier前 owner retained/wrapper存活 |
| P0-3 activation rollback无shutdown | 先发布terminal再申请API | Task 3 | shutdown一次、terminal前retention不释放 |
| P1-4 stop command crash/deadlock/hang | stack command raw pointer与stop无共同ownership | Task 4 | queued command全部完成/失败并唤醒 |
| P1-5 public裸worker | result/handle保留旧兼容字段 | Task 5 | production `WindowsWorker`引用清零 |
| P1-6 release证据缺失 | legacy用例禁用、系统门禁延期 | Task 6、7 | 恢复用例、F01-F18、Verifier、性能报告 |

## 3. 实现前必须固化的安全不变量

### 3.1 Wrapper terminal 与本地 relay 退役分离

- `TqStreamLifetime::Phase::TerminalPublished` 只能由 stable router处理真实
  `SHUTDOWN_COMPLETE`发布；合法 direct start failure使用独立的
  `PublishStartFailureAndTakeTarget()`，不得复用 terminal。
- `Closing`、TCP socket invalid、pending IO为零、relay map erase和public Stop均不是
  wrapper terminal证据。
- 本地 relay允许在 TCP/worker ownership归零后退役，但若真实 terminal未到达，owner/router/
  shutdown sink和terminal retention必须继续独立存活。
- terminal callback不得等待 worker、IOCP、API lease或本地 retirement；它只 seal route、
  发布 phase，并把 cleanup交给仍存活的 target或 worker-independent sink。
- terminal sink不能强持 owner形成 `owner -> target -> owner`环；owner由公共 retention保活，
  sink只持 control、generation和必要的独立 cleanup state。

### 3.2 Shutdown ledger 与 registration rollback

- prepare前失败可直接返回未消费 socket；一旦 socket关联 IOCP，结果必须明确
  `TcpFdConsumed=true`，caller立即置 invalid。
- publish前失败可以撤销 prepared state；publish后 activation failure不能恢复旧 target，
  但也不能伪造 terminal。
- started owner的 activation failure必须通过方向 ledger提交 `AbortBoth`；API failure保留
  desired intent和terminal retention，等待真实 terminal或后续受控重试。
- terminal与activation failure竞争时，terminal wins：terminal后不得再申请API；failure先赢时
  shutdown至多提交一次，随后真实 terminal完成最终释放。
- precommit RECEIVE在activation failure上只释放本地 copied ownership/accounting，
  不调用 `ReceiveComplete()`；没有接管的 MsQuic buffer不得伪记完成。

### 3.3 IOCP command 与 operation ownership

- 每个已成功 `PostQueuedCompletionStatus()` 的 control command由 heap-owned typed envelope持有，
  caller等待 shared completion state；IOCP operation不得保存指向caller栈的 raw `Control`。
- stop admission与 register/snapshot/trace/close post使用同一线性化锁/state。
- stop开始后新command同步失败；stop前已成功post的command必须被执行或显式标记
  `Stopped`并唤醒所有waiter。
- GQCS `FALSE + non-null OVERLAPPED` 仍退役对应 operation；只有 null overlapped表示本次没有operation。
- IOCP关闭前，control command、callback op、terminal op、TCP recv/send和maintenance owner全部为零。

### 3.4 Public control 与 worker identity

- control generation构造后 immutable，所有比较直接读取同一整数，不存在二次初始化。
- public handle只保存 control、control generation、worker index、relay id和relay generation；
  不保存可解引用的 worker pointer。
- stop/trace/runtime操作通过 runtime-owned worker lookup并受 lifetime lock保护；旧generation
  只能命中旧control，不能影响复用同一handle storage的新relay。
- active accounting、Stop publication、socket close、operation settle和context析构均once-only。

## 4. 预计文件变更与任务依赖

- `src/tunnel/relay.h`, `src/tunnel/relay.cpp`, `src/tunnel/tcp_tunnel.cpp`：immutable generation、
  删除 Windows裸worker字段、caller-owned handle publication。
- `src/tunnel/stream_lifetime.h`, `src/tunnel/stream_lifetime.cpp`：原则上只增加测试快照/
  invariant helper；不得为Windows引入第二套phase。
- `src/tunnel/windows_relay_worker.h`, `src/tunnel/windows_relay_worker.cpp`：terminal/local retire分离、
  rollback sink、typed command envelope、stop drain和test barriers。
- `src/unittest/windows_relay_worker_test.cpp`, `src/unittest/stream_lifetime_test.cpp`,
  `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/client_tunnel_open_test.cpp`,
  `src/unittest/speed_test_test.cpp`, `src/unittest/router_runtime_test.cpp`：失败测试、真实socket和E2E回归。
- `src/runtime/relay_metrics.h`, `src/runtime/relay_metrics.cpp`, `docs/relay_win.md`,
  `src/docs/thread_model_cn.md`, `src/CMakeLists.txt`：门禁metrics、线程模型和构建接线。
- `docs/test/` 或既有测试证据目录：保存Windows build、Verifier、F01-F18、k6、吞吐和soak结果。

**依赖顺序：** `Task 0 -> Task 1 -> Task 2 -> Task 3 -> Task 4 -> Task 5 -> Task 6 -> Task 7`。
Task 5可在Task 4测试稳定后与Task 6部分并行，但最终合并前必须重新跑公共三平台编译。

## Task 0: 恢复 immutable control generation 与 Windows 构建

**覆盖问题：** P0-1。

**Files:** `relay.h/.cpp`, `windows_relay_worker.cpp`, `windows_relay_worker_test.cpp`，
以及编译错误暴露的公共测试文件。

- [ ] **Step 1: 固化失败证据**
  - 在当前 HEAD 保存 MSVC Release build错误，确认 `.load()`/`.store()` 作用于
    `const uint64_t Generation`。
  - 用 `rtk rg -n "Generation\\.(load|store)"` 记录 Windows production/test调用点基线。
  - 不先把 `Generation` 改回 atomic；immutable generation是公共control契约。

- [ ] **Step 2: 统一 generation构造与读取**
  - Windows删除私有的“默认构造后 `.store()`”初始化，使用
    `std::make_shared<TqRelayStopControl>(TqRelayNextControlGeneration())`或等价公共factory。
  - 所有读取改为直接访问 `control->Generation`；generation mismatch继续通过公共helper记诊断。
  - 删除重复的 Windows generation allocator，保证进程内跨平台/后端control token不复用。

- [ ] **Step 3: 恢复编译矩阵**
  - 编译 `tcpquic_stream_lifetime_test`、`tcpquic_windows_relay_worker_test`、
    `tcpquic_windows_reactor_test`、`tcpquic_tcp_tunnel_test`、
    `tcpquic_client_tunnel_open_test`、`tcpquic_speed_test_test`、
    `tcpquic_router_runtime_test`。
  - 在Linux和macOS CI至少编译受影响的公共 `relay.h/.cpp` consumers，防止公共control回归。

- [ ] **Step 4: 独立提交**

```bash
rtk git add src/tunnel/relay.h src/tunnel/relay.cpp src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "fix(windows): align relay control with immutable generation"
```

## Task 1: 先建立“本地退役早于真实 terminal”的确定性失败测试

**覆盖问题：** P0-2。

**Files:** `windows_relay_worker.h/.cpp`, `windows_relay_worker_test.cpp`,
`stream_lifetime_test.cpp`。

- [ ] **Step 1: active close terminal barrier测试**
  - 创建 started manual owner + fixed router + Windows relay，使用barrier阻止真实
    `SHUTDOWN_COMPLETE`分派。
  - 触发 admin stop或active TCP hard error，使socket cancellation和全部本地IOCP ownership归零。
  - 在真实 terminal尚未发生时断言：owner phase仍为`Starting|Started`、terminal retention仍为1、
    wrapper析构计数为0、router target为shutdown sink或terminal-capable target。
  - 释放terminal barrier并分派真实event；callback返回后才允许phase变为
    `TerminalPublished`，最终wrapper只析构一次。

- [ ] **Step 2: worker stop terminal barrier测试**
  - 启动真实worker和pending `WSARecv`，阻塞terminal，调用runtime/worker `Stop()`。
  - 断言worker drain本地 `OVERLAPPED`并退出，但started owner retention仍存在，wrapper未析构。
  - worker对象销毁后分派terminal，必须命中worker-independent sink，signal旧control一次且不读worker。

- [ ] **Step 3: terminal与本地retire正反顺序测试**
  - terminal先于本地IO completion：terminal callback立即发布phase，terminal record/sink持有cleanup，
    late TCP completion只结算本地ownership。
  - 本地IO completion先于terminal：relay可从active map退役，但owner/target/retention保持。
  - 两种顺序结束后owner、binding、terminal record、socket、buffer、control和active accounting归零。

- [ ] **Step 4: 增加禁止调用点静态门禁**
  - 测试或lint helper列出所有`PublishTerminalAndTakeTarget()`调用点。
  - 允许集合仅含公共router terminal分支和明确的test helper；start failure必须使用独立API。

- [ ] **Step 5: 仅提交失败测试**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/stream_lifetime_test.cpp
rtk git commit -m "test(windows): expose premature relay terminal publication"
```

## Task 2: 分离 local retirement、active shutdown 与 wrapper terminal

**覆盖问题：** P0-2。

**Files:** `windows_relay_worker.h/.cpp`, `stream_lifetime.h/.cpp`,
`windows_relay_worker_test.cpp`, `relay_metrics.h/.cpp`。

- [ ] **Step 1: 定义三类独立状态**
  - owner phase继续表示wrapper lifecycle。
  - relay local state区分`Active|Closing|LocallyRetired`，只管理map/socket/IOCP ownership。
  - route target state区分Windows binding、shutdown sink和empty；只有真实terminal令route empty。
  - 禁止用reason字符串或`StopPublished`推导wrapper terminal。

- [ ] **Step 2: 删除人工 terminal publication**
  - 从`TryRetireRelay()`、`FinalizeWorkerStopOnWorkerThread()`、active close和普通rollback删除
    `PublishTerminalAndTakeTarget()`。
  - `TryRetireRelay()`只从map移除本地relay、发布control stop和转移必要的independent cleanup state。
  - worker finalize只在本地IOCP ownership归零后清worker容器；不得清除公共terminal retention。

- [ ] **Step 3: 完成 worker-independent shutdown sink**
  - stop/active close在线性化点把route切到sink；sink不持worker、relay、handle或stream裸指针。
  - sink处理真实terminal并发布control stop；non-ownership event fail-safe忽略或按公共owner规则收敛。
  - stop前已注册`SEND_COMPLETE`仍走registry保存的target snapshot；旧binding在endpoint关闭后必须能
    执行detached typed completion，或registration时使用独立completion target，不能丢operation。

- [ ] **Step 4: terminal cleanup与local cleanup解耦**
  - terminal发生在Windows binding active时，继续投递无stream pointer的terminal record。
  - terminal发生在worker退出后，由sink直接完成control/owner侧cleanup，不尝试post已关闭IOCP。
  - duplicate terminal只发布一次；late local completion不重复close socket或释放accounting。

- [ ] **Step 5: 增加metrics与断言**
  - 暴露`local_retired_waiting_terminal` count/oldest age、shutdown sink count、
    premature terminal publish invariant violation。
  - focused测试在terminal前允许waiting-terminal为1；terminal后30s内必须为0。

- [ ] **Step 6: 运行Task 1失败测试并独立提交**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/tunnel/stream_lifetime.h src/tunnel/stream_lifetime.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "fix(windows): retain stream owner until real terminal callback"
```

## Task 3: 重做 publish后 activation failure 与 socket rollback事务

**覆盖问题：** P0-3，并补齐原计划Task 1未完成项。

**Files:** `windows_relay_worker.h/.cpp`, `relay.cpp`, `tcp_tunnel.cpp`,
`windows_relay_worker_test.cpp`, `tcp_tunnel_test.cpp`, `client_tunnel_open_test.cpp`,
`speed_test_test.cpp`。

- [ ] **Step 1: 增加activation failure失败测试矩阵**
  - prepare前失败：`TcpFdConsumed=false`，owner target/phase不变。
  - IOCP association后、publish前失败：`TcpFdConsumed=true`，socket close/cancel一次，
    无public handle且无data-plane post。
  - publish后、首个`WSARecv`同步失败：shutdown ledger记录`AbortBoth`并提交一次，
    owner仍active/retained，等待真实terminal。
  - publish后shutdown API failure：desired intent保留、wrapper不析构、control发布失败结果，
    后续真实terminal仍能收敛。
  - terminal分别在prepare、publish、shutdown reservation、commit最终check前后发生，
    terminal与failure只能有一个线性化结果。

- [ ] **Step 2: 引入move-only prepared registration token**
  - token持有socket disposition、IOCP association、relay/binding、route generation、
    activation phase、precommit queue和once-only rollback bit。
  - socket一旦关联IOCP即由token消费；caller观察`TcpFdConsumed`后立即置invalid。
  - token destructor只调度worker-owned rollback，不在任意caller/callback线程直接销毁pending operation。

- [ ] **Step 3: 用activation mutex/state统一竞争**
  - `Prepared -> Active|Failed|Terminal`只有一个线性化点。
  - mutex内只校验owner phase、route generation、control generation、map identity和token；
    不调用MsQuic、Winsock、close、wait或可能析构对象的代码。
  - 线性化结果在锁外执行shutdown、cancel/drain、precommit处理和public result publication。

- [ ] **Step 4: activation failure不再伪造terminal**
  - 删除`WithdrawPublishedRelayTarget()`对`PublishTerminalAndTakeTarget()`的使用。
  - 将target切到registration-failure sink或shutdown sink，owner phase保持active。
  - 通过`RequestShutdown(AbortBoth)`自身的reservation/submitted ledger提交，不在外层先申请
    会与phase检查重复的lease。
  - terminal已赢时不再提交API，只做local rollback；API failure不释放retention。

- [ ] **Step 5: precommit与IOCP ownership收敛**
  - activation success才post/继续TCP数据面并drain precommit。
  - failure/terminal丢弃copied receive本地ownership，不调用`ReceiveComplete()`。
  - 已成功提交的`WSARecv`即使activation随后失败，也必须保留relay/operation owner到GQCS dequeue。

- [ ] **Step 6: 覆盖全部handoff入口**
  - 同步/异步client open、server accepted dispatcher、dispatcher -> tunnel、
    dispatcher -> speed-control分别验证failure result、socket disposition和owner retention。
  - `DispatchPendingRelayRx()`只交付copied bytes，不读取/人工调用wrapper callback。

- [ ] **Step 7: 独立提交**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/tunnel/relay.cpp src/tunnel/tcp_tunnel.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/tcp_tunnel_test.cpp src/unittest/client_tunnel_open_test.cpp src/unittest/speed_test_test.cpp
rtk git commit -m "fix(windows): make relay activation rollback wait for terminal"
```

## Task 4: 使 control command 与 worker stop共享安全ownership

**覆盖问题：** P1-4。

**Files:** `windows_relay_worker.h/.cpp`, `windows_relay_worker_test.cpp`,
`router_runtime_test.cpp`。

- [ ] **Step 1: 恢复并强化已禁用失败测试**
  - 恢复pending-register wake during stop测试，用barrier保证register command已经成功post且
    worker尚未消费，再启动stop。
  - 增加snapshot command、trace command和close command相同交错。
  - waiter使用确定性condition variable/barrier，不使用固定sleep；修复前应稳定暴露UAF、未唤醒或hang。

- [ ] **Step 2: 定义typed command envelope**
  - `RegisterRelayCommand`、`SnapshotCommand`等改为heap-owned shared state，含typed result、
    `Pending|Completed|Stopped|PostFailed`状态、mutex/CV和once-only completion。
  - `IoOperation`持`shared_ptr<CommandBase>`或typed variant，不保存caller栈地址/`void* Control`。
  - post failure同步完成envelope并唤醒；worker dequeue无论正常或stop都恰好完成一次。

- [ ] **Step 3: 线性化stop admission**
  - `ControlCommandLock_`下将admission从`Accepting`切到`Stopping`，之后拒绝所有新post。
  - 在同一锁下post begin-stop marker；此前已成功post的command必须位于marker前或被显式登记到
    pending command registry。
  - begin-stop消费/失败所有pending command并唤醒waiter后，才能进入IOCP ownership drain。

- [ ] **Step 4: 修复Snapshot与StopRelay交错**
  - snapshot不得等待一个只有worker线程才能推进、而worker又被当前command阻塞的状态。
  - snapshot读取worker-local状态时建立有界一致性快照；stop中返回`Stopping`快照或安全失败，
    不跨线程等待relay retire。
  - 恢复`StopRelay`后并发`Snapshot()`的禁用case并断言无deadlock。

- [ ] **Step 5: command与IOCP统一归零门禁**
  - snapshot增加pending command count/oldest age/stop-failed command count。
  - `WorkerIocpOwnershipIsZero()`纳入command envelope和queued control op。
  - `CloseHandle(Iocp_)`前断言pending command=0且所有waiter已返回。

- [ ] **Step 6: 独立提交**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "fix(windows): own relay control commands through stop drain"
```

## Task 5: 删除 Windows public裸worker capability

**覆盖问题：** P1-5。

**Files:** `relay.h/.cpp`, `windows_relay_worker.h/.cpp`, `tcp_tunnel.cpp`,
相关Windows/public handle测试。

- [ ] **Step 1: 增加public handle结构与复用测试**
  - 编译期/静态断言Windows committed result不含`TqWindowsRelayWorker*`。
  - placement-new复用同一handle storage，旧stop/trace/terminal只能命中旧control generation。
  - runtime已销毁时stop/trace返回already-stopped或安全no-op，不解引用旧worker地址。

- [ ] **Step 2: 删除裸字段**
  - 删除`TqWindowsRelayRegistrationResult::Worker`和`TqRelayHandle::WindowsWorker`。
  - 删除production/test helper中的写入、清空和诊断读取。
  - 若需要worker identity，仅保存不可解引用的worker index/token，不保存地址。

- [ ] **Step 3: 固化runtime lookup契约**
  - `TqRelayStop()`/`TqRelaySetTraceContext()`先snapshot control、worker index、relay id和双generation。
  - runtime lifetime lock保证lookup返回的worker覆盖整个post调用；lookup失败只更新匹配generation的control。
  - runtime stop与public operation竞争结果只能是post成功或already-stopped，不能部分调用。

- [ ] **Step 4: 跨平台编译与独立提交**
  - 编译Linux/Darwin/Windows受影响target，确认公共handle ABI/source使用点同步更新。

```bash
rtk git add src/tunnel/relay.h src/tunnel/relay.cpp src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/tunnel/tcp_tunnel.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "refactor(windows): remove raw worker capability from relay handle"
```

## Task 6: 恢复真实 Winsock/data-path stop-drain集成测试

**覆盖问题：** P1-4、P1-6。

**Files:** `windows_relay_worker_test.cpp`, `windows_reactor_test.cpp`及必要test hooks。

- [ ] **Step 1: 清除本计划范围内的`#if 0`**
  - 恢复pending-register、`StopRelay+Snapshot`、half-close、backpressure/fatal-send、
    TCP recv pool、zstd flush和`CancelIoEx(ERROR_NOT_FOUND)`专用case。
  - 不接受“由更小gate覆盖”作为继续禁用的理由；若旧测试假设错误，保留生产场景并重写断言。

- [ ] **Step 2: 真实pending receive cancellation**
  - 使用真实socket pair提交`WSARecv`，确认`InFlightTcpRecvs=1`后触发stop。
  - 接受cancel与真实completion竞争产生success或`ERROR_OPERATION_ABORTED`两种结果。
  - 断言GQCS dequeue后才释放operation/buffer并减counter，stop返回时active/pending为0。

- [ ] **Step 3: 真实/可控pending send drain**
  - 使用socket backpressure稳定制造pending `WSASend`；若平台不稳定，使用专用IOCP provider/hook，
    但仍必须覆盖真实`OVERLAPPED` ownership和GQCS dequeue。
  - terminal前后各保留一个recv/send completion，覆盖success、abort和teardown error。

- [ ] **Step 4: 数据面语义回归**
  - half-close保持反向转发，pending receive排空后才`shutdown(SD_SEND)`。
  - backpressure/fatal-send、TCP recv operation reuse和zstd flush均能stop/drain且payload/accounting正确。
  - active hard error仍fatal reset；teardown error downgrade不增加fatal count。

- [ ] **Step 5: 每case统一归零断言**
  - active relay、pending command、overlapped recv/send、worker op、pending receive bytes/depth、
    send registry、terminal retention、shutdown sink、local-retired-wait-terminal和buffer bytes全部归零。
  - 断言发生在registry reset之前；失败时保留现场，不用reset掩盖泄漏。

- [ ] **Step 6: 独立提交**

```bash
rtk git add src/unittest/windows_relay_worker_test.cpp src/unittest/windows_reactor_test.cpp src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp
rtk git commit -m "test(windows): restore real relay stop drain coverage"
```

## Task 7: 可观测性、文档、Windows验证与发布证据

**覆盖问题：** P1-6，并收口全部整改任务。

**Files:** `relay_metrics.h/.cpp`, `docs/relay_win.md`, `src/docs/thread_model_cn.md`,
`src/CMakeLists.txt`, Windows测试与证据目录。

- [ ] **Step 1: 补齐整改门禁metrics**
  - wrapper：terminal retained count/oldest age、real terminal publish、premature publish violation。
  - registration：prepared、activation failed、shutdown desired/submitted/failed、rollback pending IO。
  - stop：pending command、pending overlapped、local-retired-wait-terminal、sink、stop remaining/oldest age。
  - public control：generation mismatch、stop once、accounting duplicate release、runtime lookup miss。

- [ ] **Step 2: 更新线程与生命周期文档**
  - 用时序图分别记录active close、publish后activation failure、worker stop后late terminal。
  - 明确local retirement不等于wrapper terminal，IOCP close不等于operation completion。
  - 删除`docs/relay_win.md`中已修复的known test debt，保留实际未完成项和复现命令。

- [ ] **Step 3: 运行Windows focused矩阵**

```powershell
rtk cmake --build build --config Release --target tcpquic_stream_lifetime_test tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j
rtk build/bin/Release/tcpquic_stream_lifetime_test.exe
rtk build/bin/Release/tcpquic_windows_relay_worker_test.exe
rtk build/bin/Release/tcpquic_windows_reactor_test.exe
rtk build/bin/Release/tcpquic_tcp_tunnel_test.exe
rtk build/bin/Release/tcpquic_client_tunnel_open_test.exe
rtk build/bin/Release/tcpquic_speed_test_test.exe
rtk build/bin/Release/tcpquic_router_runtime_test.exe
rtk git diff --check
```

- [ ] **Step 4: 运行ASan/Application Verifier**
  - 覆盖active-close-before-terminal、worker-stop-late-terminal、activation rollback、
    command-stop、handle reuse、real pending recv/send和send registry late completion。
  - 要求0 UAF、0 double free、0 invalid handle、0 lock misuse；逻辑泄漏通过metrics/destructor归零证明。

- [ ] **Step 5: 执行系统、容量和性能门禁**
  - 执行下文R01-R18、F01-F18、k6 short-tunnel、raw TCP、30分钟churn和worker-stop恢复。
  - 保存config、git SHA、binary SHA256、测试命令、metrics、trace、Verifier输出和fault timeline。

- [ ] **Step 6: 独立提交**

```bash
rtk git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/docs/thread_model_cn.md docs/relay_win.md src/CMakeLists.txt docs/test
rtk git commit -m "docs(windows): close relay lifetime remediation gates"
```

## 5. 系统级端到端功能图

```text
HTTP CONNECT / SOCKS5 / port-forward client
  -> TqTunnelContext + per-start TqRelayStopControl(immutable generation N)
  -> TqStreamLifetime + stable router + terminal retention
  -> Windows prepared registration token owns TCP socket disposition
  -> publish Windows binding or worker-independent failure/stop sink
  -> commit validation + first WSARecv
  -> TCP <-> QUIC forwarding
       TCP->QUIC: WSARecv/GQCS -> API lease -> send registry -> SEND_COMPLETE
       QUIC->TCP: callback copy -> WSASend/GQCS -> receive lease -> ReceiveComplete
  -> graceful close / hard error / activation failure / worker stop
  -> owner shutdown ledger requests shutdown; local IOCP drains independently
  -> real SHUTDOWN_COMPLETE is the only wrapper terminal publication
  -> active binding posts terminal record, or stopped worker route uses shutdown sink
  -> control Stop(generation N) -> tunnel reaper/public stop
  -> owner/control/registry/command/OVERLAPPED/buffer/context all zero
```

| 链路 | 必测断言 | 证据 |
|---|---|---|
| ingress -> control | 每次start使用新immutable generation | handle reuse test、control trace |
| prepare -> publish | socket disposition明确，data plane未提前启动 | prepared token counters |
| publish -> commit | failure提交shutdown但不伪terminal | shutdown call log、phase snapshot |
| TCP -> QUIC | GQCS后才释放recv buffer，send key once-only claim | operation destructor、registry metrics |
| QUIC -> TCP | terminal/abort discard零stream API，active complete一次 | fake API counters、buffer hash |
| active close | 本地退役不释放terminal retention | terminal barrier、weak owner |
| worker stop | command先完成/失败，本地IOCP drain，route切sink | stop timeline、pending command=0 |
| late terminal | worker销毁后sink处理，不访问worker | Verifier、endpoint counter |
| terminal -> reaper | control/accounting/context只归零一次 | generation、destructor、active metrics |

## 6. 系统测试策略与覆盖矩阵

| ID | 场景 | 层级 | 主要门禁 |
|---|---|---|---|
| R01 | Windows全量Release/Debug编译 | build | 0 compile/link failure |
| R02 | active close本地先归零、terminal被阻塞 | deterministic concurrency | owner retained、wrapper存活 |
| R03 | worker stop后late terminal | Verifier/concurrency | sink收敛、0 worker UAF |
| R04 | terminal先于late TCP completion | IOCP concurrency | terminal立即、operation随后once-only归还 |
| R05 | prepare前/IOCP关联后失败 | transaction | socket disposition正确 |
| R06 | publish后activation failure | transaction | AbortBoth一次、无伪terminal |
| R07 | shutdown API failure后真实terminal | resilience | retention不释放、最终归零 |
| R08 | terminal与commit最终check竞争 | deterministic concurrency | 单一结果、无半handle |
| R09 | queued register command与stop | deterministic concurrency | waiter返回、0 raw pointer |
| R10 | queued snapshot/trace/close与stop | deterministic concurrency | completed或Stopped一次 |
| R11 | same-address handle reuse | ASan/generation | 旧event不影响新relay |
| R12 | real pending WSARecv cancellation | integration | GQCS后释放、stop归零 |
| R13 | pending WSASend/backpressure stop | integration | 0 hang、buffer/accounting归零 |
| R14 | half-close/zstd/fatal-send/recv-pool | integration | payload与close语义保持 |
| R15 | HTTP CONNECT/SOCKS/port-forward | E2E | payload hash 100%、结束归零 |
| R16 | runtime stop与1000 active tunnel | capacity | 受控断开、无stuck owner/IO |
| R17 | 30分钟short-tunnel churn | soak | oldest age/registry无增长 |
| R18 | UDP分区/进程重启后恢复 | resilience | 新tunnel在RTO内成功 |

## 7. k6性能基线

复用既有HTTP CONNECT拓扑，修复前基线必须来自可编译且没有已知P0生命周期缺陷的最近稳定
Windows版本；若不存在，先以修复后首个候选版本建立T0，再对后续优化做T1对比，不能拿当前坏版本
作为可接受正确性基线。

| Scenario | 流量模型 | 负载 | 时长 | 断言 |
|---|---|---|---|---|
| baseline-short | constant-arrival-rate | 20 req/s | 5m | error <0.1%，p95 <500ms，p99 <1s |
| expected-peak | ramping-arrival-rate | 20 -> 100 -> 300 req/s | 各5m | dropped=0，registry稳定 |
| spike | constant-arrival-rate | 500 req/s | 60s | 无崩溃/死锁，受控拒绝 |
| steady-active | constant-vus | 100、1024 VU | 各10m | active tunnel匹配，结束归零 |
| churn-soak | constant-arrival-rate | 100 req/s | 30m | owner/command/IO oldest age不增长 |
| breaking-point | staged | 每级+250 req/s | 每级3m | 找到受控饱和点，无silent corruption |

**请求与数据模型：** 80% 1 KiB、15% 64 KiB、5% 1 MiB；short-tunnel设置
`noConnectionReuse: true`，另跑keep-alive长连接；目标分布至少100个origin endpoint，QUIC
connection pool分别为1/4/16，覆盖冷启动与预热状态。依赖不使用stub，除非专门的fault case。

**环境要求：** 固定Windows版本、CPU/RAM、NIC、worker count、证书、配置、origin、网络条件、
git SHA和binary SHA256；T0/T1同机交替至少3轮，保存k6 summary和Windows性能计数器。

**性能门禁：** p95/p99回归<=5%，吞吐回归<=3%，Windows worker CPU增幅<=5%，
每active stream额外内存不高于基线+1 KiB，`dropped_iterations==0`，成功workload error<0.1%。

## 8. 容量、异常条件与恢复验证

### 8.1 容量梯度

- active tunnels：100 -> 1,024 -> 4,096。
- handoff/registration rate：100 -> 500 -> 1,000/s。
- pending overlapped recv/send per worker：100 -> 1,000 -> 4,000。
- worker count：1 -> CPU/2 -> CPU数。
- Admin snapshot频率：1/s -> 10/s -> 100/s，并与registration/stop并发。
- 每级结束后30s内，owner retention、local-retired-wait-terminal、control command、send registry、
  pending receive、overlapped、sink和active relay必须归零。

### 8.2 异常与恢复矩阵

| 场景 | 触发/注入 | 用户影响 | 检测信号 | 缓解/恢复 | 验收 |
|---|---|---|---|---|---|
| activation失败 | 首个WSARecv/commit fault hook | 单tunnel open失败 | activation/shutdown metric | sink + AbortBoth + IOCP drain | 无handle，真实terminal后归零 |
| shutdown API失败 | fake MsQuic返回失败 | 单tunnel延迟关闭 | desired!=submitted、owner age | 保留retention并告警 | 不提前析构，terminal后归零 |
| terminal IOCP post失败 | 关闭/故障IOCP post | 单tunnel关闭 | terminal post failure | worker-independent sink | callback不等待，control一次 |
| pending command + stop | barrier阻塞worker | control请求返回Stopped | pending command age | typed envelope唤醒 | 0 UAF/deadlock |
| CancelIoEx NOT_FOUND | 无pending IO关闭socket | 无可见影响 | cancel_not_found | 正常继续close | stop成功、非fatal |
| cancel/real completion竞争 | 对端同时发送/关闭 | 单tunnel结束 | completion error/source | 按当前disposition settle | operation一次归还 |
| UDP网络分区 | drop UDP 60s | 在途tunnel超时 | terminal retention oldest age | MsQuic timeout/reconnect | 恢复后30s内新tunnel成功 |
| CPU/内存压力 | CPU 95%/限制RSS | 延迟升高、受控拒绝 | queue/CPU/RSS | backpressure、rollback | 无死锁/损坏，解除后恢复 |
| runtime worker stop | stop 1个/all workers | active tunnel断开 | stop timeline/sink | drain IOCP、route sink | RTO<=60s，新tunnel成功 |
| 进程崩溃重启 | kill process + supervisor | 在途tunnel丢失 | health check | restart/client reconnect | RTO<=60s，RPO=0 |

本功能不持久化业务数据，故RPO=0表示无服务端持久状态丢失；在途TCP/QUIC连接允许重建，
但不得发生payload silent corruption。每个异常恢复后统一执行Admin health、10个新HTTP CONNECT、
payload hash校验，并检查所有lifecycle metrics在30s内归零。任一object age>30s时保存dump/trace并失败，
不得强制发布terminal掩盖问题。

## 9. 实施进入、退出与发布门禁

### 9.1 进入条件

- 保留原评审中的6个问题和当前`#if 0`列表作为基线，不删测试、不降低断言。
- Windows构建环境、符号、MsQuic测试依赖和socket pair能力可用。
- fault hook只在`TQ_UNIT_TESTING`/测试构建可用，production默认关闭。
- 每个并发测试都能用barrier指出线性化点和预期赢家。

### 9.2 每个Task退出条件

- 新测试在修复前稳定失败、修复后连续运行100次通过。
- 该Task涉及的owner/control/socket/operation/buffer/accounting均有once-only与最终归零断言。
- Windows focused target通过，受影响的公共Linux/Darwin target至少编译通过。
- `rtk git diff --check`通过；不把已知UAF/hang路径留给下一Task再兜底。

### 9.3 最终发布门禁

- R01-R18和既有F01-F18全部通过。
- Windows Release/Debug、ASan或Application Verifier为0 failure、0 UAF、0 double free、
  0 invalid handle、0 lock misuse、0 payload corruption。
- 原计划范围内`windows_relay_worker_test.cpp`不存在因crash/deadlock/hang而保留的`#if 0`。
- active close、activation failure、terminal post failure、explicit stop和runtime stop后，
  owner/control/socket/operation/context/accounting均恰好一次收敛。
- soak结束30s内terminal retention、local-retired-wait-terminal、pending command、send registry、
  pending receive、overlapped、shutdown sink、buffer和active relay全部为0。
- k6 p95/p99<=5%回归、吞吐<=3%回归、worker CPU<=5%增幅、dropped=0。
- 异常恢复后10个新tunnel成功，process/runtime restart RTO<=60s，RPO=0。

## 10. 风险、假设与开放问题

- **本地relay可先退役：** 需要把terminal-capable sink/control state从`RelayContext`中抽离；
  若短期无法安全抽离，可保留轻量relay shell，但不得保留socket/worker pointer或阻塞active metrics。
- **owner-target引用环：** sink不得强持owner。若sink需要调用shutdown，只能在切换前由active路径提交，
  或持不形成环的weak owner并接受terminal后lock失败。
- **shutdown API重试：** 当前公共ledger记录desired/reserved/submitted。API failure是否自动重试需遵循
  公共设计；本计划至少要求intent和retention不丢，不允许Windows私有无限重试。
- **IOCP post顺序：** 不依赖未文档化的跨线程FIFO；pending command registry + typed envelope是
  correctness来源，begin-stop marker只用于唤醒worker。
- **真实pending WSASend稳定性：** 优先使用可控backpressure；若CI不能稳定复现，允许测试provider，
  但最终Verifier/system test仍需一次真实Winsock路径证据。
- **公共handle兼容性：** 删除`WindowsWorker`可能影响测试或外部source usage；若存在ABI约束，
  先保留不可解引用的reserved字段并禁止赋值，但最终production capability必须消失。
- **系统测试基线：** 当前HEAD有P0，不能作为正确性基线；需要选择最近稳定Windows SHA或先建立
  修复后T0，并在证据中明确限制。

## 11. 最终检查清单

- [ ] 3个P0和3个P1均有修复前失败测试、实现修复和回归证据。
- [ ] immutable generation在Windows production/test中无atomic误用。
- [ ] `PublishTerminalAndTakeTarget()`只可由真实terminal router路径调用。
- [ ] active close和worker stop在真实terminal前保留owner/wrapper，late terminal命中安全sink。
- [ ] publish后activation failure提交一次shutdown且不伪terminal。
- [ ] 所有已post control command在stop前完成或显式失败，无stack raw pointer。
- [ ] public handle/result不保存可解引用的Windows worker capability。
- [ ] 所有真实Winsock/data-path legacy用例恢复，测试文件无相关`#if 0`。
- [ ] focused、Verifier/ASan、R01-R18、F01-F18、k6、capacity、soak和恢复门禁通过。
- [ ] owner/control/command/registry/receive/overlapped/sink/buffer/context在30s内归零。
- [ ] config、git/binary SHA、metrics、trace、Verifier和fault timeline证据已归档。
- [ ] `rtk git diff --check`通过，`docs/relay_win.md`和线程模型与实现一致。
