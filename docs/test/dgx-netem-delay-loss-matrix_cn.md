# DGX netem 时延丢包矩阵测试方案

日期：2026-06-27

## 目标

在本地 2 台 DGX 的 200Gbps 直连链路上，使用 `tc netem` 模拟指定时延和丢包组合，验证 `raypx2` 在单 QUIC connection、单 HTTP CONNECT stream 下的 IO 吞吐、稳定性和运行时诊断状态。

本方案用于后续重复执行，重点约束是：netem 只允许配置在 DGX 200Gbps 数据网卡上，禁止配置在 `172.16.*` SSH 管理网口上。

参考文档：

- `docs/io-throughput-diag_cn.md`
- `docs/iw-initrtt-impact-20260623/summary.md`
- `docs/superpowers/plans/2026-06-23-dgx-10loss-io-throughput-validation-cn.md`
- `docs/superpowers/specs/2026-06-24-dgx-persistent-proxy-netem-random-validation-design.md`

## 固定环境

默认沿用当前 DGX 双机环境。若实际 IP 或网卡变化，必须先更新本节变量，再执行测试。

```bash
ROOT=/home/jack/src/tcpquic-proxy
BIN="${ROOT}/build/bin/Release/raypx2"

# SSH 控制面：必须走管理网，禁止对 172.16 网段网口加 netem。
REMOTE_SSH=jack@172.16.10.81

# 数据面：QUIC 和 iperf3 使用 200Gbps 直连链路。
LOCAL_IP=169.254.250.230
REMOTE_IP=169.254.59.196
DATA_IFACE=enp1s0f0np0

REMOTE_DIR=/home/jack/tcpquic-dgx-bin
REMOTE_BIN="${REMOTE_DIR}/raypx2"

QUIC_PORT=4433
PROXY_PORT=18080
SOCKS_PORT=19080
IPERF_PORT=16001
CLIENT_ADMIN_LISTEN=127.0.0.1:18081
SERVER_ADMIN_LISTEN=127.0.0.1:18081

IPERF_DURATION_SEC=30
IPERF_TIMEOUT_SEC=120
NETEM_LIMIT=5000000
CERT_DIR="${ROOT}/cert"
```

控制面和数据面必须分离：

- 远端命令统一通过 `REMOTE_SSH=jack@172.16.10.81` 执行。
- QUIC / iperf3 目标地址使用 `REMOTE_IP=169.254.59.196` 和 `LOCAL_IP=169.254.250.230`。
- `tc netem` 只允许作用于 `$DATA_IFACE`，不允许作用于任何 `172.16.*` 路由对应网卡。

## 测试矩阵

本轮需要验证 14 个网络组合：

| delay | loss |
| ---: | ---: |
| 10ms | 5% |
| 20ms | 5% |
| 50ms | 5% |
| 80ms | 5% |
| 100ms | 5% |
| 150ms | 5% |
| 200ms | 5% |
| 10ms | 10% |
| 20ms | 10% |
| 50ms | 10% |
| 80ms | 10% |
| 100ms | 10% |
| 150ms | 10% |
| 200ms | 10% |

推荐默认每个组合执行：

- `download`：必跑，和 `docs/io-throughput-diag_cn.md` 的主要诊断方向一致。
- `upload`：建议同步跑，作为方向性对照；如果本轮只关注 download，必须在结果摘要中明确说明未跑 upload。
- 每个方向至少 3 轮重复，记录平均值、最小值、最大值和失败次数。

## 测试生命周期

本方案的核心目标之一是验证 `raypx2` 二进制在整套矩阵中的持续运行稳定性。因此生命周期是硬约束：

- 只在整套测试开始前做一次环境清理，包括清理两端残留 `raypx2`、`iperf3` 和两端 `$DATA_IFACE` 上的旧 netem。
- 清理完成后，只启动一次本机 `raypx2 client` 和对端 `raypx2 server`。
- 测试开始后，不允许在正常 case 之间重启、替换或重新执行本机/对端 `raypx2` 二进制。
- 每个 case 之间只允许修改 netem：先清理两端 `$DATA_IFACE` 旧 qdisc，再在当前方向的发送端应用本轮 netem。
- `iperf3 server` 不是被测二进制，允许在必要时重启；但如果 `iperf3` 异常影响结果，必须记录到 `run.log` 和 `summary.md`。
- 如果测试过程中发现无法继续测试、测试数据与预期偏差过大，或需要修改/重启 `raypx2`，必须停止当前矩阵并进入问题分析。凡是修改 `raypx2` 二进制、替换二进制或重启 `raypx2` 后得到的结果，都不能并入原矩阵，必须从前置清理开始重新运行整套测试。

