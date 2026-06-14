# tcpquic-proxy 线程模型优化实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 依据 `src/tools/tcpquic-proxy/thread_model_cn.md` 第 9–10 节，降低高并发下 `quic_worker` 被阻塞 TCP 写拖慢的风险，并削减每隧道 detached 线程数量，同时保留 WAN 默认 `MAX_THROUGHPUT` 行为。

**Architecture:** 分四期落地，每期可独立合并：(1) 每隧道 QUIC→TCP 异步 writer 队列，MsQuic 回调只做拷贝入队；(2) 全局 `TqTunnelReaper` 替代每隧道 cleanup watcher；(3) 固定大小 accept/handshake 线程池替代每连接 `detach`；(4) `--quic-profile` 在 `MAX_THROUGHPUT` 与 `LOW_LATENCY` 间切换。`relay` TCP→QUIC epoll 多路复用留作后续 Phase 5，本计划不实现。

**Tech Stack:** C++17, MsQuic C++ API, 现有 assert 风格单元测试, `scripts/test-tcpquic-proxy.sh` / `scripts/test-tcpquic-concurrent.sh`

**Spec 来源:** `src/tools/tcpquic-proxy/thread_model_cn.md` §9–10

**不变量（已正确，计划内仅加回归守护）：**
- 默认 Registration profile 仍为 `QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT`
- server OPEN 阶段 DNS/TCP connect 仍在 detached dial worker，不在 MsQuic 回调内阻塞

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `tcp_write_queue.h/cpp` | 有界 SPSC 队列 + writer 线程循环，封装 `TqWriteAll` |
| `tunnel_reaper.h/cpp` | 全局单例 reaper，注册/注销待清理 `TqTunnelContext` |
| `thread_pool.h/cpp` | 固定 worker 队列，供 SOCKS5/HTTP CONNECT handler 复用 |
| `relay.cpp` | `OnStreamReceive` 改入队；`Start` 启动 writer；`Stop` join writer |
| `tcp_tunnel.cpp` | 删除 `StartCleanupWatcher`；改向 reaper 注册 |
| `socks5_server.cpp` / `http_connect_server.cpp` | accept 后提交线程池而非 `detach` |
| `config.h/cpp` | 新增 `--quic-profile`、`--handshake-threads` |
| `quic_session.cpp` | Registration profile 从 config 读取 |
| `main.cpp` | 启动/停止 reaper 与 handshake 线程池 |
| `unittest/tcp_write_queue_test.cpp` | 队列与 writer 行为单测 |
| `unittest/tunnel_reaper_test.cpp` | reaper 注册/清理单测 |
| `unittest/thread_pool_test.cpp` | 线程池投递/背压单测 |

---

### Task 1: QUIC→TCP 异步写队列（模块 + 单测）

**Files:**
- Create: `src/tools/tcpquic-proxy/tcp_write_queue.h`
- Create: `src/tools/tcpquic-proxy/tcp_write_queue.cpp`
- Create: `src/tools/tcpquic-proxy/unittest/tcp_write_queue_test.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`

**设计要点：**
- 每隧道一个 `TqTcpWriteQueue`，MsQuic `RECEIVE` 回调线程只 `Enqueue`（拷贝 payload），立即返回
- 独立 `WriterThread` 阻塞 `TqWriteAll`；写失败或 FIN 时设置 `stop` 并 `shutdown(TcpFd)`
- 队列默认容量 64 条或 4MiB 总字节（取先到者），满时回调线程短暂等待（`wait_for` 10ms）后丢弃并 abort stream，避免无限占用 `quic_worker`

- [ ] **Step 1: 写失败单测（socketpair 读端关闭）**

```cpp
// unittest/tcp_write_queue_test.cpp
#include "../tcp_write_queue.h"
#include <cassert>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main() {
    int fds[2]{-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ::close(fds[1]); // 写端立即 EPIPE

    std::atomic<bool> stopped{false};
    TqTcpWriteQueue q(fds[0], &stopped, 16, 1 << 20);
    assert(q.Start());

    std::vector<uint8_t> payload(128, 0xAB);
    assert(q.Enqueue(payload.data(), payload.size(), false));
    assert(q.WaitUntilDrainedOrStopped(2000));

    assert(stopped.load());
    q.Stop();
    ::close(fds[0]);
    return 0;
}
```

