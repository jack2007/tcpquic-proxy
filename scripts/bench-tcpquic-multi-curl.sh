#!/usr/bin/env bash
# 多 curl 并发吞吐评估：N 路并行 HTTP CONNECT 下载，对比 QUIC 连接池利用率
#
# 背景：单 curl + --quic-connections>1 时 conn=4 仅比 conn=1 高约 11%；
# 本脚本用 PARALLEL_CURL 路并行传输压满多 QUIC 连接/多 stream。
#
# 用法:
#   ./scripts/bench-tcpquic-multi-curl.sh
#   PARALLEL_CURL=8 QUIC_CONNECTIONS=4 ./scripts/bench-tcpquic-multi-curl.sh
#   LOCAL=1 PARALLEL_CURL=2 SIZE_MB=8 ./scripts/bench-tcpquic-multi-curl.sh
#   COMPRESS=zstd SKIP_APPEND=1 ./scripts/bench-tcpquic-multi-curl.sh
#
# 环境变量:
#   ROOT              msquic 源码根（默认脚本上级目录）
#   BIN               tcpquic-proxy 路径（默认 build/bin/Release/tcpquic-proxy）
#   LOCAL             1 时强制本机 127.0.0.1 冒烟（不 SSH）；未设置且 SSH 失败时自动降级
#   PARALLEL_CURL     并行 curl 数量（默认 4）
#   QUIC_CONNECTIONS  client --quic-connections（默认 4）
#   COMPRESS          隧道压缩：off|zstd（默认 off）
#   SIZE_MB           每路 curl 载荷大小 MB（默认 32）
#   PAYLOAD_KIND      载荷类型：repeat|zeros|text（默认 repeat）
#   WARMUP_MB         client 启动预热下载 MB/QUIC 连接（默认：DURATION_SEC=0 且 SIZE_MB<64 时为 2，否则 0）
#   DURATION_SEC      >0 时每路持续下载秒数（覆盖 SIZE_MB 单次下载语义）
#   METRIC_MODE       steady（DURATION_SEC>0，稳态吞吐）| average（默认）
#   NETEM             1 时对 PEER 出站网卡应用 tc netem（双机模式）
#   DELAY             netem 延迟（默认 100ms）
#   LOSS              netem 丢包（默认 0%）
#   NETEM_LIMIT       netem 队列深度（默认 1000000）
#   RATE              netem 带宽上限；NETEM=1 且未设置时默认 1gbit；RATE= 显式空值则不限速
#   IFACE             对端 netem 网卡；默认自动解析
#   PROFILE_CPU       1 时采样 client/server CPU% 与 RSS（默认 0）
#   ITERATIONS        重复轮次（默认 1）
#   TARGET            对端/HTTP 目标 IP（双机默认 169.254.59.196）
#   BIND              本机出站 bind IP（双机默认 169.254.250.230）
#   PEER              SSH 目标（默认 jack@${TARGET}）
#   REMOTE_BIN        对端二进制路径
#   HTTP_PORT         HTTP 目标端口（默认 16001）
#   QUIC_PORT         QUIC UDP 端口（默认 4433）
#   PROXY_PORT        本机 HTTP CONNECT 代理端口（默认 18080）
#   SKIP_APPEND       1 时不写入 research_progress.md
#   TUNING_20GBPS     1 时启用 docs/20gbps-paramenter.md 高 BDP 参数
#   HTTP_BACKEND      busybox|python（双机默认 python ThreadingHTTPServer）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
REMOTE_DIR="${REMOTE_DIR:-~/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-${REMOTE_DIR}/tcpquic-proxy}"
TARGET="${TARGET:-169.254.59.196}"
BIND="${BIND:-169.254.250.230}"
PEER="${PEER:-jack@${TARGET}}"
LOCAL="${LOCAL:-}"
PARALLEL_CURL="${PARALLEL_CURL:-4}"
QUIC_CONNECTIONS="${QUIC_CONNECTIONS:-4}"
COMPRESS="${COMPRESS:-off}"
SIZE_MB="${SIZE_MB:-32}"
PAYLOAD_KIND="${PAYLOAD_KIND:-repeat}"
DURATION_SEC="${DURATION_SEC:-0}"
METRIC_MODE="${METRIC_MODE:-average}"
NETEM="${NETEM:-0}"
DELAY="${DELAY:-100ms}"
LOSS="${LOSS:-0%}"
NETEM_LIMIT="${NETEM_LIMIT:-1000000}"
IFACE="${IFACE:-}"
PROFILE_CPU="${PROFILE_CPU:-0}"
ITERATIONS="${ITERATIONS:-1}"
HTTP_PORT="${HTTP_PORT:-16001}"
QUIC_PORT="${QUIC_PORT:-4433}"
PROXY_PORT="${PROXY_PORT:-18080}"
SKIP_APPEND="${SKIP_APPEND:-0}"
TUNING_20GBPS="${TUNING_20GBPS:-0}"
HTTP_BACKEND="${HTTP_BACKEND:-python}"
LOG_FILE="${ROOT}/research_progress.md"

