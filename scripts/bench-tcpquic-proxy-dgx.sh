#!/usr/bin/env bash
# 双机 DGX tcpquic-proxy 压测：裸 TCP vs QUIC 隧道（off/zstd），可选对端 netem
#
# 拓扑（默认 spark 200G 直连）:
#   对端 spark-1b6f: 169.254.59.196 — HTTP 目标 + tcpquic-proxy server
#   本机 spark-1619: 169.254.250.230 — tcpquic-proxy client + curl
#
# 用法:
#   ./scripts/bench-tcpquic-proxy-dgx.sh
#   NETEM=1 DELAY=100ms LOSS=5% SIZE_MB=32 ./scripts/bench-tcpquic-proxy-dgx.sh
#   MODES="direct tunnel_off tunnel_zstd" ./scripts/bench-tcpquic-proxy-dgx.sh
#
# 环境变量:
#   ROOT          msquic 源码根（默认脚本上级目录）
#   BIN           本机 tcpquic-proxy 路径
#   REMOTE_BIN    对端二进制（默认同 BIN 路径相对 ~/src/msquic）
#   TARGET        对端 IP（HTTP + QUIC server）
#   BIND          本机出站 bind IP
#   PEER          SSH 目标（user@ip）
#   IFACE         对端 netem 网卡；默认自动取 PEER 到 BIND 的出站 dev（tc 作用于发送方向）
#   NETEM         1 时对 PEER 的 IFACE 应用 tc netem
#   NETEM_LIMIT     netem 队列深度（默认 1000000；默认 1000 会导致 UDP/QUIC 假性大量丢包）
NETEM_LIMIT="${NETEM_LIMIT:-1000000}"
#   SIZE_MB       载荷大小（默认 32）
#   PAYLOAD_KIND  载荷类型（默认 repeat）：
#                   repeat — 每 MB 为 b"x"*1MB（历史默认，近似不可压缩）
#                   zeros  — 每 MB 为 b"\x00"*1MB（高可压缩）
#                   text   — 每 MB 为 "compress-me-" 重复填充至 1 MB（UTF-8，高可压缩）
#   WARMUP_MB     client 启动预热下载 MB/QUIC 连接（默认：DURATION_SEC=0 且 SIZE_MB<64 时为 2，否则 0）
#   DURATION_SEC  若 >0，在 tunnel/direct 模式下循环下载直至达到该秒数（与 secnetperf -down:10s 对齐）
#   METRIC_MODE   指标口径：steady（DURATION_SEC>0，持续下载稳态吞吐）| average（固定 SIZE_MB 整段平均，默认）
#   RATE          netem 带宽上限，如 1gbit（需对端 sudo tc）；NETEM=1 且未设置时默认 1gbit；RATE= 显式空值则不限速
#   ITERATIONS    每模式重复次数（默认 2）
#   QUIC_CONNECTIONS  client 侧 QUIC 连接池（默认 4）
#   TUNING_20GBPS     1 时启用 docs/20gbps-paramenter.md 推荐单连接参数
#   EXTRA_PROXY_ARGS  追加到 client/server 的额外 CLI 参数
#   HTTP_PORT     对端 HTTP 目标端口（默认 16001）
#   QUIC_PORT     对端 QUIC UDP 端口（默认 4433）
#   PROXY_PORT    本机 HTTP CONNECT 代理端口（默认 18080）
#   CLIENT_QUIC_READY_WAIT_SEC
#                  等待本地 client QUIC 连接建立秒数（默认 60）
#   CLIENT_HTTP_READY_WAIT_SEC
#                  QUIC 连接建立后等待 HTTP CONNECT 监听秒数（默认 15）
#   SKIP_APPEND   1 时不写入 research_progress.md
#
# sudoers 示例（对端 netem 免密）:
#   jack ALL=(ALL) NOPASSWD: /sbin/tc
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
REMOTE_DIR="${REMOTE_DIR:-~/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-${REMOTE_DIR}/tcpquic-proxy}"
TARGET="${TARGET:-169.254.59.196}"
BIND="${BIND:-169.254.250.230}"
PEER="${PEER:-jack@${TARGET}}"
IFACE="${IFACE:-}"
NETEM="${NETEM:-0}"
DELAY="${DELAY:-100ms}"
LOSS="${LOSS:-5%}"
SIZE_MB="${SIZE_MB:-32}"
PAYLOAD_KIND="${PAYLOAD_KIND:-repeat}"
DURATION_SEC="${DURATION_SEC:-0}"
ITERATIONS="${ITERATIONS:-2}"
QUIC_CONNECTIONS="${QUIC_CONNECTIONS:-4}"
TUNING_20GBPS="${TUNING_20GBPS:-0}"
EXTRA_PROXY_ARGS="${EXTRA_PROXY_ARGS:-}"
HTTP_PORT="${HTTP_PORT:-16001}"
QUIC_PORT="${QUIC_PORT:-4433}"
PROXY_PORT="${PROXY_PORT:-18080}"
CLIENT_QUIC_READY_WAIT_SEC="${CLIENT_QUIC_READY_WAIT_SEC:-60}"
CLIENT_HTTP_READY_WAIT_SEC="${CLIENT_HTTP_READY_WAIT_SEC:-15}"
SKIP_APPEND="${SKIP_APPEND:-0}"
METRIC_MODE="${METRIC_MODE:-average}"  # steady | average
# RATE: unset vs empty distinguished — default applied in main when NETEM=1
MODES="${MODES:-direct tunnel_off tunnel_zstd}"
LOG_FILE="${ROOT}/research_progress.md"

