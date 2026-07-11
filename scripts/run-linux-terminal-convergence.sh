#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORKLOAD="$ROOT/docs/test/k6/linux-terminal-convergence.js"
if [[ -n "${TCPQUIC_BINARY:-}" ]]; then
  BINARY=$TCPQUIC_BINARY
elif [[ -x "$ROOT/build/bin/Release/raypx2" ]]; then
  BINARY="$ROOT/build/bin/Release/raypx2"
else
  BINARY="$ROOT/build/bin/Release/tcpquic-proxy"
fi
SCENARIO=""
CLIENT_CONFIG=""
SERVER_CONFIG=""
OUT=""
BASELINE_SUMMARY=""
SELF_TEST=0

usage() {
  echo "usage: $0 --self-test | --scenario incident|baseline|peak|spike|stress|soak --client-config PATH --server-config PATH --out DIR [--baseline-summary PATH]" >&2
}

while (($#)); do
  case "$1" in
    --self-test) SELF_TEST=1; shift ;;
    --scenario) SCENARIO=${2:?missing scenario}; shift 2 ;;
    --client-config) CLIENT_CONFIG=${2:?missing client config}; shift 2 ;;
    --server-config) SERVER_CONFIG=${2:?missing server config}; shift 2 ;;
    --out) OUT=${2:?missing output directory}; shift 2 ;;
    --baseline-summary) BASELINE_SUMMARY=${2:?missing baseline summary}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

self_test() {
  bash -n "$0"
  test -r "$WORKLOAD"
  grep -Fq "'checks{workload_normal:true}': ['rate>0.999']" "$WORKLOAD"
  grep -Fq "'http_req_failed{workload_normal:true}': ['rate<0.001']" "$WORKLOAD"
  grep -Fq "dropped_iterations: ['count==0']" "$WORKLOAD"
  grep -Fq "'http_req_duration{workload_normal:true}': ['p(95)<500', 'p(99)<1000']" "$WORKLOAD"
  grep -Fq "fatal_terminal_latency" "$WORKLOAD"
  grep -Fq "rst_count" "$WORKLOAD"
  grep -Fq "fin_reverse_flow_checks" "$WORKLOAD"
  grep -Fq "fatal_terminal_latency: ['p(95)<1000', 'p(99)<5000']" "$WORKLOAD"
  for name in baseline peak spike stress soak; do grep -Eq "^[[:space:]]*$name:" "$WORKLOAD"; done
  local fixture
  fixture=$(mktemp -d); trap 'rm -rf "$fixture"' RETURN
  printf '%s\n' '{"active_relays":0,"terminal_sink_pending":0,"terminal_timeout_pending":0,"terminal_exactly_once_violation":0,"terminal_retained_owner_count":0,"linux_relay_outstanding_quic_sends":0,"linux_relay_pending_events":0,"linux_relay_pending_tcp_write_queue":0,"terminal_shutdown_submitted":1,"terminal_observed":1,"terminal_watchdog_armed":1,"terminal_watchdog_canceled":1,"terminal_watchdog_timeout":0}' >"$fixture/good.json"
  jq -e 'has("active_relays") and has("terminal_sink_pending") and has("linux_relay_outstanding_quic_sends")' "$fixture/good.json" >/dev/null
  if jq -e 'has("missing_required")' "$fixture/good.json" >/dev/null; then echo "self-test accepted missing field" >&2; return 1; fi
  jq '.linux_relay_outstanding_quic_sends=1' "$fixture/good.json" >"$fixture/pending.json"
  if jq -e '.linux_relay_outstanding_quic_sends==0' "$fixture/pending.json" >/dev/null; then echo "self-test accepted pending send" >&2; return 1; fi
  if jq -n -e --argjson rst_rc 0 '$rst_rc != 0' >/dev/null; then echo "self-test accepted successful RST curl" >&2; return 1; fi
  jq '.terminal_timeout_pending=1' "$fixture/good.json" >"$fixture/timeout.json"
  if jq -e '.terminal_timeout_pending==0' "$fixture/timeout.json" >/dev/null; then echo "self-test accepted timeout pending" >&2; return 1; fi
  jq '.terminal_watchdog_armed=2' "$fixture/good.json" >"$fixture/watchdog.json"
  if jq -e '(.terminal_watchdog_armed-.terminal_watchdog_canceled-.terminal_watchdog_timeout)==0' "$fixture/watchdog.json" >/dev/null; then echo "self-test accepted watchdog pending" >&2; return 1; fi
  if jq -n -e --argjson pre 4 --argjson final 4 '($final-$pre)>0' >/dev/null; then echo "self-test attributed FIN-only metric to RST" >&2; return 1; fi
  printf '%s\n' '{"terminal_convergence":{"rst_count":{"values":{"count":2}},"fatal_terminal_latency":{"values":{"count":1}}}}' >"$fixture/uncorrelated.json"
  if jq -e '.terminal_convergence.rst_count.values.count==.terminal_convergence.fatal_terminal_latency.values.count' "$fixture/uncorrelated.json" >/dev/null; then echo "self-test accepted uncorrelated fatal samples" >&2; return 1; fi
  printf '%s\n' '{"terminal_retained_owner_count":0}' '{"terminal_retained_owner_count":1}' '{"terminal_retained_owner_count":2}' >"$fixture/monotonic.jsons"
  if jq -s '[.[].terminal_retained_owner_count] as $v | any(range(1;$v|length); $v[.] < $v[.-1]) or (($v|max)==($v|min))' "$fixture/monotonic.jsons" | grep -qx true; then echo "self-test accepted monotonic retention growth" >&2; return 1; fi
  echo "linux terminal convergence runner self-test passed"
}