- [ ] **Step 2: 运行单测确认失败**

Run: `cmake --build build-iouring --target tcpquic_tcp_write_queue_test -j$(nproc) && ./build-iouring/bin/Release/tcpquic_tcp_write_queue_test`
Expected: 链接失败 `tcp_write_queue.h: No such file`

- [ ] **Step 3: 实现 `tcp_write_queue.h/cpp`**

```cpp
// tcp_write_queue.h
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct TqTcpWriteChunk {
    std::vector<uint8_t> Data;
    bool Fin{false};
};

class TqTcpWriteQueue {
public:
    TqTcpWriteQueue(int tcpFd, std::atomic<bool>* stopFlag, size_t maxChunks, size_t maxBytes);
    bool Start();
    void Stop();
    bool Enqueue(const uint8_t* data, size_t len, bool fin);
    bool WaitUntilDrainedOrStopped(int timeoutMs);

private:
    void WriterLoop();
    bool WriteAll(const uint8_t* data, size_t len);

    int TcpFd;
    std::atomic<bool>* StopFlag;
    size_t MaxChunks;
    size_t MaxBytes;
    std::thread Writer;
    std::mutex Lock;
    std::condition_variable Cv;
    std::deque<TqTcpWriteChunk> Queue;
    size_t QueuedBytes{0};
    bool WriterRunning{false};
};
```

```cpp
// tcp_write_queue.cpp — WriterLoop 核心
void TqTcpWriteQueue::WriterLoop() {
    while (!StopFlag->load()) {
        TqTcpWriteChunk chunk;
        {
            std::unique_lock<std::mutex> lock(Lock);
            Cv.wait(lock, [&] { return !Queue.empty() || StopFlag->load(); });
            if (Queue.empty()) continue;
            chunk = std::move(Queue.front());
            QueuedBytes -= chunk.Data.size();
            Queue.pop_front();
        }
        if (!chunk.Data.empty() && !WriteAll(chunk.Data.data(), chunk.Data.size())) {
            StopFlag->store(true);
            ::shutdown(TcpFd, SHUT_RDWR);
            break;
        }
        if (chunk.Fin) {
            StopFlag->store(true);
            ::shutdown(TcpFd, SHUT_WR);
            break;
        }
        Cv.notify_all();
    }
}
```

- [ ] **Step 4: CMake 注册 `tcpquic_tcp_write_queue_test` 并运行通过**

- [ ] **Step 5: Commit**

```bash
git add src/tools/tcpquic-proxy/tcp_write_queue.* \
        src/tools/tcpquic-proxy/unittest/tcp_write_queue_test.cpp \
        src/tools/tcpquic-proxy/CMakeLists.txt
git commit -m "feat(tcpquic-proxy): add async TCP write queue for QUIC receive path"
```

---

### Task 2: relay 接入异步 writer

**Files:**
- Modify: `src/tools/tcpquic-proxy/relay.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`（`tcpquic-proxy` 链接 `tcp_write_queue.cpp`）

- [ ] **Step 1: `TqTunnelRelay` 增加 `std::unique_ptr<TqTcpWriteQueue> TcpWriter`**

- [ ] **Step 2: `Start()` 在 `TcpThread` 之前启动 writer**

```cpp
TcpWriter = std::make_unique<TqTcpWriteQueue>(TcpFd, &Handle->Stop, 64, 4u << 20);
if (!TcpWriter->Start()) {
    DetachStreamCallback();
    return false;
}
TcpThread = std::thread(&TqTunnelRelay::TcpToStreamLoop, this);
```

- [ ] **Step 3: 改写 `OnStreamReceive` — 解压后入队，不再直接 `TqWriteAll`**

