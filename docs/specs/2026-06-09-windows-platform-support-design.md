# Windows 平台支持设计规格

> 状态：草案（待评审）  
> 日期：2026-06-09  
> 依据：x86 Win32 Release 构建探测（VS 2022 / CMake 3.30）  
> 关联：`docs/finished/specs/2026-06-06-tcpquic-proxy-design.md`

## 1. 背景与目标

### 1.1 现状

2026-06-09 在 x86 Windows（Win32 / MSVC 19.40）上执行完整构建：

| 阶段 | 结果 |
|------|------|
| 子模块初始化（msquic / lz4 / zstd + 递归） | 成功 |
| CMake 配置 | 成功（msquic 自动选用 Schannel，无需 perl/quictls 源码编译） |
| 依赖库编译 | 成功（`msquic.dll`、`lz4.lib`、`zstd_static.lib`） |
| `tcpquic-proxy` 主程序 | **失败** |

主程序失败根因：`src/` 深度依赖 POSIX 网络 API（`sys/socket.h`、`unistd.h`、`fcntl`、`poll`、`dup` 等），README 亦声明运行时仅支持 Linux/POSIX。

### 1.2 目标

1. **在 Windows x86/x64 上完成 `tcpquic-proxy` 二进制构建**，仅依赖项目源码及 `.gitmodules` 声明的子模块。
2. **保持 Linux 构建与行为不退化**（同一套源码，CI 双平台）。
3. **单元测试在 Windows 可编译运行**（至少与网络无关的测试先行，网络相关测试随平台层就绪逐步启用）。

### 1.3 非目标（v1 Windows 支持）

- 不支持 Windows 服务 / SCM 集成（命令行进程即可）
- 不支持 IOCP 高性能路径（先用 Winsock 阻塞/WSAPoll 对齐现有 poll 语义）
- 不支持 ARM64 Windows（后续扩展）
- 不引入第三方跨平台库（如 libuv、Boost.Asio）——保持零额外依赖

---

## 2. 方案对比

### 方案 A：统一平台抽象层（推荐）

新增 `tq_platform.h` / `tq_socket.h`，封装 socket 类型、生命周期、I/O、非阻塞 connect、socketpair 替代；业务模块仅调用 `Tq*` 接口，POSIX/Win32 差异集中实现。

| 优点 | 缺点 |
|------|------|
| 差异集中、可单测平台层 | 需新增 2–3 个文件 |
| 业务逻辑改动小（替换 include + API 名） | 初期需仔细对齐 errno/WSAGetLastError |
| 与现有 `acl.h` 的 `_WIN32` 分支一致、可扩展 | — |

### 方案 B：按模块拆分 `*_posix.cpp` / `*_win.cpp`

`tcp_dialer`、`relay` 等各自维护两套实现，CMake 按平台选源文件。

| 优点 | 缺点 |
|------|------|
| 各平台实现清晰隔离 | 重复逻辑多（send/recv/shutdown 等） |
| — | 13 个测试 + 12 个生产文件，维护成本高 |
| — | 行为漂移风险（修 Linux bug 忘同步 Windows） |

### 方案 C：逐文件 `#ifdef _WIN32` 内联（不推荐）

复制 `acl.h` 模式到每个 `.cpp`。

| 优点 | 缺点 |
|------|------|
| 无新文件 | 条件编译散落 12+ 文件，不可维护 |
| — | `dup`、`socketpair` 等语义差异难以内联表达 |

**推荐方案 A**：以最小抽象层覆盖全部 POSIX 网络调用，同时借 Windows 移植消除 `dup()` 依赖（见 §4.3），对 Linux 也是简化。

---

## 3. 架构设计

### 3.1 平台层模块

```
src/
  tq_platform.h      # NOMINMAX、头文件聚合、TqErrno、WSA 初始化声明
  tq_socket.h        # TqSocket 类型、INVALID、内联薄包装
  tq_socket_posix.cpp
  tq_socket_win32.cpp
```

**职责划分：**

| 模块 | 职责 |
|------|------|
| `tq_platform.h` | 全局宏（`NOMINMAX`、`WIN32_LEAN_AND_MEAN`）、`TqErrno()`、`TqWsaScope` RAII |
| `tq_socket.h` | 公开 API：`TqClose`、`TqSend`、`TqRecv`、`TqShutdown*`、`TqSetNonBlocking`、`TqPollOut`、`TqSocketPair`、`TqTuneTcpSocket` |
| `tq_socket_*.cpp` | 非内联实现：`TqDialTcp` 底层 connect 流程、socketpair 替代、错误码映射 |