if ((SELF_TEST)); then self_test; exit 0; fi
case "$SCENARIO" in incident|baseline|peak|spike|stress|soak) ;; *) usage; exit 2 ;; esac
for command in curl jq openssl python3 sha256sum ss; do
  command -v "$command" >/dev/null || { echo "required command missing: $command" >&2; exit 69; }
done
[[ -x "$BINARY" ]] || { echo "proxy binary missing or not executable: $BINARY" >&2; exit 66; }
[[ -r "$CLIENT_CONFIG" ]] || { echo "client config not readable: $CLIENT_CONFIG" >&2; exit 66; }
[[ -r "$SERVER_CONFIG" ]] || { echo "server config not readable: $SERVER_CONFIG" >&2; exit 66; }
if [[ "$SCENARIO" != incident ]] && ! command -v k6 >/dev/null; then
  echo "k6 is required for performance scenarios; install: https://grafana.com/docs/k6/latest/set-up/install-k6/" >&2
  exit 69
fi
if [[ "$SCENARIO" != incident && "$SCENARIO" != baseline ]] && [[ ! -r "$BASELINE_SUMMARY" ]]; then
  echo "non-baseline performance scenarios require --baseline-summary PATH with an accepted historical baseline" >&2
  exit 66
fi

mkdir -p "$OUT" "$OUT/logs" "$OUT/metrics" "$OUT/summary" "$OUT/config"
cp "$CLIENT_CONFIG" "$OUT/config/client.json"
cp "$SERVER_CONFIG" "$OUT/config/server.json"
git -C "$ROOT" rev-parse HEAD >"$OUT/git-sha.txt"
sha256sum "$BINARY" >"$OUT/binary-sha256.txt"
uname -a >"$OUT/uname.txt"
ss -tanp >"$OUT/ss-before.txt"