TMP_DIR="/tmp/tcpquic-multi-curl-$$"
CERT_DIR="${TMP_DIR}/certs"
SPEED_DIR="${TMP_DIR}/speeds"

CLIENT_PID=""
HTTP_PID=""
SERVER_PID=""
REMOTE_SERVER_PID=""
SERVER_STDIN_FD=""
MODE="dual"

log() { printf '[tcpquic-multi-curl] %s\n' "$*" >&2; }

proxy_tuning_args() {
    local -a args=()
    if [[ "$TUNING_20GBPS" == "1" ]]; then
        args+=(
            --tuning custom
            --quic-fcw 1073741824
            --quic-srw 1073741824
            --quic-iw 4000
            --quic-initrtt-ms 200
            --relay-io-size 1048576
            --relay-inflight-bytes 1073741824
            --max-memory-mb 4096
        )
    fi
    printf '%s\n' "${args[@]}"
}

netem_tc_args() {
    local args
    if [[ -n "${LOSS}" && "${LOSS}" != "0" && "${LOSS}" != "0%" ]]; then
        args="delay ${DELAY} loss ${LOSS} limit ${NETEM_LIMIT}"
    else
        args="delay ${DELAY} limit ${NETEM_LIMIT}"
    fi
    if [[ -n "${RATE}" ]]; then
        args="${args} rate ${RATE}"
    fi
    echo "$args"
}

resolve_netem_iface() {
    [[ "$NETEM" == "1" && "$MODE" == "dual" ]] || return 0
    if [[ -n "$IFACE" ]]; then
        return 0
    fi
    IFACE=$(ssh "$PEER" "ip route get ${BIND} | awk '/ dev / {for (i=1; i<=NF; ++i) if (\$i == \"dev\") {print \$(i+1); exit}}'")
    if [[ -z "$IFACE" ]]; then
        log "failed to resolve peer egress interface toward ${BIND}; set IFACE explicitly"
        exit 1
    fi
    log "peer egress interface toward ${BIND}: ${IFACE}"
}

setup_netem() {
    [[ "$NETEM" == "1" && "$MODE" == "dual" ]] || return 0
    local args
    args=$(netem_tc_args)
    log "peer netem: ${args}"
    ssh "$PEER" "sudo tc qdisc replace dev ${IFACE} root netem ${args}"
}

cleanup_netem() {
    [[ "$NETEM" == "1" && "$MODE" == "dual" ]] || return 0
    ssh "$PEER" "sudo tc qdisc del dev ${IFACE} root 2>/dev/null || true"
}

verify_netem() {
    [[ "$NETEM" == "1" && "$MODE" == "dual" ]] || return 0
    ssh "$PEER" "tc qdisc show dev ${IFACE}" | grep -q "limit ${NETEM_LIMIT}" || {
        log "netem limit ${NETEM_LIMIT} not applied on ${IFACE}"
        ssh "$PEER" "tc qdisc show dev ${IFACE}" >&2 || true
        exit 1
    }
}

CPU_SAMPLER_PID=""
CPU_STATS_FILE=""

sample_ps_stats() {
    local pid=$1 role=$2 ts=$3
    [[ -n "$pid" ]] || return 0
    ps -p "$pid" -o %cpu=,rss= 2>/dev/null | awk -v ts="$ts" -v role="$role" '
        NF >= 2 {printf "%s\t%s\t%s\t%s\n", ts, role, $1, $2}'
}