## Proxy 拓扑

使用常驻 proxy 拓扑，整轮矩阵内不得重启 `raypx2`，便于观察长时间运行状态。

```text
本机 iperf3 client
  -> 本机 raypx2 client, 127.0.0.1:${PROXY_PORT}
  -> QUIC over 169.254.250.230 <-> 169.254.59.196
  -> 对端 raypx2 server
  -> 对端 iperf3 server, 169.254.59.196:${IPERF_PORT}
```

`download` 使用 `iperf3 --reverse`：

```text
对端 iperf3 server 发送
  -> 对端 raypx2 server
  -> QUIC download
  -> 本机 raypx2 client
  -> 本机 iperf3 client 接收
```

`upload` 不使用 `--reverse`：

```text
本机 iperf3 client 发送
  -> 本机 raypx2 client
  -> QUIC upload
  -> 对端 raypx2 server
  -> 对端 iperf3 server 接收
```

netem 放置规则：

- download：QUIC 主数据发送端是对端 DGX，netem 加在对端 `$DATA_IFACE` egress。
- upload：QUIC 主数据发送端是本机 DGX，netem 加在本机 `$DATA_IFACE` egress。
- 每轮开始前必须清理两端 `$DATA_IFACE` 上的残留 netem，随后只在当前方向的发送端加 netem。

## Proxy 参数

所有测试轮次使用一致的 proxy 参数：

```bash
--tuning wan
--connections 1
--compress off
--diag-stats
--diag-stats-interval 1
```

说明：

- `--connections 1` 固定 peer 间单 QUIC connection。
- 每轮只启动一个 `iperf3 -c`，不使用 `-P`，从而保持单 HTTP CONNECT stream。
- 本机和对端 `raypx2` 启动时必须开启 `--admin-listen`。admin 只监听各自机器的 loopback，默认两端均使用 `127.0.0.1:18081`；对端 admin 调用必须通过 SSH 在对端本机执行 `curl`，不得绑定到 172.16 管理网或数据面公网地址。
- 默认不启用 trace 文件日志；`docs/io-throughput-diag_cn.md` 已确认 trace 会明显扰动吞吐。
- 默认不设置 `--iw` / `--initrtt-ms`；`docs/iw-initrtt-impact-20260623/summary.md` 显示它们不是当前高吞吐测试的稳定收益项。若为专项诊断显式设置，必须写入 `run.env`。

## 前置检查

### 1. 确认工具和二进制

```bash
cd "$ROOT"

rtk git rev-parse HEAD
rtk git submodule status third_party/msquic
rtk test -x "$BIN"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "test -x '$REMOTE_BIN'"

rtk iperf3 --version
rtk iperf3 --help | rtk rg -- '--proxy|connect-timeout|socks'
```

期望：

- 本机和对端 `raypx2` 均可执行。
- `iperf3` 支持 HTTP CONNECT proxy 相关参数。

### 2. 确认 SSH 管理网和数据面路由

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" 'hostname; ip addr'

rtk ip route get "$REMOTE_IP"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP'"
```

期望：

- SSH 控制面使用 `172.16.*` 管理网。
- 本机到 `$REMOTE_IP`、对端到 `$LOCAL_IP` 都走 `$DATA_IFACE`。

### 3. 网口防呆检查

执行前必须确认 `$DATA_IFACE` 不是管理网口：

```bash
rtk ip -o -4 addr show dev "$DATA_IFACE"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip -o -4 addr show dev '$DATA_IFACE'"
```

如果输出中出现 `172.16.`，立即停止，说明 `$DATA_IFACE` 配错，不能继续执行 netem。

同时记录管理网路由，确认不会对管理网网卡执行 `tc`：

```bash
rtk ip route get 172.16.10.81
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get 172.16.10.81 || true"
```

## 清理残留状态

每次完整矩阵开始前执行：

```bash
rtk pkill -9 -x raypx2 2>/dev/null || true
rtk pkill -9 -x iperf3 2>/dev/null || true
rtk sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
rtk tc -s qdisc show dev "$DATA_IFACE"

