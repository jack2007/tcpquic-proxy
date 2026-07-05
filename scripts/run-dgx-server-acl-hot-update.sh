#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TS="${TS:-$(date +%Y%m%d-%H%M%S)}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/docs/test/dgx-server-acl-hot-update-$TS}"
REMOTE_SSH="${REMOTE_SSH:-jack@172.16.10.81}"
LOCAL_IP_A="${LOCAL_IP_A:-10.201.1.1}"
LOCAL_IP_B="${LOCAL_IP_B:-10.201.2.1}"
REMOTE_IP_A="${REMOTE_IP_A:-10.201.1.2}"
REMOTE_IP_B="${REMOTE_IP_B:-10.201.2.2}"
QUIC_PORT="${QUIC_PORT:-4433}"
FORWARD_PORT="${FORWARD_PORT:-15446}"
IPERF_PORT="${IPERF_PORT:-16001}"
CLIENT_ADMIN="${CLIENT_ADMIN:-127.0.0.1:18082}"
SERVER_ADMIN="${SERVER_ADMIN:-127.0.0.1:18081}"
LOCAL_BIN="${LOCAL_BIN:-$ROOT/build/bin/Release/raypx2}"
REMOTE_DIR="${REMOTE_DIR:-/home/jack/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-$REMOTE_DIR/raypx2-server-acl-$TS}"
SERVER_CONFIG_REMOTE="/home/jack/dgx-server-acl-$TS-server.json"
SERVER_LOG_REMOTE="/home/jack/dgx-server-acl-$TS-server.log"
SERVER_PID_REMOTE="/home/jack/dgx-server-acl-$TS-server.pid"
IPERF_PID_REMOTE="/home/jack/dgx-server-acl-$TS-iperf3.pid"

mkdir -p "$RESULT_ROOT"/{env,proxy,admin,case,net,summary}

log() {
  printf '[server-acl] %s\n' "$*" | tee -a "$RESULT_ROOT/case/run.log"
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
  remote "if [ -f '$SERVER_PID_REMOTE' ]; then kill \$(cat '$SERVER_PID_REMOTE') 2>/dev/null || true; rm -f '$SERVER_PID_REMOTE'; fi; if [ -f '$IPERF_PID_REMOTE' ]; then kill \$(cat '$IPERF_PID_REMOTE') 2>/dev/null || true; rm -f '$IPERF_PID_REMOTE'; fi" >/dev/null 2>&1 || true
}
trap cleanup EXIT

split_status() {
  local tmp="$1"
  local body="$2"
  local status="$3"
  tail -n 1 "$tmp" >"$status"
  sed '$d' "$tmp" >"$body"
}

local_admin_get() {
  local path="$1"
  local body="$2"
  local status="$3"
  local tmp="$body.tmp"
  timeout 5s curl -sS -H "$CLIENT_AUTH_HEADER" -w '\n%{http_code}\n' "http://$CLIENT_ADMIN/api/v1$path" >"$tmp"
  split_status "$tmp" "$body" "$status"
  rm -f "$tmp"
}

remote_admin_get() {
  local path="$1"
  local body="$2"
  local status="$3"
  local tmp="$body.tmp"
  ssh -o BatchMode=yes "$REMOTE_SSH" "timeout 5s curl -sS -H '$SERVER_AUTH_HEADER' -w '\n%{http_code}\n' 'http://$SERVER_ADMIN/api/v1$path'" >"$tmp"
  split_status "$tmp" "$body" "$status"
  rm -f "$tmp"
}

remote_admin_patch() {
  local path="$1"
  local request_body="$2"
  local response_body="$3"
  local status="$4"
  local tmp="$response_body.tmp"
  ssh -o BatchMode=yes "$REMOTE_SSH" "timeout 5s curl -sS -X PATCH -H '$SERVER_AUTH_HEADER' -H 'Content-Type: application/json' -d @- -w '\n%{http_code}\n' 'http://$SERVER_ADMIN/api/v1$path'" <"$request_body" >"$tmp"
  split_status "$tmp" "$response_body" "$status"
  rm -f "$tmp"
}

