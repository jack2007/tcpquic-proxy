# tcpquic-proxy 构建指南

本文档按步骤说明 **tcpquic-proxy** 的构建依赖、CMake 配置与编译流程。项目使用 **CMake 3.20+**（msquic 静态集成要求），核心第三方库以 Git 子模块 vendored，**无需** 安装 `libzstd-dev`、`libssl-dev` 等系统开发包。

---

## 1. 构建产物概览

| 产物 | 路径 | 说明 |
|------|------|------|
| 主程序 | `build/bin/Release/raypx2` | client / server 代理二进制；CMake 目标名仍为 `tcpquic-proxy` |
| msquic 静态库（中间产物） | `build/msquic/bin/Release/libmsquic.a` | 单体归档库，已链入主程序，部署无需拷贝 |
| 单元测试 | `build/bin/Release/tcpquic_*_test` | 各模块单元测试可执行文件 |
| OpenSSL 辅助程序 | `build/bin/Release/openssl` | 从 vendored quictls 构建并复制，用于证书/诊断辅助场景 |
| Crashpad handler（可选） | `build/bin/Release/crashpad_handler` | `TCPQUIC_ENABLE_CRASHPAD=AUTO/ON` 且 Crashpad 可检测或可由本地 GN/Ninja 构建时生成 |
| 压测工具（可选） | `build/msquic/bin/Release/secnetperf` | msquic 自带性能工具（需 `-DQUIC_BUILD_PERF=ON`） |

默认构建下 `raypx2` **不依赖** `libmsquic.so`；`ldd` 仅显示 `libnuma`、`libstdc++`、`libc` 等系统库。未启用 Crashpad 时部署只需单个可执行文件；启用 Crashpad 时需同时部署 `crashpad_handler`。

若显式开启 `TCPQUIC_MSQUIC_SHARED=ON`，则会额外生成 `libmsquic.so*`，主程序通过 `$ORIGIN` RPATH 加载。

---

## 2. 依赖清单

### 2.1 系统工具（必须）

| 工具 | 最低版本 / 要求 | 用途 |
|------|-----------------|------|
| **Git** | — | 拉取子模块 |
| **CMake** | ≥ 3.20 | 构建系统（msquic 静态库路径要求 3.20+） |
| **C/C++ 编译器** | GCC 10+ 或 Clang（完整 C++17，含 `<filesystem>`） | 编译项目与 vendored 库 |
| **make** | — | msquic 编译 quictls 时使用 |
| **perl** | — | OpenSSL/quictls `Configure` 脚本 |
| **gn** | — | 启用 Crashpad 时生成 Crashpad Ninja build files |
| **ninja** | — | 启用 Crashpad 时构建 Crashpad 静态库和 handler |

**Debian / Ubuntu 示例：**

```bash
sudo apt install git cmake build-essential perl
```

**Amazon Linux 2 / 旧 RHEL 系：** 若默认 `g++` 仍为 GCC 7，需安装 GCC 10 工具链并显式指定编译器，例如 `/usr/bin/gcc10-gcc` 与 `/usr/bin/gcc10-g++`。

**当前开发机（Ubuntu 24.04）** 可直接使用系统自带的 GCC 13，无需 gcc10 别名。

### 2.2 Vendored 库（Git 子模块）

执行 `git submodule update --init --recursive` 后，CMake 会从源码构建以下组件：

