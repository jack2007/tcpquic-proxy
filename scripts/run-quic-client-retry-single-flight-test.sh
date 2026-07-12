#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/bin/Release/raypx2}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/docs/test/quic-client-retry-single-flight-$(date +%Y%m%d-%H%M%S)}"
SOAK_SECONDS="${SOAK_SECONDS:-600}"
RECOVERY_TIMEOUT_MS="${RECOVERY_TIMEOUT_MS:-3500}"
HOLD_FOR_K6_SECONDS="${HOLD_FOR_K6_SECONDS:-0}"
ENV_OUT="${ENV_OUT:-}"
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

admin_port=$(alloc_port)
failed_quic_port=$(alloc_port)
healthy_quic_port=$(alloc_port)
healthy_socks_port=$(alloc_port)
failed_socks_port=$(alloc_port)
target_http_port=$(alloc_port)
BASE_URL="http://127.0.0.1:$admin_port/api/v1"
TRACE_LOG="$(dirname "$BIN")/log/client.log"
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
"$BIN" server --listen "127.0.0.1:$healthy_quic_port" \
  --cert "$ROOT/cert/server/server.crt" --key "$ROOT/cert/server/server.key" \
  >"$RESULT_ROOT/healthy-server.log" 2>&1 & PIDS+=("$!")
"$BIN" client --ca "$ROOT/cert/ca.crt" --config "$TMP/client.json" \
  --admin-listen "127.0.0.1:$admin_port" --admin-token-file "$TMP/admin-token.json" \
  --trace --trace-interval 30 \
  >"$RESULT_ROOT/client.log" 2>&1 & client_pid=$!; PIDS+=("$client_pid")

TOKEN=""
for _ in $(seq 1 600); do
  if [[ -s "$TMP/admin-token.json" ]]; then
    TOKEN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["token"])' "$TMP/admin-token.json")
    if curl --fail --silent --show-error -H "Authorization: Bearer $TOKEN" \
      "$BASE_URL/peers" >"$RESULT_ROOT/admin/initial-peers.json"; then
      break
    fi
  fi
  sleep 0.1
done
[[ -n "$TOKEN" ]] || { echo "admin token unavailable" >&2; exit 1; }
curl --fail --silent --show-error -H "Authorization: Bearer $TOKEN" "$BASE_URL/peers" \
  >"$RESULT_ROOT/admin/initial-peers.json"

printf 'second,healthy_probe_failures,client_rss_kb,client_log_bytes\n' >"$RESULT_ROOT/resources.csv"
printf 'second,retry_scheduled,retry_executed,retry_stale_dropped,retry_schedule_failed,delayed_task_queue_depth,reactor_wakeup_overshoot_p95_us,reactor_wakeup_overshoot_p99_us\n' \
  >"$RESULT_ROOT/retry-metrics.csv"
healthy_probe_failures=0

sample_resources() {
  local second=$1 rss bytes metrics_file pidstat_file
  rss=$(ps -o rss= -p "$client_pid" | tr -d ' ' || true)
  bytes=$(wc -c <"$RESULT_ROOT/client.log")
  metrics_file="$RESULT_ROOT/admin/metrics-$second.json"
  curl --fail --silent --show-error -H "Authorization: Bearer $TOKEN" "$BASE_URL/metrics" >"$metrics_file"
  printf '%s,%s,%s,%s\n' "$second" "$healthy_probe_failures" "${rss:-0}" "$bytes" >>"$RESULT_ROOT/resources.csv"
  if command -v pidstat >/dev/null 2>&1; then
    pidstat_file="$RESULT_ROOT/pidstat-$second.txt"
    pidstat -p "$client_pid" 1 1 >"$pidstat_file" 2>&1 || true
  fi
  python3 - "$metrics_file" "$second" >>"$RESULT_ROOT/retry-metrics.csv" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
keys=('retry_scheduled','retry_executed','retry_stale_dropped','retry_schedule_failed',
      'delayed_task_queue_depth','reactor_wakeup_overshoot_p95_us','reactor_wakeup_overshoot_p99_us')
print(','.join([sys.argv[2]] + [str(d.get(k, 0)) for k in keys]))
PY
}

