#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TS="${TS:-$(date +%Y%m%d-%H%M%S)}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/docs/test/dgx-multi-interface-hot-update-$TS}"
REMOTE_SSH="${REMOTE_SSH:-jack@172.16.10.81}"
LOCAL_IP_A="${LOCAL_IP_A:-10.201.1.1}"
LOCAL_IP_B="${LOCAL_IP_B:-10.201.2.1}"
REMOTE_IP_A="${REMOTE_IP_A:-10.201.1.2}"
REMOTE_IP_B="${REMOTE_IP_B:-10.201.2.2}"
QUIC_PORT="${QUIC_PORT:-4433}"
HTTP_PORT="${HTTP_PORT:-18080}"
SOCKS_PORT="${SOCKS_PORT:-19080}"
FORWARD_PORT="${FORWARD_PORT:-15445}"
IPERF_PORT="${IPERF_PORT:-16001}"
CLIENT_ADMIN="${CLIENT_ADMIN:-127.0.0.1:18081}"
SERVER_ADMIN="${SERVER_ADMIN:-127.0.0.1:18081}"
LOCAL_BIN="${LOCAL_BIN:-$ROOT/build/bin/Release/raypx2}"
REMOTE_DIR="${REMOTE_DIR:-/home/jack/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-$REMOTE_DIR/raypx2-f10-$TS}"
SERVER_LOG_REMOTE="/home/jack/dgx-f10-server-$TS.log"
SERVER_PID_REMOTE="/home/jack/dgx-f10-server-$TS.pid"
IPERF_PID_REMOTE="/home/jack/dgx-f10-iperf3-$TS.pid"

mkdir -p "$RESULT_ROOT"/{env,proxy,admin,case,net,summary}

log() {
  printf '[f10] %s\n' "$*" | tee -a "$RESULT_ROOT/case/run.log"
}

run_local_capture() {
  local out="$1"
  shift
  {
    printf '$'
    printf ' %q' "$@"
    printf '\n'
    "$@"
  } >"$out" 2>&1
}

remote() {
  ssh -o BatchMode=yes "$REMOTE_SSH" "$@"
}

remote_capture() {
  local out="$1"
  shift
  {
    printf '$ ssh %s %q\n' "$REMOTE_SSH" "$*"
    remote "$*"
  } >"$out" 2>&1
}

cleanup() {
  set +e
  if [[ -f "$RESULT_ROOT/proxy/local-client.pid" ]]; then
    local pid
    pid="$(cat "$RESULT_ROOT/proxy/local-client.pid")"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  remote "if [ -f '$SERVER_PID_REMOTE' ]; then kill \$(cat '$SERVER_PID_REMOTE') 2>/dev/null || true; rm -f '$SERVER_PID_REMOTE'; fi; if [ -f '$IPERF_PID_REMOTE' ]; then kill \$(cat '$IPERF_PID_REMOTE') 2>/dev/null || true; rm -f '$IPERF_PID_REMOTE'; fi"
}
trap cleanup EXIT

connected_count() {
  jq '[.. | objects | select(.state? == "connected")] | length' "$1"
}

log "result root: $RESULT_ROOT"
if [[ ! -x "$LOCAL_BIN" ]]; then
  log "missing local binary: $LOCAL_BIN"
  exit 2
fi

log "recording environment"
git rev-parse HEAD >"$RESULT_ROOT/env/git-head.txt"
git status --short >"$RESULT_ROOT/env/git-status-short.txt"
run_local_capture "$RESULT_ROOT/env/local-ip-addr.txt" ip -br addr
run_local_capture "$RESULT_ROOT/env/local-route-a.txt" ip route get "$REMOTE_IP_A" from "$LOCAL_IP_A"
run_local_capture "$RESULT_ROOT/env/local-route-b.txt" ip route get "$REMOTE_IP_B" from "$LOCAL_IP_B"
remote_capture "$RESULT_ROOT/env/remote-ip-addr.txt" "ip -br addr"
remote_capture "$RESULT_ROOT/env/remote-route-a.txt" "ip route get '$LOCAL_IP_A' from '$REMOTE_IP_A'"
remote_capture "$RESULT_ROOT/env/remote-route-b.txt" "ip route get '$LOCAL_IP_B' from '$REMOTE_IP_B'"
run_local_capture "$RESULT_ROOT/net/local-ss-before.txt" ss -ltnup
remote_capture "$RESULT_ROOT/net/remote-ss-before.txt" "ss -ltnup"

