# libuv Relay Backend 集成设计确认稿

> 状态：设计已确认，已进入实现与验证追踪
> 创建日期：2026-07-15  
> 适用范围：`tcpquic-proxy` relay 数据面  
> 说明：本文是持续更新的设计确认清单。每项经确认后，必须把最终结论、约束和确认日期写回本文；全部确认前不进入实施计划。

## 1. 文档目标

项目已将 libuv `v1.52.1` 作为 vendored 静态依赖链接进主程序，但尚未使用 libuv 驱动
relay 运行时。本设计新增一套与 native 源码隔离的跨平台 libuv relay 候选实现，用于功能、
稳定性和性能评测。评测期 native 与 libuv 只在仓库和不同构建目录中临时并存；单个
`raypx2` 构建产物只包含一种 relay 实现。评测结束后删除落选实现，不维护长期双 backend
兼容层。

本文集中记录：

- libuv 与 MsQuic 的线程模型及两者的执行域边界；
- 当前 Linux/macOS relay 注册和生命周期机制；
- libuv worker、控制命令、buffer ownership、背压和终态设计；
- 构建期实现选择、观测、测试、故障恢复和最终取舍策略；
- 所有已确认决策和待确认事项。

## 2. 确认规则与状态

状态定义：

- **已确认**：可以作为后续设计和实施的约束。
- **待确认**：已有推荐方案，但尚未得到确认。
- **待分析**：需要补充代码或官方资料证据后再提出方案。
- **否决**：明确不采用，并记录原因。

逐项确认时，一次只讨论本文一个编号项。确认后执行：

1. 将对应状态改为“已确认”；
2. 把讨论中形成的细节补入对应章节；
3. 在“决策日志”记录日期和结论；
4. 再进入下一项。

## 3. 当前决策总览

| 编号 | 主题 | 状态 | 当前结论/推荐 |
|---|---|---|---|
| D-01 | backend 产品定位 | 已确认 | 评测期源码并存、构建产物互斥、最终二选一并删除落选实现 |
| D-02 | worker 线程模型 | 已确认 | 固定数量 worker；每个 worker 一个线程和一个 `uv_loop_t` |
| D-03 | 压缩与解压归属 | 已确认 | 在 libuv worker/loop 线程内同步执行，与 Linux relay worker 一致 |
| D-04 | DNS 归属 | 已确认 | 继续使用 `TqServerDialReactor + c-ares`，不进入 libuv relay backend |
| D-05 | relay 注册返回模型 | 已确认 | 外部线程投递控制命令并同步等待 worker 完成注册 |
| D-06 | 注册命令同步原语 | 已确认 | 采用 Linux 的 command/result/notify 基本模式，但使用 libuv 同步 API |
| D-07 | 注册等待超时与取消 | 已确认 | 有界等待仅取消尚在排队的命令；开始执行后等待最终 ownership 结果 |
| D-08 | worker 选择与 relay 亲和性 | 已确认 | round-robin 选择 worker，relay 生命周期内不迁移 |
| D-09 | PublishTarget 与 FD ownership | 已确认 | 采用 Darwin 事务模型，publish 成功后不可逆转移 fd ownership |
| D-10 | libuv handle commit 顺序 | 已确认 | `uv_tcp_open` 转移 handle ownership，`uv_read_start` 成功后 Prepared → Active |
| D-11 | precommit RECEIVE | 已确认 | target publish 后、Active 前有界暂存 receive view，once-only settlement |
| D-12 | QUIC→TCP 无压缩路径 | 已确认 | pending receive + payload 零复制，只使用 `uv_write` |
| D-13 | QUIC→TCP 解压路径 | 已确认 | 解压到 relay-owned 新缓存后完成 receive，输出保持到 `uv_write` 完成 |
| D-14 | TCP→QUIC 无压缩路径 | 已确认 | 一次 `uv_read_cb` 对应一次 `StreamSend`，buffer 保持到 SEND_COMPLETE |
| D-15 | TCP→QUIC 压缩路径 | 已确认 | 继续使用现有 compressor/decompressor API，输出保持到 SEND_COMPLETE |
| D-16 | 双向背压 | 已确认 | 复用现有 tuning 字段和高低水位语义，不新增第一阶段配置 |
| D-17 | FIN 与半关闭 | 已确认 | 两个方向独立收敛，完全复用当前 terminal convergence predicate |
| D-18 | 异常和 terminal convergence | 已确认 | Darwin terminal 正确性模型为主，Linux command/local/fallback fast path 为辅 |
| D-19 | worker/runtime 停止 | 已确认 | 有界 graceful drain，超时强制 abort；开发与测试排在正常功能验证之后 |
| D-20 | 构建期实现边界 | 已确认 | 方案 A：相同公共符号、互斥 source set，不抽取 `IRelayBackend` |
| D-21 | 构建选择与回退 | 已确认 | CMake 构建期选择；单产物单 backend；失败不自动回退 |
| D-22 | metrics 与诊断 | 已确认 | libuv 独立指标；只统一性能对比指标，不统一长期 snapshot 类型 |
| D-23 | 功能与竞态测试 | 已确认 | native 基线保持不变；libuv 独立测试，先 Linux、后 macOS/Windows |
| D-24 | Linux 性能评测 | 已确认 | 采用 DGX netem 矩阵，不设自动淘汰线，由负责人依据完整数据判断 |
| D-25 | 评测与退出标准 | 已确认 | 独立构建评测；达标删除 native，不达标删除 libuv |
| D-26 | libuv allocator 适配 | 已确认 | 正常构建在首个 libuv API 前进程级注册 mimalloc；sanitizer 构建允许使用 system allocator |
| D-27 | libuv backend 平台接口边界 | 已确认 | backend 只经 libuv、C++ 标准库、现有 third_party 和项目公共接口访问能力，禁止直接调用 OS 强相关接口 |

## 4. 已确认的基础设计

### 4.1 backend 定位（D-01，已确认）

评测期保留两套源码，但每个构建产物只选择一套：

```text
build/native -> native relay source set -> raypx2
build/libuv  -> libuv relay source set  -> raypx2
```

- native build：Linux 使用 epoll、Windows 使用 IOCP、macOS 使用 kqueue；现有 native worker
  和 `tunnel/relay.cpp` 不修改。
- libuv build：三个受支持平台使用独立的 libuv relay worker，不编译 native relay source set。
- CMake 默认选择 native，避免改变现有构建行为。
- 同一二进制内不提供 `native|libuv` 运行时切换，不混合两套 ownership 语义。
- 通过同一 commit、编译参数、tuning、负载和环境比较功能、稳定性及性能。
- libuv 达标则删除 native；libuv 不达标则删除 libuv。最终仓库只保留一套 relay 实现。

### 4.2 worker 线程模型（D-02，已确认）

采用固定数量 worker 分片：

```text
TqUvRelayRuntime
└── R × TqUvRelayWorker
    ├── 1 × uv_thread_t
    ├── 1 × uv_loop_t
    ├── 1 × uv_async_t
    ├── 1 × worker control/data command queue
    └── N × worker-owned RelayState
```

不采用：

- 单个全局 loop：形成单核数据面瓶颈。
- 每条 relay 一个 loop/线程：线程数随连接数增长。
- 把 libuv loop 嵌入 MsQuic worker：破坏两套运行时各自的线程所有权。

