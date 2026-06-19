# Callback-Driven Client Reconnect 设计方案

> 日期：2026-06-19
> 状态：设计完成 / 已实现
> 参考：`docs/thread-model_cn.md`、`src/protocol/quic_session.cpp`、`src/ingress/client_ingress_reactor.cpp`、`docs/superpowers/specs/2026-06-18-thread-model-cares-design.md`。

## 1. 背景

改造前，client 每个 `QuicClientSession` 都启动一个 reconnect thread：

- single-peer client 有 1 个 `QuicClientSession`，因此有 1 个 reconnect thread。
- multi-peer client 每个 active peer 有自己的 `QuicClientSession`，因此 reconnect thread 数量随 peer 数线性增长。
- reconnect thread 周期调用 `EnsureConnected(100ms)` 和 `StartAllDueSlots()`，补齐断开的 QUIC connection slot。

这个模型在 multi-peer 场景下线程成本偏高。QUIC 连接状态变化本身已经由 MsQuic connection callback 通知，常规断线不需要额外线程轮询发现。

同时，client 入口已经迁移到 `TqClientIngressReactor`：

- 所有 peer 的 SOCKS5 / HTTP CONNECT listen、accept、handshake 由一个 ingress reactor 线程处理。
- peer 只有在 QUIC connected count 大于 0 时才开放 ingress listen。
- 断连到 connected count 为 0 时关闭该 peer ingress。

本文档描述的是 Linux / Windows / macOS 公共机制。三平台共享同一套 `QuicClientSession` reconnect、`TqClientIngressReactor` delayed task、ingress listen gating 和 speedtest ingress 数据路径；平台差异只在底层 socket reactor 与 relay worker 后端，开发完成后各平台分别做验证。

## 2. 目标

1. 删除每个 `QuicClientSession` 一个 reconnect thread 的模型。
2. QUIC 异步断开后，由 MsQuic callback 驱动对应 slot 立即重建。
3. `ConnectionOpen` / `ConnectionStart` 同步失败时不无限立即重试，而是由 client ingress reactor 的 delayed task 定时触发 retry。
4. 不把重连定时器放在 MsQuic callback 线程里等待。
5. 删除旧的 reconnect interval 配置语义，固定 retry delay 只作为防 tight loop 的内部常量。
6. 删除 warmup 功能。
7. speedtest 启动遵循普通 client ingress gating，测速数据连接通过 HTTP CONNECT ingress 进入；控制面继续使用既有 speed control stream。
8. 没有成功 QUIC connection 的 peer 不开放 SOCKS5 / HTTP CONNECT listen 端口。

## 3. 非目标

- 不改变 MsQuic 内部 worker / datapath 线程模型。
- 不把 QUIC reconnect 逻辑放到 relay worker。
- 不实现指数退避。同步 start 失败使用固定延迟 retry，避免 tight loop。
- 不改变 server 侧 dial reactor / relay worker 线程模型。
- 不改变 SOCKS5 / HTTP CONNECT 协议语义。

## 4. 总体设计

### 4.1 重连职责拆分

新模型把重连拆成两个触发源：

1. **异步 QUIC 断开**
   MsQuic callback 收到 `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE` 后，清理旧 connection context，并立即重建同一 slot。

2. **同步 start 失败**
   `ConnectionOpen` 或 `ConnectionStart` 在调用栈内同步失败时，不在同一调用栈里无限重试。`QuicClientSession` 调用注入的 delayed scheduler，把 retry 任务投递给 `TqClientIngressReactor`。常规 client 下该 delayed task 由对应共享 ingress reactor 线程执行：single-peer 是一个 client runtime 一个 reactor；multi-peer 是每个 client runtime adapter 一个共享 reactor，不是每个 peer 或每条 connection 一个重连线程。

这样常规 multi-peer client 的 reconnect 后台线程数从 `P` 降为 `0`；低频定时 retry 复用已有的 ingress reactor 线程。

### 4.2 QUIC callback 路径

`QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT` 和 `QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER`：

- 继续 abort 属于旧 connection 的 tunnels。
- 继续调用 `OnSlotDisconnected()` 更新 connected count。
- 继续通知上层 connection state handler。
- 不在这两个事件中重建 connection。

`QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE`：

- 继续 abort tunnels、unregister trace、标记 disconnected。
- 清理旧 `slot.Context`。
- `state->StateChanged.notify_all()`。
- 调用 `connection->Close()`。
- `DropOrphanedConnection(state, connection)`。
- 删除旧 `slotContext`。
- 如果 session 仍 `Started && !Stopping`，立即触发同一 slot 的 restart。

只在 `SHUTDOWN_COMPLETE` 后重建，避免旧 connection 仍在 shutdown 流程中时和新 connection 复用同一 slot 产生交错。

### 4.3 同步失败 retry

`StartSlot(index)` 可能同步失败：

