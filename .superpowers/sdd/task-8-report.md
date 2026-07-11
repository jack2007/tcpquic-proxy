# Task 8 实施报告

## 状态

已完成 Linux 确定性 terminal convergence 回归 target：`tcpquic_linux_terminal_convergence_test`。

## TDD 证据

1. RED：首次执行 `rtk cmake --build build --target tcpquic_linux_terminal_convergence_test -j$(nproc)`，构建系统返回 `No rule to make target`，退出码 2。
2. GREEN：增加 test-only hooks、八场景测试表与 CMake target 后，新 target 构建成功，八个 case 均逐项输出 `PASS`。
3. 回归：组合构建并运行新 target 与 `tcpquic_linux_relay_worker_io_test`，两个进程退出码均为 0。

## 实现摘要

- 在 `TQ_UNIT_TESTING` 下增加 terminal reserve、downcall、submit 三个无 worker/control mutex 的边界 hook。
- 增加 terminal callback queue-full 与 callback suppression 故障注入开关；production 预处理结果不保留这些字段和分支。
- 新测试通过真实 `TqLinuxRelayWorker`、`TqStreamLifetime` owner、handoff control、socketpair RST 与 worker TCP event API 驱动 fatal convergence。
- 每个 fatal 场景验证 `ABORT|IMMEDIATE`、handoff release facts、relay/terminal sink/timeout/send completion/retention 资源归零和 exactly-once 指标。
- half-close 负向场景验证 graceful peer FIN 不产生 `ABORT` 或 `IMMEDIATE`。
- 所有竞态推进均使用同步 hook、直接事件分派或 fake scheduler API；未使用 `sleep`。

## 验证命令

```bash
rtk cmake --build build --target tcpquic_linux_terminal_convergence_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_terminal_convergence_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk git diff --check
```

## 关注点

- 本任务未增加 production 行为；故障注入全部受 `TQ_UNIT_TESTING` 限制。
- 工作区原有其他未提交改动未触碰，也未纳入本任务提交。

## Reviewer 返工（2026-07-12）

原提交的七个 case 只是同一 `RunFatal` 的别名，且 worker 的三个 hook 均位于真实
`BeginTerminalShutdown` downcall 之外，不能证明 brief 中的竞态。本次返工删除该
fixture，八个入口分别驱动独立场景。

- terminal 边界 hook 移入 `TqStreamLifetime` 状态机：reserve 已提交、真实 shutdown
  downcall 内、submit 状态落定后；三个回调均在 `ControlMutex_` 外执行。
- 新增仅测试编译的 Linux tunnel fixture，直接构造 production `TqTunnelContext`、
  调用 `StartRelay` 并注册真实 reaper；`ReapReadyForTest` 与 reaper 线程共享同一个
  predicate/提取实现，实际调用 `TqReapTunnelContext`。精确 case 断言 release-ready
  前析构计数为 0，三项 handoff facts 成立后 reaper delete 恰好一次。
- 所有 TCP fixture 改用 `AF_INET/INADDR_LOOPBACK`。fd/relay generation 复用、queue-full
  owner release、四方 barrier、5s/30s fake-clock、重复事件和完整 half-close 均为独立测试。
- half-close case 由真实 worker 读取 TCP EOF、提交并完成 QUIC FIN，随后继续把反向
  payload 写回 TCP；peer FIN 最终触发 `SHUT_WR`，并断言无 abort/immediate/watchdog。
- 每个 case 结束后统一检查 terminal sink、timeout、send completion、terminal
  retention、exactly-once 指标回到基线；RAII 清理 MsQuic、scheduler、worker 和 socket。

返工 RED/GREEN 证据：先引用不存在的真实 owner boundary API，目标按预期编译失败；
实现边界后逐项推进运行期 RED，最终八项均打印 `PASS`。组合回归运行 Linux IO、
tunnel reaper、tcp tunnel 三个测试进程，退出码均为 0。

## Reviewer 第二轮返工进度（2026-07-12）

- reaper boundary 已改成三个 boundary 各自用 barrier 停住真实 owner 状态机，测试线程
  在每个停点实际调用 `ReapReadyForTest()` 并验证返回 0；handoff 三事实完成后第四次
  调用实际删除 context 且析构计数恰为 1。
- fd reuse 已为新 relay 增加独立 owner/control generation；旧 epoll token 和旧 owner
  terminal 都到达后，新 relay、owner 和 control 均保持存活。
- suppression 增加仅测试编译的 worker snapshot 计数；真实 worker 配置 suppression，
  fatal shutdown downcall 内到达的 terminal callback 命中该分支，随后验证 5s/30s
  watchdog 和 manual terminal cleanup。
- 本轮 focused target 构建并运行通过八项；尚未声称 reviewer 第二轮全部完成：精确双端
  production tunnel 链、queue-full actual tunnel reap、duplicate context/reaper、四方 fatal
  同起跑以及全 case baseline/RAII 仍需 worker override fixture 注入后继续实现。

## Reviewer 第二轮返工完成（2026-07-12）

- 在 production `TqRelayStartManaged` 的 Linux worker 选择点增加仅测试可见的可选 worker
  override；`TqTunnelContext::StartRelay` fixture 仍复用同一 registration prepare、commit、
  publish、handoff 和 active accounting 路径，没有复制 relay 启动逻辑。
- 精确事故链使用两个 production `Start()` worker、两个真实 owner/context 和两组 loopback
  TCP：client FIN、FIN send completion、server receive FIN、target `SHUT_WR` EOF、真实
  `SO_LINGER{1,0}` RST/`ECONNRESET`、server `ABORT|IMMEDIATE`、actual reaper delete、
  server terminal、client peer abort/terminal 和第二次 actual reaper delete全部按序验证。