log "deploying remote test binary without overwriting default binary"
scp -q "$LOCAL_BIN" "$REMOTE_SSH:$REMOTE_BIN"
remote "chmod +x '$REMOTE_BIN'"
sha256sum "$LOCAL_BIN" >"$RESULT_ROOT/env/local-raypx2.sha256"
remote "sha256sum '$REMOTE_BIN'" >"$RESULT_ROOT/env/remote-raypx2.sha256"

cat >"$RESULT_ROOT/proxy/client-path-a.json" <<JSON
{
  "version": 1,
  "tls": {"cert": "cert/client/client.crt", "key": "cert/client/client.key", "ca": "cert/ca.crt"},
  "admin": {"listen": "$CLIENT_ADMIN"},
  "proto": {"disable_1rtt_encryption": true, "keepalive_ms": 5000},
  "compression": {"mode": "off"},
  "tuning": {"mode": "wan"},
  "peers": [
    {
      "peer_id": "dgx-f10",
      "http_listen": "127.0.0.1:$HTTP_PORT",
      "socks_listen": "127.0.0.1:$SOCKS_PORT",
      "quic_peer": "$REMOTE_IP_A:$QUIC_PORT,$REMOTE_IP_B:$QUIC_PORT",
      "quic_connections": 8,
      "port_forwards": [
        {"listen": "127.0.0.1:$FORWARD_PORT", "target": "$REMOTE_IP_A:$IPERF_PORT"}
      ],
      "compress": "off",
      "enabled": true,
      "paths": [
        {"name": "path-a", "local": "$LOCAL_IP_A", "peer": "$REMOTE_IP_A:$QUIC_PORT", "connections": 4}
      ]
    }
  ]
}
JSON

cat >"$RESULT_ROOT/case/patch-paths-ab.json" <<JSON
{
  "paths": [
    {"name": "path-a", "local": "$LOCAL_IP_A", "peer": "$REMOTE_IP_A:$QUIC_PORT", "connections": 4},
    {"name": "path-b", "local": "$LOCAL_IP_B", "peer": "$REMOTE_IP_B:$QUIC_PORT", "connections": 4}
  ]
}
JSON

cat >"$RESULT_ROOT/case/patch-paths-clear.json" <<JSON
{"paths": []}
JSON

log "starting remote iperf3 server"
remote "rm -f '$IPERF_PID_REMOTE'; nohup iperf3 -s -B '$REMOTE_IP_A' -p '$IPERF_PORT' --one-off >/home/jack/dgx-f10-iperf3-$TS.log 2>&1 & echo \$! > '$IPERF_PID_REMOTE'"
sleep 1

log "starting remote QUIC server on explicit dual listen"
remote "rm -f '$SERVER_LOG_REMOTE' '$SERVER_PID_REMOTE'; LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server --listen '$REMOTE_IP_A:$QUIC_PORT,$REMOTE_IP_B:$QUIC_PORT' --allow-targets '$REMOTE_IP_A/32,$REMOTE_IP_B/32,127.0.0.0/8' --cert /home/jack/tcpquic-dgx-certs/10net/server-10net.crt --key /home/jack/tcpquic-dgx-certs/10net/server-10net.key --ca /home/jack/tcpquic-dgx-certs/ca.crt --compress off --tuning wan --admin-listen '$SERVER_ADMIN' --diag-stats --diag-stats-interval 1 </dev/null >'$SERVER_LOG_REMOTE' 2>&1 & echo \$! > '$SERVER_PID_REMOTE'"
sleep 3
remote "test -s '$SERVER_PID_REMOTE' && kill -0 \$(cat '$SERVER_PID_REMOTE')"
remote "grep -m1 'admin token file' '$SERVER_LOG_REMOTE' | awk '{print \$NF}'" >"$RESULT_ROOT/proxy/remote-admin-token-file.txt" || true
remote "ss -ltnup; ss -lunp" >"$RESULT_ROOT/net/remote-ss-server-started.txt" 2>&1 || true

