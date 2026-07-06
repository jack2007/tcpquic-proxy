# macOS 性能压力测试方案

日期：2026-07-05

配套文档：

- `docs/relay_macos.md` — macOS/Darwin kqueue relay 架构、锁热点与已知约束
- `docs/test/curl_test.md` — Admin API 查询命令
- `scripts/test-tcpquic-proxy.sh` — Unix/macOS loopback smoke
- `scripts/test-tcpquic-concurrent.sh` — 并发 HTTP CONNECT 隧道压测
- `docs/test/dgx-multi-interface-quic-binding-test-design_cn.md` — 双机高带宽回归参考（Linux server 侧）
- `docs/test/windows-performance-stress-test-design_cn.md` — Windows IOCP relay 压测方案参考

## 1. 范围与目标

本文档定义在**当前 macOS 开发机**上执行的 `raypx2` / `tcpquic-proxy` 性能与压力测试方案。重点覆盖 Darwin kqueue relay 数据面、并发 tunnel 调度、内置 speed test、压缩路径、send completion 生命周期、QUIC receive deferred ownership，以及在负载下 Admin/metrics 的可观测性。

### 1.1 当前环境基线

以下信息应在压测前记录到证据目录 `env/`。表中“当前值”以本机 macOS 开发环境为默认模板，执行时以实测输出为准：

| 项 | 当前值 |
|---|---|
| OS | macOS / Darwin（`sw_vers`、`uname -a` 记录实际版本） |
| CPU | Apple Silicon 或 Intel x64（记录 `sysctl -n machdep.cpu.brand_string`） |
| 逻辑 CPU | `sysctl -n hw.ncpu` |
| 物理内存 | `sysctl -n hw.memsize` |
| 构建产物 | `build/bin/Release/raypx2`（或兼容名 `tcpquic-proxy`） |
| Relay 后端 | `darwin` / `kqueue`（worker 数由 `TqDetectRelayWorkers()` 或配置决定） |
| 典型远端 QUIC server | `172.16.10.80:8443`（Linux server，压力测试默认远端） |
| 远端 SSH | `jack@172.16.10.80`（本机已配置免密码 SSH，用于启动/停止远端辅助服务和采集证据） |
| 本机 Admin | `0.0.0.0:2345`（client/router 模式） |

说明：

- macOS 压测**不替代** DGX 双 200G 网口回归；本方案聚焦 macOS client/server 在本机 loopback、管理网链路和 Darwin relay 热路径上的行为与稳定性。
- 若 server 运行在 Linux 远端，端到端吞吐可能受 Linux server、链路、NIC、QoS 或远端 CPU 限制；`summary.md` 必须区分 macOS 侧 relay 指标与端到端吞吐。
- macOS 的 loopback/本机测试更适合暴露 kqueue、send completion、event queue、压缩和生命周期问题；跨机测试更适合形成真实网络基线。

### 1.2 核心目标

| 目标 | 验收标准 |
|---|---|
| 功能正确 | smoke 与 stress 期间 HTTP CONNECT / SOCKS5 / port-forward 隧道建立成功率 ≥ 99.9% |
| 吞吐可复现 | 内置 speed test 与 iperf3 基线在相同参数下 3 轮结果偏差 ≤ 15% |
| Darwin relay 稳定 | 压测期间 `raypx2` 不崩溃、不 abort；`active_relays` 与 tunnel 数一致收敛 |
| QUIC receive ownership 正确 | 高负载下 deferred receive 可观测，`ReceiveComplete()` 不泄漏、不重复；receive pause/resume 可恢复 |
| send completion 生命周期正确 | 同步/异步/迟到 SEND_COMPLETE 下无 double-complete、无 use-after-free、无 dangling worker |
| 观测完整 | 每轮保存 admin metrics、trace 摘要、进程资源采样与原始测速输出 |

### 1.3 非功能目标

