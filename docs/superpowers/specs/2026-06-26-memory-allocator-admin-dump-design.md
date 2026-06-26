# Admin 触发 mimalloc 指标采样设计

日期：2026-06-26

## 背景

`docs/memory-allocator-design.md` 已明确项目默认构建启用 mimalloc，但不启用全局 malloc/new override。当前默认 release 构建中，项目显式 allocator、zstd、c-ares 以及 `TCPQUIC_MSQUIC_USE_MIMALLOC=AUTO` 下的 MsQuic 平台 allocator 都会进入 mimalloc；ASan 或显式 `-DTCPQUIC_USE_MIMALLOC=OFF` 构建会关闭该路径。

本设计补充一个按需诊断能力：通过 admin API 主动采集 mimalloc 结构化统计并触发日志输出，用于线上或压测过程中判断内存占用是否持续增长。该能力不接入 `--trace` 或 `--diag-stats` 周期输出，避免增加常规诊断噪声。

## 目标

- 使用 mimalloc 官方结构化接口 `mi_stats_get()` 采样，不解析 `mi_stats_print_out()` 文本。
- 在 admin API 中增加一个手动触发接口，调用一次接口只采样并输出一次日志。
- HTTP 响应返回同一份采样摘要，便于调用方确认触发成功。
- 默认 mimalloc 构建返回真实 mimalloc 指标；mimalloc 关闭构建返回 disabled 状态。
- 不改变现有 `--trace`、`--diag-stats`、relay metrics 的周期输出。

## 非目标

- 不新增后台采样线程。
- 不新增运行时命令行开关。
- 不把内存指标周期性写入 trace 文件。
- 不统计仍走 libc/OpenSSL/spdlog/STL 默认 allocator 的内存。
- 不在本阶段做按模块、按调用点、按 allocation tag 的细分统计。

## API 设计

新增接口：

```http
POST /api/v1/memory/allocator:dump
```

admin HTTP 层当前会把 `/api/v1/...` 转换成 handler 内部 legacy path，因此业务 handler 匹配：

```text
POST /memory/allocator:dump
```

响应状态：

- `200 OK`：采样逻辑执行完成。即使 mimalloc 在当前构建中关闭，也返回 `200`，并用 `enabled=false` 明确说明。
- `404 Not Found`：路径或方法不匹配时沿用现有 admin 行为。
- `401 Unauthorized`：未通过现有 admin token 校验时沿用现有 admin 行为。

响应 JSON：

```json
{
  "status": "dumped",
  "allocator": "mimalloc",
  "enabled": true,
  "available": true,
  "requested_current_bytes": 123456,
  "requested_total_bytes": 987654321,
  "requested_freed_bytes": 987530865,
  "requested_peak_bytes": 234567,
  "reserved_current_bytes": 1048576,
  "committed_current_bytes": 524288,
  "page_committed_current_bytes": 393216,
  "normal_alloc_count": 1000,
  "huge_alloc_count": 0,
  "mmap_calls": 4,
  "commit_calls": 8,
  "purge_calls": 2,
  "threads_current": 7
}
```

当 mimalloc 在当前构建中关闭：

```json
{
  "status": "dumped",
  "allocator": "mimalloc",
  "enabled": false,
  "available": false,
  "requested_current_bytes": 0,
  "requested_total_bytes": 0,
  "requested_freed_bytes": 0,
  "requested_peak_bytes": 0,
  "reserved_current_bytes": 0,
  "committed_current_bytes": 0,
  "page_committed_current_bytes": 0,
  "normal_alloc_count": 0,
  "huge_alloc_count": 0,
  "mmap_calls": 0,
  "commit_calls": 0,
  "purge_calls": 0,
  "threads_current": 0
}
```

## 日志设计

接口被调用时向 stderr 输出一条结构化日志：

```text
tcpquic-proxy memory_allocator_stats: allocator=mimalloc enabled=1 available=1 requested_current_bytes=123456 requested_total_bytes=987654321 requested_freed_bytes=987530865 requested_peak_bytes=234567 reserved_current_bytes=1048576 committed_current_bytes=524288 page_committed_current_bytes=393216 normal_alloc_count=1000 huge_alloc_count=0 mmap_calls=4 commit_calls=8 purge_calls=2 threads_current=7
```

日志选择 stderr，而不是 trace logger，原因：

- admin API 本身不依赖 `--trace` 启用。
- 该接口用于主动诊断，调用方可以在控制进程或服务日志中直接定位触发时刻。
- 避免让 trace 文件成为该能力的前置条件。

## 指标语义

采样使用 `mi_stats_get()` 汇总当前 subprocess 及其 heaps。字段映射：

