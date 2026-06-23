# 显式 mimalloc 内存管理研究

## 背景

当前工程通过 `TCPQUIC_USE_MIMALLOC` 控制 vendored mimalloc 的构建和链接。项目已有 `TqMalloc()` / `TqCalloc()` / `TqRealloc()` / `TqFree()` 以及 `TqAllocBytes()` / `TqFreeBytes()` 封装，用于项目内显式 allocator 路径。

实际测试发现，依赖 mimalloc 的编译期全局 override 在部分平台上存在兼容性问题。本轮改造目标是避免全局 malloc/new override，改为在项目代码和支持 allocator hook 的第三方库中显式接入 mimalloc。

## 当前状态

- 顶层 `CMakeLists.txt` 在 `TCPQUIC_USE_MIMALLOC=ON` 时加入 `third_party/mimalloc`，并显式关闭 `MI_OVERRIDE` / `MI_OSX_INTERPOSE` / `MI_OSX_ZONE`。
- `src/CMakeLists.txt` 将 `mimalloc-static` 链接到 `tcpquic-proxy` 和相关测试，并定义 `TCPQUIC_USE_MIMALLOC=1`。
- `src/tunnel/relay_alloc.cpp` 提供项目显式 allocator API，`TCPQUIC_USE_MIMALLOC=OFF` 时回退 libc allocator。
- zstd 已通过 `ZSTD_customMem` 接入项目 allocator。
- c-ares 已通过 `ares_library_init_mem()` 接入项目 allocator。
- MsQuic vendored 平台层 allocator patch 由 `TCPQUIC_MSQUIC_USE_MIMALLOC=AUTO|ON|OFF` 控制。
- quictls/OpenSSL 本轮不接入 `CRYPTO_set_mem_functions()`。

## 实际依赖范围

当前主程序实际链接或通过依赖使用的第三方库：

| 依赖 | 当前用途 | 是否需要 allocator 接入 |
|---|---|---|
| mimalloc | 目标 allocator | 是 |
| msquic | QUIC 协议栈 | 是，需要源码级处理 |
| quictls / OpenSSL | msquic TLS provider | 本轮暂不接入；必须先证明初始化顺序 |
| zstd | 压缩 / 解压 | 是，可直接使用官方 hook |
| c-ares | 异步 DNS | 是，可直接使用官方 hook |
| spdlog / bundled fmt | 日志 | 可选，当前不建议深改 |

`third_party/lz4` 后续会从仓库删除，当前不纳入改造范围。

## 各依赖 allocator hook 分析

### mimalloc

mimalloc 本身推荐两类用法：

- 显式 API：`mi_malloc()`、`mi_free()`、`mi_realloc()`、`mi_calloc()` 等。
- 全局 override：替换 libc malloc/free 或 C++ new/delete。

本项目目标应选择显式 API。需要在 CMake 中显式关闭 mimalloc override：

```cmake
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
set(MI_OSX_INTERPOSE OFF CACHE BOOL "" FORCE)
set(MI_OSX_ZONE OFF CACHE BOOL "" FORCE)
```

其中 `MI_OSX_INTERPOSE` 和 `MI_OSX_ZONE` 只影响 macOS，但建议一并关闭，避免不同平台行为不一致。

### zstd

zstd 提供正式 allocator hook：

- `ZSTD_customMem`
- `ZSTD_createCCtx_advanced()`
- `ZSTD_createCStream_advanced()`
- `ZSTD_createDCtx_advanced()`
- `ZSTD_createDStream_advanced()`
- `ZSTD_createCDict_advanced()`
- `ZSTD_createDDict_advanced()`

当前项目只创建 streaming compression / decompression context，`compress.cpp` 已将历史默认 context 创建方式：

```cpp
ZSTD_createCCtx();
ZSTD_createDCtx();
```

替换为项目 allocator hook：

```cpp
ZSTD_customMem mem{TqZstdAlloc, TqZstdFree, nullptr};
ZSTD_createCCtx_advanced(mem);
ZSTD_createDCtx_advanced(mem);
```

`TqZstdAlloc()` 调用 `TqMalloc()`，`TqZstdFree()` 调用 `TqFree()`。zstd 的 free callback 不带 size，因此不复用 `TqFreeBytes(ptr, bytes)`；通过项目 allocator API 可以在 `TCPQUIC_USE_MIMALLOC=ON/OFF` 下保持同一 allocator family。

### c-ares

c-ares 提供正式 allocator hook：

```c
ares_library_init_mem(flags, amalloc, afree, arealloc)
```

`src/tunnel/ares_dns_resolver.cpp` 中 `TqAresLibraryGuard::Acquire()` 已在首次引用时调用：

