# QUIC 客户端固定间隔单飞重连设计

## 1. 背景与问题

QUIC 客户端连接失败时，当前实现按固定 3000ms 间隔调度重连。每个连接槽位使用共享布尔值 `RetryScheduled` 表示已有重试，但延迟任务本身没有不可复用的身份。旧连接关闭完成、当前连接启动以及新一轮调度会重复修改同一个布尔值，使延迟队列中的历史任务可能借用新一轮的 `RetryScheduled=true` 重新获得执行资格。

结果是同一槽位逐渐积累多个可执行重试任务。服务器持续不可达时，重连频率会从每 3 秒一次增长为每秒多次，造成短命 QUIC 连接风暴、高 CPU 占用和大量日志输出。多个 peer 共享 ingress reactor，因此一个故障 peer 的重连风暴还可能增加健康 peer 的调度延迟。

## 2. 目标与非目标

### 2.1 功能目标

- 保持每个连接槽位固定 3000ms 的重试静默期，不引入指数退避。
- 每个槽位任意时刻最多存在一个有效、可执行的重试任务。
- 被取代或取消的历史任务即使仍留在 ingress 延迟队列中，也必须永久失效。
- 已经通过初步检查但尚未接管槽位的任务，必须能被手动 reconnect、slot 删除或新连接发布安全抢占。
- 旧连接的异步关闭完成不能清理、打断或为当前连接预约重试。
- slot 删除后重新使用相同 index 时，旧 slot 的任务不能发生 ABA 误匹配。
- 不同 slot 和不同 peer 的有效重试相互独立。

### 2.2 非功能目标

- 单个故障 peer 持续不可达时，连接尝试频率、CPU 和日志增长率不能随运行时间持续上升。
- 一个 peer 持续失败时，健康 peer 的连接和转发流量保持可用。
- 重试调度、执行、过期丢弃和提交失败均可观测。
- 设计在 Linux、Windows 和 macOS 共用的 `QuicClientSession` 状态机中保持一致，不依赖平台专属定时器语义。

### 2.3 非目标

- 不修改 3000ms 重试间隔的配置方式。
- 不引入指数退避、随机抖动或最大重试次数。
- 不改变 QUIC 握手、路径选择、连接负载均衡和业务流协议。
- 不处理 SOCKS/HTTP 本地监听端口冲突。
- 本次不把 ingress 延迟队列整体改造成堆或时间轮；是否需要物理取消由容量验证结果决定。

## 3. 方案比较与选择

### 3.1 采用：不可复用重试票据 + 条件化启动

每次预约重试时，从 session 级单调序列分配唯一 token。延迟任务捕获由以下字段组成的重试票据：

```text
RetryTicket = {
  slot_index,
  numeric_connection_id,
  connection_generation,
  retry_token
}
```

`numeric_connection_id` 在创建新 slot 或手动 reconnect 时由现有全局原子序列重新分配，可区分同 index 的不同 slot/连接实例；`connection_generation` 区分管理面代次；`retry_token` 区分同一实例的不同自动重试轮次。

任务不再先清除状态再调用无条件 `StartSlot(index)`，而是调用带完整票据的条件化启动入口。票据校验、消费 active token 和接管槽位必须在同一个锁内状态转换中完成。

### 3.2 未采用：仅使用槽位内 RetryGeneration

槽位被删除并在相同 index 重建后，局部计数会从初始值重新开始，可能与旧任务捕获值碰撞。仅在删除前递增局部计数不能解决问题，因为计数随槽位一起销毁。

### 3.3 暂不采用：延迟任务取消句柄

物理取消可以删除历史任务，但需要扩大 ingress reactor 接口和生命周期管理范围。票据校验足以保证功能正确性；本次先通过压力测试验证历史任务在 3000ms 窗口内的物理积压是否满足容量门槛。若不满足，再单独设计可取消句柄或更合适的延迟队列结构。

## 4. 状态模型与不变量

### 4.1 状态字段

`ConnectionSlot` 使用以下重试状态：