```cpp
QUIC_STATUS TqTunnelRelay::OnStreamReceive(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept {
    if (TcpWriter == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
    if (Decompressor != nullptr) {
        std::vector<uint8_t> output;
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            if (!Decompressor->Decompress(buffer.Buffer, buffer.Length, output)) {
                Handle->Stop.store(true);
                ::shutdown(TcpFd, SHUT_RDWR);
                stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
                return QUIC_STATUS_SUCCESS;
            }
        }
        if (!output.empty() && !TcpWriter->Enqueue(output.data(), output.size(), false)) {
            Handle->Stop.store(true);
            stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
    } else {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            if (buffer.Length > 0 &&
                !TcpWriter->Enqueue(buffer.Buffer, buffer.Length, false)) {
                Handle->Stop.store(true);
                stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
                break;
            }
        }
    }
    if (fin) {
        TcpWriter->Enqueue(nullptr, 0, true); // FIN marker
    }
    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 4: `Stop()` 在 join `TcpThread` 前 `TcpWriter->Stop()`**

- [ ] **Step 5: 删除匿名命名空间内仅 relay 使用的 `TqWriteAll`（逻辑已迁入 `tcp_write_queue.cpp`）**

- [ ] **Step 6: 构建 + smoke**

Run:
```bash
cmake --build build-iouring --target tcpquic-proxy tcpquic_tcp_write_queue_test -j$(nproc)
./scripts/test-tcpquic-proxy.sh
```
Expected: HTTP CONNECT + SOCKS5 PASS

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(tcpquic-proxy): move QUIC-to-TCP writes off quic_worker callback thread"
```

---

### Task 3: 全局 Tunnel Reaper

**Files:**
- Create: `src/tools/tcpquic-proxy/tunnel_reaper.h`
- Create: `src/tools/tcpquic-proxy/tunnel_reaper.cpp`
- Create: `src/tools/tcpquic-proxy/unittest/tunnel_reaper_test.cpp`
- Modify: `src/tools/tcpquic-proxy/tcp_tunnel.cpp`
- Modify: `src/tools/tcpquic-proxy/tcp_tunnel.h`（如前向声明 reaper 接口）
- Modify: `src/tools/tcpquic-proxy/main.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`

**设计要点：**
- `TqTunnelReaper::Instance()` 单后台线程，100ms 轮询或 `condition_variable` 唤醒
- `TqTunnelContext` relay 启动后调用 `TqTunnelReaper::Register(ctx)`，不再 `StartCleanupWatcher`
- reaper 观察到 `ctx->RelayHandle.Stop` 后执行 `TqRelayStop` → `CloseTcp` → `delete ctx`

- [ ] **Step 1: 写 reaper 单测（mock context）**

```cpp
// unittest/tunnel_reaper_test.cpp
struct FakeTunnel {
    std::atomic<bool> Stop{false};
};
// Register fake, set Stop=true, assert OnReaped callback fired within 2s
```

- [ ] **Step 2: 实现 `tunnel_reaper.h/cpp`**

```cpp
// tunnel_reaper.h
#pragma once
struct TqTunnelContext;

class TqTunnelReaper {
public:
    static TqTunnelReaper& Instance();
    void Start();
    void Stop();
    void Register(TqTunnelContext* ctx);
    void Unregister(TqTunnelContext* ctx);

private:
    void ReaperLoop();
    // mutex + vector<TqTunnelContext*> + thread + cv
};
```

- [ ] **Step 3: 删除 `TqTunnelContext::StartCleanupWatcher` 及其调用点**

在 `ArmSelfDeleteOnShutdown()` 或 `StartRelay()` 成功后改为：
```cpp
TqTunnelReaper::Instance().Register(this);
```

- [ ] **Step 4: `main.cpp` client/server 启动时 `Reaper::Start()`，进程退出前 `Reaper::Stop()`**

- [ ] **Step 5: 运行单测 + smoke**

