#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/jack/src/tcpquic-proxy}"
cd "$ROOT"

RESULT_ROOT="${RESULT_ROOT:-${ROOT}/docs/dgx-persistent-proxy-netem-random-$(date +%Y%m%d-%H%M%S)}"
CERT_DIR="${RESULT_ROOT}/certs"
BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
REMOTE_SSH="${REMOTE_SSH:-jack@172.16.10.81}"
REMOTE_DIR="${REMOTE_DIR:-/home/jack/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-${REMOTE_DIR}/tcpquic-proxy}"
LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
REMOTE_IP="${REMOTE_IP:-169.254.59.196}"
IFACE="${IFACE:-enp1s0f0np0}"
QUIC_PORT="${QUIC_PORT:-4433}"
PROXY_PORT="${PROXY_PORT:-18080}"
SOCKS_PORT="${SOCKS_PORT:-19080}"
IPERF_PORT="${IPERF_PORT:-16001}"
IPERF_DURATION_SEC="${IPERF_DURATION_SEC:-30}"
IPERF_TIMEOUT_SEC="${IPERF_TIMEOUT_SEC:-120}"
IPERF_TIMEOUT_IDLE_RETRIES="${IPERF_TIMEOUT_IDLE_RETRIES:-2}"
SSH_TIMEOUT_SEC="${SSH_TIMEOUT_SEC:-20}"
NETEM_LIMIT="${NETEM_LIMIT:-5000000}"
DELAYS="${DELAYS:-10 20 30 40 50 60 70 80 90 150 300 500}"
LOSSES="${LOSSES:-10}"
SEED="${SEED:-$(date +%s)}"
RESUME_EXISTING="${RESUME_EXISTING:-0}"

mkdir -p "$RESULT_ROOT" "$CERT_DIR" "$RESULT_ROOT/proxy" "$RESULT_ROOT/cases" "$RESULT_ROOT/anomalies"

log() {
  printf '[persistent-netem] %s\n' "$*" | tee -a "$RESULT_ROOT/orchestrator.log" >&2
}

save_vars() {
  {
    printf "ROOT=%q\n" "$ROOT"
    printf "RESULT_ROOT=%q\n" "$RESULT_ROOT"
    printf "CERT_DIR=%q\n" "$CERT_DIR"
    printf "BIN=%q\n" "$BIN"
    printf "REMOTE_SSH=%q\n" "$REMOTE_SSH"
    printf "REMOTE_DIR=%q\n" "$REMOTE_DIR"
    printf "REMOTE_BIN=%q\n" "$REMOTE_BIN"
    printf "LOCAL_IP=%q\n" "$LOCAL_IP"
    printf "REMOTE_IP=%q\n" "$REMOTE_IP"
    printf "IFACE=%q\n" "$IFACE"
    printf "QUIC_PORT=%q\n" "$QUIC_PORT"
    printf "PROXY_PORT=%q\n" "$PROXY_PORT"
    printf "SOCKS_PORT=%q\n" "$SOCKS_PORT"
    printf "IPERF_PORT=%q\n" "$IPERF_PORT"
    printf "IPERF_DURATION_SEC=%q\n" "$IPERF_DURATION_SEC"
    printf "IPERF_TIMEOUT_SEC=%q\n" "$IPERF_TIMEOUT_SEC"
    printf "IPERF_TIMEOUT_IDLE_RETRIES=%q\n" "$IPERF_TIMEOUT_IDLE_RETRIES"
    printf "SSH_TIMEOUT_SEC=%q\n" "$SSH_TIMEOUT_SEC"
    printf "NETEM_LIMIT=%q\n" "$NETEM_LIMIT"
    printf "DELAYS=%q\n" "$DELAYS"
    printf "LOSSES=%q\n" "$LOSSES"
    printf "SEED=%q\n" "$SEED"
    printf "RESUME_EXISTING=%q\n" "$RESUME_EXISTING"
  } > "$RESULT_ROOT/run.vars"
}

remote() {
  timeout "${SSH_TIMEOUT_SEC}s" ssh \
    -n \
    -o BatchMode=yes \
    -o ConnectTimeout=5 \
    -o ServerAliveInterval=5 \
    -o ServerAliveCountMax=1 \
    "$REMOTE_SSH" "$@"
}