json_value() {
  local file="$1"
  local expr="$2"
  jq -r "$expr" "$file"
}

connected_count() {
  jq '[.. | objects | select(.state? == "connected")] | length' "$1"
}

metric_acl_denied() {
  jq -r '.acl_denied // 0' "$1"
}

wait_client_connected() {
  local expected="$1"
  local prefix="$2"
  local connected=0
  for i in $(seq 1 20); do
    local body="$RESULT_ROOT/admin/${prefix}-${i}.json"
    local status="$RESULT_ROOT/admin/${prefix}-${i}.status"
    local_admin_get "/peers/dgx-server-acl/connections" "$body" "$status" || true
    connected="$(connected_count "$body" 2>/dev/null || echo 0)"
    if [[ "$connected" -ge "$expected" ]]; then
      cp "$body" "$RESULT_ROOT/admin/${prefix}.json"
      echo "$connected"
      return 0
    fi
    sleep 1
  done
  echo "$connected"
  return 1
}

run_iperf_probe() {
  local name="$1"
  local timeout_sec="$2"
  set +e
  timeout "${timeout_sec}s" iperf3 -c 127.0.0.1 -p "$FORWARD_PORT" -t 2 -J >"$RESULT_ROOT/case/${name}.stdout.json" 2>"$RESULT_ROOT/case/${name}.stderr.txt"
  local rc=$?
  set -e
  echo "$rc" >"$RESULT_ROOT/case/${name}.rc"
  log "$name rc: $rc"
  return "$rc"
}

parse_token_file() {
  local token_file="$1"
  python3 - "$token_file" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
print(f"Authorization: {data.get('token_type', 'Bearer')} {data['token']}")
PY
}

read_remote_token() {
  local remote_token_path="$1"
  local local_copy="$2"
  ssh -o BatchMode=yes "$REMOTE_SSH" "cat '$remote_token_path'" >"$local_copy"
  parse_token_file "$local_copy"
}

start_remote_server() {
  remote "rm -f '$SERVER_LOG_REMOTE' '$SERVER_PID_REMOTE'; LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server --config '$SERVER_CONFIG_REMOTE' --diag-stats --diag-stats-interval 1 </dev/null >'$SERVER_LOG_REMOTE' 2>&1 & echo \$! > '$SERVER_PID_REMOTE'"
  sleep 3
  remote "test -s '$SERVER_PID_REMOTE' && kill -0 \$(cat '$SERVER_PID_REMOTE')"
  remote "grep -m1 'admin token file' '$SERVER_LOG_REMOTE' | awk '{print \$NF}'" >"$RESULT_ROOT/proxy/remote-admin-token-file.txt"
  SERVER_AUTH_HEADER="$(read_remote_token "$(cat "$RESULT_ROOT/proxy/remote-admin-token-file.txt")" "$RESULT_ROOT/proxy/remote-admin-token.json")"
}

stop_remote_server() {
  remote "if [ -f '$SERVER_PID_REMOTE' ]; then kill \$(cat '$SERVER_PID_REMOTE') 2>/dev/null || true; wait \$(cat '$SERVER_PID_REMOTE') 2>/dev/null || true; rm -f '$SERVER_PID_REMOTE'; fi" || true
}

log "result root: $RESULT_ROOT"
if [[ ! -x "$LOCAL_BIN" ]]; then
  log "missing local binary: $LOCAL_BIN"
  exit 2
fi

for tool in jq curl iperf3 ssh scp; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    log "missing required tool: $tool"
    exit 2
  fi
done

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
remote_capture "$RESULT_ROOT/net/remote-ss-before.txt" "ss -ltnup; ss -lunp"

log "deploying remote test binary without overwriting default binary"
scp -q "$LOCAL_BIN" "$REMOTE_SSH:$REMOTE_BIN"
remote "chmod +x '$REMOTE_BIN'"
sha256sum "$LOCAL_BIN" >"$RESULT_ROOT/env/local-raypx2.sha256"
remote "sha256sum '$REMOTE_BIN'" >"$RESULT_ROOT/env/remote-raypx2.sha256"