rtk ssh -o BatchMode=yes "$REMOTE_SSH" "
  killall -9 raypx2 2>/dev/null || true
  pkill -9 -x iperf3 2>/dev/null || true
  sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true
  tc -s qdisc show dev '$DATA_IFACE'
"
```

不得对 `172.16.*` 管理网口执行 `tc qdisc del/replace`。

## 结果目录

每次完整执行创建独立目录：

```bash
RESULT_ROOT="${ROOT}/docs/dgx-netem-delay-loss-matrix-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULT_ROOT"/{proxy,cases,summary}
```

顶层文件：

```text
orchestrator.log
git-version.txt
msquic-version.txt
iperf3-version.txt
iperf3-help.txt
local-route.txt
remote-route.txt
local-data-iface.txt
remote-data-iface.txt
scenario-order.csv
summary.csv
summary.md
```

每个 case 目录：

```text
cases/<order>-<direction>-<delay>ms-<loss>loss-run<run>/
  run.env
  run.log
  iperf.stdout.json
  iperf.stderr.txt
  iperf.rc
  local-qdisc-before.txt
  local-qdisc-after.txt
  local-qdisc-samples.txt
  remote-qdisc-before.txt
  remote-qdisc-after.txt
  remote-qdisc-samples.txt
  netem-active-qdisc.txt
  local-proxy.case.log
  remote-proxy.case.log
```

`proxy/` 目录应额外保存 admin 相关文件：

```text
proxy/local-admin-token-file.txt
proxy/remote-admin-token-file.txt
proxy/local-allocator-dump.before.json
proxy/remote-allocator-dump.before.json
proxy/local-allocator-dump.after.json
proxy/remote-allocator-dump.after.json
```

`summary.csv` 至少包含：

```text
order,direction,delay_ms,loss_pct,run,iperf_rc,sent_mbps,received_mbps,baseline_mbps,baseline_delta_pct,retransmits,duration_sec,netem_side,netem_iface,netem_limit,qdisc_drop_delta,qdisc_backlog_max_bytes,srtt_avg_ms,srtt_max_ms,bbr_bw_avg_mbps,bytes_in_flight_max,event_queue_full_errors_max,notes
```

## 证书准备

本测试固定使用仓库 `cert/` 目录下的长期开发/测试 mTLS 证书，不生成临时证书。

本机证书路径：

```text
cert/ca.crt
cert/server/server.crt
cert/server/server.key
cert/client/client.crt
cert/client/client.key
```

执行前检查证书文件存在：

```bash
rtk ls -l \
  "$CERT_DIR/ca.crt" \
  "$CERT_DIR/server/server.crt" \
  "$CERT_DIR/server/server.key" \
  "$CERT_DIR/client/client.crt" \
  "$CERT_DIR/client/client.key"
```

同步到对端固定目录：

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "mkdir -p ~/tcpquic-dgx-certs"
rtk rsync -az -e "ssh -o BatchMode=yes" "$CERT_DIR/" "$REMOTE_SSH:tcpquic-dgx-certs/"
```

同步后对端证书路径应为：

```text
~/tcpquic-dgx-certs/ca.crt
~/tcpquic-dgx-certs/server/server.crt
~/tcpquic-dgx-certs/server/server.key
~/tcpquic-dgx-certs/client/client.crt
~/tcpquic-dgx-certs/client/client.key
```

## 启动常驻服务

### 1. 启动对端 iperf3 server

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "
  pkill -9 -x iperf3 2>/dev/null || true
  iperf3 -s -B '$REMOTE_IP' -p '$IPERF_PORT' -D
  pgrep -a -x iperf3
"
```

### 2. 启动对端 raypx2 server

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "
  rm -f ~/dgx-netem-server.stderr.log
  LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server \
    --listen '$REMOTE_IP:$QUIC_PORT' \
    --allow-targets '$REMOTE_IP/32,127.0.0.0/8' \
    --cert ~/tcpquic-dgx-certs/server/server.crt \
    --key ~/tcpquic-dgx-certs/server/server.key \
    --ca ~/tcpquic-dgx-certs/ca.crt \
    --compress off \
    --tuning wan \
    --connections 1 \
    --admin-listen '$SERVER_ADMIN_LISTEN' \
    --diag-stats \
    --diag-stats-interval 1 \
    </dev/null >~/dgx-netem-server.stderr.log 2>&1 &
  echo \$! > ~/dgx-netem-server.pid
"
```

