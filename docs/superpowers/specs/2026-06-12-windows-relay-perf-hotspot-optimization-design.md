# Windows Relay 热点性能优化设计

> 参考：`docs/finished/superpowers/specs/2026-06-11-relay-perf-hotspot-optimization-design.md` 与 `docs/finished/superpowers/plans/2026-06-11-relay-perf-hotspot-optimization.md`
> 日期：2026-06-12  
> 状态：待评审

## 1. 背景与问题

Linux relay 已围绕 DGX perf 暴露的热点完成了四类优化：

1. QUIC→TCP plain path 消除 relay worker 侧二次拷贝。
2. stream→relay 使用 binding 快速定位，避免热路径扫描/锁。
3. per-relay buffer slot 预分配、ingress/worker 双域、去 `shared_ptr` 控制块，并用有界 MPMC ring 替代 mutex deque。
4. 在 always-pending 分支中，非压缩 QUIC receive 采用 deferred receive view：callback 保存 `QUIC_BUFFER` slice 元数据并返回 `QUIC_STATUS_PENDING`，relay worker 通过 `sendmsg`/`writev` 借用 MsQuic buffer 写 TCP，写入后调用 `StreamReceiveComplete(bytes)`。该路径已经让 relay receive payload copy 基本消失，多流瓶颈转向 MsQuic UDP receive path。

Windows relay 走独立 IOCP 路径，当前不受 Linux `TqLinuxRelayBufferPool` 锁竞争直接影响，但源码显示它尚未迁移 always-pending deferred receive zero-copy 目标，仍存在对应的 Windows 热点：

```text
TCP → QUIC
  PostTcpRecv
    -> 每次 WSARecv 分配 IoOperation
    -> 每次 Buffer.resize(RelayIoSize)
  HandleTcpRecv
    -> op->Buffer.resize(bytes)
    -> Stream->Send(op->Buffer.data(), ..., ClientContext=op)
  SEND_COMPLETE
    -> delete IoOperation
    -> 再 PostTcpRecv

QUIC → TCP
  StreamCallback(RECEIVE)
    -> 每次 RECEIVE 分配 IoOperation
    -> vector::insert 合并所有 QUIC_BUFFER，至少一次拷贝且可能多次扩容
    -> PostQueuedCompletionStatus(IOCP)
  IOCP worker
    -> HandleQuicReceiveQueued
    -> WSASend(op->Buffer)
  TcpSend complete
    -> delete IoOperation
```

当前实现的主要成本不是显式 mutex，而是 **QUIC→TCP payload copy、热路径堆分配、`std::shared_ptr`/`weak_ptr` 原子、`std::vector` 扩容/收缩、QUIC receive completion cadence 不可控、Windows 测试覆盖不足**。其中 QUIC→TCP 必须参考 Linux always-pending 的最终选择，在本阶段升级为 deferred receive complete zero-copy 主路径；operation/buffer 复用与背压作为该目标的配套工作。

## 2. 目标与非目标

### 2.1 目标

| 指标 | 当前状态 | 目标 |
|------|----------|------|
| QUIC→TCP receive payload copy | 每次 RECEIVE copy 到 `IoOperation::Buffer` | 非压缩路径 always-pending deferred receive view，worker 借用 MsQuic buffer `WSASend` 后 `StreamReceiveComplete` |
| QUIC receive completion cadence | callback 立即返回 success，worker 不负责 complete | worker 按实际 TCP 写入字节 complete，可实验 128KB/256KB/512KB/1MB 聚合阈值 |
| TCP→QUIC hot path 分配 | 每次 recv 新建 `IoOperation` + vector buffer | 复用预分配 recv operation 与 buffer |
| Callback relay 引用 | `weak_ptr::lock()` 每次 callback 原子引用计数 | 使用 callback binding + in-flight refs，避免 hot path `shared_ptr` lock |
| QUIC receive 背压 | 只有 `QueuedQuicReceives` 计数，无上限 | pending receive bytes/queue budget + receive pause/resume；禁止无预算 callback write |
| 测试覆盖 | 仅 start/stop smoke test | 覆盖 deferred receive view、partial TCP send、receive complete 聚合、stop/drain、backpressure、metrics |