- `MsQuicConnection` 构造失败，即 `ConnectionOpen` 失败。
- `ConnectionStart` 返回 `QUIC_FAILED(status)`。
- 运行时配置刷新失败。
- 禁用 1-RTT 加密参数设置失败。

这些失败不应在同一调用栈中立即无限 retry。新设计：

- slot 保持 disconnected。
- 记录 `LastStartError` / `StartFailed` 状态用于测试和日志。
- 如果 session 配置了 delayed scheduler，则投递一个固定延迟 retry。
- 固定延迟建议为 `100ms`，只为防止 CPU tight loop，不表达退避策略。
- 如果没有 delayed scheduler，`EnsureAnyConnected()` 仍可同步尝试一次补齐，主要服务非 ingress 测试或过渡路径。
- 固定延迟不读取旧 reconnect interval 配置；该配置表面随 reconnect thread 一起删除，避免保留无效配置。

### 4.4 Delayed scheduler 注入

`QuicClientSession` 不直接依赖 `TqClientIngressReactor` 类型，而是暴露调度回调：

```cpp
using DelayedTaskScheduler =
    std::function<bool(std::chrono::milliseconds delay, std::function<void()> task)>;

void SetDelayedTaskScheduler(DelayedTaskScheduler scheduler);
```

常规 client runtime 将 scheduler 绑定到对应 ingress reactor：

- single-peer：绑定到进程唯一 `TqClientIngressReactor`。
- multi-peer：绑定到共享 `TqClientIngressReactor`。

`QuicClientSession` 内部只调用 scheduler，不 include ingress header。

常规 runtime 必须先启动 ingress reactor、注入 scheduler 和 connection state handler，再调用 `QuicClientSession::Start()`。这样初始 `ConnectionOpen` / `ConnectionStart` 同步失败也能进入 delayed retry；如果先 `Start()` 后注入 scheduler，启动阶段的同步失败会退化成只能等待外部触发。

Delayed retry task 和 `SHUTDOWN_COMPLETE` callback 都不得直接持有裸 `this` 作为跨线程生命周期依据。`QuicClientSession` 保留一个轻量 `ClientSessionGate`：

```cpp
struct ClientSessionGate {
    std::mutex Lock;
    std::condition_variable Drained;
    QuicClientSession* Session{nullptr};
    size_t ActiveCalls{0};
};
```

`ClientConnContext` 和 delayed retry lambda 捕获 `std::shared_ptr<ClientSessionGate>`。执行 restart 前通过 gate 获取 live session 引用并递增 `ActiveCalls`，随后释放 gate lock 再调用 `StartSlot()`；调用结束后递减 `ActiveCalls` 并唤醒 `Drained`。`Stop()` / 析构路径先锁 gate 把 `Session` 置空，再等待 `ActiveCalls == 0`，最后关闭 slot 和清空 scheduler。这个 gate 不提供所有权，只用于把迟到 retry / callback 与 Stop/destructor 串行化，同时避免持有 gate lock 调用 `StartSlot()` 造成锁顺序问题。

### 4.5 Ingress reactor delayed task

`TqClientIngressReactor` 当前已有：

- worker thread。
- `PendingTasks` 立即任务队列。
- `EnqueueAsync()` 跨线程投递任务。
- `Reactor.RunOnce(50)` 的周期唤醒。

新增 delayed task queue：

```cpp
struct DelayedTask {
    std::chrono::steady_clock::time_point Due;
    uint64_t Order{0};
    std::function<void()> Task;
};
```

新增接口：

```cpp
bool EnqueueDelayed(std::chrono::milliseconds delay, std::function<void()> task);
```

`Run()` 每轮处理：

1. `ProcessPendingTasks()`
2. `ProcessDueDelayedTasks()`
3. 根据最近 delayed task 的 due time 计算 `RunOnce(timeoutMs)`，最大仍限制在 50ms

Stop 时清空 delayed tasks，避免 peer/session 已停止后迟到 retry。

### 4.6 Peer ingress 开放规则

peer ingress 的状态只由 QUIC connected count 决定：

- connected count 从 0 变为大于 0：开放该 peer 的 SOCKS5 / HTTP CONNECT listen。
- connected count 变为 0：关闭该 peer 的 SOCKS5 / HTTP CONNECT listen，并清理该 peer 下 still-owned handshake / opening client sockets。
- peer 从未连接成功：不开放 listen 端口。
- reconnect 成功后重新开放 listen。

这个规则适用于 single-peer 和 multi-peer。

### 4.7 删除 warmup

删除 warmup 主路径：

- CLI / JSON 中 warmup 配置不再生效。
- `RunSinglePeerClient()` 不再在 ingress runtime 前执行旧 warmup 流程。
- 旧 warmup runtime 文件、相关 CMake target、相关单测可移除。
- 文档中删除 warmup thread / warmup 流程描述。

