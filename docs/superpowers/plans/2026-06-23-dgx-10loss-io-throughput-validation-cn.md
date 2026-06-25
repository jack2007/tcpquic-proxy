# DGX 10% 丢包 IO 吞吐验证实施计划

> **给 agentic workers:** REQUIRED SUB-SKILL: 使用 `superpowers:executing-plans` 按任务逐项执行本计划。步骤使用 checkbox (`- [ ]`) 语法跟踪。

**目标:** 在 2 台 DGX 200Gbps 直连链路上，验证 `10/20/30/40/50/60/70/80/90/150/300/500ms` 网络延迟叠加 `10%` 丢包时，tcpquic-proxy 单连接、单 stream 的 download 和 upload IO 吞吐。

**架构:** 测试工具统一使用系统已安装的 `iperf3`，并使用该版本支持的 HTTP CONNECT 代理能力。download 使用 `iperf3 --reverse --proxy http://127.0.0.1:<proxy_port>`，数据方向为远端 iperf3 server 到本机 iperf3 client；upload 使用远端 iperf3 client 经远端 tcpquic-proxy client 发送到本机 iperf3 server。两种方向都保持远端 DGX 为 QUIC 发送端，因此 netem 只加在远端 egress 网卡，符合 `docs/io-throughput-diag_cn.md` 的发送端 egress netem 方法。

**技术栈:** Bash、系统 `iperf3`、`tc netem`、SSH、tcpquic-proxy、2xDGX 200Gbps 直连链路。

---

## 一、测试设计

### 1. 固定硬件和网络假设

默认沿用当前 DGX 直连环境：

```text
本机 DGX: 169.254.250.230
远端 DGX: 169.254.59.196
远端 SSH: jack@169.254.59.196
200G 网卡: enp1s0f0np0
QUIC UDP 端口: 4433
HTTP CONNECT 代理端口: 18080
SOCKS5 代理端口: 19080
iperf3 端口: 16001
```

netem 只作用在远端发送方向：

```bash
sudo -n tc qdisc replace dev enp1s0f0np0 root netem delay <delay>ms loss 10% limit 5000000
```

禁止在本机 200G 网卡上保留 netem。

### 2. 测试矩阵

每个组合都需要跑 download 和 upload：

```text
10ms  + 10% loss
20ms  + 10% loss
30ms  + 10% loss
40ms  + 10% loss
50ms  + 10% loss
60ms  + 10% loss
70ms  + 10% loss
80ms  + 10% loss
90ms  + 10% loss
150ms + 10% loss
300ms + 10% loss
500ms + 10% loss
```

共 `12 * 2 = 24` 组方向性测试。

### 3. 固定代理参数

所有 tcpquic-proxy 进程都使用：

```bash
--tuning wan
--connections 1
--compress off
--diag-stats
--diag-stats-interval 1
```

说明：

- `--connections 1` 固定 peer 间单 QUIC connection。
- 每次只启动一个 `iperf3 -c`，且不使用 `-P` 并发，因此应用层只有一个 TCP 连接，对应一个 HTTP CONNECT stream。
- `--compress off` 避免压缩影响 IO 吞吐。
- 不启用 trace 文件日志，避免 `docs/io-throughput-diag_cn.md` 中已确认的 trace 性能扰动。

### 4. download 拓扑

download 使用 iperf3 reverse 模式，让 QUIC 数据方向为远端发送、本机接收：

```text
远端 iperf3 -s
  -> 远端 tcpquic-proxy server
  -> QUIC download
  -> 本机 tcpquic-proxy client
  -> 本机 iperf3 -c --reverse --proxy http://127.0.0.1:18080
```

本机命令口径：

```bash
iperf3 -c 169.254.59.196 \
  -B 169.254.250.230 \
  -p 16001 \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --reverse \
  --proxy http://127.0.0.1:18080
```

download 的吞吐采用 iperf3 JSON 中 `end.sum_received.bits_per_second` 和 `end.sum_sent.bits_per_second`，最终报告同时保留 sent/received，主表优先使用 receiver 侧吞吐。

### 5. upload 拓扑

upload 使用反向部署，让远端 tcpquic-proxy client 作为 QUIC 发送端：

```text
远端 iperf3 -c --proxy http://127.0.0.1:18080
  -> 远端 tcpquic-proxy client
  -> QUIC upload
  -> 本机 tcpquic-proxy server
  -> 本机 iperf3 -s
```

远端命令口径：

```bash
iperf3 -c 169.254.250.230 \
  -B 169.254.59.196 \
  -p 16001 \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --proxy http://127.0.0.1:18080
```

upload 的吞吐采用 iperf3 JSON 中 `end.sum_received.bits_per_second` 和 `end.sum_sent.bits_per_second`，最终报告同时保留 sent/received，主表优先使用 receiver 侧吞吐。

## 二、结果目录

创建一个带时间戳的结果根目录：

```text
docs/dgx-iperf3-10loss-io-matrix-YYYYMMDD-HHMMSS/
```