### 2.2 非目标

- 不把 Windows relay 改造成 Linux epoll/readv/writev 模型；继续使用 IOCP + `WSARecv`/`WSASend`。
- 不改 MsQuic 内部；本阶段只依赖 MsQuic documented/observed `QUIC_STATUS_PENDING` receive ownership 语义，并用 Windows 单测验证 callback 返回后 pending buffer 在 `StreamReceiveComplete` 前可用于 TCP write。
- 不保留 copy-into-owned-buffer 作为非压缩 QUIC→TCP 主路径；它只允许用于压缩路径。非压缩 deferred receive view 入队失败时必须关闭/abort relay 并保持 receive ownership 语义一致，不能静默退回 copy 路径。
- 不优化压缩算法本身；压缩路径保持正确性优先，仍允许 copy 到 owned buffer 后解压。
- `TqRelayBufferSlot` / `TqBufferHandle` / pending-bytes budget 这类内存池能力应抽成 OS 无关基础组件，供 Linux 与 Windows 复用；但不直接把当前 `TqLinuxRelayBufferPool` 按原名和 Linux worker/ingress 线程域假设搬到 Windows，避免把 Linux relay 的调度语义固化进通用层。

## 3. 当前 Windows 实现审查

### 3.1 热路径堆分配与 vector buffer 生命周期

`IoOperation` 内嵌 `std::vector<uint8_t> Buffer`，在 `PostTcpRecv`、`StreamCallback(RECEIVE)` 分别创建并填充。TCP→QUIC 方向每次 `WSARecv` 都创建新 operation 并把 buffer resize 到 `RelayIoSize`；发送完成后 operation 被删除，再开始下一次读取。

这有两个直接影响：

- 每个 TCP recv/send round trip 至少一次 `new`/`delete`，每个 QUIC RECEIVE 也至少一次 `new`/`delete`。
- `vector::resize(RelayIoSize)` 会触碰/提交大块内存；随后 `resize(bytes)` 改小，但容量保留直到 operation 删除，无法复用。

### 3.2 QUIC→TCP receive 合并方式粗糙

`StreamCallback(RECEIVE)` 使用：

```cpp
for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
    const QUIC_BUFFER& buffer = event->RECEIVE.Buffers[i];
    op->Buffer.insert(op->Buffer.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
}
```

这已经把同一次 RECEIVE 的多个 `QUIC_BUFFER` 合成一个 IOCP 事件，因此 Windows 不存在 Linux 早期“每个 buffer 一个事件”的同形问题。但它未预先 `reserve(totalLength)`，可能多次扩容复制；也没有预算检查，恶意或突发流量会把 callback 线程上的内存增长推给 `std::vector`。

### 3.3 Callback 引用路径依赖 `weak_ptr::lock()`

`CallbackContext` 存 `std::weak_ptr<RelayContext>`，每个 QUIC callback 都 `lock()` 得到 `shared_ptr`。这比 Linux 优化前的线性扫描好，但在 hot path 上仍有原子引用计数开销，并且 relay close 时 callback 生命周期依赖 `shared_ptr` 语义，缺少明确的 `Closing` + `CallbackRefs` 协议。

### 3.4 IOCP 队列无显式背压

`QueuedQuicReceives` 只用于 drain/close 判断，不参与拒绝或暂停 receive。高吞吐下 callback 可以持续分配并 `PostQueuedCompletionStatus`，而 worker 只能串行 `WSASend`。如果 TCP peer 慢或 `WSASend` partial/排队增长，QUIC→TCP 队列会无界放大。

### 3.5 TCP→QUIC 读取深度保守