TMP_DIR="/tmp/tcpquic-dgx-bench-$$"
CERT_DIR="${TMP_DIR}/certs"

log() { printf '[tcpquic-dgx-bench] %s\n' "$*" >&2; }

proxy_tuning_args() {
    local -a args=()
    if [[ "$TUNING_20GBPS" == "1" ]]; then
        args+=(
            --tuning wan
            --iw 4000
            --initrtt-ms 100
            --relay-io-size 1048576
        )
    fi
    if [[ -n "$EXTRA_PROXY_ARGS" ]]; then
        # shellcheck disable=SC2206
        args+=($EXTRA_PROXY_ARGS)
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
    [[ "$NETEM" == "1" ]] || return 0
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

verify_netem() {
    [[ "$NETEM" == "1" ]] || return 0
    ssh "$PEER" "tc qdisc show dev ${IFACE}" | grep -q "limit ${NETEM_LIMIT}" || {
        log "netem limit ${NETEM_LIMIT} not applied on ${IFACE}"
        ssh "$PEER" "tc qdisc show dev ${IFACE}" >&2 || true
        exit 1
    }
    local dropped
    dropped=$(ssh "$PEER" "tc -s qdisc show dev ${IFACE}" | awk '/netem/ {f=1} f && /dropped/ {print $2; exit}')
    log "netem qdisc dropped=${dropped:-0} (limit=${NETEM_LIMIT})"
}

setup_netem() {
    [[ "$NETEM" == "1" ]] || return 0
    local args
    args=$(netem_tc_args)
    log "peer netem: ${args}"
    ssh "$PEER" "sudo tc qdisc replace dev ${IFACE} root netem ${args}"
}

cleanup_netem() {
    [[ "$NETEM" == "1" ]] || return 0
    ssh "$PEER" "sudo tc qdisc del dev ${IFACE} root 2>/dev/null || true"
}

remote_kill() {
    local remote_name
    remote_name="$(basename "${REMOTE_BIN}")"
    ssh "$PEER" "killall -9 tcpquic-proxy '${remote_name}' 2>/dev/null || true; pkill -9 -f '${REMOTE_BIN}' 2>/dev/null || true; fuser -k ${HTTP_PORT}/tcp >/dev/null 2>&1; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
}

remote_kill_proxy() {
    local remote_name
    remote_name="$(basename "${REMOTE_BIN}")"
    ssh "$PEER" "killall -9 tcpquic-proxy '${remote_name}' 2>/dev/null || true; pkill -9 -f '${REMOTE_BIN}' 2>/dev/null || true; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
}

ensure_remote_bin() {
    if ssh "$PEER" "test -x ${REMOTE_BIN}"; then
        return 0
    fi
    log "deploying ${BIN} -> ${PEER}:${REMOTE_DIR}"
    ssh "$PEER" "mkdir -p ${REMOTE_DIR}"
    rsync -az "$BIN" "${PEER}:${REMOTE_DIR}/"
    ssh "$PEER" "chmod +x ${REMOTE_BIN}"
}

cleanup() {
    local status=$?
    set +e
    remote_kill
    cleanup_netem
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

require_cmd() {
    command -v "$1" >/dev/null || { log "missing: $1"; exit 1; }
}

bytes_to_mbps() {
    python3 - "$1" <<'PY'
import sys
bps = float(sys.argv[1])
print(f"{bps * 8 / 1_000_000:.2f}")
PY
}

avg_bytes() {
    python3 - "$@" <<'PY'
import sys
vals = [float(v) for v in sys.argv[1:] if v]
print(f"{sum(vals)/len(vals):.0f}" if vals else "0")
PY
}

generate_certs() {
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=tcpquic-dgx-ca" -days 1 -sha256 >/dev/null 2>&1
    cat > "$CERT_DIR/server.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:169.254.59.196
EOF
    cat > "$CERT_DIR/client.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:tcpquic-dgx-client
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
    rsync -az "$CERT_DIR/" "${PEER}:~/tcpquic-dgx-certs/"
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
    rsync -az "$TMP_DIR/payload.bin" "${PEER}:~/tcpquic-dgx-payload.bin"
}

start_remote_http() {
    ssh "$PEER" "python3 - ${HTTP_PORT} ${TARGET} <<'PY' >/tmp/tcpquic-dgx-http.log 2>&1 & echo \$! > /tmp/tcpquic-dgx-http.pid
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1])
bind = sys.argv[2]
ThreadingHTTPServer((bind, port), SimpleHTTPRequestHandler).serve_forever()
PY"
    sleep 1
}

