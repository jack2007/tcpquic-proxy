# Windows 性能压力测试方案

日期：2026-07-05

配套文档：

- `docs/relay_win.md` — Windows IOCP relay 架构与观测指标
- `docs/test/curl_test.md` — Admin API 查询命令
- `scripts/test-tcpquic-proxy-windows.ps1` — 本机 loopback smoke
- `docs/test/dgx-multi-interface-quic-binding-test-design_cn.md` — 双机高带宽回归参考（Linux server 侧）

## 1. 范围与目标

本文档定义在**当前 Windows 开发机**上执行的 `raypx2` / `tcpquic-proxy` 性能与压力测试方案。重点覆盖 Windows IOCP relay 数据面、并发 tunnel 调度、内置 speed test、压缩路径，以及在负载下 Admin/metrics 的可观测性。

### 1.1 当前环境基线

以下信息来自本机实测，压测前应先记录到证据目录 `env/`：

| 项 | 当前值 |
|---|---|
| OS | Windows 10 x64（build 19045） |
| 逻辑 CPU | 8 |
| 物理内存 | 约 32 GB |
| 构建产物 | `build-x64\bin\Release\raypx2.exe` |
| Relay 后端 | `windows-iocp`（默认 8 workers，由 `TqDetectRelayWorkers()` 检测） |
| 典型远端 | `172.16.10.80:8443`（Linux server，管理网） |
| 远端 SSH 用户 | `jack`（仅用于远端启停 iperf3 / 查日志；L2 内置测速**不需要** SSH） |
| 本机 Admin | `0.0.0.0:2345`（client router 模式） |
| 证书目录 | 仓库根 `cert/`（server/client 共用同一测试 CA） |

说明：

- Windows 压测**不替代** DGX 双 200G 网口回归；本方案聚焦 Windows client/server 在本机或管理网链路上的 relay 行为与稳定性。
- 若 server 运行在 Linux 远端，数据面瓶颈可能在链路、远端 CPU 或 Linux relay，需在 `summary.md` 中区分 Windows 侧指标与端到端吞吐。

### 1.1.1 T1 固定环境（当前轮次）

以下为本轮 T1 压测已确认的环境，压测前写入证据目录 `env/t1-fixture.txt`：

| 项 | 值 |
|---|---|
| Linux server 地址 | `172.16.10.80:8443` |
| Linux SSH 用户 | `jack@172.16.10.80`（可选；非 L2 门禁前置） |
| Server 状态 | `raypx2 server` 已在远端运行 |
| Server 证书 | `cert/server/server.crt` + `cert/server/server.key`（Linux 侧路径与仓库 `cert/` 一致） |
| Client CA | `cert/ca.crt`（Windows 侧 `--ca cert\ca.crt`） |
| 内置 speed test 模式 | 单 peer CLI（`--peer 172.16.10.80:8443`）；**禁止**与 `--client-config` router 配置混用 |
| SSH 免密 | **非必须**；仅 iperf3 长流（F09/F10/F14）或自动化远端运维时建议配置 |

证书 SAN 提示：默认 `cert/` 生成脚本的 server SAN **不含** `172.16.10.80`。若 TLS 握手因 IP 不匹配失败，在 Linux 重新生成证书时加入 `IP:172.16.10.80`，或 client 改用 `--peer tcpquic-server:8443` 并在本机 `hosts` 解析该主机名。

T1 快速连通（10 s）：

```powershell
$Bin = ".\build-x64\bin\Release\raypx2.exe"
& $Bin client --peer 172.16.10.80:8443 --ca cert\ca.crt --connections 1 --compress off --download-test 10
```

T1 下载基线（F02，60 s × 3 轮）：

```powershell
1..3 | ForEach-Object {
  Write-Host "=== Round $_ ==="
  & $Bin client --peer 172.16.10.80:8443 --ca cert\ca.crt --connections 1 --compress off --download-test 60
}
```

### 1.2 核心目标

| 目标 | 验收标准 |
|---|---|
| 功能正确 | smoke 与 stress 期间 HTTP CONNECT / SOCKS5 / port-forward 隧道建立成功率 ≥ 99.9% |
| 吞吐可复现 | 内置 speed test 与 iperf3 基线在相同参数下 3 轮结果偏差 ≤ 15% |
| Windows relay 稳定 | 压测期间 `raypx2` 不崩溃、不 abort；`active_relays` 与 tunnel 数一致收敛 |
| 背压有效 | 高负载下 `windows_relay_callback_receive_budget_*` 可观测但不导致持续 stall |
| 观测完整 | 每轮保存 admin metrics、trace 摘要、进程资源采样与原始测速输出 |

