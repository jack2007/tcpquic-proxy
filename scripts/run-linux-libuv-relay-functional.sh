#!/usr/bin/env bash
# Linux-only libuv relay functional/fault evidence runner.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build/libuv-plan"
OUT="$ROOT/build/libuv-functional-results"
DRY_RUN=0
FIXTURE_ADMIN=""
PRINT_ADMIN_TOKEN=0
CASE_DRIVER="${FUNCTIONAL_CASE_DRIVER:-}"
PIDS=()
STARTED_MODES=()
TMP=""
CASE_PGID=""
PAIR_ID=""
LAST_FORCED_KILL=0
ANY_GROUP_FORCED_KILL=0
ALL_PROCESSES_TERMINATED=1
STARTED_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
RUN_ID="$(printf '%s-%s-%s' "$STARTED_UTC" "$$" "$RANDOM" | sha256sum | cut -c1-16)"
RUN_ID="${STARTED_UTC//:/}-${RUN_ID}"
GIT_HEAD="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
GIT_STATUS="$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)"
RECURSIVE_SUBMODULE_STATUS="$(git -C "$ROOT" submodule status --recursive 2>/dev/null)"
if [[ -n "$GIT_STATUS" ]]; then
  GIT_DIRTY=true
else
  GIT_DIRTY=false