start_remote_server() {
    local compress=$1
    local -a tuning_args=()
    mapfile -t tuning_args < <(proxy_tuning_args)
    ssh "$PEER" "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
        --listen ${TARGET}:${QUIC_PORT} \
        --allow-targets ${TARGET}/32,127.0.0.0/8 \
        --cert ~/tcpquic-dgx-certs/server.crt \
        --key ~/tcpquic-dgx-certs/server.key \
        --ca ~/tcpquic-dgx-certs/ca.crt \
        --compress ${compress} \
        ${tuning_args[*]} \
        </dev/null >/tmp/tcpquic-dgx-server.log 2>&1 & echo \$! > /tmp/tcpquic-dgx-server.pid"
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.5
        if ssh "$PEER" "grep -q 'QUIC server listening' /tmp/tcpquic-dgx-server.log 2>/dev/null"; then
            return 0
        fi
    done
    log "remote server failed to start (compress=${compress})"
    ssh "$PEER" "tail -20 /tmp/tcpquic-dgx-server.log" >&2 || true
    return 1
}

start_local_client() {
    local compress=$1
    local -a warmup_args=()
    pkill -9 -f "${BIN} client" 2>/dev/null || true
    sleep 0.3
    if [[ "${WARMUP_MB}" -gt 0 ]]; then
        warmup_args=(
            --warmup-mb "${WARMUP_MB}"
            --warmup-target "${TARGET}:${HTTP_PORT}"
            --warmup-path /tcpquic-dgx-payload.bin
        )
        log "warmup enabled: ${WARMUP_MB} MB/conn -> ${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin"
    fi
    local -a tuning_args=()
    mapfile -t tuning_args < <(proxy_tuning_args)
    "$BIN" client \
        --peer "${TARGET}:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
        --cert "$CERT_DIR/client.crt" \
        --key "$CERT_DIR/client.key" \
        --ca "$CERT_DIR/ca.crt" \
        --connections "$QUIC_CONNECTIONS" \
        --compress "$compress" \
        "${tuning_args[@]}" \
        "${warmup_args[@]}" \
        >/tmp/tcpquic-dgx-client.log 2>&1 &
    CLIENT_PID=$!
    log "waiting for QUIC client connection (timeout=${CLIENT_QUIC_READY_WAIT_SEC}s)"
    local quic_wait_ticks=$((CLIENT_QUIC_READY_WAIT_SEC * 2))
    [[ "$quic_wait_ticks" -gt 0 ]] || quic_wait_ticks=1
    for _ in $(seq 1 "$quic_wait_ticks"); do
        sleep 0.5
        if grep -q "QUIC client connected" /tmp/tcpquic-dgx-client.log 2>/dev/null; then
            log "QUIC client connection established"
            break
        fi
    done
    if ! grep -q "QUIC client connected" /tmp/tcpquic-dgx-client.log 2>/dev/null; then
        log "local client failed before QUIC connection (compress=${compress})"
        tail -20 /tmp/tcpquic-dgx-client.log >&2 || true
        return 1
    fi

    log "waiting for HTTP CONNECT listener (timeout=${CLIENT_HTTP_READY_WAIT_SEC}s)"
    local http_wait_ticks=$((CLIENT_HTTP_READY_WAIT_SEC * 2))
    [[ "$http_wait_ticks" -gt 0 ]] || http_wait_ticks=1
    for _ in $(seq 1 "$http_wait_ticks"); do
        sleep 0.5
        if grep -q "HTTP CONNECT listening" /tmp/tcpquic-dgx-client.log 2>/dev/null; then
            return 0
        fi
    done
    log "local client failed before HTTP CONNECT listener (compress=${compress})"
    tail -20 /tmp/tcpquic-dgx-client.log >&2 || true
    return 1
}