### 1.3 非功能目标

| 类别 | 暂定目标 |
|---|---|
| 单隧道吞吐 | loopback 内置 download ≥ 500 Mbps（Gbps 网卡环境）；跨机视链路而定，记录实测值作基线 |
| 并发隧道 | 100 条并发短连接建立 ≤ 30 s；256 条 stress 无 OOM |
| 长稳 | soak 30 min 无进程退出；`Working Set` 无单调上升（允许 ±10% 波动） |
| Admin 扰动 | 压测中每 5 s 拉取 `/metrics` 时，p95 额外延迟 < 50 ms（相对空闲基线） |
| Worker 效率 | `windows_relay_worker_lock_wait_nanos / windows_relay_worker_lock_acquire_count` 平均 < 1 ms |

## 2. 测试分层

与 `docs/todo/todo_20260702.md` 建议一致，分四层门禁：

```text
L0 单元        tcpquic_windows_relay_worker_test 等（改 src 后增量构建）
L1 smoke       scripts/test-tcpquic-proxy-windows.ps1（< 2 min）
L2 benchmark   内置 speed test + 可选 iperf3（5–15 min）
L3 stress      并发隧道 + 长流 + Admin 轮询（15–60 min）
L4 soak        单/多隧道持续 30–120 min
```

执行顺序：**L0 → L1 → L2 → L3**；L4 在发版前或回归 Windows relay 变更后执行。L2–L4 失败时必须保留证据目录，不得覆盖上一轮结果。

## 3. 拓扑与场景矩阵

### 3.1 拓扑

| 代号 | 描述 | 用途 |
|---|---|---|
| T0 | 本机 loopback：Windows server + Windows client，临时 OpenSSL 证书 | 隔离 Windows relay，排除远端变量 |
| T1 | Windows client → 远端 Linux server（如 `172.16.10.80:8443`） | 贴近当前开发/部署形态 |
| T2 | Windows server + 远端/本机 client | 验证 server 侧 Windows IOCP + dial reactor |
| T3 | Windows client router + admin + 多 peer 配置 | Admin/metrics 与控制面压力 |

本轮**优先**覆盖 T0 与 T1；T2/T3 作为扩展场景列入矩阵但不阻塞日常门禁。

### 3.2 功能场景矩阵

| ID | 场景 | 拓扑 | 流量工具 | 并发 | QUIC 连接 | 压缩 | 时长 |
|---|---|---|---|---:|---:|---|---|
| F01 | loopback smoke | T0 | curl CONNECT + SOCKS5 | 1 | 1 | off | < 2 min |
| F02 | 内置下载基线 | T0/T1 | `--download-test` | 1 | 1 | off | 60 s × 3 |
| F03 | 内置上传基线 | T0/T1 | `--upload-test` | 1 | 1 | off | 60 s × 3 |
| F04 | 内置 download sink | T0 | `--download-sink-test` | 1 | 1 | off | 60 s |
| F05 | zstd 吞吐对照 | T0 | `--download-test` | 1 | 1 | zstd | 60 s |
| F06 | 多 QUIC 连接 | T1 | `--download-test` | 1 | 4/8 | off | 60 s |
| F07 | 并发短隧道 | T0 | 自研 PS1 / Git Bash 并发脚本 | 100/256 | 4 | off | 建立阶段 |
| F08 | 并发 + zstd | T0 | 同 F07 | 128 | 8 | zstd | 建立阶段 |
| F09 | 单隧道长流 | T1 | iperf3 `-t 600` | 1 | 4 | off | 10 min |
| F10 | 多隧道长流 | T1 | iperf3 `-P 4` | 4 | 8 | off | 10 min |
| F11 | Admin 轮询压力 | T1 | 后台 curl `/metrics` + F09 | 1+poller | 4 | off | 10 min |
| F12 | worker 数扫描 | T0 | `--download-test` | 1 | 1 | off | 60 s × {1,2,4,8} |
| F13 | 连接建立风暴 | T0 | 快速循环 curl 短请求 | 50 轮 × 4 并行 | 4 | off | 5 min |
| F14 | soak 稳态 | T1 | iperf3 或 download-test | 1–4 | 4 | off | 30 min |