fi
MSQUIC_HEAD="$(git -C "$ROOT/third_party/msquic" rev-parse HEAD 2>/dev/null || printf unavailable)"
if [[ -n "$(git -C "$ROOT/third_party/msquic" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then
  MSQUIC_DIRTY=true
else
  MSQUIC_DIRTY=false
fi
COMPILED_SOURCE_STATUS="$(git -C "$ROOT" status --porcelain --untracked-files=all -- src CMakeLists.txt cmake 2>/dev/null)"
[[ -z "$COMPILED_SOURCE_STATUS" ]] && COMPILED_SOURCES_CLEAN=true || COMPILED_SOURCES_CLEAN=false

usage() {
  echo "usage: $0 [--build-dir DIR] [--output-dir DIR] [--dry-run] [--fixture-admin JSON]" >&2
}
while (($#)); do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --output-dir) OUT="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --fixture-admin) FIXTURE_ADMIN="$2"; shift 2 ;;
    --print-admin-token) PRINT_ADMIN_TOKEN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage; echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done

make_admin_token() {
  printf '%s' "libuv-functional-$USER-$$-${RANDOM}-${RANDOM}" | sha256sum | awk '{print $1}'
}
if ((PRINT_ADMIN_TOKEN)); then make_admin_token; exit 0; fi

if ((DRY_RUN)); then
  EXECUTION_MODE=dry_run
elif [[ -n "$FIXTURE_ADMIN" && -n "$CASE_DRIVER" ]]; then
  EXECUTION_MODE=fixture_case_driver
elif [[ -n "$FIXTURE_ADMIN" ]]; then
  EXECUTION_MODE=fixture
elif [[ -n "$CASE_DRIVER" ]]; then
  EXECUTION_MODE=case_driver
else
  EXECUTION_MODE=production
fi

CASES=(
  http_download_off http_upload_off socks5_download_off port_forward_off
  fin_half_close tcp_refused tcp_reset quic_abort_unit queue_pressure_unit
  allocator_bootstrap_failure_unit http_download_zstd http_upload_zstd
)
EVIDENCE='["command","exit_code","proxy_logs","admin_snapshot","allocator_snapshot","terminal_counters"]'

pid_is_running() {
  local state
  state=$(ps -o stat= -p "$1" 2>/dev/null) || return 1
  [[ -n "$state" && "$state" != Z* ]]
}

terminate_pids_bounded() {
  local pid deadline alive=0
  LAST_FORCED_KILL=0
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  deadline=$((SECONDS + 2))
  while ((SECONDS < deadline)); do
    alive=0
    for pid in "${PIDS[@]}"; do pid_is_running "$pid" && alive=1; done
    ((alive)) || break
    sleep .05
  done
  if ((alive)); then
    LAST_FORCED_KILL=1
    for pid in "${PIDS[@]}"; do pid_is_running "$pid" && kill -KILL "$pid" 2>/dev/null || true; done
  fi
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  ALL_PROCESSES_TERMINATED=1
  for pid in "${PIDS[@]}"; do pid_is_running "$pid" && ALL_PROCESSES_TERMINATED=0; done
}

cleanup() {
  local rc=$?
  set +e
  if [[ -n "$CASE_PGID" ]]; then
    kill -TERM -- "-$CASE_PGID" 2>/dev/null || true
    sleep .05
    kill -KILL -- "-$CASE_PGID" 2>/dev/null || true
  fi
  if ((${#PIDS[@]})); then stop_pair; fi
  [[ -z "$TMP" ]] || rm -rf "$TMP"
  [[ -z "${FUNCTIONAL_CLEANUP_MARKER:-}" ]] || : >"$FUNCTIONAL_CLEANUP_MARKER"
  finish_report "$ANY_GROUP_FORCED_KILL"
  return "$rc"
}
on_signal() {
  local number=$1
  cleanup
  trap - EXIT INT TERM
  exit $((128 + number))
}
trap cleanup EXIT
trap 'on_signal 2' INT
trap 'on_signal 15' TERM

mkdir -p "$OUT/cases"

write_initial_report() {
  python3 - "$OUT/report.json" "$DRY_RUN" "$EXECUTION_MODE" "$RUN_ID" \
    "$STARTED_UTC" "$GIT_HEAD" "$GIT_DIRTY" "$GIT_STATUS" "$MSQUIC_HEAD" \
    "$MSQUIC_DIRTY" "$COMPILED_SOURCES_CLEAN" "$COMPILED_SOURCE_STATUS" \
    "$RECURSIVE_SUBMODULE_STATUS" "${CASES[@]}" <<'PY'
import datetime,json, sys
(path,dry,mode,run_id,started,head,dirty,status,msquic_head,msquic_dirty,
 compiled_clean,compiled_status,recursive_submodules,*names)=sys.argv[1:]
evidence = ["command", "exit_code", "proxy_logs", "admin_snapshot",
            "allocator_snapshot", "terminal_counters"]
is_dry=dry == "1"
json.dump({"schema_version": 2, "platform": "linux", "linux_only": True,
           "required_backend": "libuv", "required_allocator": "mimalloc",
           "dry_run": is_dry, "execution_mode": mode,
           "fixture": mode in ("fixture", "fixture_case_driver"),
           "case_driver": mode in ("case_driver", "fixture_case_driver"),
           "formal_gate_status": "not_applicable" if mode != "production" else "pending",
           "run_id": run_id, "started_utc": started,
           "ended_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ") if is_dry else None,
           "provenance": {
             "git":{"head":head,"dirty":dirty == "true"},
             "superproject":{"head":head,"dirty":dirty == "true","status":status.splitlines()},
             "recursive_submodules":{"current":recursive_submodules.replace("\n","|")},
             "submodules":{"third_party/msquic":{"head":msquic_head,"dirty":msquic_dirty=="true"}},
             "compiled_sources":{"clean_relative_to_head":compiled_clean=="true",
                                  "status":compiled_status.splitlines()},
             "binary":None},
           "valid": False,
           "ready": {"client": 0, "server": 0},
           "passed": 0, "failed": 0, "duplicate_settlement": 0,
           "cases": [{"name": n,
                      "layer": "unit" if n.endswith("_unit") else "e2e",
                      "group": ("zstd" if "zstd" in n else "off") if not n.endswith("_unit") else "focused",
                      "evidence": evidence if not n.endswith("_unit") else ["command","exit_code","case_log"],
                      "status": "planned"} for n in names]}, open(path, "w"), indent=2)
PY
}

finish_report() {
  local forced=${1:-0}
  [[ -f "$OUT/report.json" ]] || return 0
  python3 - "$OUT/report.json" "$EXECUTION_MODE" "$forced" <<'PY'
import datetime,json,sys
p,mode,forced=sys.argv[1:]; v=json.load(open(p))
v["ended_utc"]=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
v["cleanup"]={"completed":True,"forced_kill":forced=="1"}
if mode != "production":
    v["valid"]=False
    v["formal_gate_status"]="not_applicable"
json.dump(v,open(p,"w"),indent=2)
PY
}
write_initial_report
((DRY_RUN)) && exit 0

if [[ "$(uname -s)" != Linux ]]; then
  echo "this functional gate is Linux-only" >&2
  exit 69
fi

BIN="$BUILD_DIR/src/bin/Release/raypx2"
[[ -x "$BIN" ]] || BIN="$BUILD_DIR/bin/Release/raypx2"
TEST_BIN="$BUILD_DIR/src/bin/Release"
[[ -d "$TEST_BIN" ]] || TEST_BIN="$BUILD_DIR/bin/Release"
[[ -n "$FIXTURE_ADMIN" || -x "$BIN" ]] || {
  echo "libuv raypx2 not found under $BUILD_DIR" >&2; exit 66;
}
BIN="$(readlink -f "$BIN")"

python3 - "$OUT/report.json" "$BIN" "$ROOT" <<'PY'
import hashlib,json,os,pathlib,sys
p=pathlib.Path(sys.argv[1]); binary=pathlib.Path(sys.argv[2]); root=pathlib.Path(sys.argv[3])
v=json.load(open(p))
if binary.is_file():
    resolved=binary.resolve(); st=resolved.stat(); h=hashlib.sha256()
    with resolved.open("rb") as f:
        for block in iter(lambda:f.read(1024*1024),b""): h.update(block)
    v["provenance"]["binary"]={"path":str(resolved),"sha256":h.hexdigest(),
        "size":st.st_size,"mtime_ns":st.st_mtime_ns}
    manifest=resolved.with_name(resolved.name+".build-manifest.json")
    build={"path":str(manifest),"present":manifest.is_file(),
           "binary_hash_match":False,"compiled_source_match":False}
    if manifest.is_file():
        raw=manifest.read_bytes(); value=json.loads(raw)
        build.update({"sha256":hashlib.sha256(raw).hexdigest(),
                      "source_commit":value.get("source_commit"),
                      "source_tree_state":value.get("source_tree_state"),
                      "submodule_commits":value.get("submodule_commits"),
                      "compiled_relay_backend":value.get("compiled_relay_backend")})
        build["binary_hash_match"]=value.get("binary_sha256")==h.hexdigest()
        matches=True; checked=0
        for item in value.get("source_files","").split("|"):
            if "@" not in item: continue
            name,expected=item.rsplit("@",1); source=root/"src"/name
            if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest()!=expected:
                matches=False
            checked+=1
        build["compiled_source_count"]=checked
        build["compiled_source_match"]=matches and checked>0
    v["provenance"]["build_manifest"]=build
json.dump(v,open(p,"w"),indent=2)
PY

ADMIN_CLIENT="$FIXTURE_ADMIN"
ADMIN_SERVER="$FIXTURE_ADMIN"
TOKEN="$(make_admin_token)"
CLIENT_ADMIN_PORT=""
SERVER_ADMIN_PORT=""
CLIENT_HTTP_PORT=""
CLIENT_SOCKS_PORT=""
CLIENT_FORWARD_PORT=""
TARGET_HTTP_PORT=""
TARGET_ECHO_PORT=""
TARGET_RESET_PORT=""
TARGET_SOCKET_AUDIT=""
CASE_ID_FILE="$OUT/current-case-id"
: >"$CASE_ID_FILE"
PAIR_MODE=""

free_port() {
  python3 - <<'PY'
import socket
s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()
PY
}

wait_tcp_listener() {
  local port=$1
  python3 - "$port" <<'PY'
import socket, sys, time
deadline=time.monotonic()+5
while time.monotonic()<deadline:
    try:
        with socket.create_connection(("127.0.0.1",int(sys.argv[1])),.2): pass
        raise SystemExit(0)
    except OSError:
        time.sleep(.05)
raise SystemExit(1)
PY
}

admin_get() {
  local port=$1 path=$2
  curl -fsS --noproxy '*' --max-time 3 -H "Authorization: Bearer $TOKEN" \
    "http://127.0.0.1:${port}/api/v1${path}"
}
admin_post() {
  local port=$1 path=$2
  curl -fsS --noproxy '*' --max-time 3 -X POST -H "Authorization: Bearer $TOKEN" \
    "http://127.0.0.1:${port}/api/v1${path}"
}
linux_proc_start_ticks() {
  python3 - "$1" <<'PY'
import pathlib,sys
value=pathlib.Path(f"/proc/{sys.argv[1]}/stat").read_text()
print(int(value[value.rfind(")")+2:].split()[19]))
PY
}
capture_snapshot() {
  local port=$1 output=$2
  local role pid expected_ticks observed_ticks
  if [[ "$port" == "$CLIENT_ADMIN_PORT" ]]; then
    role=client; pid=$CLIENT_PID; expected_ticks=$CLIENT_START_TICKS
  elif [[ "$port" == "$SERVER_ADMIN_PORT" ]]; then
    role=server; pid=$SERVER_PID; expected_ticks=$SERVER_START_TICKS
  else
    return 1
  fi
  observed_ticks="$(linux_proc_start_ticks "$pid")" || return 1
  [[ "$observed_ticks" == "$expected_ticks" ]] || return 1
  local relay="${output}.relay" allocator="${output}.allocator"
  admin_get "$port" /relay/metrics >"$relay" || return 1
  admin_post "$port" /memory/allocator:dump >"$allocator" || return 1
  observed_ticks="$(linux_proc_start_ticks "$pid")" || return 1
  [[ "$observed_ticks" == "$expected_ticks" ]] || return 1
  python3 - "$relay" "$allocator" "$output" "$RUN_ID" "$PAIR_ID" "$role" \
    "$pid" "$expected_ticks" "$port" <<'PY'
import datetime,json,sys
relay=json.load(open(sys.argv[1])); allocator=json.load(open(sys.argv[2]))
relay.update(allocator)
relay["_evidence"]={"run_id":sys.argv[4],"pair_id":sys.argv[5],
                    "role":sys.argv[6],"pid":int(sys.argv[7]),
                    "proc_start_ticks":int(sys.argv[8]),"admin_port":int(sys.argv[9]),
                    "captured_utc":datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
json.dump(relay,open(sys.argv[3],"w"),indent=2)
PY
  rm -f "$relay" "$allocator"
}

stop_pair() {
  set +e
  terminate_pids_bounded
  ((LAST_FORCED_KILL)) && ANY_GROUP_FORCED_KILL=1
  if [[ -n "$PAIR_MODE" ]]; then
    mkdir -p "$OUT/groups"
    printf '{"run_id":"%s","pair_id":"%s","completed":true,"all_processes_terminated":%s,"graceful":%s,"forced_kill":%s,"timeout_seconds":2}\n' \
      "$RUN_ID" "$PAIR_ID" \
      "$([[ "$ALL_PROCESSES_TERMINATED" == 1 ]] && echo true || echo false)" \
      "$([[ "$LAST_FORCED_KILL" == 0 ]] && echo true || echo false)" \
      "$([[ "$LAST_FORCED_KILL" == 1 ]] && echo true || echo false)" \
      >"$OUT/groups/$PAIR_MODE-shutdown.json"
  fi
  PIDS=()
  [[ -z "$TMP" ]] || rm -rf "$TMP"
  TMP=""
  PAIR_ID=""
  set -u
}

start_metadata_pair() {
  local mode=${1:-off}
  PAIR_MODE=$mode
  PAIR_ID="$RUN_ID-$mode-$(printf '%s-%s' "$$" "$RANDOM" | sha256sum | cut -c1-10)"
  STARTED_MODES+=("$mode")
  TMP="$(mktemp -d "${TMPDIR:-/tmp}/libuv-functional.XXXXXX")"
  local quic
  quic=$(free_port); CLIENT_HTTP_PORT=$(free_port); CLIENT_SOCKS_PORT=$(free_port)
  CLIENT_FORWARD_PORT=$(free_port); TARGET_HTTP_PORT=$(free_port); TARGET_ECHO_PORT=$(free_port)
  TARGET_RESET_PORT=$(free_port)
  SERVER_ADMIN_PORT=$(free_port); CLIENT_ADMIN_PORT=$(free_port)
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/ca.key" -out "$TMP/ca.crt" \
    -subj /CN=functional-ca -days 1 -sha256 >"$TMP/openssl.log" 2>&1
  openssl req -newkey rsa:2048 -nodes -keyout "$TMP/server.key" -out "$TMP/server.csr" \
    -subj /CN=localhost >>"$TMP/openssl.log" 2>&1
  printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n' >"$TMP/server.ext"
  openssl x509 -req -in "$TMP/server.csr" -CA "$TMP/ca.crt" -CAkey "$TMP/ca.key" \
    -CAcreateserial -out "$TMP/server.crt" -days 1 -sha256 -extfile "$TMP/server.ext" >>"$TMP/openssl.log" 2>&1
  printf '{"version":1,"token_type":"Bearer","token":"%s","listen":"runner"}\n' "$TOKEN" >"$TMP/admin-token.json"
  chmod 600 "$TMP/admin-token.json"
  mkfifo "$TMP/server.stdin"
  exec {SERVER_FD}<>"$TMP/server.stdin"
  SERVER_CMD=("$BIN" server --listen "127.0.0.1:$quic" --allow-targets 127.0.0.0/8 \
    --cert "$TMP/server.crt" --key "$TMP/server.key" --ca "$TMP/ca.crt" --compress "$mode" \
    --admin-listen "127.0.0.1:$SERVER_ADMIN_PORT" --admin-token-file "$TMP/admin-token.json" \
    --diag-stats --diag-stats-interval 1)
  CLIENT_CMD=("$BIN" client --peer "127.0.0.1:$quic" --http-listen "127.0.0.1:$CLIENT_HTTP_PORT" \
    --socks-listen "127.0.0.1:$CLIENT_SOCKS_PORT" \
    --forward "127.0.0.1:$CLIENT_FORWARD_PORT=127.0.0.1:$TARGET_ECHO_PORT" \
    --ca "$TMP/ca.crt" --client-name functional \
    --compress "$mode" --admin-listen "127.0.0.1:$CLIENT_ADMIN_PORT" \
    --admin-token-file "$TMP/admin-token.json" --diag-stats --diag-stats-interval 1)
  "${SERVER_CMD[@]}" >"$OUT/proxy-server.log" 2>&1 <"$TMP/server.stdin" &
  SERVER_PID=$!; PIDS+=("$SERVER_PID")
  "${CLIENT_CMD[@]}" >"$OUT/proxy-client.log" 2>&1 &
  CLIENT_PID=$!; PIDS+=("$CLIENT_PID")
  mkdir -p "$OUT/groups"
  server_json="$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "${SERVER_CMD[@]}")"
  client_json="$(python3 -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "${CLIENT_CMD[@]}")"
  python3 "$ROOT/scripts/libuv-functional-evidence.py" record-pair \
    --output "$OUT/groups/$mode-pair.json" --run-id "$RUN_ID" --pair-id "$PAIR_ID" \
    --mode "$mode" --binary "$BIN" --client-pid "$CLIENT_PID" --server-pid "$SERVER_PID" \
    --client-command-json "$client_json" --server-command-json "$server_json" || return 1
  read -r CLIENT_START_TICKS SERVER_START_TICKS < <(python3 - "$OUT/groups/$mode-pair.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))["processes"]
print(value["client"]["proc_start_ticks"], value["server"]["proc_start_ticks"])
PY
)
  : >"$OUT/target-http-$mode.requests"
  python3 - "$TARGET_HTTP_PORT" "$OUT/target-http-$mode.requests" >"$OUT/target-http-$mode.log" 2>&1 <<'PY' &
import http.server, sys, time
class H(http.server.BaseHTTPRequestHandler):
    def audit(self, method):
        with open(sys.argv[2], "a") as f: f.write(f"{method} {self.path}\n")
    def do_GET(self):
        self.audit("GET")
        time.sleep(.25); body=(b"libuv-functional-data-"*32768)
        self.send_response(200); self.send_header("Content-Length",str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        self.audit("POST")
        n=int(self.headers.get("Content-Length","0")); body=self.rfile.read(n); time.sleep(.25)
        out=("uploaded:%d"%len(body)).encode(); self.send_response(200); self.send_header("Content-Length",str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self,*args): pass
http.server.ThreadingHTTPServer(("127.0.0.1",int(sys.argv[1])),H).serve_forever()
PY
  PIDS+=("$!")
  TARGET_SOCKET_AUDIT="$OUT/target-socket-$mode.audit.jsonl"
  : >"$TARGET_SOCKET_AUDIT"
  python3 - "$TARGET_ECHO_PORT" "$TARGET_RESET_PORT" "$TARGET_SOCKET_AUDIT" "$CASE_ID_FILE" >"$OUT/target-socket-$mode.log" 2>&1 <<'PY' &
import datetime, json, socket, struct, sys, threading, time
def audit(port,action):
 case_id=open(sys.argv[4]).read().strip()
 value={"utc":datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        "case_id":case_id,"port":port,"action":action}
 with open(sys.argv[3],"a") as f: f.write(json.dumps(value)+"\n")
def serve_echo(port):
 s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",port)); s.listen()
 while True:
  c,_=s.accept()
  def one(c):
   while True:
    x=c.recv(65536)
    if not x: break
    time.sleep(.05); c.sendall(x)
   c.close()
  threading.Thread(target=one,args=(c,),daemon=True).start()
def serve_rst(port):
 s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",port)); s.listen()
 while True:
  c,_=s.accept(); audit(port,"accepted"); c.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack("ii",1,0)); audit(port,"reset_so_linger_0"); c.close()
threading.Thread(target=serve_echo,args=(int(sys.argv[1]),),daemon=True).start(); serve_rst(int(sys.argv[2]))
PY
  PIDS+=("$!")
  # Eliminate fixture/listener startup races before creating the first relay.
  wait_tcp_listener "$TARGET_HTTP_PORT" || { echo "HTTP target did not become ready" >&2; return 1; }
  wait_tcp_listener "$TARGET_ECHO_PORT" || { echo "echo target did not become ready" >&2; return 1; }
  wait_tcp_listener "$TARGET_RESET_PORT" || { echo "reset target did not become ready" >&2; return 1; }
  local role port ready
  for role in server client; do
    [[ "$role" == server ]] && port=$SERVER_ADMIN_PORT || port=$CLIENT_ADMIN_PORT
    ready=0
    for _ in $(seq 1 150); do
      if capture_snapshot "$port" "$OUT/${role}-preflight.json" 2>/dev/null; then ready=1; break; fi
      sleep .1
    done
    ((ready)) || { echo "$role admin endpoint did not become ready" >&2; return 1; }
  done
  wait_tcp_listener "$CLIENT_HTTP_PORT" || { echo "HTTP CONNECT listener did not become ready" >&2; return 1; }
  wait_tcp_listener "$CLIENT_SOCKS_PORT" || { echo "SOCKS5 listener did not become ready" >&2; return 1; }
  wait_tcp_listener "$CLIENT_FORWARD_PORT" || { echo "forward listener did not become ready" >&2; return 1; }
  curl -fsS --noproxy '*' --max-time 5 "http://127.0.0.1:$TARGET_HTTP_PORT/" -o /dev/null || {
    echo "HTTP target readiness transfer failed" >&2; return 1;
  }
  # A real HTTP CONNECT transfer forces lazy relay worker bootstrap, including
  # process-wide uv_replace_allocator(), before allocator readiness is judged.
  ready=0
  for _ in $(seq 1 1); do
    if curl -fsS --noproxy '' -x "http://127.0.0.1:$CLIENT_HTTP_PORT" --proxytunnel \
      "http://127.0.0.1:$TARGET_HTTP_PORT/" --max-time 5 \
      >"$OUT/preflight-transfer.out" 2>"$OUT/preflight-transfer.log" &&
      grep -q libuv-functional-data "$OUT/preflight-transfer.out"; then
      ready=1; break
    fi
    sleep .2
  done
  if ((!ready)); then
    capture_snapshot "$SERVER_ADMIN_PORT" "$OUT/server-preflight-failure.json" 2>/dev/null || true
    capture_snapshot "$CLIENT_ADMIN_PORT" "$OUT/client-preflight-failure.json" 2>/dev/null || true
    python3 - "$OUT/report.json" "$mode" <<'PY'
import json,sys
p=sys.argv[1]; v=json.load(open(p))
v["failure_stage"]="production_http_connect_preflight"
v["failure_reason"]="timeout_with_zero_response_bytes"
v["admin_endpoints_ready"]={"client":1,"server":1}
v["failure_evidence"]=["preflight-transfer.log","proxy-client.log","proxy-server.log",
                       f"target-http-{sys.argv[2]}.requests","client-preflight-failure.json",
                       "server-preflight-failure.json"]
json.dump(v,open(p,"w"),indent=2)
PY
    echo "preflight HTTP CONNECT transfer did not become ready" >&2
    return 1
  fi
  capture_snapshot "$SERVER_ADMIN_PORT" "$OUT/server-preflight.json" || return 1
  capture_snapshot "$CLIENT_ADMIN_PORT" "$OUT/client-preflight.json" || return 1
  ADMIN_SERVER="$OUT/server-preflight.json"
  ADMIN_CLIENT="$OUT/client-preflight.json"
}

if [[ -z "$FIXTURE_ADMIN" ]]; then
  command -v openssl >/dev/null && command -v curl >/dev/null || {
    echo "openssl and curl are required" >&2; exit 69;
  }
  start_metadata_pair off || exit 70
fi

validate_admin() {
  python3 - "$ADMIN_CLIENT" "$ADMIN_SERVER" <<'PY'
import json, sys
bad=[]
for role,path in zip(("client","server"),sys.argv[1:]):
    try: value=json.load(open(path))
    except Exception as e: bad.append(f"{role}: invalid admin snapshot: {e}"); continue
    required = {
      "compiled_relay_backend":"libuv", "backend":"libuv",
      "relay_backend":"libuv", "linux_relay_backend":"libuv",
      "relay_snapshot_complete":True, "libuv_allocator_mode":"mimalloc",
      "libuv_allocator_attempted":True, "libuv_allocator_in_progress":False,
      "libuv_allocator_installed":True, "libuv_allocator_status":0,
    }
    for key,expected in required.items():
        if value.get(key) != expected: bad.append(f"{role}: {key}={expected!r} required")
if bad:
    print("admin contract failed: " + "; ".join(bad), file=sys.stderr); sys.exit(1)
PY
}
mark_admin_ready() {
  python3 - "$OUT/report.json" <<'PY'
import json,sys
p=sys.argv[1]; v=json.load(open(p)); v["ready"]={"client":1,"server":1}
json.dump(v,open(p,"w"),indent=2)
PY
}
if ! validate_admin; then
  # Preserve a machine-readable fail-closed preflight report.
  python3 - "$OUT/report.json" <<'PY'
import json,sys
p=sys.argv[1]; v=json.load(open(p)); v["ready"]={"client":0,"server":0}; json.dump(v,open(p,"w"),indent=2)
PY
  exit 65
fi
mark_admin_ready

CMD=()
build_case_command() {
  local name=$1 dir=$2
  case "$name" in
    http_download_off|http_download_zstd)
      CMD=(curl -fsS --noproxy '' -x "http://127.0.0.1:$CLIENT_HTTP_PORT" --proxytunnel
        "http://127.0.0.1:$TARGET_HTTP_PORT/" --max-time 10 -o "$dir/payload.out") ;;
    http_upload_off|http_upload_zstd)
      dd if=/dev/zero of="$dir/upload.bin" bs=4096 count=128 status=none
      CMD=(curl -fsS --noproxy '' -x "http://127.0.0.1:$CLIENT_HTTP_PORT" --proxytunnel
        -X POST --data-binary "@$dir/upload.bin" "http://127.0.0.1:$TARGET_HTTP_PORT/" --max-time 10 -o "$dir/payload.out") ;;
    socks5_download_off)
      CMD=(curl -fsS --noproxy '' --socks5-hostname "127.0.0.1:$CLIENT_SOCKS_PORT"
        "http://127.0.0.1:$TARGET_HTTP_PORT/" --max-time 10 -o "$dir/payload.out") ;;
    port_forward_off)
      CMD=(python3 -c 'import socket,sys; p=int(sys.argv[1]); data=b"port-forward-data"*4096; s=socket.create_connection(("127.0.0.1",p),5); s.sendall(data); out=b"";
while len(out)<len(data):
 x=s.recv(65536)
 if not x: break
 out+=x
s.close(); open(sys.argv[2],"wb").write(out); raise SystemExit(0 if out==data else 1)' "$CLIENT_FORWARD_PORT" "$dir/payload.out") ;;
    fin_half_close)
      CMD=(python3 -c 'import socket,sys; p,t,o=map(int,sys.argv[1:4]); s=socket.create_connection(("127.0.0.1",p),5); s.sendall((f"CONNECT 127.0.0.1:{t} HTTP/1.1\r\nHost: x\r\n\r\n").encode()); h=b"";
while b"\r\n\r\n" not in h: h+=s.recv(4096)
data=b"half-close-data"*4096; s.sendall(data); s.shutdown(socket.SHUT_WR); out=b"";
while True:
 x=s.recv(65536)
 if not x: break
 out+=x
s.close(); open(sys.argv[4],"wb").write(out); raise SystemExit(0 if b"200" in h and out==data else 1)' "$CLIENT_HTTP_PORT" "$TARGET_ECHO_PORT" 0 "$dir/payload.out") ;;
    tcp_refused)
      refused_port="$(free_port)"
      CMD=(python3 -c 'import datetime,errno,json,socket,sys
p,t=int(sys.argv[1]),int(sys.argv[2]); case_id,out=sys.argv[3],sys.argv[4]
direct_errno=0
try:
 s=socket.create_connection(("127.0.0.1",t),.3); s.close()
except OSError as e: direct_errno=e.errno or 0
s=socket.create_connection(("127.0.0.1",p),5); s.sendall((f"CONNECT 127.0.0.1:{t} HTTP/1.1\r\nHost: x\r\n\r\n").encode()); h=b""
while b"\r\n\r\n" not in h:
 x=s.recv(4096)
 if not x: break
 h+=x
s.close(); status=int(h.split(b" ",2)[1]) if h.startswith(b"HTTP/") else 0
result="connection_refused" if direct_errno==errno.ECONNREFUSED else "target_not_proven_refused"
utc=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
json.dump({"utc":utc,"case_id":case_id,"target_port":t,"expected_http_status":502,"observed_http_status":status,"connection_result":result,"direct_errno":direct_errno},open(out,"w"),indent=2)
raise SystemExit(0 if status==502 and result=="connection_refused" else 1)' "$CLIENT_HTTP_PORT" "$refused_port" "$case_id" "$dir/fault.json") ;;
    tcp_reset)
      CMD=(python3 -c 'import datetime,json,socket,sys; p,t=int(sys.argv[1]),int(sys.argv[2]); case_id,out=sys.argv[3],sys.argv[4]; s=socket.create_connection(("127.0.0.1",p),5); s.sendall((f"CONNECT 127.0.0.1:{t} HTTP/1.1\r\nHost: x\r\n\r\n").encode()); h=b"";
while b"\r\n\r\n" not in h: h+=s.recv(4096)
read_outcome="not_attempted"
try:
 s.sendall(b"trigger-reset"); x=s.recv(4096); read_outcome="eof_after_audited_reset" if not x else "unexpected_payload"
except ConnectionResetError: read_outcome="connection_reset"
except BrokenPipeError: read_outcome="broken_pipe_after_reset"
s.close(); status=int(h.split(b" ",2)[1]) if h.startswith(b"HTTP/") else 0
utc=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
if status==500: result="500_before_tunnel"; read_outcome="http_error_before_tunnel"
elif status==200: result="200_then_reset"
else: result="unexpected_status"
json.dump({"utc":utc,"case_id":case_id,"target_port":t,"expected_http_status":status,"observed_http_status":status,"connection_result":result,"read_outcome":read_outcome},open(out,"w"),indent=2)
raise SystemExit(0 if (status==500 and read_outcome=="http_error_before_tunnel") or (status==200 and read_outcome in ("connection_reset","broken_pipe_after_reset","eof_after_audited_reset")) else 1)' "$CLIENT_HTTP_PORT" "$TARGET_RESET_PORT" "$case_id" "$dir/fault.json") ;;
    quic_abort_unit)
      CMD=("$TEST_BIN/tcpquic_libuv_terminal_convergence_test") ;;
    queue_pressure_unit)
      CMD=("$TEST_BIN/tcpquic_libuv_relay_worker_queue_test") ;;
    allocator_bootstrap_failure_unit)
      CMD=("$TEST_BIN/tcpquic_libuv_allocator_test") ;;
    *) return 64 ;;
  esac
}

