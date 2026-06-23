# 显式 mimalloc 内存管理开发计划

日期：2026-06-23

依据：`docs/memory-allocator.md`、`docs/memory-allocator-design.md`

## 目标

按阶段把 tcpquic-proxy 的 allocator 接入从 relay buffer 扩展到项目显式 API、zstd、c-ares，完成 MsQuic vendored patch，并记录 quictls/OpenSSL no-hook 评估结论。

## 执行原则

- 每个阶段都保持 `TCPQUIC_USE_MIMALLOC=ON/OFF` 可构建；MsQuic vendored patch 额外保持 `TCPQUIC_MSQUIC_USE_MIMALLOC=AUTO|ON|OFF` 可配置。
- 每个第三方库只使用自己的 allocator hook 或 vendored 平台层入口。
- 不引入全局 malloc/new override。
- 先完成项目代码和官方 hook，再处理 MsQuic vendored patch，并评估 quictls/OpenSSL hook 是否满足安全前置条件。
- 每个阶段完成后运行最小可验证测试，再进入下一阶段。

## 阶段 0：确认构建开关基线

### 文件

- 修改：`CMakeLists.txt`

### 任务

1. 保留现有 ASan 自动禁用 mimalloc 逻辑。
2. 保留现有 `MI_OVERRIDE OFF`。
3. 在 `TCPQUIC_USE_MIMALLOC` 分支内补充 macOS override 关闭项：

```cmake
set(MI_OSX_INTERPOSE OFF CACHE BOOL "" FORCE)
set(MI_OSX_ZONE OFF CACHE BOOL "" FORCE)
```

### 验证

```bash
rtk cmake -S . -B build-mimalloc -DTCPQUIC_USE_MIMALLOC=ON
rtk cmake -S . -B build-glibc -DTCPQUIC_USE_MIMALLOC=OFF
```

预期：

- 两个 configure 都成功。
- `build-mimalloc` 中 mimalloc static target 存在。
- `build-glibc` 不要求 `third_party/mimalloc` 参与构建。

## 阶段 1：扩展项目 allocator API

### 文件

- 修改：`src/tunnel/relay_alloc.h`
- 修改：`src/tunnel/relay_alloc.cpp`
- 修改或新增测试：`src/unittest/relay_buffer_test.cpp` 或新建 `src/unittest/relay_alloc_test.cpp`
- 修改：`src/CMakeLists.txt`，仅在新增测试目标时需要

### 任务

1. 在 `relay_alloc.h` 增加：

```cpp
void* TqMalloc(size_t bytes);
void* TqCalloc(size_t count, size_t bytes);
void* TqRealloc(void* ptr, size_t bytes);
void TqFree(void* ptr);
```

2. 在 `relay_alloc.cpp` 实现：

```cpp
void* TqMalloc(size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_malloc(bytes);
#else
  return std::malloc(bytes);
#endif
}

void* TqCalloc(size_t count, size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_calloc(count, bytes);
#else
  return std::calloc(count, bytes);
#endif
}

void* TqRealloc(void* ptr, size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_realloc(ptr, bytes);
#else
  return std::realloc(ptr, bytes);
#endif
}

void TqFree(void* ptr) {
  if (ptr == nullptr) return;
#if TCPQUIC_USE_MIMALLOC
  mi_free(ptr);
#else
  std::free(ptr);
#endif
}
```

3. 将现有 `TqAllocBytes()` / `TqFreeBytes()` 改为转调：

```cpp
void* TqAllocBytes(size_t bytes) {
  return TqMalloc(bytes);
}

void TqFreeBytes(void* ptr, size_t bytes) {
  (void)bytes;
  TqFree(ptr);
}
```

4. 测试覆盖：

- `TqMalloc()` 分配可写内存并能释放。
- `TqCalloc()` 返回清零内存。
- `TqRealloc()` 能扩大已有内容。
- `TqFree(nullptr)` 不崩溃。
- relay buffer 现有测试继续通过。

### 验证

```bash
rtk cmake --build build-mimalloc --target tcpquic_relay_buffer_test -j
rtk ./build-mimalloc/bin/Release/tcpquic_relay_buffer_test
rtk cmake --build build-glibc --target tcpquic_relay_buffer_test -j
rtk ./build-glibc/bin/Release/tcpquic_relay_buffer_test
```

如果新增 `tcpquic_relay_alloc_test`，同样在 ON/OFF 两套构建中运行。

## 阶段 2：接入 zstd custom allocator

### 文件

- 修改：`src/protocol/compress.cpp`
- 修改：`src/CMakeLists.txt`
- 测试：`src/unittest/compress_test.cpp`