- `ActiveRetryToken`：当前有效重试 token；`0` 表示没有有效重试。
- `NumericConnectionId`：现有全局唯一逻辑连接身份；新 slot 和手动 reconnect 时更新。
- `Generation`：现有连接管理代次。
- `Connection` 和 `Context`：当前发布到槽位的连接及回调上下文。

`ClientSharedState` 增加：

- `NextRetryToken`：session 生命周期内单调递增的 token 分配器，从 `1` 开始，`0` 永不分配。
- session 级重试诊断计数器，定义见第 10 节。

公开快照中的 `RetryScheduled` 不再是独立事实源，而由 `ActiveRetryToken != 0` 派生，避免布尔值和 token 状态不一致。

`NextRetryToken` 使用 `uint64_t`。若分配将发生回绕，不允许复用旧 token：记录调度失败并停止为该 session 创建新重试，要求通过重启 session 恢复。正常进程生命周期内达到该边界不可行，但行为必须有定义。

### 4.2 核心不变量

在持有 `ClientSharedState::Lock` 时始终满足：

1. 每个 slot 的 `ActiveRetryToken` 为 `0` 或唯一对应一个已预约任务。
2. 任意两个成功分配的 retry token 在同一 session 生命周期内不相同。
3. 任务只有完整票据与 slot 当前 `NumericConnectionId`、`Generation` 和 `ActiveRetryToken` 同时相等时才可接管槽位。
4. 手动 reconnect、slot 删除、session stop 和当前连接成功建立都会把 `ActiveRetryToken` 置为 `0`。
5. 旧连接 callback 不能只凭 `slot_index` 或 `Generation` 改变当前 slot。
6. 外部 scheduler 调用不在 `ClientSharedState::Lock` 内执行。

## 5. 线性化操作

### 5.1 预约重试

提供只在持锁状态下调用的内部操作 `ReserveRetryLocked`。调用方必须提供预期连接身份；该操作：

1. 验证 session 为 started 且非 stopping。
2. 验证 slot 存在，并匹配预期的 `NumericConnectionId` 和 `Generation`。
3. 若 `ActiveRetryToken != 0`，返回“已合并”，不创建第二个任务。
4. 验证 scheduler 和 session gate 可用。
5. 从 `NextRetryToken` 分配非零 token，并写入 `ActiveRetryToken`。
6. 返回包含 scheduler、gate、weak state 和完整 `RetryTicket` 的 `RetrySubmission`。

该步骤是“当前 slot 拥有一个有效重试”的线性化点。

### 5.2 提交重试

`SubmitRetry` 在锁外调用 scheduler，固定传入 `std::chrono::milliseconds(3000)`。

- scheduler 接受任务后，增加 `retry_scheduled_total`。
- scheduler 拒绝任务时，重新持锁并仅在完整票据仍匹配时把 `ActiveRetryToken` 清零；增加 `retry_schedule_failed_total`，写入 `LastError` 和 trace。
- 提交失败回滚不能覆盖并发产生的新 token。

### 5.3 条件化启动

延迟任务到期后先通过捕获的 gate 获取 live session，然后调用 `StartSlotForRetry(ticket)`。该入口在 `StartSlot()` 实际移动旧连接、清理 context 的同一个 `ConfigLock + ClientSharedState::Lock` 临界区中验证完整票据。

若票据不匹配：

- 不改变 slot 的任何连接或重试状态；
- 增加 `retry_stale_dropped_total`；
- 直接返回 stale。

若票据匹配：

- 把 `ActiveRetryToken` 清零；
- 增加 `retry_executed_total`；
- 在同一临界区接管 slot，并继续现有 `StartSlot()` 启动流程。

该临界区是“重试任务获得启动权”的线性化点。手动 reconnect、slot 删除或新连接发布只要先完成，旧任务就无法接管；若重试先完成接管，后续手动 reconnect 按现有管理语义成为新的、更晚操作。

## 6. 各路径状态转换

### 6.1 自动连接失败

本地配置、ConnectionOpen 或 ConnectionStart 失败时，失败路径捕获当次启动对应的 `NumericConnectionId` 和 `Generation`。清理未发布连接后，通过带预期身份的预约入口创建下一轮重试。若其间发生手动 reconnect 或 slot 替换，身份验证失败，不为新实例预约重试。

