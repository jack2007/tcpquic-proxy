#!/usr/bin/env bash
# macOS terminal / zero-FIN ownership release gate.
# Mirrors scripts/run-linux-terminal-convergence.sh, adapted for Darwin metrics.
#
# Usage:
#   SCENARIO=baseline scripts/run-macos-terminal-convergence.sh
#   SCENARIO=soak SOAK_SECONDS=60 scripts/run-macos-terminal-convergence.sh
#   scripts/run-macos-terminal-convergence.sh --self-test
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

SCENARIO=${SCENARIO:-}
SOAK_SECONDS=${SOAK_SECONDS:-60}
CLIENT_CONFIG=${CLIENT_CONFIG:-}
SERVER_CONFIG=${SERVER_CONFIG:-}
OUT=${OUT:-}
SELF_TEST=0

usage() {
  echo "usage: SCENARIO=baseline|soak [SOAK_SECONDS=N] [OUT=DIR] $0 [--self-test]" >&2
  echo "  optional: CLIENT_CONFIG= PATH SERVER_CONFIG= PATH TCPQUIC_BINARY= PATH" >&2
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
  local fixture
  fixture=$(mktemp -d); trap 'rm -rf "$fixture"' RETURN
  printf '%s\n' '{"active_relays":0,"terminal_sink_pending":0,"terminal_timeout_pending":0,"terminal_exactly_once_violation":0,"terminal_retained_owner_count":0,"darwin_relay_receive_completion_pending":0,"darwin_relay_receive_completion_exactly_once_violation":0,"linux_relay_outstanding_quic_sends":0,"linux_relay_pending_events":0,"linux_relay_pending_tcp_write_queue":0}' >"$fixture/good.json"
  jq -e '
    .terminal_retained_owner_count == 0 and
    .terminal_sink_pending == 0 and
    .darwin_relay_receive_completion_pending == 0 and
    .darwin_relay_receive_completion_exactly_once_violation == 0
  ' "$fixture/good.json" >/dev/null
  jq '.darwin_relay_receive_completion_pending=1' "$fixture/good.json" >"$fixture/pending.json"
  if jq -e '
    .terminal_retained_owner_count == 0 and
    .terminal_sink_pending == 0 and
    .darwin_relay_receive_completion_pending == 0 and
    .darwin_relay_receive_completion_exactly_once_violation == 0
  ' "$fixture/pending.json" >/dev/null; then
    echo "self-test accepted pending receive completion" >&2
    return 1
  fi
  jq '.darwin_relay_receive_completion_exactly_once_violation=1' "$fixture/good.json" >"$fixture/violation.json"
  if jq -e '.darwin_relay_receive_completion_exactly_once_violation == 0' "$fixture/violation.json" >/dev/null; then
    echo "self-test accepted exactly-once violation" >&2
    return 1
  fi
  echo "macos terminal convergence runner self-test passed"
}

if ((SELF_TEST)); then self_test; exit 0; fi
case "$SCENARIO" in baseline|soak) ;; *) usage; exit 2 ;; esac

for command in curl jq openssl python3; do
  command -v "$command" >/dev/null || { echo "required command missing: $command" >&2; exit 69; }
done
[[ -x "$BINARY" ]] || { echo "proxy binary missing or not executable: $BINARY" >&2; exit 66; }

if [[ -z "$OUT" ]]; then
  OUT="$ROOT/build/macos-terminal-convergence-$SCENARIO-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUT" "$OUT/logs" "$OUT/metrics" "$OUT/summary" "$OUT/config" "$OUT/certs"
git -C "$ROOT" rev-parse HEAD >"$OUT/git-sha.txt" 2>/dev/null || true
shasum -a 256 "$BINARY" >"$OUT/binary-sha256.txt"
uname -a >"$OUT/uname.txt"

pick_ports() {
  python3 - <<'PY'
import socket
ports=[]
for _ in range(6):
    s=socket.socket(); s.bind(('127.0.0.1',0)); ports.append(s.getsockname()[1]); s.close()
print(*ports)
PY
}
read -r TARGET_PORT QUIC_PORT HTTP_PORT SOCKS_PORT SERVER_ADMIN_PORT CLIENT_ADMIN_PORT <<<"$(pick_ports)"

TOKEN=$(python3 - <<'PY'
import secrets
print(secrets.token_hex(32))
PY
)
printf '{"version":1,"token_type":"Bearer","token":"%s","listen":"runner"}\n' "$TOKEN" >"$OUT/admin-token.json"
chmod 600 "$OUT/admin-token.json"

# Local CA + leaf certs (same shape as scripts/test-tcpquic-proxy.sh).
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$OUT/certs/ca.key" -out "$OUT/certs/ca.crt" \
  -subj "/CN=tcpquic-macos-terminal-ca" -days 1 -sha256 \
  >"$OUT/logs/ca-cert.log" 2>&1
