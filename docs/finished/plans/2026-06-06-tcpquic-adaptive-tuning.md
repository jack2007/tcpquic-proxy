# tcpquic-proxy 自适应调参 Phase 1 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 tcpquic-proxy 中硬编码的 QUIC / relay / TCP 调优常量移入 `TqConfig`，支持 `lan|wan|auto|custom` profile 与 BDP 推算，默认 `wan` 保持现有吞吐行为。

**Architecture:** 新增 `tuning.h/cpp` 承载 `TqTuningConfig` 与 `TqComputeTuning()`；CLI 解析写入 `TqConfig` 输入字段，启动前合成最终参数；`quic_session`、`relay`、`tcp_dialer` 消费合成结果而非文件内常量。

**Tech Stack:** C++17, MsQuic settings API, 现有 tcpquic-proxy 单元测试框架

---

### Task 1: tuning 模块与单元测试

**Files:**
- Create: `src/tools/tcpquic-proxy/tuning.h`
- Create: `src/tools/tcpquic-proxy/tuning.cpp`
- Create: `src/tools/tcpquic-proxy/unittest/tuning_test.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`

- [x] 实现 BDP 推算、profile 默认值、`round_up_power_of_2`
- [x] 添加 `tcpquic_tuning_test`，验证 `wan` 与典型 BDP 场景
- [x] 构建并运行测试

### Task 2: config CLI 扩展

**Files:**
- Modify: `src/tools/tcpquic-proxy/config.h`
- Modify: `src/tools/tcpquic-proxy/config.cpp`

- [x] 新增 `--tuning`、`--target-bandwidth-mbps`、`--target-rtt-ms`、`--max-memory-mb` 及 custom 覆盖项
- [x] 解析后调用 `TqComputeTuning(cfg)`

### Task 3: 接入 quic_session / relay / tcp_dialer / main

**Files:**
- Modify: `src/tools/tcpquic-proxy/quic_session.cpp`
- Modify: `src/tools/tcpquic-proxy/relay.h`, `relay.cpp`
- Modify: `src/tools/tcpquic-proxy/tcp_dialer.h`, `tcp_dialer.cpp`
- Modify: `src/tools/tcpquic-proxy/tcp_tunnel.cpp`
- Modify: `src/tools/tcpquic-proxy/main.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`

- [x] `MakeSettings(cfg, server)` 使用 `cfg.Tuning`
- [x] `TqRelayStart(..., const TqTuningConfig&)` 实例化 relay 参数
- [x] `TqTuneTcpForThroughput(fd, bytes)` 使用 tuning
- [x] 启动时打印最终 tuning

### Task 4: 验证

- [x] `cmake --build build-iouring --target tcpquic-proxy tcpquic_tuning_test`
- [x] `./scripts/test-tcpquic-proxy.sh` smoke
### Task 5: Phase 2 — relay pool 运行时缩放

- [x] `TqSetRelayMemoryBudget` / `TqApplyRelayPoolBudget` / active relay 计数
- [x] `TqRelayStart` 按 `max_memory_mb / active_tunnels` 实例化 pool
- [x] 100 并发 + `--max-memory-mb 512` 回归

### Task 6: Phase 3 — 运行时 RTT / 吞吐 / ideal send 观测

**Goal:** 记录路径实测 RTT、relay warmup 吞吐与 MsQuic ideal send hint，在**下一条** QUIC 连接建立前降档 tuning（不热更新已建立连接）。

**Files:**
- Modify: `src/tools/tcpquic-proxy/tuning.h`, `tuning.cpp`
- Modify: `src/tools/tcpquic-proxy/quic_session.cpp`
- Modify: `src/tools/tcpquic-proxy/relay.cpp`
- Modify: `src/tools/tcpquic-proxy/main.cpp`
- Modify: `src/tools/tcpquic-proxy/unittest/tuning_test.cpp`

- [x] `TqRuntimeObservations` + `TqRecordMeasuredRtt` / `TqRecordRelayThroughput` / `TqRecordIdealSendHint`
- [x] `TqApplyRuntimeObservations` — 只降档；`wan`/`auto` 启用，`lan`/`custom` 禁用
- [x] `quic_session`: `TqSampleConnectionRtt`（匿名命名空间，读 `QUIC_STATISTICS_V2`）；`StartSlotLocked` 重连前刷新 Configuration
- [x] `relay`: 3s warmup 采样 + `IDEAL_SEND_BUFFER_SIZE` 事件
- [x] 每 30s 日志 + 单元测试
- [x] 构建与回归：`tcpquic_tuning_test`、`test-tcpquic-proxy.sh`、`test-tcpquic-concurrent.sh`（`MAX_MEMORY_MB=512`）

**Design notes:**
- `TqSampleConnectionRtt` 放在 `quic_session.cpp` 而非 `tuning.cpp`，避免单元测试链接 MsQuic

### Task 7: Phase 4 — 压缩路径自适应

**Goal:** `--compress auto` 在 warmup 期间采样压缩率，为下一条隧道选择 off / lz4 / zstd（per-stream OPEN 协商，不在流内切换）。

**Files:**
- Modify: `src/tools/tcpquic-proxy/tuning.h`, `tuning.cpp`
- Modify: `src/tools/tcpquic-proxy/relay.h`, `relay.cpp`
- Modify: `src/tools/tcpquic-proxy/tcp_tunnel.cpp`
- Modify: `src/tools/tcpquic-proxy/main.cpp`
- Modify: `src/tools/tcpquic-proxy/unittest/tuning_test.cpp`

- [x] `TqCompressionObservations` + `TqRecordCompressionSample` + `TqResolveAutoCompress`
- [x] 阈值：ratio ≥ 98% → off；≥ 82% → lz4；否则 zstd；无样本时 lz4 probe
- [x] relay 压缩路径 3s warmup 采样 raw/compressed 字节
- [x] `TqFlagsFromConfig` 调用 `TqResolveAutoCompress`
- [x] 单元测试 + smoke 回归