for ((second=1; second<=SOAK_SECONDS; second++)); do
  if ! curl --fail --silent --show-error \
    --socks5-hostname "127.0.0.1:$healthy_socks_port" \
    "http://127.0.0.1:$target_http_port/health.txt" >/dev/null; then
    healthy_probe_failures=$((healthy_probe_failures + 1))
  fi
  if (( second % 10 == 0 || second == SOAK_SECONDS )); then
    sample_resources "$second"
  fi
  sleep 1
done

(( healthy_probe_failures == 0 )) || { echo "healthy probe failures: $healthy_probe_failures" >&2; exit 1; }
max_attempts=$(( (SOAK_SECONDS + 2) / 3 + 1 ))
(( max_attempts <= 201 )) || max_attempts=201
if [[ -f "$TRACE_LOG" ]]; then
  tail -c "+$((trace_start_bytes + 1))" "$TRACE_LOG" >"$RESULT_ROOT/client-trace.log"
else
  : >"$RESULT_ROOT/client-trace.log"
fi
python3 - "$RESULT_ROOT/client-trace.log" "$failed_quic_port" "$max_attempts" <<'PY'
import datetime, re, sys
lines=[]
for line in open(sys.argv[1], errors='replace'):
    if 'event=quic_connecting' in line and ('peer=127.0.0.1:'+sys.argv[2]) in line:
        lines.append(line)
if len(lines) > int(sys.argv[3]):
    raise SystemExit(f'failed peer attempts {len(lines)} exceed {sys.argv[3]}')
if not lines:
    raise SystemExit('failed peer produced no event=quic_connecting trace')
times=[]
for line in lines:
    m=re.search(r'(\d{4}-\d\d-\d\d[T ][0-9:.]+)', line)
    if m:
        times.append(datetime.datetime.fromisoformat(m.group(1).replace(' ', 'T')).timestamp())
for a,b in zip(times,times[1:]):
    if (b-a)*1000 < 2900:
        raise SystemExit(f'retry interval {(b-a)*1000:.0f}ms below 2900ms')
print(f'failed_peer_attempts={len(lines)} parsed_intervals={max(0,len(times)-1)}')
PY

"$BIN" server --listen "127.0.0.1:$failed_quic_port" \
  --cert "$ROOT/cert/server/server.crt" --key "$ROOT/cert/server/server.key" \
  >"$RESULT_ROOT/failed-server.log" 2>&1 & PIDS+=("$!")
deadline=$(( $(date +%s%3N) + RECOVERY_TIMEOUT_MS ))
recovered=0
while (( $(date +%s%3N) <= deadline )); do
  curl --fail --silent --show-error -H "Authorization: Bearer $TOKEN" "$BASE_URL/peers" \
    >"$RESULT_ROOT/admin/recovery-peers.json"
  if python3 - "$RESULT_ROOT/admin/recovery-peers.json" <<'PY'
import json,sys
peers=json.load(open(sys.argv[1])).get('peers', [])
raise SystemExit(0 if any(p.get('peer_id') == 'failed' and p.get('connected_connections', 0) >= 1 for p in peers) else 1)
PY
  then recovered=1; break; fi
  sleep 0.1
done
(( recovered == 1 )) || { echo "failed peer did not recover within ${RECOVERY_TIMEOUT_MS}ms" >&2; exit 1; }

curl --fail --silent --show-error -H "Authorization: Bearer $TOKEN" "$BASE_URL/metrics" \
  >"$RESULT_ROOT/admin/final-metrics.json"
printf 'healthy_probe_failures=%s\nfailed_peer_recovered=1\n' "$healthy_probe_failures" >"$RESULT_ROOT/summary.txt"

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