log "starting local client with path-a only"
"$LOCAL_BIN" client --client-config "$RESULT_ROOT/proxy/client-path-a.json" --ca "$ROOT/cert/ca.crt" --admin-listen "$CLIENT_ADMIN" --diag-stats --diag-stats-interval 1 >"$RESULT_ROOT/proxy/local-client.stderr.log" 2>&1 &
CLIENT_PID=$!
echo "$CLIENT_PID" >"$RESULT_ROOT/proxy/local-client.pid"
sleep 4
kill -0 "$CLIENT_PID"
grep -m1 'admin token file' "$RESULT_ROOT/proxy/local-client.stderr.log" | awk '{print $NF}' >"$RESULT_ROOT/proxy/local-admin-token-file.txt"

TOKEN_FILE="$(cat "$RESULT_ROOT/proxy/local-admin-token-file.txt")"
AUTH_HEADER="$(python3 - "$TOKEN_FILE" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    d = json.load(f)
print(f"Authorization: {d.get('token_type', 'Bearer')} {d['token']}")
PY
)"
ADMIN="http://$CLIENT_ADMIN/api/v1"
JSON_H="Content-Type: application/json"

admin_get() {
  local path="$1"
  local out="$2"
  timeout 5s curl -sS -H "$AUTH_HEADER" "$ADMIN$path" >"$out"
}

admin_patch() {
  local path="$1"
  local body="$2"
  local out="$3"
  timeout 5s curl -sS -X PATCH -H "$AUTH_HEADER" -H "$JSON_H" -d "@$body" "$ADMIN$path" >"$out"
}

log "waiting for initial path-a connected=4"
for i in $(seq 1 20); do
  admin_get "/peers/dgx-f10/connections" "$RESULT_ROOT/admin/initial-connections-$i.json" || true
  c="$(connected_count "$RESULT_ROOT/admin/initial-connections-$i.json" 2>/dev/null || echo 0)"
  if [[ "$c" -eq 4 ]]; then
    cp "$RESULT_ROOT/admin/initial-connections-$i.json" "$RESULT_ROOT/admin/initial-connections.json"
    break
  fi
  sleep 1
done
INITIAL_CONNECTED="$(connected_count "$RESULT_ROOT/admin/initial-connections.json" 2>/dev/null || echo 0)"
log "initial connected count: $INITIAL_CONNECTED"
if [[ "$INITIAL_CONNECTED" -ne 4 ]]; then
  log "initial connected count did not reach 4"
  exit 10
fi
admin_get "/peers" "$RESULT_ROOT/admin/initial-peers.json"
admin_get "/config" "$RESULT_ROOT/admin/initial-config.json"

log "PATCH paths from path-a to path-a+path-b"
admin_patch "/peers/dgx-f10" "$RESULT_ROOT/case/patch-paths-ab.json" "$RESULT_ROOT/admin/patch-ab-response.json"
cat "$RESULT_ROOT/admin/patch-ab-response.json" >>"$RESULT_ROOT/case/run.log"
printf '\n' >>"$RESULT_ROOT/case/run.log"

