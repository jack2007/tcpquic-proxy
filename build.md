# tcpquic-proxy 构建指南

本文档按步骤说明 **tcpquic-proxy** 的构建依赖、CMake 配置与编译流程。项目使用 **CMake 3.20+**（msquic 静态集成要求），核心第三方库以 Git 子模块 vendored，**无需** 安装 `libzstd-dev`、`libssl-dev` 等系统开发包。

---

## 1. 构建产物概览

| 产物 | 路径 | 说明 |
|------|------|------|
| 主程序 | `build/bin/Release/tcpquic-proxy` | client / server 代理二进制（**静态链入 msquic + quictls + zstd + c-ares + spdlog**） |
| msquic 静态库（中间产物） | `build/msquic/bin/Release/libmsquic.a` | 单体归档库，已链入主程序，部署无需拷贝 |
| 单元测试 | `build/bin/Release/tcpquic_*_test` | 各模块单元测试可执行文件 |
| 压测工具（可选） | `build/msquic/bin/Release/secnetperf` | msquic 自带性能工具（需 `-DQUIC_BUILD_PERF=ON`） |

默认构建下 `tcpquic-proxy` **不依赖** `libmsquic.so`；`ldd` 仅显示 `libnuma`、`libstdc++`、`libc` 等系统库。主程序体积约 6–7 MB（aarch64 Release），高于动态链入时的 ~2 MB，但部署只需单个可执行文件。

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
| [msquic](https://github.com/microsoft/msquic) | `third_party/msquic` | QUIC 协议栈 | `v2.5.2-24-g0efce2bc9` |
| [quictls](https://github.com/quictls/openssl) | `third_party/msquic/submodules/quictls` | TLS/mTLS（msquic 嵌套子模块，**首次构建从源码编译**） | 随 msquic 子模块 |
| [zstd](https://github.com/facebook/zstd) | `third_party/zstd` | 流式压缩 | `v1.5.7` |
| [spdlog](https://github.com/gabime/spdlog) | `third_party/spdlog` | 日志 | `v1.2.1-2576-gd1d1b6ff` |
| [c-ares](https://github.com/c-ares/c-ares) | `third_party/c-ares` | 异步 DNS 解析 | `v1.34.6` |

仓库中还包含 `third_party/lz4` 子模块，但**顶层 CMake 当前未引用**；主构建不依赖 lz4。

### mimalloc（third_party，默认静态启用）

- 路径：`third_party/mimalloc`（Git 子模块，与 msquic/zstd 相同）
- 初始化：`git submodule update --init --recursive third_party/mimalloc`
- 构建：`-DTCPQUIC_USE_MIMALLOC=ON`（默认）→ 静态链入 `mimalloc-static`
- 关闭：`-DTCPQUIC_USE_MIMALLOC=OFF` → `TqAllocBytes` 回退 glibc `malloc`
- 部署：无需附带 `libmimalloc.so`

**不需要** 的系统包：`libzstd-dev`、`libssl-dev`、`libmsquic-dev` 等。

### 2.3 运行时依赖

主程序链接：

- `libnuma`、`libstdc++`、`libgcc_s`、`libc`、`libm`（系统 glibc 环境）

msquic（含 quictls）、zstd、c-ares、spdlog 均已静态编入 `tcpquic-proxy`。**默认部署只需拷贝单个可执行文件**，无需 `libmsquic.so`。

若使用 `-DTCPQUIC_MSQUIC_SHARED=ON` 构建，则需同时部署 `libmsquic.so*`（或通过 RPATH 指向 `build/msquic/bin/Release/`）。

### 2.4 测试与脚本（不链入主二进制）

运行 `scripts/` 下集成测试时，宿主机还需：

| 命令 | 用途 |
|------|------|
| `openssl` | 生成测试用 mTLS 证书 |
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

此步骤会拉取 msquic、zstd、spdlog、c-ares 及 msquic 内部的 quictls 等嵌套子模块。**首次构建 quictls 耗时较长**（OpenSSL 源码编译）。

验证子模块：

```bash
git submodule status
```

应看到 `third_party/msquic`、`third_party/zstd`、`third_party/spdlog`、`third_party/c-ares` 等均为已检出 commit，而非 `-` 前缀的空目录。

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

1. 检查 `third_party/msquic`、`third_party/zstd`、`third_party/spdlog` 是否存在；
2. 设置 `QUIC_TLS_LIB=quictls`，`QUIC_BUILD_SHARED=OFF`（可通过 `TCPQUIC_MSQUIC_SHARED=ON` 改回动态库）；
3. `add_subdirectory(third_party/msquic)` → 构建单体 `libmsquic.a`（含 quictls）；
4. `add_subdirectory(third_party/zstd/build/cmake)` → 构建 libzstd；
5. `add_subdirectory(third_party/spdlog)` → 构建 spdlog 静态库；
6. `add_subdirectory(src)` → 定义 `tcpquic-proxy` 与单元测试目标。

### Step 5：编译主程序

```bash
cmake --build build --target tcpquic-proxy -j$(nproc)
```

首次完整构建（含 quictls）可能需要 **数分钟到十几分钟**，取决于 CPU 与磁盘。

### Step 6：验证产物

```bash
ls -la build/bin/Release/tcpquic-proxy
build/bin/Release/tcpquic-proxy --help
ldd build/bin/Release/tcpquic-proxy
```

期望：

- 可执行文件存在且可运行；
- `ldd` **不出现** `libmsquic.so`（仅系统库如 `libnuma`）；
- 可选：确认 `build/msquic/bin/Release/libmsquic.a` 已生成。

### Step 7（可选）：编译单元测试

```bash
cmake --build build --target \
  tcpquic_acl_test \
  tcpquic_admin_http_test \
  tcpquic_compress_test \
  tcpquic_config_router_test \
  tcpquic_control_test \
  tcpquic_http_connect_test \
  tcpquic_relay_buffer_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_relay_backend_selection_test \
  tcpquic_router_runtime_test \
  tcpquic_socks5_test \
  tcpquic_thread_pool_test \
  tcpquic_tuning_test \
  tcpquic_tunnel_reaper_test \
  tcpquic_tunnel_test \
  tcpquic_production_linkage_guard_test \
  -j$(nproc)
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
└── src/CMakeLists.txt
    ├── tcpquic-proxy            → 主程序
    ├── tcpquic_*_test           → 单元测试
    └── tcpquic_tcp_sink_bench   → Linux 压测辅助工具
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

### 5.2 用户可覆盖的常用选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `CMAKE_BUILD_TYPE` | — | `Release` / `Debug` / `RelWithDebInfo` |
| `QUIC_USE_SYSTEM_LIBCRYPTO` | `OFF` | `ON` 时 msquic 使用系统 libcrypto |
| `QUIC_BUILD_PERF` | 视环境 | `ON` 时额外构建 `secnetperf` |
| `MSQUIC_SOURCE_DIR` | `third_party/msquic` | 自定义 msquic 源码路径 |
| `TCPQUIC_ZSTD_SOURCE_DIR` | `third_party/zstd` | 自定义 zstd 源码路径 |

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
# 产物：build/bin/Debug/tcpquic-proxy
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

配置检测到 `-fsanitize=address` 时会自动关闭静态 mimalloc override；ASAN 自带 allocator/interceptor 需要在进程启动早期接管 `malloc`。

运行 ASAN 二进制时，静态构建下无需设置 `LD_LIBRARY_PATH` 指向 msquic；若使用 `TCPQUIC_MSQUIC_SHARED=ON` 则需：

```bash
export LD_LIBRARY_PATH="$PWD/build-asan/msquic/bin/Release:${LD_LIBRARY_PATH}"
build-asan/bin/Release/tcpquic-proxy --help
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

产物：`build-x64\bin\Release\tcpquic-proxy.exe`（需同目录 `msquic.dll`）。

验证：

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
```

---

## 8. 部署说明

**默认（静态 msquic）最小部署集：**

```text
deploy/
└── tcpquic-proxy              # 来自 build/bin/Release/
```

**动态库模式**（`-DTCPQUIC_MSQUIC_SHARED=ON`）额外需要：

```text
deploy/
├── tcpquic-proxy
└── libmsquic.so.2.5.9         # 来自 build/msquic/bin/Release/
```

通过 [wdtq](https://github.com/) 等工具启动时，可指定：

```bash
export TCPQUIC_PROXY_BIN=/path/to/build/bin/Release/tcpquic-proxy
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
| [src/README.md](src/README.md) | 源码结构摘要 |
| [docs/finished/specs/2026-06-06-tcpquic-proxy-design.md](docs/finished/specs/2026-06-06-tcpquic-proxy-design.md) | 设计规格 |

---

## 快速命令备忘

```bash
# 完整首次构建（Linux Release）
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(nproc)

# 产物路径
./build/bin/Release/tcpquic-proxy --help
```