read -r TARGET_PORT SERVER_ADMIN_PORT CLIENT_ADMIN_PORT <<EOF
$(python3 - <<'PY'
import socket
ports=[]
for _ in range(3):
    s=socket.socket(); s.bind(('127.0.0.1',0)); ports.append(s.getsockname()[1]); s.close()
print(*ports)
PY
)
EOF
CLIENT_HTTP=$(jq -r '.peers[]? | select(.enabled != false) | .http_listen | select(length > 0)' "$CLIENT_CONFIG" | head -n1)
CLIENT_HTTP=${CLIENT_HTTP:-127.0.0.1:8080}
TOKEN=$(python3 - <<'PY'
import secrets
print(secrets.token_hex(32))
PY
)
printf '{"version":1,"token_type":"Bearer","token":"%s","listen":"runner"}\n' "$TOKEN" >"$OUT/admin-token.json"
chmod 600 "$OUT/admin-token.json"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/target.key" -out "$OUT/target.crt" \
  -days 1 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
  >"$OUT/logs/target-cert.log" 2>&1

PIDS=()
cleanup() {
  local rc=$? pid
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  ss -tanp >"$OUT/ss-after.txt" 2>/dev/null || true
  return "$rc"
}
on_signal() { local sig=$1; trap - EXIT INT TERM; cleanup || true; exit "$sig"; }
trap cleanup EXIT
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

touch "$OUT/target-hits.log"
python3 - "$TARGET_PORT" "$OUT/target.crt" "$OUT/target.key" "$OUT/target-hits.log" >"$OUT/logs/target.log" 2>&1 <<'PY' &
import http.server, socket, socketserver, ssl, struct, sys
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def do_POST(self):
        length=int(self.headers.get('Content-Length','0')); remaining=length
        while remaining:
            chunk=self.rfile.read(min(65536,remaining))
            if not chunk: break
            remaining-=len(chunk)
        kind='rst' if self.path.endswith('/rst') else 'fin'
        with open(sys.argv[4],'a',encoding='ascii') as hits: hits.write(kind+'\n'); hits.flush()
        if kind == 'rst':
            self.connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
            self.close_connection=True
            return
        body=b'reverse-flow-ok'
        self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body); self.wfile.flush()
    def log_message(self, fmt, *args): print(fmt%args, flush=True)
class Server(socketserver.ThreadingMixIn,http.server.HTTPServer): daemon_threads=True
server=Server(('127.0.0.1',int(sys.argv[1])),Handler)
context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(sys.argv[2],sys.argv[3])
server.socket=context.wrap_socket(server.socket,server_side=True)
server.serve_forever()
PY
PIDS+=("$!")

target_ready=0
for _ in $(seq 1 100); do
  if python3 - "$TARGET_PORT" <<'PY' >/dev/null 2>&1
import socket, sys
s=socket.create_connection(('127.0.0.1',int(sys.argv[1])),timeout=.2); s.close()
PY
  then target_ready=1; break; fi
  sleep .1
done
((target_ready)) || { echo "loopback TLS target failed to start" >&2; exit 70; }

"$BINARY" server --config "$SERVER_CONFIG" --admin-listen "127.0.0.1:$SERVER_ADMIN_PORT" \
  --admin-token-file "$OUT/admin-token.json" >"$OUT/logs/server.log" 2>&1 &
PIDS+=("$!")
"$BINARY" client --config "$CLIENT_CONFIG" --admin-listen "127.0.0.1:$CLIENT_ADMIN_PORT" \
  --admin-token-file "$OUT/admin-token.json" >"$OUT/logs/client.log" 2>&1 &
PIDS+=("$!")

