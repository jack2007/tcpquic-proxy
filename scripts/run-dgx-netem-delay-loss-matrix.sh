#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REMOTE_SSH="${REMOTE_SSH:-jack@172.16.10.81}"
LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
REMOTE_IP="${REMOTE_IP:-169.254.59.196}"
DATA_IFACE="${DATA_IFACE:-enp1s0f0np0}"
REMOTE_DIR="${REMOTE_DIR:-/home/jack/tcpquic-dgx-bin}"
BIN="${BIN:-$ROOT/build/bin/Release/raypx2}"
REMOTE_BIN="${REMOTE_BIN:-$REMOTE_DIR/raypx2}"
CERT_DIR="${CERT_DIR:-$ROOT/cert}"
QUIC_PORT="${QUIC_PORT:-4433}"
PROXY_PORT="${PROXY_PORT:-18080}"
SOCKS_PORT="${SOCKS_PORT:-19080}"
IPERF_PORT="${IPERF_PORT:-16001}"
CLIENT_ADMIN_LISTEN="${CLIENT_ADMIN_LISTEN:-127.0.0.1:18081}"
SERVER_ADMIN_LISTEN="${SERVER_ADMIN_LISTEN:-127.0.0.1:18081}"
IPERF_DURATION_SEC="${IPERF_DURATION_SEC:-30}"
IPERF_TIMEOUT_SEC="${IPERF_TIMEOUT_SEC:-120}"
NETEM_LIMIT="${NETEM_LIMIT:-5000000}"
DELAYS=(${DELAYS:-10 20 50 80 100 150 200})
LOSSES=(${LOSSES:-5 10})
RUNS="${RUNS:-3}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/docs/dgx-netem-delay-loss-matrix-$(date +%Y%m%d-%H%M%S)}"
RESUME_AFTER_ORDER="${RESUME_AFTER_ORDER:-0}"
SKIP_PREFLIGHT="${SKIP_PREFLIGHT:-0}"
SKIP_START_SERVICES="${SKIP_START_SERVICES:-0}"
APPEND_SUMMARY="${APPEND_SUMMARY:-0}"
CONTINUE_ON_VARIANCE="${CONTINUE_ON_VARIANCE:-0}"

mkdir -p "$RESULT_ROOT"/{proxy,cases,summary,anomalies}
SUMMARY_CSV="$RESULT_ROOT/summary.csv"
SCENARIO_CSV="$RESULT_ROOT/scenario-order.csv"
STOP_FILE="$RESULT_ROOT/STOPPED"

log() {
  printf '[%s] %s\n' "$(date -Iseconds)" "$*" | tee -a "$RESULT_ROOT/orchestrator.log" >&2
}

remote() {
  ssh -o BatchMode=yes "$REMOTE_SSH" "$@"
}

parse_iperf() {
  python3 - "$1" <<'PY'
import json, sys
try:
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    end = data.get("end", {})
    sent = end.get("sum_sent") or {}
    recv = end.get("sum_received") or {}
    test = (data.get("start") or {}).get("test_start") or {}
    sent_mbps = float(sent.get("bits_per_second") or 0.0) / 1000000
    recv_mbps = float(recv.get("bits_per_second") or 0.0) / 1000000
    retrans = sent.get("retransmits", "")
    duration = test.get("duration") or sent.get("seconds") or recv.get("seconds") or ""
    print(f"{sent_mbps:.2f},{recv_mbps:.2f},{retrans},{duration}")
except Exception as exc:
    print(f"PARSE_ERROR,{exc}")
    sys.exit(2)
PY
}

check_raypx2_alive() {
  local local_pid
  local_pid="$(cat "$RESULT_ROOT/proxy/local-client.pid" 2>/dev/null || true)"
  [[ -n "$local_pid" ]] && ps -p "$local_pid" >/dev/null || return 1
  remote 'test -s /home/jack/dgx-netem-server.pid && ps -p $(cat /home/jack/dgx-netem-server.pid) >/dev/null'
}

