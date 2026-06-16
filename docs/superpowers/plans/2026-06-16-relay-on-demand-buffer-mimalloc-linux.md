# Relay 按需 Buffer + mimalloc（Phase 1: Linux）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 最终 Linux 与 Windows relay 都统一切换到按需 `Allocate`/`Free`（经 `TqAllocBytes` 封装）+ mimalloc；本计划是 Phase 1，仅迁移 Linux relay 路径，去掉 Linux per-relay `TqRelayBufferPool`/`Reserve`，保留现有 QUIC↔TCP 背压，并默认静态链接 vendored mimalloc。

**Architecture:** `RelayState` 持有 `PendingBufferBytes`/`MaxPendingBufferBytes`；`TqAllocateRelayBuffer(relay, chunkSize)` 在背压通过后分配固定块；`TqBufferRef` RAII 在 `reset` 时释放并更新计数。Windows 在 Phase 1 暂时继续使用旧 `TqRelayBufferPool`，Phase 2 迁移到同一 `relay_buffer` / `relay_alloc` API 后再删除旧 pool。

**Tech Stack:** C++17, CMake 3.20+, **vendored mimalloc**（`third_party/mimalloc` 静态链入 `mimalloc-static`，`TCPQUIC_USE_MIMALLOC` 默认 ON）, 现有 `TqLinuxRelayWorker` epoll 模型。

**Spec:** `docs/superpowers/specs/2026-06-16-relay-on-demand-buffer-mimalloc-design.md`

---

## File Map（Linux PR 范围）

| 文件 | 职责 |
|------|------|
| `src/tunnel/relay_buffer.h` | **新建** — `TqRelayBuffer`、`TqAllocateRelayBuffer`、`TqBufferHandle`、`TqBufferView` |
| `src/tunnel/relay_buffer.cpp` | **新建** — alloc/free + 背压检查 |
| `src/tunnel/relay_buffer_pool.h/.cpp` | **Phase 1 保留** — Windows `windows_relay_worker` 仍使用；Linux 不再 include；Phase 2 Windows 迁移后删除或改成兼容层 |
| `src/tunnel/linux_relay_buffer_pool.h` | **删除或留空 alias** — 当前仅为 `using TqRelayBufferPool`；Linux 改 include `relay_buffer.h` |
| `src/tunnel/linux_relay_worker.h/.cpp` | 移除 `Pool`；接入 `PendingBufferBytes` |
| `src/config/tuning.h/.cpp` | 重命名 pending 字段；删除 `LinuxRelayWorkerSlots` |
| `src/config/config.cpp` | 废弃 `worker_slots` 解析（warn） |
| `src/runtime/relay_metrics.h/.cpp` | 删除 slot metrics；新增 `buffer_bytes_in_use` |
| `third_party/mimalloc/` | **Git 子模块** — 静态编译为 `mimalloc-static` |
| `.gitmodules` | 登记 mimalloc 子模块 |
| `CMakeLists.txt` / `src/CMakeLists.txt` | `add_subdirectory(mimalloc)` + 链接 `mimalloc-static` |
| `src/unittest/relay_buffer_test.cpp` | **新建**（参考旧 `relay_buffer_pool_test` 的背压/RAII 断言） |
| `src/unittest/linux_relay_worker_io_test.cpp` | 更新 snapshot 断言 |

**不在 Phase 1：** `windows_relay_worker.cpp`、`windows_relay_worker_test.cpp` 的 buffer API 迁移。Phase 2 必须把 Windows 也切到 `TqAllocateRelayBuffer`/`TqAllocBytes`，并移除旧 pool 依赖。

---

## 构建与测试命令（全程复用）

