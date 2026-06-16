# Relay 按需 Buffer + mimalloc 统一设计

> 日期：2026-06-16  
> 状态：已评审（Phase 1: Linux；Phase 2: Windows）  
> 前置讨论：放弃 per-relay `Reserve()` 对象池与 worker 级共享 free list；改为 relay 热路径按需分配/释放，通过 `TqAllocBytes` 显式使用 vendored mimalloc；保留现有 QUIC↔TCP 背压语义。

## 1. 背景与问题

### 1.1 当前实现

Linux 生产路径中，每个 `RelayState` 持有一个 `TqRelayBufferPool`：

```text
RelayState 构造
  └─ Pool(ReadChunkSize, WorkerSlots, MaxPendingBytes)
       └─ Pool.Reserve(WorkerSlots)   // 启动时预分配全部 slot
```

默认参数（Wan）下：`128 KiB × 128 slots = 16 MiB`/relay，在 relay 创建时即提交物理内存（通过 `std::vector<uint8_t>`）。Windows relay 同样使用 `TqRelayBufferPool`，但 `Reserve` 数量更小（recv 2、send 按 inflight）。

池同时承担两类职责：

| 职责 | 机制 |
|------|------|
| **内存复用** | `WorkerFree` free list + `Reserve` 预分配 |
| **字节背压** | `PendingReserved` atomic，Acquire 时 `+= ChunkBytes`，Release 时 `-=` |
| **slot 上限** | `WorkerSlots` / `SlotLimit` |

### 1.2 问题

1. **空间换性能过度**：idle 或小流量 relay 仍占用完整 `Reserve` 容量。
2. **无法按流量伸缩**：所有 relay 共享相同 slot 配置，大流与小流无差别。
3. **RSS 与连接数线性增长**：`N relay × Reserve(slots)`，与真实在途数据量脱钩。
4. **perf 中 malloc 非主瓶颈**：当前 `malloc`/`_int_malloc` 约占 0.05%–0.3%；主成本在 QUIC↔TCP 数据路径拷贝，换分配器 alone 不能解决吞吐上限。

### 1.3 决策摘要（已确认）

- **不做** worker 级共享 buffer 池（无 target/Grow/Trim chunk 逻辑）。
- **采用** vendored mimalloc + relay 按需 `Allocate` / `Free`；Linux 先迁移，Windows 第二阶段迁移到同一 API。
- **保留** 现有 QUIC→TCP / TCP→QUIC 背压（pause/resume、`ArmTcpReadable`、`PendingQuicReceiveBytes` 等）。

---

## 2. 目标与非目标

### 2.1 目标

| 指标 | 当前 | 目标 |
|------|------|------|
| relay 创建时 buffer RSS | `WorkerSlots × ReadChunkSize` 预分配 | **0**（无 Reserve） |
| 在途 buffer 内存 | 与 Reserve 规模绑定 | 与 **实际 Acquire 数量 × ChunkSize** 成正比 |
| 背压行为 | Pool `PendingBytes` + QUIC receive 暂停 | **语义不变**，计数迁移到 relay/worker |
| 分配器 | glibc malloc / vector 内部堆 | **mimalloc**（经 `TqAllocBytes` 显式调用；默认静态链接） |
| 代码复杂度 | Pool + Reserve + free list | **Allocate/Free + 背压计数**（删除 slot 概念） |

### 2.2 非目标

- 不引入 worker 级共享 free list、128MB chunk Grow/Trim、per-relay 保底账户。
- 不修改 MsQuic callback 线程模型与 `QUIC_STATUS_PENDING` 语义。
- 不优化 zstd 压缩算法本身；压缩路径仍允许 copy 到 owned buffer。
- 第一阶段不要求 Windows 与 Linux 同时上线：Phase 1 Linux-first，Phase 2 Windows 跟进到同一 `relay_buffer` / `relay_alloc` API。
- 不保证 RSS 与 VSZ 实时一致；mimalloc 可能在 heap 内缓存 freed block。

---

## 3. 方案对比

### 方案 A：vendored mimalloc + relay 按需 alloc（**选定**）

```text
Acquire → atomic 背压预留 → TqAllocBytes(ReadChunkSize) → TqBufferHandle
Release → TqFreeBytes → PendingBytes -= ChunkSize
```

- **优点**：实现最简单；idle relay 零 buffer；与现有 RAII 模型兼容。
- **缺点**：热路径 malloc/free 频率高（readv 批次最多 `MaxIov` 次/tick）；RSS 回落依赖 mimalloc 行为。

