# Windows IOCP 代理链路评审（对照 Linux/macOS）

基于 `docs/relay-platform-integrate_cn.md` 的平台实现对照及最近代码检视，聚焦 Windows 平台在生产中出现的 TCP 异常中断问题。目标是给出可执行的稳定性修复方向，并保持 `docs/proxy-memory-manage_cn.md` 提到的应用层 zero-copy 设计不变。

## 一、问题结论（核心）

Windows 相比 Linux/macOS 的主要不稳定性来源，主要集中在**完成事件语义和关闭竞态处理**，而非零拷贝内存模型本身。

- Linux/macOS 使用“事件队列驱动 + 集中消费 + 生命周期退场清晰”的执行风格，抗抖动能力更强。
- Windows 使用 IOCP 完成事件驱动（`GetQueuedCompletionStatus`）+ 直接回调链，当前对完成失败与零字节返回处理偏激进，容易把**合法关闭/收敛态**误判成致命 TCP 异常。
- zero-copy 方向本身仍可保持：当前非压缩路径 `quic->tcp` 与 `tcp->quic` 仍可使用现有引用/内存池机制，不建议改造为统一拷贝路径。

## 二、Windows 与 Linux/macOS 的对照差异

### 1) 事件/调度模型

- Windows:
  - IOCP completion loop 主导流程。
  - 每个完成事件直接触发 `HandleTcpRecv/HandleTcpSend` 等处理。
- Linux/macOS:
  - 更偏向统一队列消费模型。
  - 回调只做事件入队，消费逻辑集中在单一调度路径。

**影响**：Windows 完成事件边界下，连接关闭、超时、重试与收尾态的竞态更容易放大，错误判断门槛过低会触发大量回收与重连。

### 2) 错误与关闭态分离不充分

Windows 在以下分支上常直接走硬失败：

- `GetQueuedCompletionStatus` 返回 `ok == false`。
- TCP send completion 读到 `bytes == 0`。
- 部分 completion 错误在 `closing`/`aborting` 期未做特判。

**影响**：把“关闭过程中合法到达的事件”与“真实传输故障”混淆，导致链路抖动下出现异常中断。

### 3) Backpressure / in-flight 约束

- Linux 实现对发送队列和未决额度有更明确的背压与门控语义。
- Windows 在运行时控制上体现较少，容易在流量抖动时形成“出队-重试-重入”抖动放大。

**影响**：在高并发与不稳定网络下，未决状态可累积，触发级联错误路径。

### 4) zero-copy 一致性

- 无压缩路径下，仍可保持 QUIC/TCP 两侧的应用层零拷贝语义：
  - `quic->tcp`：沿用 `QUIC_BUFFER` 切片引用与分片发送。
  - `tcp->quic`：沿用中继内存池申请缓冲并转移所有权发送。

**结论**：零拷贝不应被视为本次不稳定主因。后续修复不应引入主路径额外拷贝。

## 三、Windows 高优先级排查与修复顺序（建议）

1. 区分 `closing` 与 `fatal`：
   - 在 `windows_reactor.cpp` 的 completion 处理、`windows_relay_worker.cpp` 的错误收口处加入连接状态机检查。
   - `GQCS 失败`、`bytes==0` 在连接收敛阶段应降级为“静默清理”，不应触发硬失败。

2. 完善 completion 边界判定：
   - 增加 `epoch/generation` 或 token 版本防护，避免旧 completion 触发新生命周期。
   - `StreamCallback` 的接收入列与处理需与 `pending/paused` 状态一致性对齐。

3. 限流与退避补齐到 Windows runtime：
   - 引入与 Linux 对齐的 in-flight 限制与 pending 控制路径，限制瞬时未决发送/回压扩散。

4. 指标先行、逐步放大验证：
   - 采集并对比 `GQCS_fail`、`GQCS_bytes0`、`pending_tcp_send_count`、`queued_quic_recv`、`quic_recv_pause/resume`、`relay_close_epoch_drop`。
   - 重点覆盖：长连接稳定压测、短连接高频创建销毁、丢包/抖动、强制关闭/重连。

## 四、与零拷贝文档的一致性要求

- 任何修复优先限定为“状态机与错误语义修正”，不得更改正常路径的数据转发零拷贝 ownership。
- 如需保护机制，应优先放在异常回收/关闭路径（例如统计保护、可恢复路径降级），避免在转发主链路新增 memcpy。

## 五、执行建议

- 第 1 阶段（仅修复异常判定）：按最低侵入方式改掉误报致命错误，观察错误率与重连率。
- 第 2 阶段（流控约束）：补齐 in-flight 与 backpressure 控制。
- 第 3 阶段（稳定性验证）：按文档基线对 Linux/macOS 行为对齐后逐步放量验证 Windows。
