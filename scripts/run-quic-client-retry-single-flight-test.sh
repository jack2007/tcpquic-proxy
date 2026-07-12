#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/bin/Release/raypx2}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/docs/test/quic-client-retry-single-flight-$(date +%Y%m%d-%H%M%S)}"
SOAK_SECONDS="${SOAK_SECONDS:-600}"
RECOVERY_TIMEOUT_MS="${RECOVERY_TIMEOUT_MS:-3500}"
HOLD_FOR_K6_SECONDS="${HOLD_FOR_K6_SECONDS:-0}"
ENV_OUT="${ENV_OUT:-}"
ANALYZE_FIXTURE="${ANALYZE_FIXTURE:-}"

[[ "$(uname -s)" == "Linux" ]] || { echo "this system harness is Linux-only" >&2; exit 2; }

analyze_evidence() {
  local evidence=$1 failed_port=$2 duration_ms=$3
  python3 - "$evidence" "$failed_port" "$duration_ms" <<'PY'
import csv, datetime, json, math, pathlib, re, statistics, sys

root=pathlib.Path(sys.argv[1]); failed_port=sys.argv[2]; duration_ms=int(sys.argv[3])
required_top=(
    'ingress_delayed_task_queue_depth','ingress_delayed_task_queue_depth_max',
    'ingress_reactor_timeout_overshoot_max_us','ingress_reactor_timeout_overshoot_p95_us',
    'ingress_reactor_timeout_overshoot_p99_us','ingress_reactor_timeout_overshoot_samples')
required_peer=('retry_scheduled_total','retry_executed_total','retry_stale_dropped_total','retry_schedule_failed_total')
metric_rows=[]
for path in sorted((root/'admin').glob('metrics-*.json'), key=lambda p:int(re.search(r'(\d+)',p.stem).group(1))):
    data=json.loads(path.read_text())
    for key in required_top:
        if key not in data: raise SystemExit(f'missing metric: {key} in {path}')
    peer=next((p for p in data.get('peers',[]) if p.get('peer_id') == 'failed'), None)
    if peer is None: raise SystemExit(f'missing metric peer: failed in {path}')
    for key in required_peer:
        if key not in peer: raise SystemExit(f'missing metric: failed.{key} in {path}')
    elapsed=int(re.search(r'(\d+)',path.stem).group(1))
    metric_rows.append({'elapsed_ms':elapsed, **{k:peer[k] for k in required_peer}, **{k:data[k] for k in required_top}})
if not metric_rows: raise SystemExit('missing metric samples')
with (root/'retry-metrics.csv').open('w',newline='') as out:
    writer=csv.DictWriter(out,fieldnames=('elapsed_ms',)+required_peer+required_top); writer.writeheader(); writer.writerows(metric_rows)

trace=(root/'client-trace.log').read_text(errors='replace').splitlines()
lines=[line for line in trace if 'event=quic_connecting' in line and f'peer=127.0.0.1:{failed_port}' in line]
if not lines: raise SystemExit('failed peer produced no event=quic_connecting trace')
times=[]
for line in lines:
    match=re.search(r'\[(\d{4}-\d\d-\d\d[ T][0-9:.]+)\]',line)
    if not match: raise SystemExit(f'unparseable connecting timestamp: {line}')
    try: stamp=datetime.datetime.fromisoformat(match.group(1).replace(' ','T')).timestamp()
    except ValueError: raise SystemExit(f'unparseable connecting timestamp: {line}')
    if times and stamp <= times[-1]: raise SystemExit('connecting timestamps are not strictly monotonic')
    times.append(stamp)
duration=duration_ms/1000
limit=math.ceil(duration / 3) + 1
if len(times)>limit: raise SystemExit(f'failed peer attempts {len(times)} exceed {limit}')
intervals=[(b-a)*1000 for a,b in zip(times,times[1:])]
if intervals and min(intervals)<2900: raise SystemExit(f'retry interval {min(intervals):.0f}ms below 2900ms')
if intervals and max(intervals)>3500: raise SystemExit(f'retry interval {max(intervals):.0f}ms above 3500ms')

with (root/'resources.csv').open() as inp: resources=list(csv.DictReader(inp))
if not resources: raise SystemExit('missing resource samples')
for row in resources:
    for key in ('elapsed_ms','cpu_ticks','clock_ticks_per_second','rss_kb','trace_log_bytes'):
        if key not in row or row[key]=='': raise SystemExit(f'missing resource: {key}')
elapsed=[int(r['elapsed_ms']) for r in resources]
ticks=[int(r['cpu_ticks']) for r in resources]
clock_hz=[int(r['clock_ticks_per_second']) for r in resources]
rss=[int(r['rss_kb']) for r in resources]
logs=[int(r['trace_log_bytes']) for r in resources]
queue=[int(r['ingress_delayed_task_queue_depth']) for r in metric_rows]
cpu=[]; cpu_elapsed=[]
for before,after,t0,t1,hz in zip(ticks,ticks[1:],elapsed,elapsed[1:],clock_hz[1:]):
    if t1<=t0 or after<before or hz<=0: raise SystemExit('invalid monotonic CPU sample')
    cpu.append((after-before)*100000/(hz*(t1-t0))); cpu_elapsed.append(t1)
if not cpu: cpu=[0.0]; cpu_elapsed=[duration_ms]

def rising_three_windows(values, name):
    if any(a < b < c < d for a,b,c,d in zip(values,values[1:],values[2:],values[3:])):
        raise SystemExit(f'{name} monotonic rise across three sampling windows')
rising_three_windows(rss,'rss')
rising_three_windows(queue,'delayed queue depth')
growth=[b-a for a,b in zip(logs,logs[1:])]
if any(a < b < c for a,b,c in zip(growth,growth[1:],growth[2:])):
    raise SystemExit('trace log growth rate monotonic rise across three sampling windows')

baseline_values=[v for t,v in zip(cpu_elapsed,cpu) if t <= 120000] or cpu[:1]
final_values=[v for t,v in zip(cpu_elapsed,cpu) if t >= max(0,duration_ms-120000)] or cpu[-1:]
baseline=statistics.fmean(baseline_values); final=statistics.fmean(final_values)
if final>5.0: raise SystemExit(f'final CPU mean {final:.3f}% exceeds single-core 5%')
if final>baseline*1.5 and final>0.1: raise SystemExit(f'final CPU mean {final:.3f}% exceeds baseline {baseline:.3f}% x1.5')
reactor_p99=max(int(r['ingress_reactor_timeout_overshoot_p99_us']) for r in metric_rows)
if reactor_p99>500000: raise SystemExit(f'reactor p99 delay {reactor_p99}us exceeds 500ms handshake budget')

summary=(
 f'PASS\nactual_soak_ms={duration_ms}\nfailed_peer_attempts={len(times)}\nmax_attempts={limit}\n'
 f'cpu_baseline_mean={baseline:.3f}\ncpu_final_mean={final:.3f}\nreactor_p99_max_us={reactor_p99}\n'
 'rss_trend=pass\ndelayed_queue_trend=pass\ntrace_log_growth_trend=pass\n')
(root/'summary.txt').write_text(summary)
print(summary,end='')
PY
}