```cpp
ares_library_init_mem(
    ARES_LIB_INIT_ALL,
    TqAresMalloc,
    TqAresFree,
    TqAresRealloc)
```

注意点：

- c-ares allocator 是库级全局配置，不是 channel 级配置。
- 必须在第一次 c-ares 初始化前设置。
- 当前 `TqAresLibraryGuard` 已经有进程内引用计数和互斥锁，适合承载这个初始化顺序。

### msquic

MsQuic 公共 API 未发现 allocator callback。`QUIC_API_TABLE` 提供 `SetParam` / `GetParam`，全局参数包括 retry memory、settings、perf counters、TLS provider 等，但没有 allocator 参数。

MsQuic 内部用户态分配统一收敛到平台层：

- POSIX: `CxPlatAlloc()` 使用 `calloc()`，`CxPlatAllocUninitialized()` 使用 `malloc()`，`CxPlatFree()` 使用 `free()`。
- Windows user mode: `CxPlatAlloc()` / `CxPlatAllocUninitialized()` 使用 `HeapAlloc()`，`CxPlatFree()` 使用 `HeapFree()`。
- 上层通过 `CXPLAT_ALLOC_*` / `CXPLAT_FREE` 宏调用平台层函数。

因此，MsQuic 不能通过应用层 API 显式传入 mimalloc allocator。可行方案是维护 vendored MsQuic 补丁，在用户态平台层将 `CxPlatAlloc*` / `CxPlatFree` 改为 mimalloc。

构建开关采用 tri-state：

```cmake
set(TCPQUIC_MSQUIC_USE_MIMALLOC AUTO CACHE STRING
    "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF")
set_property(CACHE TCPQUIC_MSQUIC_USE_MIMALLOC PROPERTY STRINGS AUTO ON OFF)
```

语义：

- `AUTO`：默认值，跟随最终的 `TCPQUIC_USE_MIMALLOC`。ASan 或用户显式关闭 `TCPQUIC_USE_MIMALLOC` 时，MsQuic allocator patch 也关闭。
- `ON`：强制启用 MsQuic vendored allocator patch；如果 `TCPQUIC_USE_MIMALLOC=OFF`，CMake 报错，避免缺少 `mimalloc-static` target。
- `OFF`：单独关闭 MsQuic vendored allocator patch，即使项目 allocator 仍使用 mimalloc。

补丁策略：

- `AUTO` 且最终启用，或显式 `ON` 时，为 `msquic_platform` 加入 mimalloc include path、`TCPQUIC_MSQUIC_USE_MIMALLOC=1` 编译定义和 `mimalloc-static` 链接依赖。
- `OFF` 时保留上游 allocator 行为。
- POSIX:
  - zero-init 路径使用 `mi_zalloc(ByteCount)` 或 `mi_calloc(1, ByteCount)`。
  - uninitialized 路径使用 `mi_malloc(ByteCount)`。
  - free 路径使用 `mi_free(Mem)`。
- Windows:
  - release 构建直接用 mimalloc 替换 `HeapAlloc/HeapFree`。
  - debug 构建当前在 allocation 前面额外写入 tag 并在 free 时校验。若保留 tag 校验，需要继续分配 `ByteCount + AllocOffset`，返回偏移后的地址，释放时还原指针后 `mi_free()`。
- 保留 MsQuic 原有 alloc failure debug 逻辑。

### quictls / OpenSSL

当前顶层配置使用：

```cmake
set(QUIC_TLS_LIB "quictls" CACHE STRING "TLS Library to use" FORCE)
set(QUIC_USE_SYSTEM_LIBCRYPTO OFF CACHE BOOL "Use vendored quictls crypto" FORCE)
```

OpenSSL / quictls 提供内存函数 hook：

```c
CRYPTO_set_mem_functions(malloc_fn, realloc_fn, free_fn)
```

但它要求在 OpenSSL 发生任何分配前调用，否则可能失败或产生 allocator 混用风险。由于 quictls 是 MsQuic TLS provider 的内部依赖，不能在应用层晚初始化阶段调用。

Phase 5 评估结论：本轮不接入 quictls/OpenSSL allocator hook，不新增 `CRYPTO_set_mem_functions()` 调用。

原因：

