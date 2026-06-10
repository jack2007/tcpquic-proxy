# Linux 环境验证记录

> **日期：** 2026-06-10  
> **基线提交：** `c0d2cfd`（源码目录重组 `source-file-organization` 合并后）  
> **验证环境：** Linux aarch64，GCC 13.3.0；双机 DGX Spark（本机 `169.254.250.230` ↔ 对端 `169.254.59.196`）  
> **状态：** ✅ 全部通过

### 历史基线

| 日期 | 提交 | 说明 |
|------|------|------|
| 2026-06-10 | `59580f6` | Windows IOCP 合并后首次 Linux 全量验证（含 relay 竞态修复） |
| 2026-06-10 | `c0d2cfd` | 源码目录重组后回归验证（`src/` 按职责分子目录，行为不变） |

---

## 1. 验证范围

| 类别 | 命令 / 内容 | 结果 |
|------|-------------|------|
| 单元测试 | `build/bin/Release/tcpquic_*_test`（全部 target） | PASS |
| 本机集成 | `./scripts/test-tcpquic-proxy.sh` | PASS |
| 本机并发 | `TUNNELS=2/10/100 ./scripts/test-tcpquic-concurrent.sh` | PASS |
| 双机冒烟 | `./scripts/test-tcpquic-proxy-dgx.sh` | PASS |
| 双机并发 | `TUNNELS=10/100 ./scripts/test-tcpquic-proxy-dgx.sh` | PASS |
| 双机 + 压缩 | `TUNNELS=100 COMPRESS=lz4 ./scripts/test-tcpquic-proxy-dgx.sh` | PASS |

---

## 2. 构建注意事项

### 2.1 LZ4 路径