if [[ -n "$ANALYZE_FIXTURE" ]]; then
  analyze_evidence "$ANALYZE_FIXTURE" "${FAILED_QUIC_PORT:?FAILED_QUIC_PORT required}" "$((SOAK_SECONDS * 1000))"
  exit
fi

TMP="$(mktemp -d)"
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

alloc_port() {
  python3 - <<'PY'
import socket
s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()
PY
}

monotonic_ms() {
  local uptime whole fraction
  read -r uptime _ </proc/uptime
  whole=${uptime%.*}; fraction=${uptime#*.}; fraction="${fraction}000"
  printf '%d\n' "$((10#$whole * 1000 + 10#${fraction:0:3}))"
}

admin_port=$(alloc_port)
failed_quic_port=$(alloc_port)
healthy_quic_port=$(alloc_port)
healthy_socks_port=$(alloc_port)
failed_socks_port=$(alloc_port)
target_http_port=$(alloc_port)
BASE_URL="http://127.0.0.1:$admin_port/api/v1"
cp "$BIN" "$TMP/raypx2"
chmod +x "$TMP/raypx2"
TEST_BIN="$TMP/raypx2"
TRACE_LOG="$TMP/log/client.log"
trace_start_bytes=0
[[ -f "$TRACE_LOG" ]] && trace_start_bytes=$(wc -c <"$TRACE_LOG")

mkdir -p "$TMP/http" "$RESULT_ROOT/admin"
printf 'ok\n' >"$TMP/http/health.txt"
cat >"$TMP/client.json" <<JSON
{
  "admin":{"listen":"127.0.0.1:$admin_port","threads":2,"token_file":"$TMP/admin-token.json"},
  "client":{"client_name":"retry-single-flight-test"},
  "peers":[
    {"id":"failed","enabled":true,"proto_peer":"127.0.0.1:$failed_quic_port","proto_connections":1,"socks_listen":"127.0.0.1:$failed_socks_port"},
    {"id":"healthy","enabled":true,"proto_peer":"127.0.0.1:$healthy_quic_port","proto_connections":1,"socks_listen":"127.0.0.1:$healthy_socks_port"}
  ],
  "proto":{"connections":1,"keepalive_ms":5000,"disable_1rtt_encryption":true},
  "tls":{"ca":"$ROOT/cert/ca.crt"},
  "trace":{"enabled":true,"level":"info","interval_sec":30}
}
JSON

python3 -m http.server "$target_http_port" --bind 127.0.0.1 --directory "$TMP/http" \
  >"$RESULT_ROOT/http.log" 2>&1 & PIDS+=("$!")
"$TEST_BIN" server --listen "127.0.0.1:$healthy_quic_port" \
  --cert "$ROOT/cert/server/server.crt" --key "$ROOT/cert/server/server.key" \
  >"$RESULT_ROOT/healthy-server.log" 2>&1 & PIDS+=("$!")
client_start_ms=$(monotonic_ms)
"$TEST_BIN" client --ca "$ROOT/cert/ca.crt" --config "$TMP/client.json" \
  --admin-listen "127.0.0.1:$admin_port" --admin-token-file "$TMP/admin-token.json" \
  --trace --trace-interval 30 \
  >"$RESULT_ROOT/client.log" 2>&1 & client_pid=$!; PIDS+=("$client_pid")

TOKEN=""
for _ in $(seq 1 600); do
  if [[ -s "$TMP/admin-token.json" ]]; then
    TOKEN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["token"])' "$TMP/admin-token.json")
    if curl --fail --silent --show-error --connect-timeout 1 --max-time 2 -H "Authorization: Bearer $TOKEN" \
      "$BASE_URL/peers" >"$RESULT_ROOT/admin/initial-peers.json"; then
      break
    fi
  fi
  sleep 0.1