| 组件 | 子模块路径 | 用途 | 当前 pin（示例） |
|------|-----------|------|------------------|
| [msquic](https://github.com/microsoft/msquic) | `third_party/msquic` | QUIC 协议栈 | `13c87cdb` |
| [quictls](https://github.com/quictls/openssl) | `third_party/msquic/submodules/quictls` | TLS（msquic 嵌套子模块，**首次构建从源码编译**） | `ff36838b` |
| [zstd](https://github.com/facebook/zstd) | `third_party/zstd` | 流式压缩 | `5233c58e` |
| [spdlog](https://github.com/gabime/spdlog) | `third_party/spdlog` | 日志 | `8671ca4d` |
| [c-ares](https://github.com/c-ares/c-ares) | `third_party/c-ares` | 异步 DNS 解析 | `c93e50f3` |
| [libuv](https://github.com/libuv/libuv) | `third_party/libuv` | 跨平台异步 I/O 库（静态链接进 `raypx2`） | `1cfa32ff`（v1.52.1） |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | `third_party/cpp-httplib` | Admin HTTP/1.1 server（header-only） | `9d159bb4` |
| [nlohmann/json](https://github.com/nlohmann/json) | `third_party/json` | JSON 配置、Admin API、测试工具 | `c363dc3e` |
| [Crashpad](https://github.com/jack2007/crashpad) | `third_party/crashpad` | 可选 crash dump 支持；项目 fork 已 vendored `lss`、`mini_chromium`、`zlib`、`googletest` 等 DEPS 源码 | `df2c143e` |

### mimalloc（third_party，默认静态启用）

- 路径：`third_party/mimalloc`（Git 子模块，与 msquic/zstd 相同）
- 初始化：`git submodule update --init --recursive third_party/mimalloc`
- 构建：`-DTCPQUIC_USE_MIMALLOC=ON`（默认）→ 静态链入 `mimalloc-static`，项目 allocator、zstd、c-ares 走显式 hook。
- MsQuic：`-DTCPQUIC_MSQUIC_USE_MIMALLOC=AUTO|ON|OFF` 控制 vendored MsQuic 平台层 allocator patch。默认 `AUTO` 跟随最终 `TCPQUIC_USE_MIMALLOC`；`ON` 要求 `TCPQUIC_USE_MIMALLOC=ON`；`OFF` 只关闭 MsQuic patch。
- 关闭：`-DTCPQUIC_USE_MIMALLOC=OFF` → 项目 allocator 回退 glibc `malloc`，MsQuic `AUTO` 也随之关闭。
- 部署：无需附带 `libmimalloc.so`

**不需要** 的系统包：`libzstd-dev`、`libssl-dev`、`libmsquic-dev`、`libuv1-dev` 等。Crashpad 的 `third_party/lss/lss`、`third_party/mini_chromium/mini_chromium`、`third_party/zlib/zlib`、`third_party/googletest/googletest` 来自 `jack2007/crashpad` fork 的普通源码提交；主仓库 CMake 不执行 `gclient sync`，也不下载 `depot_tools`。

### 2.3 运行时依赖

主程序链接：

- `libnuma`、`libstdc++`、`libgcc_s`、`libc`、`libm`（系统 glibc 环境）

msquic（含 quictls）、zstd、c-ares、libuv、spdlog、nlohmann/json、mimalloc 均已静态或 header-only 集成到 `raypx2`。**默认部署只需拷贝主程序**，无需 `libmsquic.so` 或 `libuv.so`；启用 Crashpad 时还需同目录拷贝 `crashpad_handler`。

若使用 `-DTCPQUIC_MSQUIC_SHARED=ON` 构建，则需同时部署 `libmsquic.so*`（或通过 RPATH 指向 `build/msquic/bin/Release/`）。

### 2.4 测试与脚本（不链入主二进制）

运行 `scripts/` 下集成测试时，宿主机还需：

| 命令 | 用途 |
|------|------|
| `openssl` | 生成测试用 CA 和 server 证书 |
| `curl` | HTTP CONNECT / SOCKS5 端到端验证 |
| `python3` | 诊断与压测脚本 |

---

## 3. 标准构建流程（Linux）

### Step 1：克隆仓库

```bash
git clone <repo-url> tcpquic-proxy
cd tcpquic-proxy
```

若已克隆但子模块为空，跳过 clone，直接进入 Step 2。

### Step 2：初始化子模块

```bash
git submodule update --init --recursive
```

此步骤会拉取 msquic、zstd、spdlog、c-ares、libuv、cpp-httplib、nlohmann/json、mimalloc、Crashpad 及 msquic 内部的 quictls 等嵌套子模块。**首次构建 quictls 耗时较长**（OpenSSL 源码编译）。

验证子模块：

```bash
git submodule status
```

应看到 `third_party/msquic`、`third_party/zstd`、`third_party/spdlog`、`third_party/c-ares`、`third_party/libuv`、`third_party/cpp-httplib`、`third_party/json`、`third_party/mimalloc`、`third_party/crashpad` 等均为已检出 commit，而非 `-` 前缀的空目录。

### Step 3：选择构建目录

**推荐** 使用仓库根目录下的 `build/`：

```bash
# 不要使用 build-gcc10/、build-asan/ 等历史临时目录作为主构建目录
# 源码或 CMakeLists 变更后，在 build/ 上重新 cmake 配置即可
```

如需 AddressSanitizer 调试构建，可使用独立目录 `build-asan/`（见 [§6 可选构建变体](#6-可选构建变体)）。

### Step 4：CMake 配置

**Ubuntu 24.04 / 现代 GCC（推荐）：**

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release
```

**Amazon Linux 2 / 需 GCC 10 的环境：**

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/gcc10-gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/gcc10-g++
```

配置阶段 CMake 会依次：

1. 检查 `third_party/msquic`、`third_party/zstd`、`third_party/spdlog`、`third_party/libuv`、`third_party/cpp-httplib`、`third_party/json`、`third_party/mimalloc` 是否存在；
2. 设置 `QUIC_TLS_LIB=quictls`，`QUIC_BUILD_SHARED=OFF`（可通过 `TCPQUIC_MSQUIC_SHARED=ON` 改回动态库）；
3. `add_subdirectory(third_party/msquic)` → 构建单体 `libmsquic.a`（含 quictls）；
4. `add_subdirectory(third_party/zstd/build/cmake)` → 构建 libzstd；
5. `add_subdirectory(third_party/spdlog)` → 构建 spdlog 静态库；
6. 定义 `cpp_httplib` header-only INTERFACE target；
7. `add_subdirectory(third_party/json)` → 定义 `nlohmann_json::nlohmann_json`；
8. `add_subdirectory(third_party/libuv)` → 只构建静态目标 `uv_a`（输出 `libuv.a`）；
9. 可选检查 Crashpad client/util/base/handler；若缺失且 `TCPQUIC_CRASHPAD_AUTO_BUILD=ON`，使用 fork vendored DEPS 和本机已有 `gn`/`ninja` 本地构建；
10. `add_subdirectory(src)` → 定义 `tcpquic-proxy` 构建目标、输出 `raypx2`，并定义单元测试目标。

libuv 集成使用上游官方跨平台 CMake。本次仅完成 Linux 实机验证，macOS 与 Windows
留待对应平台机器验证。

### Step 5：编译主程序

```bash
cmake --build build --target tcpquic-proxy -j$(nproc)
```

首次完整构建（含 quictls）可能需要 **数分钟到十几分钟**，取决于 CPU 与磁盘。

### Step 6：验证产物

```bash
ls -la build/bin/Release/raypx2
build/bin/Release/raypx2 --help
ldd build/bin/Release/raypx2
```

期望：

- 可执行文件存在且可运行；
- `ldd` **不出现** `libmsquic.so`（仅系统库如 `libnuma`）；
- 可选：确认 `build/msquic/bin/Release/libmsquic.a`、`build/bin/Release/openssl` 已生成；启用 Crashpad 时确认 `build/bin/Release/crashpad_handler` 已生成。

### Step 7（可选）：编译单元测试

```bash
cmake --build build --target help | grep '^... tcpquic_'
cmake --build build --target all -j$(nproc)
```

运行全部测试：

```bash
for t in build/bin/Release/tcpquic_*_test; do
  echo "==> $t"
  "$t" || exit 1
done
```

### Step 8（可选）：集成测试

```bash
./scripts/test-tcpquic-proxy.sh
```

---

## 4. CMake 构建拓扑

```
CMakeLists.txt (根)
├── third_party/msquic/          → libmsquic.a（默认）或 libmsquic.so（TCPQUIC_MSQUIC_SHARED=ON）
├── third_party/zstd/build/cmake → libzstd (static)
├── third_party/spdlog/          → spdlog (static)
├── third_party/cpp-httplib/     → cpp_httplib (header-only INTERFACE)
├── third_party/json/            → nlohmann_json::nlohmann_json (header-only)
├── third_party/libuv/           → uv_a / libuv.a（静态链接进 raypx2）
├── third_party/crashpad/        → 可选 Crashpad client/util/base/handler（fork vendored DEPS + 本地 GN/Ninja）
├── third_party/mimalloc/        → mimalloc-static（默认启用）
└── src/CMakeLists.txt
    ├── tcpquic-proxy            → 主程序目标，输出文件名 raypx2
    ├── tcpquic_*_test           → 单元测试
    └── tcpquic_tcp_sink_bench   → POSIX 压测辅助工具
```

**输出目录规则**（根 `CMakeLists.txt` 定义）：

- 可执行文件：`${CMAKE_BINARY_DIR}/bin/Release/`（Release）或 `bin/Debug/`（Debug）
- msquic 库：`${CMAKE_BINARY_DIR}/msquic/bin/Release/`

---

## 5. 主要 CMake 选项

### 5.1 项目级（根 CMakeLists.txt 已强制或默认）

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `TCPQUIC_MSQUIC_SHARED` | `OFF` | `ON` 时构建 `libmsquic.so` 动态库 |
| `QUIC_TLS_LIB` | `quictls`（强制） | TLS 后端；本项目统一 quictls |
| `QUIC_BUILD_TOOLS` | `OFF` | 不构建 msquic 示例工具 |
| `QUIC_ENABLE_LOGGING` | `ON` | msquic 日志 |
| `ZSTD_BUILD_PROGRAMS` | `OFF` | 不构建 zstd 命令行工具 |
| `ZSTD_BUILD_TESTS` | `OFF` | 不构建 zstd 测试 |
| `SPDLOG_BUILD_SHARED` | `OFF` | spdlog 静态库 |
| `LIBUV_BUILD_SHARED` | `OFF`（强制） | 不构建 libuv 共享库 |
| `LIBUV_BUILD_TESTS` | `OFF`（强制） | 不构建 libuv 测试 |
| `LIBUV_BUILD_BENCH` | `OFF`（强制） | 不构建 libuv benchmark |
| `TCPQUIC_USE_MIMALLOC` | `ON` | 静态链接 vendored mimalloc；ASAN 构建会自动关闭 |
| `TCPQUIC_MSQUIC_USE_MIMALLOC` | `AUTO` | 控制 vendored MsQuic 平台层 allocator patch |
| `TCPQUIC_ENABLE_CRASHPAD` | `AUTO` | Crashpad crash dump：`AUTO` / `ON` / `OFF` |
| `TCPQUIC_CRASHPAD_AUTO_BUILD` | `ON` | 缺少 Crashpad 产物时自动用 vendored 源码和本机已有 `gn`/`ninja` 本地构建；不会联网执行 `gclient sync` 或下载 `depot_tools` |
| `TCPQUIC_PREFER_STATIC_OPENSSL_HELPER` | `ON` | 非 Windows 下构建 vendored `openssl` helper 时优先尝试静态系统库 |

### 5.2 用户可覆盖的常用选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `CMAKE_BUILD_TYPE` | — | `Release` / `Debug` / `RelWithDebInfo` |
| `QUIC_USE_SYSTEM_LIBCRYPTO` | `OFF`（强制） | 固定使用 vendored quictls crypto，用户不应覆盖 |
| `QUIC_BUILD_PERF` | 视环境 | `ON` 时额外构建 `secnetperf` |
| `MSQUIC_SOURCE_DIR` | `third_party/msquic` | 自定义 msquic 源码路径 |
| `TCPQUIC_ZSTD_SOURCE_DIR` | `third_party/zstd` | 自定义 zstd 源码路径 |
| `TCPQUIC_LIBUV_SOURCE_DIR` | `third_party/libuv` | 自定义 libuv 源码路径 |

**恢复动态库构建（兼容旧部署脚本）：**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_MSQUIC_SHARED=ON
cmake --build build --target tcpquic-proxy -j$(nproc)
```

**示例：同时构建 secnetperf**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQUIC_BUILD_PERF=ON
cmake --build build --target secnetperf -j$(nproc)
```

---

## 6. 可选构建变体

### 6.1 Debug 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target tcpquic-proxy -j$(nproc)
# 产物：build/bin/Debug/raypx2
```

### 6.2 AddressSanitizer（内存调试）

使用独立构建目录，避免与 Release 产物混用：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"

cmake --build build-asan --target tcpquic-proxy tcpquic_tunnel_test -j$(nproc)
```

配置检测到 `-fsanitize=address`、`QUIC_ENABLE_ASAN=ON` 或 `QUIC_ENABLE_ALL_SANITIZERS=ON` 时会自动关闭显式 mimalloc 路径；ASAN 自带 allocator/interceptor 需要在进程启动早期接管 `malloc`。

运行 ASAN 二进制时，静态构建下无需设置 `LD_LIBRARY_PATH` 指向 msquic；若使用 `TCPQUIC_MSQUIC_SHARED=ON` 则需：

```bash
export LD_LIBRARY_PATH="$PWD/build-asan/msquic/bin/Release:${LD_LIBRARY_PATH}"
build-asan/bin/Release/raypx2 --help
```

### 6.3 增量重建

修改 `src/` 下源码后，通常只需：

```bash
cmake --build build --target tcpquic-proxy -j$(nproc)
```

修改 `CMakeLists.txt` 或子模块版本后，建议重新配置：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(nproc)
```

### 6.4 清理重建

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(nproc)
```

---

## 7. Windows 构建（简要）

**前置：** Visual Studio 2022（含 MSVC C++ 工作负载）、CMake、Git、Strawberry Perl。

本项目在 Windows 上同样使用 **quictls** 后端（`QUIC_TLS_LIB=quictls` 已强制），证书为 PEM 文件，与 Linux 一致。

```powershell
git submodule update --init --recursive
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release --target tcpquic-proxy
```

产物：`build-x64\bin\Release\raypx2.exe`。默认静态 msquic 构建不需要同目录 `msquic.dll`；仅 `-DTCPQUIC_MSQUIC_SHARED=ON` 时需要部署动态库。

验证：

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
```

---

## 8. 部署说明

**默认（静态 msquic）最小部署集：**

```text
deploy/
└── raypx2                     # 来自 build/bin/Release/
```

**动态库模式**（`-DTCPQUIC_MSQUIC_SHARED=ON`）额外需要：

```text
deploy/
├── raypx2
└── libmsquic.so*              # 来自 build/msquic/bin/Release/
```

**Crashpad 启用时**（`TCPQUIC_ENABLE_CRASHPAD=AUTO` 检测成功或显式 `ON`）还需同目录部署：

```text
deploy/
├── raypx2
└── crashpad_handler           # Windows 为 crashpad_handler.exe
```

通过 [wdtq](https://github.com/) 等工具启动时，可指定：

```bash
export TCPQUIC_PROXY_BIN=/path/to/build/bin/Release/raypx2
```

---

## 9. 常见问题

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| `msquic submodule missing` | 子模块未初始化 | `git submodule update --init --recursive` |
| `zstd submodule missing` | 同上 | 同上 |
| quictls 编译失败 | 缺少 perl / make | `sudo apt install perl build-essential` |
| `<filesystem>` 相关编译错误 | GCC 版本过低 | 使用 GCC 10+ 并 `-DCMAKE_CXX_COMPILER=...` |
| 修改 CMake 后行为异常 | 使用了过时 build 目录或缓存仍指向动态 msquic | 删除 `build/` 重新 `cmake -S . -B build` |
| 需要 `libmsquic.so` 的旧脚本失败 | 已切换静态默认 | 仅 rsync 二进制，或重建时加 `-DTCPQUIC_MSQUIC_SHARED=ON` |

---

## 10. 相关文档

| 文档 | 内容 |
|------|------|
| [README.md](README.md) | 项目概览、CLI、快速开始 |
| [docs/config_guide_cn.md](docs/config_guide_cn.md) | JSON 配置文件说明 |
| [docs/tls-cert.md](docs/tls-cert.md) | TLS 证书模式 |
| [docs/thread-model_cn.md](docs/thread-model_cn.md) | 线程模型 |
| [docs/admin-api/interface.md](docs/admin-api/interface.md) | Admin HTTP API |

---

## 快速命令备忘

```bash
# 完整首次构建（Linux Release）
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(nproc)

# 产物路径
./build/bin/Release/raypx2 --help
```