cat >"$RESULT_ROOT/proxy/server-config.initial.json" <<JSON
{
  "tls": {
    "cert": "/home/jack/tcpquic-dgx-certs/10net/server-10net.crt",
    "key": "/home/jack/tcpquic-dgx-certs/10net/server-10net.key",
    "ca": "/home/jack/tcpquic-dgx-certs/ca.crt"
  },
  "admin": {
    "listen": "$SERVER_ADMIN"
  },
  "server": {
    "proto_listen": "$REMOTE_IP_A:$QUIC_PORT,$REMOTE_IP_B:$QUIC_PORT",
    "allow_targets": ["$REMOTE_IP_A/32"],
    "deny_targets": []
  },
  "proto": {
    "disable_1rtt_encryption": true,
    "keepalive_ms": 5000
  },
  "tuning": {
    "mode": "wan"
  }
}
JSON

cat >"$RESULT_ROOT/proxy/client.json" <<JSON
{
  "version": 1,
  "tls": {"cert": "cert/client/client.crt", "key": "cert/client/client.key", "ca": "cert/ca.crt"},
  "admin": {"listen": "$CLIENT_ADMIN"},
  "proto": {"disable_1rtt_encryption": true, "keepalive_ms": 5000},
  "compression": {"mode": "off"},
  "tuning": {"mode": "wan"},
  "peers": [
    {
      "peer_id": "dgx-server-acl",
      "http_listen": "",
      "socks_listen": "",
      "quic_peer": "$REMOTE_IP_A:$QUIC_PORT",
      "quic_connections": 1,
      "port_forwards": [
        {"listen": "127.0.0.1:$FORWARD_PORT", "target": "$REMOTE_IP_A:$IPERF_PORT"}
      ],
      "compress": "off",
      "enabled": true
    }
  ]
}
JSON

cat >"$RESULT_ROOT/case/patch-deny.json" <<JSON
{
  "allow_targets": ["$REMOTE_IP_A/32"],
  "deny_targets": ["$REMOTE_IP_A/32"]
}
JSON

cat >"$RESULT_ROOT/case/patch-allow.json" <<JSON
{
  "allow_targets": ["$REMOTE_IP_A/32"],
  "deny_targets": []
}
JSON

cat >"$RESULT_ROOT/case/patch-invalid.json" <<'JSON'
{
  "allow_targets": ["bad-cidr"]
}
JSON

scp -q "$RESULT_ROOT/proxy/server-config.initial.json" "$REMOTE_SSH:$SERVER_CONFIG_REMOTE"

log "starting remote iperf3 server"
remote "rm -f '$IPERF_PID_REMOTE'; nohup iperf3 -s -B '$REMOTE_IP_A' -p '$IPERF_PORT' >/home/jack/dgx-server-acl-$TS-iperf3.log 2>&1 & echo \$! > '$IPERF_PID_REMOTE'"
sleep 1

log "starting remote server from JSON config"
start_remote_server
remote "ss -ltnup; ss -lunp" >"$RESULT_ROOT/net/remote-ss-server-started.txt" 2>&1 || true

remote_admin_get "/server/config" "$RESULT_ROOT/admin/initial-server-config.json" "$RESULT_ROOT/admin/initial-server-config.status"
if [[ "$(cat "$RESULT_ROOT/admin/initial-server-config.status")" != "200" ]]; then
  log "initial server config GET failed"
  exit 10
fi

log "starting local client"
"$LOCAL_BIN" client --client-config "$RESULT_ROOT/proxy/client.json" --ca "$ROOT/cert/ca.crt" --admin-listen "$CLIENT_ADMIN" --diag-stats --diag-stats-interval 1 >"$RESULT_ROOT/proxy/local-client.stderr.log" 2>&1 &
CLIENT_PID=$!
echo "$CLIENT_PID" >"$RESULT_ROOT/proxy/local-client.pid"
sleep 4
kill -0 "$CLIENT_PID"
grep -m1 'admin token file' "$RESULT_ROOT/proxy/local-client.stderr.log" | awk '{print $NF}' >"$RESULT_ROOT/proxy/local-admin-token-file.txt"
CLIENT_AUTH_HEADER="$(parse_token_file "$(cat "$RESULT_ROOT/proxy/local-admin-token-file.txt")")"