done
[[ -n "$TOKEN" ]] || { echo "admin token unavailable" >&2; exit 1; }
curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
  -H "Authorization: Bearer $TOKEN" "$BASE_URL/peers" \
  >"$RESULT_ROOT/admin/initial-peers.json"
curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
  -H "Authorization: Bearer $TOKEN" "$BASE_URL/metrics" \
  >"$RESULT_ROOT/admin/startup-metrics.json"
printf 'client_start_ms,captured_ms,cpu_ticks,clock_ticks_per_second,rss_kb,trace_log_bytes\n' \
  >"$RESULT_ROOT/startup-resources.csv"
startup_ticks=$(awk '{print $14+$15}' "/proc/$client_pid/stat")
startup_rss=$(ps -o rss= -p "$client_pid" | tr -d ' ')
startup_trace_bytes=0; [[ -f "$TRACE_LOG" ]] && startup_trace_bytes=$(wc -c <"$TRACE_LOG")
printf '%s,%s,%s,%s,%s,%s\n' "$client_start_ms" "$(monotonic_ms)" "$startup_ticks" \
  "$(getconf CLK_TCK)" "$startup_rss" "$startup_trace_bytes" >>"$RESULT_ROOT/startup-resources.csv"

ready_deadline=$(( $(monotonic_ms) + 60000 ))
healthy_ready=0
while (( $(monotonic_ms) <= ready_deadline )); do
  if curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
      -H "Authorization: Bearer $TOKEN" "$BASE_URL/peers" >"$RESULT_ROOT/admin/ready-peers.json" &&
    python3 - "$RESULT_ROOT/admin/ready-peers.json" <<'PY'
import json,sys
peers=json.load(open(sys.argv[1])).get('peers',[])
raise SystemExit(0 if any(p.get('peer_id')=='healthy' and p.get('connected_connections',0)>=1 for p in peers) else 1)
PY
    curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
      --socks5-hostname "127.0.0.1:$healthy_socks_port" \
      "http://127.0.0.1:$target_http_port/health.txt" >/dev/null; then
    healthy_ready=1; break
  fi
  sleep 0.1
done
(( healthy_ready == 1 )) || { echo "healthy peer readiness barrier timed out" >&2; exit 1; }
healthy_ready_ms=$(monotonic_ms)
printf '%s\n' "$healthy_ready_ms" >"$RESULT_ROOT/healthy-ready-ms.txt"

clock_ticks_per_second=$(getconf CLK_TCK)
printf 'elapsed_ms,cpu_ticks,clock_ticks_per_second,rss_kb,trace_log_bytes\n' >"$RESULT_ROOT/resources.csv"
healthy_probe_failures=0

