# tcpquic-proxy 跨平台移植：问题、目标与方案建议

> 状态：汇总文档（待评审）  
> 日期：2026-06-09  
> 来源：x86 Windows 构建探测、Windows 支持方案设计、跨平台网络库调研  
> 关联：  
> - [`2026-06-06-tcpquic-proxy-design.md`](2026-06-06-tcpquic-proxy-design.md) — 产品/协议设计  
> - [`2026-06-09-windows-platform-support-design.md`](2026-06-09-windows-platform-support-design.md) — 早期 Windows 自研抽象层草案（部分结论已被本文更新）

---

## 1. 背景

`tcpquic-proxy` 是 TCP-over-QUIC 隧道代理，当前 README 与实现均面向 **Linux/POSIX**（socket、pthread 等）。项目通过 Git 子模块 vendored 依赖 msquic、lz4、zstd。

2026-06-09 在 **x86 Windows** 上执行「仅基于项目代码与子模块、不修改源码」的构建探测，目的是发现 Win32 构建问题。随后讨论扩展至 **macOS、iOS、Android** 等多平台兼容，并调研 GitHub 现有跨平台 C++ 网络库。

---

## 2. 目标

### 2.1 短期目标（构建探测阶段）

| 目标 | 说明 |
|------|------|
| 验证子模块可构建性 | msquic / lz4 / zstd 在 Win32 上能否通过 CMake 编译 |
| 定位主程序阻塞点 | 记录编译/链接错误，**不主动修改项目代码** |
| 形成问题清单 | 为后续移植提供可操作的错误分类 |

### 2.2 中期目标（Windows 支持）

| 目标 | 说明 |
|------|------|
| Win32 / x64 可构建 | `cmake --build … --target tcpquic-proxy` 成功产出 `tcpquic-proxy.exe` |
| Linux 行为不退化 | 同一套源码，双平台 CI |
| 单元测试可移植 | 分阶段启用 L0–L4 测试（见 §5.4） |

### 2.3 长期目标（多平台路线图）

| 平台 | 优先级说明 |
|------|------------|
| Linux | 已有，保持 |
| Windows (x86/x64) | 中期首要 |
| macOS | 长期 |
| Android | 长期 |
| iOS | 长期 |

**约束：** 继续采用 vendored 子模块策略；避免引入与业务无关的重型框架。

---

## 3. 构建探测结果

### 3.1 环境与命令

| 项目 | 值 |
|------|-----|
| OS | Windows 10 x86 (Win32) |
| 编译器 | MSVC 19.40（VS 2022 Community） |
| CMake | 3.30.2 |
| Windows SDK | 10.0.26100.0 |
| 生成器 | `Visual Studio 17 2022 -A Win32` |

```powershell
git submodule update --init --recursive
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86 --config Release --target tcpquic-proxy
```

完整日志：`build-x86/build.log`（构建探测产物，未纳入版本库）。

### 3.2 各阶段结果

| 阶段 | 结果 | 说明 |
|------|------|------|
| 子模块初始化 | ✅ 成功 | lz4、zstd、msquic 及 msquic 递归子模块（quictls、clog 等） |
| CMake 配置 | ✅ 成功 | msquic 自动选择 **Windows / Schannel**，**无需 perl/make 编译 quictls** |
| lz4 编译 | ✅ 成功 | `build-x86/lz4/Release/lz4.lib` |
| zstd 编译 | ✅ 成功 | `build-x86/zstd/lib/Release/zstd_static.lib` |
| msquic 编译 | ✅ 成功 | `build-x86/msquic/bin/Release/msquic.dll` |
| tcpquic-proxy 主程序 | ❌ 失败 | 编译阶段报错，未生成 exe |

**结论：依赖链无问题；阻塞点在 `src/` 主程序源码的 POSIX 依赖。**

---

## 4. 问题清单

### 4.1 问题 1：源码深度依赖 POSIX 头文件与 API（主阻塞）

12 个生产 `.cpp` 无条件 `#include` Linux/POSIX 头文件。全项目仅 `acl.h` 有一处 `#if defined(_WIN32)` 分支（使用 `ws2tcpip.h`）。

**缺失头文件与涉及文件：**

