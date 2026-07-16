#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REMOTE_SSH="${REMOTE_SSH:-jack@172.16.10.81}"
LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
REMOTE_IP="${REMOTE_IP:-169.254.59.196}"
DATA_IFACE="${DATA_IFACE:-enp1s0f0np0}"
REMOTE_DIR="${REMOTE_DIR:-/home/jack/tcpquic-dgx-bin}"
BIN_OVERRIDE="${BIN:-}"
REMOTE_BIN_OVERRIDE="${REMOTE_BIN:-}"
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
EVAL_ROOT="$RESULT_ROOT"
RESUME_AFTER_ORDER="${RESUME_AFTER_ORDER:-0}"
SKIP_PREFLIGHT="${SKIP_PREFLIGHT:-0}"
SKIP_START_SERVICES="${SKIP_START_SERVICES:-0}"
APPEND_SUMMARY="${APPEND_SUMMARY:-0}"
CONTINUE_ON_VARIANCE="${CONTINUE_ON_VARIANCE:-0}"
PRESERVE_LOCAL_RAYPX2="${PRESERVE_LOCAL_RAYPX2:-1}"
CONTINUE_ON_CASE_FAILURE="${CONTINUE_ON_CASE_FAILURE:-0}"
RELAY_METRICS_READY_ATTEMPTS="${RELAY_METRICS_READY_ATTEMPTS:-20}"
IPERF_CONTROL_RETRIES="${IPERF_CONTROL_RETRIES:-1}"

BACKEND="${BACKEND:-native}"
LOCAL_BACKEND="${LOCAL_BACKEND:-}"
REMOTE_BACKEND="${REMOTE_BACKEND:-}"
NATIVE_BUILD="${NATIVE_BUILD:-$ROOT/build/native-plan}"
LIBUV_BUILD="${LIBUV_BUILD:-$ROOT/build/libuv-plan}"
MATRIX="full"
DRY_RUN=0
PREFLIGHT_ONLY=0
COMPARE_ONLY_ROOT=""
SEAL_BUILD=0

usage() {
  cat <<'EOF'
Usage:
  scripts/run-dgx-netem-delay-loss-matrix.sh --preflight --native-build DIR --libuv-build DIR
  scripts/run-dgx-netem-delay-loss-matrix.sh --backend native|libuv --rounds N --matrix full
  scripts/run-dgx-netem-delay-loss-matrix.sh --compare-only RESULT_ROOT

Options used by automated tests: --dry-run, --local-backend, --remote-backend.
EOF
}

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 2
}