sample_resources() {
  local elapsed=$1 rss bytes cpu_ticks metrics_file pidstat_file
  rss=$(ps -o rss= -p "$client_pid" | tr -d ' ' || true)
  cpu_ticks=$(awk '{print $14+$15}' "/proc/$client_pid/stat")
  bytes=0; [[ -f "$TRACE_LOG" ]] && bytes=$(wc -c <"$TRACE_LOG")
  metrics_file="$RESULT_ROOT/admin/metrics-$elapsed.json"
  curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    -H "Authorization: Bearer $TOKEN" "$BASE_URL/metrics" >"$metrics_file"
  printf '%s,%s,%s,%s,%s\n' "$elapsed" "$cpu_ticks" "$clock_ticks_per_second" "${rss:-0}" "$bytes" >>"$RESULT_ROOT/resources.csv"
  if command -v pidstat >/dev/null 2>&1; then
    pidstat_file="$RESULT_ROOT/pidstat-$elapsed.txt"
    pidstat -p "$client_pid" 1 1 >"$pidstat_file" 2>&1 & PIDS+=("$!")
  fi
}

soak_start_ms=$(monotonic_ms)
soak_deadline_ms=$((soak_start_ms + SOAK_SECONDS * 1000))
next_probe_ms=$soak_start_ms
next_sample_ms=$soak_start_ms
sample_resources 0
while (( $(monotonic_ms) < soak_deadline_ms )); do
  now_ms=$(monotonic_ms)
  if (( now_ms >= next_probe_ms )); then
    if ! curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    --socks5-hostname "127.0.0.1:$healthy_socks_port" \
    "http://127.0.0.1:$target_http_port/health.txt" >/dev/null; then
      healthy_probe_failures=$((healthy_probe_failures + 1))
    fi
    next_probe_ms=$((next_probe_ms + 1000))
  fi
  if (( now_ms >= next_sample_ms + 10000 )); then
    sample_resources "$((now_ms - soak_start_ms))"
    next_sample_ms=$now_ms
  fi
  sleep 0.05
done
soak_end_ms=$(monotonic_ms)
actual_soak_ms=$((soak_end_ms - soak_start_ms))
actual_attempt_window_ms=$((soak_end_ms - client_start_ms))
sample_resources "$actual_soak_ms"

(( healthy_probe_failures == 0 )) || { echo "healthy probe failures: $healthy_probe_failures" >&2; exit 1; }
if [[ -f "$TRACE_LOG" ]]; then
  tail -c "+$((trace_start_bytes + 1))" "$TRACE_LOG" >"$RESULT_ROOT/client-trace.log"
else
  : >"$RESULT_ROOT/client-trace.log"
fi
analyze_evidence "$RESULT_ROOT" "$failed_quic_port" "$actual_attempt_window_ms"

"$TEST_BIN" server --listen "127.0.0.1:$failed_quic_port" \
  --cert "$ROOT/cert/server/server.crt" --key "$ROOT/cert/server/server.key" \
  >"$RESULT_ROOT/failed-server.log" 2>&1 & PIDS+=("$!")
deadline=$(( $(monotonic_ms) + RECOVERY_TIMEOUT_MS ))
recovered=0
while (( $(monotonic_ms) <= deadline )); do
  curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
    -H "Authorization: Bearer $TOKEN" "$BASE_URL/peers" \
    >"$RESULT_ROOT/admin/recovery-peers.json"
  if python3 - "$RESULT_ROOT/admin/recovery-peers.json" <<'PY'
import json,sys
peers=json.load(open(sys.argv[1])).get('peers', [])
raise SystemExit(0 if any(p.get('peer_id') == 'failed' and p.get('connected_connections', 0) >= 1 for p in peers) else 1)
PY
  then
    if (( $(monotonic_ms) <= deadline )); then recovered=1; fi
    break
  fi
  sleep 0.1
done
(( recovered == 1 )) || { echo "failed peer did not recover within ${RECOVERY_TIMEOUT_MS}ms" >&2; exit 1; }

curl --fail --silent --show-error --connect-timeout 1 --max-time 2 \
  -H "Authorization: Bearer $TOKEN" "$BASE_URL/metrics" \
  >"$RESULT_ROOT/admin/final-metrics.json"
printf 'healthy_probe_failures=%s\nfailed_peer_recovered=1\n' "$healthy_probe_failures" >>"$RESULT_ROOT/summary.txt"

if (( HOLD_FOR_K6_SECONDS > 0 )); then
  [[ -n "$ENV_OUT" ]] || { echo "ENV_OUT is required when HOLD_FOR_K6_SECONDS > 0" >&2; exit 1; }
  umask 077
  {
    printf 'TOKEN=%q\n' "$TOKEN"
    printf 'BASE_URL=%q\n' "$BASE_URL"
    printf 'CLIENT_PID=%q\n' "$client_pid"
    printf 'RESULT_ROOT=%q\n' "$RESULT_ROOT"
  } >"$ENV_OUT"
  sleep "$HOLD_FOR_K6_SECONDS"
fi

echo "PASS result_root=$RESULT_ROOT"
