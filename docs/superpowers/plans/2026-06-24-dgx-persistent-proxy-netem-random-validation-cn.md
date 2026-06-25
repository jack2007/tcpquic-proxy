# DGX Persistent Proxy Netem Random Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to execute this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 使用常驻 `tcpquic-proxy client/server` 验证 DGX 直连链路在随机 netem 时延/丢包组合下的 IO 吞吐和异常复现规律。

**Architecture:** 本机固定运行 `tcpquic-proxy client`，对端 DGX 固定运行 `tcpquic-proxy server`，所有 iperf3 download/upload 都通过本机 HTTP CONNECT 代理访问对端 iperf3 server。download 时 netem 加在对端直连网卡，upload 时 netem 加在本机直连网卡；远端控制面统一通过 `jack@172.16.10.81`，避免 netem 影响 SSH。

**Tech Stack:** Bash、系统 `iperf3`、`tc netem`、SSH 管理网、tcpquic-proxy、2xDGX 直连链路、Python 3 汇总脚本。

---

## 一、执行原则

### 1. 固定拓扑

```text
本机 iperf3 client
  -> 本机 tcpquic-proxy client, 127.0.0.1:18080
  -> QUIC over 169.254.250.230 <-> 169.254.59.196
  -> 对端 tcpquic-proxy server
  -> 对端 iperf3 server, 169.254.59.196:16001
```

download 命令使用 `--reverse`。upload 命令不使用 `--reverse`。两种方向都不改变 proxy 角色。

### 2. 控制面

远端命令必须使用：

```bash
REMOTE_SSH=jack@172.16.10.81
```

数据面仍使用：

```bash
REMOTE_IP=169.254.59.196
LOCAL_IP=169.254.250.230
```

### 3. 常驻规则

proxy 在测试开始时启动，正常测试结束时关闭。异常发生时先保留 proxy 和当前 netem，不主动清理现场。只有 proxy 已退出、无法继续建立测试连接，或完成现场冻结并需要验证重启对照时，才允许重启。

## 二、结果目录

创建一个带时间戳的结果根目录：

```text
docs/dgx-persistent-proxy-netem-random-YYYYMMDD-HHMMSS/
```

目录结构：

```text
proxy/
baseline/download/
baseline/upload/
cases/order-0001-download-010ms-10loss/
cases/order-0002-upload-500ms-10loss/
anomalies/
```

顶层文件：

```text
orchestrator.log
scenario-order.csv
summary.csv
summary.md
local-route.txt
remote-route.txt
iperf3-version.txt
iperf3-help.txt
git-version.txt
msquic-version.txt
```

每个 case 目录必须包含：

```text
iperf.stdout.json
iperf.stderr.txt
iperf.rc
run.env
run.log
local-qdisc-before.txt
local-qdisc-after.txt
local-qdisc-samples.txt
remote-qdisc-before.txt
remote-qdisc-after.txt
remote-qdisc-samples.txt
netem-apply.stdout.txt
netem-apply.stderr.txt
netem-active-qdisc.txt
netem-verify.stderr.txt
local-proxy-health-before.txt
local-proxy-health-after.txt
remote-proxy-health-before.txt
remote-proxy-health-after.txt
proxy-health-delta.txt
proxy-idle-check.txt
local-ss-after.txt
remote-ss-after.txt
local-proxy.offsets
remote-proxy.offsets
local-proxy.case.log
remote-proxy.case.log
```

## 三、任务

### Task 1: 前置检查和环境变量

**Files:**
- Read: `docs/superpowers/plans/2026-06-23-dgx-10loss-io-throughput-validation-cn.md`
- Read: `docs/superpowers/specs/2026-06-24-dgx-persistent-proxy-netem-random-validation-design.md`
- Create: `docs/dgx-persistent-proxy-netem-random-YYYYMMDD-HHMMSS/`

- [ ] **Step 1: 设置固定变量**

Run:

```bash
ROOT=/home/jack/src/tcpquic-proxy
cd "$ROOT"
set -euo pipefail

BIN="${ROOT}/build/bin/Release/tcpquic-proxy"
REMOTE_SSH=jack@172.16.10.81
REMOTE_DIR=/home/jack/tcpquic-dgx-bin
REMOTE_BIN="${REMOTE_DIR}/tcpquic-proxy"
LOCAL_IP=169.254.250.230
REMOTE_IP=169.254.59.196
IFACE=enp1s0f0np0
QUIC_PORT=4433
PROXY_PORT=18080
SOCKS_PORT=19080
IPERF_PORT=16001
IPERF_DURATION_SEC=30
IPERF_TIMEOUT_SEC=120
NETEM_LIMIT=5000000
DELAYS="${DELAYS:-10 20 30 40 50 60 70 80 90 150 300 500}"
LOSSES="${LOSSES:-10}"
SEED="${SEED:-$(date +%s)}"
RESULT_ROOT="${RESULT_ROOT:-${ROOT}/docs/dgx-persistent-proxy-netem-random-$(date +%Y%m%d-%H%M%S)}"
CERT_DIR="${RESULT_ROOT}/certs"
mkdir -p "$RESULT_ROOT" "$CERT_DIR" "$RESULT_ROOT/proxy" "$RESULT_ROOT/cases" "$RESULT_ROOT/anomalies"
```

Expected:

```text
RESULT_ROOT 指向新的测试结果目录，REMOTE_SSH 为 jack@172.16.10.81。
```

- [ ] **Step 2: 记录版本和工具能力**

Run:

```bash
rtk git rev-parse HEAD > "$RESULT_ROOT/git-version.txt"
rtk git submodule status third_party/msquic > "$RESULT_ROOT/msquic-version.txt"
rtk iperf3 --version > "$RESULT_ROOT/iperf3-version.txt" 2>&1
rtk iperf3 --help > "$RESULT_ROOT/iperf3-help.txt" 2>&1
rtk test -x "$BIN"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "test -x '$REMOTE_BIN'"
rtk rg -- '--proxy|connect-timeout|socks' "$RESULT_ROOT/iperf3-help.txt"
```

Expected:

```text
本机和对端 tcpquic-proxy 二进制都可执行，iperf3 help 包含 HTTP CONNECT proxy 支持。
```

- [ ] **Step 3: 确认数据面路由和控制面可达**

Run:

```bash
rtk ip route get "$REMOTE_IP" > "$RESULT_ROOT/local-route.txt"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP'" > "$RESULT_ROOT/remote-route.txt"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "hostname; ip addr show '$IFACE'" > "$RESULT_ROOT/remote-iface.txt"
```

Expected:

```text
本机到 169.254.59.196、对端到 169.254.250.230 都走直连网卡 enp1s0f0np0；远端控制面通过 172.16.10.81 可达。
```

### Task 2: 清理残留状态

**Files:**
- Create: `$RESULT_ROOT/preflight-cleanup.log`

- [ ] **Step 1: 清理本机残留进程和 netem**

Run:

```bash
{
  date -Iseconds
  pkill -9 -x tcpquic-proxy 2>/dev/null || true
  pkill -9 -x iperf3 2>/dev/null || true
  sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
  tc -s qdisc show dev "$IFACE" || true
} > "$RESULT_ROOT/preflight-cleanup.log" 2>&1
```

Expected:

```text
本机无残留 tcpquic-proxy、iperf3，直连网卡无 netem。
```

- [ ] **Step 2: 清理对端残留进程和 netem**

Run:

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; tc -s qdisc show dev '$IFACE'; true" \
  >> "$RESULT_ROOT/preflight-cleanup.log" 2>&1
```

Expected:

```text
对端无残留 tcpquic-proxy、iperf3，直连网卡无 netem。SSH 命令通过管理网执行。
```

### Task 3: 证书准备

**Files:**
- Create: `$CERT_DIR/ca.crt`
- Create: `$CERT_DIR/server.crt`
- Create: `$CERT_DIR/client.crt`

- [ ] **Step 1: 生成测试证书**

Run:

```bash
rtk openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
  -subj "/CN=dgx-persistent-netem-ca" -days 1 -sha256

rtk tee "$CERT_DIR/server.ext" >/dev/null <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:${REMOTE_IP},IP:${LOCAL_IP}
EOF

rtk tee "$CERT_DIR/client.ext" >/dev/null <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:dgx-persistent-client
EOF

for n in server client; do
  rtk openssl req -newkey rsa:2048 -nodes \
    -keyout "$CERT_DIR/$n.key" -out "$CERT_DIR/$n.csr" \
    -subj "/CN=$n"
  rtk openssl x509 -req -in "$CERT_DIR/$n.csr" \
    -CA "$CERT_DIR/ca.crt" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
    -out "$CERT_DIR/$n.crt" -days 1 -sha256 \
    -extfile "$CERT_DIR/$n.ext"
done
```

Expected:

```text
server 证书 SAN 包含 169.254.59.196 和 169.254.250.230。
```

- [ ] **Step 2: 同步证书到对端**

Run:

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "mkdir -p ~/tcpquic-dgx-certs"
rtk rsync -az -e "ssh -o BatchMode=yes" "$CERT_DIR/" "$REMOTE_SSH:tcpquic-dgx-certs/"
```

Expected:

```text
对端 ~/tcpquic-dgx-certs/ 下有 ca/server/client 证书和 key。
```

### Task 4: 启动常驻服务

**Files:**
- Create: `$RESULT_ROOT/proxy/local-client.stderr.log`
- Create: `$RESULT_ROOT/proxy/remote-server.stderr.log`
- Create: `$RESULT_ROOT/proxy/pids.env`

- [ ] **Step 1: 启动对端 iperf3 server**

Run:

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "pkill -9 -x iperf3 2>/dev/null; iperf3 -s -B '$REMOTE_IP' -p '$IPERF_PORT' -D"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "pgrep -a -x iperf3"
```

Expected:

```text
对端 iperf3 server 监听 169.254.59.196:16001。
```

- [ ] **Step 2: 启动对端 tcpquic-proxy server**

Run:

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server \
  --listen '$REMOTE_IP:$QUIC_PORT' \
  --allow-targets '$REMOTE_IP/32,127.0.0.0/8' \
  --cert ~/tcpquic-dgx-certs/server.crt \
  --key ~/tcpquic-dgx-certs/server.key \
  --ca ~/tcpquic-dgx-certs/ca.crt \
  --compress off \
  --tuning wan \
  --connections 1 \
  --diag-stats \
  --diag-stats-interval 1 \
  </dev/null >~/dgx-persistent-server.stderr.log 2>&1 & echo \$! > ~/dgx-persistent-server.pid"
```

Expected:

```text
对端 tcpquic-proxy server 进程启动，监听 169.254.59.196:4433。
```

- [ ] **Step 3: 通过管理网镜像对端 proxy 连续日志**

Run:

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "tail -n +1 -F ~/dgx-persistent-server.stderr.log" \
  > "$RESULT_ROOT/proxy/remote-server.stderr.log" 2>&1 &
REMOTE_LOG_TAIL_PID=$!
echo "REMOTE_LOG_TAIL_PID=$REMOTE_LOG_TAIL_PID" >> "$RESULT_ROOT/proxy/pids.env"
```

Expected:

```text
$RESULT_ROOT/proxy/remote-server.stderr.log 持续追加对端 proxy server 日志，日志传输走 172.16.10.81 管理网。
```

- [ ] **Step 4: 启动本机 tcpquic-proxy client**

Run:

```bash
"$BIN" client \
  --peer "$REMOTE_IP:$QUIC_PORT" \
  --http-listen "127.0.0.1:$PROXY_PORT" \
  --socks-listen "127.0.0.1:$SOCKS_PORT" \
  --cert "$CERT_DIR/client.crt" \
  --key "$CERT_DIR/client.key" \
  --ca "$CERT_DIR/ca.crt" \
  --connections 1 \
  --compress off \
  --tuning wan \
  --diag-stats \
  --diag-stats-interval 1 \
  > "$RESULT_ROOT/proxy/local-client.stderr.log" 2>&1 &
LOCAL_PROXY_PID=$!
echo "LOCAL_PROXY_PID=$LOCAL_PROXY_PID" >> "$RESULT_ROOT/proxy/pids.env"
```

Expected:

```text
本机 tcpquic-proxy client 持续运行，HTTP CONNECT 监听 127.0.0.1:18080。
```

- [ ] **Step 5: 等待服务就绪并记录 PID**

Run:

```bash
for i in $(seq 1 120); do
  if rtk rg -q "HTTP CONNECT listening" "$RESULT_ROOT/proxy/local-client.stderr.log" && \
     rtk rg -q "QUIC client connected" "$RESULT_ROOT/proxy/local-client.stderr.log"; then
    break
  fi
  sleep 0.5
done
rtk rg -q "HTTP CONNECT listening" "$RESULT_ROOT/proxy/local-client.stderr.log"
rtk rg -q "QUIC client connected" "$RESULT_ROOT/proxy/local-client.stderr.log"