删除 warmup 后，client 启动只有 ingress runtime 和 QUIC state handler 控制端口开放。

### 4.8 删除 reconnect interval 配置

删除旧 reconnect interval 配置输入和内部字段，包括 CLI、single-peer JSON、router peer JSON 和对应 config 结构体字段。

固定 retry delay 是实现常量，不对外暴露，不作为性能调参项。旧配置如果继续出现在 CLI / JSON 中，parser 应返回明确错误，避免用户误以为它仍控制重连节奏。

### 4.9 Speedtest 走 ingress

speedtest 不再通过旧的直连 client speedtest API 绕过 ingress。新的约束：

- speedtest client 启动必须遵循对应 peer 的 ingress gating。
- 若该 peer 没有 connected QUIC connection，listen 端口不开放，speedtest 无法启动代理流量。
- 测速数据连接通过 `cfg.HttpListen` 上的 HTTP CONNECT ingress 路径进入。
- 控制面仍使用既有 speed control stream，用于发送 speed-test start/finish 和获取 server 侧 loopback 目标。因此不要把该路径描述成所有 speedtest 流量都完全通过 HTTP CONNECT。

这保证 speedtest 的数据面经过真实 client ingress 和 listen gating，避免专门路径隐藏 ingress/reconnect 问题。

## 5. 生命周期

### 5.1 Start

`QuicClientSession::Start(cfg)`：

前置条件：常规 runtime 已经启动 ingress reactor、注入 delayed scheduler 和 connection state handler。

1. 初始化 MsQuic API / registration / configuration。
2. 初始化 `Config.QuicConnections` 个 slots。
3. 不创建 reconnect thread。
4. 调用 `StartAllSlots()` 主动发起初始连接。
5. 返回时不要求所有 slots 已 connected；连接完成由 MsQuic callback 通知。

### 5.2 Stop

`QuicClientSession::Stop()`：

1. 设置 `Stopping=true`。
2. 将 `ClientSessionGate::Session` 置空，并清空 scheduler 或标记 generation invalid。
3. shutdown 所有 slot connections。
4. 等待 orphaned connections drain。
5. 不 join reconnect thread，因为线程已删除。

任何 delayed retry task 执行时必须检查：

- session state 仍存在。
- `Started && !Stopping`。
- slot generation 未过期。

### 5.3 Restart

`RestartSlotAfterShutdownComplete(state, slotIndex)`：

1. 检查 `Started && !Stopping`。
2. 检查 slot index 有效。
3. 确认该 slot 当前没有 valid connected connection。
4. 调用 `StartSlot(slotIndex)`。
5. 若同步失败，则通过 scheduler 投递 fixed delay retry。

## 6. 错误处理

- `ConnectionStart` 返回 `QUIC_STATUS_PENDING`：视为启动已提交，等待 callback。
- `ConnectionStart` 同步失败：记录状态并安排 delayed retry。
- delayed scheduler 不存在：记录状态，不启动后台 retry；后续 `EnsureAnyConnected()` 可尝试。
- Stop 并发：所有 restart / delayed retry 执行前检查 `Stopping`。
- 旧 callback 迟到：通过 slot context 和 connection pointer 匹配，旧 callback 不得覆盖新 connection slot。
- 迟到 delayed task：通过 `ClientSessionGate` 判断 session 是否仍存活，gate 已失效则直接返回。

## 7. 线程模型变化

更新后常规 client：

```text
1 main thread
+ MsQuic internal threads
+ 1 TqClientIngressReactor worker thread
+ R relay worker threads
+ 1 tunnel reaper thread
+ optional admin / trace / speed-test helper threads
```

已删除旧的 per-peer reconnect background thread；常规 client 不再按 peer 数量增加重连后台线程。

client reconnect 行为由：

- MsQuic callback 触发异步断线重建。
- Ingress reactor delayed task 处理同步 start 失败 retry。

## 8. 验收标准

- multi-peer client 不再为每个 peer 创建 reconnect thread。
- 删除旧 reconnect loop 字段、启动函数和 loop 函数。
- 删除旧 reconnect interval 配置字段、CLI / JSON 输入和相关测试期望。
- QUIC 异步断开后，在 `SHUTDOWN_COMPLETE` 后立即启动 replacement connection。
- 同步 `ConnectionStart` 失败不会 tight loop，会通过 ingress reactor delayed task 固定延迟 retry。
- peer 没有 connected QUIC connection 时，SOCKS5 / HTTP CONNECT listen 端口不开放。
- reconnect 成功后，对应 peer ingress 自动开放。
- connected count 变为 0 后，对应 peer ingress 自动关闭。
- warmup 功能和文档移除。
- speedtest client 启动遵循普通 client ingress gating，数据连接通过 HTTP CONNECT ingress，控制面仍走 speed control stream。
- Linux / Windows / macOS 使用同一套公共机制；本次开发完成后，三个平台分别完成平台构建、单测和回归验证即可。