### 方案 B：Worker 级共享池 + lazy Grow（前序讨论方案）

- **优点**：热路径多数为 free list pop/push；Grow/Trim 可控。
- **缺点**：需维护 target、chunk、Trim；用户已明确不采用。

### 方案 C：仅去掉 Reserve，保留 per-relay free list（无 mimalloc）

- **优点**：无新依赖；Acquire 仍从 relay 本地 free list 取。
- **缺点**：relay 关闭前 free list 可能仍囤积 slot；内存节省不如 A 彻底。

**推荐 A**：符合用户决策，改动面清晰，背压可原样迁移。

---

## 4. 架构概览

```text
                    ┌─────────────────────────────────────┐
                    │  mimalloc (vendored static library)   │
                    └─────────────────────────────────────┘
                                      ▲
                          mi_malloc / mi_free
                                      │
┌─────────────────────────────────────┴──────────────────────────────┐
│ TqLinuxRelayWorker (single thread)                                  │
│   RelayState[]                                                      │
│     ├─ PendingBufferBytes      ← atomic，在途 owned buffer 字节数   │
│     ├─ MaxPendingBufferBytes   ← 来自 tuning（per-relay 上限）      │
│     ├─ PendingQuicReceiveBytes ← 已有，不变                         │
│     └─ (无 TqRelayBufferPool)                                       │
│                                                                      │
│   DrainTcpReadable / Decompress / Compress path:                    │
│     TqAllocateRelayBuffer(relay, chunkSize) → TqBufferRef           │
│     TqBufferRef 析构/reset → TqFreeBytes + PendingBufferBytes 回退  │
└──────────────────────────────────────────────────────────────────────┘
```

数据流不变：`TqBufferView` 仍携带 `Owner`（`TqBufferRef`），在 TCP/QUIC 写完成或队列 drain 后释放。

---

## 5. 组件设计

### 5.1 `TqRelayBuffer`（替代 pool slot + vector 预分配）

```cpp
struct TqRelayBuffer {
    uint8_t* Data{nullptr};
    size_t Capacity{0};
    size_t Length{0};   // 可选：SetLength 语义保留
};
```

- `Data` 由 `TqAllocBytes(size_t bytes)` 分配，`TqFreeBytes(void*, size_t)` 释放。
- 固定使用 `Config.ReadChunkSize`（默认 128 KiB），不在热路径使用变长块。

### 5.2 `TqBufferHandle` / `TqBufferRef`（保留，改所有权）

```cpp
struct TqRelayBufferBudget {
    std::atomic<uint64_t> PendingBufferBytes{0};
    uint64_t MaxPendingBufferBytes{0};
    std::atomic<uint64_t> AllocateCount{0};
};

class TqBufferHandle {
    TqRelayBuffer* Buffer{nullptr};
    TqRelayBufferBudget* Budget{nullptr}; // 用于 PendingBufferBytes 记账
    size_t ReservedBytes{0};      // Acquire 时计入的字节数
};
```

- `RelayState` 继承或嵌入 `TqRelayBufferBudget`；Windows Phase 2 迁移时复用同一预算 struct。
- **Acquire**：通过 atomic compare/exchange 预留 pending budget 后 `TqAllocBytes`；分配失败必须回退已预留字节。
- **reset/析构**：`TqFreeBytes`；`Budget->PendingBufferBytes.fetch_sub(ReservedBytes)`（或 atomic 饱和减）。
- 仍 **move-only**，禁止拷贝（与现有 static_assert 一致）。

### 5.3 背压：两层独立预算

#### 层 1：Owned buffer 字节预算（原 Pool `PendingReserved`）

| 检查点 | 条件 | 动作 |
|--------|------|------|
| TCP read (`DrainTcpReadable`) | atomic 预留时发现 `PendingBufferBytes + ChunkSize > MaxPendingBufferBytes` | `ArmTcpReadable(false)`，停止本 batch |
| TCP→QUIC compress 分块 | 同上 | 返回 false，记录 failure |
| QUIC→TCP zstd 解压输出 | 同上 | 返回 false，`ArmTcpWritable(true)` 等待 drain |

失败原因映射：

- `PendingBytesLimit` → 保持现有 metric 名与 admin 字段。
- **删除** `SlotLimit` 路径与对应 metrics（无 slot 上限）。

#### 层 2：QUIC receive 队列预算（不变）