每个 delay 下分 download/upload：

```text
netem-010ms-10loss/download/
netem-010ms-10loss/upload/
netem-020ms-10loss/download/
netem-020ms-10loss/upload/
...
netem-500ms-10loss/download/
netem-500ms-10loss/upload/
```

每个方向目录必须包含：

```text
iperf.stdout.json
iperf.stderr.txt
local-proxy.log
remote-proxy.log
remote-qdisc-before.txt
remote-qdisc-after.txt
remote-qdisc-samples.txt
run.env
run.log
```

最终汇总文件：

```text
summary.csv
summary.md
```

`summary.csv` 字段：

```text
delay_ms,direction,loss,iperf_rc,sent_mbps,received_mbps,retransmits,jitter_ms,netem_limit,qdisc_drop_delta,qdisc_backlog_max_bytes,qdisc_backlog_max_packets,srtt_avg_ms,srtt_max_ms,bbr_bw_avg_mbps,bytes_in_flight_max,lost_retransmittable_bytes_max,event_queue_full_errors_max,notes
```

## 三、执行任务

### Task 1: 前置检查

**文件:**
- 读取: `docs/io-throughput-diag_cn.md`
- 读取: `scripts/bench-tcpquic-proxy-dgx.sh`
- 读取: `scripts/dgx-interconnect-netem-common.sh`
- 创建: `docs/dgx-iperf3-10loss-io-matrix-YYYYMMDD-HHMMSS/`

- [ ] **Step 1: 确认 iperf3 支持代理参数**

运行：

```bash
rtk which iperf3
rtk iperf3 --help | rtk rg -- '--proxy|socks|connect-timeout'
```

期望：

```text
iperf3 帮助中能看到 --proxy 或 socks/http connect 相关参数
```

- [ ] **Step 2: 确认 tcpquic-proxy 二进制和源码版本**

运行：

```bash
rtk git rev-parse HEAD
rtk git submodule status third_party/msquic
rtk test -x build/bin/Release/tcpquic-proxy
```

期望：

```text
输出主仓 commit、MsQuic submodule commit，tcpquic-proxy 二进制存在且可执行
```

- [ ] **Step 3: 清理残留进程和 netem**

运行：

```bash
rtk pkill -9 -f 'tcpquic-proxy client' || true
rtk pkill -9 -x iperf3 || true
rtk sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null || true
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true'
```

期望：

```text
本机和远端没有残留 tcpquic-proxy、iperf3、netem
```

- [ ] **Step 4: 确认直连路由**

运行：

```bash
rtk ip route get 169.254.59.196
rtk ssh -o BatchMode=yes jack@169.254.59.196 'ip route get 169.254.250.230'
```

期望：

```text
本机到远端、远端到本机都走 200G 直连网卡 enp1s0f0np0
```

### Task 2: 证书准备

**文件:**
- 创建: `<结果根目录>/certs/ca.crt`
- 创建: `<结果根目录>/certs/server.crt`
- 创建: `<结果根目录>/certs/client.crt`

- [ ] **Step 1: 生成同时覆盖两个方向的测试证书**

运行：

```bash
RESULT_ROOT="docs/dgx-iperf3-10loss-io-matrix-$(date +%Y%m%d-%H%M%S)"
CERT_DIR="${RESULT_ROOT}/certs"
rtk mkdir -p "$CERT_DIR"

rtk openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
  -subj "/CN=dgx-iperf3-10loss-ca" -days 1 -sha256

cat > "$CERT_DIR/server.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:169.254.59.196,IP:169.254.250.230
EOF

cat > "$CERT_DIR/client.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:dgx-iperf3-client
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

期望：

```text
server 证书 SAN 同时包含 169.254.59.196 和 169.254.250.230，支持 download 和 upload 两种 server 部署方向
```

- [ ] **Step 2: 同步证书到远端**

运行：

```bash
rtk ssh -o BatchMode=yes jack@169.254.59.196 'mkdir -p ~/tcpquic-dgx-certs'
rtk rsync -az "$CERT_DIR/" jack@169.254.59.196:~/tcpquic-dgx-certs/
```

期望：

```text
远端 ~/tcpquic-dgx-certs/ 下有 ca/server/client 证书和 key
```

### Task 3: download 基线验证

**文件:**
- 创建: `<结果根目录>/baseline/download/iperf.stdout.json`
- 创建: `<结果根目录>/baseline/download/local-proxy.log`
- 创建: `<结果根目录>/baseline/download/remote-proxy.log`

- [ ] **Step 1: 启动远端 iperf3 server**

运行：

```bash
rtk ssh -o BatchMode=yes jack@169.254.59.196 'pkill -9 -x iperf3 2>/dev/null; iperf3 -s -B 169.254.59.196 -p 16001 -D'
```

期望：

```text
远端 iperf3 server 监听 169.254.59.196:16001
```

- [ ] **Step 2: 启动 download 方向 proxy**

运行：

```bash
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; LD_LIBRARY_PATH=~/tcpquic-dgx-bin nohup ~/tcpquic-dgx-bin/tcpquic-proxy server \
  --listen 169.254.59.196:4433 \
  --allow-targets 169.254.59.196/32,127.0.0.0/8 \
  --cert ~/tcpquic-dgx-certs/server.crt \
  --key ~/tcpquic-dgx-certs/server.key \
  --ca ~/tcpquic-dgx-certs/ca.crt \
  --compress off \
  --tuning wan \
  --connections 1 \
  --diag-stats \
  --diag-stats-interval 1 \
  </dev/null >/tmp/dgx-download-server.log 2>&1 &'