rtk ssh -o BatchMode=yes "$REMOTE_SSH" "cat ~/dgx-persistent-server.pid; grep -E 'QUIC server listening|server listening' ~/dgx-persistent-server.stderr.log | tail -20" \
  > "$RESULT_ROOT/proxy/remote-server-ready.txt"
rtk rg -q "QUIC server listening|server listening" "$RESULT_ROOT/proxy/remote-server-ready.txt"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "cat ~/dgx-persistent-server.pid" \
  | awk '{print "REMOTE_PROXY_PID="$1}' >> "$RESULT_ROOT/proxy/pids.env"
```

Expected:

```text
本机日志包含 HTTP CONNECT listening 和 QUIC client connected；对端日志包含 QUIC server listening。
```

### Task 5: 生成随机场景顺序

**Files:**
- Create: `$RESULT_ROOT/scenario-order.csv`

- [ ] **Step 1: 生成随机顺序**

Run:

```bash
rtk python3 - "$RESULT_ROOT/scenario-order.csv" "$SEED" "$DELAYS" "$LOSSES" <<'PY'
import csv
import random
import sys

out, seed, delays, losses = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
items = []
for direction in ("download", "upload"):
    for delay in [int(x) for x in delays.split()]:
        for loss_raw in losses.split():
            loss_percent = loss_raw[:-1] if loss_raw.endswith("%") else loss_raw
            loss = float(loss_percent)
            loss_label = str(int(loss)) if loss.is_integer() else str(loss).replace(".", "p")
            items.append({
                "direction": direction,
                "delay_ms": delay,
                "loss_percent": loss_percent,
                "loss_label": loss_label,
            })
random.Random(seed).shuffle(items)
with open(out, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["order_index", "seed", "direction", "delay_ms", "loss_percent", "loss_label"])
    w.writeheader()
    for idx, item in enumerate(items, 1):
        item["order_index"] = idx
        item["seed"] = seed
        w.writerow(item)
print(out)
PY
```

Expected:

```text
scenario-order.csv 包含 direction、delay_ms、loss_percent，执行顺序随机且 seed 可复现。
```

### Task 6: Baseline 验证

**Files:**
- Create: `$RESULT_ROOT/baseline/download/*`
- Create: `$RESULT_ROOT/baseline/upload/*`

- [ ] **Step 1: 清除本机和对端 netem**

Run:

```bash
rtk sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; true"
```

Expected:

```text
本机和对端直连网卡均无 netem。
```

- [ ] **Step 2: 跑 download baseline**

Run:

```bash
case_dir="$RESULT_ROOT/baseline/download"
mkdir -p "$case_dir"
set +e
rtk iperf3 -c "$REMOTE_IP" \
  -B "$LOCAL_IP" \
  -p "$IPERF_PORT" \
  -t "$IPERF_DURATION_SEC" \
  --json \
  --connect-timeout 5000 \
  --reverse \
  --proxy "http://127.0.0.1:$PROXY_PORT" \
  > "$case_dir/iperf.stdout.json" \
  2> "$case_dir/iperf.stderr.txt"
rc=$?
set -e
echo "$rc" > "$case_dir/iperf.rc"
```

Expected:

```text
iperf.rc 为 0，JSON 中 end.sum_received.bits_per_second 有有效值。
```

- [ ] **Step 3: 跑 upload baseline**

Run:

```bash
case_dir="$RESULT_ROOT/baseline/upload"
mkdir -p "$case_dir"
set +e
rtk iperf3 -c "$REMOTE_IP" \
  -B "$LOCAL_IP" \
  -p "$IPERF_PORT" \
  -t "$IPERF_DURATION_SEC" \
  --json \
  --connect-timeout 5000 \
  --proxy "http://127.0.0.1:$PROXY_PORT" \
  > "$case_dir/iperf.stdout.json" \
  2> "$case_dir/iperf.stderr.txt"
rc=$?
set -e
echo "$rc" > "$case_dir/iperf.rc"
```

Expected:

```text
iperf.rc 为 0，JSON 中 end.sum_received.bits_per_second 有有效值。
```

### Task 7: 执行随机矩阵

**Files:**
- Create: `$RESULT_ROOT/cases/order-*/`
- Append: `$RESULT_ROOT/orchestrator.log`

- [ ] **Step 1: 定义 qdisc 和日志辅助函数**

Run in the shell that holds the variables from Task 1:

```bash
. "$RESULT_ROOT/proxy/pids.env"

remote_tc_show() {
  ssh -o BatchMode=yes "$REMOTE_SSH" "tc -s qdisc show dev '$IFACE'"
}

clear_both_netem() {
  sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
  ssh -o BatchMode=yes "$REMOTE_SSH" "sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; true"
}

apply_case_netem() {
  direction="$1"
  delay_ms="$2"
  loss_percent="$3"
  if [ "$direction" = download ]; then
    ssh -o BatchMode=yes "$REMOTE_SSH" "sudo -n tc qdisc replace dev '$IFACE' root netem delay '${delay_ms}ms' loss '${loss_percent}%' limit '$NETEM_LIMIT'"
  else
    sudo -n tc qdisc replace dev "$IFACE" root netem delay "${delay_ms}ms" loss "${loss_percent}%" limit "$NETEM_LIMIT"
  fi
}

verify_case_netem() {
  direction="$1"
  delay_ms="$2"
  loss_percent="$3"
  if [ "$direction" = download ]; then
    qdisc=$(ssh -o BatchMode=yes "$REMOTE_SSH" "tc qdisc show dev '$IFACE'")
  else
    qdisc=$(tc qdisc show dev "$IFACE")
  fi
  printf '%s\n' "$qdisc"
  printf '%s\n' "$qdisc" | grep -E "netem.*delay ${delay_ms}ms" >/dev/null
  if [ "$loss_percent" != "0" ] && [ "$loss_percent" != "0.0" ]; then
    printf '%s\n' "$qdisc" | grep -E "loss ${loss_percent}%" >/dev/null
  fi
}

sample_qdisc() {
  local_out="$1"
  remote_out="$2"
  (
    while true; do
      date -Iseconds
      tc -s qdisc show dev "$IFACE" || true
      sleep 5
    done
  ) > "$local_out" 2>&1 &
  local_pid=$!
  (
    while true; do
      date -Iseconds
      echo "[remote]"
      ssh -o BatchMode=yes "$REMOTE_SSH" "tc -s qdisc show dev '$IFACE'" || true
      sleep 5
    done
  ) > "$remote_out" 2>&1 &
  remote_pid=$!
  echo "$local_pid $remote_pid"
}

