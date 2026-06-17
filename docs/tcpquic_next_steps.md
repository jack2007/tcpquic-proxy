# tcpquic-proxy 后续重点开发方向

> 日期：2026-06-07
> 范围：当前 `src/` 实现、压测记录、线程模型、性能调优与产品化 backlog。
> 关联文档：`docs/superpowers/to_be_fixed.md`、`research_progress.md`、`src/docs/key_parameter.md`、`src/docs/thread_model_cn.md`。

## 1. 当前工作情况

`tcpquic-proxy` 已经从原型进入可验证的高吞吐代理阶段，核心 TCP-over-QUIC 链路、WAN 调优、线程模型优化和压测方法论都已有较完整闭环。

### 1.1 功能状态

已实现能力：

- TCP-over-QUIC 隧道：1 个 TCP 连接映射 1 条 QUIC 双向 stream。
- client 入口：SOCKS5 CONNECT、HTTP CONNECT。
- server 出口：动态目标拨号、B 侧 DNS、CIDR ACL。
- 安全：mTLS、ALPN、默认本地 loopback 监听。
- 传输：单 QUIC 长连接或连接池、BBR 高吞吐 profile、zstd/lz4/off/auto 压缩。
- 短载荷优化：client 启动 `--warmup-mb` / `--warmup-target` / `--warmup-path` 预热。
- 调参：`--tuning auto|lan|wan|custom`、目标带宽/RTT、内存预算、QUIC/relay 覆盖参数。

仍属 v1 预留或产品 backlog 的能力：

- SOCKS5 / HTTP CONNECT 用户认证。
- QUIC 断连后在途隧道透明迁移。
- UDP 代理。
- 普通 HTTP 代理（非 CONNECT）。
- 配置文件、部署样例、运行时指标接口。

### 1.2 性能与验证状态

当前性能目标基本达成：在 100ms RTT WAN 仿真下，`tunnel_off` 单流稳态已经逼近或超过同条件 secnetperf 基线。

已完成的重要验证：

- 本地 smoke：`scripts/test-tcpquic-proxy.sh` PASS。
- 100 并发：`scripts/test-tcpquic-concurrent.sh` PASS。
- ASAN smoke：Release + ASAN 构建 3 次连续 PASS。
- WAN 稳态：`tunnel_off` 在历史 128 MB / steady 口径下已超过 secnetperf。
- netem 方法论：确认 delay-only 会高估真实 WAN，`bench-tcpquic-proxy-dgx.sh` 已在 `NETEM=1` 时默认 `RATE=1gbit`。
- 短载荷：8/16/32 MB average 口径已从历史约 494 Mbps 提升到 3.9-4.6 Gbps 量级。
- 预热：8 MB 背靠背下 `WARMUP_MB=2` 相比无预热约 +22%。
- 多 curl：直网 `PARALLEL_CURL=4` 相比单路聚合吞吐约 +66%。
- 压缩 curl(18)：已定位并修复压缩接收侧解压输出一次性入队导致 TCP 写队列背压误判 fatal 的问题。

### 1.3 线程与资源模型状态

当前模型是应用自建线程和 MsQuic worker 的混合模型：

- MsQuic 侧使用 MAX_THROUGHPUT profile，创建 QUIC worker 与 UDP datapath worker。
- client 侧 SOCKS5 / HTTP CONNECT listener 各 1 个线程。
- client 握手使用固定大小 `TqThreadPool`，避免每 accepted TCP 连接 detached handler。
- Linux 生产 relay 由固定数量 `TqLinuxRelayWorker` 分片 epoll 多路复用全部隧道 TCP fd（含压缩）；不再为每条隧道创建 relay TCP 线程或 `TqTcpWriteQueue` writer。
- 旧 `TqBlockingDemoRelay` / `TqTcpWriteQueue` 已删除，不再作为 demo/legacy target 保留。
- 隧道清理由全局 `TqTunnelReaper` 统一回收，避免每隧道 cleanup watcher。

这个模型已通过 100 并发验证；relay 侧线程数随 worker 分片数 W 而非隧道数 L 增长，有利于向 500/1000 隧道扩展。

## 2. 当前主要风险