### 6.2 SHUTDOWN_COMPLETE

关闭完成回调在一个 `ClientSharedState::Lock` 临界区内完成以下操作：

1. 验证 slot 存在。
2. 同时验证 slot 的 `Context`、`Connection`、`NumericConnectionId` 和 `Generation` 均对应本 callback。
3. 身份匹配时清理当前连接，并在同一临界区调用 `ReserveRetryLocked` 预约唯一重试。
4. 身份不匹配时仅把 callback 对应连接作为 orphan 完成资源释放，不改变当前 slot，不预约重试。

锁外继续执行 connection close、owner 释放和 `SubmitRetry`。这样消除“先验证、解锁、再按 index 调度”的 TOCTOU 窗口。

重复 `SHUTDOWN_COMPLETE` 因 context/connection 已被清理而不能再次预约。

### 6.3 连接成功

`OnSlotConnected` 只有在 connection 指针和 context 身份仍匹配当前 slot 时才设置 connected，并把 `ActiveRetryToken` 置为 `0`。已经提交的历史任务以后只能按 stale 路径丢弃。

### 6.4 手动 reconnect

手动 reconnect 在持锁状态下：

1. 把 `ActiveRetryToken` 置为 `0`。
2. 更新 `NumericConnectionId` 并递增 `Generation`。
3. 按现有语义立即启动新连接。

已在延迟队列中或尚未接管槽位的旧票据均失效。条件化启动的最终票据校验保证在途旧任务也不能中断新连接。

### 6.5 slot 删除与重建

删除 slot 前清除 active token。旧任务即使保留 index，也携带旧 `NumericConnectionId` 和全局唯一 token。重新扩容时，新 slot 获取新的 `NumericConnectionId`；旧任务不能匹配。

### 6.6 session stop/restart

stop 关闭 session gate、清除 slots 和 scheduler。历史任务无法获取 live session。restart 创建新的 `ClientSharedState` 和 gate，旧任务的 weak state/gate 不能作用于新 session。

## 7. 固定 3000ms 语义

- 计时起点是 ingress scheduler 接受任务并计算 `Due = steady_clock::now() + 3000ms` 的时刻。
- 3000ms 是最小静默期，不是从上一次连接尝试开始计算的严格周期。
- 不追赶错过的周期；连接握手或关闭耗时较长时，下一次尝试仍从新任务提交时重新等待完整 3000ms。
- 单元测试必须验证提交参数严格等于 3000ms，并使用可控时钟/任务队列证明 due 之前不执行。
- 进程级测试使用日志时间戳时允许采集误差：立即失败场景的有效尝试间隔不得小于 2900ms；空闲测试环境中应不大于 3500ms。超过上限需记录 reactor 延迟并判定测试失败或环境无效，不能把提前执行解释为抖动。

## 8. scheduler 拒绝语义

当前 `TqClientIngressReactor::EnqueueDelayed()` 仅在任务为空或 reactor 非 Running 时返回 false。Quic session 提交的任务始终非空，因此运行态契约是：只要 peer runtime 仍依赖一个 Running ingress reactor，提交必须成功。

- stopping 期间拒绝属于正常关停，不重试、不告警。
- session 仍 started 且 ingress 按运行态契约应可用时拒绝，属于内部生命周期错误：清除精确匹配的 active token，增加失败计数，设置 `LastError` 并输出 error trace。
- 不允许在没有 scheduler 的情况下立即重试，以免重新引入忙循环。
- 运行态 scheduler 失败后的恢复方式是重启对应 peer/session；管理面必须能看到失败状态。自动恢复 scheduler 不在本次范围内。

## 9. 延迟队列容量

逻辑失效的任务仍会保留到 due。由于延迟队列使用 vector 且每轮计算 timeout 时线性扫描，实施必须验证物理积压不会形成新的 CPU 问题。

