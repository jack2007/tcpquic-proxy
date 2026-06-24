# macOS (Darwin) 平台问题调查

本文记录 2026-06-24 在 macOS arm64 上拉取最新代码、构建与验证时发现的两个测试失败及其调查结论。

**环境：** macOS arm64，Release 构建，Bash 5（Homebrew）用于运行 `scripts/test-tcpquic-proxy.sh`。

---

## 1. `tcpquic_darwin_relay_worker_io_test` 失败

### 现象

- 单元测试 `tcpquic_darwin_relay_worker_io_test` 在首个涉及 relay fd 生命周期的用例处失败：
  - `check failed at line 376`
  - 对应 `WorkerRegistersTcpReadinessShell` 中 `close(fds[0])`
- 部分运行表现为 **exit 141（SIGPIPE）**、无 stderr 输出，进程直接退出；与 `TcpErrorEventClosesAndRetiresRelay` 中向已关闭的一端 `write()` 有关。

### 根因

提交 **`7d2cefc`**（*fix relay fd ownership under persistent proxy tests*）在 `TqDarwinRelayWorker::RetireRelay()` 中增加了对 relay 所持有 `TcpFd` 的关闭，与 Linux relay 行为对齐：

```cpp
if (TqSocketValid(relay->TcpFd)) {
    TqCloseSocket(relay->TcpFd);
    relay->TcpFd = TqInvalidSocket;
}
```

同一提交已更新 **`linux_relay_worker_io_test.cpp`**：unregister 后断言 relay fd 为 `EBADF`，测试代码只关闭 socket pair 的 **peer fd**。

**Darwin 单元测试未同步更新**，仍在 `UnregisterRelay()` / `Stop()` 之后对 **relay 已接管的 fd** 再次 `close()`，触发 `EBADF`。

| 注册端 | Unregister/Stop 后 relay 关闭 | 测试应关闭 |
|--------|--------------------------------|------------|
| `fds[0]` | `fds[0]` | 仅 `fds[1]` |
| `fds[1]` | `fds[1]` | 仅 `fds[0]` |

### 附带问题：SIGPIPE

`TcpErrorEventClosesAndRetiresRelay` 在 EV_ERROR 使 relay 关闭 `fds[1]` 后，仍对 `fds[0]` 执行 `write()`。对端已关闭时 macOS 默认 **SIGPIPE** 终止进程（exit 141），不会打印 `check failed`。

### 建议修复

1. 增加辅助函数（与 Linux 测试语义一致）：
   - `CloseSocketPairAfterRelayOwned(relayTcpFd, fds)` — 断言 relay fd 已为 `EBADF`，只关闭 peer
   - `CloseSocketPairBoth(fds)` — 注册失败等 relay 未接管时使用
2. 将所有成功 register/unregister 用例的收尾 `close(fds[0/1])` 改为上述 helper
3. `TcpErrorEvent` 中对可能触发 SIGPIPE 的写操作改用 `send(..., MSG_NOSIGNAL)`

### 修复状态

- **本地已修复**（`src/unittest/darwin_relay_worker_io_test.cpp`）：helper + `MSG_NOSIGNAL`
- 修复后 `tcpquic_darwin_relay_worker_io_test` **全部通过**（exit=0）
- **尚未提交** upstream

---

## 2. 集成测试 zstd 端到端失败

### 现象

运行 `./scripts/test-tcpquic-proxy.sh`（需 Bash 4+，macOS 默认 Bash 3.2 不支持脚本中的 `exec {fd}<>`）：

- HTTP CONNECT / SOCKS5 主路径、负路径、端口转发均 **通过**
- **zstd 段失败：** `curl` 拉取 `large.txt` 超时（15s，0 bytes received）
- 目标 HTTP 日志可见 `GET /large.txt HTTP/1.1" 200`，说明 **client → server → target 上行正常**，**target → client → curl 下行无数据**

手动复现（client/server 均 `--compress zstd`，等待 QUIC 就绪后 curl）：

- 小文件 `small.txt` 同样 0 bytes / 超时
- `curl -v` 显示 CONNECT 与 GET 已发出，响应体始终为 0