### 3.3 Windows relay 专项观测场景

以下场景用于验证 `docs/relay_win.md` 中近期优化的热点路径：

| ID | 注入方式 | 关注指标 | 通过条件 |
|---|---|---|---|
| W01 | 大窗口下载（F02/F09） | `windows_relay_callback_receive_copy_*`、`windows_relay_callback_receive_budget_*` | 有 copy 计量；预算拒绝偶发可接受，但不应导致吞吐归零 |
| W02 | 高并发隧道（F07） | `windows_relay_worker_lock_*`、`windows_relay_find_relay_by_id_count` | 锁等待均值 < 1 ms；无持续增长 |
| W03 | 多 active relay（F10/F14） | `windows_relay_maintenance_*`、`windows_relay_snapshot_*` | `maintenance_full_scan_count` 低频；`maintenance_drain_nanos` 不随 relay 数线性爆炸 |
| W04 | 慢 TCP 消费（限速 iperf `-b 10M`） | `windows_relay_receive_view_finish_*`、`pending_quic_receive_depth` | `receive_view_finish_not_front_count == 0`；线性查找计数为 0 或极低 |
| W05 | 高频 metrics（F11） | `windows_relay_snapshot_build_nanos`、`windows_relay_snapshot_active_relays_scanned` | snapshot 耗时 p95 < 20 ms；数据面吞吐下降 < 10% |

## 4. 工具与前置条件

### 4.1 必备

| 工具 | 用途 | 安装/检查 |
|---|---|---|
| `raypx2.exe` | 被测二进制 | `cmake --build build-x64 --config Release --target tcpquic-proxy` |
| `curl.exe` | HTTP CONNECT / Admin | Windows 10 自带 |
| `python` | 本机 HTTP 探针目标 | `python -m http.server` |
| OpenSSL | smoke 临时证书 | Git for Windows 或 OpenSSL-Win64；`scripts/test-tcpquic-proxy-windows.ps1` 会自动查找 |
| `jq`（可选） | 解析 admin JSON | `winget install jqlang.jq` 或 Git Bash 自带 |

### 4.2 推荐