cat >"$OUT/certs/server.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF
openssl req -newkey rsa:2048 -nodes \
  -keyout "$OUT/certs/server.key" -out "$OUT/certs/server.csr" \
  -subj "/CN=localhost" >"$OUT/logs/server-csr.log" 2>&1
openssl x509 -req -in "$OUT/certs/server.csr" -CA "$OUT/certs/ca.crt" -CAkey "$OUT/certs/ca.key" \
  -CAcreateserial -out "$OUT/certs/server.crt" -days 1 -sha256 -extfile "$OUT/certs/server.ext" \
  >"$OUT/logs/server-cert.log" 2>&1

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/target.key" -out "$OUT/target.crt" \
  -days 1 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
  >"$OUT/logs/target-cert.log" 2>&1

if [[ -z "$SERVER_CONFIG" ]]; then
  # Prefer CLI flags for server (matches test-tcpquic-proxy.sh); keep a marker file.
  SERVER_CONFIG="$OUT/config/server.json"
  printf '%s\n' '{"note":"server launched via CLI flags; see server-cmd.txt"}' >"$SERVER_CONFIG"
fi
if [[ -z "$CLIENT_CONFIG" ]]; then
  CLIENT_CONFIG="$OUT/config/client.json"
  cat >"$CLIENT_CONFIG" <<EOF
{"version":1,"client":{"client_name":"macos-terminal-convergence"},"peers":[{"peer_id":"local","quic_peer":"127.0.0.1:${QUIC_PORT}","http_listen":"127.0.0.1:${HTTP_PORT}","socks_listen":"127.0.0.1:${SOCKS_PORT}","compress":"off"}]}
EOF
fi
cp "$CLIENT_CONFIG" "$OUT/config/client.json" 2>/dev/null || true
cp "$SERVER_CONFIG" "$OUT/config/server.json" 2>/dev/null || true