- 正常自动失败路径：每个 slot 最多一个 active task；历史任务在 3000ms 内被取出。
- 手动 reconnect 或配置操作可在一个窗口内制造多个 stale task，因此需要单独压力测试。
- 本次不改变延迟队列结构，但增加仅用于测试/诊断的 delayed task queue depth 和 reactor loop delay 观测。
- 在第 12 节压力模型下，如果 queue depth、CPU 或 reactor 延迟超过门槛，则本设计不得发布，必须后续采用取消句柄、管理操作合并/限流或堆式定时队列之一。

## 10. 可观测性

在 `ClientSharedState` 维护并通过现有 peer/connection 管理快照以向后兼容的新增字段暴露以下累计计数：

- `retry_scheduled_total`：scheduler 接受的任务数。
- `retry_executed_total`：票据成功接管 slot 的任务数。
- `retry_stale_dropped_total`：因票据或 session 状态不匹配丢弃的任务数。
- `retry_schedule_failed_total`：scheduler 拒绝任务数。

现有 `retry_scheduled` 字段由 `ActiveRetryToken != 0` 派生。不得向管理接口暴露 token 本身。trace 记录 schedule、execute、stale-drop 和 schedule-failed 事件，并包含 peer、slot、`NumericConnectionId` 和 `Generation`，但不在默认 info 日志中为每次 stale drop 增加一行，避免新的日志放大。

进程级测试额外采集：

- connection attempt 时间序列；
- ingress delayed queue depth 最大值；
- reactor loop delay p95/p99；
- 进程 CPU、RSS 和日志字节增长率；
- 健康 peer 的连接状态、请求成功率和请求延迟。

## 11. 系统级端到端功能图

| 路径 | 状态转换 | 可测试断言 | 可观测证据 |
|---|---|---|---|
| 不可达 peer → MsQuic 失败 → shutdown complete → ingress 延迟队列 → 条件化 StartSlot | 当前连接清理并预约唯一票据，3000ms 后消费 | 单 slot 每窗口最多一次有效尝试；无重连风暴 | retry counters、连接日志、CPU |
| 历史任务到期 → 条件化 StartSlot | 票据不匹配，无副作用丢弃 | 当前连接指针、代次和 active token 不变 | `retry_stale_dropped_total` |
| 管理 API reconnect → peer runtime → QuicClientSession | 清除 token，更新连接身份，立即手动启动 | 在途旧任务不能关闭或替换新连接 | API 202、generation、retry counters |
| 删除最高 slot → 重新扩容相同 index | 新 slot 获得新 NumericConnectionId | 旧任务不能启动重建 slot | connection snapshot、stale counter |
| peer A 不可达 + peer B 健康 → 共享 ingress reactor | A 独立重试，B 持续转发 | B 不发生非预期断连，业务探测成功 | peer metrics、请求结果、reactor delay |
| ingress stopping → scheduler 拒绝 | 精确回滚 token，不忙循环 | 关停完成且无新任务；运行态拒绝可诊断 | failed counter、LastError、trace |
| 远端恢复 | 下一有效 3000ms 任务建立连接 | RTO 不超过剩余静默期 + 500ms 握手预算 | connected 时间、attempt 序列 |

## 12. 测试策略与覆盖矩阵

### 12.1 单元测试

在 `src/unittest/quic_session_reconnect_test.cpp` 使用可注入 scheduler、受控任务队列和并发屏障覆盖：

1. 同一 slot 重复预约只提交一个 3000ms 任务。
2. 第一轮任务失效并创建第二轮后，第一轮只增加 stale counter，第二轮只启动一次。
3. 任务准备接管前暂停，执行手动 reconnect，再恢复任务；新连接不被替换。
4. shutdown callback 准备处理前暂停，执行手动 reconnect，再恢复 callback；不为新连接预约重试。
5. 当前 shutdown complete 的身份校验、清理和预约在同一锁内完成。
6. orphaned 旧连接 shutdown complete 不清理当前 token，不增加调度数。
7. 重复 shutdown complete 不产生第二个任务。
8. 删除含待执行任务的最高 slot，重建同 index 并预约新任务；旧任务失效。
9. stop/restart 后旧任务不能进入新 `ClientSharedState`。
10. 多 slot 各自最多一个任务，token 不相同且互不取消。
11. scheduler 拒绝只回滚匹配票据，不覆盖并发产生的新票据。
12. 3000ms due 之前不执行，到期后只执行一次。
13. token 回绕路径拒绝调度并产生可诊断错误。