确认 server 就绪：

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "grep -q 'QUIC server listening' ~/dgx-netem-server.stderr.log"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "grep -q 'admin token file' ~/dgx-netem-server.stderr.log"
```

### 3. 启动本机 raypx2 client

```bash
rm -f "$RESULT_ROOT/proxy/local-client.stderr.log"

setsid "$BIN" client \
  --peer "$REMOTE_IP:$QUIC_PORT" \
  --http-listen "127.0.0.1:$PROXY_PORT" \
  --socks-listen "127.0.0.1:$SOCKS_PORT" \
  --cert "$CERT_DIR/client/client.crt" \
  --key "$CERT_DIR/client/client.key" \
  --ca "$CERT_DIR/ca.crt" \
  --connections 1 \
  --compress off \
  --tuning wan \
  --admin-listen "$CLIENT_ADMIN_LISTEN" \
  --diag-stats \
  --diag-stats-interval 1 \
  </dev/null > "$RESULT_ROOT/proxy/local-client.stderr.log" 2>&1 &

echo $! > "$RESULT_ROOT/proxy/local-client.pid"
```

确认 client 就绪：

```bash
rtk rg 'QUIC client connected|HTTP CONNECT listening' "$RESULT_ROOT/proxy/local-client.stderr.log"
rtk rg 'admin token file' "$RESULT_ROOT/proxy/local-client.stderr.log"
```

### 4. 记录 admin token 文件

`--admin-listen` 启动后，`raypx2` 会在 stderr 中打印 admin token 文件路径。测试开始前必须记录两端 token 文件，后续用于调用 admin-api 和 mimalloc 分配器 dump。

```bash
rtk rg -m1 'admin token file' "$RESULT_ROOT/proxy/local-client.stderr.log" \
  | rtk awk '{print $NF}' \
  > "$RESULT_ROOT/proxy/local-admin-token-file.txt"

rtk ssh -o BatchMode=yes "$REMOTE_SSH" \
  "grep -m1 'admin token file' ~/dgx-netem-server.stderr.log | awk '{print \$NF}'" \
  > "$RESULT_ROOT/proxy/remote-admin-token-file.txt"
```

本机 allocator dump：

```bash
rtk scripts/admin-allocator-dump.sh "$(cat "$RESULT_ROOT/proxy/local-admin-token-file.txt")" \
  > "$RESULT_ROOT/proxy/local-allocator-dump.before.json"
```

对端 allocator dump：

```bash
REMOTE_ADMIN_TOKEN="$(cat "$RESULT_ROOT/proxy/remote-admin-token-file.txt")"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" \
  "python3 - '$REMOTE_ADMIN_TOKEN' <<'PY'
import json, subprocess, sys
with open(sys.argv[1], encoding='utf-8') as f:
    d = json.load(f)
subprocess.check_call([
    'curl', '-sS', '-X', 'POST',
    '-H', f\"Authorization: {d['token_type']} {d['token']}\",
    '-H', 'Accept: application/json',
    f\"http://{d['listen']}/api/v1/memory/allocator:dump\",
])
PY" \
  > "$RESULT_ROOT/proxy/remote-allocator-dump.before.json"
```

整套矩阵结束后，应再次调用两端 allocator dump，保存为：

```text
$RESULT_ROOT/proxy/local-allocator-dump.after.json
$RESULT_ROOT/proxy/remote-allocator-dump.after.json
```

如果测试中发现内存异常，也应在停止现场后立即额外保存一份：

```text
cases/<case>/freeze/local-allocator-dump.json
cases/<case>/freeze/remote-allocator-dump.json
```

## 单轮执行步骤

以下命令中的 `<direction>`、`<delay_ms>`、`<loss_pct>`、`<case_dir>` 由调度脚本替换。

### 1. 清理两端 netem

```bash
rtk sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true"
```

### 2. 采集 qdisc before

```bash
rtk tc -s qdisc show dev "$DATA_IFACE" > "<case_dir>/local-qdisc-before.txt"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "tc -s qdisc show dev '$DATA_IFACE'" > "<case_dir>/remote-qdisc-before.txt"
```

### 3. 应用 netem

download 在对端发送端应用：

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" \
  "sudo -n tc qdisc replace dev '$DATA_IFACE' root netem delay <delay_ms>ms loss <loss_pct>% limit '$NETEM_LIMIT'"
```

upload 在本机发送端应用：

```bash
rtk sudo -n tc qdisc replace dev "$DATA_IFACE" root netem delay <delay_ms>ms loss <loss_pct>% limit "$NETEM_LIMIT"
```