PIDS=()
cleanup() {
  local rc=$? pid
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  return "$rc"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

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
    def do_GET(self):
        body=b'ok'
        self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
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

SERVER_STDIN="$OUT/server.stdin"
mkfifo "$SERVER_STDIN"
exec {SERVER_STDIN_FD}<>"$SERVER_STDIN"
{
  printf '%s\n' "$BINARY" server \
    --listen "127.0.0.1:$QUIC_PORT" \
    --allow-targets "127.0.0.0/8" \
    --cert "$OUT/certs/server.crt" \
    --key "$OUT/certs/server.key" \
    --ca "$OUT/certs/ca.crt" \
    --compress off \
    --admin-listen "127.0.0.1:$SERVER_ADMIN_PORT" \
    --admin-token-file "$OUT/admin-token.json"
} >"$OUT/config/server-cmd.txt"
"$BINARY" server \
  --listen "127.0.0.1:$QUIC_PORT" \
  --allow-targets "127.0.0.0/8" \
  --cert "$OUT/certs/server.crt" \
  --key "$OUT/certs/server.key" \
  --ca "$OUT/certs/ca.crt" \
  --compress off \
  --admin-listen "127.0.0.1:$SERVER_ADMIN_PORT" \
  --admin-token-file "$OUT/admin-token.json" \
  >"$OUT/logs/server.log" 2>&1 <"$SERVER_STDIN" &
PIDS+=("$!")

"$BINARY" client --client-config "$CLIENT_CONFIG" \
  --ca "$OUT/certs/ca.crt" \
  --compress off \
  --admin-listen "127.0.0.1:$CLIENT_ADMIN_PORT" \
  --admin-token-file "$OUT/admin-token.json" \
  >"$OUT/logs/client.log" 2>&1 &
PIDS+=("$!")

admin_get() { curl -fsS --max-time 2 -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$1/api/v1$2"; }
for port in "$SERVER_ADMIN_PORT" "$CLIENT_ADMIN_PORT"; do
  ready=0
  for _ in $(seq 1 100); do if admin_get "$port" /relay/metrics >/dev/null 2>&1; then ready=1; break; fi; sleep .1; done
  ((ready)) || { echo "admin endpoint failed to start on port $port" >&2; exit 70; }
done

CLIENT_HTTP="127.0.0.1:$HTTP_PORT"
ready=0
for _ in $(seq 1 100); do
  if python3 - "$HTTP_PORT" <<'PY' >/dev/null 2>&1
import socket, sys
s=socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=.2)
s.close()
PY
  then ready=1; break; fi
  sleep .1
done
((ready)) || { echo "client HTTP CONNECT listener failed to start at $CLIENT_HTTP" >&2; exit 70; }

snapshot_relay() {
  local role=$1 port=$2 tag=$3
  admin_get "$port" /relay/metrics >"$OUT/metrics/$role-$tag-relay.json"
  cp "$OUT/metrics/$role-$tag-relay.json" "$OUT/metrics/$role-$tag.json"
}

snapshot_relay client "$CLIENT_ADMIN_PORT" baseline
snapshot_relay server "$SERVER_ADMIN_PORT" baseline

TARGET_URL="https://127.0.0.1:$TARGET_PORT"
K6_USED=0
if [[ "$SCENARIO" == soak ]] && command -v k6 >/dev/null 2>&1 && [[ -r "$WORKLOAD" ]]; then
  K6_USED=1
  (cd "$OUT" && SCENARIO=soak SOAK_SECONDS="$SOAK_SECONDS" TARGET_URL="$TARGET_URL" \
    CLIENT_ADMIN_URL="http://127.0.0.1:$CLIENT_ADMIN_PORT" ADMIN_TOKEN="$TOKEN" \
    HTTP_PROXY="http://$CLIENT_HTTP" HTTPS_PROXY="http://$CLIENT_HTTP" NO_PROXY='' no_proxy='' \
    k6 run --out "json=k6-raw.json" "$WORKLOAD") |& tee "$OUT/logs/k6.log"
elif [[ "$SCENARIO" == soak ]]; then
  echo "k6 unavailable; running curl soak for ${SOAK_SECONDS}s" | tee "$OUT/logs/soak-mode.txt"
  end=$((SECONDS + SOAK_SECONDS))
  n=0
  while ((SECONDS < end)); do
    curl -kfsS --max-time 5 --noproxy '' --proxy "http://$CLIENT_HTTP" \
      -X POST --data-binary @<(head -c 1024 /dev/zero) "$TARGET_URL/fin" \
      >"$OUT/fin-response-$n.txt" 2>>"$OUT/logs/fin-curl.log" || true
    n=$((n + 1))
    sleep 0.2
  done
  printf '%s\n' "$n" >"$OUT/soak-iterations.txt"
else
  # baseline: controlled FIN fixture through HTTP CONNECT
  curl -kfsS --max-time 10 --noproxy '' --proxy "http://$CLIENT_HTTP" \
    -X POST --data-binary @<(head -c 1024 /dev/zero) "$TARGET_URL/fin" \
    >"$OUT/fin-response.txt" 2>"$OUT/logs/fin-curl.log"
  grep -q 'reverse-flow-ok' "$OUT/fin-response.txt" || {
    echo "FIN fixture response missing reverse-flow-ok" >&2
    exit 1
  }
fi

deadline=$((SECONDS + 30))
converged=0
while ((SECONDS <= deadline)); do
  snapshot_relay client "$CLIENT_ADMIN_PORT" after-relay
  snapshot_relay server "$SERVER_ADMIN_PORT" after-relay
  cp "$OUT/metrics/client-after-relay-relay.json" "$OUT/metrics/client-after-relay.json"
  if jq -e '
    .terminal_retained_owner_count == 0 and
    .terminal_sink_pending == 0 and
    .darwin_relay_receive_completion_pending == 0 and
    .darwin_relay_receive_completion_exactly_once_violation == 0
  ' "$OUT/metrics/client-after-relay.json" >/dev/null && \
     jq -e '
    .terminal_retained_owner_count == 0 and
    .terminal_sink_pending == 0 and
    .darwin_relay_receive_completion_pending == 0 and
    .darwin_relay_receive_completion_exactly_once_violation == 0
  ' "$OUT/metrics/server-after-relay-relay.json" >/dev/null; then
    converged=1
    break
  fi
  sleep 1
done

jq -n \
  --slurpfile c "$OUT/metrics/client-after-relay.json" \
  --slurpfile s "$OUT/metrics/server-after-relay-relay.json" \
  --arg scenario "$SCENARIO" \
  --argjson soak_seconds "$SOAK_SECONDS" \
  --argjson k6_used "$K6_USED" \
  --argjson converged "$converged" '
  {
    scenario:$scenario,
    soak_seconds:$soak_seconds,
    k6_used:($k6_used==1),
    converged:($converged==1),
    client:$c[0],
    server:$s[0],
    gate:{
      terminal_retained_owner_count:$c[0].terminal_retained_owner_count,
      terminal_sink_pending:$c[0].terminal_sink_pending,
      darwin_relay_receive_completion_pending:$c[0].darwin_relay_receive_completion_pending,
      darwin_relay_receive_completion_exactly_once_violation:$c[0].darwin_relay_receive_completion_exactly_once_violation
    }
  }' >"$OUT/summary/gate.json"

if ((!converged)); then
  echo "macos terminal convergence FAILED: drain gate not met (see $OUT/summary/gate.json)" >&2
  cat "$OUT/summary/gate.json" >&2 || true
  exit 1
fi

echo "macos terminal convergence $SCENARIO PASSED (out=$OUT k6=$K6_USED)"