start_cpu_sampler() {
    [[ "$PROFILE_CPU" == "1" ]] || return 0
    CPU_STATS_FILE="${TMP_DIR}/cpu_stats.tsv"
    : >"$CPU_STATS_FILE"
    (
        while true; do
            ts=$(date +%s)
            sample_ps_stats "${CLIENT_PID:-}" client "$ts"
            if [[ "$MODE" == "dual" && -n "${REMOTE_SERVER_PID:-}" ]]; then
                ssh "$PEER" "ps -p ${REMOTE_SERVER_PID} -o %cpu=,rss= 2>/dev/null" 2>/dev/null | \
                    awk -v ts="$ts" -v role=server 'NF >= 2 {printf "%s\t%s\t%s\t%s\n", ts, role, $1, $2}'
            else
                sample_ps_stats "${SERVER_PID:-}" server "$ts"
            fi
            sleep 0.5
        done
    ) >>"$CPU_STATS_FILE" 2>/dev/null &
    CPU_SAMPLER_PID=$!
}

stop_cpu_sampler() {
    if [[ -n "${CPU_SAMPLER_PID:-}" ]]; then
        kill "$CPU_SAMPLER_PID" 2>/dev/null || true
        wait "$CPU_SAMPLER_PID" 2>/dev/null || true
        CPU_SAMPLER_PID=""
    fi
}

summarize_cpu_stats() {
    [[ -f "${CPU_STATS_FILE:-}" ]] || { echo "n/a,n/a,n/a,n/a"; return 0; }
    python3 - "$CPU_STATS_FILE" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
rows = []
for line in path.read_text().splitlines():
    parts = line.split("\t")
    if len(parts) < 4:
        continue
    role, cpu, rss = parts[1], float(parts[2]), float(parts[3])
    rows.append((role, cpu, rss))
if not rows:
    print("n/a,n/a,n/a,n/a")
    raise SystemExit
by_role = {}
for role, cpu, rss in rows:
    by_role.setdefault(role, {"cpu": [], "rss": []})
    by_role[role]["cpu"].append(cpu)
    by_role[role]["rss"].append(rss)
def avg(vals):
    return sum(vals) / len(vals) if vals else 0.0
def fmt_avg(vals, suffix=""):
    if not vals:
        return "n/a"
    return f"{avg(vals):.1f}{suffix}"

client_cpu = fmt_avg(by_role.get("client", {}).get("cpu", []))
server_cpu = fmt_avg(by_role.get("server", {}).get("cpu", []))
client_rss = fmt_avg(by_role.get("client", {}).get("rss", []))
server_rss = fmt_avg(by_role.get("server", {}).get("rss", []))
print(f"{client_cpu},{server_cpu},{client_rss},{server_rss}")
PY
}

bytes_to_mbps() {
    python3 - "$1" <<'PY'
import sys
bps = float(sys.argv[1])
print(f"{bps * 8 / 1_000_000:.2f}")
PY
}

sum_bytes() {
    python3 - "$@" <<'PY'
import sys
vals = [float(v) for v in sys.argv[1:] if v]
print(f"{sum(vals):.0f}" if vals else "0")
PY
}

avg_bytes() {
    python3 - "$@" <<'PY'
import sys
vals = [float(v) for v in sys.argv[1:] if v]
print(f"{sum(vals)/len(vals):.0f}" if vals else "0")
PY
}

require_cmd() {
    command -v "$1" >/dev/null || { log "missing: $1"; exit 1; }
}

ssh_ok() {
    ssh -o ConnectTimeout=3 -o BatchMode=yes "$PEER" "true" >/dev/null 2>&1
}

resolve_mode() {
    if [[ "$LOCAL" == "1" ]]; then
        MODE="local"
        return 0
    fi
    if ssh_ok; then
        MODE="dual"
        return 0
    fi
    log "SSH to ${PEER} unavailable; falling back to LOCAL=1 on 127.0.0.1"
    MODE="local"
}

remote_kill() {
    if [[ "$MODE" == "local" ]]; then
        pkill -9 -f "${BIN}" 2>/dev/null || true
        return 0
    fi
    ssh "$PEER" "killall -9 tcpquic-proxy 2>/dev/null; fuser -k ${HTTP_PORT}/tcp >/dev/null 2>&1; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
}

