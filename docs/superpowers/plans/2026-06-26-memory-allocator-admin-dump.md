# Memory Allocator Admin Dump Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 增加 admin API `POST /api/v1/memory/allocator:dump`，按需采样 mimalloc 结构化指标并输出一条日志。

**Architecture:** 新增 `runtime/memory_stats` 负责 mimalloc stats 采样与格式化，新增 `runtime/admin_memory` 负责公共 admin route。`admin_http` 只增加 v1 白名单，client router 和 server admin handler 在现有业务路由前复用同一个公共 handler。

**Tech Stack:** C++17, mimalloc `mi_stats_get()`, cpp-httplib admin server, existing `TqJsonResponse`, CMake, repo-local unittest executables.

---

## 文件结构

- Create: `src/runtime/memory_stats.h`：定义 memory allocator snapshot、采样、JSON 格式化、日志行格式化、dump helper。
- Create: `src/runtime/memory_stats.cpp`：实现 `mi_stats_get()` 条件采样和 stderr 日志输出。
- Create: `src/runtime/admin_memory.h`：声明公共 admin memory handler。
- Create: `src/runtime/admin_memory.cpp`：实现 `POST /memory/allocator:dump`。
- Modify: `src/runtime/admin_http.cpp`：放行 `/api/v1/memory/allocator:dump`。
- Modify: `src/runtime/router_runtime.cpp`：在 `TqRouterRuntime::HandleAdmin()` 开头接入公共 handler。
- Modify: `src/runtime/server_admin.cpp`：在 `TqHandleServerAdmin()` 开头接入公共 handler。
- Modify: `src/CMakeLists.txt`：生产目标与相关测试目标加入新源文件，并对新 memory stats 测试调用 `tcpquic_link_mimalloc()`。
- Create: `src/unittest/memory_stats_test.cpp`：覆盖格式化和采样状态。
- Modify: `src/unittest/admin_http_test.cpp`：覆盖 v1 白名单。
- Modify: `src/unittest/server_admin_test.cpp`：覆盖 server admin memory dump。
- Modify: `src/unittest/router_runtime_test.cpp`：覆盖 router admin memory dump。

## Task 1: memory_stats 格式化与采样模块

**Files:**
- Create: `src/runtime/memory_stats.h`
- Create: `src/runtime/memory_stats.cpp`
- Create: `src/unittest/memory_stats_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: 写 failing test**

Create `src/unittest/memory_stats_test.cpp`:

```cpp
#include "memory_stats.h"

#include <string>