- 四方 fatal 竞态使用 shared-owned typed async control envelope；worker admission 后在不持
  `ControlLock` 的 completion barrier 等待，测试确认 post 后才释放 RST。TCP、admin、
  worker stop 和 connection terminal 四路从同一 barrier 起跑，所有 completion 在外部等待。
- queue-full 与 duplicate case 均改为指定 production worker 的真实 `TqTunnelContext`；
  queue-full 后注入 111 个 late terminal events 并由实际 reaper 删除，duplicate abort/stop/
  terminal/reaper 最终析构恰好一次。
- 每个 case reset 前先断言 active relay、terminal sink/timeout、send completion 和 retention
  registry 为零；fixture RAII 停 worker、发布 terminal、关闭自有 fd 并恢复 scheduler/MsQuic。
- 新 target 在硬超时保护下连续运行三次，三次均八项 PASS；Linux IO 与 tunnel reaper
  进程退出码均为 0，`git diff --check` 无输出。
- `tcpquic_tcp_tunnel_test` 当前退出码 149，定位为其首个既有
  `TestTerminalLookupFailureRejectsBeforeCallbackInstall` 返回 7061；本轮未完成基线 commit
  的独立构建验证，因此不把该非零退出归因于既有问题，也不将其记录为通过。

## Reviewer 第三轮返工完成（2026-07-12）

- 所有新增 worker override、async event/envelope/completion、tunnel helper 和函数测试参数均
  完整置于 `TQ_UNIT_TESTING`（tunnel helper 额外要求 `TCPQUIC_TUNNEL_TESTING` 与 Linux）
  门禁内；production `TqTunnelContext::StartRelay` 与 relay 内部签名不含测试参数。
  `tcpquic-proxy` production target 已从头重编译成功。
- 四方 fatal case 改为真实 context 的四个独立入口：已 admitted 的 async TCP fatal、直接
  复用 production `DrainFromAdmin()` 核心的 admin 入口、production worker queue 上的
  async unregister、以及不等待 fatal completion 的 owner terminal callback。四线程从同一
  barrier 释放，completion 只在外部等待，actual reaper 删除 context 恰好一次。
- suppression case 在 registration 前安装目标 connection escalation，并断言 5s watchdog
  只 shutdown 目标 connection 一次，30s 后仍为一次；独立的另一 connection escalation
  始终为零，stream close 始终为零。
- 双端事故使用共享 fake transport：`CaptureSend` 自动接收 client FIN/key，transport pump
  自动完成 client send 并投递 server receive FIN；fake `StreamShutdown` 自动观察 server
  abort，transport pump 自动投递 client peer abort。测试只驱动 transport pump，并断言
  `client_fin → client_fin_complete → server_peer_fin → server_abort → client_peer_abort`
  因果序列，不再由测试主体分别伪造两端 QUIC 事件。
- `TcpPair` 改为 move-only RAII；A/C/D/G 的真实 context 增加 scope guard，CHECK 早退时按
  terminal owner、unregister/stop worker、reaper/delete、owner reset 的顺序幂等清理。
  guard 均声明在 worker/owner 之后并在成功路径显式 dismiss，析构时不会访问已销毁对象。
- 项目没有独立的全局 active-tunnel/close-wait test counter。本测试用每个真实 context 的
  actual reaper/destructor 计数作为 active-tunnel 替代观测，用精确事故 target EOF 与
  half-close 最终 EOF 作为 close-wait 替代观测；main 额外断言全局 active relay 为零。
- 最终门禁：production target 构建成功；新 target 在 `timeout -k` 下连续三次均八项 PASS；
  Linux IO 与 tunnel reaper 均退出 0；`git diff --check` 无输出。

## Reviewer 第四轮 RAII/并发安全返工（2026-07-12）

- handoff boundary 的 fatal thread 在创建后立即安装 scope guard；任一后续 CHECK 早退都会
  先把三个 boundary 全部 release、notify，再 join。正常路径显式 join 后 dismiss，不存在
  joinable thread 析构或 hook 永久阻塞。
- `TcpPair` 为 move-only RAII。精确链的两个 heap stream 先由 `unique_ptr` 持有，只有
  `InstallStreamForTest` 成功后才把所有权交给 owner；client/server context、committed state
  都以 nullable 状态进入 cleanup guard，guard 在第一次 context 创建之前已建立，可处理
  client 成功/server 失败、registration 未 publish、单边 committed 等 partial failure。
- fake transport 发布改为 atomic acquire/release。transport 本体由声明早于 cleanup guard 的
  shared keepalive 持有；cleanup 先 unpublish，随后 terminal/unregister 并 Stop/join 两个 worker，
  确认任何已 acquire 裸指针的 callback 已退出后，才 reset owner 与 transport keepalive。
- C/D/G 的 cleanup guard 均移动到 context 创建返回后、第一个 CHECK 之前；committed state
  可为空，cleanup 仅在已 publish 时 unregister，随后 Stop worker、actual reaper，并仅对仍未
  析构的 context 执行 fallback。成功路径全部显式 dismiss，cleanup 幂等且析构顺序不会访问
  已销毁的 worker/owner。
- 对所有 CHECK 失败点做了 partial-state 审查：socket 创建由 `TcpPair` 回收；stream install
  前由 `unique_ptr` 回收；context 创建/第二端创建/commit snapshot/async admission/transport
  pump/terminal/reaper 任一点失败，均已有先于该 CHECK 建立的 owner/thread/context guard。
- 修复后新 target 在 `timeout -k` 下再次连续运行三次，三次均八项 PASS。
