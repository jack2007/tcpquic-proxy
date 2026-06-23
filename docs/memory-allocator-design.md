# 显式 mimalloc 内存管理设计

日期：2026-06-23

## 目标

将 tcpquic-proxy 的 mimalloc 使用方式从“依赖全局 malloc/new override”收敛为“项目显式 allocator API + 第三方库官方 allocator hook + vendored 依赖补丁”。

设计目标：

- `TCPQUIC_USE_MIMALLOC=ON` 时，项目高频、大块、可控分配路径使用 mimalloc。
- 不启用 mimalloc 的 libc malloc/free 或 C++ new/delete 全局替换。
- 不让 libc 分配的内存进入 mimalloc free，也不让 mimalloc 分配的内存进入 libc free。
- ASan 构建继续禁用 mimalloc，优先保持 sanitizer 诊断可靠性。
- 第三方库只通过官方 hook 或 vendored patch 接入，不在应用层强行释放第三方内部指针。

## 当前基线

当前代码已有以下基础：

| 范围 | 当前状态 |
|---|---|
| 顶层 CMake | `TCPQUIC_USE_MIMALLOC` 默认 ON；ASan 检测到时强制 OFF |
| mimalloc 构建 | 已设置 `MI_OVERRIDE OFF` / `MI_OSX_INTERPOSE OFF` / `MI_OSX_ZONE OFF`，避免静态链接时触发全局 override |
| 目标链接 | `tcpquic_link_mimalloc()` 为主程序和相关测试链接 `mimalloc-static` 并定义 `TCPQUIC_USE_MIMALLOC` |
| 项目 allocator | `src/tunnel/relay_alloc.{h,cpp}` 提供 `TqAllocBytes()` / `TqFreeBytes()` |
| relay buffer | `src/tunnel/relay_buffer.cpp` 已通过 `TqAllocBytes()` / `TqFreeBytes()` 管理 payload storage |
| zstd | `src/protocol/compress.cpp` 已通过 `ZSTD_customMem` 接入项目 allocator |
| c-ares | `src/tunnel/ares_dns_resolver.cpp` 已通过 `ares_library_init_mem()` 接入项目 allocator |
| MsQuic | 无公共 allocator hook；vendored 平台层 patch 由 `TCPQUIC_MSQUIC_USE_MIMALLOC=AUTO|ON|OFF` 控制 |
| quictls/OpenSSL | 本轮评估后不接入 `CRYPTO_set_mem_functions()` |

macOS 相关 mimalloc override 已显式关闭，避免静态链接时触发 malloc zone 或 interpose 级别的全局替换。

## 总体架构

allocator 接入分为四层。

| 层级 | 责任 | 接入方式 |
|---|---|---|
| 项目显式 allocator | tcpquic-proxy 自有内存分配入口 | `TqMalloc()` / `TqCalloc()` / `TqRealloc()` / `TqFree()` |
| 项目按字节 buffer allocator | relay buffer 等已知 size 的存储 | 继续支持 `TqAllocBytes()` / `TqFreeBytes(ptr, bytes)`，内部转调通用 API |
| 支持 hook 的第三方库 | zstd、c-ares | 使用官方 allocator hook |
| 不支持公共 hook 的 vendored 依赖 | MsQuic | 维护 MsQuic 平台层 vendored patch，并用构建开关隔离 |
| 本轮不接入的 TLS 依赖 | quictls/OpenSSL | 仅记录 Phase 5 no-hook 结论和未来前置条件 |

不纳入本阶段：

- 全局替换 `new/delete`。
- 深改 spdlog、fmt、STL 容器 allocator。
- 删除或迁移所有 libc 分配路径。普通对象、日志、小型 STL 容器仍可继续使用默认 allocator。

构建边界：