```bash
# 配置（Release）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# mimalloc OFF 对照组
cmake -S . -B build-glibc -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_USE_MIMALLOC=OFF

# 编译 Linux 相关 target
cmake --build build --target \
  tcpquic-proxy \
  tcpquic_relay_buffer_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_tuning_test \
  tcpquic_config_router_test \
  -j$(nproc)

# 运行
./build/bin/Release/tcpquic_relay_buffer_test
./build/bin/Release/tcpquic_linux_relay_worker_io_test
./build/bin/Release/tcpquic_tuning_test
./build/bin/Release/tcpquic_config_router_test
```

---

### Task 1: 添加 `third_party/mimalloc` 子模块 + 静态链接 + 分配器封装

**Files:**
- Create: `.gitmodules` 条目（若尚无 mimalloc）
- Create: `third_party/mimalloc/`（git submodule）
- Create: `src/tunnel/relay_alloc.h`, `src/tunnel/relay_alloc.cpp`
- Modify: `CMakeLists.txt`（顶层 `add_subdirectory` mimalloc）
- Modify: `src/CMakeLists.txt`（链接 `mimalloc-static` 到 `tcpquic-proxy` 与 Linux 测试）

- [ ] **Step 1: 添加 Git 子模块**

```bash
git submodule add https://github.com/microsoft/mimalloc.git third_party/mimalloc
# 建议 pin 到稳定 tag，例如 v2.1.7（实施时选当前 verified tag）
git submodule update --init --recursive third_party/mimalloc
```

`.gitmodules` 示例：

```ini
[submodule "third_party/mimalloc"]
    path = third_party/mimalloc
    url = https://github.com/microsoft/mimalloc.git
```

- [ ] **Step 2: 根 CMake 集成（静态库，对齐 msquic/zstd 模式）**

在根 `CMakeLists.txt`（`add_subdirectory(src)` 之前，`spdlog` 块之后）：

```cmake
option(TCPQUIC_USE_MIMALLOC "Statically link vendored mimalloc for relay buffers" ON)

set(TCPQUIC_MIMALLOC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mimalloc" CACHE PATH "mimalloc source tree")
if(TCPQUIC_USE_MIMALLOC AND NOT EXISTS "${TCPQUIC_MIMALLOC_SOURCE_DIR}/include/mimalloc.h")
    message(FATAL_ERROR "mimalloc submodule missing. Run: git submodule update --init --recursive third_party/mimalloc")
endif()

if(TCPQUIC_USE_MIMALLOC)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${TCPQUIC_MIMALLOC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mimalloc")
endif()
```

- [ ] **Step 3: 链接 helper**

`src/CMakeLists.txt`：

```cmake
function(tcpquic_link_mimalloc target)
    if(TCPQUIC_USE_MIMALLOC)
        target_link_libraries(${target} PRIVATE mimalloc-static)
        target_include_directories(${target} PRIVATE
            "${TCPQUIC_MIMALLOC_SOURCE_DIR}/include")
    endif()
endfunction()
```

对所有编译 `relay_alloc.cpp` 的 target 调用 `tcpquic_link_mimalloc(...)`。Phase 1 至少包括 `tcpquic-proxy`、`tcpquic_relay_buffer_test`、`tcpquic_linux_relay_worker_queue_test`、`tcpquic_linux_relay_worker_io_test`、`tcpquic_relay_backend_selection_test`。

**验证静态链入：** `ldd build/bin/Release/tcpquic-proxy` 不应出现 `libmimalloc.so`；relay buffer 路径通过 `mi_malloc`/`mi_free` 使用 mimalloc 堆。

- [ ] **Step 4: 实现 `relay_alloc`**

`src/tunnel/relay_alloc.h`:

```cpp
#pragma once
#include <cstddef>

void* TqAllocBytes(size_t bytes);
void TqFreeBytes(void* ptr, size_t bytes);
```

`src/tunnel/relay_alloc.cpp`:

```cpp
#include "relay_alloc.h"
#include <cstdlib>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#endif

void* TqAllocBytes(size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_malloc(bytes);
#else
  return std::malloc(bytes);
#endif
}

void TqFreeBytes(void* ptr, size_t bytes) {
  (void)bytes;
  if (ptr == nullptr) return;
#if TCPQUIC_USE_MIMALLOC
  mi_free(ptr);
#else
  std::free(ptr);
#endif
}
```