应用后必须验证：

```bash
# download
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "tc qdisc show dev '$DATA_IFACE'" > "<case_dir>/netem-active-qdisc.txt"

# upload
rtk tc qdisc show dev "$DATA_IFACE" > "<case_dir>/netem-active-qdisc.txt"
```

`netem-active-qdisc.txt` 必须包含 `netem`、对应 `delay`、对应 `loss` 和 `limit 5000000`。

### 4. 采样 qdisc

测试期间每 5 秒采样两端 qdisc：

```bash
(
  while true; do
    date -Iseconds
    tc -s qdisc show dev "$DATA_IFACE" || true
    sleep 5
  done
) > "<case_dir>/local-qdisc-samples.txt" 2>&1 &
LOCAL_QDISC_SAMPLER=$!

(
  while true; do
    date -Iseconds
    ssh -o BatchMode=yes "$REMOTE_SSH" "tc -s qdisc show dev '$DATA_IFACE'" || true
    sleep 5
  done
) > "<case_dir>/remote-qdisc-samples.txt" 2>&1 &
REMOTE_QDISC_SAMPLER=$!
```

### 5. 运行 iperf3

download：

```bash
set +e
timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
  -B "$LOCAL_IP" \
  -p "$IPERF_PORT" \
  -t "$IPERF_DURATION_SEC" \
  --json \
  --connect-timeout 5000 \
  --reverse \
  --proxy "http://127.0.0.1:$PROXY_PORT" \
  > "<case_dir>/iperf.stdout.json" \
  2> "<case_dir>/iperf.stderr.txt"
IPERF_RC=$?
set -e
echo "$IPERF_RC" > "<case_dir>/iperf.rc"
```

upload：

```bash
set +e
timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
  -B "$LOCAL_IP" \
  -p "$IPERF_PORT" \
  -t "$IPERF_DURATION_SEC" \
  --json \
  --connect-timeout 5000 \
  --proxy "http://127.0.0.1:$PROXY_PORT" \
  > "<case_dir>/iperf.stdout.json" \
  2> "<case_dir>/iperf.stderr.txt"
IPERF_RC=$?
set -e
echo "$IPERF_RC" > "<case_dir>/iperf.rc"
```

### 6. 收尾采集和清理

```bash
kill "$LOCAL_QDISC_SAMPLER" "$REMOTE_QDISC_SAMPLER" 2>/dev/null || true
wait "$LOCAL_QDISC_SAMPLER" "$REMOTE_QDISC_SAMPLER" 2>/dev/null || true

rtk tc -s qdisc show dev "$DATA_IFACE" > "<case_dir>/local-qdisc-after.txt"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "tc -s qdisc show dev '$DATA_IFACE'" > "<case_dir>/remote-qdisc-after.txt"

rtk cp "$RESULT_ROOT/proxy/local-client.stderr.log" "<case_dir>/local-proxy.case.log"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "cat ~/dgx-netem-server.stderr.log" > "<case_dir>/remote-proxy.case.log"
```

如果本轮出现 `iperf.rc != 0`、JSON 缺失、吞吐为 0、超时、qdisc 不匹配、`raypx2` 进程退出，或吞吐相对基线偏差超过 20%，不要立即执行下面的 netem 清理。应先保留当前 netem 和 `raypx2` 进程现场，进入“异常判定”和“异常发生时”的流程。

仅当本轮判定正常时，才清理两端 netem 并进入下一轮：

```bash
rtk sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true"
```

更推荐调度脚本记录 proxy 日志 offset，只抽取本轮日志切片；如果脚本尚未实现 offset，允许先复制完整日志，但必须在 `summary.md` 中说明。

## 场景顺序

建议随机化执行顺序，避免固定升序时延带来的状态漂移偏差：

```text
direction,delay_ms,loss_pct,run
download,10,5,1
upload,10,5,1
...
```

如果为了便于人工观察选择固定顺序，推荐：

1. 先跑一轮 `10ms + 5%` download/upload baseline。
2. 再按随机顺序跑完整矩阵。
3. 最后复跑 `10ms + 5%` download/upload，检查整场测试后性能是否漂移。

## 结果解析

从 `iperf.stdout.json` 提取：

- `end.sum_sent.bits_per_second`
- `end.sum_received.bits_per_second`
- `end.sum_sent.retransmits`
- `start.test_start.duration`