- 任何新增调用 `TqMalloc()` / `TqFree()` 的 translation unit，所在 target 都必须同时编译 `src/tunnel/relay_alloc.cpp`。
- 任何编译 `src/tunnel/relay_alloc.cpp` 的 target 都必须调用 `tcpquic_link_mimalloc(target)`，由该 helper 负责定义 `TCPQUIC_USE_MIMALLOC=1/0` 并在 ON 时链接 `mimalloc-static`。
- 这条规则尤其影响 `protocol/compress.cpp` 和 `tunnel/ares_dns_resolver.cpp` 的测试目标；否则会出现缺少 `Tq*` 符号或不同 target 编译定义不一致的问题。

## 项目 allocator API

`src/tunnel/relay_alloc.{h,cpp}` 扩展为项目内 allocator 封装。建议保留文件名，避免扩大改动面。

目标接口：

```cpp
void* TqMalloc(size_t bytes);
void* TqCalloc(size_t count, size_t bytes);
void* TqRealloc(void* ptr, size_t bytes);
void TqFree(void* ptr);

void* TqAllocBytes(size_t bytes);
void TqFreeBytes(void* ptr, size_t bytes);
```

行为：

- `TCPQUIC_USE_MIMALLOC=1` 时转调 `mi_malloc()`、`mi_calloc()`、`mi_realloc()`、`mi_free()`。
- `TCPQUIC_USE_MIMALLOC=0` 时转调 `std::malloc()`、`std::calloc()`、`std::realloc()`、`std::free()`。
- `TqFree(nullptr)` 必须安全。
- `TqFreeBytes(ptr, bytes)` 保留 `bytes` 参数用于调用点语义和未来计数器，但释放时转调 `TqFree(ptr)`。

可选诊断：

- 增加仅测试或诊断构建启用的计数器，统计 allocator hook 命中次数。
- 计数器不作为第一阶段必需功能，避免把 allocator 改造和 metrics schema 绑定。

## zstd 接入

zstd 提供官方 `ZSTD_customMem`。`src/protocol/compress.cpp` 中 zstd context 创建改为：

```cpp
ZSTD_customMem mem{TqZstdAlloc, TqZstdFree, nullptr};
ZSTD_createCCtx_advanced(mem);
ZSTD_createDCtx_advanced(mem);
```

`ZSTD_customMem` 和 `ZSTD_create*Ctx_advanced()` 在当前 vendored zstd 头文件中属于 static-linking API，`compress.cpp` 必须在 include `zstd.h` 前定义：

```cpp
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
```

其中：

- `TqZstdAlloc(void* opaque, size_t size)` 调用 `TqMalloc(size)`。
- `TqZstdFree(void* opaque, void* address)` 调用 `TqFree(address)`。
- 不复用 `TqFreeBytes()`，因为 zstd free callback 不携带 size。

释放仍使用 `ZSTD_freeCCtx()` / `ZSTD_freeDCtx()`，由 zstd 使用同一组 callback 释放 context 内部内存。

测试重点：

- zstd compressor/decompressor 创建失败时仍返回 false 或创建失败语义。
- compress/decompress round trip 不变。
- `TCPQUIC_USE_MIMALLOC=OFF` 时仍可构建和运行。

## c-ares 接入

c-ares 提供库级 allocator hook：

```cpp
ares_library_init_mem(
    ARES_LIB_INIT_ALL,
    TqAresMalloc,
    TqAresFree,
    TqAresRealloc)
```

`TqAresLibraryGuard::Acquire()` 是合适的初始化边界，因为它已有互斥锁和进程内引用计数。改造规则：

- 仅在 `RefCount() == 0` 时调用 `ares_library_init_mem()`。
- `TqAresMalloc()` 调用 `TqMalloc()`。
- `TqAresFree()` 调用 `TqFree()`。
- `TqAresRealloc()` 调用 `TqRealloc()`。
- `Release()` 的 `ares_library_cleanup()` 保持不变。

风险约束：

- c-ares allocator 是库级全局配置，必须早于任何 c-ares 分配。
- 本项目应避免在 `TqAresLibraryGuard::Acquire()` 之外直接调用 c-ares 初始化。

## MsQuic 接入

MsQuic 没有公共 allocator callback。可行方案是维护 vendored patch，在平台层替换用户态分配入口。