| 工具 | 用途 | 备注 |
|---|---|---|
| iperf3 | 跨平台带宽基线 | Windows 可用 [iperf3 官方构建](https://github.com/esnet/iperf/releases) 或 `winget install ar51an.iperf3` |
| Git Bash / WSL | 复用 `scripts/test-tcpquic-concurrent.sh` | 并发隧道 stress 可先在 Git Bash 跑通，再逐步移植 PS1 |
| Performance Monitor / typeperf | CPU、内存、句柄采样 | 长稳与 soak 必采 |

### 4.3 T1 远端前置条件（Linux `172.16.10.80`）

| 检查项 | 命令 / 说明 |
|---|---|
| Server 进程 | Linux 上 `raypx2` 监听 `8443`（已就绪则跳过） |
| Server 证书 | `--cert cert/server/server.crt --key cert/server/server.key` |
| Client CA | Windows：`--ca cert\ca.crt` |
| 网络 | Windows → `172.16.10.80:8443` UDP/TCP 可达（QUIC） |
| SSH | 可选；L2 `--download-test` / `--upload-test` 不依赖 SSH |

Linux server 启动参考（证书路径与仓库 `cert/` 对齐时）：

```bash
cd /path/to/tcpquic-proxy
./build/bin/Release/raypx2 server \
  --listen 0.0.0.0:8443 \
  --cert cert/server/server.crt \
  --key cert/server/server.key \
  --allow-targets 0.0.0.0/0 \
  --compress off
```

### 4.4 构建与二进制约定

```powershell
# 增量构建（仅 src 变更）
cmake --build build-x64 --config Release --target tcpquic-proxy -- /m:1 /p:BuildProjectReferences=false

$Bin = "C:\src\tcpquic-proxy\build-x64\bin\Release\raypx2.exe"
```

压测前记录：

```powershell
Get-FileHash $Bin -Algorithm SHA256
git rev-parse HEAD
raypx2.exe --version  # 若支持；否则记录文件时间戳
```

## 5. 执行说明

### 5.1 L1 — loopback smoke

```powershell
cd C:\src\tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Bin .\build-x64\bin\Release\raypx2.exe -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Bin .\build-x64\bin\Release\raypx2.exe -Compress zstd -Trace
```

通过：脚本退出码 0；`-Trace` 时 `log\client.log` / `log\server.log` 含 `proxy_tunnel_ok`。

### 5.2 L2 — 内置 speed test（T0 示例）

终端 1 — server：

```powershell
$Bin = ".\build-x64\bin\Release\raypx2.exe"
# 使用 smoke 脚本生成的证书，或 cert\ 下实验证书
& $Bin server --listen 127.0.0.1:18443 --cert cert\server\server.crt --key cert\server\server.key --allow-targets 127.0.0.0/8 --compress off
```

终端 2 — client download 60 s × 3 轮：

```powershell
& $Bin client --peer 127.0.0.1:18443 --ca cert\ca.crt --connections 1 --compress off --download-test 60
```

记录每轮 stdout 中的吞吐（Mbps）、RTT、ideal_send。upload 方向将 `--download-test` 换成 `--upload-test`。

T1 远端模式：将 `--peer` 指向 `172.16.10.80:8443`，client 使用 `--ca cert\ca.crt`；**不要**与 `--client-config` router 多 peer 配置混用内置 speed test。

```powershell
& $Bin client --peer 172.16.10.80:8443 --ca cert\ca.crt --connections 1 --compress off --download-test 60
& $Bin client --peer 172.16.10.80:8443 --ca cert\ca.crt --connections 1 --compress off --upload-test 60
```

### 5.3 L2 — iperf3 长流（T1 示例）

远端 server 启动 iperf3（Linux）：

```bash
iperf3 -s -p 16001 -B 172.16.10.80
```

Windows client 经 port-forward 或 HTTP CONNECT 代理跑 iperf3（需将本地监听端口转发到远端 iperf 端口）：

```powershell
# 假设 client-config 已将 127.0.0.1:15445 转发到远端 172.16.10.80:16001
iperf3 -c 127.0.0.1 -p 15445 -t 60 -J -O 5 | Tee-Object -FilePath case\iperf.stdout.json
```

### 5.4 L3 — 并发短隧道

优先复用仓库脚本（Git Bash）：

```bash
BIN=/c/src/tcpquic-proxy/build-x64/bin/Release/raypx2.exe \
TUNNELS=100 QUIC_CONNECTIONS=4 COMPRESS=off \
./scripts/test-tcpquic-concurrent.sh
```

通过：全部隧道 HTTP 200；client/server 进程仍存活；无 `abort` 或 access violation。

后续可在 `scripts/test-tcpquic-concurrent-windows.ps1` 产品化原生 PowerShell 版本（本方案不阻塞于该脚本是否已存在）。

### 5.5 L3 — Admin metrics 轮询（F11）

参考 `docs/test/curl_test.md` 设置 `ADMIN` 与 `TOKEN`，压测期间后台执行：

```powershell
$Admin = "http://127.0.0.1:2345/api/v1"
$Token = "<从 admin-token-file 读取>"
$Auth = @{ Authorization = "Bearer $Token" }
1..120 | ForEach-Object {
    Measure-Command {
        Invoke-RestMethod -Uri "$Admin/metrics" -Headers $Auth | Out-Null
    } | Select-Object TotalMilliseconds
    Start-Sleep -Seconds 5
}
```

同时运行 F09 长流。对比空闲时与负载时的 `TotalMilliseconds` 分布。

### 5.6 L3 — Windows worker 数扫描（F12）

通过 CLI 或 JSON 覆盖 `relay.windows.worker_count`：

```powershell
& $Bin client --peer 127.0.0.1:18443 --ca cert\ca.crt --connections 1 --compress off `
  --windows-relay-worker-count 4 --download-test 60
```

对 `{1,2,4,8}` 各跑一轮，记录吞吐与 `windows_relay_worker_lock_wait_nanos`。选择吞吐/platform 平衡点写入 `summary/worker-sweep.json`。

### 5.7 推荐启动参数（压力场景）

```text
--compress off          # 吞吐基线；压缩对照单独场景
--connections 4|8       # 多 slot 轮询
--trace --trace-interval 30
--windows-relay-worker-count <n>   # 扫描场景
```

避免在首轮基线中开启 `--trace-interval 1`，高频 trace 会扰动数据面；观测专项可降至 10–30 s。

## 6. 观测与采样

### 6.1 Admin `/metrics` 关键字段

JSON 路径因版本略有差异，以下按 `relay_metrics` 语义列出 Windows 必看项：

| 字段 | 含义 |
|---|---|
| `windows_relay_worker_lock_acquire_count` | worker 锁获取次数 |
| `windows_relay_worker_lock_wait_nanos` | worker 锁等待总耗时 |
| `windows_relay_callback_receive_budget_rejected_count` | callback 复制前预算拒绝次数 |
| `windows_relay_callback_receive_budget_paused_count` | callback 暂停 QUIC receive 次数 |
| `windows_relay_callback_receive_copy_bytes` | callback 复制字节累计 |
| `windows_relay_callback_receive_copy_nanos` | callback 复制耗时累计 |
| `windows_relay_maintenance_drain_count` | maintenance 队列 drain 次数 |
| `windows_relay_maintenance_drain_nanos` | maintenance drain 耗时 |
| `windows_relay_maintenance_relays_processed` | 每轮 maintenance 处理 relay 数 |
| `windows_relay_receive_view_finish_linear_search_count` | receive view 线性查找次数（应为 0 或极低） |
| `windows_relay_receive_view_finish_not_front_count` | 非队首完成次数（应为 0） |
| `active_relays` / per-relay `queued_worker_ops` | 排队深度与背压 |

采样节奏：

- benchmark：开始前、结束后各 1 次全量 `/metrics`
- stress/soak：每 30 s 一次 `/metrics`，每 5 min 一次 `/diagnostics`
- 异常时立即补采 `/connections`、`/peers/<id>` 与 `log\*.log` 尾部 200 行

### 6.2 进程资源采样

```powershell
$id = <raypx2 PID>
while ($true) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $p) { break }
    "$(Get-Date -Format o),$($p.CPU),$($p.WorkingSet64),$($p.HandleCount)" |
        Add-Content metrics\process-sample.csv
    Start-Sleep -Seconds 10
}
```

### 6.3 Trace 摘要

启用 `--trace` 时，检查 `log\client.log` / `log\server.log` 中 `event=stats_tick` 行是否包含 `win_cb_recv_*`、`win_maint_*` 等 Windows 摘要字段（见 `docs/relay_win.md` §3.8）。

## 7. 通过 / 失败门禁

### 7.1 硬性失败（任一条即 FAIL）

- `raypx2` 进程非预期退出、崩溃、或未处理异常
- smoke 或并发隧道成功率 < 99%
- soak 期间 `Working Set` 持续上升超过 20% 且无回落
- `windows_relay_receive_view_finish_not_front_count` 在 W04/W09 期间 > 0
- Admin `/health` 在压测中连续 3 次不可达

### 7.2 软性告警（记入 summary，不单独 FAIL）

- 吞吐较上一轮基线下降 > 15%
- `windows_relay_callback_receive_budget_rejected_count` 持续单调上升
- `windows_relay_maintenance_full_scan_count` 在高 relay 数下频繁触发
- iperf3 重传率 > 1%（跨机场景）

### 7.3 报告模板

每轮测试产出 `summary/summary.md`：

```markdown
# Windows 压测摘要