collect_freeze() {
  local case_dir="$1"
  local reason="$2"
  local anomaly_dir="$RESULT_ROOT/anomalies/$(basename "$case_dir")"
  mkdir -p "$case_dir/freeze" "$anomaly_dir"
  echo "$reason" > "$case_dir/freeze/reason.txt"
  date -Iseconds > "$case_dir/freeze/time.txt"
  ss -tanp > "$case_dir/freeze/local-ss-tcp.txt" 2>&1 || true
  ss -uanp > "$case_dir/freeze/local-ss-udp.txt" 2>&1 || true
  ps -ef > "$case_dir/freeze/local-ps.txt" 2>&1 || true
  tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/freeze/local-qdisc.txt" 2>&1 || true
  cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$case_dir/freeze/local-proxy.full.log" 2>/dev/null || true
  remote 'ss -tanp; echo ===UDP===; ss -uanp; echo ===PS===; ps -ef; echo ===QDISC===; tc -s qdisc show dev enp1s0f0np0; echo ===LOG===; cat /home/jack/dgx-netem-server.stderr.log' \
    > "$case_dir/freeze/remote-freeze.txt" 2>&1 || true
  cp -a "$case_dir/freeze" "$anomaly_dir/freeze" 2>/dev/null || true
  {
    echo "time=$(date -Iseconds)"
    echo "case_dir=$case_dir"
    echo "reason=$reason"
  } > "$STOP_FILE"
}

sample_qdisc() {
  local side="$1"
  local out="$2"
  if [[ "$side" == local ]]; then
    while true; do
      date -Iseconds
      tc -s qdisc show dev "$DATA_IFACE" || true
      sleep 5
    done > "$out" 2>&1
  else
    while true; do
      date -Iseconds
      remote 'tc -s qdisc show dev enp1s0f0np0' || true
      sleep 5
    done > "$out" 2>&1
  fi
}

write_summary_md() {
  python3 - "$SUMMARY_CSV" "$RESULT_ROOT/summary.md" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="")))
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write("# DGX netem delay/loss matrix summary\n\n")
    f.write(f"- Cases: {len(rows)}\n")
    f.write("\n| direction | delay_ms | loss_pct | runs | avg_received_mbps | min | max | failures |\n")
    f.write("|---|---:|---:|---:|---:|---:|---:|---:|\n")
    keys = sorted(set((r["direction"], int(r["delay_ms"]), int(r["loss_pct"])) for r in rows),
                  key=lambda x: (x[2], x[1], x[0]))
    for direction, delay, loss in keys:
        vals, failures = [], 0
        for r in rows:
            if r["direction"] == direction and int(r["delay_ms"]) == delay and int(r["loss_pct"]) == loss:
                try:
                    vals.append(float(r["received_mbps"]))
                except Exception:
                    failures += 1
                if r["iperf_rc"] != "0" or r["notes"] != "ok":
                    failures += 1
        if vals:
            f.write(f"| {direction} | {delay} | {loss} | {len(vals)} | "
                    f"{sum(vals)/len(vals):.2f} | {min(vals):.2f} | {max(vals):.2f} | {failures} |\n")
PY
}

preflight() {
  echo "RESULT_ROOT=$RESULT_ROOT" > /tmp/dgx-netem-delay-loss-current.env
  local sha
  sha="$(sha256sum "$BIN" | cut -d' ' -f1)"
  {
    echo "RESULT_ROOT=$RESULT_ROOT"
    echo "started_at=$(date -Iseconds)"
    echo "raypx2_sha256=$sha"
  } | tee "$RESULT_ROOT/orchestrator.log" >&2

  git rev-parse HEAD > "$RESULT_ROOT/git-version.txt"
  git submodule status third_party/msquic > "$RESULT_ROOT/msquic-version.txt" 2>&1 || true
  iperf3 --version > "$RESULT_ROOT/iperf3-version.txt" 2>&1
  iperf3 --help > "$RESULT_ROOT/iperf3-help.txt" 2>&1
  ls -l "$BIN" > "$RESULT_ROOT/local-raypx2.txt"

  if ! remote "test -x '$REMOTE_BIN'"; then
    log "remote raypx2 missing; syncing before matrix"
    rsync -az -e "ssh -o BatchMode=yes" "$BIN" "$REMOTE_SSH:$REMOTE_BIN"
    remote "chmod +x '$REMOTE_BIN'"
  fi
  remote "ls -l '$REMOTE_BIN'; sha256sum '$REMOTE_BIN'" > "$RESULT_ROOT/remote-raypx2.txt"
  ls -l "$CERT_DIR/ca.crt" "$CERT_DIR/server/server.crt" "$CERT_DIR/server/server.key" \
    "$CERT_DIR/client/client.crt" "$CERT_DIR/client/client.key" > "$RESULT_ROOT/cert-files.txt"
  grep -E -- "--proxy|connect-timeout|socks" "$RESULT_ROOT/iperf3-help.txt" > "$RESULT_ROOT/iperf3-proxy-support.txt"
  ip route get "$REMOTE_IP" > "$RESULT_ROOT/local-route.txt"
  remote "ip route get '$LOCAL_IP'" > "$RESULT_ROOT/remote-route.txt"
  ip -o -4 addr show dev "$DATA_IFACE" > "$RESULT_ROOT/local-data-iface.txt"
  remote "ip -o -4 addr show dev '$DATA_IFACE'" > "$RESULT_ROOT/remote-data-iface.txt"
  ip route get 172.16.10.81 > "$RESULT_ROOT/local-management-route.txt"
  remote 'ip route get 172.16.10.81 || true' > "$RESULT_ROOT/remote-management-route.txt"
  if grep -q '172\.16' "$RESULT_ROOT/local-data-iface.txt" "$RESULT_ROOT/remote-data-iface.txt"; then
    log "ERROR: DATA_IFACE has 172.16 address; refusing netem"
    exit 20
  fi
  log "preflight complete"
}