### 3.2 Socket 类型策略

```cpp
#if defined(_WIN32)
using TqSocket = SOCKET;
constexpr TqSocket TqInvalidSocket = INVALID_SOCKET;
inline bool TqSocketValid(TqSocket s) { return s != INVALID_SOCKET; }
#else
using TqSocket = int;
constexpr TqSocket TqInvalidSocket = -1;
inline bool TqSocketValid(TqSocket s) { return s >= 0; }
#endif
```

**对外 API 迁移：** 现有 `int fd` 参数（`TqRelayStart`、`TqStartClientTunnel`、`TqDialResult::Fd` 等）统一改为 `TqSocket`，无效值用 `TqInvalidSocket`。Win32 x86 上 `SOCKET` 即 `UINT_PTR`，与 `int` 宽度兼容；Win64 需 typedef 避免截断。

### 3.3 错误码映射

| POSIX | Windows (WSA) | 用途 |
|-------|---------------|------|
| `EINPROGRESS` | `WSAEWOULDBLOCK`（connect 后） | 非阻塞 connect |
| `ECONNREFUSED` | `WSAECONNREFUSED` | 拨号失败分类 |
| `ETIMEDOUT` | `WSAETIMEDOUT` | 超时 |
| `EAGAIN` | `WSAEWOULDBLOCK` | 非阻塞 recv |
| `EINTR` | 无（WSAPoll 不中断） | poll 循环 |

封装 `TqGetSocketError()` 返回统一枚举或 int，供 `tcp_dialer` / `relay` 分支判断。

### 3.4 Winsock 生命周期

```cpp
// main.cpp 入口最早期
TqWsaScope wsa;  // 构造: WSAStartup(2,2); 析构: WSACleanup()
```

msquic 内部可能已调用 WSAStartup（引用计数），但项目代码直接使用 Winsock API，**显式 RAII 保证初始化顺序**，避免偶发 `WSANOTINITIALISED`。

### 3.5 数据流（不变）

Windows 移植不改变 QUIC 隧道语义，仅替换 OS 网络边界：

```
[SOCKS5/HTTP CONNECT listener]  --TqSocket-->
  [TunnelCore / Relay]            --msquic-->
  [QUIC Stream]                   --TqSocket-->
  [TCP dial / accept]
```

---

## 4. 关键移植点

### 4.1 API 对照表

| POSIX API | 使用位置 | Windows 实现 |
|-----------|----------|--------------|
| `close(fd)` | 全局 | `closesocket(s)` |
| `send` / `recv` | relay, servers, admin | 同名 Winsock（需 `ws2_32.lib`） |
| `MSG_DONTWAIT` | relay | 非阻塞 socket + `recv`；或忽略 flag |
| `MSG_NOSIGNAL` | relay, tcp_write_queue, admin | Windows 无 SIGPIPE，直接 `send` |
| `shutdown(SHUT_*)` | relay, tcp_write_queue | `shutdown(s, SD_SEND/SD_RECEIVE/SD_BOTH)` |
| `fcntl(O_NONBLOCK)` | tcp_dialer | `ioctlsocket(s, FIONBIO, &mode)` |
| `poll(POLLOUT)` | tcp_dialer | `WSAPoll` |
| `getsockopt(SO_ERROR)` | tcp_dialer | 同 API |
| `setsockopt(TCP_NODELAY, SO_*BUF)` | tcp_dialer, tuning | 同 API |
| `getaddrinfo` / `freeaddrinfo` | acl, tunnel, servers | 同 API（`ws2tcpip.h`） |
| `inet_pton` / `inet_ntop` | 多处 | 同 API（`Ws2_32.lib` + `ws2tcpip.h`） |
| `isatty(STDIN_FILENO)` | quic_session | `_isatty(_fileno(stdin))` + `<io.h>` |
| `socketpair(AF_UNIX)` | warmup, 3× unittest | `TqSocketPair()` loopback TCP（见 §4.4） |
| `dup(fd)` | socks5, http_connect | **消除**（见 §4.3） |
| `read`/`write` on socket | warmup | `recv`/`send` |

### 4.2 需改动的生产文件（12 个）