int main() {
    TqMemoryAllocatorStats stats{};
    stats.MimallocEnabled = true;
    stats.Available = true;
    stats.RequestedCurrentBytes = 123;
    stats.RequestedTotalBytes = 1000;
    stats.RequestedFreedBytes = 877;
    stats.RequestedPeakBytes = 456;
    stats.ReservedCurrentBytes = 2048;
    stats.CommittedCurrentBytes = 1024;
    stats.PageCommittedCurrentBytes = 512;
    stats.NormalAllocCount = 10;
    stats.HugeAllocCount = 1;
    stats.MmapCalls = 2;
    stats.CommitCalls = 3;
    stats.PurgeCalls = 4;
    stats.ThreadsCurrent = 5;

    const std::string line = TqFormatMemoryAllocatorStatsLine(stats);
    if (line.find("allocator=mimalloc") == std::string::npos) return 1;
    if (line.find("enabled=1") == std::string::npos) return 2;
    if (line.find("available=1") == std::string::npos) return 3;
    if (line.find("requested_current_bytes=123") == std::string::npos) return 4;
    if (line.find("requested_total_bytes=1000") == std::string::npos) return 5;
    if (line.find("requested_freed_bytes=877") == std::string::npos) return 6;
    if (line.find("requested_peak_bytes=456") == std::string::npos) return 7;
    if (line.find("reserved_current_bytes=2048") == std::string::npos) return 8;
    if (line.find("committed_current_bytes=1024") == std::string::npos) return 9;
    if (line.find("page_committed_current_bytes=512") == std::string::npos) return 10;
    if (line.find("normal_alloc_count=10") == std::string::npos) return 11;
    if (line.find("huge_alloc_count=1") == std::string::npos) return 12;
    if (line.find("mmap_calls=2") == std::string::npos) return 13;
    if (line.find("commit_calls=3") == std::string::npos) return 14;
    if (line.find("purge_calls=4") == std::string::npos) return 15;
    if (line.find("threads_current=5") == std::string::npos) return 16;

    const std::string json = TqMemoryAllocatorStatsJson(stats);
    if (json.find("\"status\":\"dumped\"") == std::string::npos) return 17;
    if (json.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 18;
    if (json.find("\"enabled\":true") == std::string::npos) return 19;
    if (json.find("\"available\":true") == std::string::npos) return 20;
    if (json.find("\"requested_current_bytes\":123") == std::string::npos) return 21;

    const TqMemoryAllocatorStats snapshot = TqSnapshotMemoryAllocatorStats();
#if TCPQUIC_USE_MIMALLOC
    if (!snapshot.MimallocEnabled) return 22;
#else
    if (snapshot.MimallocEnabled) return 23;
    if (snapshot.Available) return 24;
#endif

    return 0;
}
```

Modify `src/CMakeLists.txt` near the other small runtime tests:

```cmake
add_executable(tcpquic_memory_stats_test
    unittest/memory_stats_test.cpp
    runtime/memory_stats.cpp
)
tcpquic_target_include_dirs(tcpquic_memory_stats_test)
tcpquic_link_mimalloc(tcpquic_memory_stats_test)
set_property(TARGET tcpquic_memory_stats_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_memory_stats_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Add `tcpquic_memory_stats_test` to the aggregate test dependency list where other unittest targets are listed.

- [ ] **Step 2: 运行 test 确认失败**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_memory_stats_test -j
```

Expected: build fails because `memory_stats.h` and `runtime/memory_stats.cpp` do not exist.

- [ ] **Step 3: 实现 header**

Create `src/runtime/memory_stats.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

struct TqMemoryAllocatorStats {
    bool MimallocEnabled{false};
    bool Available{false};
    uint64_t RequestedCurrentBytes{0};
    uint64_t RequestedTotalBytes{0};
    uint64_t RequestedFreedBytes{0};
    uint64_t RequestedPeakBytes{0};
    uint64_t ReservedCurrentBytes{0};
    uint64_t CommittedCurrentBytes{0};
    uint64_t PageCommittedCurrentBytes{0};
    uint64_t NormalAllocCount{0};
    uint64_t HugeAllocCount{0};
    uint64_t MmapCalls{0};
    uint64_t CommitCalls{0};
    uint64_t PurgeCalls{0};
    uint64_t ThreadsCurrent{0};
};

TqMemoryAllocatorStats TqSnapshotMemoryAllocatorStats();
std::string TqFormatMemoryAllocatorStatsLine(const TqMemoryAllocatorStats& stats);
std::string TqMemoryAllocatorStatsJson(const TqMemoryAllocatorStats& stats);
void TqDumpMemoryAllocatorStatsToLog(const TqMemoryAllocatorStats& stats);
TqMemoryAllocatorStats TqDumpMemoryAllocatorStatsToLog();
```

- [ ] **Step 4: 实现 source**

Create `src/runtime/memory_stats.cpp`:

```cpp
#include "memory_stats.h"

#include <cstdio>
#include <sstream>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc-stats.h>
#endif

namespace {

uint64_t TqNonNegativeInt64(int64_t value) {
    return value <= 0 ? 0 : static_cast<uint64_t>(value);
}

const char* TqBoolJson(bool value) {
    return value ? "true" : "false";
}

} // namespace

TqMemoryAllocatorStats TqSnapshotMemoryAllocatorStats() {
    TqMemoryAllocatorStats out{};
#if TCPQUIC_USE_MIMALLOC
    out.MimallocEnabled = true;
    mi_stats_t stats;
    mi_stats_init(&stats);
    if (!mi_stats_get(&stats)) {
        out.Available = false;
        return out;
    }

    out.Available = true;
    out.RequestedCurrentBytes = TqNonNegativeInt64(stats.malloc_requested.current);
    out.RequestedTotalBytes = TqNonNegativeInt64(stats.malloc_requested.total);
    out.RequestedPeakBytes = TqNonNegativeInt64(stats.malloc_requested.peak);
    out.RequestedFreedBytes =
        out.RequestedTotalBytes >= out.RequestedCurrentBytes
            ? out.RequestedTotalBytes - out.RequestedCurrentBytes
            : 0;
    out.ReservedCurrentBytes = TqNonNegativeInt64(stats.reserved.current);
    out.CommittedCurrentBytes = TqNonNegativeInt64(stats.committed.current);
    out.PageCommittedCurrentBytes = TqNonNegativeInt64(stats.page_committed.current);
    out.NormalAllocCount = TqNonNegativeInt64(stats.malloc_normal_count.total);
    out.HugeAllocCount = TqNonNegativeInt64(stats.malloc_huge_count.total);
    out.MmapCalls = TqNonNegativeInt64(stats.mmap_calls.total);
    out.CommitCalls = TqNonNegativeInt64(stats.commit_calls.total);
    out.PurgeCalls = TqNonNegativeInt64(stats.purge_calls.total);
    out.ThreadsCurrent = TqNonNegativeInt64(stats.threads.current);
#endif
    return out;
}

std::string TqFormatMemoryAllocatorStatsLine(const TqMemoryAllocatorStats& stats) {
    char buffer[1536];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "allocator=mimalloc enabled=%d available=%d "
        "requested_current_bytes=%llu requested_total_bytes=%llu "
        "requested_freed_bytes=%llu requested_peak_bytes=%llu "
        "reserved_current_bytes=%llu committed_current_bytes=%llu "
        "page_committed_current_bytes=%llu normal_alloc_count=%llu "
        "huge_alloc_count=%llu mmap_calls=%llu commit_calls=%llu "
        "purge_calls=%llu threads_current=%llu",
        stats.MimallocEnabled ? 1 : 0,
        stats.Available ? 1 : 0,
        static_cast<unsigned long long>(stats.RequestedCurrentBytes),
        static_cast<unsigned long long>(stats.RequestedTotalBytes),
        static_cast<unsigned long long>(stats.RequestedFreedBytes),
        static_cast<unsigned long long>(stats.RequestedPeakBytes),
        static_cast<unsigned long long>(stats.ReservedCurrentBytes),
        static_cast<unsigned long long>(stats.CommittedCurrentBytes),
        static_cast<unsigned long long>(stats.PageCommittedCurrentBytes),
        static_cast<unsigned long long>(stats.NormalAllocCount),
        static_cast<unsigned long long>(stats.HugeAllocCount),
        static_cast<unsigned long long>(stats.MmapCalls),
        static_cast<unsigned long long>(stats.CommitCalls),
        static_cast<unsigned long long>(stats.PurgeCalls),
        static_cast<unsigned long long>(stats.ThreadsCurrent));
    return buffer;
}

std::string TqMemoryAllocatorStatsJson(const TqMemoryAllocatorStats& stats) {
    std::ostringstream out;
    out << "{\"status\":\"dumped\""
        << ",\"allocator\":\"mimalloc\""
        << ",\"enabled\":" << TqBoolJson(stats.MimallocEnabled)
        << ",\"available\":" << TqBoolJson(stats.Available)
        << ",\"requested_current_bytes\":" << stats.RequestedCurrentBytes
        << ",\"requested_total_bytes\":" << stats.RequestedTotalBytes
        << ",\"requested_freed_bytes\":" << stats.RequestedFreedBytes
        << ",\"requested_peak_bytes\":" << stats.RequestedPeakBytes
        << ",\"reserved_current_bytes\":" << stats.ReservedCurrentBytes
        << ",\"committed_current_bytes\":" << stats.CommittedCurrentBytes
        << ",\"page_committed_current_bytes\":" << stats.PageCommittedCurrentBytes
        << ",\"normal_alloc_count\":" << stats.NormalAllocCount
        << ",\"huge_alloc_count\":" << stats.HugeAllocCount
        << ",\"mmap_calls\":" << stats.MmapCalls
        << ",\"commit_calls\":" << stats.CommitCalls
        << ",\"purge_calls\":" << stats.PurgeCalls
        << ",\"threads_current\":" << stats.ThreadsCurrent
        << '}';
    return out.str();
}

void TqDumpMemoryAllocatorStatsToLog(const TqMemoryAllocatorStats& stats) {
    std::fprintf(
        stderr,
        "tcpquic-proxy memory_allocator_stats: %s\n",
        TqFormatMemoryAllocatorStatsLine(stats).c_str());
    std::fflush(stderr);
}

TqMemoryAllocatorStats TqDumpMemoryAllocatorStatsToLog() {
    const TqMemoryAllocatorStats stats = TqSnapshotMemoryAllocatorStats();
    TqDumpMemoryAllocatorStatsToLog(stats);
    return stats;
}
```

- [ ] **Step 5: 运行 test 确认通过**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_memory_stats_test -j
rtk ./build-regression/bin/Release/tcpquic_memory_stats_test
```

Expected: build succeeds and executable exits with status 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/memory_stats.h src/runtime/memory_stats.cpp src/unittest/memory_stats_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add memory allocator stats snapshot"
```

## Task 2: 公共 admin memory handler

**Files:**
- Create: `src/runtime/admin_memory.h`
- Create: `src/runtime/admin_memory.cpp`
- Modify: `src/unittest/server_admin_test.cpp`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: 写 server admin failing test**

Modify `src/unittest/server_admin_test.cpp` to add a request after the existing server metrics request:

```cpp
    std::string memory = TqHandleServerAdmin(Request("POST", "/memory/allocator:dump"), metrics, 10);
    if (memory.find("HTTP/1.1 200") == std::string::npos) return 120;
    if (memory.find("\"status\":\"dumped\"") == std::string::npos) return 121;
    if (memory.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 122;
```

Modify the `tcpquic_server_admin_test` target:

```cmake
add_executable(tcpquic_server_admin_test
    unittest/server_admin_test.cpp
    runtime/server_admin.cpp
    runtime/admin_memory.cpp
    runtime/memory_stats.cpp
)
tcpquic_target_include_dirs(tcpquic_server_admin_test)
target_link_libraries(tcpquic_server_admin_test PRIVATE Threads::Threads inc warnings msquic)
tcpquic_link_mimalloc(tcpquic_server_admin_test)
```

- [ ] **Step 2: 运行 test 确认失败**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_server_admin_test -j
rtk ./build-regression/bin/Release/tcpquic_server_admin_test
```

Expected: test fails because `TqHandleServerAdmin()` still returns 404 for `/memory/allocator:dump`.

- [ ] **Step 3: 实现 admin_memory header**

Create `src/runtime/admin_memory.h`:

```cpp
#pragma once

#include "admin_http.h"

#include <string>

bool TqHandleMemoryAdmin(const TqHttpRequest& req, std::string& response);
```

- [ ] **Step 4: 实现 admin_memory source**

Create `src/runtime/admin_memory.cpp`:

```cpp
#include "admin_memory.h"

#include "memory_stats.h"

bool TqHandleMemoryAdmin(const TqHttpRequest& req, std::string& response) {
    if (req.Method != "POST" || req.Path != "/memory/allocator:dump") {
        return false;
    }

    const TqMemoryAllocatorStats stats = TqDumpMemoryAllocatorStatsToLog();
    response = TqJsonResponse(200, TqMemoryAllocatorStatsJson(stats));
    return true;
}
```

- [ ] **Step 5: 接入 server admin**

Modify `src/runtime/server_admin.cpp`:

```cpp
#include "server_admin.h"

#include "admin_memory.h"
#include "quic_session.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"
```

At the top of `TqHandleServerAdmin()`:

```cpp
std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds) {
    std::string commonResponse;
    if (TqHandleMemoryAdmin(req, commonResponse)) {
        return commonResponse;
    }

    if (req.Method == "GET" && (req.Path == "/server" || req.Path == "/server/metrics")) {
```

- [ ] **Step 6: 运行 test 确认通过**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_server_admin_test -j
rtk ./build-regression/bin/Release/tcpquic_server_admin_test
```

Expected: build succeeds and executable exits with status 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/runtime/admin_memory.h src/runtime/admin_memory.cpp src/runtime/server_admin.cpp src/unittest/server_admin_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add admin memory dump handler"
```

## Task 3: admin HTTP v1 白名单

**Files:**
- Modify: `src/runtime/admin_http.cpp`
- Modify: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: 写 failing test**

In `src/unittest/admin_http_test.cpp`, add a routed request test near the other `/api/v1` route checks:

```cpp
        const std::string memoryRequest = "POST /api/v1/memory/allocator:dump HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            token + "\r\nContent-Length: 0\r\n\r\n";
        int memoryFd = TqConnect(admin.ListenAddress());
        if (!TqSocketValid(memoryFd)) return 110;
        if (!TqSendAll(memoryFd, memoryRequest)) return 111;
        const std::string memoryResponse = TqRecvSome(memoryFd);
        TqCloseSocket(memoryFd);
        if (memoryResponse.find("HTTP/1.1 200") == std::string::npos) return 112;
        if (memoryResponse.find("\"path\":\"/memory/allocator:dump\"") == std::string::npos) return 113;
```

Use the same test server handler style already present in that file: return a JSON body that echoes `req.Path`.

- [ ] **Step 2: 运行 test 确认失败**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_admin_http_test -j
rtk ./build-regression/bin/Release/tcpquic_admin_http_test
```

Expected: request returns 404 because `/api/v1/memory/allocator:dump` is not in `TqIsV1AdminPath()`.

- [ ] **Step 3: 修改白名单**

Modify `src/runtime/admin_http.cpp` inside `TqIsV1AdminPath()`:

```cpp
        path == "/api/v1/relay/workers" ||
        path.compare(0, 22, "/api/v1/relay/workers/") == 0 ||
        path == "/api/v1/memory/allocator:dump" ||
        path == "/api/v1/server" ||
```

- [ ] **Step 4: 运行 test 确认通过**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_admin_http_test -j
rtk ./build-regression/bin/Release/tcpquic_admin_http_test
```

Expected: build succeeds and executable exits with status 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_http.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "feat: allow admin memory dump route"
```

## Task 4: router runtime 接入

**Files:**
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: 写 failing test**

In `src/unittest/router_runtime_test.cpp`, add an admin request assertion after the existing health or metrics admin checks:

```cpp
    std::string memory = runtime.HandleAdmin(TqHttpRequest{"POST", "/memory/allocator:dump", "", {}});
    if (memory.find("HTTP/1.1 200") == std::string::npos) return 210;
    if (memory.find("\"status\":\"dumped\"") == std::string::npos) return 211;
    if (memory.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 212;
```

Modify `TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES` to include:

```cmake
    runtime/admin_memory.cpp
    runtime/memory_stats.cpp
```

Keep the existing `tcpquic_link_mimalloc(tcpquic_router_runtime_test)` calls.

- [ ] **Step 2: 运行 test 确认失败**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_router_runtime_test -j
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: test fails because router admin still returns 404 for `/memory/allocator:dump`.

- [ ] **Step 3: 接入 router admin**

Modify includes in `src/runtime/router_runtime.cpp`:

```cpp
#include "admin_memory.h"
```

At the top of `TqRouterRuntime::HandleAdmin()`:

```cpp
std::string TqRouterRuntime::HandleAdmin(const TqHttpRequest& req) {
    std::string commonResponse;
    if (TqHandleMemoryAdmin(req, commonResponse)) {
        return commonResponse;
    }

    if (req.Method == "GET" && req.Path == "/health") {
```

- [ ] **Step 4: 运行 test 确认通过**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_router_runtime_test -j
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: build succeeds and executable exits with status 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/router_runtime.cpp src/unittest/router_runtime_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: expose memory dump in router admin"
```

## Task 5: 生产目标链接与端到端验证

**Files:**
- Modify: `src/CMakeLists.txt`
- Test: production binary `tcpquic-proxy` target

- [ ] **Step 1: 修改生产源文件列表**

Modify `TCPQUIC_PROXY_SOURCES` in `src/CMakeLists.txt`:

```cmake
    runtime/admin_auth.cpp
    runtime/admin_http.cpp
    runtime/admin_memory.cpp
    runtime/client_peer_runtime.cpp
    runtime/memory_stats.cpp
    runtime/router_runtime.cpp
```

The existing `tcpquic_link_mimalloc(tcpquic-proxy)` remains unchanged.

- [ ] **Step 2: 构建生产目标**

Run:

```bash
rtk cmake --build build-regression --target tcpquic-proxy -j
```

Expected: build succeeds.

- [ ] **Step 3: 运行相关 unittest 目标**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_memory_stats_test tcpquic_admin_http_test tcpquic_server_admin_test tcpquic_router_runtime_test -j
rtk ./build-regression/bin/Release/tcpquic_memory_stats_test
rtk ./build-regression/bin/Release/tcpquic_admin_http_test
rtk ./build-regression/bin/Release/tcpquic_server_admin_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: all four executables exit with status 0.

- [ ] **Step 4: 验证 mimalloc OFF configure/build**

Run:

```bash
rtk cmake -S . -B build-memory-stats-glibc -DTCPQUIC_USE_MIMALLOC=OFF
rtk cmake --build build-memory-stats-glibc --target tcpquic_memory_stats_test tcpquic_server_admin_test -j
rtk ./build-memory-stats-glibc/bin/Release/tcpquic_memory_stats_test
rtk ./build-memory-stats-glibc/bin/Release/tcpquic_server_admin_test
```

Expected:

- configure succeeds without requiring mimalloc stats symbols.
- `tcpquic_memory_stats_test` exits with status 0.
- `tcpquic_server_admin_test` exits with status 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/CMakeLists.txt
rtk git commit -m "build: link admin memory dump sources"
```

## Self-Review

- Spec coverage: plan covers structured `mi_stats_get()` sampling, admin-triggered log output, JSON response, no trace/diag integration, mimalloc disabled behavior, and client/server admin access.
- Placeholder scan: no unresolved placeholder markers are present.
- Type consistency: `TqMemoryAllocatorStats`, `TqSnapshotMemoryAllocatorStats`, `TqFormatMemoryAllocatorStatsLine`, `TqMemoryAllocatorStatsJson`, `TqDumpMemoryAllocatorStatsToLog`, and `TqHandleMemoryAdmin` signatures are consistent across tasks.