若 CMake 缓存中 `LZ4_SOURCE_DIR` 被错误写入，配置时需显式指定：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLZ4_SOURCE_DIR="$PWD/third_party/lz4"
cmake --build build --target tcpquic-proxy -j$(nproc)
```

### 2.2 源码目录结构（重组后）

生产代码按职责分布在 `src/` 子目录：`acl/`、`config/`、`protocol/`、`platform/`、`ingress/`、`tunnel/`、`runtime/`。`main.cpp`、`CMakeLists.txt`、`unittest/` 仍在 `src/` 顶层。`src/CMakeLists.txt` 通过 `TCPQUIC_SOURCE_INCLUDE_DIRS` 集中管理 include 路径，源码中的短 include 名无需改动。

### 2.3 测试 target 链接

多个单元测试 target 需链接 `${TCPQUIC_PLATFORM_SOURCES}`，否则 Linux 平台 socket 符号未解析（见 `src/CMakeLists.txt`）。

---

## 3. 发现的问题与修复

### 3.1 Linux relay：`Stream->Send` 缓冲区生命周期

**现象：** HTTP CONNECT 返回 200 后 GET 无响应；客户端偶发 segfault。

**根因：** `TqLinuxRelaySendOperation` 将栈上 `QUIC_BUFFER` 指针交给 MsQuic；回调返回后内存失效。

**修复：** 在 send 操作中持久化 `QuicBuffers` 数组，于 `SEND_COMPLETE` 释放（`src/tunnel/linux_relay_worker.cpp`）。

### 3.2 Linux relay：误消费 tunnel OPEN 的 `SEND_COMPLETE`

**现象：** 并发 `TUNNELS≥2` 时 CONNECT 长时间无 HTTP 200（60s 超时）；`OpenSendComplete` 永不置位。

**根因：** `StartRelay()` 替换 stream 回调后，OPEN 帧的 `SEND_COMPLETE` 被 relay 回调当作 relay send 完成处理并 `free` 错误 context。

**修复：**

- `TqLinuxRelaySendOperation::MagicValue` 标记 relay 发送；非 magic 的 `SEND_COMPLETE` 直接 `free` 并忽略（`src/tunnel/linux_relay_worker.h/.cpp`）。
- 客户端 OPEN 路径：`PendingClientRelay` + `OpenSendComplete` + `FinishClientOpenAndStartRelay()`，仅在 OPEN 发送完成后再 `StartRelay()`（`src/tunnel/tcp_tunnel.cpp`）。

### 3.3 MsQuic 回调与锁：潜在死锁

**现象：** 并发隧道建立卡死。

**修复：** `SendBytes` / `StartRelay` 不在持有 `TqTunnelContext::Lock` 时调用 `Stream->Send` 或 `TqRelayStart`（`src/tunnel/tcp_tunnel.cpp`）。

### 3.4 Server OPEN 失败语义与 TCP 生命周期

**现象：** ACL 拒绝返回 `Internal(5)` 而非 `AclDenied(1)`；HTTP ACL 负路径返回空连接而非 403。

**修复：**

- `PendingOpenFailureShutdown`：OPEN_FAIL 发送完成后再 shutdown stream（`src/tunnel/tcp_tunnel.cpp`）。
- ACL / 拨号失败路径使用 `ReleaseTcpWithoutClose()`，由 proxy 层发送 HTTP 403 / SOCKS REP（`src/tunnel/tcp_tunnel.cpp`）。

### 3.5 SOCKS5 ACL 负路径

**现象：** ACL 拒绝时仍返回 REP=0（成功）。

**修复：** 先调用 `onTunnel`；失败再发送错误 REP（`src/ingress/socks5_server.cpp`）。

### 3.6 并发 QUIC 连接选取竞态

**现象：** 多路 CONNECT 同时建立时偶发失败。

**修复：** `clientTunnelStartMutex` / `PeerRuntime::TunnelStartMutex` **仅**保护 `EnsureConnected()` + `PickConnection()`，不在整个 `TqStartClientTunnel()`（含 OPEN 往返等待）期间持锁，避免双机高延迟下 100 路隧道排队超时（`src/main.cpp`，未随目录重组移动）。

### 3.7 测试脚本

| 脚本 | 变更 |
|------|------|
| `scripts/test-tcpquic-proxy.sh` | SOCKS ACL 负路径端口改为 `11881`（本机 `1081` 常被 `microsocks` 占用） |
| `scripts/test-tcpquic-concurrent.sh` | 启动前清理占用 `17001` 的残留 HTTP 进程 |

### 3.8 编译：`linux_relay_worker.cpp`

删除与 `platform_socket.h` 冲突的重复 `TqSetNonBlocking(int)` 定义。

---

## 4. 修改文件清单

```
scripts/test-tcpquic-concurrent.sh
scripts/test-tcpquic-proxy.sh
src/CMakeLists.txt
src/tunnel/linux_relay_worker.cpp
src/tunnel/linux_relay_worker.h
src/main.cpp
src/ingress/socks5_server.cpp
src/tunnel/tcp_tunnel.cpp
docs/linux-verification-2026-06-10.md   （本文档）
```

> 注：上述文件路径已在 `source-file-organization` 重组中迁移至子目录；逻辑未变。

---

## 5. 复现命令

```bash
# 本机
./scripts/test-tcpquic-proxy.sh
TUNNELS=100 ./scripts/test-tcpquic-concurrent.sh

# 双机（需 SSH 到 PEER，默认 jack@169.254.59.196）
./scripts/test-tcpquic-proxy-dgx.sh
TUNNELS=100 COMPRESS=lz4 ./scripts/test-tcpquic-proxy-dgx.sh
```

环境变量：`PEER`、`TARGET`、`BIND`、`REMOTE_DIR` 等见 `scripts/test-tcpquic-proxy-dgx.sh` 头部。

---

## 6. 后续建议

- **Schannel / 跨平台互操作：** Windows Schannel 凭证加载仍为独立 hardening 项；Linux ↔ Windows 互操作待专用环境验证。
- **互斥锁进一步收窄：** 若 MsQuic 确认同连接并发 `StreamOpen` 线程安全，可评估去掉 `PickConnection` 全局锁，改为 per-connection 锁。
- **CMake LZ4 缓存：** 调查 `LZ4_SOURCE_DIR` 被错误缓存的根因，避免每次手动 `-D`。