| 输出字段 | mimalloc 字段 | 说明 |
|---|---|---|
| `requested_current_bytes` | `malloc_requested.current` | 当前仍被 mimalloc 视为已请求的字节数 |
| `requested_total_bytes` | `malloc_requested.total` | 进程生命周期内累计请求字节数 |
| `requested_freed_bytes` | `total - current` | 推导值，表示累计已释放或不再处于 current 的请求字节数 |
| `requested_peak_bytes` | `malloc_requested.peak` | 请求字节数峰值 |
| `reserved_current_bytes` | `reserved.current` | mimalloc 当前保留的虚拟内存字节数 |
| `committed_current_bytes` | `committed.current` | mimalloc 当前提交内存字节数 |
| `page_committed_current_bytes` | `page_committed.current` | mimalloc page 内部提交字节数 |
| `normal_alloc_count` | `malloc_normal_count.total` | 普通大小分配累计次数 |
| `huge_alloc_count` | `malloc_huge_count.total` | huge allocation 累计次数 |
| `mmap_calls` | `mmap_calls.total` | OS mmap/VirtualAlloc 类调用累计次数 |
| `commit_calls` | `commit_calls.total` | OS commit 调用累计次数 |
| `purge_calls` | `purge_calls.total` | purge 调用累计次数 |
| `threads_current` | `threads.current` | mimalloc 当前观察到的线程数 |

`requested_freed_bytes` 不是 free 调用次数，也不是业务释放总量。它只用于趋势判断：如果负载结束后 `requested_current_bytes` 长时间不回落，而 relay、tunnel、connection 活跃数已经回落，则需要进一步排查泄漏或长期持有对象。

## 代码结构

新增公共内存指标模块：

- `src/runtime/memory_stats.h`
- `src/runtime/memory_stats.cpp`

职责：

- 封装 `TCPQUIC_USE_MIMALLOC` 条件编译。
- 在 mimalloc 构建中 include `<mimalloc-stats.h>` 并调用 `mi_stats_get()`。
- 在非 mimalloc 构建中返回 disabled snapshot。
- 提供 JSON 格式化、日志行格式化、dump helper。

新增公共 admin handler：

- `src/runtime/admin_memory.h`
- `src/runtime/admin_memory.cpp`

职责：

- 识别 `POST /memory/allocator:dump`。
- 调用内存指标模块进行采样和日志输出。
- 返回 `TqJsonResponse(200, ...)`。
- 对不匹配请求返回 `false`，让 client/server 原有 handler 继续处理。

接入点：

- `src/runtime/admin_http.cpp`：将 `/api/v1/memory/allocator:dump` 加入 `TqIsV1AdminPath()` 白名单。
- `src/runtime/router_runtime.cpp`：`TqRouterRuntime::HandleAdmin()` 开头调用公共 handler。
- `src/runtime/server_admin.cpp`：`TqHandleServerAdmin()` 开头调用公共 handler。
- `src/CMakeLists.txt`：生产目标和相关测试目标加入新源文件，调用 `tcpquic_link_mimalloc()` 的目标保持一致。

## 错误处理

- `mi_stats_get()` 返回 false 时，输出 `enabled=true available=false`，数值字段为 0，HTTP 仍返回 `200`。这表示接口可用但当前采样失败。
- 非 mimalloc 构建输出 `enabled=false available=false`。
- 采样不抛异常，不分配大块临时内存，不调用 `mi_stats_get_json()`。
- admin API 方法不为 POST 时返回现有 404 行为，避免新增方法语义。

## 测试策略

- `memory_stats` 单元测试覆盖 JSON 和日志行格式化字段。
- `memory_stats` 单元测试覆盖当前构建下 `TqSnapshotMemoryAllocatorStats()` 的 enabled/available 语义。
- `admin_http` 单元测试覆盖 `/api/v1/memory/allocator:dump` 能通过白名单进入 handler。
- `router_runtime` 单元测试覆盖 client/admin handler 返回 memory JSON。
- `server_admin` 单元测试覆盖 server/admin handler 返回 memory JSON。
- CMake 测试目标需要链接 `memory_stats.cpp` 并调用 `tcpquic_link_mimalloc()`，保证 mimalloc ON/OFF 构建都可编译。

## 验收标准

- 默认构建中调用 `POST /api/v1/memory/allocator:dump` 返回 `allocator=mimalloc`、`enabled=true`，并输出一条 `tcpquic-proxy memory_allocator_stats:` 日志。
- ASan 或 `-DTCPQUIC_USE_MIMALLOC=OFF` 构建中同一接口返回 `enabled=false`、`available=false`，不因缺少 mimalloc 符号而链接失败。
- 现有 `/api/v1/health`、`/api/v1/metrics`、server admin API 行为不变。
- `--trace` 和 `--diag-stats` 输出格式不变。