- `third_party/msquic/submodules/quictls/crypto/mem.c` 中 `CRYPTO_set_mem_functions()` 在 `allow_customize == 0` 后返回失败；`CRYPTO_malloc()` 首次使用默认实现时会把 `allow_customize` 置 0。
- MsQuic 的 `CxPlatCryptInitialize()` 首先调用 `OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL)`，该调用可能触发 OpenSSL 配置、provider 和内部对象初始化分配。
- 同一初始化函数随后执行 `EVP_CIPHER_fetch()`、`EVP_MAC_fetch()` 和 `EVP_MAC_CTX_new()`，这些路径会进入 OpenSSL 3 provider/fetch/context 分配逻辑。
- POSIX 和 Windows user mode 都从 `CxPlatInitialize()` 调用 `CxPlatCryptInitialize()`；TLS provider 的 `SSL_CTX_new()` / `SSL_new()` 发生得更晚，不能作为安全 hook 点。

因此，当前不能严格证明存在早于 OpenSSL/quictls 首次 allocation 的可控插入点。本轮保持 OpenSSL 默认 allocator，避免 allocator 混用。

未来只有满足以下前置条件才重新评估：

- 找到或新增单次执行的最早初始化点，并证明其早于 `OPENSSL_init_ssl()`、provider/config 载入、所有 `EVP_*_fetch()`、`SSL_CTX_new()`、`SSL_new()` 和 quictls 内部 allocation。
- 新增独立开关控制 OpenSSL allocator hook，不能成为 `TCPQUIC_MSQUIC_USE_MIMALLOC` 下不可单独关闭的副作用。
- 同时替换 malloc/realloc/free 三个 callback，并检查 `CRYPTO_set_mem_functions()` 返回值。
- 覆盖 TLS handshake 成功、handshake 失败、证书加载失败和进程退出路径。

### spdlog / bundled fmt

spdlog 没有进程级 allocator hook。其内存主要来自：

- `std::shared_ptr` / `std::make_shared`
- `std::string`
- `std::vector`
- `spdlog::memory_buf_t`
- bundled fmt 的 `fmt::basic_memory_buffer`

理论上可以通过替换 allocator 模板参数、改 spdlog 类型别名、或引入自定义 sink/logger 工厂来局部控制，但改动面大，收益不确定，也容易和上游升级冲突。

建议当前不处理 spdlog。若后续 profiling 证明日志路径分配是问题，再单独评估。

## 改造阶段状态

1. 已关闭 mimalloc 全局 override，仅保留显式 `mi_*` API 链接。
2. 已扩展项目 allocator 封装：
   - `TqMalloc()`
   - `TqCalloc()`
   - `TqRealloc()`
   - `TqFree()`
   - 必要时增加 STL allocator，但不要全局替换 `new/delete`。
3. zstd context 已改为 `ZSTD_create*_advanced()` 并传入项目 allocator hook。
4. c-ares 初始化已改为 `ares_library_init_mem()`。
5. MsQuic 平台层已做 vendored 补丁，由 `TCPQUIC_MSQUIC_USE_MIMALLOC=AUTO|ON|OFF` 控制 `CxPlatAlloc*` / `CxPlatFree` 是否走 mimalloc。
6. quictls/OpenSSL 本轮暂不接入 `CRYPTO_set_mem_functions()`；只记录评估结论和未来前置条件。
7. spdlog 暂不处理，除非 profiling 显示有明确收益。

## 验证建议

- 构建验证：
  - Linux release
  - Linux ASan
  - Windows release
  - macOS release
- 运行验证：
  - 启动 / 退出无 allocator 混用崩溃。
  - DNS 查询路径覆盖 c-ares allocator。
  - zstd 压缩和解压路径覆盖 custom allocator。
  - QUIC 连接建立、断开、重连覆盖 MsQuic allocator。
  - quictls/OpenSSL 路径保持默认 allocator；不应出现新增 `CRYPTO_set_mem_functions()` 调用。
- 诊断验证：
  - 临时在项目 allocator hook 中增加计数器，确认 zstd / c-ares / relay buffer 命中。
  - MsQuic 补丁阶段可在 `CxPlatAlloc*` 增加 debug counter，确认路径命中。
  - ASan 构建下保持 mimalloc disabled 或确保 mimalloc 使用非 override 且与 ASan 策略一致。

## 风险点

- allocator 混用是最大风险。由 mimalloc 分配的内存必须由 mimalloc 释放，反之亦然。
- c-ares 和 OpenSSL 的 allocator hook 都是库级全局行为，必须保证首次初始化前设置；OpenSSL 本轮因无法证明顺序而不接入。
- MsQuic 没有公共 allocator hook，后续需要维护 vendored patch，升级 MsQuic 时要重新检查平台层分配函数。
- Windows debug build 中 MsQuic 当前有 tag 校验逻辑，替换 allocator 时需要保留 offset/tag 语义。
- spdlog 和 STL 容器不会自动走 mimalloc，除非使用全局 override 或全面引入 custom allocator。
