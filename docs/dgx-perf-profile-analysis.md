# tcpquic-proxy 高速吞吐 perf 热点分析

- 时间: 2026-06-11
- 环境: DGX spark-1619 (169.254.250.230) ↔ spark-1b6f (169.254.59.196)，无时延 200G LAN
- 场景: `proxy-1x1`（单 QUIC + 循环 curl 下载，持续填满 25s 采样窗口）
- 工具: `perf record -F 999 -g --call-graph dwarf`，`scripts/dgx-perf-profile.sh`
- 原始数据: `docs/dgx-perf-profile-rerun/`

## 实测吞吐（采样同期）

| 角色 | 吞吐 |
|------|------|
| tcpquic-proxy client | ~8.5 Gbps（`speed_download≈1.06e9 B/s`） |
| secnetperf 单连接（对照） | ~7.7 Gbps（`773156 kbps`） |

> 本轮采样中 proxy 与 secnetperf 绝对吞吐接近，但 **proxy 多消耗约 40% CPU 在 relay 层**，同等 CPU 预算下会拉开与 msquic 的差距。

---

## Client 热点（本机 tcpquic-proxy client）

采样 212168 次，leaf 符号 Top：

| 占比 | 符号 | 归属 |
|------|------|------|
| **19.0%** | `__aarch64_swp4_rel` | pthread mutex 原子操作 |
| **13.7%** | `__aarch64_cas4_acq` | pthread mutex 原子操作 |
| **7.0%** | `TqLinuxRelayBufferPool::Acquire()` | proxy relay |
| **4.6%** | `__aarch64_cas8_rel` | 原子 / 锁 |
| **3.9%** | `_int_malloc` | glibc 堆 |
| **3.8%** | `__aarch64_cas8_acq` | 原子 / 锁 |
| **3.2%** | `TqLinuxRelayWorker::DrainTcpReadable` | proxy relay |
| **2.7%** | `___pthread_mutex_lock` | 锁 |
| **2.6%** | `_int_free` | glibc 堆 |
| **2.1%** | `aes_gcm_dec_256_kernel` | QUIC/TLS 解密 |
| **1.3%** | `TqLinuxRelayBufferPool::Release` | proxy relay |
| **1.2%** | `__GI___readv` | 内核 TCP 读 |

**调用栈聚合（tcpquic-proxy 二进制内）**

| 栈帧出现次数 | 函数 |
|-------------|------|
| 179252 | `DrainTcpReadable` |
| 162766 | `Run` |
| 57041 | `Acquire` |
| 41843 | `shared_ptr deleter → Release` |
| 8242 | `QuicWorkerThread` |
| 6361 | `CxPlatSocketReceiveCoalesced` |
| 4431 | `aes_gcm_dec_256_kernel` |

**Client 结论**

1. **最大热点是 `TqLinuxRelayBufferPool` 的 mutex 竞争**（`swp`/`cas` 合计 ~33% + `Acquire`/`Release` ~8%）。MsQuic 回调线程 `CopyQuicReceiveToEvent` 与 relay worker 线程 `DrainTcpReadable`/`FlushTcpWrites` **同时抢同一把 `Pool.Lock`**。
2. **堆分配占 ~7%**（`malloc`/`free` + `shared_ptr` deleter），与 buffer 池 acquire/release 频率一致。
3. **TLS 解密仅 ~2%**（因 mutex 占满 CPU，占比被稀释；对照 secnetperf 见下文）。
4. **额外拷贝**：`OnStreamEvent(RECEIVE)` → `CopyQuicReceiveToEvent`（`memcpy` 进池化 buffer）→ `Enqueue` → `FlushTcpWrites`，相对 secnetperf 多 1–2 次内存拷贝。

---

## Server 热点（对端 tcpquic-proxy server）

采样 ~196K（`armv8_pmuv3_1`），leaf Top：