### 压缩模式矩阵

| Server `--compress` | Client `--compress` | 结果 |
|---------------------|---------------------|------|
| off | off | OK |
| zstd | off | OK（open flags 协商为不压缩） |
| zstd | zstd | **FAIL** |
| off | zstd | **FAIL**（client open flags 请求 zstd，server relay 按 open flags 启用 zstd） |

**结论：** 问题由 **client open flags 启用 zstd** 触发；server CLI 的 `--compress` 不直接决定该条隧道是否压缩，server relay 使用 client 发来的 open flags。client `off` 时隧道内不走压缩，因此通过；client `zstd` 时双向 relay 都进入 zstd 路径，因此暴露 Darwin relay 的压缩/解压实现差异。

### 调试线索（`TQ_TUNNEL_DEBUG=1`）

典型一次失败请求的关键事件：

- `open_request_send` / `open_response_rx` / `open_complete` 正常
- `relay_started role=client ... fd=<ingress-fd> flags=0x1`（`TQ_FLAG_COMPRESS`）
- server 侧 `relay_started`、target `GET ... 200` 正常
- client 侧无数据写回 curl

### 根因（代码层）

#### 2.1 Darwin TCP → QUIC 压缩缺少 flush（直接触发因素）

Linux 的 `BuildTcpToQuicViews()` 在 `Compress(..., endStream=false)` 后如果 `CompressionOutput.empty()`，会调用 `Compressor::Flush()` 推出当前批次可解码的 zstd 块。

Darwin 的 `DrainTcpReadable()` 当前只调用 `Compress(..., endStream=false)`，随后直接把 `CompressionOutput` 拆成 QUIC send views；若 zstd 对小响应或 keep-alive 响应暂时没有输出，`SubmitTcpBatchToQuic(..., fin=false)` 会因 views 为空直接返回，导致下游永远收不到响应体。

这能解释 `python -m http.server` keep-alive 场景中 target 已返回 `200`，但 curl 侧 0 bytes 超时。

#### 2.2 Darwin 与 Linux 解压路径不一致（后续稳健性补强）

| | Linux | Darwin（当前） |
|---|--------|----------------|
| 压缩 QUIC 接收 | `DrainCompressedQuicReceiveView()` + `DecompressInto()` 增量流式解压 | `EnqueueQuicReceiveForTcp()` 内对每个 slice 批量 `Decompress()` |
| 空解压输出 | 正确处理 `NeedsMoreInput` / `NeedsMoreOutput`，按 consumed input 确认 QUIC receive | 若 `decompressed.empty()` 且 `remainingTcpWriteBytes == 0`，**错误地将整条 QUIC receive 标记为已完成** |
| 多 QUIC 包 | 同一 relay 上 decompressor 状态跨 view 延续 | 首包无即时 plaintext 时可能提前 ACK，后续压缩数据丢失 |

Darwin 单元测试 `CompressedQuicReceiveDecompressesToTcpAndCompletesCompressedBytes` 使用 **单帧完整 zstd 数据**，在旧批量路径下仍可通过，**无法覆盖**真实多包/流式场景。发送端 flush 修复后，当前 smoke/integration zstd 段已通过；但为长期对齐 Linux 行为，仍建议后续补齐增量解压路径。

Linux 参考实现：

- `TqLinuxRelayWorker::DrainCompressedQuicReceiveView()` — `src/tunnel/linux_relay_worker.cpp`
- `TqLinuxRelayWorker::FlushDeferredQuicReceives()` — 对 `PendingQuicReceives` 队列逐条 drain
- `TqLinuxRelayWorker::BuildTcpToQuicViews()` — 压缩输出为空时执行 `Flush()`

#### 2.3 zstd 结束帧与 HTTP keep-alive

Server relay 在 `DrainTcpReadable()` 中：

- 每次 TCP read：`Compress(..., endStream=false)`（`ZSTD_e_continue`）
- 仅当 `read == 0`（TCP EOF）：`Compress(nullptr, 0, ..., true)`（`ZSTD_e_end`）