stop_local_client() {
    if [[ -n "${CLIENT_PID:-}" ]]; then
        kill -9 "$CLIENT_PID" 2>/dev/null || true
        wait "$CLIENT_PID" 2>/dev/null || true
        CLIENT_PID=""
    fi
}

measure_direct() {
    local rc=0 speed
    if [[ "${DURATION_SEC}" -gt 0 ]]; then
        speed=$(curl -fsS --interface "$BIND" \
            "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
            -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' 2>/dev/null) || rc=$?
        printf '%s\n%s\n' "${speed:-0}" "$rc"
        return
    fi
    speed=$(curl -fsS --interface "$BIND" \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time 180 -w '%{speed_download}' 2>/dev/null) || rc=$?
    printf '%s\n%s\n' "${speed:-0}" "$rc"
}

measure_tunnel() {
    local rc=0 speed
    if [[ "${DURATION_SEC}" -gt 0 ]]; then
        speed=$(curl -fsS --interface "$BIND" \
            -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
            "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
            -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' 2>/dev/null) || rc=$?
        printf '%s\n%s\n' "${speed:-0}" "$rc"
        return
    fi
    speed=$(curl -fsS --interface "$BIND" \
        -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time 180 -w '%{speed_download}' 2>/dev/null) || rc=$?
    printf '%s\n%s\n' "${speed:-0}" "$rc"
}

run_mode() {
    local mode=$1
    local compress=$2
    local -a samples=()
    local -a curl_rcs=()
    local i speed avg measure_out curl_rc

    log "mode=${mode} iterations=${ITERATIONS} payload_kind=${PAYLOAD_KIND} payload=${SIZE_MB}MB duration=${DURATION_SEC}s quic-connections=${QUIC_CONNECTIONS}"
    if [[ "$NETEM" == "1" && "${DURATION_SEC}" -eq 0 && "${SIZE_MB}" -lt 128 ]]; then
        log "hint: 100ms netem 下 32MB 传输过短，BBR 难稳态；建议 SIZE_MB>=128 或 DURATION_SEC=10"
    fi
    for i in $(seq 1 "$ITERATIONS"); do
        curl_rc=0
        case "$mode" in
            direct)
                measure_out=$(measure_direct)
                ;;
            tunnel_*)
                remote_kill_proxy
                start_remote_server "$compress"
                start_local_client "$compress"
                measure_out=$(measure_tunnel)
                stop_local_client
                ;;
            *)
                log "unknown mode: $mode"
                exit 1
                ;;
        esac
        speed=$(echo "$measure_out" | sed -n '1p')
        curl_rc=$(echo "$measure_out" | sed -n '2p')
        samples+=("${speed:-0}")
        curl_rcs+=("${curl_rc:-0}")
        if [[ "$curl_rc" != "0" ]]; then
            log "  run ${i}/${ITERATIONS}: $(bytes_to_mbps "${speed:-0}") Mbps (curl exit ${curl_rc})"
        else
            log "  run ${i}/${ITERATIONS}: $(bytes_to_mbps "${speed:-0}") Mbps"
        fi
    done
    avg=$(avg_bytes "${samples[@]}")
    printf '%s,%s,%s,%s\n' "$mode" "$avg" "$(bytes_to_mbps "$avg")" "$(IFS=,; echo "${curl_rcs[*]}")"
}