| 占比 | 符号 | 归属 |
|------|------|------|
| **19.2%** | `__aarch64_swp4_rel` | mutex 原子 |
| **13.7%** | `__aarch64_cas4_acq` | mutex 原子 |
| **6.9%** | `TqLinuxRelayBufferPool::Acquire()` | proxy relay |
| **6.0%** | `CxPlatHashtableEnumerateNext` | msquic（BBR/丢包检测） |
| **5.7%** | `__aarch64_cas8_rel` | 原子 |
| **5.2%** | `_int_malloc` | glibc |
| **3.3%** | `DrainTcpReadable` | 从 busybox HTTP 读 TCP |
| **3.0%** | `_int_free` | glibc |
| **1.4%** | `TqLinuxRelayBufferPool::Release` | proxy relay |
| **1.0%** | `__GI___readv` | 批量读 origin |
| **0.75%** | `aes_gcm_enc_256_kernel` | QUIC/TLS 加密 |
| **0.56%** | `FindRelayById` | relay 查找锁 |

**Server 结论**

- 下载方向 server 主路径：`DrainTcpReadable`（`readv` origin）→ `SubmitTcpBatchToQuic`（`Stream::Send`），同样被 **buffer 池 mutex + malloc** 主导。
- msquic 内部 `CxPlatHashtableEnumerateNext` ~6%（与 secnetperf 同源，非 proxy 独有）。

---

## secnetperf 基线（单连接，同 LAN）

| 占比 | 符号 |
|------|------|
| **23.3%** | `aes_gcm_dec_256_kernel` |
| **8.1%** | `__arch_copy_to_user`（UDP recvmmsg） |
| 4.7% | `__aarch64_ldadd8_acq_rel` |
| 2.3% | `__memcpy_sve` |

**对比要点**

| 开销类别 | secnetperf | tcpquic-proxy |
|----------|------------|---------------|
| TLS AES-GCM | **~23%**（主导） | ~2%（client）/ ~0.8%（server），被 relay 开销掩盖 |
| Buffer 池 mutex | **无** | **~33%** |
| malloc/free | 少量 | **~7–8%** |
| HTTP/TCP relay | **无** | `readv`/`writev` + 拷贝 + `FindRelayById` |
| msquic 哈希表遍历 | 有 | ~6%（相近） |

proxy 与 msquic 的差距，**主要不是 QUIC 栈本身**，而是 **relay 层 buffer 池锁竞争 + 堆分配 + 额外 memcpy**。优化 relay 后，TLS/UDP 会成为下一瓶颈（与 secnetperf 对齐）。

---

## 根因代码定位

### 1. Buffer 池全局互斥锁

```44:70:src/tunnel/linux_relay_buffer_pool.cpp
TqBufferRef TqLinuxRelayBufferPool::Acquire() {
    std::lock_guard<std::mutex> guard(Lock);
    // ...
    return TqBufferRef(buffer, [this](TqRelayBuffer* released) {
        Release(released);
    });
}
```

MsQuic 回调（`CopyQuicReceiveToEvent`）与 relay worker（`DrainTcpReadable`/`FlushTcpWrites`）**跨线程共享** `relay->Pool`。

### 2. QUIC 收包额外拷贝

```528:561:src/tunnel/linux_relay_worker.cpp
bool TqLinuxRelayWorker::CopyQuicReceiveToEvent(...) {
    while (offset < length) {
        auto buffer = relay->Pool.Acquire();
        std::memcpy(buffer->Data(), data + offset, chunk);
        Enqueue(event);
    }
}
```

MsQuic 已提供 `QUIC_BUFFER` 指针，proxy 仍拷贝进池再 `writev`。

### 3. Relay 查找锁

`FindRelayById` / `FindRelayIdByStream` 在 MsQuic 回调热路径上持 `RelayLock`。

### 4. 事件队列 mutex

`Enqueue` / `DrainEvents` 对 `QueueLock` 加锁，每个 QUIC receive 事件一次。

---

## 改进方案（按预期收益排序）

### P0 — Buffer 池去锁 / 降锁（预期回收 **25–35% CPU**）