### 任务

1. 在 `compress.cpp` 的 zstd 编译分支内启用 zstd static-linking API，并 include allocator：

```cpp
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include "relay_alloc.h"
```

`ZSTD_STATIC_LINKING_ONLY` 必须出现在 `#include <zstd.h>` 之前，因为 `ZSTD_customMem` 和 `ZSTD_create*Ctx_advanced()` 在当前 vendored zstd 头文件中属于该 API 区域。

2. 在匿名 namespace 中增加 zstd callback：

```cpp
void* TqZstdAlloc(void* opaque, size_t size) {
    (void)opaque;
    return TqMalloc(size);
}

void TqZstdFree(void* opaque, void* address) {
    (void)opaque;
    TqFree(address);
}

ZSTD_customMem TqZstdCustomMem() {
    return ZSTD_customMem{TqZstdAlloc, TqZstdFree, nullptr};
}
```

3. 将 compressor context 创建改为：

```cpp
ctx_ = ZSTD_createCCtx_advanced(TqZstdCustomMem());
```

4. 将 decompressor context 创建改为：

```cpp
ctx_ = ZSTD_createDCtx_advanced(TqZstdCustomMem());
```

5. 保持 `ZSTD_freeCCtx()` / `ZSTD_freeDCtx()` 不变。

6. 修改 `tcpquic_compress_test`，让它和生产目标一样链接 allocator：

```cmake
add_executable(tcpquic_compress_test
    unittest/compress_test.cpp
    protocol/compress.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_compress_test)
target_link_libraries(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_LIBS})
target_compile_definitions(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_DEFS})
tcpquic_link_mimalloc(tcpquic_compress_test)
```

7. 检查所有编译 `protocol/compress.cpp` 的 target。凡是还没有包含 `tunnel/relay_alloc.cpp` 的 target，都必须补上该源文件并调用 `tcpquic_link_mimalloc(target)`。当前至少需要重点检查：

- `tcpquic_compress_test`
- `tcpquic_tunnel_test`
- `tcpquic_speed_test_test`
- `tcpquic_client_tunnel_open_test`
- `tcpquic_linux_relay_worker_io_test`
- `tcpquic_windows_relay_worker_test`

### 验证

运行 compression round-trip 测试。若当前没有独立测试，补充以下行为测试：

- `TqCreateCompressor(TqCompressAlgo::Zstd, level)` 返回非空。
- 输入一段非空 payload，压缩后再解压，输出与输入完全一致。
- `Reset()` 后重复 round-trip 仍正确。
- `DecompressInto()` 在输出 buffer 太小时正确设置 `NeedsMoreOutput`。

命令按实际目标运行，例如：

```bash
rtk cmake --build build-mimalloc --target tcpquic_compress_test -j
rtk ./build-mimalloc/bin/Release/tcpquic_compress_test
rtk cmake --build build-glibc --target tcpquic_compress_test -j
rtk ./build-glibc/bin/Release/tcpquic_compress_test

rtk cmake --build build-mimalloc --target tcpquic_tunnel_test -j
rtk ./build-mimalloc/bin/Release/tcpquic_tunnel_test
rtk cmake --build build-glibc --target tcpquic_tunnel_test -j
rtk ./build-glibc/bin/Release/tcpquic_tunnel_test
```

## 阶段 3：接入 c-ares allocator hook

### 文件

- 修改：`src/tunnel/ares_dns_resolver.cpp`
- 修改：`src/CMakeLists.txt`
- 测试：`src/unittest/ares_dns_resolver_test.cpp`

### 任务

1. include allocator：

```cpp
#include "relay_alloc.h"
```

2. 在匿名 namespace 中增加 callback：

```cpp
void* TqAresMalloc(size_t size) {
    return TqMalloc(size);
}

void TqAresFree(void* ptr) {
    TqFree(ptr);
}

void* TqAresRealloc(void* ptr, size_t size) {
    return TqRealloc(ptr, size);
}
```

3. 将 `TqAresLibraryGuard::Acquire()` 中的初始化替换为：

```cpp
if (RefCount() == 0 &&
    ares_library_init_mem(ARES_LIB_INIT_ALL, TqAresMalloc, TqAresFree, TqAresRealloc) != ARES_SUCCESS) {
    return false;
}
```

4. 保留互斥锁、引用计数、`ares_library_cleanup()` 行为。

5. 修改所有直接编译 `tunnel/ares_dns_resolver.cpp` 的测试 target，补上 `tunnel/relay_alloc.cpp` 并调用 `tcpquic_link_mimalloc(target)`。当前至少包括：