- 场景: F09
- 拓扑: T1
- git: <sha>
- binary sha256: <hash>
- 结果: PASS | FAIL | WARN

## 吞吐
- download_test 平均: X Mbps（3 轮）
- iperf3 终点: Y Gbps

## Windows relay
- lock wait avg: Z us/op
- cb budget rejected: N
- maint drain total: M ms

## 资源
- peak working set: W MB
- 进程退出码: 0

## 备注
- ...
```

## 8. 证据目录结构

沿用 DGX 回归命名，便于横向对比：

```text
docs/test/windows-perf-<scenario>-<YYYYMMDD-HHMMSS>/
  env/
    git-head.txt
    git-status-short.txt
    binary.sha256
    systeminfo.txt
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
    health.json
  metrics/
    process-sample.csv
    admin-latency.csv
  proxy/
    client.stderr.log
    server.stderr.log
    client.trace.log             # 可选复制 log\client.log
  summary/
    summary.md
    worker-sweep.json            # F12 专用
```

## 9. 风险与限制

| 风险 | 说明 | 缓解 |
|---|---|---|
| 远端瓶颈掩盖 Windows 问题 | T1 吞吐受 Linux server、链路带宽限制 | 必做 T0 loopback；对比 Windows metrics 与端到端吞吐 |
| 内置 speed test 与 router 互斥 | `--download-test` 要求单 peer | 压测使用独立 CLI 或临时单 peer 配置 |
| iperf3 未安装 | 当前 PATH 可能无 iperf3 | 使用内置 speed test 作 L2 门禁；iperf3 作增强项 |
| 并发脚本依赖 Bash | `test-tcpquic-concurrent.sh` 非原生 PS1 | Git Bash 执行；或降级为 PowerShell 循环 curl |
| trace 扰动 | 过短 `trace-interval` 放大锁竞争 | 基线用 30 s；专项观测再缩短 |
| 管理网 QoS | `172.16.*` 与生产数据网不同 | 记录路由与接口；高吞吐以 T0 或专用数据网为准 |

## 10. 与 CI / 日常开发的衔接

| 触发条件 | 最低门禁 |
|---|---|
| 修改 `src/tunnel/windows_relay_worker.*` | L0 `tcpquic_windows_relay_worker_test` + L1 smoke |
| 修改 `src/runtime/relay_metrics.*` 或 trace | L0 相关单测 + L1 smoke + 一次 L2 download |
| 发版前 / 大改 Windows relay | L1 + L2 全方向 + F07 并发 + F14 soak 30 min |
| 仅文档 / 脚本 | L1 smoke |

```powershell
# L0 示例
cmake --build build-x64 --config Release --target tcpquic_windows_relay_worker_test -- /m:1 /p:BuildProjectReferences=false
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