- `PendingQuicReceiveBytes` vs `MaxPendingQuicReceiveBytesPerRelay`（来自 `LinuxRelayPerTunnelPendingBytes`）。
- `MaybePauseQuicReceive` / `MaybeResumeQuicReceive` 逻辑不变。
- plain QUIC→TCP 仍借用 MsQuic slice；zstd 路径使用 owned buffer。

#### 层 3：Worker 级汇总（可选，仅观测）

- `Snapshot()` 继续汇总 `PendingBufferBytes` 到 worker/process metrics。
- **不**新增 worker 级 hard cap（除非后续压测证明需要）；hard cap 仍在 per-relay `MaxPendingBufferBytes`。

### 5.4 mimalloc 集成（third_party 静态链接）

mimalloc 与 msquic、zstd、spdlog 相同：**vendored Git 子模块 + 源码静态链入**，不依赖系统 `libmimalloc-dev`，运行时无需 `libmimalloc.so`。

**目录与版本**

```text
third_party/mimalloc/          # git submodule → https://github.com/microsoft/mimalloc
.gitmodules                    # 登记子模块路径
```

初始化：

```bash
git submodule update --init --recursive third_party/mimalloc
```

**构建（CMake）**

根 `CMakeLists.txt`（在 `add_subdirectory(src)` 之前）：

```cmake
option(TCPQUIC_USE_MIMALLOC "Statically link vendored mimalloc for relay buffers" ON)

set(TCPQUIC_MIMALLOC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc" CACHE PATH "mimalloc source tree")
if(TCPQUIC_USE_MIMALLOC AND NOT EXISTS "${TCPQUIC_MIMALLOC_SOURCE_DIR}/include/mimalloc.h")
    message(FATAL_ERROR "mimalloc submodule missing. Run: git submodule update --init --recursive")
endif()

if(TCPQUIC_USE_MIMALLOC)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${TCPQUIC_MIMALLOC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mimalloc")
endif()
```

链接（Phase 1: `tcpquic-proxy` 与各 Linux relay 测试 target；Phase 2: Windows relay worker 相关 target 也使用同一 helper）：

```cmake
if(TCPQUIC_USE_MIMALLOC)
    target_link_libraries(${target} PRIVATE mimalloc-static)
endif()
```

**关闭 mimalloc（对照 / 回滚）**

`-DTCPQUIC_USE_MIMALLOC=OFF`：`TqAllocBytes` 回退 `std::malloc`/`std::free`，不编译 `third_party/mimalloc`。

**分配 API 封装**（便于单测与 ASan）

```cpp
void* TqAllocBytes(size_t bytes);   // mi_malloc 或 std::malloc
void  TqFreeBytes(void* p, size_t bytes);
```

**RSS 回收（可选，后续优化）**

- relay 大量关闭或 idle 峰值过后，可调用 `mi_collect(true)`（进程级，低频）。
- 不在每次 `Free` 后调用，避免热路径开销。

**说明：** 静态链入后，MsQuic/zstd/spdlog 等仍各自使用 `malloc`；只有显式调用 `TqAllocBytes` / `mi_malloc` 的路径走 mimalloc 堆。若后续需要进程级全局替换，可再评估 override 层；本设计当前只要求 relay owned buffer 统一走 mimalloc。

---

## 6. 配置变更

### 6.1 删除 / 废弃

| 配置 | 处理 |
|------|------|
| `relay.linux.worker_slots` | **删除**；解析时 warn 并忽略 |
| `TuningOverrideLinuxRelayWorkerSlots` | 删除 |
| `LinuxRelayWorkerSlots` | 删除 |

### 6.2 保留 / 重命名（文档层）

| 现有 | 语义 | 变更 |
|------|------|------|
| `LinuxRelayReadChunkSize` | 单次 buffer 大小 | 不变 |
| `LinuxRelayPerWorkerPendingBytes` | 名称误导 | **重命名为 `MaxPendingBufferBytesPerRelay`**（代码 + 配置文档同步） |
| `LinuxRelayPerTunnelPendingBytes` | QUIC receive 队列上限 | 不变 |
| `LinuxRelayGlobalPendingBytes` | 进程级预算推导 | 可用于推导 per-relay 默认上限（`TqComputeTuning` 不变） |

### 6.3 新增（可选）

| 配置 | 默认 | 说明 |
|------|------|------|
| `TCPQUIC_USE_MIMALLOC` | ON | 构建开关 |
| `relay.linux.mimalloc_collect_on_relay_close` | false | 关闭 relay 时是否 `mi_collect` |