在 `src/CMakeLists.txt` 为所有编译 `relay_alloc.cpp` 的 target 添加：

```cmake
target_compile_definitions(${target} PRIVATE
  $<IF:$<BOOL:${TCPQUIC_USE_MIMALLOC}>,TCPQUIC_USE_MIMALLOC=1,TCPQUIC_USE_MIMALLOC=0>)
```

Phase 1 至少包括 `tcpquic-proxy`、`tcpquic_relay_buffer_test`、`tcpquic_linux_relay_worker_queue_test`、`tcpquic_linux_relay_worker_io_test`、`tcpquic_relay_backend_selection_test`。Phase 2 迁移 Windows 时，再把 Windows relay worker 相关 target 加入同一 helper。

- [ ] **Step 5: 编译验证**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(nproc)
ldd build/bin/Release/tcpquic-proxy | rg mimalloc || echo "OK: no libmimalloc.so (static)"
```

- [ ] **Step 6: Commit**

```bash
git add .gitmodules third_party/mimalloc CMakeLists.txt src/CMakeLists.txt \
        src/tunnel/relay_alloc.h src/tunnel/relay_alloc.cpp
git commit -m "build: vendored mimalloc static lib in third_party for relay buffers"
```

---

### Task 2: `relay_buffer` API + 单元测试（TDD）

**Files:**
- Create: `src/tunnel/relay_buffer.h`, `src/tunnel/relay_buffer.cpp`
- Create: `src/unittest/relay_buffer_test.cpp`
- Modify: `src/CMakeLists.txt` — `tcpquic_relay_buffer_test` 使用新源文件

- [ ] **Step 1: 写失败测试 `relay_buffer_test.cpp`**

```cpp
#include "relay_buffer.h"
#include <cassert>

int main() {
    TqRelayBufferBudget relay{};
    relay.MaxPendingBufferBytes = 128 * 1024;

    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
    auto first = TqAllocateRelayBuffer(&relay, 64 * 1024, &failure);
    assert(first);
    assert(failure == TqBufferAcquireFailure::None);
    assert(relay.PendingBufferBytes.load() == 64 * 1024);
    assert(relay.AllocateCount.load() == 1);

    auto second = TqAllocateRelayBuffer(&relay, 64 * 1024, &failure);
    assert(second);
    assert(relay.PendingBufferBytes.load() == 128 * 1024);

    auto denied = TqAllocateRelayBuffer(&relay, 64 * 1024, &failure);
    assert(!denied);
    assert(failure == TqBufferAcquireFailure::PendingBytesLimit);

    first.reset();
    assert(relay.PendingBufferBytes.load() == 64 * 1024);

    second.reset();
    assert(relay.PendingBufferBytes.load() == 0);
    return 0;
}
```

为避免 fake 类型与真实 `RelayState` 漂移，在 `relay_buffer.h` 定义共享预算 struct，并让 `RelayState` 继承或嵌入：

```cpp
#include <atomic>
#include <cstdint>

struct TqRelayBufferBudget {
    std::atomic<uint64_t> PendingBufferBytes{0};
    uint64_t MaxPendingBufferBytes{0};
    std::atomic<uint64_t> AllocateCount{0};
};
```

`RelayState` 公开继承或嵌入该 struct，避免 fake 类型漂移。

- [ ] **Step 2: 注册 test target**

```cmake
add_executable(tcpquic_relay_buffer_test
    unittest/relay_buffer_test.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_relay_buffer_test)