libuv 官方约束：一个 loop 绑定一个线程，网络 I/O 在 loop 线程执行；除明确声明外，loop 和 handle API 不是线程安全的。参考 [libuv Design overview](https://docs.libuv.org/en/v1.x/design.html)。

### 4.3 压缩、解压与 DNS（D-03、D-04，已确认）

libuv worker 等价于当前 Linux relay worker，因此以下数据面工作在 loop 线程执行：

- TCP read/write 状态推进；
- Zstd 压缩和解压；
- relay buffer pool 分配与归还；
- MsQuic send/receive completion；
- 背压、FIN 和 relay 状态机。

当前 Linux 同样在 worker 内执行压缩与解压：

- TCP→QUIC 压缩：`src/tunnel/linux_relay_worker.cpp` 的 `BuildTcpToQuicViews()`；
- QUIC→TCP 解压：同文件的 `DrainCompressedQuicReceiveView()`。

DNS 不属于 relay 数据面。普通 OPEN 继续由：

```text
TqServerDialReactor
  -> ACL
  -> c-ares async DNS
  -> non-blocking TCP connect
  -> relay handoff
```

libuv backend 不调用同步 `getaddrinfo()`，也不接管 c-ares channel。

### 4.4 同步注册返回（D-05，已确认）

生产环境中，Linux/macOS relay 的外部注册调用者主要是：

- client ingress reactor；
- MsQuic connection/stream callback worker；
- 测试线程。

libuv 沿用同步注册语义：

```text
外部调用线程                         TqUvRelayWorker
    │                                     │
    ├─ enqueue RegisterRelayCommand ─────>│
    ├─ uv_async_send()                    │
    ├─ 同步等待                           ├─ RegisterRelayWithIdLocal()
    │                                     ├─ 完成/回滚
    │<──────── result + signal ───────────┤
    └─ TqRelayStartManaged() 返回         │
```

若调用者就是目标 libuv worker，直接调用 local variant，禁止向自己投递后等待。

### 4.5 libuv 同步原语（D-06，已确认）

命令完成机制采用 Linux 的基本模式，但 libuv backend 内使用 libuv 跨平台原语：

| 用途 | API |
|---|---|
| worker thread | `uv_thread_t`、`uv_thread_create()`、`uv_thread_join()` |
| queue/command mutex | `uv_mutex_t` |
| command completion | `uv_cond_t` |
| 可选有界等待 | `uv_cond_timedwait()` |
| loop wake | `uv_async_t`、`uv_async_send()` |

禁止直接使用 pthread、futex、Windows Event/Critical Section 或 macOS 专用同步接口。

`uv_async_send()` 只表示“需要唤醒”，通知允许合并。实际命令由 `uv_mutex_t` 保护的队列承载，async callback 每次把共享队列 swap 到本地后处理。

`RegisterRelayCommand` 持有：

```text
uv_mutex_t
uv_cond_t
Registration
Result
Done
Cancelled
WaitNanos
```

command 由 `shared_ptr` 同时被调用者和队列持有，确保超时、停止和迟到处理不会访问已释放的同步对象。`uv_cond_wait/timedwait` 必须用谓词循环处理虚假唤醒。

## 5. 当前 native backend 注册模型

### 5.1 Linux epoll

Linux 是“外部同步等待 + worker 内部两阶段提交”：

```text
RegisterRelayCommand
  -> worker prepare RelayState/Binding(Prepared)
  -> PublishTarget
  -> publish 成功后转移 fd ownership
  -> epoll_ctl(ADD)
  -> Prepared -> Active
  -> drain precommit RECEIVE
  -> 返回 RegistrationResult
```

PublishTarget 和 epoll commit 之间的 Prepared 窗口允许 MsQuic callback 进入新 binding，因此需要 precommit queue。Linux 注册有 `ControlCommandTimeoutMs`。

### 5.2 macOS kqueue

Darwin 也是外部同步等待，但内部事务更严格：

```text
Prepare identity/RelayState/Binding(Prepared)
  -> PublishTarget
  -> ownership transfer
  -> InstallInactiveTcpFilters
  -> activation mutex 下 Prepared/Terminal 竞争
  -> Prepared -> Active
  -> EnableTcpFilters
  -> exactly-once settle precommit
  -> 返回 RegistrationResult
```

关键机制：

- `PreparedRelayToken` 跟踪 fd、filter、map 的一次性 disposition；
- `Prepared/Active/Terminal/Failed` 四态；
- callback-visible identity 在 PublishTarget 前全部初始化，publish 后不再改写；
- `PrecommitSettled` 保证 receive queue 只能 drain 或 discard 一次；
- Darwin 当前周期性 Wake 并持续等待，没有 Linux 式整体注册 deadline。

### 5.3 对 libuv 的启示

libuv 的注册和 ownership 基线优先采用 Darwin 的事务边界，同时保留 Linux 风格的 command/result 同步返回。等待超时语义单独确认，不能直接混用两边行为。

## 6. 待逐项确认的详细设计

### 6.1 D-07 注册等待超时与取消（已确认）

command 状态显式化为：

```text
Queued -> Executing -> Completed
   └────> Cancelled
```

确认规则：

1. `Queued` 超过 `ControlCommandTimeoutMs`：调用者可取消，worker不得开始注册，`TcpFdConsumed=false`。
2. 已进入 `Executing`：调用者必须等到 `Completed`，因为 PublishTarget 后 ownership 可能转移。
3. runtime stop：尚未执行的命令统一失败并 signal；执行中的命令完成或 rollback 后 signal。
4. 禁止出现调用者因超时关闭 fd、worker又完成注册/关闭同一 fd 的不确定状态。

采用上述状态机，不完全复刻 Linux 当前 `Done/Cancelled` 行为。等待超时只能取消仍处于
`Queued` 的命令；命令进入 `Executing` 后，调用者必须等待 worker 给出最终结果以及明确的
`TcpFdConsumed`，不得自行推断 fd ownership。

### 6.2 D-08 worker 选择与亲和性（已确认）

确认方案：

- runtime 启动时创建固定 `R` 个 worker；
- relay 注册时选择一次 worker；
- relay 生命周期内不迁移 loop；
- 第一阶段使用 round-robin；
- snapshot 记录 active relay、pending bytes 和 loop lag，后续再评估 least-loaded；
- 不因某一 worker backlog 临时升高而迁移已有 `uv_tcp_t`。

第一阶段使用 round-robin，不直接使用 least-loaded。least-loaded 需要先依靠 snapshot 和
实际负载数据验证选择指标，后续作为独立优化项评估。

### 6.3 D-09 PublishTarget 与 TCP fd ownership（已确认）

确认采用 Darwin 风格的不可逆 ownership 转移，线性化规则如下：

- PublishTarget 前失败：fd 仍属于调用者。
- PublishTarget 成功：fd ownership 不可逆转移到 prepared token。
- PublishTarget 后任意 activation/terminal 失败：worker/token 关闭 fd，调用者不得关闭。
- `RegistrationResult` 即使 `Ok=false`，也必须准确返回 `TcpFdConsumed`。
- publish 后不修改 binding 中的 worker identity、relay ID、route generation、control generation、stream owner identity。

fd ownership 与 relay phase 分开建模：

```text
CallerOwned
  -> PreparedTokenOwned   // PublishTarget 成功
  -> UvHandleOwned        // uv_tcp_open 成功
  -> ActiveRelayOwned     // uv_read_start 成功并提交 Active
  -> ClosedExactlyOnce
```

`Prepared` 不等于 fd 仍归调用者所有。`uv_tcp_open()` 成功后即使 relay 尚未进入 `Active`，
fd 也已经由 libuv handle 管理；后续失败必须通过 worker/token 的统一关闭路径收敛。

### 6.4 D-10 libuv handle commit 顺序（已确认）

确认顺序：

```text
Prepare binding/relay/token
-> PublishTarget
-> uv_tcp_init
-> uv_tcp_open(fd)
-> uv_read_start
-> activation mutex: Prepared -> Active
-> settle precommit
```

libuv 没有与 kqueue disabled filter 完全相同的公共抽象。`uv_tcp_open()` 只把已有 socket
关联到 TCP handle，`uv_read_start()` 才启动读取。因此二者分别视为“转移到 inactive
handle”和“启用 read 数据面”。

`Active` 的不变量是：fd 已归 libuv handle 所有，并且 TCP read 已成功启动。若
`uv_read_start()` 失败，phase 保持 `Prepared`，由 prepared rollback 路径执行
`uv_close()`、discard precommit 并返回 `Ok=false, TcpFdConsumed=true`，不得对外短暂发布
`Active`。

若 terminal 在 `uv_read_start()` 成功与提交 `Active` 之间到达，由 activation mutex 决定
唯一胜者：激活胜出则进入正常 Active terminal 路径；terminal 胜出则执行
`uv_read_stop()`、`uv_close()` 和 precommit discard，不再进入 `Active`。

### 6.5 D-11 precommit RECEIVE（已确认）

确认沿用 Darwin 四态和 once-only settlement：

- binding 初始 `Prepared`；
- callback复制 `QUIC_BUFFER` 描述符，不复制 payload；
- 非 FIN-only RECEIVE 返回 `QUIC_STATUS_PENDING`；
- `PrecommitMaxPendingBytes` 限制单 relay precommit bytes；
- activation mutex内复查 phase，处理 callback/commit 竞争；
- Active 胜出：drain 到正常 QUIC→TCP 路径；
- Terminal/Failed 胜出：对全部 pending receive 执行明确的 discard/终止结算；
- `PrecommitSettled` 保证 exactly-once。

不得在 `uv_read_start()` 成功且 activation mutex 提交 `Active` 之前 drain precommit。
QUIC RECEIVE、activation 和 terminal 三者竞争时，precommit 只能整体 drain 或 discard 一次。

### 6.6 D-12 QUIC→TCP 无压缩路径（已确认）

确认采用 payload 零复制，第一阶段只使用 `uv_write()`：

```text
MsQuic RECEIVE
  -> copy slice metadata
  -> return PENDING
  -> worker uv_write
  -> write completion
  -> StreamReceiveComplete(bytes)
```

ownership 规则：

- `QUIC_BUFFER` 数组只在 callback 内有效，必须复制 `{pointer,length}` 描述；
- payload 由 MsQuic 保持到 `StreamReceiveComplete` 或 stream abort；
- `uv_write()` 不复制 payload，底层内存必须保持到 write callback；
- 一个 receive view 可映射为一个或多个 `uv_write()` 请求，但必须按完成结果准确累计 receive bytes；
- 只有对应 payload 不再被任何 pending write 引用后，才能执行 `StreamReceiveComplete(bytes)`；
- 不采用“复制全部 payload 后立即 ReceiveComplete”的简化方案。

第一阶段禁止混用 `uv_try_write()` fast path，避免同时维护同步 prefix 与异步 remainder 两套
completion ownership。后续若性能证据表明有必要，再单独设计和验证 `uv_try_write()` 优化。

### 6.7 D-13 QUIC→TCP 解压路径（已确认）

确认方案：

1. loop 线程消费压缩输入；
2. decompressor 消费输入并把解压结果写入 relay-owned 新缓存；
3. 新缓存取得独立 ownership 后，立即对相应的 MsQuic 输入执行 receive completion，不等待 TCP write；
4. `uv_write()` 完成后释放输出 buffer；
5. 分开统计 `pending_quic_receive_bytes` 和 `pending_tcp_write_bytes`。

第一阶段保留当前压缩接口可能产生的中间 `std::vector -> pooled buffer` 复制，后续另立优化项。

输入 ownership 与输出 ownership 在“解压结果进入 relay-owned 新缓存”时解耦。此后 compressed
receive payload 已消费完成，可以调用 `StreamReceiveComplete()`；解压输出则由对应的
`uv_write_t`/write operation 保持，直到 write callback 或 terminal rollback 完成。若一次解压
只消费部分输入，只能 completion 已被完整消费且不再被 decompressor 引用的输入字节。

### 6.8 D-14 TCP→QUIC 无压缩路径（已确认）

确认方案：

```text
uv_alloc_cb -> relay buffer pool
uv_read_cb  -> StreamSend(buffer)
SEND_COMPLETE callback -> command/fallback lane
worker -> release buffer / update backlog / resume read
```

buffer 必须保持到 SEND_COMPLETE；提交前通过 `TqStreamLifetime::SendCompletionReservation` 预留 completion ownership。

第一阶段一次成功的 `uv_read_cb` 对应一个 send operation，不等待后续 read callback 做跨批次
合并。该行为对应 native backend“一次 `readv()` 批次对应一次 `StreamSend`”的边界；native
允许同一 `readv()` 批次用多个 `QUIC_BUFFER` scatter/gather，但不会合并相邻 `readv()` 批次。

### 6.9 D-15 TCP→QUIC 压缩路径（已确认）

确认与 Linux 一致：

- loop 线程压缩；
- compressor 消费输入后归还 TCP read buffer；
- 压缩输出由 send operation 持有到 SEND_COMPLETE；
- FIN 触发 compressor end-stream flush；
- 第一阶段保留现有 `CompressionOutput -> pooled buffer` 行为。

复用现有 compressor/decompressor API 是第一阶段硬约束，不在 libuv backend 集成中同时
改写压缩接口或引入新的压缩线程模型。

### 6.10 D-16 双向背压（已确认）

TCP→QUIC：

- `MaxBufferedQuicSendBytes` 为高水位；
- `ResumeBufferedQuicSendBytes` 为低水位；
- 高水位或 MsQuic resource error 时在 loop线程执行 `uv_read_stop()`；
- SEND_COMPLETE 使 backlog 低于低水位后 `uv_read_start()`。

QUIC→TCP：

- 分别统计 pending receive input 和 pending TCP output；
- 达到高水位时可靠拒绝/暂停新的 receive；
- 降到低水位后 `StreamReceiveSetEnabled(true)`；
- 考虑 multi-receive 下已有 callback 的额外预算。

第一阶段复用现有 tuning 字段、高低水位和动态 read-ahead 语义，不新增 libuv 专用配置。
libuv backend 只负责把现有的 pause/resume 动作映射为 `uv_read_stop()`、`uv_read_start()` 和
`StreamReceiveSetEnabled()`；指标必须能够区分 TCP→QUIC 与 QUIC→TCP 两个方向的压力来源。

### 6.11 D-17 FIN 与半关闭（已确认）

确认分别建模两个方向：

```text
TCP EOF
  -> 停止 TCP read
  -> flush compressor
  -> QUIC StreamSend(FIN)
  -> 等 SEND_COMPLETE

QUIC FIN
  -> 完成此前所有 TCP write
  -> uv_shutdown(TCP write direction)
  -> 等 shutdown callback
```

只有两个方向及其 pending operation 全部收敛后，才进入正常 terminal release。

完全复用当前 terminal convergence predicate，不为 libuv 放宽条件。至少要求两个方向均已
关闭、QUIC FIN 已提交并完成、pending/in-flight QUIC send、TCP write、QUIC receive 以及
callback/precommit ownership 均已结算，且不存在待处理的 shutdown/sticky 事件，才能发布
正常 stop。单独观察到 TCP EOF、QUIC FIN 或某个 shutdown callback 均不等同于 relay 已收敛。

### 6.12 D-18 异常与 terminal convergence（已确认）

异常 terminal 适用于 TCP reset、libuv read/write/shutdown 失败、QUIC peer abort、buffer
allocation 失败、关键命令投递失败以及 runtime stop 等无法继续正常转发的场景。异常路径可以
跳过 D-17 的正常 FIN 等待并请求 abort，但不得跳过 operation ownership、callback 和
terminal handoff 的收敛。

确认复用：

- `TqStreamLifetime`；
- route/control generation；
- send completion reservation/retention；
- `TqRelayStopControl`；
- terminal ledger 和 escalation；
- shutdown sink；
- active relay accounting once-only release。

#### 6.12.1 主模型：Darwin terminal correctness

libuv backend 采用 Darwin 的显式生命周期和延迟回收模型作为正确性基线：

```text
Prepared
   ├─> Active
   ├─> Terminal
   └─> Failed

Active -> Terminal
```

- activation 与 terminal 在同一个 activation mutex 下仲裁，terminal 胜出后禁止再次提交
  `Active`；
- terminal 首先 seal callback-visible binding，再停止和清理数据面；
- seal 设置 `Active=false`、`Terminal=true`、`Closing=true`，并 exactly-once settle
  precommit RECEIVE；
- publish 后的 relay ID、worker identity、route/control generation、stream owner 和
  completion identity 保持不变；
- callback target 切换到 shutdown sink，继续接收真实 `SHUTDOWN_COMPLETE`、迟到的
  `SEND_COMPLETE`、terminal 后 RECEIVE 和 `CANCEL_ON_LOSS`，但不重新进入 active 数据面；
- 从 active map 移除的 binding、relay、send operation 和 `uv_tcp_t` 进入 retired/closing
  storage，直到 callback refs、completion registry、pending operation 和 `uv_close` callback
  全部结算后才能释放；
- fd 已进入 `UvHandleOwned` 后只通过 libuv handle 的 close 路径关闭，不再使用裸
  `close(fd)` 与 libuv 竞争。

`Closing=true`、stop flag、调用 `uv_close()` 或成功提交 `StreamShutdown()` 都不单独代表
terminal 已完成。

#### 6.12.2 辅助机制：Linux fast path

复用 Linux 的低开销投递机制，但不得绕过上述正确性状态机：

- MsQuic callback 构造 typed command，进入 worker queue 后调用 `uv_async_send()`；
- 若调用者已经是目标 loop 线程，直接调用统一的 local variant，禁止向自己投递后等待；
- 普通 bounded queue 满时，数据事件按背压/失败策略处理；不能丢失的 SEND_COMPLETE、
  terminal handoff 和 shutdown completion 进入可靠 fallback lane；
- `uv_async_send()` 只负责唤醒且允许合并，queue/fallback lane 才是事件事实来源；
- command 携带 relay ID、route generation 和 control generation，stale event 不得作用于
  新 relay；持有 completion ownership 的 stale event 仍须完成资源释放；
- loop-originated error 可以直接走 local fast path，但仍必须进入同一个
  `BeginTerminalLocal()`。

#### 6.12.3 统一异常收敛流程

```text
error/abort/stop detected
-> seal binding: Prepared/Active -> Terminal
-> reject new active-only operations
-> uv_read_stop
-> settle precommit exactly once
-> drain/cancel/transfer pending read/write/send/receive ownership
-> install terminal shutdown sink
-> TqStreamLifetime::BeginTerminalShutdown
-> uv_close(tcp handle)
-> wait uv write/shutdown/close callbacks and MsQuic completions
-> release active accounting and retired storage exactly once
```

若错误来自 MsQuic callback 线程，只允许 seal/enqueue/wake；`uv_read_stop()`、
`uv_shutdown()`、`uv_close()` 及其他 handle API 必须在所属 loop 线程执行。

只有 MsQuic 实际产生的 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 才能把 terminal ledger
推进到 `TerminalObserved`。本地错误、shutdown downcall 成功或 libuv close 均不得伪造该
事件。`TqStreamLifetime` 继续负责 shutdown reservation、提交失败重试、watchdog 和
connection escalation。

异常 terminal 的基础释放条件为：

```text
DataPlaneStopped
&& TerminalHandoffComplete
&& LocalOperationOwnershipTransferredOrDrained
&& uv_close callback completed
```

正常 FIN 继续使用 D-17 的完整 half-close predicate；异常 abort 可以跳过正常 FIN 等待，
但不能跳过 ownership、callback、terminal ledger 和 handle close 收敛。

禁止：

- 伪造 `SHUTDOWN_COMPLETE`；
- terminal 后继续调用 libuv TCP handle API；
- callback 持有 tunnel-owned raw handle；
- publish 后重写 callback identity；
- outstanding send/receive ownership 未结算就释放 binding。

测试必须覆盖 terminal/activation 同时发生、普通队列满、fallback lane 持续受压、
SEND_COMPLETE 晚于 `uv_close()` 调用、重复 terminal、旧 generation callback、shutdown
sink 接管，以及 worker stop 期间 late wake。所有 fast path 最终必须进入统一 terminal
状态机，不得形成第二套释放条件。

### 6.13 D-19 worker/runtime 停止（已确认）

确认顺序：

```text
Running -> Draining
-> 拒绝新注册
-> queued control command 失败并 signal
-> active binding 切换 shutdown sink
-> 停止 TCP read，关闭/abort relay
-> 等本地 uv_write/uv_shutdown/uv_close callback
-> uv_close(async handle)
-> uv_run 直到 closing handles 清空
-> uv_loop_close
-> worker Closed
```

`uv_async_send()` 与 async handle close 必须有明确 lifetime gate，late callback不能唤醒已关闭 handle。

停止采用有界 graceful drain，不允许无界等待：

1. runtime 进入 `Draining` 后拒绝新注册，并取消仍在 `Queued` 的注册命令；
2. 在 grace deadline 内允许 active relay 按 D-17 正常 FIN predicate 收敛；
3. deadline 到达后，对剩余 relay 统一执行 D-18 异常 terminal/abort；
4. 强制 abort 仍不得跳过 local operation、callback、terminal handoff 和 `uv_close` callback
   的 ownership 收敛；
5. 记录 graceful drained、forced abort、deadline exceeded、remaining handles/operations 以及
   各阶段耗时；
6. 关闭 `uv_async_t` 前封闭 callback admission 和 late-wake gate，随后继续运行 loop，直到
   closing handles 清空，再执行 `uv_loop_close()` 和 join worker thread。

具体 grace 时长及其配置来源在实施计划中确定，但实现和测试必须使用可控 deadline，不得
依赖无界等待。

实施顺序约束：D-19 的完整 stop orchestration、强制 abort 和专项竞态/长稳测试放在开发计划
最后部分。前序阶段先完成 runtime 启动、relay 注册、TCP↔QUIC 正常转发、压缩、背压、FIN
以及单 relay terminal 的功能验证。即使完整 D-19 延后，早期测试 fixture 仍必须提供最小安全
清理能力，避免遗留 worker thread 或 libuv handle；不得把临时清理路径当作最终生产 stop
实现。

### 6.14 D-20 构建期实现边界（已确认）

采用方案 A：native 与 libuv 提供相同的上层 `TqRelay*` 符号，但通过 CMake source set
互斥链接，不抽取长期 `IRelayBackend`，也不在 `relay.cpp` 中增加运行时 backend 分发。

```text
common tunnel/session
    -> TqRelayStartManaged / TqRelayStop / TqRelaySetTraceContext
            |
            +-- native build: tunnel/relay.cpp + platform native worker
            |
            +-- libuv build:  tunnel/libuv_relay.cpp + libuv relay worker
```

必须满足：

- `tunnel/relay.cpp`、`linux_relay_worker.*`、`darwin_relay_worker.*`、
  `windows_relay_worker.*` 及其现有 native 测试保持不变；
- libuv build 不编译 `tunnel/relay.cpp` 和任一 native worker；
- native build 不编译任何 libuv relay runtime 源文件；
- CMake 配置阶段验证 native/libuv source set 恰好选择一个，同时选择或同时缺失均失败；
- 两套实现继续提供上层现有的 `TqRelayStartManaged()`、`TqRelayStop()`、
  `TqRelaySetTraceContext()` 和 receive sink 入口；
- 不抽取 `StartRuntime/StopRuntime/RegisterRelay/...` 虚接口，控制面和数据面均无 runtime
  backend virtual dispatch；
- common header 允许以 `TCPQUIC_RELAY_BACKEND_LIBUV` 条件定义 libuv-only handle layout；
  未定义该宏时，native handle layout 和行为保持不变；
- libuv handle 只发布一个不可变 `TqUvRelayCommittedState`，不无条件扩张 native handle，也
  不以裸 `TqUvRelayWorker*` 作为 callback lifetime 依据；
- common tunnel 代码只允许增加 libuv build 条件分支和 compiled-backend metadata，不修改
  native worker 内部实现。

建议新增的隔离文件：

```text
src/tunnel/libuv_relay.cpp
src/tunnel/libuv_relay_worker.h
src/tunnel/libuv_relay_worker.cpp
src/tunnel/libuv_relay_event_queue.h
```

评测完成后不保留这一构建抽象：胜出实现成为唯一 `relay` 实现，同时删除 source-set 选择、
落选源码和落选测试。

### 6.15 D-21 构建选择与回退（已确认）

使用 CMake cache 变量，不增加运行时配置：

```text
TCPQUIC_RELAY_BACKEND=native|libuv
```

典型构建：

```text
cmake -S . -B build/native -DTCPQUIC_RELAY_BACKEND=native
cmake -S . -B build/libuv  -DTCPQUIC_RELAY_BACKEND=libuv
```

规则：

- 默认值为 `native`；
- 两个构建目录均生成名为 `raypx2` 的主程序；
- build/version/admin/metrics 必须报告 `compiled_relay_backend=native|libuv`；
- 不存在运行时 `relay.backend`、configured/effective backend 差异或热切换；
- libuv runtime 启动失败时当前 libuv build 明确启动失败，不加载或回退 native；
- 回切通过部署 native build 并重启进程完成；
- 同一进程内永远不混合 native 与 libuv relay；
- 性能产物必须记录 commit、CMake cache、编译器、优化级别和 compiled backend。

### 6.16 D-22 metrics 与诊断（已确认）

至少包括：

- worker：loop alive、loop iteration/lag、queue depth、wake/coalesce、active relay；
- control：register wait、timeout、cancel、rollback；
- QUIC→TCP：receive view、pending bytes、write queue、pause/resume、completion/discard；
- TCP→QUIC：read bytes、outstanding sends、send bytes、pause/resume、completion fallback；
- lifecycle：Prepared/Active/Terminal/Failed、precommit、late callback、generation mismatch；
- errors：libuv status、translated errno/name、terminal trigger；
- capacity：buffer pool使用量和每 worker热点分布。

libuv snapshot 不与 native snapshot 建立长期公共类型，也不要求内部字段一一对应。两套构建
只统一评测所需的可比指标：吞吐、CPU、RSS、context switch、p50/p95/p99、active relay、
pending bytes、错误率和 terminal convergence time。native 特有 epoll/kqueue/IOCP 指标和
libuv 特有 loop/wake 指标分别保留。

### 6.17 D-23 功能、竞态与恢复测试（已确认）

测试层次：

1. command queue 和 libuv同步原语单测；
2. registration prepare/publish/commit/rollback 故障注入；
3. precommit receive 与 terminal barrier 竞态；
4. send/receive exactly-once ownership；
5. FIN/half-close/zero-length FIN；
6. queue full、allocation failure、worker stop、late callback；
7. client/server 端到端 SOCKS5、HTTP CONNECT、port forward；
8. Linux 首先构建和运行，macOS/Windows 随后在对应机器验证；
9. ASan/TSan（平台允许时）、长稳、反复启停；
10. native/libuv 相同故障矩阵对照。

测试 source set 同样隔离：native build 运行当前 native 测试且不链接 libuv relay；libuv
build 新增独立的 queue、I/O、registration、terminal 和 runtime 测试且不链接 native
worker。第一轮只对 Linux 作集成验证；不得把 Linux 结果表述为 macOS/Windows 已通过。

实现和验证顺序遵循 D-19：先完成注册、无压缩/压缩双向转发、背压、FIN 和单 relay
terminal，再完成性能评测，最后实现完整 runtime stop、强制 abort、反复启停和 stop 专项
竞态/长稳测试。

### 6.18 D-24 Linux 性能评测（已确认）

本轮性能验收仅在 Linux 执行，直接参照
[`docs/test/dgx-netem-delay-loss-matrix_cn.md`](test/dgx-netem-delay-loss-matrix_cn.md)。macOS
和 Windows 不属于本轮 D-24 范围，不得从 Linux 结果推断其他平台性能。

native 与 libuv 使用同一 commit 的两个独立构建目录。除
`TCPQUIC_RELAY_BACKEND=native|libuv` 导致的 relay source set 差异外，编译器、优化级别、
依赖版本、链接方式、tuning 和运行环境必须一致。两端必须部署同一种 compiled backend，
禁止 native/libuv 混搭形成不可解释结果。

#### 6.18.1 固定 DGX 矩阵

- 使用两台 DGX 的 200Gbps 直连数据网；SSH 继续走 `172.16.*` 管理网；
- netem 只允许配置在数据网卡，禁止作用于管理网口；
- 单 QUIC connection、单 HTTP CONNECT stream；
- `--compress off --tuning wan --connections 1`；
- download 和 upload 均执行；
- 14 个 `delay × loss` 组合：delay 为 `10/20/50/80/100/150/200ms`，loss 为 `5%/10%`；
- 每个方向、每个组合至少重复 3 轮；
- 每套 backend 矩阵开始前只启动一次两端 `raypx2`，正常 case 之间不得重启或替换二进制；
- 若需要修改、替换或重启 `raypx2`，当前 backend 的整套矩阵作废，必须从前置清理重新执行；
- download 在对端发送网卡配置 netem，upload 在本机发送网卡配置 netem；
- 默认不启用 trace 文件，保留 `--diag-stats --diag-stats-interval 1`。

native 和 libuv 必须各自完成一套独立、完整的矩阵。为识别环境漂移，记录 backend 执行
顺序，并在每套矩阵前后重复 `10ms + 5%` 的 download/upload anchor case。若资源允许，使用
`native -> libuv -> native anchor` 的交叉顺序校验基线稳定性。

#### 6.18.2 证据与结果目录

沿用参考文档中的 qdisc、iperf JSON、proxy 日志、admin allocator dump、route/interface 和
异常现场证据。结果根目录增加 backend 层级或等价 metadata：

```text
docs/dgx-netem-delay-loss-matrix-<timestamp>/
  native/
    build-metadata.txt
    proxy/
    cases/
    summary.csv
    summary.md
  libuv/
    build-metadata.txt
    proxy/
    cases/
    summary.csv
    summary.md
  comparison.csv
  comparison.md
```

`build-metadata.txt` 至少保存 commit、submodule commit、compiled relay backend、完整 CMake
cache/命令、编译器、优化级别、二进制 hash、tuning 和两端部署路径。

#### 6.18.3 对比口径

先按参考文档独立判定每套 backend 的 hard/soft anomaly。`baseline_delta_pct > 20%` 继续
作为单套矩阵的异常停止与现场保留规则，只用于保证测试数据有效，不是 backend 淘汰线。

对每个相同的 `direction/delay/loss` 组合，使用至少 3 轮的中位数比较：

```text
native_median_mbps
libuv_median_mbps
libuv_delta_pct =
  (libuv_median_mbps - native_median_mbps) / native_median_mbps * 100
```

`comparison.csv` 至少包含吞吐中位数/最小值/最大值、失败次数、retransmits、qdisc drop/backlog、
srtt、BBR bandwidth、bytes in flight、CPU、RSS、context switch、pending bytes、queue-full/send
错误和 compiled backend。最终报告必须分别汇总 download/upload、5%/10% loss 和全部 14 个
组合，不能只报告整体平均值掩盖局部退化。

#### 6.18.4 本轮不覆盖的容量目标

参考矩阵固定单 connection、单 stream，因此本轮 D-24 不声称验证多连接容量、worker 扩展
效率、spike、breaking point 或多小时 soak。这些项目只有在 Linux DGX 对比显示 libuv 值得
继续评估后，才另立后续性能测试；macOS/Windows 性能也暂不纳入本轮。

不预设 libuv 相对 native 的吞吐、CPU、内存或其他自动淘汰线，也不由测试脚本输出
pass/fail 取舍结论。测试产物只负责验证执行有效性、完整呈现原始数据、统计量、差值、异常和
环境证据；最终保留 native 还是 libuv，由项目负责人结合完整报告人工判断。

### 6.19 D-25 评测与退出标准（已确认）

评测阶段：

1. native build 运行现有测试并建立功能、稳定性和性能基线；
2. libuv build 依次完成 Linux 正常功能、异常矩阵和本轮 DGX netem 性能验证；容量和长稳
   性能只在本轮结果值得继续评估后另立测试；
3. 本轮性能取舍只使用 Linux DGX 结果；macOS/Windows 的验证不属于 D-24，后续另行安排；
4. 使用同一 commit、编译器/优化级别、依赖版本、tuning、CPU affinity、worker 数、负载、
   网络条件和数据集进行对照；
5. 两套产物的唯一预期差异是 relay source set，并记录完整构建证据；
6. 项目负责人依据 D-24 完整数据作出明确的保留/删除决策，不进入长期双 backend 维护。

libuv 达标：

```text
删除 native relay.cpp/native workers/native 专用测试
-> libuv 收敛为唯一 relay 实现
-> 删除 TCPQUIC_RELAY_BACKEND 构建选项
-> 删除临时评测兼容代码
```

libuv 不达标：

```text
删除 libuv relay runtime、测试和 CMake source-set 分支
-> native 恢复为唯一构建路径
-> 若项目无其他 libuv 用途，再删除 libuv submodule 和静态链接
```

评测期间的回切通过重新部署 native build 和进程重启完成；不在活跃 relay 上热切实现。

### 6.20 D-26 libuv allocator 显式适配 mimalloc（已确认）

libuv backend 不依赖链接顺序、符号抢占或 mimalloc 全局 override，而是在 libuv runtime
bootstrap 阶段调用 `uv_replace_allocator()`，显式注册四个进程级 allocator：

```text
uv_replace_allocator(
    LibuvMiMalloc,
    LibuvMiRealloc,
    LibuvMiCalloc,
    LibuvMiFree)
```

`LibuvMi*` 适配函数分别调用 `mi_malloc`、`mi_realloc`、`mi_calloc` 和 `mi_free`。保留专用适配
层而不直接散布 mimalloc 调用，便于 exactly-once 初始化、统计、故障注入和单元测试。该设计
不改变项目当前 `MI_OVERRIDE=OFF`、`MI_OSX_INTERPOSE=OFF` 和 `MI_OSX_ZONE=OFF` 的全局隔离
策略，也不改变 native backend 的内存分配行为。

初始化必须遵守以下顺序和失败语义：

1. allocator bootstrap 必须发生在进程内第一次 libuv API 调用之前；包括
   `uv_loop_init()`、`uv_thread_create()`、`uv_mutex_init()`、`uv_async_init()` 以及任何其他
   runtime/worker/handle 初始化；
2. 注册是进程全局、线程安全且 exactly-once 的操作，不按 runtime、loop 或 worker 重复执行；
3. 正常 libuv 构建强制要求 `TCPQUIC_USE_MIMALLOC=ON`，不满足时 CMake 配置直接失败，禁止
   静默回退 system allocator；
4. 项目识别出的 ASan 等 sanitizer 构建允许沿用现有规则关闭 mimalloc，此时明确不调用
   mimalloc allocator replacement，libuv 使用 system allocator；
5. 正常构建中 `uv_replace_allocator()` 失败时，libuv runtime 启动失败，不得创建 loop、worker
   或 handle，也不得自动回退到 native backend；
6. runtime stop 后不恢复 system allocator。`Start -> Stop -> Start` 复用已经注册的相同
   allocator，避免在仍可能存在旧分配对象时切换 allocator；
7. allocator 必须满足 libuv 的跨线程分配/释放要求；不得用 worker-local heap 或
   thread-affine wrapper 改变 mimalloc 的线程安全语义。

新增诊断至少报告：compiled allocator（`mimalloc|system`）、replacement 是否适用及是否成功、
失败的 libuv status。D-24 的 `build-metadata.txt` 和对比结果必须记录
`TCPQUIC_USE_MIMALLOC`、sanitizer 状态和 libuv allocator 状态，避免 allocator 配置差异污染
native/libuv 性能结论。

测试至少覆盖：首个 libuv API 前完成注册、多 worker 并发启动只注册一次、反复启停不重复
注册、注册失败时零 libuv 资源创建、四类分配确实经过适配函数，以及 sanitizer 构建使用
system allocator 并保持 sanitizer clean。allocator failure injection 只允许在 bootstrap 前
安装；测试不得在已有 libuv allocation 存活时替换 allocator。

### 6.21 D-27 libuv backend 平台接口边界（已确认）

libuv backend 的目标是以同一套源码跨 Linux、macOS 和 Windows 评测。其生产实现及专用支持
模块只能使用：

- libuv 提供的公共接口；
- C++ 标准库接口；
- 本设计确认时仓库已有 third_party 的公共接口，例如 MsQuic、mimalloc、zstd 和 spdlog；
- 项目已有的跨平台公共接口和数据类型，例如 `TqStreamLifetime`、compressor/decompressor、
  relay tuning、buffer pool、metrics 和 trace 接口。

“允许调用项目公共接口”不允许绕过本条约束：若该调用只是为了从 libuv backend 代调用某个
OS syscall 或平台专用 helper，同样视为违规。新增 third_party 依赖也不自动进入白名单，必须
单独设计和确认。

libuv backend 禁止直接调用或依赖操作系统强相关能力，包括但不限于：

- Linux `epoll`、macOS `kqueue`、Windows IOCP 以及 `poll/select` 等原生事件接口；
- `socket/accept/connect/close/shutdown/read/write/readv/writev`、`fcntl/ioctl/setsockopt` 等
  socket、fd 或 handle 系统接口；
- pthread、futex、Windows Event/Critical Section 或其他平台线程和同步接口；
- 在 backend 源文件中直接包含 `sys/socket.h`、`unistd.h`、`windows.h` 等平台系统头；
- 使用 `__linux__`、`__APPLE__`、`_WIN32` 等条件分支形成平台专用数据面、ownership、错误
  恢复或性能 fast path。

平台类型若由 libuv 或其他获准 third_party 公共头传递，可以作为不透明 API 参数使用；不得
据此提取 native handle 后调用 OS API。编译器属性、warning 控制等不改变运行语义的构建适配
不属于平台数据面实现，但应限制在构建层或独立 portability header，不能演变为 backend
行为分叉。

socket 生命周期边界固定为：现有 client ingress 或 server dial reactor 负责创建、连接以及
handoff 前必须完成的 socket option；libuv backend 接收已建立的 socket，只通过
`uv_tcp_open()` 纳入 handle ownership，通过 `uv_read_start()`、`uv_write()`、
`uv_shutdown()` 和 `uv_close()` 推进后续生命周期。进入 `UvHandleOwned` 后禁止裸
`close()`。如果某项需求无法由 libuv、C++ 标准库、现有 third_party 或既有跨平台公共接口
表达，必须停止实现并回到设计层确认，不得增加 Linux/macOS/Windows fallback。

准入检查包括：代码评审逐项检查 include/API/平台宏；为 libuv 专用 source set 增加静态扫描
门禁；验证 libuv target 不链接 native worker 对象；Linux 首轮通过后在 macOS/Windows 使用
相同 backend 源文件编译和验证。测试 harness、netem 脚本和外部故障注入工具可以使用所在
平台能力，但这些调用不得编译进 libuv backend，也不能成为生产正确性的必要条件。

## 7. 实现、测试、平台与证据追踪

本节记录当前实现落点和验证入口，不以“存在源码或测试”代替 fresh 执行结果。表中的
验证结果以 2026-07-16 Task 13 fresh 执行为准。macOS 与 Windows **未在本轮验证**，不得从
Linux 构建、单测、功能或性能结果推断为 PASS。负责人已确认本次只闭环 libuv backend；
native regression 和 native 零改动审计取消。后续派生只保留 libuv 代码的分支属于独立目标，
本次不删除 native 实现。

### 7.1 证据索引

| 代号 | 证据位置 | 当前用途与状态 |
|---|---|---|
| E-LUV | `build/libuv-final/` 及 Task 13 报告 | libuv Release configure/build 通过；53 个 `tcpquic_*test` 可执行测试全部通过；CTest 未注册测试，输出 `No tests were found` |
| E-NAT | — | **负责人取消** native regression，不属于本次完成门禁 |
| E-API | `scripts/check-libuv-backend-api.py` 及 `tests/scripts/test_check_libuv_backend_api.py` | 生产源码扫描 exit 0；Python 契约测试 32 项通过、1 项因 lane worktree 不存在跳过 |
| E-FUNC | `docs/test/libuv-relay-linux-functional-<timestamp>/` | Linux 功能/故障 formal evidence；**本轮尚无正式结果目录** |
| E-DGX | 本次会话中已展示并由负责人确认 OK 的 Linux DGX 数据 | D-24 性能结果已由负责人接受；无自动淘汰线；当前工作区未保存正式对照目录，因此不把历史单 backend 目录冒充本轮原始证据 |
| E-NATIVE-AUDIT | — | **负责人取消** native 代码零改动审计 |
| E-ASAN | `build-terminal-asan/` 及 Task 13 报告 | system allocator 的 ASan/LeakSanitizer 聚焦验证：worker queue、runtime stop、terminal convergence 全部通过 |
| E-SRC | 本表列出的实现文件、CMake fragment、单元测试和脚本契约 | 静态追踪依据；只证明落点存在，不证明运行通过 |

仓库已有的
`docs/dgx-netem-delay-loss-matrix-20260706-083620/` 和
`docs/dgx-netem-delay-loss-matrix-20260712-071932/` 是早期单 backend 历史结果索引。前者含
1 个失败 case，后者在 39/84 case 后因数据有效性门禁停止；二者都不含本设计要求的同 commit
`native/`、`libuv/` 和 `comparison.*` 对照结构，因此只能用于环境和流程参考，不能替代负责
人已经审阅的本轮会话数据，也不能单独作为 D-25 删除实现的依据。

### 7.2 D-01～D-27 追踪表

表中测试 target 是最直接的主证据入口；同一决策也可能被其他 lane 或 Linux formal runner
交叉覆盖。该表的“fresh 待补”文字是实施映射建立时的原始状态，当前执行结论统一由 7.3
覆盖：E-LUV、E-API、E-ASAN 已完成，D-24 数据已由负责人接受，E-NAT/E-NATIVE-AUDIT 已取消；
macOS/Windows 本轮未验证。

| 编号 | 实现文件 | 主要测试 target / 门禁 | 验证平台 | 证据目录/状态 |
|---|---|---|---|---|
| D-01 | `CMakeLists.txt`、`src/CMakeLists.txt`；`src/tunnel/libuv_relay*.{h,cpp}` | backend/API 契约测试；`tcpquic_production_linkage_guard_test` | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-API |
| D-02 | `libuv_relay_worker.{h,cpp}`、`libuv_relay_event_queue.h` | `tcpquic_libuv_relay_worker_queue_test`、`tcpquic_libuv_relay_metrics_test` | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-ASAN |
| D-03 | `libuv_relay_quic_to_tcp.cpp`、`libuv_relay_tcp_to_quic.cpp`、`protocol/compress.cpp` | 两个 libuv 数据方向 test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程 |
| D-04 | `libuv_relay.cpp`、`libuv_relay_worker.cpp`；DNS 仍由既有 server dial/c-ares 路径承担 | E-API；E-FUNC 的 HTTP CONNECT、SOCKS5、port-forward case | Linux：API 边界已验证；macOS/Windows：未在本轮验证 | E-API；E-FUNC 复跑规程 |
| D-05 | `libuv_relay.cpp`、`libuv_relay_worker.{h,cpp}` | registration、worker queue test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV |
| D-06 | `libuv_relay_event_queue.h`、`libuv_relay_worker.{h,cpp}` | worker queue test、E-API | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-API |
| D-07 | `libuv_relay_worker.{h,cpp}` 的注册状态机 | worker queue、registration test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV |
| D-08 | `libuv_relay_worker.{h,cpp}` 的 runtime worker 集合和 round-robin | worker queue、metrics test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV |
| D-09 | `libuv_relay.cpp`、`libuv_relay_worker.cpp`、`libuv_relay_internal.h` | registration test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV |
| D-10 | `libuv_relay_worker.cpp` 的 publish → open → read start → Active 顺序 | registration test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV |
| D-11 | internal、worker、QUIC→TCP 实现 | registration、QUIC→TCP test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV |
| D-12 | `libuv_relay_quic_to_tcp.cpp` | QUIC→TCP test；E-FUNC download/fault case | Linux：单测已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程 |
| D-13 | QUIC→TCP、compress 实现 | QUIC→TCP test；E-FUNC zstd download | Linux：单测已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程 |
| D-14 | `libuv_relay_tcp_to_quic.cpp` | TCP→QUIC test；E-FUNC upload/half-close | Linux：单测已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程 |
| D-15 | TCP→QUIC、compress 实现 | TCP→QUIC test；E-FUNC zstd upload | Linux：单测已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程 |
| D-16 | internal、两个方向实现、tuning | 两方向、metrics、queue pressure test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程 |
| D-17 | terminal、两个方向、公共 lifetime/convergence | terminal、两方向 test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-ASAN；E-FUNC 复跑规程 |
| D-18 | terminal、worker、公共 lifetime/convergence | terminal、registration、queue test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-ASAN；E-FUNC 复跑规程 |
| D-19 | worker、terminal | runtime stop、registration test | Linux：已验证且按计划最后完成；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-ASAN；E-FUNC 复跑规程 |
| D-20 | 根/`src/CMakeLists.txt`、libuv CMake fragments、`relay.h` | API/backend 契约、ready contract、linkage guard | Linux：已验证；macOS/Windows：未在本轮验证 | E-LUV、E-API |
| D-21 | CMake、main、metrics/memory stats | backend selection、metrics、libuv configure/build | Linux：已验证；macOS/Windows：未在本轮验证 | E-LUV、E-API |
| D-22 | worker、relay metrics、memory stats | metrics、system allocator metrics test | Linux：已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV；E-FUNC 复跑规程、E-DGX |
| D-23 | libuv tests/CMake fragments、functional runner | 全部 libuv targets、functional runner 契约 | Linux：Release/ASan 已验证；macOS/Windows：**未在本轮验证** | E-LUV、E-ASAN；E-FUNC 复跑规程 |
| D-24 | DGX runner、evidence 工具 | backend matrix test；会话测试数据 | 仅 Linux DGX：负责人已接受；macOS/Windows：不在本轮范围 | E-DGX；**由负责人判断，无自动淘汰线** |
| D-25 | 本文 D-24/D-25 规则；构建 source-set 暂时并存 | 负责人已确认 libuv 性能满足需求 | Linux：性能已接受；macOS/Windows：未在本轮验证 | 仅保留 libuv 的分支属于后续独立目标；本次不执行 |
| D-26 | allocator、CMake、memory stats | allocator、metrics、system allocator metrics test | Linux：Release 与 ASan 已验证；macOS/Windows：未在本轮验证 | E-SRC、E-LUV、E-ASAN |
| D-27 | API scanner、CMake API target | E-API；Python 契约测试 | Linux：已验证；macOS/Windows：未在本轮验证 | E-API |

### 7.3 Task 13 fresh 验证登记

2026-07-16 fresh 验证结果如下。CTest 当前没有注册项目，因此其 exit 0 不能表述为测试通过；
实际回归以逐个运行构建目录中的测试二进制为准。

| 门禁 | 命令/范围 | 当前状态 | 实际证据待填项 |
|---|---|---|---|
| libuv configure | `cmake -S . -B build/libuv-final ...` | 通过 | Release、backend=`libuv`、mimalloc=ON，exit 0 |
| libuv build | `cmake --build build/libuv-final -j2` | 通过 | `raypx2` 和全部 libuv 专项目标构建成功，exit 0 |
| libuv tests | CTest + 逐二进制运行 | 通过（有基础设施备注） | CTest：0 tests；逐个运行 53 个 `tcpquic_*test`：53/53 通过 |
| 平台 API | Task 13 列出的生产 `.cpp` + Python 契约测试 | 通过 | 生产扫描 exit 0；unittest 32 passed、1 skipped |
| libuv ASan | system allocator 聚焦构建 | 通过 | worker queue、runtime stop、terminal convergence 3/3 通过，无 sanitizer 报告 |
| native configure/build/tests | — | 已取消 | 负责人决定本次只闭环 libuv；不作为完成门禁 |
| native 零改动/source-set | — | 已取消 | native 代码零改动审计取消；libuv source-set/非法值/API 门禁仍通过 |
| Linux functional | 见 `docs/test/libuv-relay-linux-functional_cn.md` | 历史功能验证已完成；本轮未重跑 formal runner | 当前工作区无新的 formal 结果目录，不虚构目录证据 |
| DGX 对照 | 会话中已展示的 Linux 数据 | 负责人已确认 OK | 满足需求；无自动淘汰线；本轮不重跑 |
| macOS | 相同 libuv backend 源码构建/验证 | 未在本轮验证 | 后续独立机器证据 |
| Windows | 相同 libuv backend 源码构建/验证 | 未在本轮验证 | 后续独立机器证据 |

D-24 的 `baseline_delta_pct > 20%` 是数据有效性异常停止线，不是 backend 自动淘汰线。负责人
已确认当前 libuv 性能满足需求。删除 native、派生仅保留 libuv 代码的分支属于后续独立目标，
本任务不执行。

## 8. 系统级验证框架

### 8.1 端到端功能图

```text
client ingress
  -> QUIC OPEN
  -> server dial reactor / c-ares / TCP connect
  -> OPEN response
  -> libuv relay registration
  -> TCP↔QUIC 双向转发
  -> FIN/abort/terminal convergence
  -> tunnel reaper
  -> metrics/trace/admin snapshot
```

每个边界都需要成功、失败、超时、取消、并发和停止场景，并断言日志、指标、资源和用户可见结果。

### 8.2 性能基线框架

- 环境：固定 CPU、内存、NIC、MsQuic/libuv版本和 tuning。
- 对照：同一 commit 和配置，仅切换构建期 relay source set。
- 负载：单流、多流、多连接、双向、压缩/不压缩、不同 RTT/loss。
- 判定：只设置测试有效性和异常判定规则，不设置 backend 自动淘汰线；最终由负责人审阅
  完整报告后决定。
- 证据：脚本参数、版本、原始日志、metrics snapshot、结果汇总。

### 8.3 异常与恢复框架

至少覆盖：

- TCP reset/refused/timeout、QUIC abort、idle timeout；
- queue full、buffer allocation failure、libuv API failure；
- registration publish/commit barrier 注入 terminal；
- worker stop、runtime restart、进程 shutdown；
- CPU/内存压力、网络分区、持续丢包；
- late SEND_COMPLETE/RECEIVE/SHUTDOWN_COMPLETE；
- 检测信号、用户影响、自动缓解、恢复步骤和验收条件。

## 9. 明确非目标

第一阶段不做：

- 用 libuv替换 client ingress reactor；
- 用 libuv替换 server dial reactor/c-ares；
- 每条 relay 动态迁移 worker；
- 同一二进制运行时选择、混合或热切 native/libuv；
- 抽取长期 `IRelayBackend` 或统一 native/libuv snapshot 类型；
- 修改 Linux/Windows/macOS native worker 内部实现；
- 在 libuv backend 中直接调用 OS syscall、平台事件/同步接口或增加平台专用 fast path；
- 通过全局 allocator override 使 libuv 间接使用 mimalloc；
- 为压缩新增独立计算线程池；
- 立即删除 Linux/Windows/macOS native backend；
- 在评测结论前长期承诺保留任一实现；
- 同时重写 compressor/decompressor API；
- 未经实机测试宣称三个平台均已验证。

## 10. 主要风险

1. MsQuic callback 同步等待注册会增加协议线程停顿；本设计先保持 native 语义，必须通过 register wait metrics 验证。
2. `uv_async_send()` 允许合并，命令队列不能把 wake 次数当作命令数。
3. MsQuic multi-receive 允许多个 outstanding receive，必须严格限制 receive ownership预算。
4. `uv_write()` 不复制 payload，write callback 前必须保持 MsQuic receive buffer。
5. SEND_COMPLETE 位于 MsQuic线程，不能直接修改 loop-owned relay state。
6. `uv_close()` 是异步的；调用 close 不代表 handle 已释放。
7. worker shutdown 与 late callback 竞争可能触发对已关闭 `uv_async_t` 的访问。
8. 压缩/解压造成输入和输出 backlog 比例不同，单一 pending bytes 指标不够。
9. Windows libuv TCP/IOCP 行为与现有定制 IOCP backend 性能可能存在明显差距。
10. CMake source set 选择错误可能把两套同名 `TqRelay*` 实现同时链接，必须在配置阶段拒绝。
11. 两个构建目录的编译参数或依赖漂移会污染性能结论，评测证据必须保存完整 build metadata。
12. common header/调用点的 libuv 条件分支可能意外改变 native build，native 现有测试是强制回归门禁。
13. `uv_replace_allocator()` 调用过晚或重复切换 allocator 会造成跨 allocator 释放；bootstrap
    顺序和进程级 exactly-once 必须成为测试门禁。
14. 为补足 libuv API 差异而引入 OS fallback 会使三平台产生不同 ownership 和 terminal
    语义；缺失能力必须回到设计层解决。

## 11. 决策日志

| 日期 | 编号 | 结论 |
|---|---|---|
| 2026-07-15 | D-01 | 评测期源码并存、构建产物互斥，评测后删除落选实现 |
| 2026-07-15 | D-02 | 固定 worker，每 worker 一个线程和一个 loop |
| 2026-07-15 | D-03 | 压缩与解压在 libuv loop线程执行 |
| 2026-07-15 | D-04 | DNS 继续由现有 c-ares dial reactor 负责 |
| 2026-07-15 | D-05 | relay 注册采用外部调用者同步等待 |
| 2026-07-15 | D-06 | 沿用 Linux command/result 模式，使用 libuv 内置同步接口 |
| 2026-07-15 | D-07 | 有界等待只取消 Queued 命令；进入 Executing 后等待最终 ownership 结果 |
| 2026-07-15 | D-08 | 第一阶段 round-robin 选择 worker，relay 生命周期内不迁移 |
| 2026-07-15 | D-09 | PublishTarget 成功后 fd ownership 不可逆转移，区分 token 与 uv handle ownership |
| 2026-07-15 | D-10 | `uv_read_start()` 成功后才提交 Prepared → Active |
| 2026-07-15 | D-11 | 采用 Darwin 四态 precommit queue 和 once-only settlement |
| 2026-07-15 | D-12 | QUIC→TCP 无压缩路径只使用 `uv_write()`，保持 payload 到 write completion |
| 2026-07-15 | D-13 | 解压结果进入新缓存后完成 compressed receive，输出缓存保持到 TCP write 完成 |
| 2026-07-15 | D-14 | 一次 `uv_read_cb` 对应一次 `StreamSend`，不跨 read callback 主动合并 |
| 2026-07-15 | D-15 | 第一阶段继续使用现有 compressor/decompressor API |
| 2026-07-15 | D-16 | 复用现有 tuning 字段和阈值语义，不新增第一阶段配置 |
| 2026-07-15 | D-17 | FIN/半关闭完全复用当前 terminal convergence predicate |
| 2026-07-15 | D-18 | Darwin terminal correctness 为主，Linux typed command/local/fallback fast path 为辅 |
| 2026-07-15 | D-19 | 有界 graceful drain 后强制 abort；完整停止功能和专项测试排在开发计划最后 |
| 2026-07-15 | D-20 | 方案 A：相同 `TqRelay*` 符号由 CMake 互斥 source set 实现，不抽取 `IRelayBackend` |
| 2026-07-15 | D-21 | 构建期选择 native/libuv，单产物单 backend，失败不自动回退 |
| 2026-07-15 | D-22 | libuv 使用独立 snapshot，只统一评测指标 |
| 2026-07-15 | D-23 | native 测试保持不变，libuv 独立测试；先 Linux 后 macOS/Windows |
| 2026-07-15 | D-25 | 评测后只保留胜出实现并删除 source-set 选择和落选实现 |
| 2026-07-15 | D-24 范围 | Linux 参照 DGX netem 14 组合矩阵对比 native/libuv；其他平台暂不考虑 |
| 2026-07-15 | D-24 判定 | 不设置自动淘汰线，最终由项目负责人依据完整测试数据判断 |
| 2026-07-15 | D-26 | 正常 libuv 构建在首个 libuv API 前进程级注册 mimalloc；sanitizer 构建允许 system allocator |
| 2026-07-15 | D-27 | libuv backend 禁止直接调用 OS 强相关接口，只使用 libuv、标准库、现有 third_party 和项目公共接口 |

## 12. 设计确认结果

D-01～D-27 已全部确认。后续开发计划必须完整继承本文约束，尤其是构建期互斥 source set、
Linux-first 验证、D-09～D-18 ownership/terminal 规则，以及 D-19 在正常
功能和性能验证之后实施。D-24 只生成完整对比证据，不自动决定保留哪套实现。D-26 要求
libuv 对 mimalloc 显式、进程级且先于所有 libuv API 完成适配；D-27 禁止 libuv backend
直接调用 OS 强相关接口或形成平台专用实现分支。native 零改动仍是原始设计边界，但负责人已
取消其作为本次 Task 13 的审计门禁；后续仅保留 libuv 的分支属于独立目标。