capture_proxy_health() {
  phase="$1"
  case_dir="$2"
  local_pid="$LOCAL_PROXY_PID"
  remote_pid=$(ssh -o BatchMode=yes "$REMOTE_SSH" "cat ~/dgx-persistent-server.pid")
  {
    echo "phase=$phase"
    echo "time=$(date -Iseconds)"
    echo "pid=$local_pid"
    ps -o pid,ppid,stat,etime,rss,vsz,nlwp,cmd -p "$local_pid" || true
    awk '/VmRSS|VmHWM|VmSize|VmData|Threads|FDSize/ {print}' "/proc/$local_pid/status" 2>/dev/null || true
    echo "fd_count=$(find "/proc/$local_pid/fd" -maxdepth 1 -type l 2>/dev/null | wc -l)"
    echo "tcp_sockets=$(ss -tanp 2>/dev/null | grep -F "$local_pid" | wc -l)"
    echo "udp_sockets=$(ss -uanp 2>/dev/null | grep -F "$local_pid" | wc -l)"
  } > "$case_dir/local-proxy-health-${phase}.txt"
  ssh -o BatchMode=yes "$REMOTE_SSH" "pid='$remote_pid'; {
    echo phase='$phase';
    echo time=\$(date -Iseconds);
    echo pid=\$pid;
    ps -o pid,ppid,stat,etime,rss,vsz,nlwp,cmd -p \"\$pid\" || true;
    awk '/VmRSS|VmHWM|VmSize|VmData|Threads|FDSize/ {print}' \"/proc/\$pid/status\" 2>/dev/null || true;
    echo fd_count=\$(find \"/proc/\$pid/fd\" -maxdepth 1 -type l 2>/dev/null | wc -l);
    echo tcp_sockets=\$(ss -tanp 2>/dev/null | grep -F \"\$pid\" | wc -l);
    echo udp_sockets=\$(ss -uanp 2>/dev/null | grep -F \"\$pid\" | wc -l);
  }" > "$case_dir/remote-proxy-health-${phase}.txt"
}

write_proxy_health_delta() {
  case_dir="$1"
  python3 - "$case_dir" <<'PY'
import pathlib
import re
import sys

case = pathlib.Path(sys.argv[1])

def read(path):
    try:
        return path.read_text(errors="ignore")
    except FileNotFoundError:
        return ""

def metric(text, name):
    if name.startswith("Vm"):
        m = re.search(rf"^{name}:\s+(\d+)\s+kB", text, re.M)
    else:
        m = re.search(rf"^{name}=(\d+)", text, re.M)
    return int(m.group(1)) if m else 0

for side in ("local", "remote"):
    before = read(case / f"{side}-proxy-health-before.txt")
    after = read(case / f"{side}-proxy-health-after.txt")
    values = {
        "rss_delta_kb": metric(after, "VmRSS") - metric(before, "VmRSS"),
        "vmsize_delta_kb": metric(after, "VmSize") - metric(before, "VmSize"),
        "vmdata_delta_kb": metric(after, "VmData") - metric(before, "VmData"),
        "fd_delta": metric(after, "fd_count") - metric(before, "fd_count"),
        "tcp_socket_delta": metric(after, "tcp_sockets") - metric(before, "tcp_sockets"),
        "udp_socket_delta": metric(after, "udp_sockets") - metric(before, "udp_sockets"),
    }
    for key, value in values.items():
        print(f"{side}_{key}={value}")
PY
}

last_diag_line() {
  log="$1"
  rg "tcpquic-proxy diag_stats:" "$log" | tail -1 || true
}

diag_line_idle() {
  line="$1"
  printf '%s\n' "$line" | grep -q "active_relays=0" && \
  printf '%s\n' "$line" | grep -q "active_quic_send_relays=0" && \
  printf '%s\n' "$line" | grep -q "outstanding_quic_send_bytes=0" && \
  printf '%s\n' "$line" | grep -q "pending_tcp_write_bytes=0"
}

wait_proxy_idle() {
  case_dir="$1"
  for i in $(seq 1 15); do
    sleep 1
    local_line=$(last_diag_line "$RESULT_ROOT/proxy/local-client.stderr.log")
    remote_line=$(ssh -o BatchMode=yes "$REMOTE_SSH" "grep 'tcpquic-proxy diag_stats:' ~/dgx-persistent-server.stderr.log | tail -1" || true)
    {
      echo "attempt=$i"
      echo "local=$local_line"
      echo "remote=$remote_line"
    } > "$case_dir/proxy-idle-check.txt"
    if diag_line_idle "$local_line" && diag_line_idle "$remote_line"; then
      return 0
    fi
  done
  return 1
}
```

Expected:

```text
download 时 apply_case_netem 操作对端直连网卡，upload 时操作本机直连网卡。
```

- [ ] **Step 2: 按 scenario-order.csv 执行每轮**

Run:

```bash
tail -n +2 "$RESULT_ROOT/scenario-order.csv" | while IFS=, read -r order_index seed direction delay_ms loss_percent loss_label; do
  case_dir="$RESULT_ROOT/cases/order-$(printf '%04d' "$order_index")-${direction}-$(printf '%03d' "$delay_ms")ms-${loss_label}loss"
  mkdir -p "$case_dir"
  {
    echo "order_index=$order_index"
    echo "seed=$seed"
    echo "direction=$direction"
    echo "delay_ms=$delay_ms"
    echo "loss_percent=$loss_percent"
    echo "netem_side=$([ "$direction" = download ] && echo remote || echo local)"
    echo "netem_limit=$NETEM_LIMIT"
    echo "started_at=$(date -Iseconds)"
  } > "$case_dir/run.env"

  echo "[$(date -Iseconds)] start order=$order_index direction=$direction delay=${delay_ms}ms loss=${loss_percent}%" \
    | tee -a "$RESULT_ROOT/orchestrator.log" "$case_dir/run.log"

  capture_proxy_health before "$case_dir"
  stat -c '%s' "$RESULT_ROOT/proxy/local-client.stderr.log" > "$case_dir/local-proxy.offsets"
  ssh -o BatchMode=yes "$REMOTE_SSH" "stat -c '%s' ~/dgx-persistent-server.stderr.log" > "$case_dir/remote-proxy.offsets"

  tc -s qdisc show dev "$IFACE" > "$case_dir/local-qdisc-before.txt" || true
  remote_tc_show > "$case_dir/remote-qdisc-before.txt" || true
  clear_both_netem
  set +e
  apply_case_netem "$direction" "$delay_ms" "$loss_percent" > "$case_dir/netem-apply.stdout.txt" 2> "$case_dir/netem-apply.stderr.txt"
  netem_apply_rc=$?
  verify_case_netem "$direction" "$delay_ms" "$loss_percent" > "$case_dir/netem-active-qdisc.txt" 2> "$case_dir/netem-verify.stderr.txt"
  netem_verify_rc=$?
  set -e
  if [ "$netem_apply_rc" -ne 0 ] || [ "$netem_verify_rc" -ne 0 ]; then
    echo "125" > "$case_dir/iperf.rc"
    : > "$case_dir/iperf.stdout.json"
    {
      echo "netem_apply_rc=$netem_apply_rc"
      echo "netem_verify_rc=$netem_verify_rc"
      cat "$case_dir/netem-apply.stderr.txt"
      cat "$case_dir/netem-verify.stderr.txt"
    } > "$case_dir/iperf.stderr.txt"
    echo "[$(date -Iseconds)] anomaly detected before iperf: netem apply/verify failed: $case_dir" \
      | tee -a "$RESULT_ROOT/orchestrator.log" "$case_dir/run.log"
    capture_proxy_health after "$case_dir"
    write_proxy_health_delta "$case_dir" > "$case_dir/proxy-health-delta.txt"
    echo "$case_dir" > "$RESULT_ROOT/anomaly-current-case.txt"
    break
  fi

  sampler_pids=$(sample_qdisc "$case_dir/local-qdisc-samples.txt" "$case_dir/remote-qdisc-samples.txt")

  set +e
  if [ "$direction" = download ]; then
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
      -B "$LOCAL_IP" \
      -p "$IPERF_PORT" \
      -t "$IPERF_DURATION_SEC" \
      --json \
      --connect-timeout 5000 \
      --reverse \
      --proxy "http://127.0.0.1:$PROXY_PORT" \
      > "$case_dir/iperf.stdout.json" \
      2> "$case_dir/iperf.stderr.txt"
  else
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
      -B "$LOCAL_IP" \
      -p "$IPERF_PORT" \
      -t "$IPERF_DURATION_SEC" \
      --json \
      --connect-timeout 5000 \
      --proxy "http://127.0.0.1:$PROXY_PORT" \
      > "$case_dir/iperf.stdout.json" \
      2> "$case_dir/iperf.stderr.txt"
  fi
  rc=$?
  set -e
  echo "$rc" > "$case_dir/iperf.rc"

  for sampler_pid in $sampler_pids; do
    kill "$sampler_pid" 2>/dev/null || true
  done
  for sampler_pid in $sampler_pids; do
    wait "$sampler_pid" 2>/dev/null || true
  done
  tc -s qdisc show dev "$IFACE" > "$case_dir/local-qdisc-after.txt" || true
  remote_tc_show > "$case_dir/remote-qdisc-after.txt" || true
  ss -tanp > "$case_dir/local-ss-after.txt" 2>&1 || true
  ssh -o BatchMode=yes "$REMOTE_SSH" "ss -tanp; echo '--- udp ---'; ss -uanp" > "$case_dir/remote-ss-after.txt" 2>&1 || true

  local_start=$(sed -n '1p' "$case_dir/local-proxy.offsets")
  local_end=$(stat -c '%s' "$RESULT_ROOT/proxy/local-client.stderr.log")
  printf '%s\n%s\n' "$local_start" "$local_end" > "$case_dir/local-proxy.offsets"
  tail -c +"$((local_start + 1))" "$RESULT_ROOT/proxy/local-client.stderr.log" > "$case_dir/local-proxy.case.log"

  remote_start=$(sed -n '1p' "$case_dir/remote-proxy.offsets")
  remote_end=$(ssh -o BatchMode=yes "$REMOTE_SSH" "stat -c '%s' ~/dgx-persistent-server.stderr.log")
  printf '%s\n%s\n' "$remote_start" "$remote_end" > "$case_dir/remote-proxy.offsets"
  ssh -o BatchMode=yes "$REMOTE_SSH" "tail -c +$((remote_start + 1)) ~/dgx-persistent-server.stderr.log" > "$case_dir/remote-proxy.case.log"

  set +e
  wait_proxy_idle "$case_dir"
  idle_rc=$?
  set -e
  echo "proxy_idle_rc=$idle_rc" >> "$case_dir/run.env"
  capture_proxy_health after "$case_dir"
  write_proxy_health_delta "$case_dir" > "$case_dir/proxy-health-delta.txt"

  echo "[$(date -Iseconds)] finish order=$order_index rc=$rc" \
    | tee -a "$RESULT_ROOT/orchestrator.log" "$case_dir/run.log"

  if [ "$rc" -eq 0 ] && [ "$idle_rc" -eq 0 ]; then
    clear_both_netem
  else
    echo "[$(date -Iseconds)] anomaly detected, keeping proxy and netem in place: $case_dir rc=$rc idle_rc=$idle_rc" \
      | tee -a "$RESULT_ROOT/orchestrator.log" "$case_dir/run.log"
    echo "$case_dir" > "$RESULT_ROOT/anomaly-current-case.txt"
    break
  fi
done
```

Expected:

```text
每个随机 case 都生成 iperf、qdisc、proxy 日志切片、proxy health 前后快照和 RSS/FD/socket delta。正常 case 必须同时满足 iperf rc 为 0 且 client/server diag idle 收敛；否则保留 proxy 和当前 netem 并暂停主矩阵。
```

每轮健康检查判定：

```text
proxy_idle_rc=0: client/server 最新 diag_stats 都显示 active_relays=0、active_quic_send_relays=0、outstanding_quic_send_bytes=0、pending_tcp_write_bytes=0
rss_delta_kb: 单轮 VmRSS 增量；单轮超过 262144 KiB 标记为疑似泄漏
fd_delta: 单轮 fd_count 增量；单轮超过 4 标记为疑似 fd 泄漏
tcp_socket_delta: 单轮 proxy 相关 TCP socket 增量；单轮超过 2 标记为疑似 tunnel/socket 未关闭
diag error counters: fatal_relay_resets、tcp_hard_errors、quic_send_failures、quic_send_api_failures、quic_send_fatal_errors、event_queue_full_errors 任一非零都写入 Notes
```

### Task 8: 异常现场冻结和复现

**Files:**
- Create: `$RESULT_ROOT/anomalies/<case-name>/`

- [ ] **Step 1: 冻结异常现场**

For the anomalous case directory:

```bash
bad_case=$(cat "$RESULT_ROOT/anomaly-current-case.txt")
anomaly_dir="$RESULT_ROOT/anomalies/$(basename "$bad_case")"
mkdir -p "$anomaly_dir"
. "$RESULT_ROOT/proxy/pids.env"
cp "$bad_case/run.env" "$anomaly_dir/anomaly.env"
cp "$RESULT_ROOT/proxy/pids.env" "$anomaly_dir/proxy-pids.txt"
cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$anomaly_dir/local-proxy.full-at-freeze.log"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "cat ~/dgx-persistent-server.stderr.log" > "$anomaly_dir/remote-proxy.full-at-freeze.log"
ps -fp "$LOCAL_PROXY_PID" > "$anomaly_dir/local-proxy-ps.txt" 2>&1 || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "cat ~/dgx-persistent-server.pid; ps -fp \$(cat ~/dgx-persistent-server.pid)" > "$anomaly_dir/remote-proxy-ps.txt" 2>&1 || true
cat "/proc/$LOCAL_PROXY_PID/status" > "$anomaly_dir/local-proc-status.txt" 2>&1 || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "cat /proc/\$(cat ~/dgx-persistent-server.pid)/status" > "$anomaly_dir/remote-proc-status.txt" 2>&1 || true
ss -tanp > "$anomaly_dir/local-ss-tcp.txt" 2>&1 || true
ss -uanp > "$anomaly_dir/local-ss-udp.txt" 2>&1 || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ss -tanp; echo '--- udp ---'; ss -uanp" > "$anomaly_dir/remote-ss.txt" 2>&1 || true
ip -s link show "$IFACE" > "$anomaly_dir/local-ip-link.txt" 2>&1 || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip -s link show '$IFACE'" > "$anomaly_dir/remote-ip-link.txt" 2>&1 || true
tc -s qdisc show dev "$IFACE" > "$anomaly_dir/local-qdisc-freeze.txt" 2>&1 || true
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "tc -s qdisc show dev '$IFACE'" > "$anomaly_dir/remote-qdisc-freeze.txt" 2>&1 || true
```

Expected:

```text
异常目录包含 full proxy 日志、qdisc、socket、进程和网卡状态。此步骤不停止 proxy，不删除 netem。
```

- [ ] **Step 2: 同一现场重复复现 3 次**

Run after sourcing `bad_case/run.env`:

```bash
. "$bad_case/run.env"
for attempt in 1 2 3; do
  repro_dir="$anomaly_dir/repro-same-netem-attempt-$attempt"
  mkdir -p "$repro_dir"
  set +e
  if [ "$direction" = download ]; then
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
      -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" \
      --json --connect-timeout 5000 --reverse \
      --proxy "http://127.0.0.1:$PROXY_PORT" \
      > "$repro_dir/iperf.stdout.json" 2> "$repro_dir/iperf.stderr.txt"
  else
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
      -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" \
      --json --connect-timeout 5000 \
      --proxy "http://127.0.0.1:$PROXY_PORT" \
      > "$repro_dir/iperf.stdout.json" 2> "$repro_dir/iperf.stderr.txt"
  fi
  echo "$?" > "$repro_dir/iperf.rc"
  set -e
done
```

Expected:

```text
同一 proxy 进程、同一 netem 规则下获得 3 次复现样本。
```

- [ ] **Step 3: 删除并重建同一 netem 后复现 3 次**

Run:

```bash
clear_both_netem
apply_case_netem "$direction" "$delay_ms" "$loss_percent"
for attempt in 1 2 3; do
  repro_dir="$anomaly_dir/repro-reapply-netem-attempt-$attempt"
  mkdir -p "$repro_dir"
  set +e
  if [ "$direction" = download ]; then
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
      -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" \
      --json --connect-timeout 5000 --reverse \
      --proxy "http://127.0.0.1:$PROXY_PORT" \
      > "$repro_dir/iperf.stdout.json" 2> "$repro_dir/iperf.stderr.txt"
  else
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
      -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" \
      --json --connect-timeout 5000 \
      --proxy "http://127.0.0.1:$PROXY_PORT" \
      > "$repro_dir/iperf.stdout.json" 2> "$repro_dir/iperf.stderr.txt"
  fi
  echo "$?" > "$repro_dir/iperf.rc"
  set -e
done
```

Expected:

```text
判断异常是否依赖 qdisc 当前实例，还是只依赖 direction/delay/loss 或 proxy 长期状态。
```

- [ ] **Step 4: 写异常复现笔记**

Create `$anomaly_dir/anomaly-notes.md` with this required structure:

```markdown
# Anomaly Notes

- Case:
- First failure:
- Proxy restarted before freeze: no
- Netem side:
- Same-netem reproduction:
- Reapply-netem reproduction:
- Neighbor/direction checks:
- Current rule:
- Restart comparison:
- Conclusion:
```

Expected:

```text
异常笔记明确记录是否可复现、依赖哪些条件、是否需要重启 proxy 对照。
```

### Task 9: 汇总结果

**Files:**
- Create: `$RESULT_ROOT/summary.csv`
- Create: `$RESULT_ROOT/summary.md`

- [ ] **Step 1: 生成 summary.csv 和 summary.md**

Run:

```bash
rtk python3 - "$RESULT_ROOT" <<'PY'
import csv
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])