吞吐主指标使用 receiver 侧：

```text
received_mbps = end.sum_received.bits_per_second / 1000000
sent_mbps = end.sum_sent.bits_per_second / 1000000
```

与基线偏差的计算口径：

```text
baseline_delta_pct = abs(received_mbps - baseline_mbps) / baseline_mbps * 100
```

`baseline_mbps` 优先使用本轮前置 baseline 或历史可信结果；如果没有外部基线，则使用同方向、同 delay/loss 已完成正常轮次的中位数。`baseline_delta_pct > 20` 时，本轮判定为有问题，必须停止矩阵并进入分析。

从 proxy `--diag-stats` 日志提取：

- `bbr_bw_mbps`
- `bytes_in_flight`
- `bytes_in_flight_max`
- `srtt`
- `cwnd`
- `posted`
- `ideal`
- `event_queue_full_errors`
- `quic_send_failures`
- `quic_send_api_failures`
- `quic_send_fatal_errors`
- `tcp_read_disabled_relays`
- `pending_bytes`

从 qdisc 采样提取：

- drop 增量
- backlog 最大 bytes
- backlog 最大 packets
- overlimits / requeues 是否异常增长

## 异常判定

硬异常：

- `iperf.rc != 0`
- `iperf.stdout.json` 缺失或 JSON 解析失败
- `received_mbps == 0`
- 达到 `IPERF_TIMEOUT_SEC`
- netem 应用失败，或 active qdisc 不匹配当前 delay/loss/limit
- 本机 `raypx2 client` 或对端 `raypx2 server` 进程退出
- HTTP CONNECT 代理无法建立连接
- `received_mbps` 相对同方向、同 delay/loss 基线偏差超过 20%。基线优先使用本轮前置 baseline 或历史可信结果；如果没有基线，使用同组合前 3 轮的中位数，发现偏差后停止扩展矩阵并补充分析。

软异常：

- 同一组合 3 轮吞吐波动超过 50%。
- 低时延场景吞吐明显低于高时延相邻场景，且无法由 qdisc drop/backlog 解释。
- proxy diag 出现非零 `event_queue_full_errors`、`quic_send_failures`、`quic_send_api_failures`、`quic_send_fatal_errors`。
- `srtt` 明显高于 netem 基础时延对应 RTT，且伴随吞吐塌陷。

异常发生时：

1. 立即停止继续跑矩阵。
2. 保留当前 netem 和 proxy 进程现场。
3. 采集 `ss -tanp`、`ss -uanp`、`ps -ef`、两端 qdisc、两端 proxy 完整日志。
4. 在 `anomalies/<case>/anomaly-notes.md` 记录现象、当前 netem、复现命令、基线值和偏差比例。
5. 先分析并处理问题；如果需要修改 `raypx2` 二进制、替换二进制或重启本机/对端 `raypx2`，当前整套矩阵作废，修复后必须从前置清理开始重新运行完整测试。

## 最终报告

`summary.md` 建议包含：

1. 测试环境：commit、MsQuic commit、二进制路径、两端 IP、数据网卡、SSH 管理网。
2. 执行范围：是否跑 download/upload、每个组合重复次数、是否随机顺序。
3. 网口确认：明确记录 netem 只加在 `$DATA_IFACE`，未作用于 `172.16.*` 管理网口。
4. 吞吐主表：按 delay/loss/direction 汇总平均、最小、最大、失败次数。
5. 诊断摘要：每组 `srtt`、`bbr_bw_mbps`、`bytes_in_flight_max`、qdisc drop/backlog。
6. 异常列表：失败 case、软异常、与基线偏差超过 20% 的 case、是否复现。
7. 结论：5% 与 10% 丢包下随时延变化的吞吐趋势，以及是否存在方向性差异。

## 最终清理

测试结束后必须清理两端进程和 netem：

```bash
rtk pkill -9 -x raypx2 2>/dev/null || true
rtk pkill -9 -x iperf3 2>/dev/null || true
rtk sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
rtk tc -s qdisc show dev "$DATA_IFACE"

rtk ssh -o BatchMode=yes "$REMOTE_SSH" "
  killall -9 raypx2 2>/dev/null || true
  pkill -9 -x iperf3 2>/dev/null || true
  sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true
  tc -s qdisc show dev '$DATA_IFACE'
"
```

确认两端 `tc qdisc show` 输出均不包含 `netem` 后，才算清理完成。