1. **per-relay 无锁 free-list**：relay 注册时预分配 `MaxIov * 4` 个 buffer；Acquire/Release 仅在 owner worker 线程执行时用无锁栈，MsQuic 回调改为 **只投递指针到 SPSC 队列**，由 worker 归还。
2. 或 **thread-local 缓存**：回调线程批量 `Acquire(N)` 一次加锁，降低竞争频率。
3. 避免 `shared_ptr` deleter 触发的 `Release`：改用 intrusive refcount 或固定 ring buffer。

### P1 — 零拷贝 / 少拷贝（预期 **5–15%** + 降低延迟）

1. **QUIC receive → TCP writev 直通**：在 msquic 允许的生命周期内，直接把 `QUIC_BUFFER` 挂到 `PendingTcpWrites`，send 完成后再通知 msquic（需核对 `QUIC_STREAM_EVENT_RECEIVE` buffer 有效期）。
2. Server 方向：`readv` 进池后 `Stream::Send` — 评估 **registered buffer / 外部 QUIC_BUFFER** 是否可避免 `SubmitTcpBatchToQuic` 前的二次持有。

### P2 — 降低回调路径锁（预期 **3–8%**）

1. `Stream` 上下文存 `relayId` / `RelayState*`，去掉 `FindRelayIdByStream` 的 `RelayLock`。
2. `Enqueue` 改 **SPSC ring**（单生产者 MsQuic 回调，单消费者 relay worker），去掉 `QueueLock`。

### P3 — 与 msquic 对齐的公共开销（预期 **2–6%**，需改 msquic 或调参）

1. `CxPlatHashtableEnumerateNext` 出现在 BBR/丢包路径；可尝试 cubic、或减少 flow-control 汇总频率（属 msquic 内部）。
2. 构建 Release 时加 `-fno-omit-frame-pointer` 便于后续 perf（不提升吞吐，仅改善可观测性）。

### P4 — 架构层（中长期）

1. **收包在 MsQuic worker 线程直接 `writev`**（需仔细处理线程模型与 msquic 回调约束）。
2. **多连接扩展**时已验证聚合可达线速；单连接优化应聚焦 P0/P1。

---

## 复现命令

```bash
# proxy 单连接 perf（client + 建议另开终端抓 server）
CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh

# secnetperf 基线（需对端先起 server）
CASE=secnetperf-1conn DURATION_SEC=20 ./scripts/dgx-perf-profile.sh

# 查看报告
sudo perf report -f -i docs/dgx-perf-profile-rerun/client.perf.data --comm tcpquic-proxy
sudo perf report -f -i docs/dgx-perf-profile-rerun/server.perf.data --comm tcpquic-proxy
```

---

## 总结

| 层级 | 热点 | proxy 独有？ |
|------|------|-------------|
| relay buffer 池 mutex | ~33% | **是** |
| malloc/free | ~7% | **是**（池 + shared_ptr） |
| QUIC→TCP 额外 memcpy | 未单独量化，路径明确 | **是** |
| msquic 哈希表 | ~6% | 否 |
| TLS AES-GCM | 2–23%（视 CPU 余量） | 否 |
| UDP 内核拷贝 | ~1–8% | 否 |

**优先做 buffer 池无锁化 / 预分配**，这是 tcpquic-proxy 相对原始 msquic 最突出的差距；完成后预期单连接吞吐可更接近 secnetperf，下一瓶颈将回到 TLS 与 UDP 收包（与 msquic 基线一致）。

---

## Phase 1 优化结果（2026-06-11）

已完成 Phase 1 快速路径修复：

1. QUIC receive plain path 跳过 worker 侧第二次 buffer acquire/memcpy，`8192` 字节、`4096` chunk 的生产路径回归测试将 `BufferAcquireCount` 锁定为 `2`。
2. MsQuic stream callback 使用 `StreamRelayBinding` O(1) 定位 relay，生产路径回归测试将 `StreamLookupScanCount` 锁定为 `0`。
3. 同一个 `QUIC_STREAM_EVENT_RECEIVE` 中的多个 `QUIC_BUFFER` 合并为一个 relay 队列事件，回归测试验证 3-buffer receive 在 drain 前 `PendingEvents == 1`。