log "waiting for initial client connection"
INITIAL_CONNECTED="$(wait_client_connected 1 initial-client-connections || true)"
log "initial connected count: $INITIAL_CONNECTED"
if [[ "$INITIAL_CONNECTED" -lt 1 ]]; then
  log "client did not connect"
  exit 11
fi

remote_admin_get "/metrics" "$RESULT_ROOT/admin/metrics-before-deny.json" "$RESULT_ROOT/admin/metrics-before-deny.status"
ACL_DENIED_BEFORE="$(metric_acl_denied "$RESULT_ROOT/admin/metrics-before-deny.json")"

log "running initial allow probe"
if ! run_iperf_probe initial-iperf 30; then
  log "initial probe failed"
  exit 12
fi

log "PATCH server ACL to deny target"
remote_admin_patch "/server/config" "$RESULT_ROOT/case/patch-deny.json" "$RESULT_ROOT/admin/patch-deny-response.json" "$RESULT_ROOT/admin/patch-deny-response.status"
if [[ "$(cat "$RESULT_ROOT/admin/patch-deny-response.status")" != "200" ]]; then
  log "deny patch failed"
  exit 13
fi
remote "cat '$SERVER_CONFIG_REMOTE'" >"$RESULT_ROOT/proxy/remote-server-config-after-deny.json"

log "running denied probe"
if run_iperf_probe deny-iperf 15; then
  log "denied probe unexpectedly succeeded"
  exit 14
fi

ACL_DENIED_AFTER="$ACL_DENIED_BEFORE"
for i in $(seq 1 10); do
  remote_admin_get "/metrics" "$RESULT_ROOT/admin/metrics-after-deny-$i.json" "$RESULT_ROOT/admin/metrics-after-deny-$i.status" || true
  ACL_DENIED_AFTER="$(metric_acl_denied "$RESULT_ROOT/admin/metrics-after-deny-$i.json" 2>/dev/null || echo "$ACL_DENIED_BEFORE")"
  if [[ "$ACL_DENIED_AFTER" -gt "$ACL_DENIED_BEFORE" ]]; then
    cp "$RESULT_ROOT/admin/metrics-after-deny-$i.json" "$RESULT_ROOT/admin/metrics-after-deny.json"
    break
  fi
  sleep 1
done
log "acl_denied before=$ACL_DENIED_BEFORE after=$ACL_DENIED_AFTER"
if [[ "$ACL_DENIED_AFTER" -le "$ACL_DENIED_BEFORE" ]]; then
  log "acl_denied did not increase"
  exit 15
fi

log "PATCH server ACL to allow target again"
remote_admin_patch "/server/config" "$RESULT_ROOT/case/patch-allow.json" "$RESULT_ROOT/admin/patch-allow-response.json" "$RESULT_ROOT/admin/patch-allow-response.status"
if [[ "$(cat "$RESULT_ROOT/admin/patch-allow-response.status")" != "200" ]]; then
  log "allow patch failed"
  exit 16
fi
remote "cat '$SERVER_CONFIG_REMOTE'" >"$RESULT_ROOT/proxy/remote-server-config-after-allow.json"

log "running recovery probe"
if ! run_iperf_probe recover-iperf 30; then
  log "recovery probe failed"
  exit 17
fi

log "PATCH invalid CIDR and verify rollback"
remote_admin_patch "/server/config" "$RESULT_ROOT/case/patch-invalid.json" "$RESULT_ROOT/admin/patch-invalid-response.json" "$RESULT_ROOT/admin/patch-invalid-response.status" || true
if [[ "$(cat "$RESULT_ROOT/admin/patch-invalid-response.status")" != "400" ]]; then
  log "invalid patch did not return 400"
  exit 18