- `tcpquic_ares_dns_resolver_test`
- `tcpquic_server_dial_reactor_test`
- `tcpquic_server_dial_reactor_worker_test`
- `tcpquic_speed_test_test`
- `tcpquic_tunnel_test`
- `tcpquic_client_tunnel_open_test`

示例：

```cmake
set(TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES
    unittest/ares_dns_resolver_test.cpp
    tunnel/ares_dns_resolver.cpp
    tunnel/relay_alloc.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
add_executable(tcpquic_ares_dns_resolver_test ${TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_ares_dns_resolver_test)
target_link_libraries(tcpquic_ares_dns_resolver_test PRIVATE Threads::Threads ${TCPQUIC_CARES_TARGET})
target_compile_definitions(tcpquic_ares_dns_resolver_test PRIVATE TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_ares_dns_resolver_test)
```

### 验证

运行 DNS resolver 路径 smoke test：

- 启动 client/server 中会触发解析的路径。
- 解析一个普通 hostname。
- 退出进程无崩溃。

若测试环境不稳定，至少运行包含 `ares_dns_resolver.cpp` 的单元测试目标，并在本地增加临时计数器确认 malloc/free/realloc callback 命中；计数器验证完成后不要提交调试输出。

```bash
rtk cmake --build build-mimalloc --target tcpquic_ares_dns_resolver_test -j
rtk ./build-mimalloc/bin/Release/tcpquic_ares_dns_resolver_test
rtk cmake --build build-glibc --target tcpquic_ares_dns_resolver_test -j
rtk ./build-glibc/bin/Release/tcpquic_ares_dns_resolver_test
```

## 阶段 4：MsQuic vendored allocator patch

### 文件

- 修改：`CMakeLists.txt`
- 修改：`third_party/msquic/src/platform/platform_posix.c`
- 修改：`third_party/msquic/src/platform/platform_winuser.c`
- 可能修改：`third_party/msquic/src/inc/quic_platform_posix.h`
- 可能修改：`third_party/msquic/src/inc/quic_platform_winuser.h`

### 任务

1. 增加 tri-state 构建开关：

```cmake
set(TCPQUIC_MSQUIC_USE_MIMALLOC AUTO CACHE STRING
    "Use mimalloc in vendored MsQuic platform allocator: AUTO, ON, or OFF")
set_property(CACHE TCPQUIC_MSQUIC_USE_MIMALLOC PROPERTY STRINGS AUTO ON OFF)
```

`AUTO` 跟随最终的 `TCPQUIC_USE_MIMALLOC`，`ON` 强制启用并要求 `TCPQUIC_USE_MIMALLOC=ON`，`OFF` 单独关闭 MsQuic allocator patch。

2. 当 tri-state 解析后的 `TCPQUIC_MSQUIC_MIMALLOC_ENABLED=ON` 时，为 MsQuic 平台目标传入 mimalloc 依赖。当前实现放在 `third_party/msquic/src/platform/CMakeLists.txt`：

```cmake
if(TCPQUIC_MSQUIC_MIMALLOC_ENABLED)
    target_compile_definitions(msquic_platform PRIVATE TCPQUIC_MSQUIC_USE_MIMALLOC=1)
    target_include_directories(msquic_platform PRIVATE "${TCPQUIC_MIMALLOC_SOURCE_DIR}/include")
    target_link_libraries(msquic_platform PRIVATE mimalloc-static)
endif()
```

顶层 CMake 在 `add_subdirectory("${MSQUIC_SOURCE_DIR}" ...)` 之前完成 tri-state 解析，并在启用时要求 `mimalloc-static` target 已存在。

3. POSIX 平台层：

- `CxPlatAlloc()` 在开关开启时使用 mimalloc zero-init API。
- `CxPlatAllocUninitialized()` 使用 `mi_malloc()`。
- `CxPlatFree()` 使用 `mi_free()`。
- 开关关闭时保留上游 `calloc()` / `malloc()` / `free()`。

4. Windows user mode 平台层：

- release 路径替换为 mimalloc。
- debug 路径保留 tag 校验和 offset 语义。
- allocation failure 注入和 debug logging 不删除。

### 验证

Linux：

```bash
rtk cmake -S . -B build-msquic-mi -DTCPQUIC_USE_MIMALLOC=ON -DTCPQUIC_MSQUIC_USE_MIMALLOC=ON
rtk cmake --build build-msquic-mi --target tcpquic-proxy -j
rtk ./build-msquic-mi/bin/Release/tcpquic-proxy --help

rtk cmake -S . -B build-msquic-tristate-default -DTCPQUIC_USE_MIMALLOC=ON
rtk cmake -S . -B build-msquic-tristate-explicit-off -DTCPQUIC_USE_MIMALLOC=ON -DTCPQUIC_MSQUIC_USE_MIMALLOC=OFF
```