| 类别 | 暂定目标 |
|---|---|
| 单隧道吞吐 | loopback 内置 download ≥ 500 Mbps（现代 Mac）；跨机视链路而定，记录实测值作基线 |
| 并发隧道 | 100 条并发短连接建立 ≤ 30 s；256 条 stress 无 OOM / abort |
| 长稳 | soak 30 min 无进程退出；RSS 无单调上升（允许 ±10% 波动） |
| Admin 扰动 | 压测中每 5 s 拉取 `/metrics` 时，p95 额外延迟 < 50 ms（相对空闲基线） |
| Event queue | `relay_event_queue_full_errors` 可偶发但不能持续单调增长并造成吞吐归零 |
| 背压恢复 | `relay_quic_receive_paused_count` 增长后，应有对应 resume，且 pending bytes 回落 |

## 2. 测试分层

与 Windows 方案保持一致，分五层门禁：

```text
L0 单元        tcpquic_darwin_*_test 等（改 src 后增量构建）
L1 smoke       scripts/test-tcpquic-proxy.sh（< 2 min）
L2 benchmark   内置 speed test + 可选 iperf3（5–15 min）
L3 stress      并发隧道 + 长流 + Admin 轮询（15–60 min）
L4 soak        单/多隧道持续 30–120 min
```

执行顺序：**L0 → L1 → L2 → L3**；L4 在发版前、回归 Darwin relay 变更后、或 macOS 版本 / MsQuic 版本变化后执行。L2–L4 失败时必须保留证据目录，不得覆盖上一轮结果。

## 3. 拓扑与场景矩阵

### 3.1 拓扑

| 代号 | 描述 | 用途 |
|---|---|---|
| T0 | 本机 loopback：macOS server + macOS client，临时 OpenSSL 证书 | 隔离 Darwin relay，排除远端变量 |
| T1 | macOS client → 远端 Linux QUIC server `172.16.10.80:8443`（SSH：`jack@172.16.10.80`） | 默认压力测试拓扑，贴近当前开发/部署形态 |
| T2 | macOS server + 远端/本机 client | 验证 server 侧 Darwin relay + dial reactor |
| T3 | macOS client router + admin + 多 peer 配置 | Admin/metrics 与控制面压力 |
| T4 | macOS client/server + zstd 压缩 | 验证压缩 flush、decompress 和背压路径 |

本轮**优先**覆盖 T0 与 T1；T2/T3/T4 作为扩展场景列入矩阵，但不阻塞日常门禁。

### 3.2 功能场景矩阵

| ID | 场景 | 拓扑 | 流量工具 | 并发 | QUIC 连接 | 压缩 | 时长 |
|---|---|---|---|---:|---:|---|---|
| F01 | loopback smoke | T0 | curl CONNECT + SOCKS5 + port-forward | 1 | 1 | off | < 2 min |
| F02 | 内置下载基线 | T0/T1 | `--download-test` | 1 | 1 | off | 60 s × 3 |
| F03 | 内置上传基线 | T0/T1 | `--upload-test` | 1 | 1 | off | 60 s × 3 |
| F04 | download sink 能力检查 | T0 | `--download-sink-test` | 1 | 1 | off | 60 s |
| F05 | zstd 吞吐对照 | T0/T1 | `--download-test` / curl 长响应 | 1 | 1 | zstd | 60 s |
| F06 | 多 QUIC 连接 | T1 | `--download-test` | 1 | 4/8 | off | 60 s |
| F07 | 并发短隧道 | T0 | `scripts/test-tcpquic-concurrent.sh` | 100/256 | 4 | off | 建立阶段 |
| F08 | 并发 + zstd | T0 | `scripts/test-tcpquic-concurrent.sh` | 128 | 8 | zstd | 建立阶段 |
| F09 | 单隧道长流 | T1 | iperf3 `-t 600` | 1 | 4 | off | 10 min |
| F10 | 多隧道长流 | T1 | iperf3 `-P 4` | 4 | 8 | off | 10 min |
| F11 | Admin 轮询压力 | T1 | 后台 curl `/metrics` + F09 | 1+poller | 4 | off | 10 min |
| F12 | worker 数扫描 | T0 | `--download-test` | 1 | 1 | off | 60 s × {1,2,4,8} |
| F13 | 连接建立风暴 | T0 | 快速循环 curl 短请求 | 50 轮 × 4 并行 | 4 | off | 5 min |
| F14 | soak 稳态 | T1 | iperf3 或 download-test | 1–4 | 4 | off | 30 min |
| F15 | 慢 TCP 消费 | T0/T1 | 限速 iperf3 / 慢读 Python server | 1–4 | 4 | off | 10 min |
| F16 | peer abort / shutdown 压力 | T0 | 长流中断目标 TCP / client | 10 轮 | 1–4 | off | 5 min |