Run:
```bash
cmake --build build-iouring --target tcpquic_tunnel_reaper_test tcpquic-proxy -j$(nproc)
./build-iouring/bin/Release/tcpquic_tunnel_reaper_test
./scripts/test-tcpquic-proxy.sh
```

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(tcpquic-proxy): replace per-tunnel cleanup watcher with global reaper"
```

---

### Task 4: Accept / Handshake 线程池

**Files:**
- Create: `src/tools/tcpquic-proxy/thread_pool.h`
- Create: `src/tools/tcpquic-proxy/thread_pool.cpp`
- Create: `src/tools/tcpquic-proxy/unittest/thread_pool_test.cpp`
- Modify: `src/tools/tcpquic-proxy/config.h`, `config.cpp`
- Modify: `src/tools/tcpquic-proxy/socks5_server.cpp`
- Modify: `src/tools/tcpquic-proxy/http_connect_server.cpp`
- Modify: `src/tools/tcpquic-proxy/main.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`

- [ ] **Step 1: config 新增字段与 CLI**

```cpp
// config.h
uint32_t HandshakeThreads = 8; // 0 = hardware_concurrency, capped 64
```

```
--handshake-threads <n>   SOCKS5/HTTP 握手 worker 数（默认 8，0=自动）
```

- [ ] **Step 2: 实现固定大小 `TqThreadPool`**

```cpp
class TqThreadPool {
public:
    explicit TqThreadPool(uint32_t workers);
    void Start();
    void Stop();
    void Submit(std::function<void()> fn);
};
```

- [ ] **Step 3: `main.cpp` 创建全局 pool，`RunClient` 注入**

```cpp
static TqThreadPool* gHandshakePool = nullptr;
// RunClient 开头:
gHandshakePool = new TqThreadPool(cfg.HandshakeThreads);
gHandshakePool->Start();
```

- [ ] **Step 4: 替换 detach**

```cpp
// socks5_server.cpp — 原:
// std::thread(TqHandleSocks5Client, clientFd, onTunnel).detach();
gHandshakePool->Submit([clientFd, onTunnel] {
    TqHandleSocks5Client(clientFd, onTunnel);
});
```

对 `http_connect_server.cpp` 做同样替换。

- [ ] **Step 5: 线程池单测 + smoke**

Run:
```bash
cmake --build build-iouring --target tcpquic_thread_pool_test tcpquic-proxy -j$(nproc)
./build-iouring/bin/Release/tcpquic_thread_pool_test
./scripts/test-tcpquic-proxy.sh
```

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(tcpquic-proxy): use handshake thread pool instead of per-accept detach"
```

---

### Task 5: QUIC Execution Profile 可配置

**Files:**
- Modify: `src/tools/tcpquic-proxy/config.h`, `config.cpp`
- Modify: `src/tools/tcpquic-proxy/quic_session.cpp`
- Modify: `src/tools/tcpquic-proxy/unittest/tuning_test.cpp`（或新建 `config_test.cpp` 解析 profile）
- Modify: `src/tools/tcpquic-proxy/main.cpp`（启动日志打印 profile）

- [ ] **Step 1: 新增枚举与 CLI**

```cpp
enum class TqQuicProfile { MaxThroughput, LowLatency };

// config.h
TqQuicProfile QuicProfile{TqQuicProfile::MaxThroughput};
```

```
--quic-profile <max-throughput|low-latency>   默认 max-throughput
```

- [ ] **Step 2: 解析函数**

```cpp
QUIC_EXECUTION_PROFILE_TYPE TqToMsQuicProfile(TqQuicProfile p) {
    switch (p) {
    case TqQuicProfile::LowLatency:
        return QUIC_EXECUTION_PROFILE_TYPE_LOW_LATENCY;
    default:
        return QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT;
    }
}
```

- [ ] **Step 3: `InitApiAndRegistration` 接受 profile 参数**

```cpp
bool InitApiAndRegistration(
    std::unique_ptr<MsQuicApi>& api,
    std::unique_ptr<MsQuicRegistration>& registration,
    QUIC_EXECUTION_PROFILE_TYPE profile);
```

`QuicClientSession::Start` / `QuicServerSession::Start` 传入 `TqToMsQuicProfile(cfg.QuicProfile)`。

- [ ] **Step 4: 单测验证解析 + 默认仍为 max-throughput**