无效组合验证，预期 configure 失败并输出 `TCPQUIC_MSQUIC_USE_MIMALLOC=ON requires TCPQUIC_USE_MIMALLOC=ON`：

```bash
rtk cmake -S . -B build-msquic-tristate-invalid-on-off -DTCPQUIC_USE_MIMALLOC=OFF -DTCPQUIC_MSQUIC_USE_MIMALLOC=ON
```

Windows release：

```powershell
cmake -S . -B build-msquic-mi -DTCPQUIC_USE_MIMALLOC=ON -DTCPQUIC_MSQUIC_USE_MIMALLOC=ON
cmake --build build-msquic-mi --config Release --target tcpquic-proxy
```

运行 smoke：

- QUIC listener 启动成功。
- client 建连成功。
- 连接断开和进程退出无崩溃。

## 阶段 5：quictls / OpenSSL allocator 评估结论

### 文件

- 调研：`third_party/msquic/src/platform/tls_quictls.c`
- 调研：`third_party/msquic/src/platform/crypt_openssl.c`
- 调研：`third_party/msquic/src/platform/platform_posix.c`
- 调研：`third_party/msquic/src/platform/platform_winuser.c`
- 调研：`third_party/msquic/submodules/quictls/crypto/mem.c`
- 调研：顶层 CMake、`third_party/msquic/CMakeLists.txt`、`third_party/msquic/src/platform/CMakeLists.txt`、`third_party/msquic/submodules/CMakeLists.txt`
- 修改：`docs/memory-allocator-design.md`
- 修改：`docs/memory-allocator-dev-plan.md`

### 结论

本轮不接入 quictls/OpenSSL allocator hook，不修改 quictls/MsQuic TLS 源码，不新增 `CRYPTO_set_mem_functions()` 调用。

原因是当前无法严格证明存在一个 tcpquic-proxy 可控的调用点早于 OpenSSL/quictls 首次 allocation：

- `third_party/msquic/submodules/quictls/crypto/mem.c` 中 `CRYPTO_set_mem_functions()` 会在 `allow_customize == 0` 后返回 0；`CRYPTO_malloc()` 首次使用默认实现时会把 `allow_customize` 置 0。
- `third_party/msquic/src/platform/crypt_openssl.c` 的 `CxPlatCryptInitialize()` 首先调用 `OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, NULL)`。该调用可能触发 OpenSSL 配置、provider 和内部对象初始化分配。
- 同一初始化函数随后立即执行 `EVP_CIPHER_fetch()`、`EVP_MAC_fetch()` 和 `EVP_MAC_CTX_new()`，这些路径都会进入 OpenSSL 3 provider/fetch/上下文分配逻辑。
- POSIX 和 Windows user mode 都从 `CxPlatInitialize()` 调用 `CxPlatCryptInitialize()`；TLS provider 的 `SSL_CTX_new()` / `SSL_new()` 发生得更晚，不能作为安全 hook 点。
- CMake 当前固定使用 vendored quictls static build；静态链接和 target 依赖只能说明链接路径，不能证明运行时 hook 早于 OpenSSL 首次 allocation。

因此，在 `CxPlatCryptInitialize()` 内 `OPENSSL_init_ssl()` 之后、`tls_quictls.c` 的 TLS context 初始化中，或 tcpquic-proxy 应用层晚初始化阶段调用 `CRYPTO_set_mem_functions()` 都不满足安全条件。为了避免 allocator 混用，本轮只记录评估结论。

### 未来前置条件

只有满足以下条件才重新开启实现：

1. 找到或新增单次执行的最早初始化点，并证明其早于 `OPENSSL_init_ssl()`、provider/config 载入、所有 `EVP_*_fetch()`、`SSL_CTX_new()`、`SSL_new()` 和 quictls 内部 allocation。
2. 新增独立开关控制 OpenSSL allocator hook，不能成为 `TCPQUIC_MSQUIC_USE_MIMALLOC` 下不可单独关闭的副作用。
3. 同时提供 malloc/realloc/free 三个 callback，签名匹配 OpenSSL API，且全部使用同一 allocator family。
4. 检查 `CRYPTO_set_mem_functions()` 返回值，失败时禁用该路径并保留可诊断错误。
5. 跑通 TLS handshake 成功路径、handshake 失败路径、证书加载失败路径和进程退出路径。

未来若满足上述条件，实现形态必须至少包含：