admin_get() { curl -fsS --max-time 2 -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$1/api/v1$2"; }
for port in "$SERVER_ADMIN_PORT" "$CLIENT_ADMIN_PORT"; do
  ready=0
  for _ in $(seq 1 100); do if admin_get "$port" /relay/metrics >/dev/null 2>&1; then ready=1; break; fi; sleep .1; done
  ((ready)) || { echo "admin endpoint failed to start on port $port" >&2; exit 70; }
done

# The Admin server can become ready before the client has completed QUIC setup
# and published its HTTP CONNECT listener.  Gate workload start on that listener.
client_host=${CLIENT_HTTP%:*}
client_port=${CLIENT_HTTP##*:}
ready=0
for _ in $(seq 1 100); do
  if python3 - "$client_host" "$client_port" <<'PY' >/dev/null 2>&1
import socket, sys
s=socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=.2)
s.close()
PY
  then ready=1; break; fi
  sleep .1
done
((ready)) || { echo "client HTTP CONNECT listener failed to start at $CLIENT_HTTP" >&2; exit 70; }

snapshot_role() {
  local role=$1 port=$2 tag=$3 tunnels_path
  [[ "$role" == client ]] && tunnels_path=/tunnels || tunnels_path=/server/tunnels
  admin_get "$port" /relay/metrics >"$OUT/metrics/$role-$tag-relay.json"
  admin_get "$port" "$tunnels_path" >"$OUT/metrics/$role-$tag-tunnels.json"
  admin_get "$port" /relay/active-relays >"$OUT/metrics/$role-$tag-active-relays.json"
  admin_get "$port" /relay/terminal-retentions >"$OUT/metrics/$role-$tag-retentions.json"
  jq -n --slurpfile r "$OUT/metrics/$role-$tag-relay.json" \
    --slurpfile c "$OUT/metrics/$role-$tag-tunnels.json" \
    --slurpfile a "$OUT/metrics/$role-$tag-active-relays.json" \
    --slurpfile t "$OUT/metrics/$role-$tag-retentions.json" '
      ($r[0]) as $m |
      if (($c[0].tunnels|type)!="array" or ($a[0].relays|type)!="array" or ($t[0].retentions|type)!="array") then error("invalid admin collection schema") else
      ["active_relays","terminal_sink_pending","terminal_timeout_pending","terminal_exactly_once_violation","terminal_retained_owner_count","linux_relay_outstanding_quic_sends","linux_relay_pending_events","linux_relay_pending_tcp_write_queue","terminal_shutdown_submitted","terminal_observed","terminal_connection_escalation","terminal_watchdog_armed","terminal_watchdog_canceled","terminal_watchdog_timeout"] as $required |
      if ($required|any(.[]; . as $key|$m|has($key)|not)) then error("missing required relay metric") else {
        active_tunnels:($c[0].tunnels|length),
        active_relays:$m.active_relays,
        active_relay_records:($a[0].relays|length),
        terminal_sink_pending:$m.terminal_sink_pending,
        send_completion_pending:$m.linux_relay_outstanding_quic_sends,
        relay_pending_events:$m.linux_relay_pending_events,
        pending_tcp_write_queue:$m.linux_relay_pending_tcp_write_queue,
        retained_owner:$m.terminal_retained_owner_count,
        retention_records:($t[0].retentions|length),
        terminal_timeout_pending:$m.terminal_timeout_pending,
        exactly_once_violation:$m.terminal_exactly_once_violation,
        watchdog_pending:($m.terminal_watchdog_armed-$m.terminal_watchdog_canceled-$m.terminal_watchdog_timeout),
        terminal_shutdown_submitted:$m.terminal_shutdown_submitted,
        terminal_observed:$m.terminal_observed,
        terminal_connection_escalation:$m.terminal_connection_escalation
      } end end' >"$OUT/metrics/$role-$tag.json"
}
snapshot_role client "$CLIENT_ADMIN_PORT" baseline
snapshot_role server "$SERVER_ADMIN_PORT" baseline
stop_sampler=0
sample_metrics() {
  local n=0
  while ((stop_sampler == 0)); do
    admin_get "$CLIENT_ADMIN_PORT" /relay/metrics >"$OUT/metrics/client-sample-$(printf '%04d' "$n").json" 2>/dev/null || true
    admin_get "$SERVER_ADMIN_PORT" /relay/metrics >"$OUT/metrics/server-sample-$(printf '%04d' "$n").json" 2>/dev/null || true
    printf '%s\t%s\n' "$(date +%s)" "$n" >>"$OUT/metrics/sample-index.tsv"
    ps -o pid=,pcpu=,rss=,nlwp= -p "${PIDS[1]},${PIDS[2]}" >>"$OUT/process-samples.txt" 2>/dev/null || true
    n=$((n+1)); sleep 5
  done
}
sample_metrics & SAMPLER_PID=$!; PIDS+=("$SAMPLER_PID")

TARGET_URL="https://127.0.0.1:$TARGET_PORT"
started=$(date +%s%3N)
if [[ "$SCENARIO" == incident ]]; then
  set +e
  curl -kfsS --max-time 10 --noproxy '' --proxy "http://$CLIENT_HTTP" -X POST --data-binary @<(head -c 1024 /dev/zero) "$TARGET_URL/fin" >"$OUT/fin-response.txt" 2>"$OUT/logs/fin-curl.log"
  fin_rc=$?
  set -e
  snapshot_role client "$CLIENT_ADMIN_PORT" pre-rst
  snapshot_role server "$SERVER_ADMIN_PORT" pre-rst
  rst_started=$(date +%s%3N)
  set +e
  curl -ksS --max-time 10 --noproxy '' --proxy "http://$CLIENT_HTTP" -X POST --data-binary @<(head -c 65536 /dev/zero) "$TARGET_URL/rst" >"$OUT/rst-response.txt" 2>"$OUT/logs/rst-curl.log"
  rst_rc=$?
  set -e
  printf '%s\n' "$fin_rc" >"$OUT/fin-curl-rc.txt"
  printf '%s\n' "$rst_rc" >"$OUT/rst-curl-rc.txt"
else
  (cd "$OUT" && SCENARIO="$SCENARIO" TARGET_URL="$TARGET_URL" CLIENT_ADMIN_URL="http://127.0.0.1:$CLIENT_ADMIN_PORT" ADMIN_TOKEN="$TOKEN" HTTP_PROXY="http://$CLIENT_HTTP" \
    HTTPS_PROXY="http://$CLIENT_HTTP" NO_PROXY='' no_proxy='' k6 run --out "json=k6-raw.json" "$WORKLOAD") |& tee "$OUT/logs/k6.log"
fi

deadline=$((SECONDS + 30)); converged=0
while ((SECONDS <= deadline)); do
  snapshot_role client "$CLIENT_ADMIN_PORT" final
  snapshot_role server "$SERVER_ADMIN_PORT" final
  if jq -e --slurpfile b "$OUT/metrics/client-baseline.json" '
      .active_tunnels==$b[0].active_tunnels and .active_relays==$b[0].active_relays and
      .active_relay_records==$b[0].active_relay_records and .terminal_sink_pending==$b[0].terminal_sink_pending and
      .send_completion_pending==$b[0].send_completion_pending and .relay_pending_events==$b[0].relay_pending_events and
      .pending_tcp_write_queue==$b[0].pending_tcp_write_queue and .retained_owner==$b[0].retained_owner and
      .retention_records==$b[0].retention_records and .terminal_timeout_pending==0 and $b[0].terminal_timeout_pending==0 and
      .exactly_once_violation==0 and $b[0].exactly_once_violation==0 and .watchdog_pending==0 and $b[0].watchdog_pending==0' "$OUT/metrics/client-final.json" >/dev/null && \
     jq -e --slurpfile b "$OUT/metrics/server-baseline.json" '
      .active_tunnels==$b[0].active_tunnels and .active_relays==$b[0].active_relays and
      .active_relay_records==$b[0].active_relay_records and .terminal_sink_pending==$b[0].terminal_sink_pending and
      .send_completion_pending==$b[0].send_completion_pending and .relay_pending_events==$b[0].relay_pending_events and
      .pending_tcp_write_queue==$b[0].pending_tcp_write_queue and .retained_owner==$b[0].retained_owner and
      .retention_records==$b[0].retention_records and .terminal_timeout_pending==0 and $b[0].terminal_timeout_pending==0 and
      .exactly_once_violation==0 and $b[0].exactly_once_violation==0 and .watchdog_pending==0 and $b[0].watchdog_pending==0' "$OUT/metrics/server-final.json" >/dev/null; then converged=1; break; fi
  sleep 1
done
elapsed_ms=$(( $(date +%s%3N) - started ))
fatal_latency_ms=$(( $(date +%s%3N) - ${rst_started:-started} ))
ss -tanp >"$OUT/ss-final.txt"
close_wait=$(awk -v p=":$TARGET_PORT" '$1=="CLOSE-WAIT" && index($0,p){n++} END{print n+0}' "$OUT/ss-final.txt")
jq -n --slurpfile b "$OUT/metrics/client-baseline.json" --slurpfile f "$OUT/metrics/client-final.json" \
  '{baseline:$b[0],final:$f[0],delta:($f[0] as $final|$b[0]|with_entries(.value=($final[.key]-.value)))}' >"$OUT/summary/client-resources.json"
jq -n --slurpfile b "$OUT/metrics/server-baseline.json" --slurpfile f "$OUT/metrics/server-final.json" \
  '{baseline:$b[0],final:$f[0],delta:($f[0] as $final|$b[0]|with_entries(.value=($final[.key]-.value)))}' >"$OUT/summary/server-resources.json"
fin_hits=$(grep -c '^fin$' "$OUT/target-hits.log" || true)
rst_hits=$(grep -c '^rst$' "$OUT/target-hits.log" || true)
passed=false
incident_io_ok=true
if [[ "$SCENARIO" == incident ]] && { [[ "${fin_rc:-1}" != 0 ]] || [[ "${rst_rc:-0}" == 0 ]] || \
   [[ "$(cat "$OUT/fin-response.txt")" != reverse-flow-ok ]] || ((fin_hits < 1 || rst_hits < 1)); }; then
  incident_io_ok=false
fi
if [[ "$SCENARIO" == incident ]]; then
  terminal_delta=$(jq -n --slurpfile ci "$OUT/metrics/client-pre-rst.json" --slurpfile cf "$OUT/metrics/client-final.json" \
    --slurpfile si "$OUT/metrics/server-pre-rst.json" --slurpfile sf "$OUT/metrics/server-final.json" \
    '($cf[0].terminal_observed-$ci[0].terminal_observed)+($cf[0].terminal_shutdown_submitted-$ci[0].terminal_shutdown_submitted)+($cf[0].terminal_connection_escalation-$ci[0].terminal_connection_escalation)+($sf[0].terminal_observed-$si[0].terminal_observed)+($sf[0].terminal_shutdown_submitted-$si[0].terminal_shutdown_submitted)+($sf[0].terminal_connection_escalation-$si[0].terminal_connection_escalation)')
else
  terminal_delta=$(jq -s 'map(.delta.terminal_observed + .delta.terminal_shutdown_submitted + .delta.terminal_connection_escalation)|add' \
    "$OUT/summary/client-resources.json" "$OUT/summary/server-resources.json")
fi
performance='null'
performance_ok=true
if [[ "$SCENARIO" != incident ]]; then
  if ! jq -e '.passed==true and (.terminal_convergence.fatal_terminal_latency.values["p(95)"] < 1000) and (.terminal_convergence.fatal_terminal_latency.values["p(99)"] < 5000) and (.terminal_convergence.fatal_terminal_latency.values.count == .terminal_convergence.rst_count.values.count) and (.terminal_convergence.rst_expected.values.count == .terminal_convergence.rst_count.values.count) and (.terminal_convergence.reset_observed.values.rate > 0.999)' "$OUT/summary.json" >/dev/null; then performance_ok=false; fi
  process_stats=$(awk 'NF==4{if($2>cpu)cpu=$2;if($3>rss)rss=$3;if($4>thr)thr=$4}END{printf "{\"cpu_peak\":%.3f,\"rss_peak_kb\":%d,\"thread_peak\":%d}",cpu,rss,thr}' "$OUT/process-samples.txt")
  performance=$(jq -n --argjson ps "$process_stats" --slurpfile k "$OUT/summary.json" '{establish_p95_ms:$k[0].metrics["http_req_duration{workload_normal:true}"].values["p(95)"],establish_p99_ms:$k[0].metrics["http_req_duration{workload_normal:true}"].values["p(99)"],throughput_per_sec:$k[0].metrics.iterations.values.rate,cpu_peak:$ps.cpu_peak,rss_peak_kb:$ps.rss_peak_kb,thread_peak:$ps.thread_peak}')
  if [[ "$SCENARIO" != baseline ]] && ! jq -e --argjson p "$performance" '
      (.performance|type)=="object" and
      $p.establish_p95_ms <= (.performance.establish_p95_ms*1.05) and
      $p.establish_p99_ms <= (.performance.establish_p99_ms*1.10) and
      $p.throughput_per_sec >= (.performance.throughput_per_sec*0.95) and
      $p.cpu_peak <= (.performance.cpu_peak*1.10) and $p.rss_peak_kb <= (.performance.rss_peak_kb*1.10) and
      $p.thread_peak <= .performance.thread_peak' "$BASELINE_SUMMARY" >/dev/null; then performance_ok=false; fi
  if [[ "$SCENARIO" == soak ]]; then
    sample_count=$(wc -l <"$OUT/metrics/sample-index.tsv")
    sample_span=$(awk 'NR==1{first=$1} {last=$1} END{print last-first}' "$OUT/metrics/sample-index.tsv")
    if ((sample_count < 360 || sample_span < 1795 || elapsed_ms < 1800000)); then performance_ok=false; fi
    for role in client server; do
      if ! jq -s '[.[].terminal_retained_owner_count] as $v | (($v|length)>=360) and (any(range(1;$v|length); $v[.] < $v[.-1]) or (($v|max)==($v|min)))' "$OUT"/metrics/$role-sample-*.json | grep -qx true; then performance_ok=false; fi
    done
  fi
fi
if ((converged)) && [[ "$close_wait" == 0 ]] && [[ "$incident_io_ok" == true ]] && [[ "$performance_ok" == true ]]; then
  if [[ "$SCENARIO" != incident ]] || ((fatal_latency_ms <= 10000 && terminal_delta > 0)); then passed=true; fi
fi
jq -n --arg scenario "$SCENARIO" --argjson passed "$passed" --argjson close_wait "$close_wait" \
  --argjson fin_rc "${fin_rc:-0}" --argjson rst_rc "${rst_rc:-0}" \
  --argjson fin_hits "$fin_hits" --argjson rst_hits "$rst_hits" --argjson terminal_delta "$terminal_delta" --argjson performance "$performance" \
  --argjson elapsed_ms "$elapsed_ms" --argjson fatal_latency_ms "$fatal_latency_ms" --slurpfile client "$OUT/summary/client-resources.json" \
  --slurpfile server "$OUT/summary/server-resources.json" \
  '{scenario:$scenario,passed:$passed,close_wait:$close_wait,convergence_ms:$elapsed_ms,fatal_terminal_latency_ms:$fatal_latency_ms,fin_rc:$fin_rc,rst_rc:$rst_rc,target_hits:{fin:$fin_hits,rst:$rst_hits},terminal_metric_delta:$terminal_delta,performance:$performance,client_resources:$client[0],server_resources:$server[0]}' \
  >"$OUT/summary/result.json"
stop_sampler=1
kill "$SAMPLER_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true
unset 'PIDS[-1]'
if [[ "$passed" != true ]]; then echo "terminal convergence release gate failed; see $OUT/summary/result.json" >&2; exit 1; fi
echo "linux terminal convergence $SCENARIO passed; evidence: $OUT"