说明：Darwin relay 当前 `TqRelayStartQuicReceiveSink()` 不是完整支持路径；F04 用于记录能力边界。若 `--download-sink-test` 在 macOS 上返回“不支持”或 relay start failed，应在 summary 中标记为 **KNOWN LIMITATION**，不把它与普通 relay 稳定性失败混淆。

### 3.3 Darwin relay 专项观测场景

以下场景用于验证 `docs/relay_macos.md` 中近期优化的热点路径：

| ID | 注入方式 | 关注指标 | 通过条件 |
|---|---|---|---|
| D01 | 大窗口下载（F02/F09） | `relay_outstanding_quic_sends`、`relay_outstanding_quic_send_bytes`、legacy `linux_relay_quic_send_backpressure_events` | in-flight 有峰值但能回落；无持续 backpressure 导致吞吐归零 |
| D02 | 高并发隧道（F07） | `/relay/workers` active relays、pending bytes、event queue 深度 | active relay 与 tunnel 数一致；结束后归零 |
| D03 | QUIC receive burst（F10/F15） | `relay_quic_receive_view_count`、`relay_quic_receive_view_bytes`、`relay_quic_receive_paused_count`、`relay_quic_receive_resumed_count` | receive pause/resume 成对恢复；pending bytes 不持续攀升 |
| D04 | Event queue 满压测（降低 queue capacity 后 F07/F10） | `relay_event_queue_full_errors`、legacy `linux_relay_event_queue_*` 细项 | 可触发 backpressure，但无 abort、无永久 stall |
| D05 | zstd flush（F05/F08） | `linux_relay_zstd_decompress_*`、compressed/decompressed bytes、curl 完整响应 | 小响应不被 zstd 长时间缓存；HTTP body 完整 |
| D06 | SEND_COMPLETE 生命周期（F09/F16） | `relay_errors`、trace 中 send complete / close 事件 | peer abort/shutdown 后不再提交已关闭 stream；无 SIGSEGV |
| D07 | 高频 metrics（F11） | `/relay/metrics`、`/relay/workers`、`/relay/active-relays` 响应耗时 | p95 < 50 ms；数据面吞吐下降 < 10% |
| D08 | direct unregister / stop 压力（F16） | process exit code、trace close event、active relays 收敛 | stop/unregister 与 TCP write 并发时无崩溃 |

## 4. 工具与前置条件

### 4.1 必备

| 工具 | 用途 | 安装/检查 |
|---|---|---|
| `raypx2` | 被测二进制 | `cmake --build build --target tcpquic-proxy -j$(sysctl -n hw.ncpu)` |
| `curl` | HTTP CONNECT / Admin | macOS 自带 |
| `python3` | 本机 HTTP 探针目标、并发脚本 | Xcode CLT / Homebrew / 系统 Python |
| OpenSSL | smoke 临时证书 | `brew install openssl` 或系统兼容命令 |
| `jq`（推荐） | 解析 admin JSON | `brew install jq` |

### 4.2 推荐

| 工具 | 用途 | 备注 |
|---|---|---|
| iperf3 | 跨平台带宽基线 | `brew install iperf3` |
| `sample` / `spindump` | 卡顿时采样堆栈 | macOS 自带；异常时采集 |
| `vm_stat` / `ps` | 资源采样 | 长稳与 soak 必采 |
| `nettop` / `lsof` | 网络与 fd 观测 | 并发连接泄漏排查 |
| Instruments / Activity Monitor | 手动性能分析 | 不作为自动门禁 |