start_services() {
  log "sync certs"
  remote "mkdir -p /home/jack/tcpquic-dgx-certs"
  rsync -az -e "ssh -o BatchMode=yes" "$CERT_DIR/" "$REMOTE_SSH:tcpquic-dgx-certs/"
  remote 'ls -l /home/jack/tcpquic-dgx-certs/ca.crt /home/jack/tcpquic-dgx-certs/server/server.crt /home/jack/tcpquic-dgx-certs/server/server.key /home/jack/tcpquic-dgx-certs/client/client.crt /home/jack/tcpquic-dgx-certs/client/client.key' \
    > "$RESULT_ROOT/remote-cert-files.txt"

  log "one-time cleanup before matrix"
  {
    date -Iseconds
    pkill -9 -x raypx2 2>/dev/null || true
    pkill -9 -x iperf3 2>/dev/null || true
    sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
    tc -s qdisc show dev "$DATA_IFACE" || true
  } > "$RESULT_ROOT/preflight-cleanup-local.log" 2>&1
  remote "killall -9 raypx2 2>/dev/null || true; pkill -9 -x iperf3 2>/dev/null || true; sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true; tc -s qdisc show dev '$DATA_IFACE' || true" \
    > "$RESULT_ROOT/preflight-cleanup-remote.log" 2>&1

  log "start remote iperf3 server"
  remote "iperf3 -s -B '$REMOTE_IP' -p '$IPERF_PORT' -D; pgrep -a -x iperf3; ss -ltnp | grep '$IPERF_PORT'" \
    > "$RESULT_ROOT/proxy/remote-iperf3-server.txt"

  log "start remote raypx2 server"
  remote "rm -f /home/jack/dgx-netem-server.stderr.log /home/jack/dgx-netem-server.pid; LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server --listen '$REMOTE_IP:$QUIC_PORT' --allow-targets '$REMOTE_IP/32,127.0.0.0/8' --cert /home/jack/tcpquic-dgx-certs/server/server.crt --key /home/jack/tcpquic-dgx-certs/server/server.key --ca /home/jack/tcpquic-dgx-certs/ca.crt --compress off --tuning wan --connections 1 --admin-listen '$SERVER_ADMIN_LISTEN' --diag-stats --diag-stats-interval 1 </dev/null >/home/jack/dgx-netem-server.stderr.log 2>&1 & echo \$! > /home/jack/dgx-netem-server.pid"
  for _ in $(seq 1 80); do
    remote 'grep -q "QUIC server listening" /home/jack/dgx-netem-server.stderr.log' && break
    sleep 0.5
  done
  remote 'grep -q "QUIC server listening" /home/jack/dgx-netem-server.stderr.log'
  remote 'grep -q "admin token file" /home/jack/dgx-netem-server.stderr.log'
  remote 'cat /home/jack/dgx-netem-server.pid; ps -p $(cat /home/jack/dgx-netem-server.pid) -o pid,ppid,stat,etime,cmd' \
    > "$RESULT_ROOT/proxy/remote-server-pid.txt"
  remote "grep -m1 'admin token file' /home/jack/dgx-netem-server.stderr.log | awk '{print \\\$NF}'" \
    > "$RESULT_ROOT/proxy/remote-admin-token-file.txt" 2>/dev/null || true

  log "start local raypx2 client"
  rm -f "$RESULT_ROOT/proxy/local-client.stderr.log" "$RESULT_ROOT/proxy/local-client.pid"
  setsid "$BIN" client --peer "$REMOTE_IP:$QUIC_PORT" --http-listen "127.0.0.1:$PROXY_PORT" \
    --socks-listen "127.0.0.1:$SOCKS_PORT" --cert "$CERT_DIR/client/client.crt" \
    --key "$CERT_DIR/client/client.key" --ca "$CERT_DIR/ca.crt" --connections 1 \
    --compress off --tuning wan --admin-listen "$CLIENT_ADMIN_LISTEN" \
    --diag-stats --diag-stats-interval 1 \
    </dev/null > "$RESULT_ROOT/proxy/local-client.stderr.log" 2>&1 &
  echo $! > "$RESULT_ROOT/proxy/local-client.pid"
  for _ in $(seq 1 120); do
    if grep -q "HTTP CONNECT listening" "$RESULT_ROOT/proxy/local-client.stderr.log" &&
       grep -q "QUIC client connected" "$RESULT_ROOT/proxy/local-client.stderr.log"; then
      break
    fi
    sleep 0.5
  done
  grep -q "HTTP CONNECT listening" "$RESULT_ROOT/proxy/local-client.stderr.log"
  grep -q "QUIC client connected" "$RESULT_ROOT/proxy/local-client.stderr.log"
  grep -q "admin token file" "$RESULT_ROOT/proxy/local-client.stderr.log"
  ps -p "$(cat "$RESULT_ROOT/proxy/local-client.pid")" -o pid,ppid,stat,etime,cmd \
    > "$RESULT_ROOT/proxy/local-client-pid.txt"
  grep -m1 'admin token file' "$RESULT_ROOT/proxy/local-client.stderr.log" | awk '{print $NF}' \
    > "$RESULT_ROOT/proxy/local-admin-token-file.txt" 2>/dev/null || true
  log "persistent services ready"
}