## 11. 后续工作

- [ ] 新增 `scripts/test-tcpquic-concurrent-windows.ps1`，对齐 `test-tcpquic-concurrent.sh` 语义
- [ ] 新增 `scripts/run-windows-perf-matrix.ps1`，按本章 F01–F14 自动创建证据目录
- [ ] 将 L1 smoke 纳入 Windows CI（GitHub Actions `windows-latest`）
- [ ] 补充 k6 脚本压测 client admin（参考 `docs/server-admin-console.md` §7.4 模型）
- [x] T1 固定环境与证书约定已写入本文 §1.1.1、§4.3（`172.16.10.80:8443`，`cert/ca.crt`）
- [ ] 在 T1 环境固定 iperf3 端口转发配置，写入 `docs/test/windows-perf-t1-fixtures.json`

---

**执行入口速查**

```powershell
# 日常三步
.\scripts\test-tcpquic-proxy-windows.ps1
.\build-x64\bin\Release\raypx2.exe client --peer 172.16.10.80:8443 --ca cert\ca.crt --connections 1 --compress off --download-test 60
# 查看 metrics
curl.exe -sS -H "Authorization: Bearer <token>" http://127.0.0.1:2345/api/v1/metrics
```

## 12. 首轮执行记录（T1）

日期：2026-07-05 18:32（本地）

证据目录：`docs/test/windows-perf-t1-20260705-183250/`

| 项 | 结果 |
|---|---|
| L0 单测 | SKIP（shell 无 cmake） |
| L1 smoke | SKIP（无 openssl） |
| F02 download ×3 | FAIL（pump worker failed） |
| F03 upload | PASS（111.46 MiB/s） |
| F06 download conn=4 | FAIL |

详见 `docs/test/windows-perf-t1-20260705-183250/summary/summary.md`。下载方向失败阻塞 L3/L4；需先排查 Windows client download relay 或 speed test pump。

### 12.1 复测记录（T1）

日期：2026-07-05 23:48（本地）

证据目录：`docs/test/windows-perf-t1-20260705-234845/`

| 项 | 结果 |
|---|---|
| 当前 `raypx2.exe` 进程 | 无存活进程；基于当前构建产物重新启动受控测速 |
| F02 download ×3 | FAIL（均触发 local/server byte mismatch，平均 93.69 MiB/s） |
| F03 upload ×1 | PASS（111.46 MiB/s） |
| F06 download conn=4 | FAIL（local/server byte mismatch，111.43 MiB/s） |

详见 `docs/test/windows-perf-t1-20260705-234845/summary/summary.md`。复测结论与首轮一致：upload 方向正常，download 方向可完成测速但稳定触发 byte mismatch，因此 L3/L4 继续阻塞。