tcpquic_link_mimalloc(tcpquic_relay_buffer_test)
```

- [ ] **Step 3: 运行测试 — 预期 FAIL（符号未定义）**

```bash
cmake --build build --target tcpquic_relay_buffer_test -j$(nproc)
./build/bin/Release/tcpquic_relay_buffer_test
```

- [ ] **Step 4: 实现 `relay_buffer.h/.cpp`**

`relay_buffer.h` 核心 API：

```cpp
enum class TqBufferAcquireFailure { None, PendingBytesLimit, AllocationFailure };

struct TqRelayBufferBudget {
    std::atomic<uint64_t> PendingBufferBytes{0};
    uint64_t MaxPendingBufferBytes{0};
    std::atomic<uint64_t> AllocateCount{0};
};

struct TqRelayBuffer {
    uint8_t* Data{nullptr};
    size_t Capacity{0};
    size_t UsedLength{0};
    void SetLength(size_t len);
};

class TqBufferHandle {
public:
    TqBufferHandle() = default;
    ~TqBufferHandle();
    TqBufferHandle(TqBufferHandle&&) noexcept;
    TqBufferHandle& operator=(TqBufferHandle&&) noexcept;
    TqBufferHandle(const TqBufferHandle&) = delete;
    TqBufferHandle& operator=(const TqBufferHandle&) = delete;

    TqRelayBuffer* get() const;
    TqRelayBuffer* operator->() const;
    explicit operator bool() const;
    void reset();

private:
    friend TqBufferHandle TqAllocateRelayBuffer(
        TqRelayBufferBudget* budget, size_t bytes, TqBufferAcquireFailure* failure);
    TqRelayBuffer* Buffer{nullptr};
    TqRelayBufferBudget* Budget{nullptr};
    size_t ReservedBytes{0};
};

using TqBufferRef = TqBufferHandle;

struct TqBufferView {
    uint8_t* Data{nullptr};
    size_t Len{0};
    TqBufferRef Owner;
};

TqBufferRef TqAllocateRelayBuffer(
    TqRelayBufferBudget* budget,
    size_t bytes,
    TqBufferAcquireFailure* failure = nullptr);
```

`TqAllocateRelayBuffer` 逻辑：

1. `bytes == 0` → 返回空。
2. 用 atomic compare/exchange 预留 pending bytes；若 `PendingBufferBytes + bytes > MaxPendingBufferBytes` → `PendingBytesLimit`，返回空。
3. `ptr = TqAllocBytes(bytes)`；失败 → `AllocationFailure`。
4. 填充 `TqRelayBuffer`，pending bytes 已预留，`AllocateCount.fetch_add(1)`。
5. 返回 owning `TqBufferHandle`。

`reset()`：`TqFreeBytes(Data, Capacity)`，`PendingBufferBytes.fetch_sub(ReservedBytes)`；若实现选择防御式饱和减，必须保持 atomic。

- [ ] **Step 5: 运行测试 — 预期 PASS**

```bash
cmake --build build --target tcpquic_relay_buffer_test -j$(nproc)
./build/bin/Release/tcpquic_relay_buffer_test
```

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/relay_buffer.h src/tunnel/relay_buffer.cpp \
        src/unittest/relay_buffer_test.cpp src/CMakeLists.txt
git commit -m "feat(linux): add on-demand relay buffer alloc with pending budget"
```

---

### Task 3: Tuning 字段重命名 + 删除 worker_slots

**Files:**
- Modify: `src/config/tuning.h`, `src/config/tuning.cpp`
- Modify: `src/config/config.cpp`, `src/config/config.h`
- Modify: `src/unittest/tuning_test.cpp`, `src/unittest/config_router_test.cpp`

- [ ] **Step 1: 重命名 tuning 字段**

`tuning.h`:

```cpp
// 删除: uint32_t LinuxRelayWorkerSlots{128};
uint64_t MaxPendingBufferBytesPerRelay{32ull * 1024 * 1024};  // 原 LinuxRelayPerWorkerPendingBytes
```

全局替换 `LinuxRelayPerWorkerPendingBytes` → `MaxPendingBufferBytesPerRelay`（Linux 路径与测试）。