### 4.3 构建与二进制约定

```bash
# 增量构建（仅 src 变更）
cmake --build build --target tcpquic-proxy -j"$(sysctl -n hw.ncpu)"

BIN="$PWD/build/bin/Release/raypx2"
if [ ! -x "$BIN" ]; then
  BIN="$PWD/build/bin/Release/tcpquic-proxy"
fi
```

压测前记录：

```bash
shasum -a 256 "$BIN"
git rev-parse HEAD
git status --short
"$BIN" --version 2>/dev/null || stat -f '%Sm %N' "$BIN"
sw_vers
uname -a
sysctl -n hw.ncpu hw.memsize machdep.cpu.brand_string
```

## 5. 执行说明

### 5.1 L0 — Darwin 单元测试

```bash
cmake --build build --target tcpquic_darwin_reactor_test -j"$(sysctl -n hw.ncpu)"
cmake --build build --target tcpquic_darwin_relay_worker_queue_test -j"$(sysctl -n hw.ncpu)"
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j"$(sysctl -n hw.ncpu)"
cmake --build build --target tcpquic_darwin_relay_metrics_test -j"$(sysctl -n hw.ncpu)"

./build/bin/Release/tcpquic_darwin_reactor_test
./build/bin/Release/tcpquic_darwin_relay_worker_queue_test
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
./build/bin/Release/tcpquic_darwin_relay_metrics_test
```

通过：所有测试退出码 0。若 macOS runner 上出现 `-Werror` 编译失败，应先区分当前变更引入与既有 Darwin 编译问题，并在 summary 中记录。

### 5.2 L1 — loopback smoke

```bash
cd /Users/ry/src/tcpquic-proxy
TCPQUIC_PROXY_BIN="$PWD/build/bin/Release/raypx2" ./scripts/test-tcpquic-proxy.sh

COMPRESS=zstd TCPQUIC_PROXY_BIN="$PWD/build/bin/Release/raypx2" ./scripts/test-tcpquic-proxy.sh
```

通过：脚本退出码 0；client/server log 含 listener ready 与 tunnel 成功事件；无 `abort`、`Segmentation fault` 或 `receive_fake_fin` fail-fast。

### 5.3 L2 — 内置 speed test（T0 示例）

终端 1 — server：

```bash
BIN="./build/bin/Release/raypx2"
"$BIN" server \
  --listen 127.0.0.1:18443 \
  --cert cert/server/server.crt \
  --key cert/server/server.key \
  --allow-targets 127.0.0.0/8 \
  --compress off
```

终端 2 — client download 60 s × 3 轮：

```bash
"$BIN" client \
  --peer 127.0.0.1:18443 \
  --ca cert/ca.crt \
  --connections 1 \
  --compress off \
  --download-test 60
```

记录每轮 stdout 中的吞吐（Mbps）、RTT、ideal send。upload 方向将 `--download-test` 换成 `--upload-test`。

T1 远端模式：压力测试默认 QUIC server 为 `172.16.10.80:8443`，本机已可免密码 SSH 到 `jack@172.16.10.80`。证书与 `--config` 使用现有 `client-config-*.json`；内置 speed test 优先使用单 peer/单用途 CLI，避免与 router 多 peer 配置混用。

```bash
# 远端连通性与环境快照，保存到 env/remote-*.txt
ssh jack@172.16.10.80 'hostname; uname -a; ss -lunpt 2>/dev/null | grep 8443 || true'

"$BIN" client \
  --peer 172.16.10.80:8443 \
  --ca cert/ca.crt \
  --connections 1 \
  --compress off \
  --download-test 60
```

若远端 `raypx2 server` 需要由本机启动/重启，应通过 `ssh jack@172.16.10.80 '<server start command>'` 执行，并把远端命令、PID、stdout/stderr 路径写入 `env/remote-server-command.txt` 与 `proxy/remote-server.stderr.log`。

### 5.4 L2 — iperf3 长流（T1 示例）