| 文件 | 改动要点 |
|------|----------|
| `tcp_dialer.h/.cpp` | 全部 POSIX connect 逻辑迁入平台层或 `#include "tq_socket.h"` |
| `tcp_tunnel.cpp` | 替换头文件；`close` → `TqClose` |
| `relay.cpp` | `MSG_*` flags 抽象；`shutdown` 常量 |
| `tcp_write_queue.cpp` | 同上 |
| `socks5_server.cpp` | 去 `dup`；统一 `TqCloseFd` 到平台层 |
| `http_connect_server.cpp` | 同上 |
| `admin_http.cpp` | Winsock server/client |
| `acl.cpp` | 移除 `arpa/inet.h`，依赖 `acl.h` 已选的 ws2 头 |
| `acl.h` | 保留现有分支，补充 `TQ_SHUTDOWN_*` 等若需要 |
| `warmup.cpp` | `TqSocketPair` + `TqSend`/`TqRecv` |
| `quic_session.cpp` | `isatty` 一行 |
| `main.cpp` | `TqWsaScope` |

**无需改动的生产文件（7 个）：** `config.cpp`、`control_protocol.cpp`、`compress.cpp`、`thread_pool.cpp`、`tunnel_reaper.cpp`、`router_runtime.cpp`、`server_metrics.cpp`、`tuning.cpp`（`config.cpp` 仅加 `(std::numeric_limits<>::max)()` 或全局 `NOMINMAX`）。

### 4.3 消除 `dup()` — 握手顺序调整（跨平台改进）

**现状（Linux）：**

```
onTunnel(tunnel, dup(clientFd))   // tunnel 可能已注册 relay
TqSendSocks5Reply(clientFd, ...)  // 仍在原 fd 写回复
TqCloseFd(clientFd)               // 关闭 dup 的另一端
```

`dup` 是为在 `onTunnel` 与 `SendReply` 之间保持两个独立 fd 引用。

**分析：** `TqStartClientTunnel` 在成功路径上仅发送 QUIC OPEN 帧，**不读取 TCP 数据**，直到 OPEN_OK 后 relay 才 recv。因此可在调用 `onTunnel` **之前**发送 SOCKS5/HTTP 成功响应。

**新顺序（Linux + Windows 统一）：**

```
TqSendSocks5Reply(clientFd, SUCCEEDED)   // 或 HTTP 200
if (failed) { TqClose(clientFd); return; }
onTunnel(tunnel, clientFd)               // 直接移交 fd 所有权，不再 dup
// clientFd 由 tunnel/relay 生命周期管理，此处不再 close
```

失败路径保持不变。此改动**减少一次 fd 泄漏风险**，且为 Windows 所必需。

### 4.4 `TqSocketPair()` 实现

POSIX `socketpair(AF_UNIX, SOCK_STREAM)` 用于 warmup 自测与单元测试 dummy fd。

**Windows 替代（loopback TCP）：**

1. `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`
2. `bind(127.0.0.1:0)` + `listen(1)`
3. `socket` + `connect` 到上述端口
4. `accept` 得到 `[0]`、`[1]` 一对互联 socket
5. 可选：`SetHandleInformation(INHERIT_NONE)` 模拟 `SOCK_CLOEXEC`

接口：

```cpp
bool TqSocketPair(TqSocket out[2]);  // 失败时 out[i] = TqInvalidSocket
```

### 4.5 MSVC 特有问题

| 问题 | 解决方案 |
|------|----------|
| `std::numeric_limits<T>::max()` 与 `max` 宏冲突 | 全局 `NOMINMAX`（`tq_platform.h` 在 `<windows.h>` 之前定义） |
| msquic 继承 `/WX`（警告视为错误） | `NOMINMAX` 即可消除 config.cpp 报错；其余 warnings 随平台层修复 |
| `SOMAXCONN` / `SHUT_*` 命名 | 在 `tq_socket.h` 提供 `TqShutRd` / `TqShutWr` / `TqShutBoth` 常量 |
| `socklen_t` | Windows 有定义（`ws2tcpip.h`），与 POSIX 兼容 |

---

## 5. CMake / 构建集成

### 5.1 顶层 `CMakeLists.txt`

无需大改；msquic 在 Windows 已自动走 Schannel。

### 5.2 `src/CMakeLists.txt` 变更

```cmake
# 平台源文件
if(WIN32)
  list(APPEND TCPQUIC_PLATFORM_SOURCES tq_socket_win32.cpp)
else()
  list(APPEND TCPQUIC_PLATFORM_SOURCES tq_socket_posix.cpp)
endif()

# 所有 executable 统一
target_sources(tcpquic-proxy PRIVATE ${TCPQUIC_PLATFORM_SOURCES})
target_compile_definitions(tcpquic-proxy PRIVATE
  $<$<BOOL:${WIN32}>:NOMINMAX> $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>)
if(WIN32)
  target_link_libraries(tcpquic-proxy PRIVATE ws2_32)
endif()
```