rtk build/bin/Release/tcpquic-proxy client \
  --peer 169.254.59.196:4433 \
  --http-listen 127.0.0.1:18080 \
  --socks-listen 127.0.0.1:19080 \
  --cert "$CERT_DIR/client.crt" \
  --key "$CERT_DIR/client.key" \
  --ca "$CERT_DIR/ca.crt" \
  --connections 1 \
  --compress off \
  --tuning wan \
  --diag-stats \
  --diag-stats-interval 1 \
  >/tmp/dgx-download-client.log 2>&1 &
```

期望：

```text
远端日志包含 QUIC server listening，本机日志包含 HTTP CONNECT listening 和 QUIC client connected
```

- [ ] **Step 3: 跑无 netem download iperf3 基线**

运行：

```bash
rtk mkdir -p "$RESULT_ROOT/baseline/download"
rtk iperf3 -c 169.254.59.196 \
  -B 169.254.250.230 \
  -p 16001 \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --reverse \
  --proxy http://127.0.0.1:18080 \
  > "$RESULT_ROOT/baseline/download/iperf.stdout.json" \
  2> "$RESULT_ROOT/baseline/download/iperf.stderr.txt"
echo $? > "$RESULT_ROOT/baseline/download/iperf.rc"
```

期望：

```text
iperf.rc 为 0，JSON 中 end.sum_received.bits_per_second 有有效值
```

### Task 4: upload 基线验证

**文件:**
- 创建: `<结果根目录>/baseline/upload/iperf.stdout.json`
- 创建: `<结果根目录>/baseline/upload/local-proxy.log`
- 创建: `<结果根目录>/baseline/upload/remote-proxy.log`

- [ ] **Step 1: 启动本机 iperf3 server**

运行：

```bash
rtk pkill -9 -x iperf3 || true
rtk iperf3 -s -B 169.254.250.230 -p 16001 -D
```

期望：

```text
本机 iperf3 server 监听 169.254.250.230:16001
```

- [ ] **Step 2: 启动 upload 方向 proxy**

运行：

```bash
rtk pkill -9 -f 'tcpquic-proxy server' || true
rtk build/bin/Release/tcpquic-proxy server \
  --listen 169.254.250.230:4433 \
  --allow-targets 169.254.250.230/32,127.0.0.0/8 \
  --cert "$CERT_DIR/server.crt" \
  --key "$CERT_DIR/server.key" \
  --ca "$CERT_DIR/ca.crt" \
  --compress off \
  --tuning wan \
  --connections 1 \
  --diag-stats \
  --diag-stats-interval 1 \
  >/tmp/dgx-upload-server.log 2>&1 &

rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; LD_LIBRARY_PATH=~/tcpquic-dgx-bin nohup ~/tcpquic-dgx-bin/tcpquic-proxy client \
  --peer 169.254.250.230:4433 \
  --http-listen 127.0.0.1:18080 \
  --socks-listen 127.0.0.1:19080 \
  --cert ~/tcpquic-dgx-certs/client.crt \
  --key ~/tcpquic-dgx-certs/client.key \
  --ca ~/tcpquic-dgx-certs/ca.crt \
  --connections 1 \
  --compress off \
  --tuning wan \
  --diag-stats \
  --diag-stats-interval 1 \
  </dev/null >/tmp/dgx-upload-client.log 2>&1 &'
```

期望：

```text
本机日志包含 QUIC server listening，远端日志包含 HTTP CONNECT listening 和 QUIC client connected
```

- [ ] **Step 3: 跑无 netem upload iperf3 基线**

运行：

```bash
rtk mkdir -p "$RESULT_ROOT/baseline/upload"
rtk ssh -o BatchMode=yes jack@169.254.59.196 "iperf3 -c 169.254.250.230 \
  -B 169.254.59.196 \
  -p 16001 \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --proxy http://127.0.0.1:18080" \
  > "$RESULT_ROOT/baseline/upload/iperf.stdout.json" \
  2> "$RESULT_ROOT/baseline/upload/iperf.stderr.txt"