当前 `SEND_COMPLETE` 后才重新 `PostTcpRecv`，因此每个 relay 同一时间只有一个 TCP recv outstanding。这降低了 buffer 生命周期复杂度，但在高 BDP/高 CPU 场景可能限制 TCP→QUIC 吞吐。是否增加多 outstanding recv 必须在 operation pool 与 send in-flight budget 建好后进行。

## 4. 方案对比

### 方案 A：Windows always-pending deferred receive view（推荐）

分三阶段：

| 阶段 | 内容 | 风险 | 收益 |
|------|------|------|------|
| Phase 1 | 增加 snapshot/测试；实现非压缩 QUIC RECEIVE deferred receive view；callback 返回 `QUIC_STATUS_PENDING`；worker `WSASend` 完成后 `StreamReceiveComplete` | 中 | 消除 QUIC→TCP payload copy，与 Linux always-pending 主路径对齐 |
| Phase 2 | callback binding 替代 `weak_ptr::lock()`；pending receive bytes/queue budget；receive pause/resume；complete 聚合阈值实验 | 中 | 降低原子引用计数、控制 pending 内存与 completion cadence |
| Phase 3 | TCP recv operation 复用；可选多 outstanding TCP recv；Windows ETW/heap profile 验证 | 中 | 收敛 TCP→QUIC 分配与 IOCP 饱和度问题 |

优点：与 Linux 最终选择的 always-pending 分支保持架构一致，把 relay receive copy 从主路径移除；metrics 和 DGX/Windows perf 结论可对照。  
缺点：必须严格管理 pending receive lifetime、partial `WSASend`、abort/close、FIN 与 `StreamReceiveComplete` 字节对应关系。

### 方案 B：copy-into-owned-buffer 渐进优化

保留当前 callback copy 语义，只优化 `vector::reserve`、operation pool、queued byte budget。

优点：实现风险最低，不依赖 pending receive lifecycle。  
缺点：与 Linux always-pending 最终方向不一致，QUIC→TCP payload copy 仍在主路径；在多流场景下无法验证“relay receive copy 已基本消失”这一目标。

### 方案 C：同步 callback 直写 TCP

在 MsQuic callback 内直接 `WSASend`/写 socket，避免 IOCP 投递和 copy。

优点：路径短。  
缺点：违反现有线程模型；callback 内 socket I/O、backpressure、partial write、close/abort 复杂，风险高。

**推荐：方案 A**。方案 B 仅作为压缩路径和性能对照方案；方案 C 不采用。

## 5. 详细设计

### 5.1 Phase 1：always-pending deferred receive view

#### 5.1.1 添加 Windows relay snapshot

新增 `TqWindowsRelayWorkerSnapshot`，暴露与 Linux always-pending 对齐的指标：

- `DeferredReceiveQueued`
- `DeferredReceiveBytesQueued`
- `DeferredReceiveCompleteBytes`
- `DeferredReceiveCompletes`
- `DeferredReceiveCompletionFlushes`
- `PendingQuicReceiveBytes`
- `MaxPendingQuicReceiveBytes`
- `PendingQuicReceiveQueueDepth`
- `MaxPendingQuicReceiveQueueDepth`
- `QuicReceivePausedCount`
- `QuicReceiveResumedCount`
- `TcpSendOperationsPosted`
- `TcpSendBytes`
- `TcpSendPartialCompletions`
- `TcpSendWouldBlockOrPendingCount`
- `Errors`

这些指标是 Windows 实现 always-pending 的前置条件。没有 pending bytes、complete bytes、TCP partial 统计时，无法判断单流瓶颈是 completion cadence、IOCP 调度还是 TCP backpressure。

#### 5.1.2 Deferred receive view 数据结构

Windows 需要保存 MsQuic callback 中的 `QUIC_BUFFER` slice 元数据，而不是复制 payload：