远端 server 启动 iperf3（Linux）。本机已可免密码 SSH 到 `jack@172.16.10.80`，推荐通过 SSH 启动并保存远端日志：

```bash
ssh jack@172.16.10.80 'nohup iperf3 -s -p 16001 -B 172.16.10.80 > /tmp/raypx2-iperf3-16001.log 2>&1 & echo $!'
ssh jack@172.16.10.80 'ss -lntp 2>/dev/null | grep 16001 || true'
```

若已在远端交互 shell 中操作，也可直接执行：

```bash
iperf3 -s -p 16001 -B 172.16.10.80
```

macOS client 经 port-forward 或 HTTP CONNECT 代理跑 iperf3：

```bash
# 假设 client-config 已将 127.0.0.1:15445 转发到远端 172.16.10.80:16001
iperf3 -c 127.0.0.1 -p 15445 -t 60 -J -O 5 | tee case/iperf.stdout.json
```

如使用 HTTP CONNECT 而不是 port-forward，应保存代理配置、curl/iperf wrapper 命令和目标端口映射，保证后续可复现。

### 5.5 L3 — 并发短隧道

```bash
BIN="$PWD/build/bin/Release/raypx2" \
TUNNELS=100 QUIC_CONNECTIONS=4 COMPRESS=off HANDSHAKE_THREADS=32 \
./scripts/test-tcpquic-concurrent.sh

BIN="$PWD/build/bin/Release/raypx2" \
TUNNELS=128 QUIC_CONNECTIONS=8 COMPRESS=zstd HANDSHAKE_THREADS=64 \
./scripts/test-tcpquic-concurrent.sh
```

通过：全部隧道 HTTP 200；client/server 进程仍存活；无 `abort`、`SIGSEGV`、`receive_fake_fin`；结束后 `/relay/metrics` 中 active relays 回落到 0。

### 5.6 L3 — Admin metrics 轮询（F11）

参考 `docs/test/curl_test.md` 设置 `ADMIN` 与 `TOKEN`，压测期间后台执行：

```bash
ADMIN='http://127.0.0.1:2345/api/v1'
TOKEN='<从 admin-token-file 读取>'
AUTH="Authorization: Bearer ${TOKEN}"

for i in $(seq 1 120); do
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  start_ns="$(python3 - <<'PY'
import time
print(time.time_ns())
PY
)"
  curl -sS --max-time 3 -H "$AUTH" "$ADMIN/metrics" > "admin/metrics-${i}.json"
  rc=$?
  end_ns="$(python3 - <<'PY'
import time
print(time.time_ns())
PY
)"
  printf '%s,%s,%s,%s\n' "$ts" "$rc" "$start_ns" "$end_ns" >> metrics/admin-latency.csv
  sleep 5
done
```

同时运行 F09 长流。对比空闲时与负载时的 `admin-latency.csv` 分布，并额外采集 `/relay/metrics`、`/relay/workers` 与 `/relay/active-relays`。

### 5.7 L3 — Darwin worker 数扫描（F12）

通过 CLI 或 JSON 覆盖平台中性 `relay.common.worker_count`：

```bash
for n in 1 2 4 8; do
  "$BIN" client \
    --peer 127.0.0.1:18443 \
    --ca cert/ca.crt \
    --connections 1 \
    --compress off \
    --relay-worker-count "$n" \
    --download-test 60 | tee "case/worker-${n}.txt"
done
```

对 `{1,2,4,8}` 各跑一轮，记录吞吐、pending bytes 峰值、event queue full 次数、pause/resume 次数。选择吞吐/资源平衡点写入 `summary/worker-sweep.json`。

### 5.8 推荐启动参数（压力场景）

```text
--compress off             # 吞吐基线；压缩对照单独场景
--connections 4|8          # 多 QUIC slot 轮询
--trace --trace-interval 30
--relay-worker-count <n>   # worker 扫描场景
```

避免在首轮基线中开启 `--trace-interval 1`，高频 trace 会扰动数据面；观测专项可降至 10–30 s。

## 6. 观测与采样