def text(path):
    try:
        return path.read_text(errors="ignore")
    except FileNotFoundError:
        return ""

def parse_iperf(case):
    rc = text(case / "iperf.rc").strip() or "missing"
    sent = recv = retrans = jitter = ""
    note = ""
    try:
        data = json.loads(text(case / "iperf.stdout.json"))
        end = data.get("end", {})
        ss = end.get("sum_sent", {}) or {}
        rr = end.get("sum_received", {}) or {}
        sent = f"{float(ss.get('bits_per_second', 0)) / 1_000_000:.2f}"
        recv = f"{float(rr.get('bits_per_second', 0)) / 1_000_000:.2f}"
        retrans = ss.get("retransmits", "")
        jitter = rr.get("jitter_ms", "") or ss.get("jitter_ms", "") or ""
    except Exception as exc:
        note = f"json:{exc}"
    return rc, sent, recv, retrans, jitter, note

def env(case):
    out = {}
    for line in text(case / "run.env").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    return out

def nums(log, name):
    values = []
    for m in re.finditer(rf"{re.escape(name)}=([0-9.]+)", log):
        try:
            values.append(float(m.group(1)))
        except ValueError:
            pass
    return values

def num_delta(values):
    if not values:
        return 0
    if len(values) == 1:
        return int(values[0])
    return int(values[-1] - values[0])