run_case() {
  local direction="$1" delay="$2" loss="$3" run="$4"
  case_order=$((case_order + 1))
  local order_fmt label case_dir netem_side rc parse sent recv retrans duration baseline delta base_file
  order_fmt="$(printf 'order-%04d' "$case_order")"
  label="${order_fmt}-${direction}-$(printf '%03d' "$delay")ms-${loss}loss-run${run}"
  case_dir="$RESULT_ROOT/cases/$label"
  mkdir -p "$case_dir"
  echo "$case_order,$direction,$delay,$loss,$run" >> "$SCENARIO_CSV"
  log "case start $label"
  {
    echo "started_at=$(date -Iseconds)"
    echo "direction=$direction"
    echo "delay_ms=$delay"
    echo "loss_pct=$loss"
    echo "run=$run"
    echo "result_dir=$case_dir"
  } > "$case_dir/run.env"

  if ! check_raypx2_alive; then
    log "ABORT $label: raypx2 not alive before case"
    collect_freeze "$case_dir" raypx2_not_alive_before_case
    return 99
  fi

  sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
  remote "sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true"
  tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-before.txt" 2>&1 || true
  remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-before.txt" 2>&1 || true

  if [[ "$direction" == download ]]; then
    netem_side=remote
    remote "sudo -n tc qdisc replace dev '$DATA_IFACE' root netem delay ${delay}ms loss ${loss}% limit ${NETEM_LIMIT}" \
      > "$case_dir/netem-apply.stdout.txt" 2> "$case_dir/netem-apply.stderr.txt" || {
      log "ABORT $label: remote netem apply failed"
      collect_freeze "$case_dir" remote_netem_apply_failed
      return 98
    }
    remote "tc qdisc show dev '$DATA_IFACE'" > "$case_dir/netem-active-qdisc.txt" 2>&1 || true
  else
    netem_side=local
    sudo -n tc qdisc replace dev "$DATA_IFACE" root netem delay "${delay}ms" loss "${loss}%" limit "$NETEM_LIMIT" \
      > "$case_dir/netem-apply.stdout.txt" 2> "$case_dir/netem-apply.stderr.txt" || {
      log "ABORT $label: local netem apply failed"
      collect_freeze "$case_dir" local_netem_apply_failed
      return 98
    }
    tc qdisc show dev "$DATA_IFACE" > "$case_dir/netem-active-qdisc.txt" 2>&1 || true
  fi
  if ! grep -Eq "netem.*delay ${delay}ms.*loss ${loss}%" "$case_dir/netem-active-qdisc.txt"; then
    log "ABORT $label: netem verify failed"
    collect_freeze "$case_dir" netem_verify_failed
    return 97
  fi

  sample_qdisc local "$case_dir/local-qdisc-samples.txt" &
  local lsampler=$!
  sample_qdisc remote "$case_dir/remote-qdisc-samples.txt" &
  local rsampler=$!

  set +e
  if [[ "$direction" == download ]]; then
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" \
      -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 --reverse \
      --proxy "http://127.0.0.1:${PROXY_PORT}" \
      > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
  else
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" \
      -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 \
      --proxy "http://127.0.0.1:${PROXY_PORT}" \
      > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
  fi
  rc=$?
  set -e
  echo "$rc" > "$case_dir/iperf.rc"
  kill "$lsampler" "$rsampler" 2>/dev/null || true
  wait "$lsampler" "$rsampler" 2>/dev/null || true

  tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-after.txt" 2>&1 || true
  remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-after.txt" 2>&1 || true
  cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$case_dir/local-proxy.case.log" 2>/dev/null || true
  remote 'cat /home/jack/dgx-netem-server.stderr.log' > "$case_dir/remote-proxy.case.log" 2>&1 || true

  if [[ "$rc" != 0 ]]; then
    log "ABORT $label: iperf rc=$rc"
    collect_freeze "$case_dir" "iperf_rc_${rc}"
    return 96
  fi
  parse="$(parse_iperf "$case_dir/iperf.stdout.json")" || {
    log "ABORT $label: iperf json parse failed"
    collect_freeze "$case_dir" iperf_json_parse_failed
    return 95
  }
  sent="$(printf '%s' "$parse" | cut -d, -f1)"
  recv="$(printf '%s' "$parse" | cut -d, -f2)"
  retrans="$(printf '%s' "$parse" | cut -d, -f3)"
  duration="$(printf '%s' "$parse" | cut -d, -f4)"
  if python3 - "$recv" <<'PY'
import sys
sys.exit(0 if float(sys.argv[1]) <= 0.0 else 1)
PY
  then
    log "ABORT $label: received_mbps=$recv"
    collect_freeze "$case_dir" zero_received_mbps
    return 94
  fi
  if ! check_raypx2_alive; then
    log "ABORT $label: raypx2 exited after case"
    collect_freeze "$case_dir" raypx2_exited_after_case
    return 93
  fi

  base_file="$RESULT_ROOT/summary/baseline-${direction}-${delay}-${loss}.txt"
  baseline="$(cat "$base_file" 2>/dev/null || true)"
  if [[ -n "$baseline" ]]; then
    delta="$(python3 - "$recv" "$baseline" <<'PY'
import sys
recv=float(sys.argv[1]); base=float(sys.argv[2])
print(f"{abs(recv-base)/base*100:.2f}" if base > 0 else "0.00")
PY
)"
  else
    baseline=""
    delta=""
  fi

  printf '%s\n' "$case_order,$direction,$delay,$loss,$run,$rc,$sent,$recv,$baseline,$delta,$retrans,$duration,$netem_side,$DATA_IFACE,$NETEM_LIMIT,ok" >> "$SUMMARY_CSV"
  log "case ok $label recv=${recv}Mbps sent=${sent}Mbps delta=${delta:-pending}%"
}