目标文件：

- POSIX：`third_party/msquic/src/platform/platform_posix.c`
- Windows user mode：`third_party/msquic/src/platform/platform_winuser.c`
- 头文件声明按需调整：`third_party/msquic/src/inc/quic_platform_posix.h`、`third_party/msquic/src/inc/quic_platform_winuser.h`

构建开关：

```cmake
set(TCPQUIC_MSQUIC_USE_MIMALLOC AUTO CACHE STRING
    "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF")
set_property(CACHE TCPQUIC_MSQUIC_USE_MIMALLOC PROPERTY STRINGS AUTO ON OFF)
```

`TCPQUIC_MSQUIC_USE_MIMALLOC` 是 tri-state：

- `AUTO`：默认值，跟随最终的 `TCPQUIC_USE_MIMALLOC`。ASan 或用户配置把 `TCPQUIC_USE_MIMALLOC` 关闭后，MsQuic patch 也随之关闭。
- `ON`：强制启用 MsQuic vendored allocator patch；如果 `TCPQUIC_USE_MIMALLOC=OFF`，CMake 报错，避免生成没有 `mimalloc-static` target 的配置。
- `OFF`：单独关闭 MsQuic vendored allocator patch，即使项目 allocator 仍使用 mimalloc。

补丁策略：

- `TCPQUIC_MSQUIC_USE_MIMALLOC=OFF` 时保持上游行为。
- `AUTO` 且最终启用，或显式 `ON` 时，为 `msquic_platform` 加入 mimalloc include path、`TCPQUIC_MSQUIC_USE_MIMALLOC=1` 编译定义和 `mimalloc-static` 链接依赖。
- POSIX：
  - `CxPlatAlloc()` 使用 `mi_zalloc(ByteCount)` 或 `mi_calloc(1, ByteCount)`。
  - `CxPlatAllocUninitialized()` 使用 `mi_malloc(ByteCount)`。
  - `CxPlatFree()` 使用 `mi_free(Mem)`。
- Windows：
  - release 路径可直接替换 `HeapAlloc()` / `HeapFree()`。
  - debug 路径如保留 allocation tag 校验，必须维持原有 offset/tag 语义：分配 `ByteCount + AllocOffset`，返回偏移后地址，释放时还原原始指针再 `mi_free()`。
- 保留 MsQuic 原有 allocation failure debug 逻辑。

该补丁属于 vendored 依赖维护项，升级 MsQuic 时必须复查。

## quictls / OpenSSL 接入

OpenSSL 提供：

```c
CRYPTO_set_mem_functions(malloc_fn, realloc_fn, free_fn)
```

但它必须在 OpenSSL 发生任何分配前调用。由于 quictls 当前作为 MsQuic TLS provider 的内部依赖，不建议在 tcpquic-proxy 应用层晚初始化阶段调用。

本轮 Phase 5 结论：不接入 quictls/OpenSSL allocator hook，不新增 `CRYPTO_set_mem_functions()` 调用。

验证证据：

- `third_party/msquic/submodules/quictls/crypto/mem.c` 中 `CRYPTO_set_mem_functions()` 在 `allow_customize == 0` 后返回失败；`CRYPTO_malloc()` 首次走默认实现时会把 `allow_customize` 置 0。
- `third_party/msquic/src/platform/crypt_openssl.c` 的 `CxPlatCryptInitialize()` 首先调用 `OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL)`，随后预取 `EVP_CIPHER_fetch()` / `EVP_MAC_fetch()` 并创建 `EVP_MAC_CTX_new()`。
- `third_party/msquic/src/platform/platform_posix.c` 和 `third_party/msquic/src/platform/platform_winuser.c` 都从 `CxPlatInitialize()` 进入 `CxPlatCryptInitialize()`，没有 tcpquic-proxy 可控、且能证明早于所有 OpenSSL/quictls 分配的初始化回调。
- CMake 当前固定 vendored quictls static build，并把 OpenSSL 链入 MsQuic 平台目标；链接关系本身不能提供“早于 OpenSSL 首次 allocation”的运行时保证。