```cpp
struct TqWindowsQuicReceiveSlice {
    const uint8_t* Data{nullptr};
    uint32_t Length{0};
};

struct TqWindowsPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    uint64_t RelayId{0};
    std::vector<TqWindowsQuicReceiveSlice> Slices;
    size_t SliceIndex{0};
    size_t SliceOffset{0};
    uint64_t TotalLength{0};
    uint64_t CompletedLength{0};
    uint64_t PendingCompleteBytes{0};
    bool Fin{false};
};

struct IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsRelayEvent Event{TqWindowsRelayEvent::TcpSend};
    std::shared_ptr<RelayContext> Relay;
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    WSABUF Buffer{};
};
```

`ReceiveView` 负责记录当前 slice/offset 和已写入但尚未 complete 的字节。`IoOperation` 只描述一次 outstanding `WSASend`，不拥有 payload。

#### 5.1.3 StreamCallback(RECEIVE) always pending

非压缩路径：

```text
StreamCallback(RECEIVE)
  -> validate buffers
  -> create TqWindowsPendingQuicReceive with slices
  -> account PendingQuicReceiveBytes / queue depth
  -> PostQueuedCompletionStatus(QuicReceiveViewQueued)
  -> return QUIC_STATUS_PENDING
```

规则：

- `CompressAlgo == None && Decompressor == nullptr` 时必须走 deferred receive view。
- 压缩路径仍 copy 到 owned buffer 后入队，因为 decompressor 需要连续 owned output/lifetime。
- 入队失败、预算超限、slice 分配失败时，必须返回可控结果：关闭 relay 或先调用 `StreamReceiveComplete(0)` 后关闭，禁止既返回 `PENDING` 又丢失 view。
- FIN 与 view 绑定；只有该 view 所有字节写入并 complete 后，才对 TCP 侧执行 send shutdown/drain。

#### 5.1.4 IOCP worker 写 borrowed QUIC buffer

IOCP worker 收到 `QuicReceiveViewQueued` 后，按当前 slice 构造 `WSABUF`：

```cpp
WSABUF buf{};
buf.buf = reinterpret_cast<char*>(const_cast<uint8_t*>(slice.Data + view->SliceOffset));
buf.len = slice.Length - static_cast<ULONG>(view->SliceOffset);
WSASend(relay->TcpFd, &buf, 1, &sent, 0, &op->Overlapped, nullptr);
```

completion 后：

1. 按 `bytes` 推进 `SliceOffset` / `SliceIndex`。
2. 增加 `CompletedLength` 与 `PendingCompleteBytes`。
3. 根据 `WindowsRelayQuicReceiveCompleteBatchBytes` 决定是否调用 `StreamReceiveComplete(PendingCompleteBytes)`。
4. 如果 view 完成，强制 flush 剩余 `PendingCompleteBytes`，释放 pending bytes，处理 FIN/shutdown。
5. 如果 TCP partial，继续用同一个 view post 下一次 `WSASend`。

#### 5.1.5 StreamReceiveComplete 聚合

参考 Linux always-pending 文档，Windows 也需要实验聚合阈值：

| complete threshold | 目标 |
|--------------------|------|
| 0 | 默认：写多少 complete 多少，最保守 |
| 128KB | 验证减少 complete 次数 |
| 256KB | 平衡 latency 与吞吐 |
| 512KB | 偏吞吐 |
| 1MB | 压测上限，观察 flow-control 副作用 |

任何错误、unregister、view 完成、relay close 都必须强制 flush 已完成但未 complete 的字节，避免 MsQuic flow control 卡死。

### 5.2 Phase 2：binding、pending budget 与 receive pause/resume

#### 5.2.1 Callback binding 替代 weak_ptr lock

新增 `CallbackBinding`，与 Linux `StreamRelayBinding` 思路一致：

```cpp
struct CallbackBinding {
    TqWindowsRelayWorker* Worker{nullptr};
    std::atomic<RelayContext*> Relay{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};
```

规则：