while (( $# > 0 )); do
  case "$1" in
    --backend) [[ $# -ge 2 ]] || die "--backend requires a value"; BACKEND="$2"; shift 2 ;;
    --local-backend) [[ $# -ge 2 ]] || die "--local-backend requires a value"; LOCAL_BACKEND="$2"; shift 2 ;;
    --remote-backend) [[ $# -ge 2 ]] || die "--remote-backend requires a value"; REMOTE_BACKEND="$2"; shift 2 ;;
    --rounds) [[ $# -ge 2 ]] || die "--rounds requires a value"; RUNS="$2"; shift 2 ;;
    --matrix) [[ $# -ge 2 ]] || die "--matrix requires a value"; MATRIX="$2"; shift 2 ;;
    --native-build) [[ $# -ge 2 ]] || die "--native-build requires a value"; NATIVE_BUILD="$2"; shift 2 ;;
    --libuv-build) [[ $# -ge 2 ]] || die "--libuv-build requires a value"; LIBUV_BUILD="$2"; shift 2 ;;
    --preflight) PREFLIGHT_ONLY=1; shift ;;
    --compare-only) [[ $# -ge 2 ]] || die "--compare-only requires a result root"; COMPARE_ONLY_ROOT="$2"; shift 2 ;;
    --seal-build) SEAL_BUILD=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[[ "$BACKEND" == native || "$BACKEND" == libuv ]] || die "backend must be native or libuv"
LOCAL_BACKEND="${LOCAL_BACKEND:-$BACKEND}"
REMOTE_BACKEND="${REMOTE_BACKEND:-$BACKEND}"
[[ "$LOCAL_BACKEND" == native || "$LOCAL_BACKEND" == libuv ]] || die "backend must be native or libuv"
[[ "$REMOTE_BACKEND" == native || "$REMOTE_BACKEND" == libuv ]] || die "backend must be native or libuv"
[[ "$LOCAL_BACKEND" == "$REMOTE_BACKEND" ]] || die "local and remote backend must match"
[[ "$LOCAL_BACKEND" == "$BACKEND" ]] || die "selected backend must match both endpoints"
[[ "$MATRIX" == full ]] || die "matrix must be full"
[[ "$RUNS" =~ ^[0-9]+$ ]] && (( RUNS >= 3 )) || die "rounds must be an integer >= 3"
[[ "$REMOTE_SSH" =~ ^[A-Za-z0-9._@:-]+$ ]] || die "REMOTE_SSH contains unsafe characters"
[[ "$DATA_IFACE" =~ ^[A-Za-z0-9_.:-]+$ ]] || die "DATA_IFACE contains unsafe characters"
[[ "$REMOTE_DIR" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "REMOTE_DIR contains unsafe characters"
[[ "$LOCAL_IP" =~ ^[0-9A-Fa-f:.]+$ && "$REMOTE_IP" =~ ^[0-9A-Fa-f:.]+$ ]] || die "data IP contains unsafe characters"
for value in "$QUIC_PORT" "$PROXY_PORT" "$SOCKS_PORT" "$IPERF_PORT" "$NETEM_LIMIT"; do
  [[ "$value" =~ ^[0-9]+$ ]] || die "numeric network parameter contains unsafe characters"
done
[[ "$SERVER_ADMIN_LISTEN" =~ ^([A-Za-z0-9._-]+):([0-9]+)$ ]] || die "SERVER_ADMIN_LISTEN must be host:port"
(( 10#${BASH_REMATCH[2]} >= 1 && 10#${BASH_REMATCH[2]} <= 65535 )) || die "SERVER_ADMIN_LISTEN port is out of range"
for value in "${DELAYS[@]}"; do
  [[ "$value" =~ ^[0-9]+$ ]] || die "DELAYS must contain integers"
  (( 10#$value <= 60000 )) || die "DELAYS value is out of range"
done
for value in "${LOSSES[@]}"; do
  [[ "$value" =~ ^[0-9]+$ ]] || die "LOSSES must contain integers"
  (( 10#$value <= 100 )) || die "LOSSES value is out of range"
done

[[ "$RESUME_AFTER_ORDER" =~ ^[0-9]+$ ]] || die "RESUME_AFTER_ORDER must be numeric"
[[ "$RELAY_METRICS_READY_ATTEMPTS" =~ ^[0-9]+$ ]] &&
  (( RELAY_METRICS_READY_ATTEMPTS >= 1 )) ||
  die "RELAY_METRICS_READY_ATTEMPTS must be an integer >= 1"
[[ "$IPERF_CONTROL_RETRIES" == 0 || "$IPERF_CONTROL_RETRIES" == 1 ]] ||
  die "IPERF_CONTROL_RETRIES must be 0 or 1"
RESUME_MODE=0
if (( RESUME_AFTER_ORDER > 0 )) || [[ "$APPEND_SUMMARY" == 1 ]]; then
  RESUME_MODE=1
  [[ "$APPEND_SUMMARY" == 1 && "$RESUME_AFTER_ORDER" =~ ^[0-9]+$ ]] || \
    die "resume requires APPEND_SUMMARY=1 and numeric RESUME_AFTER_ORDER"
  (( RESUME_AFTER_ORDER >= 2 )) || die "resume requires completed pre-anchors (order >= 2)"
  [[ "$SKIP_START_SERVICES" == 1 ]] || die "resume requires SKIP_START_SERVICES=1 to preserve process identity"
fi

BUILD_DIR="$NATIVE_BUILD"
[[ "$BACKEND" == libuv ]] && BUILD_DIR="$LIBUV_BUILD"
BIN="${BIN_OVERRIDE:-$BUILD_DIR/bin/Release/raypx2}"
REMOTE_BIN="${REMOTE_BIN_OVERRIDE:-$REMOTE_DIR/raypx2-$BACKEND}"
[[ "$REMOTE_BIN" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "REMOTE_BIN contains unsafe characters"
RESULT_ROOT="$EVAL_ROOT/$BACKEND"

SUMMARY_CSV="$RESULT_ROOT/summary.csv"
SCENARIO_CSV="$RESULT_ROOT/scenario-order.csv"
STOP_FILE="$RESULT_ROOT/STOPPED"

write_summary_header() {
  cat > "$SUMMARY_CSV" <<'EOF'
phase,order,direction,delay_ms,loss_pct,run,iperf_rc,sent_mbps,received_mbps,baseline_mbps,baseline_delta_pct,retransmits,duration_sec,netem_side,netem_iface,netem_limit,qdisc_drop_delta,qdisc_backlog_max_bytes,srtt_avg_ms,srtt_max_ms,bbr_bw_avg_mbps,bytes_in_flight_max,bytes_in_flight_source,cpu_pct,rss_bytes,context_switches,pending_bytes,event_queue_full_errors_max,relay_errors,quic_send_failures,quic_send_api_failures,quic_send_fatal_errors,send_error_scope,evidence_status,evidence_missing,notes
EOF
}

append_summary_row() {
  python3 - "$SUMMARY_CSV" "$@" <<'PY'
import csv, sys
path = sys.argv[1]
values = sys.argv[2:]
names = (
    "phase", "order", "direction", "delay_ms", "loss_pct", "run", "iperf_rc",
    "sent_mbps", "received_mbps", "baseline_mbps", "baseline_delta_pct",
    "retransmits", "duration_sec", "netem_side", "netem_iface", "netem_limit",
    "notes",
)
row = dict(zip(names, values[:len(names)]))
if len(values) > len(names) and values[len(names)]:
    import json
    with open(values[len(names)], encoding="utf-8") as evidence:
        row.update(json.load(evidence))
with open(path, newline="") as source:
    fields = next(csv.reader(source))
with open(path, "a", newline="") as output:
    csv.DictWriter(output, fieldnames=fields).writerow(row)
PY
}

cache_value() {
  local cache="$1" key="$2"
  sed -n "s/^${key}:[^=]*=//p" "$cache" 2>/dev/null | head -1
}

write_build_metadata() {
  local backend="$1" build_dir="$2" output="$3" remote_sha="$4"
  local cache="$build_dir/CMakeCache.txt" binary="${5:-$build_dir/bin/Release/raypx2}"
  local remote_binary="${6:-$REMOTE_DIR/raypx2-$backend}"
  local manifest="$binary.build-manifest.json"
  [[ -f "$cache" ]] || die "missing CMake cache: $cache"
  [[ -x "$binary" ]] || die "missing executable: $binary"
  [[ "$remote_sha" =~ ^[0-9a-f]{64}$ ]] || die "missing actual remote binary hash evidence"
  python3 "$ROOT/scripts/dgx-netem-evidence.py" verify-build \
    --backend "$backend" --binary "$binary" --cmake-cache "$cache" --manifest "$manifest" \
    > "$output.build-manifest.json"
  python3 - "$output.build-manifest.json" "$output" "$remote_binary" "$remote_sha" "$REMOTE_SSH" <<'PY'
import json, pathlib, sys
manifest_path, text_path, remote_path, remote_sha, remote_host = sys.argv[1:]
manifest = json.loads(pathlib.Path(manifest_path).read_text(encoding="utf-8"))
metadata = dict(manifest)
metadata.update({
    "local_backend": manifest["compiled_relay_backend"],
    "remote_backend": manifest["compiled_relay_backend"],
    "remote_binary_path": remote_path,
    "remote_binary_sha256": remote_sha,
    "remote_host": remote_host,
    "deployment_verified": True,
    "cmake_cache_path": str(pathlib.Path(text_path).parent / "CMakeCache.txt"),
})
pathlib.Path(text_path + ".json").write_text(
    json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
aliases = {"source_commit": "commit", "submodule_commits": "submodule_commits"}
with pathlib.Path(text_path).open("w", encoding="utf-8") as stream:
    for key, value in sorted(metadata.items()):
        if isinstance(value, bool):
            value = str(value).lower()
        stream.write(f"{aliases.get(key, key)}={value}\n")
PY
}

ensure_remote_binary() {
  local backend="$1" build_dir="$2" binary="$3" remote_binary="$4"
  local cache="$build_dir/CMakeCache.txt" manifest="$binary.build-manifest.json"
  local manifest_json local_sha remote_sha
  manifest_json="$(python3 "$ROOT/scripts/dgx-netem-evidence.py" verify-build \
    --backend "$backend" --binary "$binary" --cmake-cache "$cache" --manifest "$manifest")"
  local_sha="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["binary_sha256"])' <<<"$manifest_json")"
  remote_sha="$(remote "test -x '$remote_binary' && sha256sum '$remote_binary' | cut -d' ' -f1" 2>/dev/null || true)"
  if [[ "$remote_sha" != "$local_sha" ]]; then
    rsync -az -e "ssh -o BatchMode=yes" "$binary" "$REMOTE_SSH:$remote_binary" >/dev/null
    remote "chmod +x '$remote_binary'"
  fi
  remote_sha="$(remote "sha256sum '$remote_binary' | cut -d' ' -f1")"
  [[ "$remote_sha" == "$local_sha" ]] || die "remote binary hash mismatch after sync"
  printf '%s\n' "$remote_sha"
}

prepare_backend_evidence() {
  local backend="$1" build_dir="$2" output_root="$3"
  local binary="$build_dir/bin/Release/raypx2" remote_binary="$REMOTE_DIR/raypx2-$backend"
  [[ "$backend" == "$BACKEND" && -n "$BIN_OVERRIDE" ]] && binary="$BIN_OVERRIDE"
  [[ "$backend" == "$BACKEND" && -n "$REMOTE_BIN_OVERRIDE" ]] && remote_binary="$REMOTE_BIN_OVERRIDE"
  [[ "$remote_binary" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "remote binary path is unsafe"
  mkdir -p "$output_root"
  cp "$build_dir/CMakeCache.txt" "$output_root/CMakeCache.txt"
  local remote_sha
  remote_sha="$(ensure_remote_binary "$backend" "$build_dir" "$binary" "$remote_binary")"
  write_build_metadata "$backend" "$build_dir" "$output_root/build-metadata.txt" \
    "$remote_sha" "$binary" "$remote_binary"
}

local_admin_authorization() {
  local token_file="$1"
  python3 - "$token_file" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
print(f"{data['token_type']} {data['token']}")
PY
}

capture_remote_allocator_with_retry() {
  local remote_token="$1" remote_out="$2" attempt
  for attempt in $(seq 1 80); do
    if remote "TOKEN_FILE='$remote_token'; AUTH=\$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d[\"token_type\"]+\" \"+d[\"token\"])' \"\$TOKEN_FILE\"); curl -sS -X POST -H \"Authorization: \$AUTH\" -H 'X-Tcpquic-Expected-Backend: $BACKEND' 'http://${SERVER_ADMIN_LISTEN}/api/v1/memory/allocator:dump'" \
        > "$remote_out" &&
       python3 - "$remote_out" "$BACKEND" <<'PY'
import json, sys
try:
    body = json.load(open(sys.argv[1], encoding="utf-8"))
except (OSError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if body.get("compiled_relay_backend") == sys.argv[2] and
                 body.get("status") == "dumped" else 1)
PY
    then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

capture_allocator_snapshot() {
  local phase="$1" local_out remote_out manifest auth token_file remote_token
  local_out="$RESULT_ROOT/proxy/local-allocator.${phase}.json"
  remote_out="$RESULT_ROOT/proxy/remote-allocator.${phase}.json"
  manifest="$RESULT_ROOT/build-metadata.txt.build-manifest.json"
  if [[ "$DRY_RUN" == 1 ]]; then
    curl -sS -X POST -H "X-Tcpquic-Expected-Backend: $BACKEND" \
      "http://${CLIENT_ADMIN_LISTEN}/api/v1/memory/allocator:dump" > "$local_out"
    remote "curl -sS -X POST -H 'X-Tcpquic-Expected-Backend: $BACKEND' 'http://${SERVER_ADMIN_LISTEN}/api/v1/memory/allocator:dump'" \
      > "$remote_out"
  else
    token_file="$(cat "$RESULT_ROOT/proxy/local-admin-token-file.txt")"
    auth="$(local_admin_authorization "$token_file")"
    curl -sS -X POST -H "Authorization: $auth" \
      -H "X-Tcpquic-Expected-Backend: $BACKEND" \
      "http://${CLIENT_ADMIN_LISTEN}/api/v1/memory/allocator:dump" > "$local_out"
    remote_token="$(cat "$RESULT_ROOT/proxy/remote-admin-token-file.txt")"
    [[ "$remote_token" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "remote admin token path is unsafe"
    capture_remote_allocator_with_retry "$remote_token" "$remote_out" ||
      die "remote allocator admin endpoint did not become ready"
  fi
  python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-allocator \
    --backend "$BACKEND" --manifest "$manifest" --snapshot "$local_out"
  python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-allocator \
    --backend "$BACKEND" --manifest "$manifest" --snapshot "$remote_out"
}

capture_process_image_snapshot() {
  local phase="$1" manifest expected local_pid remote_pid side output
  manifest="$RESULT_ROOT/build-metadata.txt.build-manifest.json"
  expected="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["binary_sha256"])' "$manifest")"
  local_pid="$(cat "$RESULT_ROOT/proxy/local-client.pid")"
  remote_pid="$(remote 'cat /home/jack/dgx-netem-server.pid')"
  [[ "$local_pid" =~ ^[0-9]+$ && "$remote_pid" =~ ^[0-9]+$ ]] || die "invalid relay pid evidence"
  for side in local remote; do
    output="$RESULT_ROOT/proxy/${side}-process-image.${phase}.json"
    if [[ "$DRY_RUN" == 1 ]]; then
      local pid=123 inode=1001 path="$BIN"
      [[ "$side" == remote ]] && pid=456 inode=2001 path="$REMOTE_BIN"
      python3 - "$output" "$pid" "$inode" "$path" "${FAKE_PROCESS_SHA256:-$expected}" \
        "${FAKE_PROCESS_STARTTIME:-777}" "${FAKE_PROCESS_PID_DELTA:-0}" \
        "${FAKE_PROCESS_INODE_DELTA:-0}" "${FAKE_PROCESS_DEVICE:-1}" <<'PY'
import json, pathlib, sys, time
output, pid, inode, path, digest, starttime, pid_delta, inode_delta, device = sys.argv[1:]
pathlib.Path(output).write_text(json.dumps({
    "schema_version": 1, "pid": int(pid) + int(pid_delta),
    "starttime_ticks": int(starttime), "exe_path": path, "exe_sha256": digest,
    "exe_device": int(device), "exe_inode": int(inode) + int(inode_delta),
    "captured_unix_ns": time.time_ns(),
}, sort_keys=True) + "\n", encoding="utf-8")
PY
    elif [[ "$side" == local ]]; then
      python3 "$ROOT/scripts/dgx-netem-evidence.py" capture-process \
        --pid "$local_pid" --output "$output"
    else
      remote "python3 -c 'import hashlib,json,os,pathlib,sys,time; p=pathlib.Path(\"/proc\")/sys.argv[1]; s=(p/\"stat\").read_text().split(); e=p/\"exe\"; st=e.stat(); h=hashlib.sha256(); h.update(e.read_bytes()); print(json.dumps({\"schema_version\":1,\"pid\":int(sys.argv[1]),\"starttime_ticks\":int(s[21]),\"exe_path\":str(e.resolve()),\"exe_sha256\":h.hexdigest(),\"exe_device\":st.st_dev,\"exe_inode\":st.st_ino,\"captured_unix_ns\":time.time_ns()},sort_keys=True))' '$remote_pid'" > "$output"
    fi
    python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-process \
      --before "$output" --after "$output" --expected-sha256 "$expected"
  done
  if [[ "$phase" == after ]]; then
    for side in local remote; do
      python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-process \
        --before "$RESULT_ROOT/proxy/${side}-process-image.before.json" \
        --after "$RESULT_ROOT/proxy/${side}-process-image.after.json" \
        --expected-sha256 "$expected"
    done
  fi
}

seal_runtime_evidence_metadata() {
  python3 - "$RESULT_ROOT/build-metadata.txt.json" "$RESULT_ROOT" <<'PY'
import hashlib, json, pathlib, sys
metadata_path, root_value = map(pathlib.Path, sys.argv[1:])
metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
hashes = {}
for path in sorted((root_value / "proxy").glob("*-allocator.*.json")) + \
            sorted((root_value / "proxy").glob("*-process-image.*.json")):
    hashes[path.relative_to(root_value).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
metadata["runtime_evidence_sha256"] = hashes
metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
}

validate_resume_build_evidence() {
  local current_manifest="$BIN.build-manifest.json"
  [[ -f "$SUMMARY_CSV" && -f "$SCENARIO_CSV" ]] || die "resume state is missing summary/scenario"
  [[ -f "$RESULT_ROOT/build-metadata.txt.json" &&
     -f "$RESULT_ROOT/build-metadata.txt.build-manifest.json" ]] || die "resume build evidence is incomplete"
  python3 "$ROOT/scripts/dgx-netem-evidence.py" verify-build \
    --backend "$BACKEND" --binary "$BIN" --cmake-cache "$BUILD_DIR/CMakeCache.txt" \
    --manifest "$current_manifest" > "$RESULT_ROOT/.resume-current-manifest.json"
  local remote_row remote_host remote_path remote_sha actual
  remote_row="$(python3 - "$RESULT_ROOT/.resume-current-manifest.json" \
    "$RESULT_ROOT/build-metadata.txt.build-manifest.json" \
    "$RESULT_ROOT/build-metadata.txt.json" "$RESULT_ROOT/build-metadata.txt" "$BACKEND" <<'PY'
import hashlib, json, pathlib, sys
current_path, recorded_path, metadata_path = map(pathlib.Path, sys.argv[1:4])
text_path = pathlib.Path(sys.argv[4])
backend = sys.argv[5]
current = json.loads(current_path.read_text(encoding="utf-8"))
recorded = json.loads(recorded_path.read_text(encoding="utf-8"))
metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
if current != recorded:
    raise SystemExit("resume build manifest differs from original invocation")
expected = dict(recorded)
expected.update({
    "local_backend": backend,
    "remote_backend": backend,
    "remote_binary_path": metadata.get("remote_binary_path"),
    "remote_binary_sha256": metadata.get("remote_binary_sha256"),
    "remote_host": metadata.get("remote_host"),
    "deployment_verified": True,
    "cmake_cache_path": str(metadata_path.parent / "CMakeCache.txt"),
})
if metadata != expected:
    raise SystemExit("resume metadata differs from original sealed build/deployment evidence")
cache_path = pathlib.Path(metadata["cmake_cache_path"])
try:
    cache_sha = hashlib.sha256(cache_path.read_bytes()).hexdigest()
except OSError as exc:
    raise SystemExit(f"resume copied CMake cache is missing: {exc}")
if cache_sha != recorded.get("cmake_cache_sha256"):
    raise SystemExit("resume copied CMake cache differs from sealed build")
aliases = {"source_commit": "commit", "submodule_commits": "submodule_commits"}
expected_text = "".join(
    f"{aliases.get(key, key)}={str(value).lower() if isinstance(value, bool) else value}\n"
    for key, value in sorted(metadata.items())
)
try:
    actual_text = text_path.read_text(encoding="utf-8")
except OSError as exc:
    raise SystemExit(f"resume text metadata is missing: {exc}")
if actual_text != expected_text:
    raise SystemExit("resume text metadata differs from canonical JSON evidence")
print("\t".join((metadata.get("remote_host", ""), metadata.get("remote_binary_path", ""),
                 metadata.get("remote_binary_sha256", ""))))
PY
)"
  IFS=$'\t' read -r remote_host remote_path remote_sha <<< "$remote_row"
  [[ "$remote_host" == "$REMOTE_SSH" && "$remote_path" == "$REMOTE_BIN" ]] || \
    die "resume remote deployment identity differs from original invocation"
  actual="$(remote "sha256sum '$remote_path' | cut -d' ' -f1")"
  [[ "$actual" == "$remote_sha" ]] || die "resume remote binary hash is stale"
}

validate_resume_state() {
  python3 - "$SUMMARY_CSV" "$SCENARIO_CSV" "$RESULT_ROOT/execution-order.csv" \
    "$EVAL_ROOT/backend-execution-order.csv" "$BACKEND" "$RESUME_AFTER_ORDER" \
    "$(IFS=,; echo "${DELAYS[*]}")" "$(IFS=,; echo "${LOSSES[*]}")" "$RUNS" <<'PY'
import csv, pathlib, sys
summary_path, scenario_path, local_order_path, shared_order_path, backend, resume, delays, losses, runs = sys.argv[1:]
resume, runs = int(resume), int(runs)
plan = [("pre-anchor", "download", 10, 5, 1), ("pre-anchor", "upload", 10, 5, 1)]
for loss in map(int, losses.split(',')):
    for delay in map(int, delays.split(',')):
        for direction in ("download", "upload"):
            for run in range(1, runs + 1):
                plan.append(("matrix", direction, delay, loss, run))
matrix_end = len(plan)
if resume >= matrix_end:
    raise SystemExit("resume order must stop before post-anchor phase")

def load(path):
    try:
        return list(csv.DictReader(open(path, newline="")))
    except OSError as exc:
        raise SystemExit(f"resume evidence missing: {exc}")

for name, rows in (("summary", load(summary_path)), ("scenario", load(scenario_path))):
    orders = []
    for row in rows:
        try:
            order = int(row["order"])
            key = (row["phase"], row["direction"], int(row["delay_ms"]),
                   int(row["loss_pct"]), int(row["run"]))
        except (KeyError, ValueError) as exc:
            raise SystemExit(f"resume {name} schema is invalid: {exc}")
        if order < 1 or order > len(plan) or key != plan[order - 1]:
            raise SystemExit(f"resume {name} order/key mismatch at {order}")
        orders.append(order)
    if orders != list(range(1, resume + 1)):
        raise SystemExit(f"resume {name} must contain exactly unique completed orders 1..{resume}")

local_order = load(local_order_path)
if [(row.get("backend"), row.get("event")) for row in local_order] != [(backend, "start")]:
    raise SystemExit("resume local execution order must contain exactly the original start")
shared_order = load(shared_order_path)
pairs = [(row.get("backend"), row.get("event")) for row in shared_order]
if pairs.count((backend, "start")) != 1 or (backend, "end") in pairs or pairs[-1] != (backend, "start"):
    raise SystemExit("resume shared execution order must end at the single original start")
PY
  [[ -f "$RESULT_ROOT/proxy/local-process-image.before.json" &&
     -f "$RESULT_ROOT/proxy/remote-process-image.before.json" ]] || \
    die "resume original process identity is missing"
  [[ ! -e "$RESULT_ROOT/proxy/local-process-image.after.json" &&
     ! -e "$RESULT_ROOT/proxy/remote-process-image.after.json" ]] || \
    die "resume state already contains final process evidence"
}

validate_resume_process_identity() {
  capture_process_image_snapshot resume-current
  local manifest="$RESULT_ROOT/build-metadata.txt.build-manifest.json" expected side
  expected="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["binary_sha256"])' "$manifest")"
  for side in local remote; do
    python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-process \
      --before "$RESULT_ROOT/proxy/${side}-process-image.before.json" \
      --after "$RESULT_ROOT/proxy/${side}-process-image.resume-current.json" \
      --expected-sha256 "$expected" || die "resume process identity changed on $side"
    python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-allocator \
      --backend "$BACKEND" --manifest "$manifest" \
      --snapshot "$RESULT_ROOT/proxy/${side}-allocator.before.json" || \
      die "resume original allocator evidence is invalid on $side"
  done
  capture_allocator_snapshot resume-current
}

capture_relay_metrics() {
  local side="$1" output="$2" auth token_file remote_token attempt tmp
  tmp="${output}.ready.tmp"
  for attempt in $(seq 1 "$RELAY_METRICS_READY_ATTEMPTS"); do
    if [[ "$DRY_RUN" == 1 ]]; then
      if [[ "$side" == local ]]; then
        curl -sS -H "X-Tcpquic-Expected-Backend: $BACKEND" \
          "http://${CLIENT_ADMIN_LISTEN}/api/v1/relay/metrics" > "$tmp"
      else
        remote "curl -sS -H 'X-Tcpquic-Expected-Backend: $BACKEND' 'http://${SERVER_ADMIN_LISTEN}/api/v1/relay/metrics'" \
          > "$tmp"
      fi
    elif [[ "$side" == local ]]; then
      token_file="$(cat "$RESULT_ROOT/proxy/local-admin-token-file.txt")"
      auth="$(local_admin_authorization "$token_file")"
      curl -sS -H "Authorization: $auth" -H "X-Tcpquic-Expected-Backend: $BACKEND" \
        "http://${CLIENT_ADMIN_LISTEN}/api/v1/relay/metrics" > "$tmp"
    else
      remote_token="$(cat "$RESULT_ROOT/proxy/remote-admin-token-file.txt")"
      [[ "$remote_token" =~ ^/[A-Za-z0-9._/-]+$ ]] || die "remote admin token path is unsafe"
      remote "TOKEN_FILE='$remote_token'; AUTH=\$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d[\"token_type\"]+\" \"+d[\"token\"])' \"\$TOKEN_FILE\"); curl -sS -H \"Authorization: \$AUTH\" -H 'X-Tcpquic-Expected-Backend: $BACKEND' 'http://${SERVER_ADMIN_LISTEN}/api/v1/relay/metrics'" \
        > "$tmp"
    fi
    if python3 - "$tmp" "$BACKEND" <<'PY'
import json, sys
try:
    body = json.load(open(sys.argv[1], encoding="utf-8"))
except (OSError, ValueError):
    raise SystemExit(1)
raise SystemExit(0 if body.get("compiled_relay_backend") == sys.argv[2] else 1)
PY
    then
      mv "$tmp" "$output"
      return 0
    fi
    sleep 0.25
  done
  rm -f "$tmp"
  printf 'relay metrics readiness timed out: side=%s backend=%s\n' "$side" "$BACKEND" >&2
  return 1
}

iperf_has_control_channel_error() {
  local rc="$1" stdout_file="$2" stderr_file="$3"
  (( rc != 124 && rc < 128 )) || return 1
  grep -Eiq \
    'unable to receive cookie|unable to receive control message|control socket|port may not be available|transport endpoint is not connected' \
    "$stdout_file" "$stderr_file" 2>/dev/null
}

relay_metrics_are_converged() {
  python3 - "$1" "$2" "$BACKEND" <<'PY'
import json, sys

required_zero = (
    "active_relays",
    "relay_pending_bytes",
    "relay_pending_events",
    "relay_outstanding_quic_sends",
    "relay_outstanding_quic_send_bytes",
    "linux_relay_pending_tcp_write_bytes",
    "relay_event_queue_full_errors",
    "relay_errors",
    "linux_relay_quic_send_failures",
)
try:
    snapshots = [json.load(open(path, encoding="utf-8")) for path in sys.argv[1:3]]
except (OSError, ValueError):
    raise SystemExit(1)
backend = sys.argv[3]
healthy = all(
    item.get("compiled_relay_backend") == backend
    and item.get("relay_snapshot_complete") is True
    and all(item.get(key) == 0 for key in required_zero)
    for item in snapshots
)
raise SystemExit(0 if healthy else 1)
PY
}

capture_converged_retry_metrics() {
  local output_dir="$1" attempt
  mkdir -p "$output_dir"
  for attempt in $(seq 1 60); do
    check_raypx2_alive || return 1
    capture_relay_metrics local "$output_dir/local-relay-metrics.json" || return 1
    capture_relay_metrics remote "$output_dir/remote-relay-metrics.json" || return 1
    if relay_metrics_are_converged \
      "$output_dir/local-relay-metrics.json" \
      "$output_dir/remote-relay-metrics.json" &&
       check_raypx2_alive; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

archive_iperf_attempt() {
  local case_dir="$1" attempt_dir="$2" rc="$3"
  mkdir -p "$attempt_dir"
  [[ -f "$case_dir/iperf.stderr.txt" ]] || : > "$case_dir/iperf.stderr.txt"
  cp "$case_dir/iperf.stdout.json" "$attempt_dir/iperf.stdout.json"
  cp "$case_dir/iperf.stderr.txt" "$attempt_dir/iperf.stderr.txt"
  printf '%s\n' "$rc" > "$attempt_dir/iperf.rc"
}

restart_remote_iperf_server() {
  remote "set -eu; PID_FILE=/home/jack/dgx-netem-iperf3.pid; : 'restart iperf'; test -s \"\$PID_FILE\"; OLD=\$(cat \"\$PID_FILE\"); case \"\$OLD\" in ''|*[!0-9]*) exit 1;; esac; ps -p \"\$OLD\" -o args= | grep -Fx -- 'iperf3 -s -B $REMOTE_IP -p $IPERF_PORT'; kill -TERM \"\$OLD\"; for _ in \$(seq 1 20); do if ! kill -0 \"\$OLD\" 2>/dev/null; then break; fi; sleep 0.1; done; if kill -0 \"\$OLD\" 2>/dev/null; then kill -KILL \"\$OLD\"; fi; rm -f \"\$PID_FILE\"; nohup iperf3 -s -B '$REMOTE_IP' -p '$IPERF_PORT' >/home/jack/dgx-netem-iperf3.log 2>&1 </dev/null & echo \$! > \"\$PID_FILE\"; NEW=\$(cat \"\$PID_FILE\"); case \"\$NEW\" in ''|*[!0-9]*) exit 1;; esac; for _ in \$(seq 1 20); do if ss -H -ltnp 'sport = :$IPERF_PORT' | grep -Fq \"pid=\$NEW,\"; then break; fi; sleep 0.1; done; ps -p \"\$NEW\" -o args= | grep -Fx -- 'iperf3 -s -B $REMOTE_IP -p $IPERF_PORT'; ss -H -ltnp 'sport = :$IPERF_PORT' | grep -F \"pid=\$NEW,\""
}

archive_attempt_sampling() {
  local case_dir="$1" attempt_dir="$2" file
  for file in local-qdisc-before.txt remote-qdisc-before.txt \
    local-qdisc-after.txt remote-qdisc-after.txt \
    local-qdisc-samples.txt remote-qdisc-samples.txt \
    local-ss.txt remote-ss.txt local-process.txt remote-process.txt \
    local-relay-metrics.before.json remote-relay-metrics.before.json; do
    [[ ! -f "$case_dir/$file" ]] || cp "$case_dir/$file" "$attempt_dir/$file"
  done
}

collect_case_evidence() {
  local case_dir="$1" netem_side="$2" allow_incomplete="${3:-0}"
  if [[ "$netem_side" == local ]]; then
    cp "$case_dir/local-qdisc-before.txt" "$case_dir/qdisc-before.txt"
    cp "$case_dir/local-qdisc-after.txt" "$case_dir/qdisc-after.txt"
    cp "$case_dir/local-qdisc-samples.txt" "$case_dir/qdisc-samples.txt"
  else
    cp "$case_dir/remote-qdisc-before.txt" "$case_dir/qdisc-before.txt"
    cp "$case_dir/remote-qdisc-after.txt" "$case_dir/qdisc-after.txt"
    cp "$case_dir/remote-qdisc-samples.txt" "$case_dir/qdisc-samples.txt"
  fi
  cat "$case_dir/local-ss.txt" "$case_dir/remote-ss.txt" > "$case_dir/ss.txt"
  cat "$case_dir/local-process.txt" "$case_dir/remote-process.txt" > "$case_dir/process.txt"
  capture_relay_metrics local "$case_dir/local-relay-metrics.after.json"
  capture_relay_metrics remote "$case_dir/remote-relay-metrics.after.json"
  python3 - "$case_dir/local-relay-metrics.before.json" "$case_dir/remote-relay-metrics.before.json" \
    "$case_dir/local-relay-metrics.after.json" "$case_dir/remote-relay-metrics.after.json" \
    "$case_dir/metrics.json" <<'PY'
import json, sys
before = [json.load(open(path, encoding="utf-8")) for path in sys.argv[1:3]]
after = [json.load(open(path, encoding="utf-8")) for path in sys.argv[3:5]]
items = before + after
if len({item.get("compiled_relay_backend") for item in items}) != 1:
    raise SystemExit("data-validity relay metrics backend mismatch")
merged = {"compiled_relay_backend": items[0].get("compiled_relay_backend"),
          "relay_snapshot_complete": all(item.get("relay_snapshot_complete") is True for item in items)}
pending = [item["relay_pending_bytes"] for item in items if "relay_pending_bytes" in item]
if pending:
    merged["relay_pending_bytes"] = max(pending)
for key in ("relay_event_queue_full_errors", "relay_errors", "linux_relay_quic_send_failures"):
    if all(key in item for item in items):
        merged[key] = sum(max(0, after[index][key] - before[index][key]) for index in (0, 1))
json.dump(merged, open(sys.argv[5], "w", encoding="utf-8"), sort_keys=True)
PY
  local extra=()
  [[ "$allow_incomplete" != 1 ]] || extra+=(--allow-incomplete)
  python3 "$ROOT/scripts/dgx-netem-evidence.py" parse-case \
    --case-dir "$case_dir" --output "$case_dir/evidence.json" "${extra[@]}"
}

capture_case_process_sample() {
  local side="$1" phase="$2" output="$3" pid
  pid="$(cat "$RESULT_ROOT/proxy/local-client.pid")"
  [[ "$side" == local ]] || pid="$(remote 'cat /home/jack/dgx-netem-server.pid')"
  if [[ "$DRY_RUN" == 1 ]]; then
    local ns=1000000000 ticks=100 pages=1024 ctx=10
    [[ "$phase" == after ]] && ns=2000000000 ticks=110 pages=1030 ctx=16
    printf '%s,%s,%s,%s,%s,%s,100,4096\n' "$side" "$phase" "$ns" "$ticks" "$pages" "$ctx" >> "$output"
  elif [[ "$side" == local ]]; then
    python3 - "$side" "$phase" "$pid" >> "$output" <<'PY'
import os, pathlib, sys, time
side, phase, pid = sys.argv[1:]
p = pathlib.Path('/proc') / pid
s = (p/'stat').read_text().split()
status = (p/'status').read_text().splitlines()
ctx = sum(int(line.split(':',1)[1]) for line in status if line.startswith(('voluntary_ctxt_switches:', 'nonvoluntary_ctxt_switches:')))
print(','.join(map(str, (side, phase, time.monotonic_ns(), int(s[13])+int(s[14]), int(s[23]), ctx, os.sysconf('SC_CLK_TCK'), os.sysconf('SC_PAGE_SIZE')))))
PY
  else
    remote "python3 -c 'import os,pathlib,sys,time; p=pathlib.Path(\"/proc\")/sys.argv[3]; s=(p/\"stat\").read_text().split(); q=(p/\"status\").read_text().splitlines(); c=sum(int(x.split(\":\",1)[1]) for x in q if x.startswith((\"voluntary_ctxt_switches:\",\"nonvoluntary_ctxt_switches:\"))); print(\",\".join(map(str,(sys.argv[1],sys.argv[2],time.monotonic_ns(),int(s[13])+int(s[14]),int(s[23]),c,os.sysconf(\"SC_CLK_TCK\"),os.sysconf(\"SC_PAGE_SIZE\")))))' '$side' '$phase' '$pid'" >> "$output"
  fi
}

sample_transport() {
  local side="$1" out="$2"
  if [[ "$side" == local ]]; then
    while true; do
      ss -ti state established "( sport = :$PROXY_PORT or dport = :$PROXY_PORT )" || true
      sleep 1
    done > "$out" 2>&1
  else
    while true; do
      remote "ss -ti state established '( sport = :$IPERF_PORT or dport = :$IPERF_PORT )'" || true
      sleep 1
    done > "$out" 2>&1
  fi
}

record_execution_event() {
  local event="$1" shared="$EVAL_ROOT/backend-execution-order.csv"
  if [[ ! -e "$shared" ]]; then
    printf 'backend,event,timestamp\n' > "$shared"
  fi
  printf '%s,%s,%s\n' "$BACKEND" "$event" "$(date -Iseconds)" >> "$shared"
}

compare_results() {
  local root="$1"
  python3 - "$root" <<'PY'
import csv
import pathlib
import statistics
import sys

root = pathlib.Path(sys.argv[1])
backends = ("native", "libuv")
rows_by_backend = {}
for backend in backends:
    path = root / backend / "summary.csv"
    if not path.is_file():
        raise SystemExit(f"missing summary: {path}")
    all_rows = list(csv.DictReader(path.open(newline="")))
    rows_by_backend[backend] = [
        row for row in all_rows if not row.get("phase") or row.get("phase") == "matrix"
    ]

keys = sorted(
    set((r["direction"], int(r["delay_ms"]), int(r["loss_pct"]))
        for rows in rows_by_backend.values() for r in rows),
    key=lambda value: (value[2], value[1], value[0]),
)
expected_keys = {
    (direction, delay, loss)
    for loss in (5, 10)
    for delay in (10, 20, 50, 80, 100, 150, 200)
    for direction in ("download", "upload")
}

problems = []
required_metrics = (
    "qdisc_drop_delta", "qdisc_backlog_max_bytes", "srtt_avg_ms",
    "bbr_bw_avg_mbps", "bytes_in_flight_max", "cpu_pct", "rss_bytes",
    "context_switches", "pending_bytes", "event_queue_full_errors_max",
    "relay_errors", "quic_send_failures", "send_error_scope",
    "evidence_status",
)
for backend, rows in rows_by_backend.items():
    actual_keys = {(r["direction"], int(r["delay_ms"]), int(r["loss_pct"])) for r in rows}
    if actual_keys != expected_keys:
        problems.append(f"{backend} key count={len(actual_keys)}, expected={len(expected_keys)}")
    for key in expected_keys:
        selected_rows = [r for r in rows if
                         (r["direction"], int(r["delay_ms"]), int(r["loss_pct"])) == key]
        run_ids = []
        try:
            run_ids = [int(r["run"]) for r in selected_rows]
        except (KeyError, ValueError):
            problems.append(f"{backend} {key} invalid run ids")
        if len(run_ids) < 3 or len(set(run_ids)) != len(run_ids) or \
                set(run_ids) != set(range(1, len(run_ids) + 1)):
            problems.append(f"{backend} {key} run ids must be unique 1..N with N>=3")
        successful = [r for r in selected_rows
                      if r.get("iperf_rc") == "0" and r.get("notes") == "ok"
                      and r.get("received_mbps", "") != ""]
        if len(successful) < 3:
            problems.append(f"{backend} {key} fewer than 3 successful samples")
        for row in successful:
            missing = [field for field in required_metrics if row.get(field, "") == ""]
            if row.get("evidence_status") != "complete":
                missing.append("evidence_status=complete")
            if missing:
                problems.append(
                    f"{backend} {key} run={row.get('run')} data-validity missing "
                    + ",".join(missing))
if problems:
    raise SystemExit("incomplete fixed matrix: " + "; ".join(problems))
metadata = {}
for backend in backends:
    path = root / backend / "build-metadata.txt"
    if not path.is_file():
        raise SystemExit(f"missing metadata: {path}")
    values = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    if values.get("compiled_relay_backend") != backend:
        raise SystemExit(
            f"metadata backend mismatch: {backend} directory reports "
            f"{values.get('compiled_relay_backend', 'missing')}"
        )
    metadata[backend] = values
metrics = (
    "retransmits", "qdisc_drop_delta", "qdisc_backlog_max_bytes",
    "srtt_avg_ms", "bbr_bw_avg_mbps", "bytes_in_flight_max", "cpu_pct",
    "rss_bytes", "context_switches", "pending_bytes", "relay_errors",
)
max_metrics = (
    "event_queue_full_errors_max", "quic_send_failures",
    "quic_send_api_failures", "quic_send_fatal_errors",
)

def selected(backend, key):
    direction, delay, loss = key
    return [r for r in rows_by_backend[backend]
            if r["direction"] == direction and int(r["delay_ms"]) == delay
            and int(r["loss_pct"]) == loss]

def valid(rows):
    return [r for r in rows if r.get("iperf_rc") == "0" and r.get("notes") == "ok"
            and r.get("received_mbps", "") != ""]

def numbers(rows, field):
    values = []
    for row in rows:
        try:
            values.append(float(row.get(field, "")))
        except (TypeError, ValueError):
            pass
    return values

fieldnames = [
    "direction", "delay_ms", "loss_pct",
    "native_compiled_backend", "libuv_compiled_backend",
    "native_runs", "libuv_runs", "native_failures", "libuv_failures",
    "native_median_mbps", "native_min_mbps", "native_max_mbps",
    "libuv_median_mbps", "libuv_min_mbps", "libuv_max_mbps", "libuv_delta_pct",
]
for metric in metrics:
    fieldnames.extend((f"native_{metric}_median", f"libuv_{metric}_median"))
for metric in max_metrics:
    suffix = metric if metric.endswith("_max") else f"{metric}_max"
    fieldnames.extend((f"native_{suffix}", f"libuv_{suffix}"))

output_rows = []
for key in keys:
    out = {"direction": key[0], "delay_ms": key[1], "loss_pct": key[2],
           "native_compiled_backend": metadata["native"]["compiled_relay_backend"],
           "libuv_compiled_backend": metadata["libuv"]["compiled_relay_backend"]}
    throughput = {}
    for backend in backends:
        all_rows = selected(backend, key)
        ok_rows = valid(all_rows)
        received = numbers(ok_rows, "received_mbps")
        out[f"{backend}_runs"] = len(ok_rows)
        out[f"{backend}_failures"] = len(all_rows) - len(ok_rows)
        if len(received) >= 3:
            throughput[backend] = statistics.median(received)
            out[f"{backend}_median_mbps"] = f"{throughput[backend]:.2f}"
            out[f"{backend}_min_mbps"] = f"{min(received):.2f}"
            out[f"{backend}_max_mbps"] = f"{max(received):.2f}"
        else:
            out[f"{backend}_median_mbps"] = ""
            out[f"{backend}_min_mbps"] = ""
            out[f"{backend}_max_mbps"] = ""
        for metric in metrics:
            values = numbers(ok_rows, metric)
            out[f"{backend}_{metric}_median"] = f"{statistics.median(values):.2f}" if values else ""
        for metric in max_metrics:
            values = numbers(ok_rows, metric)
            suffix = metric if metric.endswith("_max") else f"{metric}_max"
            out[f"{backend}_{suffix}"] = f"{max(values):.2f}" if values else ""
    native = throughput.get("native")
    libuv = throughput.get("libuv")
    out["libuv_delta_pct"] = f"{(libuv-native)/native*100:.2f}" if native and libuv is not None else ""
    output_rows.append(out)

with (root / "comparison.csv").open("w", newline="") as stream:
    writer = csv.DictWriter(stream, fieldnames=fieldnames)
    writer.writeheader()
    writer.writerows(output_rows)

with (root / "comparison.md").open("w", encoding="utf-8") as stream:
    stream.write("# Linux DGX native/libuv 对比\n\n")
    stream.write("本报告只呈现测量数据和数据有效性异常，不作自动取舍结论。\n\n")
    stream.write("| direction | delay_ms | loss_pct | native median Mbps | libuv median Mbps | libuv delta % | native/libuv failures |\n")
    stream.write("|---|---:|---:|---:|---:|---:|---:|\n")
    for row in output_rows:
        stream.write(f"| {row['direction']} | {row['delay_ms']} | {row['loss_pct']} | "
                     f"{row['native_median_mbps']} | {row['libuv_median_mbps']} | "
                     f"{row['libuv_delta_pct']} | {row['native_failures']}/{row['libuv_failures']} |\n")
PY
}

validate_result_evidence() {
  local root="$1" backend host remote_path expected actual rows
  rows="$(python3 "$ROOT/scripts/dgx-netem-evidence.py" validate-result --root "$root")"
  while IFS=$'\t' read -r backend host remote_path expected; do
    [[ -n "$backend" && -n "$host" && -n "$remote_path" && -n "$expected" ]] || \
      die "invalid deployment evidence row"
    actual="$(ssh -o BatchMode=yes "$host" "sha256sum '$remote_path' | cut -d' ' -f1")"
    [[ "$actual" == "$expected" ]] || die "$backend remote deployment hash is stale"
  done <<< "$rows"
}

plan_dry_run() {
  mkdir -p "$RESULT_ROOT"/{proxy,cases,summary,anomalies}
  printf '123\n' > "$RESULT_ROOT/proxy/local-client.pid"
  if [[ "$RESUME_MODE" == 1 ]]; then
    validate_resume_build_evidence
    validate_resume_state
    validate_resume_process_identity
  else
    write_summary_header
    capture_allocator_snapshot before
    capture_process_image_snapshot before
    printf 'phase,order,direction,delay_ms,loss_pct,run\n' > "$SCENARIO_CSV"
    : > "$RESULT_ROOT/execution-order.csv"
    printf 'backend,event,timestamp\n%s,start,%s\n' "$BACKEND" "$(date -Iseconds)" \
      >> "$RESULT_ROOT/execution-order.csv"
    record_execution_event start
  fi
  local order=2 loss delay direction run
  dry_case() {
    local phase="$1" direction="$2" delay="$3" loss="$4" run="$5"
    local label case_dir netem_side
    order=$((order + 1))
    label="order-$(printf '%04d' "$order")-${phase}-${direction}-$(printf '%03d' "$delay")ms-${loss}loss-run${run}"
    case_dir="$RESULT_ROOT/cases/$label"
    mkdir -p "$case_dir"
    printf 'backend=%s\nphase=%s\ndirection=%s\ndelay_ms=%s\nloss_pct=%s\nrun=%s\n' \
      "$BACKEND" "$phase" "$direction" "$delay" "$loss" "$run" > "$case_dir/run.env"
    tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-before.txt"
    remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-before.txt"
    if [[ "$direction" == download ]]; then
      netem_side=remote
      remote "tc qdisc replace dev '$DATA_IFACE' root netem delay ${delay}ms loss ${loss}% limit ${NETEM_LIMIT}" >/dev/null
    else
      netem_side=local
      tc qdisc replace dev "$DATA_IFACE" root netem delay "${delay}ms" loss "${loss}%" limit "$NETEM_LIMIT" >/dev/null
    fi
    : > "$case_dir/local-process.txt"
    : > "$case_dir/remote-process.txt"
    capture_case_process_sample local before "$case_dir/local-process.txt"
    capture_case_process_sample remote before "$case_dir/remote-process.txt"
    capture_relay_metrics local "$case_dir/local-relay-metrics.before.json"
    capture_relay_metrics remote "$case_dir/remote-relay-metrics.before.json"
    local rc attempt=1 attempt_dir retry_performed=0 blocking_failure=""
    while :; do
      set +e
      if [[ "$direction" == download ]]; then
        iperf3 -c "$REMOTE_IP" --json --reverse \
          > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
      else
        iperf3 -c "$REMOTE_IP" --json \
          > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt"
      fi
      rc=$?
      set -e
      [[ "$rc" != 0 ]] || break
      (( attempt <= IPERF_CONTROL_RETRIES )) || break
      iperf_has_control_channel_error "$rc" \
        "$case_dir/iperf.stdout.json" "$case_dir/iperf.stderr.txt" || break
      attempt_dir="$case_dir/attempt-$attempt"
      if ! capture_converged_retry_metrics "$attempt_dir"; then
        blocking_failure=iperf_control_retry_health_gate_failed
        break
      fi
      archive_iperf_attempt "$case_dir" "$attempt_dir" "$rc"
      tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-after.txt"
      remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-after.txt"
      tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-samples.txt"
      remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-samples.txt"
      ss -ti state established "( sport = :$PROXY_PORT or dport = :$PROXY_PORT )" \
        > "$case_dir/local-ss.txt"
      remote "ss -ti state established '( sport = :$IPERF_PORT or dport = :$IPERF_PORT )'" \
        > "$case_dir/remote-ss.txt"
      capture_case_process_sample local after "$case_dir/local-process.txt"
      capture_case_process_sample remote after "$case_dir/remote-process.txt"
      archive_attempt_sampling "$case_dir" "$attempt_dir"
      printf '%s\n' "iperf control transient: ${label} attempt=${attempt} rc=${rc}" \
        >> "$RESULT_ROOT/known-anomalies.txt"
      if ! restart_remote_iperf_server > "$attempt_dir/remote-iperf3-restart.txt" 2>&1; then
        blocking_failure=iperf_server_restart_failed
        break
      fi
      tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-before.txt"
      remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-before.txt"
      : > "$case_dir/local-process.txt"
      : > "$case_dir/remote-process.txt"
      capture_case_process_sample local before "$case_dir/local-process.txt"
      capture_case_process_sample remote before "$case_dir/remote-process.txt"
      capture_relay_metrics local "$case_dir/local-relay-metrics.before.json"
      capture_relay_metrics remote "$case_dir/remote-relay-metrics.before.json"
      retry_performed=1
      attempt=$((attempt + 1))
    done
    tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-after.txt"
    remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-after.txt"
    tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-samples.txt"
    remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-samples.txt"
    ss -ti state established "( sport = :$PROXY_PORT or dport = :$PROXY_PORT )" > "$case_dir/local-ss.txt"
    remote "ss -ti state established '( sport = :$IPERF_PORT or dport = :$IPERF_PORT )'" > "$case_dir/remote-ss.txt"
    capture_case_process_sample local after "$case_dir/local-process.txt"
    capture_case_process_sample remote after "$case_dir/remote-process.txt"
    if [[ "$rc" != 0 ]]; then
      collect_case_evidence "$case_dir" "$netem_side" 1
      if [[ -n "$blocking_failure" || "$retry_performed" == 1 ]]; then
        [[ -n "$blocking_failure" ]] || blocking_failure=iperf_control_retry_exhausted
        collect_freeze "$case_dir" "$blocking_failure"
        die "dry-run blocking iperf failure: $label reason=$blocking_failure"
      fi
      if [[ "$CONTINUE_ON_CASE_FAILURE" != 1 ]]; then
        die "dry-run iperf case failed: $label"
      fi
      append_summary_row "$phase" "$order" "$direction" "$delay" "$loss" "$run" "$rc" \
        "" "" "" "" "" "" "$netem_side" "$DATA_IFACE" "$NETEM_LIMIT" "iperf_rc_${rc}" \
        "$case_dir/evidence.json"
      printf '%s,%s,%s,%s,%s,%s\n' "$phase" "$order" "$direction" "$delay" "$loss" "$run" >> "$SCENARIO_CSV"
      printf '%s\n' "case failure: ${label} iperf_rc=${rc}" >> "$RESULT_ROOT/known-anomalies.txt"
    else
      collect_case_evidence "$case_dir" "$netem_side"
      append_summary_row "$phase" "$order" "$direction" "$delay" "$loss" "$run" 0 \
        101.00 100.00 "" "" 1 1 "$netem_side" "$DATA_IFACE" "$NETEM_LIMIT" ok \
        "$case_dir/evidence.json"
      printf '%s,%s,%s,%s,%s,%s\n' "$phase" "$order" "$direction" "$delay" "$loss" "$run" >> "$SCENARIO_CSV"
    fi
  }
  if [[ "$RESUME_MODE" != 1 ]]; then
    order=0
    for direction in download upload; do
      dry_case pre-anchor "$direction" 10 5 1
    done
  fi
  for loss in "${LOSSES[@]}"; do
    for delay in "${DELAYS[@]}"; do
      for direction in download upload; do
        for run in $(seq 1 "$RUNS"); do
          if (( order + 1 <= RESUME_AFTER_ORDER )); then
            order=$((order + 1))
            continue
          fi
          dry_case matrix "$direction" "$delay" "$loss" "$run"
        done
      done
    done
  done
  for direction in download upload; do
    dry_case post-anchor "$direction" 10 5 1
  done
  capture_allocator_snapshot after
  capture_process_image_snapshot after
  seal_runtime_evidence_metadata
  printf '%s,end,%s\n' "$BACKEND" "$(date -Iseconds)" >> "$RESULT_ROOT/execution-order.csv"
  record_execution_event end
}

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
  remote "ss -tanp; echo ===UDP===; ss -uanp; echo ===PS===; ps -ef; echo ===QDISC===; tc -s qdisc show dev '$DATA_IFACE'; echo ===LOG===; cat /home/jack/dgx-netem-server.stderr.log" \
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
      remote "tc -s qdisc show dev '$DATA_IFACE'" || true
      sleep 5
    done > "$out" 2>&1
  fi
}

write_summary_md() {
  python3 - "$SUMMARY_CSV" "$RESULT_ROOT/summary.md" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="")))
matrix_rows = [row for row in rows if row.get("phase", "matrix") == "matrix"]
anchors = [row for row in rows if row.get("phase") in ("pre-anchor", "post-anchor")]
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write("# DGX netem delay/loss matrix summary\n\n")
    f.write(f"- Matrix cases: {len(matrix_rows)}\n")
    f.write("\n| direction | delay_ms | loss_pct | runs | avg_received_mbps | min | max | failures |\n")
    f.write("|---|---:|---:|---:|---:|---:|---:|---:|\n")
    keys = sorted(set((r["direction"], int(r["delay_ms"]), int(r["loss_pct"])) for r in matrix_rows),
                  key=lambda x: (x[2], x[1], x[0]))
    for direction, delay, loss in keys:
        vals, failures = [], 0
        for r in matrix_rows:
            if r["direction"] == direction and int(r["delay_ms"]) == delay and int(r["loss_pct"]) == loss:
                if r["iperf_rc"] != "0" or r["notes"] != "ok":
                    failures += 1
                    continue
                try:
                    vals.append(float(r["received_mbps"]))
                except Exception:
                    failures += 1
        if vals:
            f.write(f"| {direction} | {delay} | {loss} | {len(vals)} | "
                    f"{sum(vals)/len(vals):.2f} | {min(vals):.2f} | {max(vals):.2f} | {failures} |\n")
    f.write("\n## 前后锚点（不计入矩阵统计）\n\n")
    f.write("| phase | direction | received_mbps | evidence_status |\n|---|---|---:|---|\n")
    for row in anchors:
        f.write(f"| {row['phase']} | {row['direction']} | {row['received_mbps']} | {row.get('evidence_status','')} |\n")
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
    if [[ "$PRESERVE_LOCAL_RAYPX2" == 1 ]]; then
      echo "PRESERVE_LOCAL_RAYPX2=1: skip local pkill -x raypx2"
      pgrep -a -x raypx2 2>/dev/null || true
    else
      pkill -9 -x raypx2 2>/dev/null || true
    fi
    pkill -9 -x iperf3 2>/dev/null || true
    sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
    tc -s qdisc show dev "$DATA_IFACE" || true
  } > "$RESULT_ROOT/preflight-cleanup-local.log" 2>&1
  remote "pkill -9 -x raypx2 2>/dev/null || true; pkill -9 -x raypx2-native 2>/dev/null || true; pkill -9 -x raypx2-libuv 2>/dev/null || true; pkill -9 -x iperf3 2>/dev/null || true; sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true; tc -s qdisc show dev '$DATA_IFACE' || true" \
    > "$RESULT_ROOT/preflight-cleanup-remote.log" 2>&1

  log "start remote iperf3 server"
  remote "set -eu; rm -f /home/jack/dgx-netem-iperf3.pid /home/jack/dgx-netem-iperf3.log; nohup iperf3 -s -B '$REMOTE_IP' -p '$IPERF_PORT' >/home/jack/dgx-netem-iperf3.log 2>&1 </dev/null & echo \$! > /home/jack/dgx-netem-iperf3.pid; PID=\$(cat /home/jack/dgx-netem-iperf3.pid); case \"\$PID\" in ''|*[!0-9]*) exit 1;; esac; for _ in \$(seq 1 20); do if ss -H -ltnp 'sport = :$IPERF_PORT' | grep -Fq \"pid=\$PID,\"; then break; fi; sleep 0.1; done; ps -p \"\$PID\" -o args= | grep -Fx -- 'iperf3 -s -B $REMOTE_IP -p $IPERF_PORT'; ss -H -ltnp 'sport = :$IPERF_PORT' | grep -F \"pid=\$PID,\"" \
    > "$RESULT_ROOT/proxy/remote-iperf3-server.txt"

  log "start remote raypx2 server"
  remote "rm -f /home/jack/dgx-netem-server.stderr.log /home/jack/dgx-netem-server.pid; LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server --listen '$REMOTE_IP:$QUIC_PORT' --allow-targets '$REMOTE_IP/32,127.0.0.0/8' --cert /home/jack/tcpquic-dgx-certs/server/server.crt --key /home/jack/tcpquic-dgx-certs/server/server.key --ca /home/jack/tcpquic-dgx-certs/ca.crt --compress off --tuning wan --connections 1 --admin-listen '$SERVER_ADMIN_LISTEN' --diag-stats --diag-stats-interval 1 </dev/null >/home/jack/dgx-netem-server.stderr.log 2>&1 & echo \$! > /home/jack/dgx-netem-server.pid"
  for _ in $(seq 1 80); do
    remote 'grep -q "QUIC server listening" /home/jack/dgx-netem-server.stderr.log' && break
    sleep 0.5
  done
  remote 'grep -q "QUIC server listening" /home/jack/dgx-netem-server.stderr.log'
  remote 'grep -q "admin token file" /home/jack/dgx-netem-server.stderr.log'
  remote "EXPECTED=\$(cat /home/jack/dgx-netem-server.pid); LISTENERS=\$(ss -H -ltnp 'sport = :${SERVER_ADMIN_LISTEN##*:}' | grep -o 'pid=[0-9]*' | sort -u); test \"\$LISTENERS\" = \"pid=\$EXPECTED\"" ||
    die "remote admin port is not owned exclusively by the current server"
  remote 'cat /home/jack/dgx-netem-server.pid; ps -p $(cat /home/jack/dgx-netem-server.pid) -o pid,ppid,stat,etime,cmd' \
    > "$RESULT_ROOT/proxy/remote-server-pid.txt"
  remote "sed -n 's/.*admin token file //p' /home/jack/dgx-netem-server.stderr.log | head -1" \
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
  local direction="$1" delay="$2" loss="$3" run="$4" phase="${5:-matrix}"
  case_order=$((case_order + 1))
  local order_fmt label case_dir netem_side rc parse sent recv retrans duration baseline delta base_file
  order_fmt="$(printf 'order-%04d' "$case_order")"
  label="${order_fmt}-${phase}-${direction}-$(printf '%03d' "$delay")ms-${loss}loss-run${run}"
  case_dir="$RESULT_ROOT/cases/$label"
  mkdir -p "$case_dir"
  log "case start $label"
  {
    echo "started_at=$(date -Iseconds)"
    echo "phase=$phase"
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
  sample_transport local "$case_dir/local-ss.txt" &
  local ltransport=$!
  sample_transport remote "$case_dir/remote-ss.txt" &
  local rtransport=$!
  : > "$case_dir/local-process.txt"
  : > "$case_dir/remote-process.txt"
  capture_case_process_sample local before "$case_dir/local-process.txt"
  capture_case_process_sample remote before "$case_dir/remote-process.txt"
  capture_relay_metrics local "$case_dir/local-relay-metrics.before.json"
  capture_relay_metrics remote "$case_dir/remote-relay-metrics.before.json"

  local attempt=1 attempt_dir iperf_pid retry_performed=0 blocking_failure=""
  local samplers_running=1
  while :; do
    set +e
    if [[ "$direction" == download ]]; then
      timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" \
        -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 --reverse \
        --proxy "http://127.0.0.1:${PROXY_PORT}" \
        > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt" &
    else
      timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" -B "$LOCAL_IP" -p "$IPERF_PORT" \
        -t "$IPERF_DURATION_SEC" --json --connect-timeout 5000 \
        --proxy "http://127.0.0.1:${PROXY_PORT}" \
        > "$case_dir/iperf.stdout.json" 2> "$case_dir/iperf.stderr.txt" &
    fi
    iperf_pid=$!
    wait "$iperf_pid"
    rc=$?
    set -e
    [[ "$rc" != 0 ]] || break
    (( attempt <= IPERF_CONTROL_RETRIES )) || break
    iperf_has_control_channel_error "$rc" \
      "$case_dir/iperf.stdout.json" "$case_dir/iperf.stderr.txt" || break
    attempt_dir="$case_dir/attempt-$attempt"
    if ! capture_converged_retry_metrics "$attempt_dir"; then
      blocking_failure=iperf_control_retry_health_gate_failed
      break
    fi
    archive_iperf_attempt "$case_dir" "$attempt_dir" "$rc"
    printf '%s\n' "iperf control transient: ${label} attempt=${attempt} rc=${rc}" \
      >> "$RESULT_ROOT/known-anomalies.txt"
    log "WARN $label: iperf control transient rc=$rc; restarting iperf3 and retrying"
    kill "$lsampler" "$rsampler" "$ltransport" "$rtransport" 2>/dev/null || true
    wait "$lsampler" "$rsampler" "$ltransport" "$rtransport" 2>/dev/null || true
    samplers_running=0
    capture_case_process_sample local after "$case_dir/local-process.txt" || true
    capture_case_process_sample remote after "$case_dir/remote-process.txt" || true
    tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-after.txt" 2>&1 || true
    remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-after.txt" 2>&1 || true
    archive_attempt_sampling "$case_dir" "$attempt_dir"
    if ! restart_remote_iperf_server > "$attempt_dir/remote-iperf3-restart.txt" 2>&1; then
      blocking_failure=iperf_server_restart_failed
      break
    fi
    tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-before.txt" 2>&1 || true
    remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-before.txt" 2>&1 || true
    : > "$case_dir/local-qdisc-samples.txt"
    : > "$case_dir/remote-qdisc-samples.txt"
    : > "$case_dir/local-ss.txt"
    : > "$case_dir/remote-ss.txt"
    : > "$case_dir/local-process.txt"
    : > "$case_dir/remote-process.txt"
    capture_case_process_sample local before "$case_dir/local-process.txt"
    capture_case_process_sample remote before "$case_dir/remote-process.txt"
    capture_relay_metrics local "$case_dir/local-relay-metrics.before.json"
    capture_relay_metrics remote "$case_dir/remote-relay-metrics.before.json"
    sample_qdisc local "$case_dir/local-qdisc-samples.txt" &
    lsampler=$!
    sample_qdisc remote "$case_dir/remote-qdisc-samples.txt" &
    rsampler=$!
    sample_transport local "$case_dir/local-ss.txt" &
    ltransport=$!
    sample_transport remote "$case_dir/remote-ss.txt" &
    rtransport=$!
    samplers_running=1
    retry_performed=1
    attempt=$((attempt + 1))
  done
  capture_case_process_sample local after "$case_dir/local-process.txt" || true
  capture_case_process_sample remote after "$case_dir/remote-process.txt" || true
  echo "$rc" > "$case_dir/iperf.rc"
  if [[ "$samplers_running" == 1 ]]; then
    kill "$lsampler" "$rsampler" "$ltransport" "$rtransport" 2>/dev/null || true
    wait "$lsampler" "$rsampler" "$ltransport" "$rtransport" 2>/dev/null || true
  fi

  tc -s qdisc show dev "$DATA_IFACE" > "$case_dir/local-qdisc-after.txt" 2>&1 || true
  remote "tc -s qdisc show dev '$DATA_IFACE'" > "$case_dir/remote-qdisc-after.txt" 2>&1 || true
  cp "$RESULT_ROOT/proxy/local-client.stderr.log" "$case_dir/local-proxy.case.log" 2>/dev/null || true
  remote 'cat /home/jack/dgx-netem-server.stderr.log' > "$case_dir/remote-proxy.case.log" 2>&1 || true
  if [[ "$rc" != 0 ]]; then
    if [[ -n "$blocking_failure" || "$retry_performed" == 1 ]]; then
      [[ -n "$blocking_failure" ]] || blocking_failure=iperf_control_retry_exhausted
      log "ABORT $label: $blocking_failure rc=$rc"
      collect_freeze "$case_dir" "$blocking_failure"
      return 96
    fi
    if [[ "$CONTINUE_ON_CASE_FAILURE" == 1 ]]; then
      if ! collect_case_evidence "$case_dir" "$netem_side" 1; then
        printf '{"evidence_status":"incomplete","evidence_missing":"collection_failed"}\n' \
          > "$case_dir/evidence.json"
      fi
      append_summary_row "$phase" "$case_order" "$direction" "$delay" "$loss" "$run" "$rc" \
        "" "" "" "" "" "" "${netem_side:-}" "$DATA_IFACE" "$NETEM_LIMIT" "iperf_rc_${rc}" \
        "$case_dir/evidence.json" || return $?
      echo "$phase,$case_order,$direction,$delay,$loss,$run" >> "$SCENARIO_CSV"
      printf '%s\n' "case failure: ${label} iperf_rc=${rc}" >> "$RESULT_ROOT/known-anomalies.txt"
      mkdir -p "$RESULT_ROOT/anomalies/$label"
      {
        echo "time=$(date -Iseconds)"
        echo "case_dir=$case_dir"
        echo "reason=iperf_rc_${rc}"
        echo "continued=true"
      } > "$RESULT_ROOT/anomalies/$label/reason.txt"
      log "WARN $label: iperf rc=$rc; recorded and continuing"
      return 0
    fi
    log "ABORT $label: iperf rc=$rc"
    collect_freeze "$case_dir" "iperf_rc_${rc}"
    return 96
  fi
  collect_case_evidence "$case_dir" "$netem_side" || {
    log "ABORT $label: case evidence is incomplete"
    collect_freeze "$case_dir" data_validity_evidence_incomplete
    return 91
  }
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
  baseline=""
  [[ "$phase" != matrix ]] || baseline="$(cat "$base_file" 2>/dev/null || true)"
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

  append_summary_row "$phase" "$case_order" "$direction" "$delay" "$loss" "$run" "$rc" \
    "$sent" "$recv" "$baseline" "$delta" "$retrans" "$duration" "$netem_side" \
    "$DATA_IFACE" "$NETEM_LIMIT" ok "$case_dir/evidence.json" || return $?
  echo "$phase,$case_order,$direction,$delay,$loss,$run" >> "$SCENARIO_CSV"
  log "case ok $label recv=${recv}Mbps sent=${sent}Mbps delta=${delta:-pending}%"
}

check_combo_variance() {
  local direction="$1" delay="$2" loss="$3"
  local result
  result="$(python3 - "$SUMMARY_CSV" "$direction" "$delay" "$loss" <<'PY'
import csv, statistics, sys
path, direction, delay, loss = sys.argv[1:5]
rows = [r for r in csv.DictReader(open(path, newline=""))
        if r.get("phase", "matrix") == "matrix" and r["direction"] == direction
        and r["delay_ms"] == delay and r["loss_pct"] == loss and r["notes"] == "ok"]
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
  mkdir -p "$RESULT_ROOT"/{proxy,cases,summary,anomalies}
  if [[ "$RESUME_MODE" == 1 ]]; then
    validate_resume_build_evidence
    validate_resume_state
    validate_resume_process_identity
    rm -f "$STOP_FILE"
    log "resume validated through global order $RESUME_AFTER_ORDER; original process identity preserved"
  else
    prepare_backend_evidence "$BACKEND" "$BUILD_DIR" "$RESULT_ROOT"
    if [[ "$SKIP_PREFLIGHT" == 1 ]]; then
      log "skip preflight; using caller-validated environment"
    else
      preflight
    fi
    if [[ "$SKIP_START_SERVICES" == 1 ]]; then
      log "skip start_services; reusing existing raypx2 and iperf3 processes"
    else
      start_services
    fi
    capture_allocator_snapshot before
    capture_process_image_snapshot before
    write_summary_header
    cat > "$SCENARIO_CSV" <<'EOF'
phase,order,direction,delay_ms,loss_pct,run
EOF
    case_order=0
    printf 'backend,event,timestamp\n%s,start,%s\n' "$BACKEND" "$(date -Iseconds)" \
      > "$RESULT_ROOT/execution-order.csv"
    record_execution_event start
    for direction in download upload; do
      run_case "$direction" 10 5 1 pre-anchor || exit $?
    done
  fi
  case_order=2
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
  for direction in download upload; do
    run_case "$direction" 10 5 1 post-anchor || exit $?
  done
  log "matrix cases complete; cleaning netem"
  sudo -n tc qdisc del dev "$DATA_IFACE" root 2>/dev/null || true
  remote "sudo -n tc qdisc del dev '$DATA_IFACE' root 2>/dev/null || true"
  capture_allocator_snapshot after
  capture_process_image_snapshot after
  seal_runtime_evidence_metadata
  printf '%s,end,%s\n' "$BACKEND" "$(date -Iseconds)" >> "$RESULT_ROOT/execution-order.csv"
  record_execution_event end
  write_summary_md
  log "summary written $RESULT_ROOT/summary.md"
}

if [[ -n "$COMPARE_ONLY_ROOT" ]]; then
  validate_result_evidence "$COMPARE_ONLY_ROOT"
  compare_results "$COMPARE_ONLY_ROOT"
  exit 0
fi

if [[ "$SEAL_BUILD" == 1 ]]; then
  python3 "$ROOT/scripts/dgx-netem-evidence.py" verify-build \
    --backend "$BACKEND" \
    --binary "$BUILD_DIR/bin/Release/raypx2" \
    --cmake-cache "$BUILD_DIR/CMakeCache.txt" \
    --manifest "$BUILD_DIR/bin/Release/raypx2.build-manifest.json" >/dev/null
  exit 0
fi

if [[ "$PREFLIGHT_ONLY" == 1 ]]; then
  mkdir -p "$EVAL_ROOT/native" "$EVAL_ROOT/libuv"
  prepare_backend_evidence native "$NATIVE_BUILD" "$EVAL_ROOT/native"
  prepare_backend_evidence libuv "$LIBUV_BUILD" "$EVAL_ROOT/libuv"
  python3 "$ROOT/scripts/dgx-netem-evidence.py" compare-builds \
    --native "$EVAL_ROOT/native/build-metadata.txt.build-manifest.json" \
    --libuv "$EVAL_ROOT/libuv/build-metadata.txt.build-manifest.json"
  python3 - "$NATIVE_BUILD/CMakeCache.txt" "$LIBUV_BUILD/CMakeCache.txt" <<'PY'
import pathlib, sys

def cache(path):
    values = {}
    for line in pathlib.Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        key_type, value = line.split("=", 1)
        values[key_type.split(":", 1)[0]] = value
    return values

native, libuv = map(cache, sys.argv[1:3])
required_equal = (
    "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_VERSION",
    "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_EXE_LINKER_FLAGS",
    "BUILD_SHARED_LIBS", "TCPQUIC_USE_MIMALLOC", "TCPQUIC_MSQUIC_USE_MIMALLOC",
    "TCPQUIC_SANITIZER", "QUIC_ENABLE_ASAN", "QUIC_ENABLE_ALL_SANITIZERS",
)
differences = [key for key in required_equal if native.get(key) != libuv.get(key)]
if native.get("TCPQUIC_RELAY_BACKEND") != "native":
    differences.append("native TCPQUIC_RELAY_BACKEND")
if libuv.get("TCPQUIC_RELAY_BACKEND") != "libuv":
    differences.append("libuv TCPQUIC_RELAY_BACKEND")
if differences:
    raise SystemExit("incompatible builds: " + ", ".join(differences))
PY
  printf 'DGX backend preflight metadata is consistent\n'
  exit 0
fi

if [[ "$DRY_RUN" == 1 ]]; then
  if [[ "$RESUME_MODE" != 1 ]]; then
    prepare_backend_evidence "$BACKEND" "$BUILD_DIR" "$RESULT_ROOT"
  fi
  plan_dry_run
  exit 0
fi

main