ensure_remote_bin() {
    [[ "$MODE" == "dual" ]] || return 0
    if ssh "$PEER" "test -x ${REMOTE_BIN}"; then
        return 0
    fi
    log "deploying ${BIN} -> ${PEER}:${REMOTE_DIR}"
    ssh "$PEER" "mkdir -p ${REMOTE_DIR}"
    rsync -az "$BIN" "${ROOT}/build/bin/Release/libmsquic.so.2" \
        "${ROOT}/build/bin/Release/libmsquic.so.2.6.0" \
        "${PEER}:${REMOTE_DIR}/"
    ssh "$PEER" "chmod +x ${REMOTE_BIN}"
}

generate_certs() {
    local san_ip="${TARGET}"
    [[ "$MODE" == "local" ]] && san_ip="127.0.0.1"
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=tcpquic-multi-curl-ca" -days 1 -sha256 >/dev/null 2>&1
    cat > "$CERT_DIR/server.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:${san_ip}
EOF
    cat > "$CERT_DIR/client.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:tcpquic-multi-curl-client
EOF
    for n in server client; do
        openssl req -newkey rsa:2048 -nodes \
            -keyout "$CERT_DIR/$n.key" -out "$CERT_DIR/$n.csr" \
            -subj "/CN=$n" >/dev/null 2>&1
        openssl x509 -req -in "$CERT_DIR/$n.csr" \
            -CA "$CERT_DIR/ca.crt" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
            -out "$CERT_DIR/$n.crt" -days 1 -sha256 \
            -extfile "$CERT_DIR/$n.ext" >/dev/null 2>&1
    done
    if [[ "$MODE" == "dual" ]]; then
        rsync -az "$CERT_DIR/" "${PEER}:~/tcpquic-dgx-certs/"
    fi
}

deploy_payload() {
    case "$PAYLOAD_KIND" in
        repeat|zeros|text) ;;
        *)
            log "unknown PAYLOAD_KIND=${PAYLOAD_KIND}; use repeat|zeros|text"
            exit 1
            ;;
    esac
    python3 - "$TMP_DIR/payload.bin" "$SIZE_MB" "$PAYLOAD_KIND" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
mb = int(sys.argv[2])
kind = sys.argv[3]
if kind == "repeat":
    chunk = b"x" * (1024 * 1024)
elif kind == "zeros":
    chunk = b"\x00" * (1024 * 1024)
elif kind == "text":
    pat = "compress-me-"
    pat_b = pat.encode("utf-8")
    need = 1024 * 1024
    reps = need // len(pat_b)
    chunk = (pat * reps + pat[: (need - reps * len(pat_b))]).encode("utf-8")
    assert len(chunk) == need
else:
    raise SystemExit(f"unknown PAYLOAD_KIND={kind}")
with path.open("wb") as f:
    for _ in range(mb):
        f.write(chunk)
PY
    if [[ "$MODE" == "dual" ]]; then
        rsync -az "$TMP_DIR/payload.bin" "${PEER}:~/tcpquic-dgx-payload.bin"
    else
        mkdir -p "$TMP_DIR/http-root"
        cp "$TMP_DIR/payload.bin" "$TMP_DIR/http-root/tcpquic-dgx-payload.bin"
    fi
}

wait_log() {
    local file=$1 pattern=$2 name=$3
    local deadline=$((SECONDS + 30))
    while [[ "$SECONDS" -lt "$deadline" ]]; do
        if grep -q "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    log "timeout waiting for $name"
    tail -20 "$file" >&2 || true
    return 1
}

start_http_target() {
    if [[ "$MODE" == "dual" ]]; then
        if [[ "$HTTP_BACKEND" == "busybox" ]]; then
            ssh "$PEER" "fuser -k ${HTTP_PORT}/tcp 2>/dev/null || true"
            ssh "$PEER" "nohup busybox httpd -f -p ${TARGET}:${HTTP_PORT} -h /home/jack >/tmp/tcpquic-dgx-http.log 2>&1 & echo \$! > /tmp/tcpquic-dgx-http.pid"
        else
            ssh "$PEER" "python3 - ${HTTP_PORT} ${TARGET} <<'PY' >/tmp/tcpquic-dgx-http.log 2>&1 & echo \$! > /tmp/tcpquic-dgx-http.pid
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1])
bind = sys.argv[2]
ThreadingHTTPServer((bind, port), SimpleHTTPRequestHandler).serve_forever()
PY"
        fi
        sleep 1
        return 0
    fi
    mkdir -p "$TMP_DIR/http-root"
    python3 - "$TMP_DIR/http-root" "$HTTP_PORT" <<'PY' >"$TMP_DIR/http.log" 2>&1 &
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