因此，在 `OPENSSL_init_ssl()` 之后或 TLS context 创建路径中调用 hook 都可能已经晚于首次 OpenSSL allocation，且返回值可能失败。即使返回成功，也缺少覆盖 provider/config 初始化、cipher/mac fetch、TLS handshake 成功和失败路径的顺序证明。本轮选择记录结论并保持上游 OpenSSL allocator 行为，避免 allocator 混用。

未来只有满足以下前置条件才重新评估：

- 找到或新增一个单次执行的最早初始化点，能够证明早于 `OPENSSL_init_ssl()`、provider/config 载入、所有 `EVP_*_fetch()`、`SSL_CTX_new()`、`SSL_new()` 以及任何 quictls 内部 allocation。
- 独立开关控制 OpenSSL allocator hook，不能和 MsQuic 平台 allocator patch 绑定成不可单独关闭的路径。
- callback 签名必须匹配 OpenSSL API：`malloc(size_t, const char*, int)`、`realloc(void*, size_t, const char*, int)`、`free(void*, const char*, int)`；`file` / `line` 参数只用于兼容签名，不参与 allocator 选择。
- 三个 callback 必须同时替换，必须检查 `CRYPTO_set_mem_functions()` 返回值。
- 必须覆盖 TLS handshake 成功、handshake 失败、证书加载失败和进程退出路径。

## spdlog / fmt / STL

本阶段不处理 spdlog、fmt、STL 容器 allocator。

原因：

- 没有进程级 allocator hook。
- 深改类型别名或 sink/logger 工厂会扩大改动面。
- 日志路径不是当前研究文档确认的 allocator 接入主目标。

后续只有 profiling 证明日志分配是瓶颈时，才单独评估。

## 构建与平台策略

构建矩阵：

| 构建 | mimalloc 策略 |
|---|---|
| Linux release | 启用显式 mimalloc |
| Linux ASan | 禁用 mimalloc |
| macOS release | 启用显式 mimalloc，并关闭 `MI_OVERRIDE` / `MI_OSX_INTERPOSE` / `MI_OSX_ZONE` |
| Windows release | 启用显式 mimalloc，MsQuic patch 需保留 debug tag 语义 |

`TCPQUIC_USE_MIMALLOC=OFF` 必须保持可用，用于问题隔离和 sanitizer。

## 验收标准

功能验收：

- `TCPQUIC_USE_MIMALLOC=ON` 时构建成功，且没有 mimalloc 全局 override。
- `TCPQUIC_USE_MIMALLOC=OFF` 时构建成功，行为回退到 libc allocator。
- zstd 压缩/解压路径正常。
- c-ares DNS 解析路径正常。
- QUIC 连接建立、断开、重连正常。

诊断验收：

- zstd 和 c-ares hook 可通过临时计数器或调试日志确认命中。
- relay buffer 继续命中项目 allocator。
- MsQuic patch 阶段可确认 `CxPlatAlloc*` / `CxPlatFree` 走 mimalloc。

稳定性验收：

- 启动/退出无 allocator 混用崩溃。
- ASan 构建不链接 mimalloc 显式路径。
- Windows debug 构建不破坏 MsQuic allocation tag 校验。

## 风险

| 风险 | 缓解 |
|---|---|
| allocator 混用 | 只在同一库提供的 hook 内成对分配/释放；不跨库释放指针 |
| c-ares/OpenSSL 初始化顺序错误 | c-ares hook 放在现有 library guard；OpenSSL 本轮因无法证明早于首次 allocation 而不接入 hook |
| MsQuic vendored patch 升级成本 | 独立构建开关、补丁集中在平台层 allocator 函数 |
| ASan 与 mimalloc 诊断冲突 | ASan 构建继续强制 `TCPQUIC_USE_MIMALLOC=OFF` |
| spdlog/STL 未覆盖 | 明确非目标，避免以全局 override 追求覆盖率 |