- 测试 target 同样链接 `ws2_32`（当前仅主程序经 msquic `base_link` 间接链接）。
- `BUILD_RPATH "$ORIGIN"` 保留（Linux 有效，Windows 无操作）。
- Windows 输出：`build/bin/Release/tcpquic-proxy.exe`；运行需同目录 `msquic.dll`（与 Linux `libmsquic.so` 一致）。

### 5.3 README 补充

新增 **Windows 构建** 小节：

```powershell
git submodule update --init --recursive
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32   # 或 -A x64
cmake --build build --config Release --target tcpquic-proxy
```

说明：

- 不需要 perl/make（Schannel 替代 quictls）
- 需要 VS 2022 + Windows SDK
- 依赖 DLL 与 exe 同目录部署

### 5.4 CI（建议）

GitHub Actions `windows-latest` matrix：`Win32` + `x64`，步骤 configure → build `tcpquic-proxy` → 运行无网络单元测试。

---

## 6. 单元测试策略

### 6.1 分级

| 级别 | 测试 target | Windows v1 |
|------|-------------|------------|
| L0 无 POSIX | control, compress, config_router, tuning, tunnel_reaper | 直接启用 |
| L1 仅 inet | acl_test | 改 include 后启用 |
| L2 socketpair | tcp_write_queue, thread_pool, tunnel | 依赖 `TqSocketPair` |
| L3 TCP client | admin_http_test | 改 Winsock client 后启用 |
| L4 链接 POSIX 模块 | http_connect_test, socks5_test, router_runtime_test | 随生产代码移植启用 |

### 6.2 测试辅助

可选 `src/unittest/tq_test_util.h`：封装 `TqLoopbackConnect()`、`TqCloseQuietly()`，减少各测试重复。

---

## 7. 实施阶段

```
Phase 1 — 构建基线（1–2 天）
  ├─ tq_platform.h / tq_socket.h / tq_socket_win32.cpp
  ├─ CMake: NOMINMAX + ws2_32 + 平台源
  ├─ main.cpp: TqWsaScope
  └─ 验证：依赖库 + 空壳 main 链接通过

Phase 2 — 网络核心（2–3 天）
  ├─ tcp_dialer 非阻塞 connect（WSAPoll）
  ├─ relay / tcp_write_queue I/O 抽象
  ├─ acl.cpp 头文件清理
  └─ 验证：server 模式编译通过

Phase 3 — 前端与 warmup（1–2 天）
  ├─ socks5 / http_connect / admin_http
  ├─ dup 消除 + 握手重排
  ├─ warmup TqSocketPair
  └─ 验证：client+server 二进制完整链接

Phase 4 — 测试与 CI（1–2 天）
  ├─ unittest 移植
  ├─ README Windows 文档
  └─ GitHub Actions windows job
```

**里程碑验收：** `cmake --build . --target tcpquic-proxy --config Release` 在 Win32/x64 零错误；L0+L1 测试 PASS；手动 SOCKS5 冒烟（可选）。

---

## 8. 风险与缓解

| 风险 | 缓解 |
|------|------|
| `WSAPoll` 在旧 Win 行为差异 | 最低目标 Win10；文档声明 |
| msquic `/WX` 引入新警告 | 平台层先 MSVC 本地编译；必要时 `target_compile_options` 仅对 tcpquic 关闭特定 warning |
| SOCKET 与 int 混用 | 全面 `TqSocket` typedef + `-Werror` 式 review |
| 握手重排改变时序 | 补充集成测试：SOCKS5/HTTP CONNECT 成功路径仍通 |
| Win ARM64 需求 | 明确为非目标，后续单独评估 |

---

## 9. 开放问题（评审时确认）

1. **架构优先级：** 仅 Win32 x86 还是 Win32 + x64 同步？（建议 x64 同步，成本低）
2. **CI 范围：** 仅编译还是包含 L0–L2 单元测试？
3. **握手重排：** 是否接受作为跨平台行为变更（推荐是，需回归 Linux SOCKS5/HTTP 测试）？

---

## 10. 参考

- x86 构建日志：`build-x86/build.log`
- 已有 Windows 分支：`src/acl.h`
- msquic Windows 文档：Schannel 自动启用，无需 quictls 源码构建