root = sys.argv[1]
port = int(sys.argv[2])
os.chdir(root)
server = ThreadingHTTPServer(("127.0.0.1", port), SimpleHTTPRequestHandler)
server.serve_forever()
PY
    HTTP_PID=$!
    sleep 0.5
}

start_server() {
    local -a tuning_args=()
    mapfile -t tuning_args < <(proxy_tuning_args)
    if [[ "$MODE" == "dual" ]]; then
        ssh "$PEER" "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
            --quic-listen ${TARGET}:${QUIC_PORT} \
            --allow-targets ${TARGET}/32,127.0.0.0/8 \
            --quic-cert ~/tcpquic-dgx-certs/server.crt \
            --quic-key ~/tcpquic-dgx-certs/server.key \
            --quic-ca ~/tcpquic-dgx-certs/ca.crt \
            --compress ${COMPRESS} \
            ${tuning_args[*]} \
            </dev/null >/tmp/tcpquic-dgx-server.log 2>&1 & echo \$! > /tmp/tcpquic-dgx-server.pid"
        REMOTE_SERVER_PID=$(ssh "$PEER" "cat /tmp/tcpquic-dgx-server.pid 2>/dev/null || true")
        for _ in $(seq 1 20); do
            sleep 0.5
            if ssh "$PEER" "grep -q 'QUIC server listening' /tmp/tcpquic-dgx-server.log 2>/dev/null"; then
                return 0
            fi
        done
        log "remote server failed"
        ssh "$PEER" "tail -20 /tmp/tcpquic-dgx-server.log" >&2 || true
        return 1
    fi
    mkfifo "$TMP_DIR/server.stdin"
    eval "exec {SERVER_STDIN_FD}<>\"$TMP_DIR/server.stdin\""
    "$BIN" server \
        --quic-listen "127.0.0.1:${QUIC_PORT}" \
        --allow-targets 127.0.0.0/8 \
        --quic-cert "$CERT_DIR/server.crt" \
        --quic-key "$CERT_DIR/server.key" \
        --quic-ca "$CERT_DIR/ca.crt" \
        --compress "$COMPRESS" \
        >"$TMP_DIR/server.log" 2>&1 <"$TMP_DIR/server.stdin" &
    SERVER_PID=$!
    wait_log "$TMP_DIR/server.log" "QUIC server listening" "server"
}

start_client() {
    local quic_peer http_host bind_iface curl_bind
    local -a warmup_args=()
    if [[ "$MODE" == "dual" ]]; then
        quic_peer="${TARGET}:${QUIC_PORT}"
        http_host="${TARGET}"
        curl_bind=(--interface "$BIND")
    else
        quic_peer="127.0.0.1:${QUIC_PORT}"
        http_host="127.0.0.1"
        curl_bind=()
    fi
    pkill -9 -f "${BIN} client" 2>/dev/null || true
    sleep 0.3
    if [[ "${WARMUP_MB}" -gt 0 ]]; then
        warmup_args=(
            --warmup-mb "${WARMUP_MB}"
            --warmup-target "${http_host}:${HTTP_PORT}"
            --warmup-path /tcpquic-dgx-payload.bin
        )
        log "warmup enabled: ${WARMUP_MB} MB/conn -> ${http_host}:${HTTP_PORT}/tcpquic-dgx-payload.bin"
    fi
    local -a tuning_args=()
    mapfile -t tuning_args < <(proxy_tuning_args)
    "$BIN" client \
        --quic-peer "$quic_peer" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
        --quic-cert "$CERT_DIR/client.crt" \
        --quic-key "$CERT_DIR/client.key" \
        --quic-ca "$CERT_DIR/ca.crt" \
        --quic-connections "$QUIC_CONNECTIONS" \
        --compress "$COMPRESS" \
        "${tuning_args[@]}" \
        "${warmup_args[@]}" \
        >"$TMP_DIR/client.log" 2>&1 &
    CLIENT_PID=$!
    wait_log "$TMP_DIR/client.log" "HTTP CONNECT listening" "client"

    # Export for measure_multi_curl
    MEASURE_HTTP_HOST="$http_host"
    MEASURE_CURL_BIND=("${curl_bind[@]}")
}