- [ ] **Step 5: smoke 两种 profile**

Run:
```bash
# 默认
./scripts/test-tcpquic-proxy.sh
# low-latency 手动 spot-check（可选环境变量 QUIC_PROFILE=low-latency 扩展脚本）
```

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(tcpquic-proxy): add --quic-profile for MAX_THROUGHPUT vs LOW_LATENCY"
```

---

### Task 6: 并发回归与线程数验证

**Files:**
- Modify: `scripts/test-tcpquic-concurrent.sh`（可选：打印 `/proc/$pid/status` 的 Threads 字段）
- Modify: `research_progress.md`（记录优化前后线程估算）
- Modify: `src/tools/tcpquic-proxy/thread_model_cn.md` §10（标记已实现项）

- [ ] **Step 1: 100 隧道并发回归**

Run:
```bash
TUNNELS=100 MAX_MEMORY_MB=512 ./scripts/test-tcpquic-concurrent.sh
```
Expected: 全部隧道 OPEN 成功

- [ ] **Step 2: 对比线程数（优化前后）**

```bash
# server/client 启动后
grep Threads /proc/$(pgrep -n tcpquic-proxy)/status
```
记录：优化前约 `1 + 2N + 2L`（client 再加 2 listener + A handler），优化后应减少约 `L`（cleanup watcher）+ 短连接 handler 波动。

- [ ] **Step 3: 慢后端阻塞实验（验证 quic_worker 不再卡在 send）**

```bash
# 终端1: 启动只收不读的 nc 后端
nc -l 127.0.0.1 9 &
# 终端2: 通过隧道灌入大流量，同时用 gdb 对 quic_worker 线程采样
# 期望: OnStreamReceive 栈深度极浅（入队即返），阻塞 send 出现在 TcpWriteQueue::WriterLoop
```

- [ ] **Step 4: 更新 `thread_model_cn.md` 第 10 节状态表**

- [ ] **Step 5: Commit**

```bash
git commit -m "docs(tcpquic-proxy): record thread model optimization results"
```

---

## 验收标准

| 指标 | 目标 |
|------|------|
| `test-tcpquic-proxy.sh` | 始终 PASS |
| `test-tcpquic-concurrent.sh` TUNNELS=100 | PASS |
| 100 隧道稳定态应用线程 | 比优化前少约 100（无 per-tunnel cleanup watcher） |
| `OnStreamReceive` 回调 | 不做阻塞 `send()`；写操作仅在 writer 线程 |
| 默认 profile | 仍为 `MAX_THROUGHPUT`，行为与优化前 smoke 一致 |
| server OPEN | DNS/dial 仍在 detached worker，MsQuic 回调无 connect |

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 写队列满导致丢包 | 有界队列 + 满时 abort stream；容量与 `RelayIoSize` 对齐调参 |
| reaper 与 relay 竞态 | `TqRelayStop` 前先 `Unregister`；`Stop` 标志用 atomic |
| 线程池任务堆积 | 默认 8 worker；后续可加队列深度指标日志 |
| `LOW_LATENCY` 降低吞吐 | 文档说明场景；默认不变 |

## 明确不在本计划范围

- relay TCP→QUIC 改 epoll 多路复用（`thread_model_cn.md` 低优先级项）
- 修改 MsQuic 内部 worker 数量
- 改变 1 TCP = 1 QUIC Stream 语义

---

## Self-Review（计划自检）

| Spec 条目 | 对应 Task |
|-----------|-----------|
| §9.1 QUIC 回调 blocking send | Task 1–2 |
| §9.2 每隧道 cleanup watcher | Task 3 |
| §9.3 listener detached handler | Task 4 |
| §10 LOW_LATENCY / MAX_THROUGHPUT 切换 | Task 5 |
| §10 保持 MAX_THROUGHPUT 默认 | Task 5 默认 + Task 6 回归 |
| §10 避免 OPEN 回调 DNS/connect | 不变量 + Task 6 |
| §10 epoll 多路复用 | 明确排除 |

无 TBD / 占位步骤；每个 Task 含具体文件路径与可运行命令。