- `RegisterRelay` 创建 binding，`stream->Context = binding`。
- `StreamCallback` 先检查 `Closing`，递增 `CallbackRefs`，读取 `Relay`，退出时递减。
- `CloseRelay` 设置 `Closing=true`、`Relay=nullptr`，恢复 stream no-op callback；释放 relay 前确保不会再读 binding。
- pending receive view 的 lifecycle 由 relay pending queue 和 outstanding IOCP operation 管理，callback 本身不拥有 relay。

#### 5.2.2 Pending receive budget

新增 Windows 对等配置：

- `WindowsRelayMaxPendingQuicReceiveBytesPerRelay`，默认建议与 Linux per-tunnel pending budget 同量级。
- `WindowsRelayQuicReceiveCompleteBatchBytes`，默认 `0`，即每次 TCP send completion 立即 complete。

预算规则：

```text
if PendingQuicReceiveBytes + view.TotalLength > MaxPendingQuicReceiveBytesPerRelay:
  SetQuicReceiveEnabled(false) 或关闭 relay
else:
  enqueue pending view
```

优先实现 pause/resume，而不是无预算关闭：

- 高水位：pending bytes 超阈值时 `StreamReceiveSetEnabled(false)`。
- 低水位：pending bytes 降到阈值的一半时 `StreamReceiveSetEnabled(true)`。
- 如果 Windows MsQuic wrapper 无法直接调用 receive enable API，则第一版记录 `QuicReceivePausedCount` 并关闭 relay，文档中标记为兼容 fallback。

#### 5.2.3 禁止无预算 callback write

不采用 callback 内直接 `WSASend`。所有 QUIC receive 都必须先进入 pending view 预算体系，由 IOCP worker 统一处理 partial write、completion 聚合、close/drain。

### 5.3 Phase 3：TCP→QUIC operation 复用与 IOCP 并发深度

#### 5.3.1 TCP recv operation 复用

TCP→QUIC 方向仍有 per recv `IoOperation` + vector buffer 分配，可在 deferred receive 主路径稳定后优化：

```cpp
struct RelayContext {
    std::vector<std::unique_ptr<IoOperation>> TcpRecvFree;
};
```

`PostTcpRecv` 从 pool 取 operation，buffer 只在容量不足时扩容到 `RelayIoSize`，`SEND_COMPLETE` 后回 pool。

#### 5.3.2 多 outstanding TCP recv（可选）

默认仍保持每 relay 一个 outstanding `WSARecv`。只有 ETW 显示 TCP→QUIC 不饱和时，再引入 `WindowsRelayTcpRecvOutstanding`，并同时证明 send ordering 不被破坏。

### 5.4 后续专项：Windows UDP / socket buffer 环境

Linux always-pending 文档指出，多流瓶颈可能转向 UDP kernel copy 和系统 socket buffer。Windows 阶段完成 relay zero-copy 后，需要用 ETW 分离：

- relay copy 是否消失；
- CPU 是否转向 MsQuic UDP receive / AFD / kernel copy；
- Winsock TCP send buffer 是否限制单流 completion cadence；
- RSS/IRQ/网卡 offload 是否成为瓶颈。

这部分不和 relay pending receive 修改混在一个 commit。

## 6. 优化后数据流

### 6.1 TCP → QUIC（Phase 3 后）

```text
IOCP worker
  -> GetQueuedCompletionStatus(TcpRecv)
  -> Reused IoOperation contains stable recv buffer
  -> optional compress
  -> Stream->Send(buffer, ClientContext=operation)

MsQuic SEND_COMPLETE callback
  -> binding O(1) worker/relay access
  -> recycle recv operation
  -> PostTcpRecv if relay still open and recv budget allows
```

### 6.2 QUIC → TCP（Phase 1 后）