- [ ] **Step 2: 删除 worker_slots 配置解析**

`config.cpp`：移除 `worker_slots` 分支；若 JSON 仍含该键，打 `spdlog::warn` 并忽略（可选兼容）。

- [ ] **Step 3: 更新 `TqPrintTuning`**

删除 `worker_slots=%u` 打印项；保留 `read_chunk` 与 pending 字节。

- [ ] **Step 4: 修复单元测试**

- `tuning_test.cpp`：删除所有 `LinuxRelayWorkerSlots` 断言。
- `config_router_test.cpp`：删除 `worker_slots:1024` fixture 或改为断言 warn-only。

- [ ] **Step 5: 运行测试**

```bash
cmake --build build --target tcpquic_tuning_test tcpquic_config_router_test -j$(nproc)
./build/bin/Release/tcpquic_tuning_test
./build/bin/Release/tcpquic_config_router_test
```

- [ ] **Step 6: Commit**

```bash
git add src/config/tuning.h src/config/tuning.cpp src/config/config.cpp \
        src/unittest/tuning_test.cpp src/unittest/config_router_test.cpp
git commit -m "refactor(config): rename MaxPendingBufferBytesPerRelay, drop worker_slots"
```

---

### Task 4: 接入 `linux_relay_worker`（核心迁移）

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: 更新 `RelayState`**

```cpp
#include "relay_buffer.h"

struct RelayState : TqRelayBufferBudget {
    // ... 现有字段 ...
    // 删除: TqLinuxRelayBufferPool Pool;
};
```

构造函数删除 `Pool(...)` 与 `Pool.Reserve(...)`。

在 `TqLinuxRelayRuntime::Start` / worker config 中：

```cpp
relay->MaxPendingBufferBytes = config.MaxPendingBufferBytes;
```

`TqLinuxRelayWorkerConfig`：

```cpp
// 删除: uint32_t WorkerSlots;
uint64_t MaxPendingBufferBytes{0};  // 来自 tuning.MaxPendingBufferBytesPerRelay
```

- [ ] **Step 2: 替换所有 `relay->Pool.AcquireWorker`**

统一为：

```cpp
TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
auto buffer = TqAllocateRelayBuffer(relay, Config.ReadChunkSize, &acquireFailure);
```

出现位置（`linux_relay_worker.cpp`）：
- `DrainTcpReadable` (~662)
- `BuildTcpToQuicViews` / compress path (~772, ~807)
- `DrainCompressedQuicReceiveView` (~1418)
- 其他 `Pool.AcquireWorker` grep 命中

- [ ] **Step 3: 替换背压检查**

删除 worker 里对 `relay->Pool.PendingBytes()` / `relay->Pool.MaxPendingBytes()` 的预检查。新的 hard check 统一在 `TqAllocateRelayBuffer` 的 atomic pending budget 预留中完成；worker 根据 `TqBufferAcquireFailure::PendingBytesLimit` 暂停 TCP readable 或停止当前 batch。

- [ ] **Step 4: 更新 `Snapshot()`**

删除：

```cpp
snapshot.WorkerSlotsAllocated += relay->Pool.AllocatedCount();
snapshot.WorkerSlotsFree += relay->Pool.FreeCount();
snapshot.BufferAcquireCount += relay->Pool.AcquireCount();
```

改为：

```cpp
snapshot.BufferAcquireCount += relay->AllocateCount.load(std::memory_order_relaxed);
snapshot.RelayBufferBytesInUse += relay->PendingBufferBytes.load(std::memory_order_relaxed);
snapshot.PendingBytes += relay->PendingBufferBytes.load(std::memory_order_relaxed);
// 继续加 PendingQuicReceiveBytes、PendingTcpWrites 等
```

- [ ] **Step 5: 删除 SlotLimit 分支**

`RecordBufferAcquireFailure`：移除 `TqBufferAcquireFailure::SlotLimit` case。