为避免测试绕过生产逻辑，应提取生产内部状态转换辅助函数，并提供 `TQ_UNIT_TESTING` 包装调用同一个函数。测试包装可以构造连接身份，但不得直接调用旧式 `RestartSlotAfterShutdownCompleteForTest(index, generation)` 绕过 context、connection 和票据校验，也不得解引用伪造的 MsQuic 对象。

### 12.2 组件测试

- 使用真实 `TqClientIngressReactor` 和可控 steady-clock 测试队列 due、wake、stale drain 和 queue depth。
- 并发执行 scheduler 提交失败回滚与新票据预约，验证比较完整票据后才回滚。
- 在 AddressSanitizer/ThreadSanitizer 可用构建中重复运行并发用例，检查 UAF 和数据竞争。

### 12.3 双 peer 端到端测试

新增独立脚本 `scripts/run-quic-client-retry-single-flight-test.sh`，所有监听端口由脚本动态分配，避免影响现有 proxy client：

- peer A：一个 slot，目标为能快速返回不可达的隔离地址。
- peer B：一个健康测试 server，持续通过 SOCKS/HTTP 或端口转发执行请求。
- 预热 30 秒，稳定运行 10 分钟，恢复 peer A 后继续运行 60 秒。

门槛：

- peer A 任意相邻有效尝试间隔不小于 2900ms，空闲环境不大于 3500ms。
- 10 分钟内尝试数不超过 `ceil(600 / 3) + 1 = 201`。
- peer B 业务探测成功率 100%，无非预期 QUIC 断连。
- peer A 恢复后，在剩余静默期加 500ms 握手预算内 connected，最大 3500ms。
- 稳态最后 2 分钟 CPU 均值不超过预热后首个 2 分钟均值的 1.5 倍，且不超过单核 5%；若硬件噪声使绝对门槛无效，测试必须标记环境无效而不是放宽相对门槛。
- RSS、延迟队列深度和每分钟日志增长率不呈连续三个采样窗口单调上升。

## 13. k6 管理面压力基线

k6 不能直接生成 MsQuic callback 时序，因此只用于验证管理 reconnect API 对 stale task 容量和共享 reactor 的影响；核心功能仍由第 12.3 节进程级故障注入验证。

### 13.1 环境和数据

- 单独测试进程，不复用开发机上现存 proxy client。
- 一个不可达测试 peer、一个 slot、独立管理端口和 Bearer token。
- Release 构建，trace info，关闭无关诊断流量。
- 每次请求：`POST /api/v1/peers/<peer>/connections/conn-0:reconnect`。

### 13.2 负载模型

| 阶段 | 负载 | 时长 | 目的 |
|---|---:|---:|---|
| baseline | 1 req/s | 60s | 建立管理面和队列基线 |
| expected peak | 5 req/s | 120s | 验证受支持的短时并发控制 |
| spike | 20 req/s | 30s | 制造 stale task 积压 |
| soak | 1 req/s | 10min | 验证队列和 CPU 能恢复稳定 |
| ramp-down | 0 req/s | 10s，随后观察 10s | 验证历史任务排空 |

另行执行 50、100 req/s 的 breaking-point 探索，只记录容量拐点，不作为本次发布门槛。

### 13.3 指标与门槛

- HTTP 202 检查成功率 100%，请求错误率 0，dropped iterations 为 0。
- expected peak 阶段管理 API p95 小于 200ms、p99 小于 500ms。
- 任意时刻管理快照中每 slot 的 `retry_scheduled` 只能为 false 或一个逻辑真值，不得出现第二个有效 token。
- ramp-down 后 5 秒内 delayed queue depth 回到 baseline，10 秒观察期内 CPU 回到 baseline 的 1.5 倍以内。
- stale counter 可以增长，但 executed counter 不得呈现小于 2900ms 的自动重试突发。
- 若 expected peak 已超过队列或 CPU 门槛，则本次不能发布，必须引入 reconnect 合并/限流或物理取消。

## 14. 异常条件与恢复