```text
MsQuic callback
  -> CallbackBinding / relay lookup
  -> validate QUIC_BUFFER slices
  -> create TqWindowsPendingQuicReceive metadata only
  -> account pending receive bytes / queue
  -> PostQueuedCompletionStatus(QuicReceiveViewQueued)
  -> return QUIC_STATUS_PENDING

IOCP worker
  -> HandleQuicReceiveViewQueued
  -> WSASend borrowed QUIC slice buffer
  -> TcpSend complete advances slice offset
  -> StreamReceiveComplete(bytes or aggregated bytes)
  -> view completion releases pending budget and handles FIN
```

压缩路径保留 owned-buffer copy：

```text
MsQuic callback
  -> copy QUIC_BUFFER payload into owned buffer
  -> return QUIC_STATUS_SUCCESS
  -> worker decompresses and WSASend output
```

## 7. 测试与验证

### 7.1 单元测试

| 测试 | 文件 | 验证点 |
|------|------|--------|
| start/stop smoke | `src/unittest/windows_relay_worker_test.cpp` | 现有行为保留 |
| deferred receive view | `windows_relay_worker_test.cpp` | 非压缩 RECEIVE 返回 `QUIC_STATUS_PENDING`，只保存 slice 元数据，不复制 payload |
| receive complete | `windows_relay_worker_test.cpp` | TCP `WSASend` completion 后按字节调用 `StreamReceiveComplete` |
| partial WSASend | `windows_relay_worker_test.cpp` | partial completion 推进 slice offset，未写完不 complete 未发送字节 |
| complete 聚合 | `windows_relay_worker_test.cpp` | 128KB/256KB 等阈值下延迟 complete，view 完成/关闭强制 flush |
| pending budget | `windows_relay_worker_test.cpp` | 超过 pending receive bytes 上限时 pause/resume 或关闭，不丢 pending view |
| close/drain | `windows_relay_worker_test.cpp` | close/abort/unregister 强制 flush 已完成字节，释放 pending budget |
| TCP recv operation reuse | `windows_relay_worker_test.cpp` | 多次 `SEND_COMPLETE` 后 `TcpRecvOperationsReused > 0` |
| runtime selection | `relay_backend_selection_test`（新增 Windows 分支断言） | Windows runtime 使用 `WindowsWorker` backend |

### 7.2 性能验证

Windows 本地：

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test tcpquic_tunnel_test tcpquic_tuning_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

Windows perf/ETW 建议：

```powershell
# 采集 CPU + heap + winsock/afd 事件（具体 provider 按环境调整）
wpr -start GeneralProfile -start CPU -filemode
# 运行 tcpquic-proxy throughput 用例
wpr -stop windows-relay-phase1.etl
```

验收标准：

- `tcpquic_windows_relay_worker_test` 覆盖 deferred receive view、receive complete、partial send、budget、close/drain 并稳定通过。
- Phase 1 后，非压缩 QUIC receive callback 返回 `QUIC_STATUS_PENDING`，relay 不再 copy payload 到 owned receive buffer。
- Phase 2 后，pending receive bytes/queue、`StreamReceiveComplete` bytes/calls、pause/resume、TCP send bytes/calls 都可观测；complete 聚合阈值可配置。
- Windows throughput 不低于优化前，且 CPU/heap allocation profile 显示 QUIC→TCP receive payload copy 和 receive-side allocation 明显下降。

## 8. 风险与缓解

| 风险 | 缓解 |
|------|------|
| Pending receive view 生命周期 UAF | callback 返回 `PENDING` 后 view 保留 slices；只有 view 完成、错误 flush 或 close flush 后才 `StreamReceiveComplete`；单测覆盖 callback 返回后写入 |
| `StreamReceiveComplete` 字节不匹配 | 只按 TCP completion bytes 推进；partial send 不 complete 未写字节；view 完成时断言 completed <= total |
| complete 聚合过大导致 flow control 卡住 | 默认阈值 0；128KB/256KB/512KB/1MB 作为实验矩阵；pending bytes 和 pause/resume 指标兜底 |
| binding 生命周期 UAF | `Closing + CallbackRefs + Relay=nullptr` 协议；并发 stop/callback 单测 |
| receive pause/resume API 不可用 | 第一版可关闭 relay 作为 fallback，但必须记录 backpressure；后续接入 `StreamReceiveSetEnabled` |
| 压缩路径 buffer ownership 复杂 | 压缩路径不走 borrowed view，继续 copy 到 owned buffer 后解压 |
| 多 outstanding recv 改变 TCP→QUIC ordering | 默认不启用；启用前证明 completion order 与 send order 处理策略 |