---

## 7. Metrics 与 Admin

### 7.1 删除

- `linux_relay_worker_slots_allocated` / `linux_relay_worker_slots_free`
- 所有 `*_slot_limit_failures` 计数器
- `TqRelayBufferPool::{FreeCount, AllocatedCount, Reserve}`

### 7.2 保留

- `*_pending_budget_failures`（`PendingBytesLimit`）
- `*_alloc_failures`（`mi_malloc` 返回 nullptr）
- `BufferAcquireCount` → 改为 relay/worker 级 `AllocateCount`
- `PendingBytes` 汇总（worker snapshot）

### 7.3 新增（建议）

- `linux_relay_buffer_bytes_in_use`：sum(`PendingBufferBytes`)
- `linux_relay_buffer_alloc_bytes_total`：累计分配字节（可选）

---

## 8. 文件级改动清单

### 8.1 核心

| 文件 | 改动 |
|------|------|
| `src/tunnel/relay_buffer.h` | 新增：`TqRelayBufferBudget`、`TqRelayBuffer`、`TqBufferHandle`、`TqBufferView`、`TqAllocateRelayBuffer` |
| `src/tunnel/relay_buffer.cpp` | 新增：alloc/free + 背压 helper |
| `src/tunnel/relay_alloc.h/.cpp` | 新增：`TqAllocBytes` / `TqFreeBytes`，按 `TCPQUIC_USE_MIMALLOC` 选择 `mi_malloc` 或 `std::malloc` |
| `src/tunnel/relay_buffer_pool.h/.cpp` | Phase 1 保留给 Windows 旧路径；Phase 2 Windows 迁移后删除 |
| `src/tunnel/linux_relay_worker.h` | `RelayState` 增加 `PendingBufferBytes`、`MaxPendingBufferBytes`；移除 `Pool` |
| `src/tunnel/linux_relay_worker.cpp` | 所有 `relay->Pool.*` → `TqAllocateRelayBuffer(relay, ...)` |
| `src/tunnel/windows_relay_worker.cpp` | Phase 2 同上，迁移 `TcpRecvBuffers` / `TcpSendBuffers` |
| `CMakeLists.txt` / `src/CMakeLists.txt` | `third_party/mimalloc` 子模块 + 静态链接 `mimalloc-static` |
| `.gitmodules` | 登记 `third_party/mimalloc` 子模块 |

### 8.2 配置 / 观测

| 文件 | 改动 |
|------|------|
| `src/config/tuning.cpp` | 删除 worker_slots 打印；保留 pending 推导 |
| `src/config/config.cpp` | 废弃 worker_slots 解析 |
| `src/runtime/relay_metrics.cpp` | Phase 1 更新 Linux JSON 字段；Phase 2 再同步 Windows 相关 pool 指标 |

### 8.3 测试

| 文件 | 改动 |
|------|------|
| `src/unittest/relay_buffer_test.cpp` | 新增 alloc/backpressure 单测 |
| `src/unittest/linux_relay_buffer_pool_test.cpp` | Phase 1 合并或删除 |
| `src/unittest/relay_buffer_pool_test.cpp` | Phase 1 可保留 legacy pool 测试；Phase 2 随旧 pool 删除 |
| `src/unittest/linux_relay_worker_io_test.cpp` | 更新断言（无 slot metrics） |

---

## 9. 热路径分配频率估算

| 路径 | 每次触发 | 块大小 | 备注 |
|------|----------|--------|------|
| TCP→QUIC readv | ≤ `Min(MaxIov, ReadBatchBytes/ChunkSize)` | 128 KiB | 每 worker tick 一次 batch |
| TCP→QUIC zstd 压缩输出 | 压缩输出长度 / ChunkSize | 128 KiB | 仅压缩 relay |
| QUIC→TCP zstd 解压 | 每 decompress 迭代 1 块 | 128 KiB | plain 路径不 alloc |

Worst case（单 relay 满速）：~16 alloc/批次 × 批次频率。在 mimalloc 下通常可接受，**必须以压测验证** p99 延迟与 CPU。

---

## 10. 错误处理