删除 worker 内所有 `TcpReadBufferAcquireSlotLimitFailures` 等 atomic 成员与 snapshot 字段赋值（Task 5 与 metrics 同步）。

`DrainTcpReadable` 中 failure 判断仅保留 `PendingBytesLimit` 与 `AllocationFailure`。

- [ ] **Step 6: 编译 + IO 测试**

```bash
cmake --build build --target tcpquic-proxy tcpquic_linux_relay_worker_io_test -j$(nproc)
./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

- [ ] **Step 7: Commit**

```bash
git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp
git commit -m "feat(linux): wire relay worker to on-demand buffer alloc"
```

---

### Task 5: Metrics / Admin JSON 清理

**Files:**
- Modify: `src/runtime/relay_metrics.h`, `src/runtime/relay_metrics.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`（snapshot 字段）
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: 删除 slot 相关字段**

从 `TqLinuxRelayWorkerSnapshot`、`TqRelayMetrics`、JSON 输出中移除：
- `WorkerSlotsAllocated`, `WorkerSlotsFree`
- `TcpReadBufferAcquireSlotLimitFailures` 等三个 `*SlotLimit*` 字段

- [ ] **Step 2: 新增观测字段**

在 `Snapshot()` 聚合：

```cpp
snapshot.RelayBufferBytesInUse += relay->PendingBufferBytes;
```

JSON：

```json
"linux_relay_buffer_bytes_in_use": <uint64>
```

保留 `linux_relay_*_pending_budget_failures` 与 `*_alloc_failures`。

- [ ] **Step 3: 更新 `router_runtime_test`**

断言包含 `linux_relay_buffer_bytes_in_use`；删除对 `worker_slots_allocated` 的断言。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
./build/bin/Release/tcpquic_router_runtime_test
```

- [ ] **Step 5: Commit**

```bash
git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp \
        src/tunnel/linux_relay_worker.h src/unittest/router_runtime_test.cpp
git commit -m "feat(metrics): replace worker slot stats with buffer_bytes_in_use"
```

---

### Task 6: 清理 Linux 旧测试 target + 文档

**Files:**
- Modify: `src/CMakeLists.txt`
- Delete or archive: `src/unittest/linux_relay_buffer_pool_test.cpp`
- Modify: `build.md`（mimalloc 依赖说明）
- Modify: `README.md`（若提及 worker_slots）

- [ ] **Step 1: 移除 Linux obsolete test target**

删除 `tcpquic_linux_relay_buffer_pool_test` target 定义。

保留 `tcpquic_relay_buffer_test`（Task 2）。`tcpquic_relay_buffer_pool_test` 在 Phase 1 仍可保留给 Windows 旧 pool 行为；若该 legacy 测试会阻塞当前 Linux CI，可改成 `if(WIN32)` 或标注 legacy。Phase 2 Windows 迁移完成后，再删除 `relay_buffer_pool_test.cpp` 与旧 pool target。

- [ ] **Step 2: `build.md` 增加 mimalloc 小节**

```markdown
### mimalloc（third_party，默认静态启用）

- 路径：`third_party/mimalloc`（Git 子模块，与 msquic/zstd 相同）
- 初始化：`git submodule update --init --recursive third_party/mimalloc`
- 构建：`-DTCPQUIC_USE_MIMALLOC=ON`（默认）→ 静态链入 `mimalloc-static`
- 关闭：`-DTCPQUIC_USE_MIMALLOC=OFF` → `TqAllocBytes` 回退 glibc `malloc`
- 部署：无需附带 `libmimalloc.so`
```

- [ ] **Step 3: 全量 Linux 测试**

```bash
cmake --build build --target \
  tcpquic-proxy \
  tcpquic_relay_buffer_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_tuning_test \
  tcpquic_config_router_test \
  tcpquic_router_runtime_test \
  -j$(nproc)

for t in tcpquic_relay_buffer_test tcpquic_linux_relay_worker_io_test \
         tcpquic_tuning_test tcpquic_config_router_test tcpquic_router_runtime_test; do
  ./build/bin/Release/$t || exit 1
done
```