RECOVER_CONNECTED=0
for sec in 10 20 30; do
  sleep 10
  admin_get "/peers/dgx-f10/connections" "$RESULT_ROOT/admin/after-patch-${sec}s-connections.json" || true
  admin_get "/peers/dgx-f10" "$RESULT_ROOT/admin/after-patch-${sec}s-peer.json" || true
  c="$(connected_count "$RESULT_ROOT/admin/after-patch-${sec}s-connections.json" 2>/dev/null || echo 0)"
  log "after patch ${sec}s connected count: $c"
  if [[ "$c" -ge 8 ]]; then
    RECOVER_CONNECTED="$c"
    cp "$RESULT_ROOT/admin/after-patch-${sec}s-connections.json" "$RESULT_ROOT/admin/after-patch-connections.json"
    break
  fi
done
if [[ "$RECOVER_CONNECTED" -lt 8 ]]; then
  log "patched connected count did not reach 8"
  exit 11
fi

log "running iperf3 probe through local port forward"
set +e
timeout 45s iperf3 -c 127.0.0.1 -p "$FORWARD_PORT" -P 8 -t 8 -J >"$RESULT_ROOT/case/iperf.stdout.json" 2>"$RESULT_ROOT/case/iperf.stderr.txt"
IPERF_RC=$?
set -e
echo "$IPERF_RC" >"$RESULT_ROOT/case/iperf.rc"
log "iperf rc: $IPERF_RC"
if [[ "$IPERF_RC" -ne 0 ]]; then
  exit 12
fi

log "PATCH paths clear for rollback behavior"
admin_patch "/peers/dgx-f10" "$RESULT_ROOT/case/patch-paths-clear.json" "$RESULT_ROOT/admin/patch-clear-response.json"
if jq -e '.error? != null' "$RESULT_ROOT/admin/patch-clear-response.json" >/dev/null; then
  log "paths clear PATCH returned error"
  cat "$RESULT_ROOT/admin/patch-clear-response.json" >>"$RESULT_ROOT/case/run.log"
  printf '\n' >>"$RESULT_ROOT/case/run.log"
  exit 13
fi
sleep 5
admin_get "/peers/dgx-f10/connections" "$RESULT_ROOT/admin/after-clear-connections.json" || true
admin_get "/peers/dgx-f10/config" "$RESULT_ROOT/admin/after-clear-config.json" || true
admin_get "/peers" "$RESULT_ROOT/admin/final-peers.json" || true
AFTER_CLEAR_PATHS="$(jq '.paths | length' "$RESULT_ROOT/admin/after-clear-config.json")"
if [[ "$AFTER_CLEAR_PATHS" -ne 0 ]]; then
  log "paths clear PATCH did not clear paths"
  exit 14
fi

run_local_capture "$RESULT_ROOT/net/local-ss-after.txt" ss -ltnup
remote_capture "$RESULT_ROOT/net/remote-ss-after.txt" "ss -ltnup; ss -lunp"
cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$RESULT_ROOT/proxy/local-client.stderr.final.log"
remote "cat '$SERVER_LOG_REMOTE'" >"$RESULT_ROOT/proxy/remote-server.stderr.log" 2>&1 || true
remote "cat /home/jack/dgx-f10-iperf3-$TS.log" >"$RESULT_ROOT/proxy/remote-iperf3.log" 2>&1 || true

jq -r 'def objs: .. | objects; [objs | select(.state? == "connected") | {id, path, local, peer, state, active_tunnels, total_tunnels}]' "$RESULT_ROOT/admin/after-patch-connections.json" >"$RESULT_ROOT/summary/connected-after-patch.json"
jq -r '{sum_sent: .end.sum_sent.bits_per_second, sum_received: .end.sum_received.bits_per_second}' "$RESULT_ROOT/case/iperf.stdout.json" >"$RESULT_ROOT/summary/iperf-summary.json"
cat >"$RESULT_ROOT/summary/summary.md" <<EOF
# DGX F10 paths PATCH hot update result

- result_root: $RESULT_ROOT
- initial_connected: $INITIAL_CONNECTED
- after_patch_connected: $RECOVER_CONNECTED
- iperf_rc: $IPERF_RC
- remote_bin: $REMOTE_BIN
EOF

log "F10 completed: $RESULT_ROOT"