def keyvals(path):
    out = {}
    for line in text(path).splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out

def dropped(path):
    m = re.search(r"dropped\s+(\d+)", text(path))
    return int(m.group(1)) if m else 0

def backlog(path):
    max_bytes = max_packets = 0
    for m in re.finditer(r"backlog\s+([0-9.]+)([KMG]?b)\s+(\d+)p", text(path), re.I):
        mult = {"b": 1, "kb": 1000, "mb": 1000000, "gb": 1000000000}.get(m.group(2).lower(), 1)
        max_bytes = max(max_bytes, int(float(m.group(1)) * mult))
        max_packets = max(max_packets, int(m.group(3)))
    return max_bytes, max_packets

rows = []
for case in sorted((root / "cases").glob("order-*")):
    e = env(case)
    rc, sent, recv, retrans, jitter, note = parse_iperf(case)
    direction = e.get("direction", "")
    log = text(case / ("remote-proxy.case.log" if direction == "download" else "local-proxy.case.log"))
    srtt = nums(log, "srtt")
    bbr = nums(log, "bbr_bw_mbps")
    bif = nums(log, "bytes_in_flight_max")
    lost = nums(log, "lost_retransmittable_bytes")
    eq = nums(log, "event_queue_full_errors")
    fatal_resets = nums(log, "fatal_relay_resets")
    tcp_hard = nums(log, "tcp_hard_errors")
    send_fail = nums(log, "quic_send_failures")
    send_api_fail = nums(log, "quic_send_api_failures")
    send_fatal = nums(log, "quic_send_fatal_errors")
    active_relays = nums(log, "active_relays")
    active_quic_send_relays = nums(log, "active_quic_send_relays")
    before = dropped(case / (f"{'remote' if direction == 'download' else 'local'}-qdisc-before.txt"))
    after = dropped(case / (f"{'remote' if direction == 'download' else 'local'}-qdisc-after.txt"))
    samples_file = case / ("remote-qdisc-samples.txt" if direction == "download" else "local-qdisc-samples.txt")
    max_backlog_bytes, max_backlog_packets = backlog(samples_file)
    delta = keyvals(case / "proxy-health-delta.txt")
    proxy_idle_rc = e.get("proxy_idle_rc", "")
    notes = []
    if note:
        notes.append(note)
    if rc not in ("0", ""):
        notes.append(f"iperf_rc_{rc}")
    if proxy_idle_rc not in ("", "0"):
        notes.append(f"proxy_idle_rc_{proxy_idle_rc}")
    try:
        if float(recv or 0) == 0:
            notes.append("zero_received_mbps")
    except ValueError:
        notes.append("invalid_received_mbps")
    for side in ("local", "remote"):
        if int(delta.get(f"{side}_rss_delta_kb", 0)) > 262144:
            notes.append(f"{side}_rss_growth_gt_256m")
        if int(delta.get(f"{side}_fd_delta", 0)) > 4:
            notes.append(f"{side}_fd_growth_gt_4")
        if int(delta.get(f"{side}_tcp_socket_delta", 0)) > 2:
            notes.append(f"{side}_tcp_socket_growth_gt_2")
    error_max = {
        "fatal_relay_resets": int(max(fatal_resets)) if fatal_resets else 0,
        "tcp_hard_errors": int(max(tcp_hard)) if tcp_hard else 0,
        "quic_send_failures": int(max(send_fail)) if send_fail else 0,
        "quic_send_api_failures": int(max(send_api_fail)) if send_api_fail else 0,
        "quic_send_fatal_errors": int(max(send_fatal)) if send_fatal else 0,
        "event_queue_full_errors": int(max(eq)) if eq else 0,
    }
    error_delta = {
        "fatal_relay_resets": num_delta(fatal_resets),
        "tcp_hard_errors": num_delta(tcp_hard),
        "quic_send_failures": num_delta(send_fail),
        "quic_send_api_failures": num_delta(send_api_fail),
        "quic_send_fatal_errors": num_delta(send_fatal),
        "event_queue_full_errors": num_delta(eq),
    }
    for name, value in error_delta.items():
        if value:
            notes.append(f"{name}_delta_{value}")
    anomaly = "yes" if notes else "no"
    rows.append({
        "order_index": e.get("order_index", ""),
        "seed": e.get("seed", ""),
        "direction": direction,
        "delay_ms": e.get("delay_ms", ""),
        "loss": e.get("loss_percent", ""),
        "netem_side": e.get("netem_side", ""),
        "iperf_rc": rc,
        "sent_mbps": sent,
        "received_mbps": recv,
        "retransmits": retrans,
        "jitter_ms": jitter,
        "netem_limit": e.get("netem_limit", ""),
        "qdisc_drop_delta": str(after - before),
        "qdisc_backlog_max_bytes": str(max_backlog_bytes),
        "qdisc_backlog_max_packets": str(max_backlog_packets),
        "srtt_avg_ms": f"{sum(srtt) / len(srtt):.3f}" if srtt else "",
        "srtt_max_ms": f"{max(srtt):.3f}" if srtt else "",
        "bbr_bw_avg_mbps": f"{sum(bbr) / len(bbr):.2f}" if bbr else "",
        "bytes_in_flight_max": str(int(max(bif))) if bif else "",
        "lost_retransmittable_bytes_max": str(int(max(lost))) if lost else "",
        "event_queue_full_errors_max": str(int(max(eq))) if eq else "",
        "fatal_relay_resets_max": str(error_max["fatal_relay_resets"]),
        "tcp_hard_errors_max": str(error_max["tcp_hard_errors"]),
        "quic_send_failures_max": str(error_max["quic_send_failures"]),
        "quic_send_api_failures_max": str(error_max["quic_send_api_failures"]),
        "quic_send_fatal_errors_max": str(error_max["quic_send_fatal_errors"]),
        "event_queue_full_errors_delta": str(error_delta["event_queue_full_errors"]),
        "fatal_relay_resets_delta": str(error_delta["fatal_relay_resets"]),
        "tcp_hard_errors_delta": str(error_delta["tcp_hard_errors"]),
        "quic_send_failures_delta": str(error_delta["quic_send_failures"]),
        "quic_send_api_failures_delta": str(error_delta["quic_send_api_failures"]),
        "quic_send_fatal_errors_delta": str(error_delta["quic_send_fatal_errors"]),
        "active_relays_max": str(int(max(active_relays))) if active_relays else "",
        "active_quic_send_relays_max": str(int(max(active_quic_send_relays))) if active_quic_send_relays else "",
        "proxy_idle_rc": proxy_idle_rc,
        "local_rss_delta_kb": str(delta.get("local_rss_delta_kb", "")),
        "remote_rss_delta_kb": str(delta.get("remote_rss_delta_kb", "")),
        "local_fd_delta": str(delta.get("local_fd_delta", "")),
        "remote_fd_delta": str(delta.get("remote_fd_delta", "")),
        "local_tcp_socket_delta": str(delta.get("local_tcp_socket_delta", "")),
        "remote_tcp_socket_delta": str(delta.get("remote_tcp_socket_delta", "")),
        "proxy_restarted": "no",
        "anomaly": anomaly,
        "notes": ";".join(notes),
    })