### 6.1 Admin `/metrics` 与 `/relay/*` 关键字段

JSON 路径因版本略有差异，优先使用平台中性 `relay_*` 字段；旧 `linux_relay_*` 字段仍会在 Darwin 后端输出，作为细粒度兼容指标参考。

| 字段 | 含义 |
|---|---|
| `relay_backend` | 当前 relay 后端，应为 Darwin/kqueue 语义 |
| `relay_active_relays` | 当前活跃 relay 数 |
| `relay_pending_bytes` | 后端聚合 pending bytes |
| `relay_buffer_bytes_in_use` | relay buffer 当前占用 |
| `relay_outstanding_quic_sends` | TCP->QUIC in-flight send 数 |
| `relay_outstanding_quic_send_bytes` | TCP->QUIC in-flight send 字节 |
| `linux_relay_quic_send_backpressure_events` | legacy 细项：QUIC send 背压事件数 |
| `relay_quic_receive_view_count` | deferred QUIC receive view 数 |
| `relay_quic_receive_view_bytes` | deferred QUIC receive view 字节 |
| `relay_quic_receive_paused_count` | receive pause 次数 |
| `relay_quic_receive_resumed_count` | receive resume 次数 |
| `relay_event_queue_full_errors` | event queue 满次数 |
| `relay_event_queue_capacity` | event queue 容量 |
| `relay_errors` | relay 聚合错误数 |
| `linux_relay_event_queue_push_cas_retries` | legacy 细项：event queue push CAS 重试 |
| `linux_relay_event_queue_pop_cas_retries` | legacy 细项：event queue pop CAS 重试 |
| `linux_relay_control_command_timeouts` | legacy 细项：控制命令等待超时 |
| `linux_relay_snapshot_command_timeouts` | legacy 细项：snapshot 等待超时 |
| `linux_relay_quic_receive_view_backpressure_queued` | legacy 细项：queue 满后 callback 暂存 receive view |
| `linux_relay_quic_receive_view_failures` | legacy 细项：receive view 入队失败总数 |
| `linux_relay_zstd_decompress_failures` | legacy 细项：zstd 解压失败 |

采样节奏：

- benchmark：开始前、结束后各 1 次全量 `/metrics`、`/relay/metrics`、`/relay/workers`
- stress/soak：每 30 s 一次 `/metrics` 与 `/relay/metrics`，每 5 min 一次 `/diagnostics`
- 并发场景：建立前、建立峰值、完成后各采 `/relay/active-relays`
- 异常时立即补采 `/tunnels`、`/peers/<id>`、`/relay/workers`、`sample <pid> 5` 和 `log/*.log` 尾部

### 6.2 进程资源采样

```bash
pid=<raypx2 PID>
mkdir -p metrics
printf 'ts,pid,rss_kb,vsz_kb,cpu_percent,threads\n' > metrics/process-sample.csv
while kill -0 "$pid" 2>/dev/null; do
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  ps -o pid=,rss=,vsz=,%cpu=,nlwp= -p "$pid" | awk -v ts="$ts" '{printf "%s,%s,%s,%s,%s,%s\n", ts,$1,$2,$3,$4,$5}' >> metrics/process-sample.csv
  sleep 10
done
```

内存异常时补采：

```bash
vm_stat > metrics/vm-stat.txt
lsof -p "$pid" > metrics/lsof.txt
sample "$pid" 5 -file metrics/sample-${pid}.txt
```

### 6.3 Trace 摘要

启用 `--trace` 时，检查 `log/client.log` / `log/server.log` 中 `event=stats_tick`、relay close、send complete、receive pause/resume、event queue full 等关键事件。重点关注：

- `receive_fake_fin`：当前 Darwin/Windows 约定为 fail-fast；出现即硬失败，需保留完整 trace。
- peer abort / shutdown complete 后是否仍有 TCP->QUIC send 提交。
- event queue full 后是否 pause receive，并在 worker drain 后恢复。
- zstd 小响应是否因为缺少 flush 而卡住。

## 7. 通过 / 失败门禁