| 缺失头文件 | 涉及文件（节选） |
|-----------|------------------|
| `sys/socket.h` | `tcp_dialer.h`, `relay.cpp`, `tcp_write_queue.cpp`, `socks5_server.cpp`, `http_connect_server.cpp`, `tcp_tunnel.cpp`, `admin_http.cpp`, `warmup.cpp` |
| `arpa/inet.h` | `acl.cpp`, `admin_http.cpp`, `warmup.cpp`, `http_connect_server.cpp`, `socks5_server.cpp`, `tcp_tunnel.cpp` |
| `unistd.h` | `quic_session.cpp`, `tcp_dialer.cpp`, `relay.cpp` 等 |
| `fcntl.h` / `poll.h` / `netinet/tcp.h` | `tcp_dialer.cpp` |

**关键 POSIX API 使用：**

| API | 用途 | Windows 差异 |
|-----|------|--------------|
| `fcntl` + `O_NONBLOCK` | 非阻塞 connect | `ioctlsocket(FIONBIO)` |
| `poll` | connect 完成等待 | `WSAPoll` |
| `close` | 关闭 fd | `closesocket` |
| `dup` | SOCKS5/HTTP 握手与隧道 fd 分离 | **Windows 不支持 socket dup** |
| `socketpair(AF_UNIX)` | warmup、单元测试 | 需 loopback TCP 或 pipe 替代 |
| `MSG_DONTWAIT` / `MSG_NOSIGNAL` | relay、tcp_write_queue | 需非阻塞 socket 或忽略 |
| `isatty(STDIN_FILENO)` | quic_session | `_isatty(_fileno(stdin))` |

**fd 类型问题：** 全局使用 `int fd`；Windows 上 `SOCKET` 为 `UINT_PTR`，Win64 可能截断。无效值 Linux 用 `-1`，Windows 用 `INVALID_SOCKET`。

### 4.2 问题 2：MSVC `max` 宏冲突

**位置：** `src/config.cpp:317`

```cpp
if (value > std::numeric_limits<uint32_t>::max()) {
```

**现象：** `warning C4003`（宏 `max` 参数不足）+ `error C2220`（警告视为错误，继承 msquic `/WX`）。

**原因：** Windows 头文件定义 `max` 宏，与 `std::numeric_limits::max()` 冲突。

**修复方向：** 全局 `NOMINMAX`，或写 `(std::numeric_limits<uint32_t>::max)()`。

### 4.3 问题 3：文档与 CMake 面向 Linux

- README 构建说明为 bash（`mkdir -p`、`nproc`、`$ORIGIN` RPATH）
- 声明「运行时依赖 Linux/POSIX」
- 构建工具列表含 perl/make（Windows + Schannel 路径不需要）
- `src/CMakeLists.txt` 中 `BUILD_RPATH "$ORIGIN"` 仅 Linux 有效

### 4.4 问题 4（信息性）：Windows 下 quictls 无需源码编译

README 写「首次构建从源码编译 quictls、需要 perl/make」。在 Win32 上 msquic 使用 **Schannel**，上述工具链**不适用**。属文档/platform 差异，非构建失败原因。

### 4.5 问题 5（长期）：msquic 官方平台支持有限