preflight() {
  log "preflight result_root=$RESULT_ROOT"
  save_vars
  git rev-parse HEAD > "$RESULT_ROOT/git-version.txt"
  git submodule status third_party/msquic > "$RESULT_ROOT/msquic-version.txt"
  iperf3 --version > "$RESULT_ROOT/iperf3-version.txt" 2>&1
  iperf3 --help > "$RESULT_ROOT/iperf3-help.txt" 2>&1
  test -x "$BIN"
  remote "test -x '$REMOTE_BIN'"
  rg -- "--proxy|connect-timeout|socks" "$RESULT_ROOT/iperf3-help.txt" >/dev/null
  ip route get "$REMOTE_IP" > "$RESULT_ROOT/local-route.txt"
  remote "ip route get '$LOCAL_IP'" > "$RESULT_ROOT/remote-route.txt"
  remote "hostname; ip addr show '$IFACE'" > "$RESULT_ROOT/remote-iface.txt"
  {
    date -Iseconds
    pkill -9 -x tcpquic-proxy 2>/dev/null || true
    pkill -9 -x iperf3 2>/dev/null || true
    sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
    tc -s qdisc show dev "$IFACE" || true
  } > "$RESULT_ROOT/preflight-cleanup.log" 2>&1
  remote "killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; tc -s qdisc show dev '$IFACE'; true" >> "$RESULT_ROOT/preflight-cleanup.log" 2>&1
}

generate_certs() {
  log "generate certs"
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
    -subj "/CN=dgx-persistent-netem-ca" -days 1 -sha256 >/dev/null 2>&1
  tee "$CERT_DIR/server.ext" >/dev/null <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:${REMOTE_IP},IP:${LOCAL_IP}
EOF
  tee "$CERT_DIR/client.ext" >/dev/null <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:dgx-persistent-client
EOF
  for n in server client; do
    openssl req -newkey rsa:2048 -nodes \
      -keyout "$CERT_DIR/$n.key" -out "$CERT_DIR/$n.csr" \
      -subj "/CN=$n" >/dev/null 2>&1
    openssl x509 -req -in "$CERT_DIR/$n.csr" \
      -CA "$CERT_DIR/ca.crt" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
      -out "$CERT_DIR/$n.crt" -days 1 -sha256 \
      -extfile "$CERT_DIR/$n.ext" >/dev/null 2>&1
  done
  remote "mkdir -p ~/tcpquic-dgx-certs"
  rsync -az -e "ssh -o BatchMode=yes" "$CERT_DIR/" "$REMOTE_SSH:tcpquic-dgx-certs/"
}

start_services() {
  log "start persistent services"
  remote "pkill -9 -x iperf3 2>/dev/null; iperf3 -s -B '$REMOTE_IP' -p '$IPERF_PORT' -D; pgrep -a -x iperf3" > "$RESULT_ROOT/proxy/remote-iperf3-server.txt"
  rm -f "$(dirname "$BIN")/log/client.log" 2>/dev/null || true
  remote "rm -f '$REMOTE_DIR/log/server.log' ~/dgx-persistent-server.stderr.log 2>/dev/null || true"
  remote "sh -c 'LD_LIBRARY_PATH=$REMOTE_DIR TQ_TUNNEL_DEBUG=1 nohup $REMOTE_BIN server --listen $REMOTE_IP:$QUIC_PORT --allow-targets $REMOTE_IP/32,127.0.0.0/8 --cert ~/tcpquic-dgx-certs/server.crt --key ~/tcpquic-dgx-certs/server.key --ca ~/tcpquic-dgx-certs/ca.crt --compress off --tuning wan --connections 1 --diag-stats --diag-stats-interval 1 --trace --trace-interval 1 </dev/null >~/dgx-persistent-server.stderr.log 2>&1 & echo \$! > ~/dgx-persistent-server.pid'"
  : > "$RESULT_ROOT/proxy/remote-server.stderr.log"
  echo "REMOTE_LOG_TAIL_PID=" > "$RESULT_ROOT/proxy/pids.env"
  remote "cat ~/dgx-persistent-server.pid" | awk '{print "REMOTE_PROXY_PID=" $1}' >> "$RESULT_ROOT/proxy/pids.env"
  nohup env TQ_TUNNEL_DEBUG=1 TQ_QUIC_CLIENT_DEBUG=1 "$BIN" client \
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
    --trace \
    --trace-interval 1 \
    </dev/null \
    > "$RESULT_ROOT/proxy/local-client.stderr.log" 2>&1 &
  LOCAL_PROXY_PID=$!
  echo "LOCAL_PROXY_PID=$LOCAL_PROXY_PID" >> "$RESULT_ROOT/proxy/pids.env"
  for _ in $(seq 1 160); do
    if proxy_ready; then
      break
    fi
    sleep 0.5
  done
  proxy_ready
  capture_remote_proxy_log
  remote "pid=\$(cat ~/dgx-persistent-server.pid); echo pid=\$pid; ps -p \$pid -o pid,ppid,stat,etime,cmd; ss -uanp | grep -F '$QUIC_PORT' | grep -F \"pid=\$pid\" || true; grep -E 'QUIC server listening|server listening|diag_stats' ~/dgx-persistent-server.stderr.log | tail -20" > "$RESULT_ROOT/proxy/remote-server-ready.txt"
  remote "ps -p \$(cat ~/dgx-persistent-server.pid) >/dev/null"
}