- [ ] **Step 4: Commit**

```bash
git add src/CMakeLists.txt build.md README.md
git rm -f src/unittest/linux_relay_buffer_pool_test.cpp 2>/dev/null || true
git commit -m "docs: mimalloc build notes; drop obsolete pool unit tests"
```

---

### Task 7: 可选压测验收（P5，非阻塞 merge）

**Files:** 无代码变更

- [ ] **Step 1: DGX / 本地 perf 对比**

```bash
# 改前 tag 对比或同机 A/B
scripts/dgx-perf-profile.sh   # 若可用
```

记录：吞吐、CPU、`linux_relay_buffer_bytes_in_use`、RSS vs active relay 数。

- [ ] **Step 2: mimalloc OFF 对照**

```bash
cmake -S . -B build-glibc -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_USE_MIMALLOC=OFF
cmake --build build-glibc --target tcpquic-proxy -j$(nproc)
# 同场景对比 alloc 延迟
```

- [ ] **Step 3: 在 spec 或 PR 描述记录结论**

idle relay 场景 RSS 应显著低于 `N × 16MiB`；吞吐波动 ≤2%。

---

### Task 8: Phase 2（Windows，独立计划/PR）

**Scope:** Windows relay 也切到 `relay_buffer` / `relay_alloc` / mimalloc，删除 `TqRelayBufferPool` 最后依赖。

- [ ] `windows_relay_worker.cpp/.h` 迁移 `TcpRecvBuffers` / `TcpSendBuffers` 到 `TqAllocateRelayBuffer`。
- [ ] `windows_relay_worker_test.cpp` 更新 slot/pool 断言为 pending bytes / allocation count 断言。
- [ ] `tcpquic_windows_relay_worker_test` 链接 `mimalloc-static` 并编译 `relay_alloc.cpp` / `relay_buffer.cpp`。
- [ ] 删除 `relay_buffer_pool.h/.cpp`、`relay_buffer_pool_test.cpp`，或仅保留临时兼容层直到 Windows 迁移完成。

---

## Spec Coverage Checklist

| Spec 要求 | Task |
|-----------|------|
| 无 Reserve / 无 worker_slots | Task 3, 4 |
| `MaxPendingBufferBytesPerRelay` | Task 3, 4 |
| mimalloc 默认 ON + **third_party 静态链入** | Task 1 |
| `TqAllocBytes` 封装 | Task 1, 2 |
| 背压 PendingBytesLimit 保留 | Task 2, 4 |
| 删除 SlotLimit | Task 4, 5 |
| QUIC receive 背压不变 | 无代码改（仅验证 IO test） |
| Metrics `buffer_bytes_in_use` | Task 5 |
| Windows Phase 1 暂不迁移、Phase 2 统一到 mimalloc | Task 8 |
| 单元 + IO 测试 | Task 2, 4, 6 |

---

## 风险回滚

若 mimalloc 子模块缺失：运行 `git submodule update --init --recursive third_party/mimalloc`。

若需回滚分配器：`-DTCPQUIC_USE_MIMALLOC=OFF` 仍可运行（glibc malloc）。

若热路径 alloc 回归：Phase 1 可先 git revert Linux 单 PR；Windows 在 Phase 2 前仍用旧 pool，不受影响。

---

## 执行选项

Plan complete and saved to `docs/superpowers/plans/2026-06-16-relay-on-demand-buffer-mimalloc-linux.md`.

**1. Subagent-Driven（推荐）** — 每个 Task 派生子 agent，Task 间人工/自动 review。

**2. Inline Execution** — 本会话按 Task 1→7 顺序实现，每 Task 后跑测试；Task 8 另起 Windows 独立计划/PR。

请告知选择哪种执行方式；若选 Inline，从 Task 1 开始。