| 场景 | 行为 |
|------|------|
| `mi_malloc` 失败 | 返回空 `TqBufferRef`，`AllocationFailure`，记录 metric；TCP 侧 `ArmTcpReadable(false)` |
| `PendingBytesLimit` | 与现有一致，不分配 |
| relay 关闭时仍有 in-flight buffer | `TqBufferHandle` RAII 保证在 `RelayState` 销毁前全部 reset；关闭顺序：先 drain 队列，再析构 relay |
| double-free | 禁止；`TqBufferHandle` move-only，Pool 指针改为 raw buffer + relay 指针 |

---

## 11. 测试计划

### 11.1 单元测试

1. Acquire 成功 → `PendingBufferBytes == ChunkSize`；Release → 0。
2. 超过 `MaxPendingBufferBytes` → `PendingBytesLimit`，不分配。
3. `mi_malloc` 失败注入（mock allocator）→ `AllocationFailure`。
4. move-only / 析构顺序：`TqBufferView` 链释放后字节计数归零。

### 11.2 集成 / IO 测试

- 复用 `linux_relay_worker_io_test`：高 BDP readv、zstd round-trip、relay 批量 register/unregister。
- 验证 unregister 后 `PendingBufferBytes` 进程级为 0。

### 11.3 压测 / 回归

- DGX 脚本：`bench-tcpquic-multi-curl.sh`、`dgx-perf-profile.sh` 对比改前改后。
- 观测：吞吐、CPU、`malloc` 占比、RSS vs active relay 数。
- 对比 `TCPQUIC_USE_MIMALLOC=OFF`（glibc）确认 mimalloc 净收益。

---

## 12. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 热路径 alloc 频率导致 CPU 上升 | mimalloc + 固定块大小；压测；若不足再考虑 per-worker 热缓存（非本方案） |
| RSS 高于在途 buffer（mimalloc 缓存） | 文档说明；可选 `mi_collect`；监控 `buffer_bytes_in_use` vs RSS |
| MsQuic/zstd 与 mimalloc 交互 | 长跑 + ASan/LSan 构建 |
| 配置迁移 | worker_slots 废弃 warn；文档更新 |
| Windows 滞后 | Linux-first 合并；Windows 跟进同一 `TqAllocateRelayBuffer` API |

---

## 13. 实施阶段

| 阶段 | 内容 | 验收 |
|------|------|------|
| **P1** | `relay_buffer` API + Linux `RelayState` 背压计数 + 删除 Pool/Reserve | unit test + io test 绿 |
| **P2** | `third_party/mimalloc` 子模块 + CMake 静态链接 + `TqAllocBytes` 封装 | Release 链入 `mimalloc-static` |
| **P3** | metrics/admin 清理 + 废弃 config | JSON 无 slot 字段 |
| **P4** | Windows relay 对齐（**独立 PR**） | windows_relay_worker_test 绿 |
| **P5** | DGX perf 对比 + 文档 | 吞吐不低于基线 ±2%；idle RSS 明显下降 |

---

## 14. 评审结论（2026-06-16 已确认）

| 问题 | 决策 |
|------|------|
| Pending 上限字段命名 | **`MaxPendingBufferBytesPerRelay`**（替换 `LinuxRelayPerWorkerPendingBytes`） |
| mimalloc 集成方式 | **`third_party/mimalloc` Git 子模块，静态链接 `mimalloc-static`**（与 msquic/zstd 相同 vendored 模式） |
| mimalloc 构建默认 | **`TCPQUIC_USE_MIMALLOC` 默认 ON** |
| Windows 范围 | **Linux-first 本 PR；Windows 跟进独立 PR** |

### 14.1 Vendored 依赖一览

| 组件 | 子模块路径 | 用途 | 链接方式 |
|------|-----------|------|----------|
| mimalloc | `third_party/mimalloc` | relay buffer 按需分配 | 静态 `mimalloc-static` |

初始化：`git submodule update --init --recursive third_party/mimalloc`

### 14.2 字段迁移说明

- `TqTuningConfig::LinuxRelayPerWorkerPendingBytes` → `MaxPendingBufferBytesPerRelay`
- `RelayState::MaxPendingBufferBytes` 初始化取自 tuning 新字段
- 配置 JSON/YAML 键名若对外暴露，增加别名兼容期（可选）：旧键读入后映射到新字段并打 warn

---

## 15. 参考

- 现有实现：`src/tunnel/relay_buffer_pool.{h,cpp}`、`src/tunnel/linux_relay_worker.cpp`
- 背压：`MaybePauseQuicReceive`、`DrainTcpReadable` 中 `ArmTcpReadable`
- 前序 worker 池方案（已否决）：本文档第 3 节方案 B
