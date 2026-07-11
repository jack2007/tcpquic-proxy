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
SELF_TEST=0

usage() {
  echo "usage: $0 --self-test | --scenario incident|baseline|peak|spike|stress|soak --client-config PATH --server-config PATH --out DIR" >&2
}

while (($#)); do
  case "$1" in
    --self-test) SELF_TEST=1; shift ;;
    --scenario) SCENARIO=${2:?missing scenario}; shift 2 ;;
    --client-config) CLIENT_CONFIG=${2:?missing client config}; shift 2 ;;
    --server-config) SERVER_CONFIG=${2:?missing server config}; shift 2 ;;
    --out) OUT=${2:?missing output directory}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

self_test() {
  bash -n "$0"
  test -r "$WORKLOAD"
  grep -Fq "checks: ['rate>0.999']" "$WORKLOAD"
  grep -Fq "http_req_failed: ['rate<0.001']" "$WORKLOAD"
  grep -Fq "dropped_iterations: ['count==0']" "$WORKLOAD"
  grep -Fq "http_req_duration: ['p(95)<500', 'p(99)<1000']" "$WORKLOAD"
  grep -Fq "fatal_terminal_latency" "$WORKLOAD"
  grep -Fq "rst_count" "$WORKLOAD"
  grep -Fq "fin_reverse_flow_checks" "$WORKLOAD"
  for name in baseline peak spike stress soak; do grep -Eq "^[[:space:]]*$name:" "$WORKLOAD"; done
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
  local rc=$?
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  ss -tanp >"$OUT/ss-after.txt" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

python3 - "$TARGET_PORT" "$OUT/target.crt" "$OUT/target.key" >"$OUT/logs/target.log" 2>&1 <<'PY' &
import http.server, socket, socketserver, ssl, struct, sys
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def do_POST(self):
        length=int(self.headers.get('Content-Length','0')); remaining=length
        while remaining:
            chunk=self.rfile.read(min(65536,remaining))
            if not chunk: break
            remaining-=len(chunk)
        body=b'reverse-flow-ok'
        self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body); self.wfile.flush()
        if self.path.endswith('/rst'):
            self.connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
            self.close_connection=True
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

admin_get() { curl -fsS --max-time 2 -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$1/api/v1/relay/metrics"; }
for port in "$SERVER_ADMIN_PORT" "$CLIENT_ADMIN_PORT"; do
  ready=0
  for _ in $(seq 1 100); do if admin_get "$port" >/dev/null 2>&1; then ready=1; break; fi; sleep .1; done
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

admin_get "$CLIENT_ADMIN_PORT" >"$OUT/metrics/client-baseline.json"
admin_get "$SERVER_ADMIN_PORT" >"$OUT/metrics/server-baseline.json"
stop_sampler=0
sample_metrics() {
  local n=0
  while ((stop_sampler == 0)); do
    admin_get "$CLIENT_ADMIN_PORT" >"$OUT/metrics/client-$(printf '%04d' "$n").json" 2>/dev/null || true
    admin_get "$SERVER_ADMIN_PORT" >"$OUT/metrics/server-$(printf '%04d' "$n").json" 2>/dev/null || true
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
  curl -ksS --max-time 10 --noproxy '' --proxy "http://$CLIENT_HTTP" -X POST --data-binary @<(head -c 65536 /dev/zero) "$TARGET_URL/rst" >"$OUT/rst-response.txt" 2>"$OUT/logs/rst-curl.log"
  rst_rc=$?
  set -e
  printf '%s\n' "$fin_rc" >"$OUT/fin-curl-rc.txt"
  printf '%s\n' "$rst_rc" >"$OUT/rst-curl-rc.txt"
else
  (cd "$OUT" && SCENARIO="$SCENARIO" TARGET_URL="$TARGET_URL" HTTP_PROXY="http://$CLIENT_HTTP" \
    HTTPS_PROXY="http://$CLIENT_HTTP" NO_PROXY='' no_proxy='' k6 run --out "json=k6-raw.json" "$WORKLOAD") |& tee "$OUT/logs/k6.log"
fi

resource_filter='{
 active_tunnels:(.active_tunnels // .tunnels_active // 0),
 active_relays:(.active_relays // .linux_relay_active // 0),
 terminal_sink_pending:(.terminal_sink_pending // 0),
 terminal_watchdog_pending:(.terminal_watchdog_pending // 0),
 send_completion_pending:(.send_completion_pending // .send_registry_pending // 0),
 retained_owner:(.terminal_retained_owner_count // 0),
 terminal_timeout_pending:(.terminal_timeout_pending // 0),
 exactly_once_violation:(.terminal_exactly_once_violation // 0)}'
deadline=$((SECONDS + 30)); converged=0
while ((SECONDS <= deadline)); do
  admin_get "$CLIENT_ADMIN_PORT" >"$OUT/metrics/client-final.json"
  admin_get "$SERVER_ADMIN_PORT" >"$OUT/metrics/server-final.json"
  if jq -e "$resource_filter | to_entries | all(.value == 0)" "$OUT/metrics/client-final.json" >/dev/null && \
     jq -e "$resource_filter | to_entries | all(.value == 0)" "$OUT/metrics/server-final.json" >/dev/null; then converged=1; break; fi
  sleep 1
done
elapsed_ms=$(( $(date +%s%3N) - started ))
ss -tanp >"$OUT/ss-final.txt"
close_wait=$(awk -v p=":$TARGET_PORT" '$1=="CLOSE-WAIT" && index($0,p){n++} END{print n+0}' "$OUT/ss-final.txt")
jq "$resource_filter" "$OUT/metrics/client-final.json" >"$OUT/summary/client-resources.json"
jq "$resource_filter" "$OUT/metrics/server-final.json" >"$OUT/summary/server-resources.json"
passed=false
incident_io_ok=true
if [[ "$SCENARIO" == incident ]] && { [[ "${fin_rc:-1}" != 0 ]] || [[ "$(cat "$OUT/fin-response.txt")" != reverse-flow-ok ]]; }; then
  incident_io_ok=false
fi
if ((converged)) && [[ "$close_wait" == 0 ]] && [[ "$incident_io_ok" == true ]] && jq -e '.terminal_timeout_pending==0 and .exactly_once_violation==0' \
  "$OUT/summary/client-resources.json" "$OUT/summary/server-resources.json" >/dev/null; then
  if [[ "$SCENARIO" != incident || "$elapsed_ms" -le 10000 ]]; then passed=true; fi
fi
jq -n --arg scenario "$SCENARIO" --argjson passed "$passed" --argjson close_wait "$close_wait" \
  --argjson fin_rc "${fin_rc:-0}" --argjson rst_rc "${rst_rc:-0}" \
  --argjson elapsed_ms "$elapsed_ms" --slurpfile client "$OUT/summary/client-resources.json" \
  --slurpfile server "$OUT/summary/server-resources.json" \
  '{scenario:$scenario,passed:$passed,close_wait:$close_wait,convergence_ms:$elapsed_ms,fin_rc:$fin_rc,rst_rc:$rst_rc,client_resources:$client[0],server_resources:$server[0]}' \
  >"$OUT/summary/result.json"
stop_sampler=1
kill "$SAMPLER_PID" 2>/dev/null || true
wait "$SAMPLER_PID" 2>/dev/null || true
if [[ "$passed" != true ]]; then echo "terminal convergence release gate failed; see $OUT/summary/result.json" >&2; exit 1; fi
echo "linux terminal convergence $SCENARIO passed; evidence: $OUT"