本特性不持久化业务数据，RPO 为 0；不存在备份恢复。恢复目标集中在连接可用性和不干扰健康 peer。

| 场景 | 故障注入 | 用户影响 | 检测信号 | 缓解与恢复 | 验收条件 |
|---|---|---|---|---|---|
| 远端不可达 | 丢弃/拒绝 peer A UDP | A 不可用，B 正常 | attempt、retry counters | 固定 3000ms 单飞重试 | 无突发，B 成功率 100% |
| 远端恢复 | 恢复 peer A 网络 | 最多等待剩余静默期 | connected 事件 | 下一有效任务自动连接 | RTO ≤ 3500ms |
| 网络分区反复抖动 | 每 10s 切换可达性 | A 间歇不可用 | state/trace | 每次终止只保留一个任务 | 无 token 泄漏和频率累积 |
| orphan callback 延迟 | 人工延后旧连接 shutdown complete | 无可见影响 | stale/identity mismatch | 只释放 orphan | 当前连接不变 |
| scheduler stopping | 在待调度时停止 ingress | peer 随进程关停 | failed counter 或正常 stopping | 精确回滚，不立即重试 | stop 无死锁、无忙循环 |
| 运行态 scheduler 异常拒绝 | 测试注入 false | 对应 peer down | LastError、failed counter、alert | 重启 peer/session | 失败可诊断，重启后恢复 |
| CPU 饱和 | 对测试机施加 CPU 压力 | 重试可延后，不可提前 | reactor p99 delay | 压力解除后自动处理 | 无小于 3000ms 的执行；积压 5s 内排空 |
| 管理 reconnect 洪峰 | k6 spike | 测试 peer 连接抖动 | API、queue depth、stale counter | ramp-down 后自然排空 | 满足第 13.3 节门槛 |
| 进程重启/滚动升级 | 在待重试时重启 client | 短暂不可用 | process/peer state | 新 state 从空 token 启动 | 无旧任务跨进程；启动后正常连接 |

## 15. 发布门槛

进入实施前：

- 本文 P0/P1 状态转换、票据字段和计时语义已落实到实施计划。
- 每个新增生产辅助函数都有对应的先失败后通过测试。

退出条件：

- `cmake --build build --target tcpquic_quic_session_reconnect_test` 成功。
- `ctest --test-dir build -R tcpquic_quic_session_reconnect_test --output-on-failure` 通过。
- 相关 Release 构建和现有测试通过。
- 第 12.3 节双 peer 端到端测试通过并保留日志、metrics、CPU、RSS 和 attempt 时间序列。
- 第 13 节 expected peak 与 spike/ramp-down 门槛通过。
- ASan/TSan 可用环境中的并发回归测试无 UAF、数据竞争或死锁。
- 所有新增管理 JSON 字段保持向后兼容，现有字段含义不变。

## 16. 已确认评审问题处理状态

| 原评审问题 | 确认结果 | 设计处理 |
|---|---|---|
| 延迟任务校验后抢占手动重连 | 确认 | 完整票据在 `StartSlot` 接管临界区最终校验 |
| shutdown 身份校验与预约之间 TOCTOU | 确认 | 身份校验、清理和 `ReserveRetryLocked` 合并为一个锁内转换 |
| slot 删除重建 ABA | 确认 | 使用全局唯一 `NumericConnectionId` 和 session 级唯一 retry token |
| 关闭回调测试接缝不足 | 确认 | 测试包装调用同一生产状态转换辅助函数 |
| 3000ms 计时语义不明确 | 确认 | 定义 scheduler 接受时刻为起点和可执行误差门槛 |
| 缺少双 peer 端到端测试 | 确认 | 增加共享 reactor 的 10 分钟故障隔离场景 |
| 延迟队列物理积压风险 | 确认风险，影响需测量 | 增加 queue depth、CPU、reactor delay 和 k6 管理面压力门槛 |
| 可观测性和量化门槛不足 | 确认 | 增加四类 retry counter、attempt 序列和发布门槛 |
| scheduler 拒绝后恢复语义未定义 | 确认 | 明确运行态契约、失败诊断和 peer/session 重启恢复 |