### 7.1 硬性失败（任一条即 FAIL）

- `raypx2` 进程非预期退出、崩溃、`SIGSEGV`、`SIGABRT` 或未处理异常
- smoke 或并发隧道成功率 < 99%
- soak 期间 RSS 持续上升超过 20% 且无回落
- `receive_fake_fin` 触发 fail-fast
- Admin `/health` 在压测中连续 3 次不可达
- `relay_active_relays` 在测试结束后 60 s 内未回落到预期值
- `relay_event_queue_full_errors` 持续增长并伴随吞吐归零或 receive 不恢复

### 7.2 软性告警（记入 summary，不单独 FAIL）

- 吞吐较上一轮基线下降 > 15%
- `linux_relay_quic_send_backpressure_events` 持续增长但最终恢复
- `ssh jack@172.16.10.80` 偶发超时，但本地压测已完成且远端 QUIC `172.16.10.80:8443` 未中断
- `relay_quic_receive_paused_count` 明显大于 resumed count，但 pending bytes 能在结束后归零
- iperf3 重传率 > 1%（跨机场景）
- `/relay/metrics` p95 响应耗时 > 50 ms，但数据面无明显下降
- F04 receive sink 不支持（当前已知限制，标为 KNOWN LIMITATION）

### 7.3 报告模板

每轮测试产出 `summary/summary.md`：

```markdown
# macOS 压测摘要

- 场景: F09
- 拓扑: T1
- git: <sha>
- binary sha256: <hash>
- 结果: PASS | FAIL | WARN | KNOWN LIMITATION

## 吞吐
- download_test 平均: X Mbps（3 轮）
- iperf3 终点: Y Gbps

## Darwin relay
- backend: <relay_backend>
- active relays peak: N
- pending bytes peak: B
- event queue full: E
- receive pause/resume: P/R
- outstanding quic send bytes peak: Q

## 资源
- peak RSS: W MB
- threads peak: T
- 进程退出码: 0

## 备注
- ...
```

## 8. 证据目录结构

沿用 Windows / DGX 回归命名，便于横向对比：

```text
docs/test/macos-perf-<scenario>-<YYYYMMDD-HHMMSS>/
  env/
    git-head.txt
    git-status-short.txt
    binary.sha256
    sw-vers.txt
    uname.txt
    sysctl.txt
    remote-ssh.txt              # ssh jack@172.16.10.80 连通性与远端环境
    remote-server-command.txt   # 如通过 SSH 启停远端 raypx2/iperf3，记录完整命令和 PID
    tuning-snapshot.txt          # 启动时 stdout tuning 行
  case/
    run.log
    speed-test-round-{1,2,3}.txt
    iperf.stdout.json
    iperf.rc
    concurrent-tunnels.rc
  admin/
    metrics-before.json
    metrics-after.json
    metrics-t{30,60,300}s.json
    relay-metrics-before.json
    relay-metrics-after.json
    relay-workers-t{30,60,300}s.json
    relay-active-relays-peak.json
    health.json
  metrics/
    process-sample.csv
    admin-latency.csv
    vm-stat.txt
    lsof.txt
    sample-<pid>.txt
  proxy/
    client.stderr.log
    server.stderr.log
    client.trace.log             # 可选复制 log/client.log
    server.trace.log             # 可选复制 log/server.log
    remote-server.stderr.log     # T1 远端 server stderr/stdout 快照（通过 SSH 采集）
  summary/
    summary.md
    worker-sweep.json            # F12 专用
```

## 9. 风险与限制