write_command() {
  local file=$1; shift
  printf '%q ' "$@" >"$file"; printf '\n' >>"$file"
}

process_alive() {
  local pid
  for pid in "${PIDS[@]}"; do pid_is_running "$pid" || return 1; done
}
finalize_group() {
  [[ -z "$PAIR_MODE" || -n "$FIXTURE_ADMIN" ]] && return 0
  mkdir -p "$OUT/groups"
  process_alive || return 1
  capture_snapshot "$CLIENT_ADMIN_PORT" "$OUT/groups/$PAIR_MODE-client-final.json" || return 1
  capture_snapshot "$SERVER_ADMIN_PORT" "$OUT/groups/$PAIR_MODE-server-final.json" || return 1
}

poll_fin_convergence() {
  local dir=$1 converged=0
  : >"$dir/fin-convergence.jsonl"
  for _ in $(seq 1 100); do
    capture_snapshot "$CLIENT_ADMIN_PORT" "$dir/fin-client-poll.json" 2>/dev/null || return 1
    capture_snapshot "$SERVER_ADMIN_PORT" "$dir/fin-server-poll.json" 2>/dev/null || return 1
    if python3 - "$dir/fin-client-poll.json" "$dir/fin-server-poll.json" \
      "$dir/fin-convergence.jsonl" <<'PY'
import datetime,json,sys
fields=("relay_active_relays","relay_pending_events","relay_pending_bytes",
        "relay_active_send_reservations","terminal_retained_owner_count",
        "terminal_sink_pending","relay_stop_remaining")
value={"utc":datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")}
for role,path in zip(("client","server"),sys.argv[1:3]):
    snapshot=json.load(open(path)); value[role]={key:snapshot.get(key) for key in fields}
with open(sys.argv[3],"a") as f: f.write(json.dumps(value)+"\n")
raise SystemExit(0 if all(value[role].get(key)==0 for role in ("client","server") for key in fields) else 1)
PY
    then converged=1; break; fi
    sleep .02
  done
  ((converged)) || return 1
  python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-fin \
    --poll "$dir/fin-convergence.jsonl"
}

RESULTS="$OUT/results.tsv"
: >"$RESULTS"
sync_partial_report() {
  python3 - "$OUT/report.json" "$RESULTS" <<'PY'
import json,sys
p,results=sys.argv[1:]; v=json.load(open(p)); rows={}
for line in open(results):
    name,status,rc=line.rstrip().split("\t"); rows[name]=(status,int(rc))
for case in v["cases"]:
    if case["name"] in rows:
        case["status"],case["exit_code"]=rows[case["name"]]
v["passed"]=sum(status=="passed" for status,_ in rows.values())
v["failed"]=sum(status=="failed" for status,_ in rows.values())
json.dump(v,open(p,"w"),indent=2)
PY
}
passed=0 failed=0
for name in "${CASES[@]}"; do
  dir="$OUT/cases/$name"; mkdir -p "$dir"
  layer=e2e; [[ "$name" == *_unit ]] && layer=unit
  case_id="$RUN_ID-$name"
  python3 - "$dir/case.json" "$RUN_ID" "$PAIR_ID" "$case_id" "$name" "$layer" <<'PY'
import json,sys
p,run_id,pair_id,case_id,name,layer=sys.argv[1:]
json.dump({"run_id":run_id,"pair_id":pair_id or None,"case_id":case_id,
           "name":name,"layer":layer},open(p,"w"),indent=2)
PY
  if [[ -n "$CASE_DRIVER" ]]; then
    write_command "$dir/command.txt" "$CASE_DRIVER" "$name"
    if [[ "$name" == *_unit ]]; then
      printf '{"layer":"unit","admin_bound":false,"fixture":true}\n' >"$dir/unit.json"
    else
      cp "$ADMIN_CLIENT" "$dir/admin.json"; cp "$ADMIN_CLIENT" "$dir/allocator.json"; cp "$ADMIN_SERVER" "$dir/terminal.json"
      printf 'fixture mode\n' >"$dir/proxy.log"
    fi
    setsid "$CASE_DRIVER" "$name" >"$dir/case.log" 2>&1 &
  else
    if [[ "$layer" == e2e ]]; then
      wanted=off; [[ "$name" == *zstd* ]] && wanted=zstd
      if [[ "$PAIR_MODE" != "$wanted" ]]; then
        finalize_group || exit 72
        stop_pair
        start_metadata_pair "$wanted" || exit 70
        validate_admin || exit 65
        mark_admin_ready
        python3 - "$dir/case.json" "$PAIR_ID" <<'PY'
import json,sys
p=sys.argv[1]; v=json.load(open(p)); v["pair_id"]=sys.argv[2]; json.dump(v,open(p,"w"),indent=2)
PY
      fi
      printf '%s\n' "$case_id" >"$CASE_ID_FILE"
      process_alive || { echo "production pair died before $name" >&2; exit 71; }
      capture_snapshot "$CLIENT_ADMIN_PORT" "$dir/client-before.json" || exit 72
      capture_snapshot "$SERVER_ADMIN_PORT" "$dir/server-before.json" || exit 72
    fi
    build_case_command "$name" "$dir" || exit 64
    write_command "$dir/command.txt" "${CMD[@]}"
    setsid "${CMD[@]}" >"$dir/case.log" 2>&1 &
  fi
  case_pid=$!; CASE_PGID=$case_pid
  active_observed=0
  if [[ -z "$CASE_DRIVER" && "$layer" == e2e && "$name" != tcp_refused ]]; then
    while kill -0 "$case_pid" 2>/dev/null; do
      if capture_snapshot "$CLIENT_ADMIN_PORT" "$dir/client-live.json" 2>/dev/null &&
         capture_snapshot "$SERVER_ADMIN_PORT" "$dir/server-live.json" 2>/dev/null &&
         python3 - "$dir/client-live.json" "$dir/server-live.json" <<'PY'
import json,sys
raise SystemExit(0 if sum(json.load(open(p)).get("relay_active_relays",0) for p in sys.argv[1:]) > 0 else 1)
PY
      then active_observed=1; fi
      sleep .02
    done
  fi
  wait "$case_pid"; rc=$?
  kill -TERM -- "-$CASE_PGID" 2>/dev/null || true
  sleep .02
  kill -KILL -- "-$CASE_PGID" 2>/dev/null || true
  CASE_PGID=""
  if [[ -z "$CASE_DRIVER" && "$layer" == e2e ]]; then
    process_alive || rc=73
    if [[ "$name" == fin_half_close ]]; then poll_fin_convergence "$dir" || rc=75; fi
    post_ok=0
    for _ in $(seq 1 100); do
      if capture_snapshot "$CLIENT_ADMIN_PORT" "$dir/client-after.json" 2>/dev/null &&
         capture_snapshot "$SERVER_ADMIN_PORT" "$dir/server-after.json" 2>/dev/null; then post_ok=1; break; fi
      sleep .02
    done
    ((post_ok)) || rc=72
    if ((post_ok)); then
      python3 - "$name" "$active_observed" "$dir" <<'PY' || rc=74
import json,pathlib,sys
name,active,d=sys.argv[1],int(sys.argv[2]),pathlib.Path(sys.argv[3])
cb=json.load(open(d/"client-before.json")); ca=json.load(open(d/"client-after.json"))
sb=json.load(open(d/"server-before.json")); sa=json.load(open(d/"server-after.json"))
def delta(k): return (ca.get(k,0)-cb.get(k,0))+(sa.get(k,0)-sb.get(k,0))
e={"active_relay_observed":active,"worker_events_delta":delta("relay_events_processed"),
   "terminal_handoff_delta":delta("terminal_handoff_completed"),
   "duplicate_settlement":ca.get("terminal_exactly_once_violation",0)+sa.get("terminal_exactly_once_violation",0)+ca.get("relay_accounting_duplicate_release",0)+sa.get("relay_accounting_duplicate_release",0)}
payload=d/"payload.out"; e["application_bytes"]=payload.stat().st_size if payload.exists() else 0
e["relay_activity_delta"]=sum(max(0,delta(k)) for k in (
    "relay_events_processed","relay_activation_failure_count",
    "relay_terminal_before_commit_rollbacks","terminal_handoff_started",
    "terminal_handoff_completed"))
e["zstd_compress_input_delta"]=max(0,delta("relay_tcp_read_bytes"))
e["zstd_compress_output_delta"]=max(0,delta("linux_relay_compressed_tcp_bytes"))
e["zstd_decompress_input_delta"]=max(0,delta("linux_relay_zstd_decompress_input_bytes"))
e["zstd_decompress_output_delta"]=max(0,delta("linux_relay_zstd_decompress_output_bytes"))
e["zstd_failures_delta"]=sum(max(0,delta(k)) for k in (
    "linux_relay_tcp_to_quic_compress_failures",
    "linux_relay_quic_receive_decompress_failures",
    "linux_relay_zstd_decompress_failures"))
normal=name not in ("tcp_refused","tcp_reset")
reset_before=(name=="tcp_reset" and (d/"fault.json").exists() and
              json.load(open(d/"fault.json")).get("connection_result")=="500_before_tunnel")
fault_ok=(name=="tcp_refused" or (name=="tcp_reset" and
          (reset_before or e["relay_activity_delta"]>0)))
e["passed"]=(e["duplicate_settlement"]==0 and ((not normal and fault_ok) or (normal and active>0 and e["worker_events_delta"]>0 and e["terminal_handoff_delta"]>0 and e["application_bytes"]>0)))
json.dump(e,open(d/"deltas.json","w"),indent=2)
raise SystemExit(0 if e["passed"] else 1)
PY
    fi
    if [[ "$name" == tcp_reset ]]; then
      cp "$TARGET_SOCKET_AUDIT" "$dir/target-audit.jsonl" || rc=76
      python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-fault --name "$name" \
        --case-dir "$dir" --audit "$dir/target-audit.jsonl" || rc=76
    elif [[ "$name" == tcp_refused ]]; then
      python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-fault --name "$name" \
        --case-dir "$dir" || rc=76
    fi
    if [[ "$name" == *zstd* ]]; then
      python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-zstd \
        --case-dir "$dir" --pair "$OUT/groups/zstd-pair.json" || rc=77
    fi
    cp "$dir/client-after.json" "$dir/admin.json" 2>/dev/null || rc=72
    cp "$dir/client-after.json" "$dir/allocator.json" 2>/dev/null || rc=72
    cp "$dir/server-after.json" "$dir/terminal.json" 2>/dev/null || rc=72
    { tail -200 "$OUT/proxy-client.log"; tail -200 "$OUT/proxy-server.log"; } >"$dir/proxy.log"
  elif [[ -z "$CASE_DRIVER" ]]; then
    printf '{"layer":"unit","admin_bound":false}\n' >"$dir/unit.json"
  fi
  printf '%s\n' "$rc" >"$dir/exit-code.txt"
  if ((rc==0)); then status=passed; ((passed+=1)); else status=failed; ((failed+=1)); fi
  printf '%s\t%s\t%s\n' "$name" "$status" "$rc" >>"$RESULTS"
  sync_partial_report
done

cleanup_contract=0
provenance_contract=0
if [[ -z "$CASE_DRIVER" ]]; then
  finalize_group || exit 72
  stop_pair
  python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-bindings \
    --output-dir "$OUT" --run-id "$RUN_ID" || exit 78
  python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-shutdown \
    --groups-dir "$OUT/groups" --run-id "$RUN_ID" --modes "${STARTED_MODES[@]}" || exit 79
  cleanup_contract=1
fi
if [[ "$EXECUTION_MODE" == production ]] && \
   python3 "$ROOT/scripts/libuv-functional-evidence.py" validate-provenance \
     --report "$OUT/report.json"; then
  provenance_contract=1
fi

python3 - "$OUT/report.json" "$RESULTS" "$OUT" "$ADMIN_CLIENT" "$ADMIN_SERVER" \
  "$EXECUTION_MODE" "$cleanup_contract" "$provenance_contract" <<'PY'
import json,sys
p,results,out,client,server,mode,cleanup_contract,provenance_contract=sys.argv[1:]
v=json.load(open(p)); rows={}
for line in open(results):
    name,status,rc=line.rstrip().split("\t"); rows[name]=(status,int(rc))
for case in v["cases"]:
    case["status"],case["exit_code"]=rows[case["name"]]
v["passed"]=sum(c[0]=="passed" for c in rows.values())
v["failed"]=sum(c[0]=="failed" for c in rows.values())
provenance=v.get("provenance",{}); manifest=provenance.get("build_manifest",{})
v["valid"]=(mode=="production" and v["failed"]==0 and
            v.get("ready")=={"client":1,"server":1} and cleanup_contract=="1" and
            provenance_contract=="1" and
            provenance.get("compiled_sources",{}).get("clean_relative_to_head") is True and
            manifest.get("binary_hash_match") is True and
            manifest.get("compiled_source_match") is True and
            manifest.get("compiled_relay_backend")=="libuv")
duplicate=0
paths=list(__import__('pathlib').Path(out).glob('groups/*-final.json')) or [__import__('pathlib').Path(client),__import__('pathlib').Path(server)]
for path in paths:
    m=json.load(open(path))
    duplicate += int(m.get("terminal_exactly_once_violation",0))
    duplicate += int(m.get("relay_accounting_duplicate_release",0))
v["duplicate_settlement"]=duplicate
v["admin_contract"]={k:json.load(open(client)).get(k) for k in ("compiled_relay_backend","backend","relay_backend","linux_relay_backend","relay_snapshot_complete","libuv_allocator_mode","libuv_allocator_attempted","libuv_allocator_in_progress","libuv_allocator_installed","libuv_allocator_status")}
v["valid"]=v["valid"] and duplicate==0
v["formal_gate_status"]="passed" if v["valid"] else ("not_applicable" if mode!="production" else "failed")
json.dump(v,open(p,"w"),indent=2)
PY

if ((failed)) || ! python3 - "$OUT/report.json" "$EXECUTION_MODE" <<'PY'
import json,sys
v=json.load(open(sys.argv[1])); mode=sys.argv[2]
ok=v["duplicate_settlement"]==0 and (mode!="production" or v.get("valid") is True)
raise SystemExit(0 if ok else 1)
PY
then
  echo "Linux libuv functional matrix failed: passed=$passed failed=$failed; see $OUT/report.json" >&2
  exit 1
fi
echo "Linux libuv functional matrix passed: passed=$passed failed=0 evidence=$OUT"