据 [msquic Release 文档](https://github.com/microsoft/msquic/blob/main/docs/Release.md)：

| 官方支持 | 架构 |
|----------|------|
| Windows | x64, arm64 |
| Linux | x64, arm64, arm32 |

macOS、iOS、Android、x86 等 **可能可用但未官方支持**。移动端 TLS（Secure Transport / BoringSSL / quictls）需单独规划。参见 [msquic#4041](https://github.com/microsoft/msquic/issues/4041)、社区 [swift-msquic](https://github.com/PADL/swift-msquic)。

**含义：** 即使 TCP 层完全跨平台，QUIC 层在 iOS/Android 上仍有独立工程风险。

### 4.6 编译错误汇总（Win32 探测）

```
C1083  无法打开包括文件 (sys/socket.h, unistd.h, arpa/inet.h)  → 9+ 处
C2220/C2589/C4003  max 宏冲突 (config.cpp:317)                  → 1 处
```

---

## 5. 解决方案建议

方案经历两阶段讨论：**阶段 A** 针对 Windows 的自研薄抽象层；**阶段 B** 纳入 macOS/iOS/Android 后转向 vendored 跨平台网络库。以下为综合建议。

### 5.1 架构原则

1. **TCP 网络 I/O 与 QUIC 解耦** — msquic 负责 QUIC；TCP 侧统一经平台层。
2. **保持现有线程模型** — 线程池 + 每隧道 relay 线程；第一阶段不强制全异步事件循环。
3. **消除 `dup()`** — 握手重排为跨平台改进，与库选型无关。
4. **vendored 子模块** — 与 msquic/lz4/zstd 策略一致。

### 5.2 推荐方案：libuv + 项目内 `tq_net` 薄封装

#### 为何不用纯自研 `tq_socket_win32/posix`

| 自研抽象层 | 问题 |
|-----------|------|
| 仅覆盖 Win + Linux | 每增 macOS/iOS/Android 需重复踩坑 |
| 12+ 文件 POSIX API | 分散维护，易行为漂移 |
| `dup` / `socketpair` / poll 语义 | 各 OS 差异大 |

#### 为何选 libuv

| 维度 | libuv |
|------|-------|
| GitHub | [libuv/libuv](https://github.com/libuv/libuv)（~26k stars，MIT） |
| 平台 | Linux/macOS/Windows Tier 1；Android Tier 3；iOS 社区生产案例 |
| 能力 | TCP/UDP、`uv_getaddrinfo`、epoll/kqueue/IOCP 抽象 |
| 集成 | C 库，易 `third_party/libuv` submodule + CMake |
| 与 msquic | 均为 C 风格平台层，层次清晰 |

可选 C++ 封装 [uvw](https://github.com/skypjack/uvw)（libuv 的 C++17 wrapper）；建议优先 **自研薄 `tq_net` 直接调用 libuv C API**，减少依赖层。

#### 架构示意

```
[Socks5 / HttpConnect / Relay / TcpDialer / ACL]
                    ↓
              tq_net.h（同步 API：Connect/Send/Recv/Listen/…）
                    ↓
              libuv（vendored 子模块）
                    ↓
    epoll / kqueue / IOCP / BSD sockets（各 OS）

[QuicSession / TunnelCore] ──→ msquic（独立，各平台 TLS 另规划）
```

#### POSIX → libuv / tq_net 映射

| 现有 POSIX | tq_net / libuv |
|-----------|----------------|
| `socket/connect/poll` | `uv_tcp_*` + 同步封装或 `uv_poll_t` |
| `getaddrinfo` | `uv_getaddrinfo` |
| `send/recv` | `uv_write`/`uv_read` 或同步包装 |
| `socketpair` | loopback TCP 或 `uv_pipe_t` |
| `close` | `uv_close` / `closesocket` |
| `dup(clientFd)` | **消除**：先 reply 再 `onTunnel`（见 §5.3） |

### 5.3 跨平台改进：消除 `dup()`

**现状：** SOCKS5/HTTP CONNECT 在 `onTunnel(dup(clientFd))` 之后才发送成功响应，因 Linux 需两个 fd 引用同一 socket。

**分析：** `TqStartClientTunnel` 成功路径仅发 QUIC OPEN，**不读 TCP**，故可在 `onTunnel` **之前**发送成功响应。

**建议顺序（Linux + Windows 统一）：**

```
1. TqSendSocks5Reply(clientFd, SUCCEEDED)   // 或 HTTP 200
2. if (failed) { TqClose(clientFd); return; }
3. onTunnel(tunnel, clientFd)               // 直接移交所有权，不再 dup
```

需 Linux 回归 SOCKS5/HTTP CONNECT 集成测试。

### 5.4 单元测试分级

| 级别 | 测试 target | 依赖 |
|------|-------------|------|
| L0 | control, compress, config_router, tuning, tunnel_reaper | 无网络，Win 可直接启用 |
| L1 | acl_test | inet API |
| L2 | tcp_write_queue, thread_pool, tunnel | socketpair 替代 |
| L3 | admin_http_test | TCP client |
| L4 | http_connect, socks5, router_runtime | 链接已移植网络模块 |

### 5.5 备选方案对比

| 方案 | 适用场景 | 不推荐原因 |
|------|----------|------------|
| **Asio standalone** [chriskohlhoff/asio](https://github.com/chriskohlhoff/asio) | 团队强偏好纯 C++17 头文件 | iOS/Android 文档弱；与 msquic 双异步模型；standalone 长期维护存疑 |
| **Sogou Workflow** [sogou/workflow](https://github.com/sogou/workflow) | 需要 HTTP/Redis 异步 DAG | 框架过重；与现有 SOCKS5/HTTP CONNECT 重复；iOS 不明确 |
| **POCO / libevent** | 全栈或 C 事件库 | 移动端生态或体积不如 libuv |
| **逐文件 `#ifdef _WIN32`** | — | 12+ 文件不可维护 |
| **按模块 `*_win.cpp` / `*_posix.cpp`** | — | 重复多、易漂移 |

### 5.6 CMake / 文档变更要点

**CMake：**

- 新增 `third_party/libuv` 子模块
- `add_subdirectory(third_party/libuv)`，链接 `uv_a` 或等价 target
- 全局 `NOMINMAX`（Windows）
- 测试 target 链接 ws2_32（若仍需要）
- 保留 Linux `BUILD_RPATH "$ORIGIN"`

**README：**

- 新增 Windows / macOS 构建小节
- 注明 Windows 下 msquic 用 Schannel，**不需要 perl/make**
- 更新「运行时平台」声明

---

## 6. 实施路线图

| 阶段 | 内容 | 验收 |
|------|------|------|
| **P0** | 选型确认；本文档评审通过 | 团队对齐 libuv + tq_net |
| **P1** | vendored libuv + `tq_net` 最小 API；消除 dup；NOMINMAX | Win32/x64 `tcpquic-proxy.exe` 链接成功 |
| **P2** | relay/dialer/servers 迁移至 tq_net；Linux 回归 | Linux + Windows CI 双绿 |
| **P3** | macOS 构建；L0–L2 单测 | macOS CI |
| **P4** | Android NDK 交叉编译 | `.so` 冒烟 |
| **P5** | iOS + msquic 移动端 TLS 方案 | 与 msquic 社区/issue 联动 |

**改动范围预估：**

| 类别 | 数量 |
|------|------|
| 新增 | `third_party/libuv`、`src/tq_net.h`、`src/tq_net.cpp`（及可选测试 util） |
| 需改生产文件 | 12 个网络相关 `.cpp/.h` |
| 无需改 | config/compress/thread_pool/tuning 等 7+ 文件 |
| 单元测试 | 5 直接 + 3 间接 POSIX 测试 |

---

## 7. 风险与缓解

| 风险 | 缓解 |
|------|------|
| libuv 偏异步，与现有同步 relay 不匹配 | `tq_net` 提供同步 API，内部封装 libuv |
| iOS 无 libuv 官方 Tier | 参考社区 Xcode 集成；关注 ARM64 修复（libuv PR #4414） |
| msquic 移动端未官方支持 | P5 单独立项；运行时 `IsSupported` 检测 |
| 握手重排改变时序 | Linux SOCKS5/HTTP 集成回归 |
| msquic `/WX` 引入新 MSVC 警告 | `NOMINMAX` + 必要时对 tcpquic target 单独设 warning |

---

## 8. 待决策项

| # | 问题 | 建议 |
|---|------|------|
| 1 | 网络库：libuv vs Asio standalone | **libuv** |
| 2 | 集成深度：仅 TCP 同步封装 vs 全 event loop | **仅同步封装**（第一阶段） |
| 3 | 架构：Win32 + x64 同步还是分阶段 | **同步** |
| 4 | 握手重排（去 dup）是否接受为跨平台行为变更 | **接受**（需 Linux 回归） |
| 5 | 移动端优先级：Android vs iOS 先后 | 待产品确认 |
| 6 | 早期 Windows 自研草案 [`2026-06-09-windows-platform-support-design.md`](2026-06-09-windows-platform-support-design.md) | 以本文为准，旧文档中「不引入 libuv」结论作废 |

---

## 9. 参考

| 资源 | 链接 |
|------|------|
| libuv 支持平台 | [SUPPORTED_PLATFORMS.md](https://github.com/libuv/libuv/blob/v1.x/SUPPORTED_PLATFORMS.md) |
| libuv 网络指南 | [docs.libuv.org networking](https://docs.libuv.org/en/stable/guide/networking.html) |
| Asio standalone | [think-async.com](http://think-async.com/Asio/AsioStandalone) |
| msquic 官方支持矩阵 | [docs/Release.md](https://github.com/microsoft/msquic/blob/main/docs/Release.md) |
| msquic Android 讨论 | [Issue #4041](https://github.com/microsoft/msquic/issues/4041) |
| 构建探测日志 | `build-x86/build.log`（本地，未提交） |

---

## 10. 文档修订记录

| 日期 | 变更 |
|------|------|
| 2026-06-09 | 初版：合并构建探测、Windows 方案、跨平台库调研与最终 libuv 建议 |