capture_remote_proxy_log() {
  remote "cat ~/dgx-persistent-server.stderr.log" > "$RESULT_ROOT/proxy/remote-server.stderr.log" 2>/dev/null || true
}

capture_trace_logs() {
  cp "$(dirname "$BIN")/log/client.log" "$RESULT_ROOT/proxy/local-client.trace.log" 2>/dev/null || true
  remote "cat '$REMOTE_DIR/log/server.log'" > "$RESULT_ROOT/proxy/remote-server.trace.log" 2>/dev/null || true
}

proxy_ready() {
  local remote_pid
  remote_pid=$(remote "cat ~/dgx-persistent-server.pid" 2>/dev/null || true)
  [ -n "$remote_pid" ] || return 1
  ps -p "$LOCAL_PROXY_PID" >/dev/null || return 1
  remote "ps -p '$remote_pid' >/dev/null" || return 1
  rg -q "HTTP CONNECT listening" "$RESULT_ROOT/proxy/local-client.stderr.log" || return 1
  ss -ltnp 2>/dev/null | grep -F "127.0.0.1:$PROXY_PORT" | grep -F "pid=$LOCAL_PROXY_PID" >/dev/null || return 1
  if ss -uanp 2>/dev/null | grep -F "$REMOTE_IP:$QUIC_PORT" | grep -F "pid=$LOCAL_PROXY_PID" >/dev/null; then
    return 0
  fi
  rg -q "QUIC client connected|net_stats: bbr_bw" "$RESULT_ROOT/proxy/local-client.stderr.log"
}

load_existing_services() {
  log "resume existing persistent services"
  # shellcheck disable=SC1091
  . "$RESULT_ROOT/proxy/pids.env"
  LOCAL_PROXY_PID="${LOCAL_PROXY_PID:?missing LOCAL_PROXY_PID in pids.env}"
  REMOTE_PROXY_PID="${REMOTE_PROXY_PID:-$(remote "cat ~/dgx-persistent-server.pid" 2>/dev/null || true)}"
  {
    echo "REMOTE_LOG_TAIL_PID=${REMOTE_LOG_TAIL_PID:-}"
    echo "REMOTE_PROXY_PID=$REMOTE_PROXY_PID"
    echo "LOCAL_PROXY_PID=$LOCAL_PROXY_PID"
  } > "$RESULT_ROOT/proxy/pids.env"
  proxy_ready
  capture_remote_proxy_log
}

generate_order() {
  log "generate randomized scenario order seed=$SEED"
  python3 - "$RESULT_ROOT/scenario-order.csv" "$SEED" "$DELAYS" "$LOSSES" <<'PY'
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
    w.writer = csv.writer(f, lineterminator="\n")
    w.writeheader()
    for idx, item in enumerate(items, 1):
        item["order_index"] = idx
        item["seed"] = seed
        w.writerow(item)
print(out)
PY
}

clear_both_netem() {
  sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
  remote "sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; true"
}

remote_tc_show() {
  remote "tc -s qdisc show dev '$IFACE'"
}

apply_case_netem() {
  direction="$1"
  delay_ms="$2"
  loss_percent="$3"
  if [ "$direction" = download ]; then
    remote "sudo -n tc qdisc replace dev '$IFACE' root netem delay '${delay_ms}ms' loss '${loss_percent}%' limit '$NETEM_LIMIT'"
  else
    sudo -n tc qdisc replace dev "$IFACE" root netem delay "${delay_ms}ms" loss "${loss_percent}%" limit "$NETEM_LIMIT"
  fi
}