check_combo_variance() {
  local direction="$1" delay="$2" loss="$3"
  local result
  result="$(python3 - "$SUMMARY_CSV" "$direction" "$delay" "$loss" <<'PY'
import csv, statistics, sys
path, direction, delay, loss = sys.argv[1:5]
rows = [r for r in csv.DictReader(open(path, newline=""))
        if r["direction"] == direction and r["delay_ms"] == delay and r["loss_pct"] == loss and r["notes"] == "ok"]
vals = [float(r["received_mbps"]) for r in rows]
if len(vals) < 3:
    print("PENDING")
else:
    med = statistics.median(vals)
    deltas = [abs(v - med) / med * 100 if med > 0 else 0 for v in vals]
    print(f"{med:.2f}," + ",".join(f"{d:.2f}" for d in deltas))
PY
)"
  [[ "$result" != "PENDING" ]] || return 0
  local median max_delta
  median="$(printf '%s' "$result" | cut -d, -f1)"
  max_delta="$(printf '%s' "$result" | tr ',' '\n' | tail -n +2 | sort -nr | head -1)"
  echo "$median" > "$RESULT_ROOT/summary/baseline-${direction}-${delay}-${loss}.txt"
  if python3 - "$max_delta" <<'PY'
import sys
sys.exit(0 if float(sys.argv[1]) > 20.0 else 1)
PY
  then
    if [[ "$CONTINUE_ON_VARIANCE" == 1 ]]; then
      local anomaly_dir="$RESULT_ROOT/anomalies/combo-${direction}-$(printf '%03d' "$delay")ms-${loss}loss"
      mkdir -p "$anomaly_dir"
      {
        echo "time=$(date -Iseconds)"
        echo "reason=combo_median_delta_gt_20"
        echo "direction=$direction"
        echo "delay_ms=$delay"
        echo "loss_pct=$loss"
        echo "median_mbps=$median"
        echo "max_delta_pct=$max_delta"
      } > "$anomaly_dir/reason.txt"
      printf '%s\n' "variance anomaly: ${direction}-${delay}ms-${loss}loss median=${median} max_delta=${max_delta}% > 20%" >> "$RESULT_ROOT/known-anomalies.txt"
      log "WARN ${direction}-${delay}ms-${loss}loss: 3-run median delta max ${max_delta}% > 20% (median=$median); recorded and continuing"
      return 0
    fi
    log "ABORT ${direction}-${delay}ms-${loss}loss: 3-run median delta max ${max_delta}% > 20% (median=$median)"
    collect_freeze "$RESULT_ROOT/cases/order-$(printf '%04d' "$case_order")-${direction}-$(printf '%03d' "$delay")ms-${loss}loss-run${RUNS}" "combo_median_delta_gt_20"
    return 92
  fi
  log "combo ok ${direction}-${delay}ms-${loss}loss median=${median} max_delta=${max_delta}%"
}