fi
remote_admin_get "/server/config" "$RESULT_ROOT/admin/server-config-after-invalid.json" "$RESULT_ROOT/admin/server-config-after-invalid.status"
remote "cat '$SERVER_CONFIG_REMOTE'" >"$RESULT_ROOT/proxy/remote-server-config-after-invalid.json"
if [[ "$(json_value "$RESULT_ROOT/proxy/remote-server-config-after-invalid.json" '.server.deny_targets | length')" -ne 0 ]]; then
  log "invalid patch polluted deny_targets"
  exit 19
fi
if [[ "$(json_value "$RESULT_ROOT/proxy/remote-server-config-after-invalid.json" '.server.allow_targets[0]')" != "$REMOTE_IP_A/32" ]]; then
  log "invalid patch polluted allow_targets"
  exit 20
fi

log "restarting remote server to verify persisted ACL"
stop_remote_server
sleep 2
start_remote_server
remote_admin_get "/server/config" "$RESULT_ROOT/admin/post-restart-server-config.json" "$RESULT_ROOT/admin/post-restart-server-config.status"
remote "cat '$SERVER_CONFIG_REMOTE'" >"$RESULT_ROOT/proxy/remote-server-config-post-restart.json"

log "waiting for client reconnect after server restart"
POST_RESTART_CONNECTED="$(wait_client_connected 1 post-restart-client-connections || true)"
log "post-restart connected count: $POST_RESTART_CONNECTED"
if [[ "$POST_RESTART_CONNECTED" -lt 1 ]]; then
  log "client did not reconnect after server restart"
  exit 21
fi

log "running post-restart probe"
if ! run_iperf_probe post-restart-iperf 30; then
  log "post-restart probe failed"
  exit 22
fi

remote_admin_get "/metrics" "$RESULT_ROOT/admin/final-server-metrics.json" "$RESULT_ROOT/admin/final-server-metrics.status" || true
local_admin_get "/peers/dgx-server-acl/connections" "$RESULT_ROOT/admin/final-client-connections.json" "$RESULT_ROOT/admin/final-client-connections.status" || true
run_local_capture "$RESULT_ROOT/net/local-ss-after.txt" ss -ltnup
remote_capture "$RESULT_ROOT/net/remote-ss-after.txt" "ss -ltnup; ss -lunp"
cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$RESULT_ROOT/proxy/local-client.stderr.final.log"
remote "cat '$SERVER_LOG_REMOTE'" >"$RESULT_ROOT/proxy/remote-server.stderr.log" 2>&1 || true
remote "cat /home/jack/dgx-server-acl-$TS-iperf3.log" >"$RESULT_ROOT/proxy/remote-iperf3.log" 2>&1 || true

cat >"$RESULT_ROOT/summary/summary.md" <<EOF
# DGX server ACL hot update result

- result_root: $RESULT_ROOT
- git_head: $(cat "$RESULT_ROOT/env/git-head.txt")
- remote_bin: $REMOTE_BIN
- server_config_remote: $SERVER_CONFIG_REMOTE
- initial_connected: $INITIAL_CONNECTED
- acl_denied_before: $ACL_DENIED_BEFORE
- acl_denied_after: $ACL_DENIED_AFTER
- post_restart_connected: $POST_RESTART_CONNECTED
- initial_iperf_rc: $(cat "$RESULT_ROOT/case/initial-iperf.rc")
- deny_iperf_rc: $(cat "$RESULT_ROOT/case/deny-iperf.rc")
- recover_iperf_rc: $(cat "$RESULT_ROOT/case/recover-iperf.rc")
- invalid_patch_status: $(cat "$RESULT_ROOT/admin/patch-invalid-response.status")
- post_restart_iperf_rc: $(cat "$RESULT_ROOT/case/post-restart-iperf.rc")
- final_allow_targets: $(jq -c '.server.allow_targets' "$RESULT_ROOT/proxy/remote-server-config-post-restart.json")
- final_deny_targets: $(jq -c '.server.deny_targets' "$RESULT_ROOT/proxy/remote-server-config-post-restart.json")
EOF

log "server ACL hot update completed: $RESULT_ROOT"