echo $? > "$RESULT_ROOT/baseline/upload/iperf.rc"
```

期望：

```text
iperf.rc 为 0，JSON 中 end.sum_received.bits_per_second 有有效值
```

### Task 5: 执行 10% 丢包 download 矩阵

**文件:**
- 创建: `<结果根目录>/netem-<delay>ms-10loss/download/*`

- [ ] **Step 1: 重启 download 方向 proxy**

运行：

```bash
rtk pkill -9 -f 'tcpquic-proxy client' || true
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; true'

rtk ssh -o BatchMode=yes jack@169.254.59.196 'LD_LIBRARY_PATH=~/tcpquic-dgx-bin nohup ~/tcpquic-dgx-bin/tcpquic-proxy server \
  --listen 169.254.59.196:4433 \
  --allow-targets 169.254.59.196/32,127.0.0.0/8 \
  --cert ~/tcpquic-dgx-certs/server.crt \
  --key ~/tcpquic-dgx-certs/server.key \
  --ca ~/tcpquic-dgx-certs/ca.crt \
  --compress off \
  --tuning wan \
  --connections 1 \
  --diag-stats \
  --diag-stats-interval 1 \
  </dev/null >/tmp/dgx-download-server.log 2>&1 &'

rtk build/bin/Release/tcpquic-proxy client \
  --peer 169.254.59.196:4433 \
  --http-listen 127.0.0.1:18080 \
  --socks-listen 127.0.0.1:19080 \
  --cert "$CERT_DIR/client.crt" \
  --key "$CERT_DIR/client.key" \
  --ca "$CERT_DIR/ca.crt" \
  --connections 1 \
  --compress off \
  --tuning wan \
  --diag-stats \
  --diag-stats-interval 1 \
  >/tmp/dgx-download-client.log 2>&1 &
```

期望：

```text
远端 tcpquic-proxy server 监听 169.254.59.196:4433，本机 HTTP CONNECT 监听 127.0.0.1:18080
```

- [ ] **Step 2: 按 delay 顺序逐组运行 download**

使用固定顺序：

```bash
DELAYS=(10 20 30 40 50 60 70 80 90 150 300 500)
```

每个 delay 运行。下面示例使用 `d=10`，实际执行时替换为 `DELAYS` 中的每个值：

```bash
d=10
case_dir="$RESULT_ROOT/netem-$(printf '%03d' "$d")ms-10loss/download"
rtk mkdir -p "$case_dir"

rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true"
rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" > "$case_dir/remote-qdisc-before.txt"
rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc replace dev enp1s0f0np0 root netem delay ${d}ms loss 10% limit 5000000"

(
  while true; do
    date -Iseconds
    rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" || true
    sleep 5
  done
) > "$case_dir/remote-qdisc-samples.txt" 2>&1 &
sampler_pid=$!

rtk ssh -o BatchMode=yes jack@169.254.59.196 'pkill -9 -x iperf3 2>/dev/null; iperf3 -s -B 169.254.59.196 -p 16001 -D'

rtk iperf3 -c 169.254.59.196 \
  -B 169.254.250.230 \
  -p 16001 \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --reverse \
  --proxy http://127.0.0.1:18080 \
  > "$case_dir/iperf.stdout.json" \
  2> "$case_dir/iperf.stderr.txt"
echo $? > "$case_dir/iperf.rc"

kill "$sampler_pid" 2>/dev/null || true
wait "$sampler_pid" 2>/dev/null || true

rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" > "$case_dir/remote-qdisc-after.txt"
rtk cp /tmp/dgx-download-client.log "$case_dir/local-proxy.log" 2>/dev/null || true
rtk scp -q jack@169.254.59.196:/tmp/dgx-download-server.log "$case_dir/remote-proxy.log" 2>/dev/null || true

rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true"
```

期望：

```text
每个 delay 都生成 iperf JSON、qdisc before/after/samples、local/remote proxy diag 日志
```

### Task 6: 执行 10% 丢包 upload 矩阵

**文件:**
- 创建: `<结果根目录>/netem-<delay>ms-10loss/upload/*`

- [ ] **Step 1: 重启 upload 方向 proxy**

运行：

```bash
rtk pkill -9 -f 'tcpquic-proxy server' || true
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; true'

rtk build/bin/Release/tcpquic-proxy server \
  --listen 169.254.250.230:4433 \
  --allow-targets 169.254.250.230/32,127.0.0.0/8 \
  --cert "$CERT_DIR/server.crt" \
  --key "$CERT_DIR/server.key" \
  --ca "$CERT_DIR/ca.crt" \
  --compress off \
  --tuning wan \
  --connections 1 \
  --diag-stats \
  --diag-stats-interval 1 \
  >/tmp/dgx-upload-server.log 2>&1 &

rtk ssh -o BatchMode=yes jack@169.254.59.196 'LD_LIBRARY_PATH=~/tcpquic-dgx-bin nohup ~/tcpquic-dgx-bin/tcpquic-proxy client \
  --peer 169.254.250.230:4433 \
  --http-listen 127.0.0.1:18080 \
  --socks-listen 127.0.0.1:19080 \
  --cert ~/tcpquic-dgx-certs/client.crt \
  --key ~/tcpquic-dgx-certs/client.key \
  --ca ~/tcpquic-dgx-certs/ca.crt \
  --connections 1 \
  --compress off \
  --tuning wan \
  --diag-stats \
  --diag-stats-interval 1 \
  </dev/null >/tmp/dgx-upload-client.log 2>&1 &'
```

期望：

```text
本机 tcpquic-proxy server 监听 169.254.250.230:4433，远端 HTTP CONNECT 监听 127.0.0.1:18080
```

- [ ] **Step 2: 按 delay 顺序逐组运行 upload**

每个 delay 运行。下面示例使用 `d=10`，实际执行时替换为 `DELAYS` 中的每个值：

```bash
d=10
case_dir="$RESULT_ROOT/netem-$(printf '%03d' "$d")ms-10loss/upload"
rtk mkdir -p "$case_dir"

rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true"
rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" > "$case_dir/remote-qdisc-before.txt"
rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc replace dev enp1s0f0np0 root netem delay ${d}ms loss 10% limit 5000000"

(
  while true; do
    date -Iseconds
    rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" || true
    sleep 5
  done
) > "$case_dir/remote-qdisc-samples.txt" 2>&1 &
sampler_pid=$!

rtk pkill -9 -x iperf3 || true
rtk iperf3 -s -B 169.254.250.230 -p 16001 -D

rtk ssh -o BatchMode=yes jack@169.254.59.196 "iperf3 -c 169.254.250.230 \
  -B 169.254.59.196 \
  -p 16001 \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --proxy http://127.0.0.1:18080" \
  > "$case_dir/iperf.stdout.json" \
  2> "$case_dir/iperf.stderr.txt"
echo $? > "$case_dir/iperf.rc"

kill "$sampler_pid" 2>/dev/null || true
wait "$sampler_pid" 2>/dev/null || true

rtk ssh -o BatchMode=yes jack@169.254.59.196 "tc -s qdisc show dev enp1s0f0np0" > "$case_dir/remote-qdisc-after.txt"
rtk cp /tmp/dgx-upload-server.log "$case_dir/local-proxy.log" 2>/dev/null || true
rtk scp -q jack@169.254.59.196:/tmp/dgx-upload-client.log "$case_dir/remote-proxy.log" 2>/dev/null || true

rtk ssh -o BatchMode=yes jack@169.254.59.196 "sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true"
```

期望：

```text
每个 delay 都生成 upload 方向 iperf JSON、qdisc 和 proxy diag 日志
```

### Task 7: 汇总结果

**文件:**
- 创建: `<结果根目录>/summary.csv`
- 创建: `<结果根目录>/summary.md`

- [ ] **Step 1: 解析 iperf3 JSON**

对每个方向目录解析：

```text
iperf.rc
end.sum_sent.bits_per_second
end.sum_received.bits_per_second
end.sum_sent.retransmits
```

推荐命令：

```bash
rtk python3 - "$RESULT_ROOT" <<'PY'
import csv, json, pathlib, re, sys

root = pathlib.Path(sys.argv[1])
rows = []
for case in sorted(root.glob("netem-*ms-10loss/*")):
    if case.name not in {"download", "upload"}:
        continue
    m = re.search(r"netem-(\d+)ms-10loss", str(case))
    delay = int(m.group(1)) if m else -1
    direction = case.name
    rc_path = case / "iperf.rc"
    iperf_rc = rc_path.read_text().strip() if rc_path.exists() else "missing"
    sent_mbps = received_mbps = retransmits = ""
    data_path = case / "iperf.stdout.json"
    if data_path.exists() and data_path.stat().st_size:
        data = json.loads(data_path.read_text())
        end = data.get("end", {})
        sent = end.get("sum_sent", {})
        recv = end.get("sum_received", {})
        sent_mbps = f"{float(sent.get('bits_per_second', 0)) / 1_000_000:.2f}"
        received_mbps = f"{float(recv.get('bits_per_second', 0)) / 1_000_000:.2f}"
        retransmits = sent.get("retransmits", "")
    rows.append({
        "delay_ms": delay,
        "direction": direction,
        "loss": "10%",
        "iperf_rc": iperf_rc,
        "sent_mbps": sent_mbps,
        "received_mbps": received_mbps,
        "retransmits": retransmits,
        "notes": "",
    })

fields = ["delay_ms","direction","loss","iperf_rc","sent_mbps","received_mbps","retransmits","notes"]
with (root / "summary.csv").open("w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    w.writerows(rows)
PY
```

期望：

```text
summary.csv 至少包含 24 行：12 个 delay * download/upload
```

- [ ] **Step 2: 解析 proxy diag**

从每个方向的发送端 proxy 日志解析：

```text
srtt
bbr_bw_mbps
bytes_in_flight_max
lost_retransmittable_bytes
event_queue_full_errors
flush_scheduling
flush_pacing_delayed
flush_cc_blocked
```

发送端日志规则：

```text
download: remote-proxy.log
upload: remote-proxy.log
```

原因：本计划让两个方向都由远端 DGX 发送 QUIC 数据。

- [ ] **Step 3: 解析 qdisc**

从 `remote-qdisc-before.txt` 和 `remote-qdisc-after.txt` 计算：

```text
qdisc_drop_delta = dropped_after - dropped_before
```

从 `remote-qdisc-samples.txt` 计算：

```text
qdisc_backlog_max_bytes
qdisc_backlog_max_packets
```

注意：netem backlog 包含模拟 delay 持有的报文，不等价于物理网卡拥塞。

- [ ] **Step 4: 生成中文 summary.md**

`summary.md` 必须包含：

```markdown
| Delay | Direction | Loss | Received Mbps | Sent Mbps | iperf rc | Retransmits | qdisc drops | qdisc backlog max | srtt avg/max | bytes_in_flight_max | lost bytes max | Notes |
```

并按 delay 排序，每个 delay 下先 download 后 upload。

### Task 8: 完整性校验

**文件:**
- 读取: `<结果根目录>/summary.csv`
- 读取: `<结果根目录>/summary.md`
- 读取: 每个 `run.log` / `iperf.stderr.txt`

- [ ] **Step 1: 校验矩阵覆盖**

期望：

```text
delay_ms = 10,20,30,40,50,60,70,80,90,150,300,500
direction = download,upload
每个 delay 都有两个方向
```

- [ ] **Step 2: 校验 netem 参数**

每个方向目录都应满足：

```text
delay <delay>ms loss 10% limit 5000000
```

不允许出现：

```text
rate 1gbit
```

- [ ] **Step 3: 校验单连接单 stream**

检查 proxy 和 iperf 参数：

```text
tcpquic-proxy: --connections 1
iperf3: 没有 -P 参数
每个方向只有一个 iperf3 client
```

- [ ] **Step 4: 清理环境**

运行：

```bash
rtk pkill -9 -f 'tcpquic-proxy' || true
rtk pkill -9 -x iperf3 || true
rtk sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null || true
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev enp1s0f0np0 root 2>/dev/null; true'
```

期望：

```text
本机和远端都无残留测试进程，远端 netem 已清除
```

## 四、报告规则

最终报告必须先给出完整矩阵，再给诊断解读：

1. 每个 delay 必须同时展示 download 和 upload。
2. 主要吞吐口径使用 `received_mbps`，同时保留 `sent_mbps`。
3. `iperf_rc != 0` 的场景不能丢弃，必须在 Notes 中标记。
4. 500ms + 10% loss 可能出现握手慢、代理连接超时或吞吐极低；保留原始 stderr 和 proxy diag 后再判断。
5. qdisc backlog 只能作为 netem 延迟队列/突发强度观察，不能直接写成物理网卡拥塞。
6. 如需和 `docs/io-throughput-diag_cn.md` 中 5% loss 历史结果对比，只作为背景；本次交付物是 10% loss 下 download/upload 完整矩阵。

## 五、2026-06-24 执行记录

### 1. 构建与执行信息

本轮已基于当前 `master` 主线重新构建 `build` 目录中的 `tcpquic-proxy`，并使用新二进制执行完整 iperf3 download/upload 矩阵。

| 项目 | 值 |
|---|---|
| 主仓 commit | `48245c0058823606ec556622e5c8542f9b02f2d2` |
| MsQuic submodule | `f42ae7b741a24ae3b29c2eec356b76a18e2fac66 third_party/msquic (v0.9-draft-29-3422-gf42ae7b74)` |
| 构建命令 | `cmake --build build --target tcpquic-proxy -j$(nproc)` |
| 本机二进制 | `build/bin/Release/tcpquic-proxy` |
| 远端二进制 | `/home/jack/tcpquic-dgx-bin/tcpquic-proxy` |
| 测试工具 | 系统 `iperf3 3.21+`，HTTP CONNECT proxy |
| 结果目录 | `docs/dgx-iperf3-10loss-io-matrix-20260623-235738` |
| 汇总文件 | `docs/dgx-iperf3-10loss-io-matrix-20260623-235738/summary.md` |
| CSV | `docs/dgx-iperf3-10loss-io-matrix-20260623-235738/summary.csv` |

### 2. 吞吐结果

单位为 Mbps，取 iperf3 receiver 侧吞吐。所有测试均为 `loss 10%`、远端 egress netem、`limit 5000000`、tcpquic-proxy `--connections 1`、iperf3 单 client 且不使用 `-P`。

| Delay | Download | Download rc | Upload | Upload rc |
|---:|---:|---:|---:|---:|
| 10ms | 11490.35 | 0 | 8763.89 | 0 |
| 20ms | 9850.53 | 0 | 9502.17 | 0 |
| 30ms | 9488.76 | 0 | 8998.60 | 0 |
| 40ms | 0.00 | 1 | 9244.85 | 0 |
| 50ms | 8618.65 | 0 | 8754.67 | 0 |
| 60ms | 8571.35 | 0 | 5819.79 | 0 |
| 70ms | 7928.94 | 0 | 8056.53 | 0 |
| 80ms | 7765.24 | 0 | 7558.92 | 0 |
| 90ms | 7464.76 | 0 | 7700.24 | 0 |
| 150ms | 5099.34 | 0 | 6469.34 | 0 |
| 300ms | 71.47 | 124 | 3434.15 | 0 |
| 500ms | 0.00 | 1 | 2123.51 | 0 |

### 3. 异常项

| 场景 | 结果 | 原因 |
|---|---:|---|
| `40ms + 10% loss` download | `rc=1` | iperf3 JSON 报 `proxy negotiation failed: Protocol error` |
| `300ms + 10% loss` download | `rc=124` | 外层 `90s` timeout |
| `500ms + 10% loss` download | `rc=1` | iperf3 JSON 报 `proxy negotiation failed` |

upload 方向 12 组均为 `rc=0`。

### 4. 完整性与清理验证

执行后已验证：

```text
summary.csv 行数: 24
delay 覆盖: 10,20,30,40,50,60,70,80,90,150,300,500
direction 覆盖: download,upload
缺失组合: 无
```

环境清理状态：

```text
本机 enp1s0f0np0: 无 netem
远端 enp1s0f0np0: 无 netem
本机残留 tcpquic-proxy/iperf3 进程: 无
远端残留 tcpquic-proxy/iperf3 进程: 无
```

### 5. 执行脚本

为避免长 heredoc 命令被截断，并保证矩阵可恢复、可审计，本轮新增了可复用执行脚本：

```text
scripts/run-dgx-iperf3-10loss-matrix.sh
```

脚本默认对单个 iperf3 用例设置 `IPERF_TIMEOUT_SEC=90` 硬超时。超时用例会记录非 0 rc 并继续后续矩阵。

## 六、2026-06-24 异常复现与根因分析记录

### 1. 调试目标

本轮不是重新获取吞吐矩阵，而是针对上一轮记录中的异常项做现场复现和根因分析：

| 异常项 | 上一轮现象 |
|---|---|
| `40ms + 10% loss` download | `iperf3 rc=1`，`proxy negotiation failed: Protocol error` |
| `300ms + 10% loss` download | `iperf3 rc=124`，外层 `90s` timeout，receiver 吞吐只有 `71.47 Mbps` |
| `500ms + 10% loss` download | `iperf3 rc=1`，`proxy negotiation failed` |

调试要求是先打开 `iperf3 --verbose` 和 `tcpquic-proxy` 的详细日志，重新运行并在问题出现时保留现场，不停止问题现场的 `tcpquic-proxy`。

### 2. 新增诊断脚本

新增脚本：

```text
scripts/debug-dgx-iperf3-download-issue.sh
```

该脚本用于 download 方向复现，关键设置如下：

```text
iperf3: -V -d --timestamps
tcpquic-proxy: --trace --trace-interval 1 --diag-stats --diag-stats-interval 1
netem: delay <N>ms loss 10% limit 5000000
IPERF_TIMEOUT_SEC=240
LOW_THROUGHPUT_MBPS=500
```

脚本会记录：

```text
iperf verbose stdout/stderr
本机和远端 tcpquic-proxy stderr
本机和远端 trace log
本机和远端 ss 快照
本机和远端 qdisc 快照
进程、fd、/proc stack 快照
```

触发非 0 rc 或低吞吐后，脚本会保留现场，不清理 `tcpquic-proxy`、`iperf3`、`netem`。

### 3. 单独 fresh proxy 复测结果

先对异常 delay 使用每轮重新启动 proxy 的方式复测：

| Delay | 复测次数 | 结果 |
|---:|---:|---|
| 40ms | 6 | 6/6 成功，receiver 约 `9.05-9.50 Gbps` |
| 500ms | 3 | 3/3 成功，receiver 约 `1.19-1.24 Gbps` |
| 300ms | 3 | 3/3 成功，receiver 约 `1.61-3.88 Gbps` |

结论：`40ms` 和 `500ms` 的 `proxy negotiation failed` 在 fresh proxy 条件下没有稳定复现，不是单个 delay 本身必然触发的 CONNECT 协商失败。该现象更像是长矩阵连续运行时，前一个 download 用例未完全 drain/close 后留下的状态影响后续用例。

### 4. 长生命周期 proxy 顺序复现

随后使用 `SEQUENTIAL=1`，复用同一个 download proxy，按上一轮矩阵顺序运行：

```bash
rtk env SEQUENTIAL=1 DELAYS='10 20 30 40 50 60 70 80 90 150 300 500' ATTEMPTS=1 scripts/debug-dgx-iperf3-download-issue.sh
```

本轮在 `90ms + 10% loss` 处复现了与上一轮 `300ms` 低吞吐相同类型的问题：

| 项目 | 值 |
|---|---|
| 结果目录 | `docs/dgx-iperf3-10loss-debug-20260624-081022` |
| 问题用例目录 | `docs/dgx-iperf3-10loss-debug-20260624-081022/sequential-netem-090ms-10loss-attempt-9` |
| 触发原因 | `trigger=iperf_rc_124` |
| iperf rc | `124` |
| receiver 吞吐 | `788.00 Mbps` |
| netem | `delay 90ms loss 10% limit 5000000` |

`iperf3 --verbose` 的关键时间线：

```text
08:16:17 进入 TEST_RUNNING
08:16:47 State set to 4-TEST_END
08:20:16 State set to 14-DISPLAY_RESULTS
08:20:16 0.00-239.51 sec 22.0 GBytes 788 Mbits/sec receiver
08:20:16 iperf3: interrupt - the client has terminated by signal Terminated(15)
```

含义：30 秒数据阶段已经结束，但从 `TEST_END` 到 `DISPLAY_RESULTS` 又等待了约 209 秒。最终 receiver 吞吐使用 `239.51 sec` 做分母，因此显示为低吞吐。这与上一轮 `300ms` download 的低吞吐/timeout 属于同一类问题：不是 30 秒数据窗口内吞吐一直极低，而是测试结束后的关闭/排空阶段卡住，被外层 timeout 打断。

### 5. tcpquic-proxy 与 socket 证据

远端发送侧 `tcpquic-proxy` 在问题现场没有崩溃，也没有 QUIC send API 失败：

```text
pending_bytes 约 444-448 MB
relay_buffer_bytes 约 444-448 MB
outstanding_quic_sends=598
outstanding_quic_send_bytes=436837103
quic_send_failures=0
quic_send_api_failures=0
last_quic_send_status=0
bytes_in_flight=0
bbr_bw_mbps=0.00
out_flow_blocked=0x10
```

这说明远端 proxy 仍有大量 relay/QUIC 尾部数据排队，但 QUIC 侧已经没有 bytes in flight，且处于 flow blocked 状态；问题不表现为 send 调用失败，也不表现为当时链路上仍有大量 netem backlog。

远端 `ss` 快照显示 iperf server 与远端 proxy 的本机 TCP 连接处于关闭半途中：

```text
iperf server 侧: CLOSING, Send-Q=68370961, notsent=68370961, rwnd_limited=237599ms(91.2%)
tcpquic-proxy 侧: FIN-WAIT-2, Recv-Q=21995580
```

这说明 reverse download 的发送端本机 TCP 已经进入关闭流程，但仍有约 68 MB 未发送数据和约 22 MB proxy 未读数据。

本机接收侧同时存在前序 CONNECT 残留 socket：

```text
FIN-WAIT-2 169.254.250.230:<ephemeral> -> 127.0.0.1:18080
CLOSE-WAIT 127.0.0.1:18080 -> 169.254.250.230:<ephemeral> users:(("tcpquic-proxy",...))
```

这支持 fresh proxy 不复现、长生命周期 proxy 顺序运行复现的判断：前序用例的 HTTP CONNECT/TCP/stream close 未完全收敛，会污染后续矩阵用例。

### 6. 当前根因判断

当前证据指向的问题不是 200Gbps 直连链路带宽不足，也不是 `iperf3` 30 秒数据阶段真实吞吐降到几十 Mbps，而是 high RTT/high loss download 场景下的关闭/排空路径问题：

1. iperf3 reverse download 在数据阶段结束后进入 `TEST_END`。
2. 接收端/客户端开始结束测试，但 tcpquic-proxy 两侧仍存在大量未完成的 relay/QUIC/TCP 尾部数据。
3. 远端 proxy 发送侧保留约 444-448 MB pending buffer，QUIC send 没有失败，但连接处于 `out_flow_blocked=0x10` 且 `bytes_in_flight=0`，无法继续向前推进。
4. 远端 iperf server 到 proxy 的本机 TCP 处于 `CLOSING/FIN-WAIT-2`，还有未发送和未读数据。
5. iperf3 等待结果/关闭阶段长时间不能完成，最终被外层 timeout 杀掉；iperf3 summary 用完整等待时间做分母，导致记录为极低 receiver 吞吐。

上一轮 `40ms`、`500ms` 的 `proxy negotiation failed` 在 fresh proxy 下未复现，结合顺序复现中的 `FIN-WAIT-2/CLOSE-WAIT` 残留，当前判断为同一类 close/drain 不完整导致的次生现象：长生命周期 proxy 连续跑矩阵时，前序 download 用例没有完全释放连接/stream 状态，后续 CONNECT 协商可能撞到未收敛状态并失败。

### 7. 现场保留状态

按调试要求，本次触发问题后未清理远端现场。确认时远端仍保留：

```text
tcpquic-proxy server pid=1738474
iperf3 server pid=1784823
netem qdisc: delay 90ms loss 10% limit 5000000
```

本机问题进程已退出，但关键远端发送侧现场、日志和 socket/qdisc 快照均已保留在：

```text
docs/dgx-iperf3-10loss-debug-20260624-081022/sequential-netem-090ms-10loss-attempt-9
```

后续若需要清理现场，再显式执行清理命令；本轮按要求不自动清理。