fields = [
    "order_index","seed","direction","delay_ms","loss","netem_side","iperf_rc",
    "sent_mbps","received_mbps","retransmits","jitter_ms","netem_limit",
    "qdisc_drop_delta","qdisc_backlog_max_bytes","qdisc_backlog_max_packets",
    "srtt_avg_ms","srtt_max_ms","bbr_bw_avg_mbps","bytes_in_flight_max",
    "lost_retransmittable_bytes_max","event_queue_full_errors_max",
    "fatal_relay_resets_max","tcp_hard_errors_max","quic_send_failures_max",
    "quic_send_api_failures_max","quic_send_fatal_errors_max",
    "event_queue_full_errors_delta","fatal_relay_resets_delta","tcp_hard_errors_delta",
    "quic_send_failures_delta","quic_send_api_failures_delta","quic_send_fatal_errors_delta",
    "active_relays_max","active_quic_send_relays_max","proxy_idle_rc",
    "local_rss_delta_kb","remote_rss_delta_kb","local_fd_delta","remote_fd_delta",
    "local_tcp_socket_delta","remote_tcp_socket_delta",
    "proxy_restarted","anomaly","notes",
]
with (root / "summary.csv").open("w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(rows)

with (root / "summary.md").open("w") as f:
    f.write("# DGX Persistent Proxy Netem Random Validation Summary\n\n")
    f.write(f"- Result root: `{root}`\n")
    f.write("- Proxy topology: local client, remote server\n")
    f.write("- SSH control plane: `jack@172.16.10.81`\n")
    f.write("- download netem side: remote egress\n")
    f.write("- upload netem side: local egress\n\n")
    f.write("| Order | Direction | Delay | Loss | Netem side | Received Mbps | Sent Mbps | rc | Drops | RSS delta local/remote KiB | FD delta local/remote | idle rc | srtt avg/max | Anomaly | Notes |\n")
    f.write("|---:|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|\n")
    for r in rows:
        f.write(
            f"| {r['order_index']} | {r['direction']} | {r['delay_ms']} | {r['loss']} | {r['netem_side']} | "
            f"{r['received_mbps']} | {r['sent_mbps']} | {r['iperf_rc']} | {r['qdisc_drop_delta']} | "
            f"{r['local_rss_delta_kb']}/{r['remote_rss_delta_kb']} | "
            f"{r['local_fd_delta']}/{r['remote_fd_delta']} | {r['proxy_idle_rc']} | "
            f"{r['srtt_avg_ms']}/{r['srtt_max_ms']} | {r['anomaly']} | {r['notes']} |\n"
        )
print(root)
PY
```

Expected:

```text
summary.csv 包含所有已执行 case；summary.md 按随机执行顺序列出结果和异常。
```

### Task 10: 完整性校验和结束清理

**Files:**
- Read: `$RESULT_ROOT/summary.csv`
- Read: `$RESULT_ROOT/summary.md`
- Append: `$RESULT_ROOT/orchestrator.log`

- [ ] **Step 1: 校验覆盖范围**

Run:

```bash
rtk python3 - "$RESULT_ROOT" "$DELAYS" "$LOSSES" <<'PY'
import csv
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
expected_delays = {int(x) for x in sys.argv[2].split()}
expected_losses = {x[:-1] if x.endswith("%") else x for x in sys.argv[3].split()}
rows = list(csv.DictReader((root / "summary.csv").open()))
seen = {(r["direction"], int(r["delay_ms"]), r["loss"]) for r in rows if r.get("delay_ms")}
missing = []
for direction in ("download", "upload"):
    for delay in expected_delays:
        for loss in expected_losses:
            if (direction, delay, loss) not in seen:
                missing.append((direction, delay, loss))
print("rows", len(rows))
print("missing", missing)
raise SystemExit(1 if missing else 0)
PY
```

Expected:

```text
没有异常中断时，missing 为空。若主矩阵因异常暂停，missing 用于说明未执行的剩余场景。
```

- [ ] **Step 2: 正常结束后清理**

Only run this step when matrix and anomaly collection are complete:

```bash
{
  echo "cleanup_at=$(date -Iseconds)"
  if [ -f "$RESULT_ROOT/proxy/pids.env" ]; then
    . "$RESULT_ROOT/proxy/pids.env"
  fi
  if [ -n "${REMOTE_LOG_TAIL_PID:-}" ]; then
    kill "$REMOTE_LOG_TAIL_PID" 2>/dev/null || true
  fi
  pkill -9 -x tcpquic-proxy 2>/dev/null || true
  pkill -9 -x iperf3 2>/dev/null || true
  sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
  tc -s qdisc show dev "$IFACE" || true
} >> "$RESULT_ROOT/orchestrator.log" 2>&1

rtk ssh -o BatchMode=yes "$REMOTE_SSH" "killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; tc -s qdisc show dev '$IFACE'; true" \
  >> "$RESULT_ROOT/orchestrator.log" 2>&1
```

Expected:

```text
测试完成后，本机和对端无残留 proxy、iperf3、netem。异常未完成分析前不要执行此清理步骤。
```

## 四、报告要求

最终报告必须包含：

1. 完整随机顺序和 seed。
2. 每个已执行场景的 receiver 侧吞吐。
3. download 使用对端 qdisc，upload 使用本机 qdisc。
4. proxy 是否全程未重启；若重启，列出原因和时间。
5. 异常场景的复现次数、复现条件和当前结论。
6. 未执行场景列表，如果主矩阵因异常暂停。
7. 每轮 client/server `VmRSS`、FD、TCP socket 的前后 delta，并说明是否存在持续增长趋势。
8. 每轮结束后的 proxy diag idle 检查结果，特别是 `active_relays`、`active_quic_send_relays`、`outstanding_quic_send_bytes`、`pending_tcp_write_bytes` 是否回到 0。
9. proxy diag 错误计数的本轮 delta，包括 `fatal_relay_resets`、`tcp_hard_errors`、`quic_send_failures`、`quic_send_api_failures`、`quic_send_fatal_errors`、`event_queue_full_errors`。

历史结果 `docs/dgx-iperf3-10loss-io-matrix-20260623-235738/summary.csv` 只作为异常判断参考，不作为本轮通过/失败标准。