main() {
  if [[ "$SKIP_PREFLIGHT" == 1 ]]; then
    log "skip preflight; resume existing result root"
  else
    preflight
  fi
  if [[ "$SKIP_START_SERVICES" == 1 ]]; then
    log "skip start_services; reusing existing raypx2 and iperf3 processes"
  else
    start_services
  fi
  if [[ "$APPEND_SUMMARY" != 1 ]]; then
    cat > "$SUMMARY_CSV" <<'EOF'
order,direction,delay_ms,loss_pct,run,iperf_rc,sent_mbps,received_mbps,baseline_mbps,baseline_delta_pct,retransmits,duration_sec,netem_side,netem_iface,netem_limit,notes
EOF
    cat > "$SCENARIO_CSV" <<'EOF'
order,direction,delay_ms,loss_pct,run
EOF
  fi
  case_order=0
  for loss in "${LOSSES[@]}"; do
    for delay in "${DELAYS[@]}"; do
      for direction in download upload; do
        for run in $(seq 1 "$RUNS"); do
          if (( case_order + 1 <= RESUME_AFTER_ORDER )); then
            case_order=$((case_order + 1))
            continue
          fi
          [[ ! -e "$STOP_FILE" ]] || exit 90
          run_case "$direction" "$delay" "$loss" "$run" || exit $?
        done
        if (( case_order <= RESUME_AFTER_ORDER )); then
          continue
        fi
        check_combo_variance "$direction" "$delay" "$loss" || exit $?
      done
    done
  done
  log "matrix cases complete; cleaning netem"
  sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
  remote "sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true"
  write_summary_md
  log "summary written $RESULT_ROOT/summary.md"
}

main "$@"