集成测试使用 `python -m http.server`，响应后连接常 **keep-alive**，target TCP 不立即关闭，zstd 流可能长期缺少 end frame。实现不应依赖 EOF 才输出可解码数据；Darwin 应对齐 Linux，在非 EOF 批次也能 flush 出可解码块。

### 建议修复方向（按优先级）

1. **补齐 Darwin TCP → QUIC 压缩 flush**（已完成）
   - 对齐 Linux：`Compress(..., endStream=false)` 后若本批次无输出，调用 `Compressor::Flush()`
   - 覆盖小响应/keep-alive 场景，确保 zstd 数据不会长期停留在 compressor 内部
2. **移植 Linux 流式解压到 Darwin**（后续补强）
   - 实现 `DrainCompressedQuicReceiveView()` / `FlushDeferredQuicReceives()`
   - 使用 `DecompressInto()`，按 `InputConsumed` 分批 `ReceiveComplete`，避免 empty-output early-complete
3. 重跑 `./scripts/test-tcpquic-proxy.sh` zstd 段及 `tcpquic_darwin_relay_worker_io_test` 中压缩相关用例

### 修复状态

- **本地已修复直接触发因素**（`src/tunnel/darwin_relay_worker.cpp`）：Darwin TCP → QUIC 压缩批次在无即时输出时调用 `Compressor::Flush()`
- **已新增回归单测**（`src/unittest/darwin_relay_worker_io_test.cpp`）：`CompressedTcpReadFlushesWhenCompressorBuffersInput`
- 修复后 `tcpquic_darwin_relay_worker_io_test` **全部通过**（exit=0）
- 修复后 `/opt/homebrew/bin/bash ./scripts/test-tcpquic-proxy.sh` **全部通过**，包含 zstd 端到端段（exit=0）
- Darwin QUIC → TCP 流式解压仍未移植 Linux `DrainCompressedQuicReceiveView()`，建议作为后续稳健性工作单独推进

---

## 3. 其他 macOS 构建/测试说明

### Bash 版本

`scripts/test-tcpquic-proxy.sh` 使用 Bash 4+ 语法 `exec {fd}<>file`。macOS 自带 Bash 3.2 会报：

```text
exec: {fd}: not found
```

需使用 Homebrew Bash：

```bash
brew install bash
/opt/homebrew/bin/bash ./scripts/test-tcpquic-proxy.sh
```

### Clang `-Wformat-nonliteral`

上游在 `TqTunnelDebugLog()` 中使用非字面量 `vsnprintf` format，曾在 macOS Clang + `-Werror` 下编译失败。当前 `src/runtime/trace.cpp`、`src/tunnel/tcp_tunnel.cpp`、`src/ingress/client_ingress_reactor.cpp` 均已有局部 `#pragma clang diagnostic ignored "-Wformat-nonliteral"`，此项作为历史构建说明保留。

---

## 4. 相关文件

| 区域 | 路径 |
|------|------|
| Darwin relay 实现 | `src/tunnel/darwin_relay_worker.cpp`, `darwin_relay_worker.h` |
| Linux 流式解压参考 | `src/tunnel/linux_relay_worker.cpp` |
| Darwin relay 单测 | `src/unittest/darwin_relay_worker_io_test.cpp` |
| fd 所有权变更 | git `7d2cefc` |
| 集成测试脚本 | `scripts/test-tcpquic-proxy.sh` |
| 压缩协议/实现 | `src/protocol/compress.cpp`, `src/protocol/control_protocol.h`（`TQ_FLAG_COMPRESS`） |

---

## 5. 验证清单（修复后）

- [x] `build/bin/Release/tcpquic_darwin_relay_worker_io_test` — 全部通过
- [x] `/opt/homebrew/bin/bash ./scripts/test-tcpquic-proxy.sh` — 含 zstd 段
- [ ] 手动：`server/client --compress zstd`，curl HTTP CONNECT 拉取 ≥49KB `large.txt`
- [ ] 压缩矩阵四类组合行为符合 open flags 协商预期