```c
static void* TqOpenSslMalloc(size_t size, const char* file, int line) {
    (void)file;
    (void)line;
    return mi_malloc(size);
}

static void* TqOpenSslRealloc(void* ptr, size_t size, const char* file, int line) {
    (void)file;
    (void)line;
    return mi_realloc(ptr, size);
}

static void TqOpenSslFree(void* ptr, const char* file, int line) {
    (void)file;
    (void)line;
    mi_free(ptr);
}

CRYPTO_set_mem_functions(TqOpenSslMalloc, TqOpenSslRealloc, TqOpenSslFree);
```

### 验证

本轮未实现 hook，因此验证重点是文档和源码边界：

```bash
rtk git diff --check -- docs/memory-allocator.md docs/memory-allocator-design.md docs/memory-allocator-dev-plan.md
rtk rg -n 'CRYPTO_set_mem_functions|TCPQUIC_MSQUIC_USE_MIMALLOC|AUTO|quictls|OpenSSL' docs/memory-allocator.md docs/memory-allocator-design.md docs/memory-allocator-dev-plan.md
rtk rg -n 'CRYPTO_set_mem_functions\s*\(' third_party/msquic/src src CMakeLists.txt third_party/msquic/submodules/quictls/crypto/mem.c -g '*.c' -g '*.cc' -g '*.cpp' -g '*.h' -g '*.hpp' -g 'CMakeLists.txt'
```

注意：`git diff --check -- <path>` 只检查已跟踪或已暂存的 diff；对未跟踪文档不会检查内容。若这些文档仍是未跟踪文件，需要先用 `git add -N docs/memory-allocator.md docs/memory-allocator-design.md docs/memory-allocator-dev-plan.md` 建立 intent-to-add，或单独检查未跟踪文件内容后再运行该命令。

第三条命令预期只看到 `third_party/msquic/submodules/quictls/crypto/mem.c` 中的函数定义；不应出现 tcpquic-proxy 或 MsQuic 平台层新增调用。

## 阶段 6：端到端回归

### 构建矩阵

```bash
rtk cmake -S . -B build-mimalloc -DTCPQUIC_USE_MIMALLOC=ON
rtk cmake --build build-mimalloc --target tcpquic-proxy -j

rtk cmake -S . -B build-glibc -DTCPQUIC_USE_MIMALLOC=OFF
rtk cmake --build build-glibc --target tcpquic-proxy -j

rtk cmake -S . -B build-asan -DCMAKE_CXX_FLAGS=-fsanitize=address -DCMAKE_C_FLAGS=-fsanitize=address
rtk cmake --build build-asan --target tcpquic-proxy -j
```

### 运行验证

- `tcpquic-proxy --help`。
- client/server 本机建连 smoke。
- DNS hostname 解析 smoke。
- 开启 zstd 的数据传输 smoke。
- QUIC 连接建立、断开、重连。
- 进程启动/退出循环 20 次，无崩溃。

### 诊断验证

临时计数器或调试日志确认：

- relay buffer 命中 `TqAllocBytes()` / `TqFreeBytes()`。
- zstd context 命中 `TqZstdAlloc()` / `TqZstdFree()`。
- c-ares 初始化后命中 `TqAresMalloc()` / `TqAresFree()` / `TqAresRealloc()`。
- MsQuic patch 阶段命中 `CxPlatAlloc()` / `CxPlatFree()` 的 mimalloc 分支。

诊断代码不得随正式功能一起提交，除非另行设计为稳定 metrics。

## 提交建议

建议按阶段拆分提交：

1. `build: disable all mimalloc global override modes`
2. `refactor: expand tcpquic explicit allocator api`
3. `perf: use explicit allocator hooks for zstd`
4. `perf: initialize c-ares with tcpquic allocator hooks`
5. `build: add msquic mimalloc allocator switch`
6. `third_party: route msquic platform allocations through mimalloc`
7. `docs: document explicit mimalloc allocator rollout`

如果阶段 5 无法安全接入 OpenSSL allocator，单独提交文档结论，不提交半成品 hook。

## 回滚策略

- 项目和 zstd/c-ares 阶段可通过 `-DTCPQUIC_USE_MIMALLOC=OFF` 回退到 libc allocator。
- MsQuic 阶段可通过 `-DTCPQUIC_MSQUIC_USE_MIMALLOC=OFF` 单独关闭 vendored patch；默认 `AUTO` 跟随最终 `TCPQUIC_USE_MIMALLOC`。
- quictls/OpenSSL 阶段本轮不接入。未来若接入，必须保留独立开关，不能成为不可关闭路径。