| 风险 | 说明 | 缓解 |
|---|---|---|
| 远端瓶颈掩盖 macOS 问题 | T1 吞吐受 Linux server、链路带宽、QoS 限制 | 必做 T0 loopback；对比 relay metrics 与端到端吞吐 |
| 远端 server 状态漂移 | 默认 QUIC server 为 `172.16.10.80:8443`，若远端进程、证书或 ACL 被其他任务改动，会影响复现 | 每轮保存 `ssh jack@172.16.10.80` 环境快照、监听端口、server 命令与日志 |
| receive sink 不完整 | Darwin `TqRelayStartQuicReceiveSink()` 当前不是完整支持路径 | F04 标记能力边界；普通 relay 不受该场景阻塞 |
| fake FIN fail-fast | Darwin 遇到 fake FIN 会 abort，这是当前设计契约 | 作为硬失败保存 trace；优先调查 MsQuic receive 语义 |
| zstd 小响应缓存 | TCP->QUIC zstd path 必须 flush，否则小响应可能卡住 | F05/F08 必测 curl 完整 body 与压缩指标 |
| trace 扰动 | 过短 `trace-interval` 放大 wake/锁竞争 | 基线用 30 s；专项观测再缩短 |
| macOS 调度与省电 | 笔记本温控、省电模式影响吞吐 | 记录电源状态；长测保持接电并关闭重负载后台任务 |
| 高并发端口耗尽 | 短连接风暴可能受 ephemeral port / TIME_WAIT 影响 | 记录失败类型；必要时降低并发或延长间隔 |
| Admin metrics 字段兼容 | Darwin 仍输出部分 legacy `linux_relay_*` 字段 | 报告优先用 `relay_*`；legacy 仅用于细粒度分析 |

## 10. 与 CI / 日常开发的衔接

| 触发条件 | 最低门禁 |
|---|---|
| 修改 `src/tunnel/darwin_relay_worker.*` | L0 `tcpquic_darwin_relay_worker_io_test` + queue test + L1 smoke |
| 修改 `src/runtime/darwin_reactor.*` | L0 `tcpquic_darwin_reactor_test` + L1 smoke |
| 修改 `src/runtime/relay_metrics.*` 或 trace | L0 `tcpquic_darwin_relay_metrics_test` + L1 smoke + 一次 L2 download |
| 修改压缩 / buffer / relay alloc | L0 Darwin relay IO test + F05 zstd 对照 |
| 发版前 / 大改 Darwin relay | L1 + L2 全方向 + F07 并发 + F14 soak 30 min |
| 仅文档 / 脚本 | L1 smoke（如脚本相关）或文档自查 |

```bash
# L0 最小集合示例
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j"$(sysctl -n hw.ncpu)"
./build/bin/Release/tcpquic_darwin_relay_worker_io_test

cmake --build build --target tcpquic_darwin_relay_metrics_test -j"$(sysctl -n hw.ncpu)"
./build/bin/Release/tcpquic_darwin_relay_metrics_test
```

## 11. 后续工作

- [ ] 新增 `scripts/run-macos-perf-matrix.sh`，按本章 F01–F16 自动创建证据目录
- [ ] 为 `scripts/test-tcpquic-concurrent.sh` 增加 macOS 资源采样和 Admin metrics 采集选项
- [ ] 将 L0 Darwin relay tests 纳入 macOS CI runner
- [ ] 明确 macOS receive sink 产品策略：实现、禁用并显式报错，或从压测矩阵移除
- [ ] 补充 TSAN 任务，重点覆盖 unregister 与 `FlushTcpWrites()` 并发、worker stop 与 late callback、event queue 满压测
- [ ] 将 `/relay/workers` 与 `/relay/active-relays` 快照纳入性能报告可视化

---

**执行入口速查**

```bash
# 日常三步
cmake --build build --target tcpquic-proxy -j"$(sysctl -n hw.ncpu)"
TCPQUIC_PROXY_BIN="$PWD/build/bin/Release/raypx2" ./scripts/test-tcpquic-proxy.sh
"$PWD/build/bin/Release/raypx2" client --peer 127.0.0.1:18443 --ca cert/ca.crt --connections 1 --compress off --download-test 60

# 查看 metrics
curl -sS -H "Authorization: Bearer <token>" http://127.0.0.1:2345/api/v1/metrics
curl -sS -H "Authorization: Bearer <token>" http://127.0.0.1:2345/api/v1/relay/metrics
curl -sS -H "Authorization: Bearer <token>" http://127.0.0.1:2345/api/v1/relay/workers
```