stop_client() {
    if [[ -n "${CLIENT_PID:-}" ]]; then
        kill -9 "$CLIENT_PID" 2>/dev/null || true
        wait "$CLIENT_PID" 2>/dev/null || true
        CLIENT_PID=""
    fi
}

one_curl() {
    local idx=$1 out=$2 rc_out=$3
    local url max_time_arg=()
    if [[ "${DURATION_SEC}" -gt 0 ]]; then
        max_time_arg=(--max-time "${DURATION_SEC}")
    else
        max_time_arg=(--max-time 180)
    fi
    url="http://${MEASURE_HTTP_HOST}:${HTTP_PORT}/tcpquic-dgx-payload.bin"
    local speed rc
    set +e
    speed=$(curl -fsS "${MEASURE_CURL_BIND[@]}" \
        -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
        "$url" -o /dev/null "${max_time_arg[@]}" -w '%{speed_download}')
    rc=$?
    set -e
    if [[ -z "$speed" ]]; then
        speed=0
    fi
    echo "$speed" >"$out"
    echo "$rc" >"$rc_out"
}

measure_multi_curl() {
    local i
    local -a pids=()
    mkdir -p "$SPEED_DIR"
    rm -f "$SPEED_DIR"/*

    log "starting ${PARALLEL_CURL} parallel curls (quic-connections=${QUIC_CONNECTIONS}, compress=${COMPRESS})"
    local wall_start wall_end wall_ms
    wall_start=$(date +%s%3N)
    start_cpu_sampler
    for i in $(seq 1 "$PARALLEL_CURL"); do
        one_curl "$i" "$SPEED_DIR/curl_${i}.speed" "$SPEED_DIR/curl_${i}.rc" &
        pids+=($!)
    done
    for pid in "${pids[@]}"; do
        wait "$pid" || true
    done
    stop_cpu_sampler
    wall_end=$(date +%s%3N)
    wall_ms=$((wall_end - wall_start))

    local -a speeds=() curl_rcs=()
    local total=0
    for i in $(seq 1 "$PARALLEL_CURL"); do
        local bps mbps rc
        bps=$(cat "$SPEED_DIR/curl_${i}.speed" 2>/dev/null || echo 0)
        rc=$(cat "$SPEED_DIR/curl_${i}.rc" 2>/dev/null || echo 1)
        speeds+=("$bps")
        curl_rcs+=("$rc")
        mbps=$(bytes_to_mbps "$bps")
        log "  curl[${i}]: ${mbps} Mbps (exit ${rc})"
        total=$(python3 - "$total" "$bps" <<'PY'
import sys
print(float(sys.argv[1]) + float(sys.argv[2]))
PY
)
    done
    local agg_mbps cpu_summary client_cpu server_cpu client_rss server_rss
    agg_mbps=$(bytes_to_mbps "$total")
    cpu_summary=$(summarize_cpu_stats)
    client_cpu=$(echo "$cpu_summary" | cut -d, -f1)
    server_cpu=$(echo "$cpu_summary" | cut -d, -f2)
    client_rss=$(echo "$cpu_summary" | cut -d, -f3)
    server_rss=$(echo "$cpu_summary" | cut -d, -f4)
    log "  aggregate: ${agg_mbps} Mbps (sum of per-curl speeds)"
    log "  wall time: ${wall_ms} ms"
    if [[ "$PROFILE_CPU" == "1" ]]; then
        log "  CPU avg: client=${client_cpu}% server=${server_cpu}% RSS client=${client_rss}KB server=${server_rss}KB"
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$total" "$agg_mbps" "$wall_ms" "$(IFS=,; echo "${curl_rcs[*]}")" \
        "$client_cpu" "$server_cpu" "$client_rss" "$server_rss"
}

append_log() {
    local per_curl_table="$1" agg_mbps="$2" wall_ms="$3" curl_rcs="$4"
    local client_cpu="$5" server_cpu="$6" client_rss="$7" server_rss="$8"
    local netem_label
    if [[ "$NETEM" == "1" && "$MODE" == "dual" ]]; then
        netem_label="${DELAY}"
        [[ -n "$LOSS" && "$LOSS" != "0" && "$LOSS" != "0%" ]] && netem_label="${netem_label} + ${LOSS} loss"
        [[ -n "${RATE}" ]] && netem_label="${netem_label}, rate ${RATE}"
    elif [[ "$MODE" == "dual" ]]; then
        netem_label="200G 直连 (无 netem)"
    else
        netem_label="127.0.0.1 本机"
    fi
    {
        echo ""
        echo "## tcpquic-proxy 多 curl 并发压测 ($(date -Iseconds))"
        echo ""
        echo "| 项目 | 值 |"
        echo "|------|-----|"
        echo "| 模式 | ${MODE} |"
        echo "| 并行 curl | ${PARALLEL_CURL} |"
        echo "| QUIC 连接池 | ${QUIC_CONNECTIONS} |"
        echo "| 压缩 | ${COMPRESS} |"
        echo "| 载荷 | ${PAYLOAD_KIND} ${SIZE_MB} MB/路$([ "${DURATION_SEC}" -gt 0 ] && echo ", 持续 ${DURATION_SEC}s") |"
        echo "| 指标口径 | ${METRIC_MODE} |"
        echo "| 网络 | ${netem_label} |"
        echo "| 轮次 | ${ITERATIONS} |"
        if [[ "$MODE" == "dual" ]]; then
            echo "| 服务端 | ${TARGET} |"
            echo "| 客户端 | ${BIND} |"
            if [[ "$NETEM" == "1" ]]; then
                echo "| netem interface | ${IFACE} |"
                echo "| netem limit | ${NETEM_LIMIT} |"
            fi
        else
            echo "| 拓扑 | 127.0.0.1 本机 |"
        fi
        if [[ "$PROFILE_CPU" == "1" ]]; then
            echo "| CPU avg (client/server) | ${client_cpu}% / ${server_cpu}% |"
            echo "| RSS avg (client/server) | ${client_rss} KB / ${server_rss} KB |"
        fi
        echo ""
        echo "### 每路吞吐"
        echo ""
        echo "$per_curl_table"
        echo ""
        echo "| 聚合吞吐 | ${agg_mbps} Mbps |"
        echo "| 墙钟时间 | ${wall_ms} ms |"
        if [[ -n "$curl_rcs" && "$curl_rcs" != "0" && "$curl_rcs" != "0,0" && "$curl_rcs" != "0,0,0,0" ]]; then
            echo "| curl exit | ${curl_rcs} |"
        fi
        echo ""
    } >> "$LOG_FILE"
}

cleanup() {
    local status=$?
    set +e
    stop_cpu_sampler
    remote_kill
    cleanup_netem
    stop_client
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "${HTTP_PID:-}" ]]; then
        kill -9 "$HTTP_PID" 2>/dev/null || true
        wait "$HTTP_PID" 2>/dev/null || true
    fi
    if [[ -n "${SERVER_STDIN_FD:-}" ]]; then
        eval "exec ${SERVER_STDIN_FD}>&-"
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

require_cmd curl
require_cmd openssl
require_cmd python3
[[ -x "$BIN" ]] || { log "missing binary: $BIN"; exit 1; }

resolve_mode
if [[ -z "${RATE+x}" && "$NETEM" == "1" && "$MODE" == "dual" ]]; then
    RATE="1gbit"
    log "NETEM=1: default RATE=1gbit (set RATE= to disable, or RATE=<value> to override)"
fi
if [[ "${METRIC_MODE}" == "steady" && "${DURATION_SEC}" -eq 0 ]]; then
    DURATION_SEC=10
    log "METRIC_MODE=steady: default DURATION_SEC=10"
fi
if [[ "${DURATION_SEC}" -gt 0 && "$PAYLOAD_KIND" == "repeat" && "${PARALLEL_CURL}" -eq 1 ]]; then
    min_mb=$((DURATION_SEC * 150))
    if [[ "${SIZE_MB}" -lt "${min_mb}" ]]; then
        log "DURATION_SEC=${DURATION_SEC}: bump payload ${SIZE_MB}MB -> ${min_mb}MB (sustained download)"
        SIZE_MB=$min_mb
    fi
fi
if [[ -z "${WARMUP_MB+x}" ]]; then
    if [[ "${DURATION_SEC}" -eq 0 && "${SIZE_MB}" -lt 64 ]]; then
        WARMUP_MB=2
    else
        WARMUP_MB=0
    fi
fi
log "mode=${MODE} parallel_curl=${PARALLEL_CURL} quic_connections=${QUIC_CONNECTIONS} netem=${NETEM}"

mkdir -p "$TMP_DIR" "$SPEED_DIR"
if [[ "$MODE" == "dual" ]]; then
    require_cmd ssh
    require_cmd rsync
    ensure_remote_bin
    resolve_netem_iface
    setup_netem
    verify_netem
fi

remote_kill
generate_certs
deploy_payload
start_http_target
start_server
start_client

declare -a ITER_AGG=()
declare -a ITER_WALL=()
LAST_CURL_RCS=""
LAST_CLIENT_CPU="n/a"
LAST_SERVER_CPU="n/a"
LAST_CLIENT_RSS="n/a"
LAST_SERVER_RSS="n/a"
PER_CURL_TABLE="| curl | Mbps | exit |"
PER_CURL_TABLE+=$'\n|------|------|------|'

for iter in $(seq 1 "$ITERATIONS"); do
    [[ "$ITERATIONS" -gt 1 ]] && log "iteration ${iter}/${ITERATIONS}"
    line=$(measure_multi_curl)
    agg_bps=$(echo "$line" | cut -d, -f1)
    agg_mbps=$(echo "$line" | cut -d, -f2)
    wall_ms=$(echo "$line" | cut -d, -f3)
    LAST_CURL_RCS=$(echo "$line" | cut -d, -f4)
    LAST_CLIENT_CPU=$(echo "$line" | cut -d, -f5)
    LAST_SERVER_CPU=$(echo "$line" | cut -d, -f6)
    LAST_CLIENT_RSS=$(echo "$line" | cut -d, -f7)
    LAST_SERVER_RSS=$(echo "$line" | cut -d, -f8)
    ITER_AGG+=("$agg_bps")
    ITER_WALL+=("$wall_ms")
done

# Build per-curl table from last iteration
for i in $(seq 1 "$PARALLEL_CURL"); do
    bps=$(cat "$SPEED_DIR/curl_${i}.speed" 2>/dev/null || echo 0)
    rc=$(cat "$SPEED_DIR/curl_${i}.rc" 2>/dev/null || echo 1)
    mbps=$(bytes_to_mbps "$bps")
    PER_CURL_TABLE+=$'\n'"| ${i} | ${mbps} | ${rc} |"
done

avg_agg_bps=$(avg_bytes "${ITER_AGG[@]}")
avg_agg_mbps=$(bytes_to_mbps "$avg_agg_bps")
avg_wall=$(avg_bytes "${ITER_WALL[@]}")

echo ""
echo "=== tcpquic multi-curl (${MODE}) ==="
echo "parallel_curl=${PARALLEL_CURL} quic_connections=${QUIC_CONNECTIONS} compress=${COMPRESS}"
echo "per-curl (last run):"
for i in $(seq 1 "$PARALLEL_CURL"); do
    bps=$(cat "$SPEED_DIR/curl_${i}.speed" 2>/dev/null || echo 0)
    echo "  curl[${i}]: $(bytes_to_mbps "$bps") Mbps"
done
echo "aggregate: ${avg_agg_mbps} Mbps (avg over ${ITERATIONS} run(s))"
echo "wall time: ${avg_wall} ms (avg)"

if [[ "$SKIP_APPEND" != "1" ]]; then
    append_log "$PER_CURL_TABLE" "$avg_agg_mbps" "$avg_wall" "$LAST_CURL_RCS" \
        "$LAST_CLIENT_CPU" "$LAST_SERVER_CPU" "$LAST_CLIENT_RSS" "$LAST_SERVER_RSS"
    log "appended results to ${LOG_FILE}"
fi

log "done"