verify_case_netem() {
  direction="$1"
  delay_ms="$2"
  loss_percent="$3"
  if [ "$direction" = download ]; then
    qdisc=$(remote "tc qdisc show dev '$IFACE'")
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
      remote "tc -s qdisc show dev '$IFACE'" || true
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
  remote_pid=$(remote "cat ~/dgx-persistent-server.pid")
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
  remote "pid='$remote_pid'; {
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
  log_file="$1"
  rg "tcpquic-proxy diag_stats:" "$log_file" | tail -1 || true
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
    remote_line=$(remote "grep 'tcpquic-proxy diag_stats:' ~/dgx-persistent-server.stderr.log | tail -1" || true)
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

run_baseline() {
  log "run baseline"
  clear_both_netem
  local case_dir rc
  case_dir="$RESULT_ROOT/baseline/download"
  mkdir -p "$case_dir"
  set +e
  iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 --reverse --proxy "http://127.0.0.1:$PROXY_PORT" > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
  rc=$?
  set -e
  echo "$rc" > "$case_dir/iperf.rc"
  case_dir="$RESULT_ROOT/baseline/upload"
  mkdir -p "$case_dir"
  set +e
  iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 --proxy "http://127.0.0.1:$PROXY_PORT" > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
  rc=$?
  set -e
  echo "$rc" > "$case_dir/iperf.rc"
}

run_iperf_case() {
  direction="$1"
  case_dir="$2"
  set +e
  if [ "$direction" = download ]; then
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 --reverse --proxy "http://127.0.0.1:$PROXY_PORT" > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
  else
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 --proxy "http://127.0.0.1:$PROXY_PORT" > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
  fi
  rc=$?
  set -e
  echo "$rc" > "$case_dir/iperf.rc"
}

run_matrix() {
  log "run randomized matrix"
  rm -f "$RESULT_ROOT/anomaly-current-case.txt"
  while IFS=, read -r order_index seed direction delay_ms loss_percent loss_label; do
    [ "$order_index" = "order_index" ] && continue
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
    log "case order=$order_index direction=$direction delay=${delay_ms}ms loss=${loss_percent}%"
    capture_proxy_health before "$case_dir"
    stat -c '%s' "$RESULT_ROOT/proxy/local-client.stderr.log" > "$case_dir/local-proxy.offsets"
    remote "stat -c '%s' ~/dgx-persistent-server.stderr.log" > "$case_dir/remote-proxy.offsets"
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
      capture_proxy_health after "$case_dir"
      write_proxy_health_delta "$case_dir" > "$case_dir/proxy-health-delta.txt"
      echo "$case_dir" > "$RESULT_ROOT/anomaly-current-case.txt"
      return 10
    fi
    sampler_pids=$(sample_qdisc "$case_dir/local-qdisc-samples.txt" "$case_dir/remote-qdisc-samples.txt")
    run_iperf_case "$direction" "$case_dir"
    for sampler_pid in $sampler_pids; do kill "$sampler_pid" 2>/dev/null || true; done
    for sampler_pid in $sampler_pids; do wait "$sampler_pid" 2>/dev/null || true; done
    tc -s qdisc show dev "$IFACE" > "$case_dir/local-qdisc-after.txt" || true
    remote_tc_show > "$case_dir/remote-qdisc-after.txt" || true
    ss -tanp > "$case_dir/local-ss-after.txt" 2>&1 || true
    remote "ss -tanp; echo '--- udp ---'; ss -uanp" > "$case_dir/remote-ss-after.txt" 2>&1 || true
    local_start=$(sed -n '1p' "$case_dir/local-proxy.offsets")
    local_end=$(stat -c '%s' "$RESULT_ROOT/proxy/local-client.stderr.log")
    printf '%s\n%s\n' "$local_start" "$local_end" > "$case_dir/local-proxy.offsets"
    tail -c +"$((local_start + 1))" "$RESULT_ROOT/proxy/local-client.stderr.log" > "$case_dir/local-proxy.case.log"
    remote_start=$(sed -n '1p' "$case_dir/remote-proxy.offsets")
    remote_end=$(remote "stat -c '%s' ~/dgx-persistent-server.stderr.log")
    printf '%s\n%s\n' "$remote_start" "$remote_end" > "$case_dir/remote-proxy.offsets"
    remote "tail -c +$((remote_start + 1)) ~/dgx-persistent-server.stderr.log" > "$case_dir/remote-proxy.case.log"
    set +e
    wait_proxy_idle "$case_dir"
    idle_rc=$?
    set -e
    rc=$(cat "$case_dir/iperf.rc")
    if [ "$rc" -ne 0 ] && [ "$idle_rc" -eq 0 ] && [ "$IPERF_TIMEOUT_IDLE_RETRIES" -gt 0 ]; then
      cp "$case_dir/iperf.rc" "$case_dir/initial-nonzero-idle.iperf.rc" 2>/dev/null || true
      cp "$case_dir/iperf.stdout.json" "$case_dir/initial-nonzero-idle.iperf.stdout.json" 2>/dev/null || true
      cp "$case_dir/iperf.stderr.txt" "$case_dir/initial-nonzero-idle.iperf.stderr.txt" 2>/dev/null || true
      for retry in $(seq 1 "$IPERF_TIMEOUT_IDLE_RETRIES"); do
        retry_dir="$case_dir/retry-nonzero-idle-attempt-$retry"
        mkdir -p "$retry_dir"
        log "case order=$order_index retry_nonzero_idle rc=$rc attempt=$retry"
        run_iperf_case "$direction" "$retry_dir"
        set +e
        wait_proxy_idle "$retry_dir"
        retry_idle_rc=$?
        set -e
        echo "proxy_idle_rc=$retry_idle_rc" >> "$retry_dir/run.env"
        retry_rc=$(cat "$retry_dir/iperf.rc")
        if [ "$retry_rc" -eq 0 ] && [ "$retry_idle_rc" -eq 0 ]; then
          cp "$retry_dir/iperf.rc" "$case_dir/iperf.rc"
          cp "$retry_dir/iperf.stdout.json" "$case_dir/iperf.stdout.json"
          cp "$retry_dir/iperf.stderr.txt" "$case_dir/iperf.stderr.txt"
          echo "nonzero_idle_retry_resolved=$retry" >> "$case_dir/run.env"
          idle_rc=0
          break
        fi
      done
      local_end=$(stat -c '%s' "$RESULT_ROOT/proxy/local-client.stderr.log")
      printf '%s\n%s\n' "$local_start" "$local_end" > "$case_dir/local-proxy.offsets"
      tail -c +"$((local_start + 1))" "$RESULT_ROOT/proxy/local-client.stderr.log" > "$case_dir/local-proxy.case.log"
      remote_end=$(remote "stat -c '%s' ~/dgx-persistent-server.stderr.log")
      printf '%s\n%s\n' "$remote_start" "$remote_end" > "$case_dir/remote-proxy.offsets"
      remote "tail -c +$((remote_start + 1)) ~/dgx-persistent-server.stderr.log" > "$case_dir/remote-proxy.case.log"
      rc=$(cat "$case_dir/iperf.rc")
    fi
    echo "proxy_idle_rc=$idle_rc" >> "$case_dir/run.env"
    capture_proxy_health after "$case_dir"
    write_proxy_health_delta "$case_dir" > "$case_dir/proxy-health-delta.txt"
    rc=$(cat "$case_dir/iperf.rc")
    log "case order=$order_index rc=$rc idle_rc=$idle_rc"
    if [ "$rc" -eq 0 ] && [ "$idle_rc" -eq 0 ]; then
      clear_both_netem
    else
      echo "$case_dir" > "$RESULT_ROOT/anomaly-current-case.txt"
      return 10
    fi
  done < "$RESULT_ROOT/scenario-order.csv"
}

freeze_anomaly() {
  [ -f "$RESULT_ROOT/anomaly-current-case.txt" ] || return 0
  bad_case=$(cat "$RESULT_ROOT/anomaly-current-case.txt")
  anomaly_dir="$RESULT_ROOT/anomalies/$(basename "$bad_case")"
  mkdir -p "$anomaly_dir"
  capture_remote_proxy_log
  capture_trace_logs
  # shellcheck disable=SC1091
  . "$RESULT_ROOT/proxy/pids.env"
  cp "$bad_case/run.env" "$anomaly_dir/anomaly.env" 2>/dev/null || true
  cp "$RESULT_ROOT/proxy/pids.env" "$anomaly_dir/proxy-pids.txt" 2>/dev/null || true
  cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$anomaly_dir/local-proxy.full-at-freeze.log" 2>/dev/null || true
  remote "cat ~/dgx-persistent-server.stderr.log" > "$anomaly_dir/remote-proxy.full-at-freeze.log" 2>/dev/null || true
  cp "$RESULT_ROOT/proxy/local-client.trace.log" "$anomaly_dir/local-client.trace-at-freeze.log" 2>/dev/null || true
  cp "$RESULT_ROOT/proxy/remote-server.trace.log" "$anomaly_dir/remote-server.trace-at-freeze.log" 2>/dev/null || true
  ps -fp "${LOCAL_PROXY_PID:-0}" > "$anomaly_dir/local-proxy-ps.txt" 2>&1 || true
  remote "cat ~/dgx-persistent-server.pid; ps -fp \$(cat ~/dgx-persistent-server.pid)" > "$anomaly_dir/remote-proxy-ps.txt" 2>&1 || true
  cat "/proc/${LOCAL_PROXY_PID:-0}/status" > "$anomaly_dir/local-proc-status.txt" 2>&1 || true
  remote "cat /proc/\$(cat ~/dgx-persistent-server.pid)/status" > "$anomaly_dir/remote-proc-status.txt" 2>&1 || true
  {
    echo "pid=${LOCAL_PROXY_PID:-0}"
    ls -l "/proc/${LOCAL_PROXY_PID:-0}/fd" 2>&1 || true
    echo "--- readlink ---"
    for fd in "/proc/${LOCAL_PROXY_PID:-0}/fd/"*; do
      [ -e "$fd" ] || continue
      printf '%s -> ' "$fd"
      readlink "$fd" 2>&1 || true
    done
    echo "--- fdinfo ---"
    for fd in "/proc/${LOCAL_PROXY_PID:-0}/fdinfo/"*; do
      [ -e "$fd" ] || continue
      echo "### $fd"
      cat "$fd" 2>&1 || true
    done
    echo "--- proc net tcp ---"
    cat /proc/net/tcp /proc/net/tcp6 2>&1 || true
    echo "--- proc net udp ---"
    cat /proc/net/udp /proc/net/udp6 2>&1 || true
  } > "$anomaly_dir/local-proxy-fd-snapshot.txt"
  remote "pid=\$(cat ~/dgx-persistent-server.pid); {
    echo pid=\$pid;
    ls -l \"/proc/\$pid/fd\" 2>&1 || true;
    echo '--- readlink ---';
    for fd in /proc/\$pid/fd/*; do
      [ -e \"\$fd\" ] || continue;
      printf '%s -> ' \"\$fd\";
      readlink \"\$fd\" 2>&1 || true;
    done;
    echo '--- fdinfo ---';
    for fd in /proc/\$pid/fdinfo/*; do
      [ -e \"\$fd\" ] || continue;
      echo \"### \$fd\";
      cat \"\$fd\" 2>&1 || true;
    done;
    echo '--- proc net tcp ---';
    cat /proc/net/tcp /proc/net/tcp6 2>&1 || true;
    echo '--- proc net udp ---';
    cat /proc/net/udp /proc/net/udp6 2>&1 || true;
  }" > "$anomaly_dir/remote-proxy-fd-snapshot.txt" 2>&1 || true
  ss -tanp > "$anomaly_dir/local-ss-tcp.txt" 2>&1 || true
  ss -uanp > "$anomaly_dir/local-ss-udp.txt" 2>&1 || true
  remote "ss -tanp; echo '--- udp ---'; ss -uanp" > "$anomaly_dir/remote-ss.txt" 2>&1 || true
  ip -s link show "$IFACE" > "$anomaly_dir/local-ip-link.txt" 2>&1 || true
  remote "ip -s link show '$IFACE'" > "$anomaly_dir/remote-ip-link.txt" 2>&1 || true
  tc -s qdisc show dev "$IFACE" > "$anomaly_dir/local-qdisc-freeze.txt" 2>&1 || true
  remote "tc -s qdisc show dev '$IFACE'" > "$anomaly_dir/remote-qdisc-freeze.txt" 2>&1 || true
  # shellcheck disable=SC1090
  . "$bad_case/run.env"
  for attempt in 1 2 3; do
    repro_dir="$anomaly_dir/repro-same-netem-attempt-$attempt"
    mkdir -p "$repro_dir"
    run_iperf_case "$direction" "$repro_dir"
  done
  clear_both_netem
  apply_case_netem "$direction" "$delay_ms" "$loss_percent"
  for attempt in 1 2 3; do
    repro_dir="$anomaly_dir/repro-reapply-netem-attempt-$attempt"
    mkdir -p "$repro_dir"
    run_iperf_case "$direction" "$repro_dir"
  done
  cat > "$anomaly_dir/anomaly-notes.md" <<EOF
# Anomaly Notes

- Case: $(basename "$bad_case")
- First failure: see \`$bad_case\`
- Proxy restarted before freeze: no
- Netem side: $(grep '^netem_side=' "$bad_case/run.env" | cut -d= -f2-)
- Same-netem reproduction: see \`repro-same-netem-attempt-*\`
- Reapply-netem reproduction: see \`repro-reapply-netem-attempt-*\`
- Neighbor/direction checks: not run automatically
- Current rule: delay=${delay_ms}ms loss=${loss_percent}% limit=${NETEM_LIMIT}
- Restart comparison: not run automatically
- Conclusion: pending manual analysis
EOF
}

parse_summary() {
  capture_remote_proxy_log
  capture_trace_logs
  python3 - "$RESULT_ROOT" <<'PY'
import csv, json, pathlib, re, sys
root = pathlib.Path(sys.argv[1])
def text(path):
    try: return path.read_text(errors="ignore")
    except FileNotFoundError: return ""
def parse_iperf(case):
    rc = text(case / "iperf.rc").strip() or "missing"; sent = recv = retrans = jitter = ""; note = ""
    try:
        data = json.loads(text(case / "iperf.stdout.json")); end = data.get("end", {})
        ss = end.get("sum_sent", {}) or {}; rr = end.get("sum_received", {}) or {}
        sent = f"{float(ss.get('bits_per_second', 0)) / 1_000_000:.2f}"
        recv = f"{float(rr.get('bits_per_second', 0)) / 1_000_000:.2f}"
        retrans = ss.get("retransmits", ""); jitter = rr.get("jitter_ms", "") or ss.get("jitter_ms", "") or ""
    except Exception as exc: note = f"json:{exc}"
    return rc, sent, recv, retrans, jitter, note
def env(case):
    out = {}
    for line in text(case / "run.env").splitlines():
        if "=" in line:
            k, v = line.split("=", 1); out[k] = v
    return out
def nums(log, name):
    vals = []
    for m in re.finditer(rf"{re.escape(name)}=([0-9.]+)", log):
        try: vals.append(float(m.group(1)))
        except ValueError: pass
    return vals
def num_delta(values):
    if not values: return 0
    if len(values) == 1: return int(values[0])
    return int(values[-1] - values[0])
def keyvals(path):
    out = {}
    for line in text(path).splitlines():
        if "=" not in line: continue
        k, v = line.split("=", 1)
        try: out[k] = int(v)
        except ValueError: out[k] = v
    return out
def dropped(path):
    m = re.search(r"dropped\s+(\d+)", text(path)); return int(m.group(1)) if m else 0
def backlog(path):
    max_bytes = max_packets = 0
    for m in re.finditer(r"backlog\s+([0-9.]+)([KMG]?b)\s+(\d+)p", text(path), re.I):
        mult = {"b":1,"kb":1000,"mb":1000000,"gb":1000000000}.get(m.group(2).lower(), 1)
        max_bytes = max(max_bytes, int(float(m.group(1)) * mult)); max_packets = max(max_packets, int(m.group(3)))
    return max_bytes, max_packets
rows = []
for case in sorted((root / "cases").glob("order-*")):
    e = env(case); rc, sent, recv, retrans, jitter, note = parse_iperf(case); direction = e.get("direction", "")
    log = text(case / ("remote-proxy.case.log" if direction == "download" else "local-proxy.case.log"))
    srtt = nums(log, "srtt"); bbr = nums(log, "bbr_bw_mbps"); bif = nums(log, "bytes_in_flight_max"); lost = nums(log, "lost_retransmittable_bytes")
    eq = nums(log, "event_queue_full_errors"); fatal_resets = nums(log, "fatal_relay_resets"); tcp_hard = nums(log, "tcp_hard_errors")
    send_fail = nums(log, "quic_send_failures"); send_api_fail = nums(log, "quic_send_api_failures"); send_fatal = nums(log, "quic_send_fatal_errors")
    active_relays = nums(log, "active_relays"); active_quic_send_relays = nums(log, "active_quic_send_relays")
    before = dropped(case / (f"{'remote' if direction == 'download' else 'local'}-qdisc-before.txt")); after = dropped(case / (f"{'remote' if direction == 'download' else 'local'}-qdisc-after.txt"))
    samples_file = case / ("remote-qdisc-samples.txt" if direction == "download" else "local-qdisc-samples.txt")
    max_backlog_bytes, max_backlog_packets = backlog(samples_file); delta = keyvals(case / "proxy-health-delta.txt"); proxy_idle_rc = e.get("proxy_idle_rc", "")
    notes = []
    if note: notes.append(note)
    if rc not in ("0", ""): notes.append(f"iperf_rc_{rc}")
    if proxy_idle_rc not in ("", "0"): notes.append(f"proxy_idle_rc_{proxy_idle_rc}")
    try:
        if float(recv or 0) == 0: notes.append("zero_received_mbps")
    except ValueError: notes.append("invalid_received_mbps")
    for side in ("local", "remote"):
        if int(delta.get(f"{side}_rss_delta_kb", 0)) > 262144: notes.append(f"{side}_rss_growth_gt_256m")
        if int(delta.get(f"{side}_fd_delta", 0)) > 4: notes.append(f"{side}_fd_growth_gt_4")
        if int(delta.get(f"{side}_tcp_socket_delta", 0)) > 2: notes.append(f"{side}_tcp_socket_growth_gt_2")
    error_delta = {
        "fatal_relay_resets": num_delta(fatal_resets), "tcp_hard_errors": num_delta(tcp_hard), "quic_send_failures": num_delta(send_fail),
        "quic_send_api_failures": num_delta(send_api_fail), "quic_send_fatal_errors": num_delta(send_fatal), "event_queue_full_errors": num_delta(eq),
    }
    for name, value in error_delta.items():
        if value: notes.append(f"{name}_delta_{value}")
    rows.append({
        "order_index": e.get("order_index", ""), "seed": e.get("seed", ""), "direction": direction, "delay_ms": e.get("delay_ms", ""), "loss": e.get("loss_percent", ""),
        "netem_side": e.get("netem_side", ""), "iperf_rc": rc, "sent_mbps": sent, "received_mbps": recv, "retransmits": retrans, "jitter_ms": jitter,
        "netem_limit": e.get("netem_limit", ""), "qdisc_drop_delta": str(after - before), "qdisc_backlog_max_bytes": str(max_backlog_bytes), "qdisc_backlog_max_packets": str(max_backlog_packets),
        "srtt_avg_ms": f"{sum(srtt)/len(srtt):.3f}" if srtt else "", "srtt_max_ms": f"{max(srtt):.3f}" if srtt else "", "bbr_bw_avg_mbps": f"{sum(bbr)/len(bbr):.2f}" if bbr else "",
        "bytes_in_flight_max": str(int(max(bif))) if bif else "", "lost_retransmittable_bytes_max": str(int(max(lost))) if lost else "", "event_queue_full_errors_delta": str(error_delta["event_queue_full_errors"]),
        "fatal_relay_resets_delta": str(error_delta["fatal_relay_resets"]), "tcp_hard_errors_delta": str(error_delta["tcp_hard_errors"]), "quic_send_failures_delta": str(error_delta["quic_send_failures"]),
        "quic_send_api_failures_delta": str(error_delta["quic_send_api_failures"]), "quic_send_fatal_errors_delta": str(error_delta["quic_send_fatal_errors"]),
        "active_relays_max": str(int(max(active_relays))) if active_relays else "", "active_quic_send_relays_max": str(int(max(active_quic_send_relays))) if active_quic_send_relays else "",
        "proxy_idle_rc": proxy_idle_rc, "local_rss_delta_kb": str(delta.get("local_rss_delta_kb", "")), "remote_rss_delta_kb": str(delta.get("remote_rss_delta_kb", "")),
        "local_fd_delta": str(delta.get("local_fd_delta", "")), "remote_fd_delta": str(delta.get("remote_fd_delta", "")), "local_tcp_socket_delta": str(delta.get("local_tcp_socket_delta", "")),
        "remote_tcp_socket_delta": str(delta.get("remote_tcp_socket_delta", "")), "proxy_restarted": "no", "anomaly": "yes" if notes else "no", "notes": ";".join(notes),
    })
fields = list(rows[0].keys()) if rows else ["order_index","seed","direction","delay_ms","loss","netem_side","iperf_rc","sent_mbps","received_mbps","notes"]
with (root / "summary.csv").open("w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(rows)
with (root / "summary.md").open("w") as f:
    f.write("# DGX Persistent Proxy Netem Random Validation Summary\n\n")
    f.write(f"- Result root: `{root}`\n- Proxy topology: local client, remote server\n- SSH control plane: `jack@172.16.10.81`\n- download netem side: remote egress\n- upload netem side: local egress\n\n")
    f.write("| Order | Direction | Delay | Loss | Netem side | Received Mbps | Sent Mbps | rc | Drops | RSS delta local/remote KiB | FD delta local/remote | idle rc | srtt avg/max | Anomaly | Notes |\n")
    f.write("|---:|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|\n")
    for r in rows:
        f.write(f"| {r['order_index']} | {r['direction']} | {r['delay_ms']} | {r['loss']} | {r['netem_side']} | {r['received_mbps']} | {r['sent_mbps']} | {r['iperf_rc']} | {r['qdisc_drop_delta']} | {r['local_rss_delta_kb']}/{r['remote_rss_delta_kb']} | {r['local_fd_delta']}/{r['remote_fd_delta']} | {r['proxy_idle_rc']} | {r['srtt_avg_ms']}/{r['srtt_max_ms']} | {r['anomaly']} | {r['notes']} |\n")
print(root)
PY
}

cleanup_all() {
  log "cleanup all"
  if [ -f "$RESULT_ROOT/proxy/pids.env" ]; then
    # shellcheck disable=SC1091
    . "$RESULT_ROOT/proxy/pids.env"
  fi
  if [ -n "${REMOTE_LOG_TAIL_PID:-}" ]; then
    kill "$REMOTE_LOG_TAIL_PID" 2>/dev/null || true
  fi
  pkill -9 -x tcpquic-proxy 2>/dev/null || true
  pkill -9 -x iperf3 2>/dev/null || true
  sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
  remote "killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev '$IFACE' root 2>/dev/null; true" || true
}

main() {
  if [ "$RESUME_EXISTING" = "1" ]; then
    save_vars
    load_existing_services
  else
    preflight
    generate_certs
    start_services
  fi
  generate_order
  run_baseline
  matrix_rc=0
  run_matrix || matrix_rc=$?
  parse_summary
  if [ "$matrix_rc" -eq 0 ]; then
    cleanup_all
  else
    log "anomaly encountered matrix_rc=$matrix_rc; freezing and reproducing without proxy restart"
    freeze_anomaly
    parse_summary
    log "leaving proxy/netem state as-is for inspection"
  fi
  log "result_root=$RESULT_ROOT"
}

main "$@"