## 9. 实施顺序与里程碑

```text
M1 (Phase 1, 2-3 天)
  ├─ Windows always-pending snapshot/metrics
  ├─ TqWindowsPendingQuicReceive view
  ├─ RECEIVE callback 返回 QUIC_STATUS_PENDING
  ├─ IOCP worker borrowed-buffer WSASend
  └─ StreamReceiveComplete 字节级单测

M2 (Phase 2, 2-3 天)
  ├─ CallbackBinding 替代 weak_ptr hot path
  ├─ pending receive bytes budget
  ├─ receive pause/resume 或关闭 fallback
  ├─ complete 聚合阈值
  └─ close/drain/并发 callback 单测

M3 (Phase 3, 1-2 天，按 perf 决定)
  ├─ TCP recv operation 复用
  ├─ 可选多 outstanding recv
  └─ ETW/吞吐复测

M4 (后续专项)
  └─ Windows UDP/socket buffer、RSS/offload/IRQ affinity 调查
```

## 10. 参考

- `docs/finished/superpowers/specs/2026-06-11-relay-perf-hotspot-optimization-design.md`
- `docs/finished/superpowers/plans/2026-06-11-relay-perf-hotspot-optimization.md`
- `src/tunnel/windows_relay_worker.cpp`
- `src/tunnel/windows_relay_worker.h`
- `src/unittest/windows_relay_worker_test.cpp`
- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/linux_relay_buffer_pool.cpp`
- `src/tunnel/linux_relay_event_queue.h`

## Always-pending Windows 验证结果（2026-06-13）

- Build: `143279c7fb1c2cedf063c2f11342470f13c163fc`，working tree 含既有未提交改动（包括 Windows relay、tuning、buffer pool、文档与构建产物）；未创建 git commit。
- Tests: PASS
  - `cmake --build "C:\src\tcpquic-proxy\build-win" --target tcpquic-proxy tcpquic_windows_relay_worker_test tcpquic_tunnel_test tcpquic_tuning_test tcpquic_relay_buffer_pool_test -j 2`
  - `.\build-win\bin\Release\tcpquic_windows_relay_worker_test.exe`
  - `.\build-win\bin\Release\tcpquic_tunnel_test.exe`
  - `.\build-win\bin\Release\tcpquic_tuning_test.exe`
  - `.\build-win\bin\Release\tcpquic_relay_buffer_pool_test.exe`
- ETW: skipped，`wpr -help` 可用且 `wpr -status` 显示未录制，但最小本机采集 `wpr -start GeneralProfile -start CPU -filemode` 失败：`Failed to enable the policy to profile system performance`，错误码 `0xc5585011`，退出码 `-984068079`；未生成 ETL，当前会话疑似缺少启用系统性能分析策略所需权限/策略。
- Throughput: skipped，当前会话没有两台 Windows 测试机，未配置远端 OpenSSH，也未提供吞吐用例参数；仅执行本机构建与单测，不伪造吞吐结果。
- Deferred receive: N/A，未运行双机吞吐或带指标采样的 relay workload；本机单测仅验证逻辑路径并有 `tcpquic_tuning_test` runtime 输出。
- Allocation observation: N/A，WPR 采集启动失败，未获得 ETW/WPA heap allocation 观察。
- Decision: 当前本机构建与 Windows 单测通过，可继续保留 always-pending Windows relay 实现；但缺少 ETW 与双机吞吐证据，合并或调参前需在具备管理员/性能分析权限与双机 Windows 环境中补跑 ETW、heap profile 和吞吐验证。