### 2.1 压缩路径仍缺生产定标

curl(18) 已修复，zstd/lz4 双机大载荷已恢复可用，但仍不能直接把压缩作为生产默认策略。

待确认点：

- CPU 成本、压缩比、墙钟、RSS、吞吐之间的真实权衡。
- 不同载荷类型下 `off` / `lz4` / `zstd` 的稳定收益。
- `--compress auto` 的采样窗口是否足够，是否会对短流误判。
- LZ4 compressor 当前每个输入块 `compressUpdate` 后 `compressEnd`，实质更接近多 frame 行为，需确认是否应改为真正 streaming frame。

### 2.2 连接池收益还缺限速 WAN 矩阵

已有直网多 curl 数据说明连接池收益需要多流并发才能体现；单 curl 下 `QUIC_CONNECTIONS=4` 相比 `1` 只有小幅增益。

缺口：

- `NETEM=1` + `RATE=1gbit/5gbit` 下的多 curl 限速矩阵尚未完成。
- 还不能明确生产默认 `--quic-connections` 是否应继续为 1，或按并发档位推荐 2/4/8。

### 2.3 高并发资源边界未知

100 隧道已验证，但 500/1000 隧道还没有系统数据。

潜在瓶颈：

- Linux relay worker 分片数 W 与单 worker relay 密度、fairness budget。
- worker 池化缓冲预算与大解压输出之间的关系。
- server 侧 OPEN 后目标拨号仍使用 detached thread。
- relay buffer pool 在高并发下的总内存压力。

### 2.4 Fork 核心调优边界需要清理

当前 fork 中有 MsQuic 核心侧调优，例如 BBR startup / min RTT、ideal send buffer 默认值等。后续上游合并或长期维护时，需要明确哪些是 proxy 场景策略，哪些必须留在核心。

原则：

- 能放到 proxy settings / CLI 的策略，尽量不要固化到 MsQuic 核心默认值。
- secnetperf 基线应暴露相同调参入口，避免 proxy 与基线参数不对齐。

## 3. 后续重点开发方向

### 3.1 压缩路径专项 profiling（✅ 2026-06-07 初版完成）

目标：决定 zstd/lz4 是否能作为生产可选策略，以及 `--compress auto` 是否可信。

建议工作：

- 评估并修正 LZ4 多 frame 行为，确认是否改为长流 streaming frame。
- 跑 `off` / `zstd` / `lz4` 对比矩阵。
- 覆盖 `zeros` / `text` / `repeat` / `random` / 真实业务载荷。
- 覆盖 `RATE=1gbit` / `RATE=5gbit` / 不限速直网。
- 记录 CPU、压缩比、墙钟、curl exit、吞吐、RSS、writer queue 等指标。
- 对 `--compress auto` 建立明确策略：低压缩率 off，中等压缩率 lz4，高压缩率 zstd。
- 输出生产建议：默认 off、默认 auto，或按场景启用。

建议验收：

- 所有压缩模式 curl exit 0。
- zstd/lz4 在可压缩载荷下有明确收益，且 CPU 成本可接受。
- 不可压缩载荷下 auto 能稳定选择 off 或低成本路径。
- 结果写入 `research_progress.md`，并同步更新 `to_be_fixed.md`。

### 3.2 `NETEM=1` 多 curl 限速矩阵（✅ 2026-06-07 QUICK 矩阵完成）

目标：把连接池收益从直网局部数据扩展为真实 WAN 仿真结论。

建议矩阵：

- `PARALLEL_CURL=1/2/4/8`
- `QUIC_CONNECTIONS=1/2/4/8`
- `RATE=1gbit/5gbit`
- `DELAY=100ms`
- `LOSS=0%/1%/5%`
- `compress=off` 先跑基线，再跑 `lz4/zstd`

需要回答的问题：

- 生产默认 `--quic-connections` 是否继续为 1。
- 多并发下载时推荐连接数是多少。
- 高 RTT 限速场景下连接池是否改善聚合吞吐或公平性。
- 多连接是否带来 tail latency、CPU、内存或重传风险。

建议验收：

- 形成连接数推荐表。
- 标记单流、多流、低带宽、高带宽各自推荐参数。
- 更新 `scripts/bench-tcpquic-multi-curl.sh` 文档和 `research_progress.md`。