append_log() {
    local netem_label results="$1"
    if [[ "$NETEM" == "1" ]]; then
        netem_label="${DELAY}"
        [[ -n "$LOSS" && "$LOSS" != "0" && "$LOSS" != "0%" ]] && netem_label="${netem_label} + ${LOSS} loss"
    else
        netem_label="200G 直连 (无 netem)"
    fi
    {
        echo ""
        echo "## tcpquic-proxy 双机压测 ($(date -Iseconds))"
        echo ""
        echo "| 项目 | 值 |"
        echo "|------|-----|"
        echo "| 服务端 | ${TARGET} (spark-1b6f) |"
        echo "| 客户端 | ${BIND} (spark-1619) |"
        echo "| 网络 | ${netem_label} |"
        echo "| 载荷 | ${PAYLOAD_KIND} ${SIZE_MB} MB × ${ITERATIONS}$([ "${DURATION_SEC}" -gt 0 ] && echo ", 持续 ${DURATION_SEC}s") |"
        echo "| QUIC 连接池 | ${QUIC_CONNECTIONS} |"
        if [[ "$TUNING_20GBPS" == "1" ]]; then
            echo "| QUIC 调优 | 20Gbps preset: BBR, fcw=1GiB, srw=1GiB, iw=4000, initrtt=200ms, relay-io=1MiB, inflight=1GiB |"
        else
            echo "| QUIC 调优 | BBR, fcw=500MB, srw=512MB, iw=2000, initrtt=100ms |"
        fi
        echo "| 指标口径 | ${METRIC_MODE} |"
        if [[ -n "${RATE}" ]]; then
            echo "| netem rate | ${RATE} |"
        fi
        if [[ "$NETEM" == "1" ]]; then
            echo "| netem interface | ${IFACE}（PEER 到 ${BIND} 的发送端出站网卡） |"
            echo "| netem limit | ${NETEM_LIMIT} |"
        fi
        echo ""
        echo "$results"
        echo ""
    } >> "$LOG_FILE"
}

require_cmd ssh
require_cmd rsync
require_cmd curl
require_cmd openssl
require_cmd python3
[[ -x "$BIN" ]] || { log "missing binary: $BIN"; exit 1; }

mkdir -p "$TMP_DIR"
if [[ -z "${RATE+x}" && "$NETEM" == "1" ]]; then
    RATE="1gbit"
    log "NETEM=1: default RATE=1gbit (set RATE= to disable, or RATE=<value> to override)"
fi
ensure_remote_bin
remote_kill
resolve_netem_iface
setup_netem
verify_netem
generate_certs
if [[ -z "${WARMUP_MB+x}" ]]; then
    if [[ "${DURATION_SEC}" -eq 0 && "${SIZE_MB}" -lt 64 ]]; then
        WARMUP_MB=2
    else
        WARMUP_MB=0
    fi
fi
if [[ "${METRIC_MODE}" == "steady" && "${DURATION_SEC}" -eq 0 ]]; then
    DURATION_SEC=10
    log "METRIC_MODE=steady: default DURATION_SEC=10 (align with secnetperf -down:10s)"
fi
if [[ "${DURATION_SEC}" -gt 0 ]]; then
    min_mb=$((DURATION_SEC * 150))
    if [[ "${SIZE_MB}" -lt "${min_mb}" ]]; then
        log "DURATION_SEC=${DURATION_SEC}: bump payload ${SIZE_MB}MB -> ${min_mb}MB (single-stream sustained)"
        SIZE_MB=$min_mb
    fi
fi
deploy_payload
start_remote_http

declare -A COMPRESS_MAP=(
    [tunnel_off]=off
    [tunnel_zstd]=zstd
)

RESULT_ROWS="| 模式 | 平均吞吐 |"
RESULT_ROWS+=$'\n|------|----------|'
CSV_LINES=""

for mode in $MODES; do
    compress="${COMPRESS_MAP[$mode]:-off}"
    line=$(run_mode "$mode" "$compress")
    echo "$line"
    CSV_LINES+="${line}"$'\n'
    mbps=$(echo "$line" | cut -d, -f3)
    curl_rcs=$(echo "$line" | cut -d, -f4)
    if [[ -n "$curl_rcs" && "$curl_rcs" != "0" && "$curl_rcs" != "0,0" ]]; then
        RESULT_ROWS+=$'\n'"| ${mode} | ${mbps} Mbps | curl(${curl_rcs}) |"
    else
        RESULT_ROWS+=$'\n'"| ${mode} | ${mbps} Mbps |"
    fi
done

if [[ "$SKIP_APPEND" != "1" ]]; then
    append_log "$RESULT_ROWS"
    log "appended results to ${LOG_FILE}"
fi

log "done"
