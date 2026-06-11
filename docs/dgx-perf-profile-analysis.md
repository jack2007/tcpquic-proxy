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