### 3.3 高并发资源模型优化（Linux worker relay ✅ 2026-06-09）

目标：在 Linux worker relay 已替代 per-tunnel blocking relay 的前提下，为 500/1000 隧道级别扩展做观测与调优。

建议阶段：

1. 先加观测，不先重构。
2. 记录 active tunnels、thread count、RSS、worker pool bytes、pending relay bytes、in-flight sends、per-tunnel throughput。
3. 将 server 侧 detached dial thread 改为 bounded dial pool。
4. 将 worker 池化缓冲与 fairness budget 纳入 tuning 的自动 relay memory budget。
5. 跑 100/500/1000 并发阶梯测试。

建议验收：

- 100 隧道无回归。
- 500 隧道资源曲线可解释，无 OOM、无明显 thread explosion（relay 侧线程应随 W 而非 L 增长）。
- 若 1000 隧道失败，能从指标定位是 worker 公平性、内存、TCP 后端还是 QUIC 限制。

### 3.4 基线工具与上游边界清理

目标：让 proxy 与 secnetperf 的对比更公平，也降低 fork 私有调优维护成本。

建议工作：

- 为 secnetperf 增加 `-fcw` / `-srw` / `-iw` / `-initrtt` 等 CLI。
- bench 输出自动记录 git commit、binary path、Release/ASAN、QUIC tuning、relay tuning、netem qdisc。
- 将 MsQuic 核心调优分类：必须保留、可迁移到 proxy settings、仅实验保留。
- 文档明确哪些性能数据依赖 fork 核心改动，哪些只依赖 proxy settings。

建议验收：

- secnetperf 与 proxy 基线参数可对齐。
- `research_progress.md` 新数据不再出现参数来源不明的问题。
- fork 核心改动有单独评审清单。

### 3.5 产品化 v1 backlog

目标：在性能基线稳定后，把工具推进为可部署代理。

建议拆成独立计划：

- SOCKS5 / HTTP CONNECT 用户认证。
- 配置文件支持，减少生产长 CLI。
- systemd / container 部署样例。
- 运行时 stats 输出或周期 JSON 日志。
- ACL 审计日志与 DNS 解析日志。
- QUIC 断连语义设计：明确是否追求 TCP 连接透明保持，还是快速失败并重建。
- UDP 代理和普通 HTTP 代理后置，除非有明确业务需求。

## 4. 推荐路线

### Phase A：验证与定标

优先做压缩 CPU profiling 和 `NETEM=1` multi-curl 限速矩阵。

特点：

- 主要补测试、脚本和观测。
- 不大改架构。
- 直接决定 `--compress auto`、`--quic-connections`、warmup 和默认 tuning 的推荐值。

### Phase B：资源模型收敛

基于 Phase A 数据处理高并发资源问题。

重点：

- bounded dial pool。
- writer queue tuning。
- relay memory budget。
- 线程/RSS/队列观测。
- 500/1000 tunnel 阶梯测试。

### Phase C：产品化

性能基线稳定后再做功能产品化，避免认证、配置、部署、断连语义等改动污染压测结论。

重点：

- 认证。
- 配置文件。
- 部署样例。
- 指标输出。
- 断连语义。

## 5. 当前推荐下一步

Phase A（压缩 profiling + NETEM 多 curl 矩阵）已于 2026-06-07 完成，脚本与 QUICK 矩阵数据见 `research_progress.md`。

当前最值得推进：

- Linux relay Phase E: validate MsQuic receive buffer lifetime and only then consider deferred receive views with explicit `StreamReceiveComplete`; keep copy-into-pool as the safe default until that test exists.

1. **高并发资源模型**（Phase B）：500/1000 隧道阶梯测试、bounded dial pool、writer queue tuning。
2. **secnetperf CLI 对齐**（Phase 3 Task 5）：`-fcw/-srw/-iw/-initrtt` 与 proxy tuning 对齐。
3. **全量矩阵复测**（可选）：`run-compression-profiling.sh`（非 QUICK）与 `run-multi-curl-netem-matrix.sh`（`RATE=5gbit`、丢包维度）。