本地验证：

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test tcpquic_relay_backend_selection_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_relay_backend_selection_test
rtk cmake --build build -j2
```

结果：以上本地编译与单元测试均通过。

DGX 吞吐测试未在当前系统执行：当前系统不具备 DGX 测试环境。吞吐是否达到 `proxy-1x1 >= 8.5 Gbps` 需在 DGX 环境补跑 `CASE=proxy-1x1 ./scripts/dgx-throughput-matrix.sh` 后确认。

---

## Phase 2 优化结果（2026-06-11）

已完成 Phase 2 buffer 池低锁化：

1. `TqBufferRef` 从 `std::shared_ptr` 改为 move-only `TqBufferHandle`，移除 relay buffer 热路径上的 shared_ptr 控制块分配。
2. worker 线程路径使用 `AcquireWorker()` / `ReleaseWorker()` 无 mutex free-list，并在 relay 注册时预分配 worker slots。
3. QUIC callback receive 路径使用 `AcquireIngress()` 分配 ingress slot；worker 消费事件时通过 `TransferToWorker()` 转移归还域，不复制 payload，也不提前释放 TCP pending write 使用中的 buffer。

本地验证：

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test tcpquic_relay_backend_selection_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk ./build/bin/Release/tcpquic_relay_backend_selection_test
rtk cmake --build build -j2
```

结果：以上本地编译与单元测试均通过。

DGX perf 未在当前系统执行：当前系统不具备 DGX 测试环境。以下 Phase 2 验收项需在 DGX 环境补跑 `CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh` 后确认：

- `TqLinuxRelayBufferPool::Acquire` 不在 Top 10。
- mutex/原子符号合计 < 15%（目标 < 10%）。

---

## Phase 2 DGX 验证结果（2026-06-11 补测）

**代码**：`c74ac09` + `0f7260a`（worker 无锁 handle + ingress/worker 双域分离）

**注意**：`dgx-throughput-matrix.sh` 不会 rsync 二进制到对端；未先 deploy 时 Step2 仅 ~0.7 Gbps（curl_exit=18）。以下数据来自先 deploy 的 `dgx-msquic-vs-proxy-bench.sh` 与 `dgx-perf-profile.sh`。

### 吞吐（proxy-1x1，30s，无时延）

| 指标 | Phase 1 完整 | **Phase 2** | 变化 |
|------|-------------|-------------|------|
| msquic-vs-proxy proxy-1x1 | ~6.24 Gbps | **13.38 Gbps** | **+114%** |
| matrix Step2（需 deploy） | 6.29 Gbps | — | matrix 脚本需先 rsync |

### Perf 热点（proxy-1x1，25s @ 999Hz）

原始数据：`docs/dgx-perf-profile-phase2/`

| 热点 / 指标 | Phase 1 Client | Phase 2 Client | Phase 1 Server | Phase 2 Server |
|-------------|----------------|----------------|----------------|----------------|
| mutex 原子合计 (`swp4`+`cas4`+pthread) | **43.4%** | **2.6%** | **39.7%** | **3.7%** |
| `Acquire()` / `AcquireWorker()` | 8.2% / — | **0%** / — | 7.2% / — | — / **5.2%** |
| `__aarch64_swp4_rel` | 20.4% | 0.9% | **18.6%** | **0%** |
| `shared_ptr` deleter | 有 | **无** | 有（~8.3% 栈） | **无** |
| `aes_gcm_dec`（client） | 32.0% | 26.5% | — | — |
| `DrainTcpReadable`（server） | 3.5% | — | 3.5% | **9.3%** |

**Task 7 验收**：mutex 原子合计 < 15%（实际 ~3–4%）✅；`Acquire()` 已不在 Top 10（改为无锁 `AcquireWorker` ~5%）✅；吞吐超过 Phase 1 目标 9 Gbps ✅。

**结论**：Phase 2 消除 buffer 池跨线程 mutex 竞争，单流吞吐翻倍至 13+ Gbps；relay 热点从锁/分配转向 `DrainTcpReadable` + TLS/UDP 栈，与 secnetperf 对齐。下一步 Phase 3（事件队列低锁化）可继续压 `QueueLock`。
