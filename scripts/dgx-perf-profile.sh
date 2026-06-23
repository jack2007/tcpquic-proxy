#!/usr/bin/env bash
# DGX 无时延 LAN：tcpquic-proxy 高速下载时 perf 热点采样
#
# 用法:
#   ./scripts/dgx-perf-profile.sh
#   DURATION_SEC=25 CASE=proxy-1x1 ./scripts/dgx-perf-profile.sh
#   CASE=proxy-4x16 ./scripts/dgx-perf-profile.sh
#   CASE=secnetperf-1conn ./scripts/dgx-perf-profile.sh   # msquic 基线对照
#
# 输出:
#   docs/dgx-perf-profile-<timestamp>/
#     client.perf.data / client.top.txt
#     server.perf.data / server.top.txt   (proxy 模式)
#     summary.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=dgx-interconnect-netem-common.sh
source "${ROOT}/scripts/dgx-interconnect-netem-common.sh"

BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
SECNETPERF="${SECNETPERF:-${ROOT}/build/msquic/bin/Release/secnetperf}"
MSQUIC_LIB_DIR="${MSQUIC_LIB_DIR:-${ROOT}/build/msquic/bin/Release}"
REMOTE_DIR="${REMOTE_DIR:-~/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-${REMOTE_DIR}/tcpquic-proxy}"
TARGET="${TARGET:-169.254.59.196}"
BIND="${BIND:-169.254.250.230}"
PEER="${PEER:-jack@${TARGET}}"
LOCAL_IFACE="${LOCAL_IFACE:-enp1s0f0np0}"
PEER_IFACE="${PEER_IFACE:-enp1s0f0np0}"
HTTP_PORT="${HTTP_PORT:-16001}"
QUIC_PORT="${QUIC_PORT:-4433}"
PERF_PORT="${PERF_PORT:-4434}"
PROXY_PORT="${PROXY_PORT:-18080}"
DURATION_SEC="${DURATION_SEC:-25}"
CASE="${CASE:-proxy-1x1}"   # proxy-1x1 | proxy-4x16 | proxy-custom | secnetperf-1conn
QUIC_CONNS="${QUIC_CONNS:-1}"
CURL_PARALLEL="${CURL_PARALLEL:-1}"
PROXY_EXTRA_ARGS="${PROXY_EXTRA_ARGS:-}"
FREQ="${FREQ:-999}"
TS="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="${OUT_DIR:-${ROOT}/docs/dgx-perf-profile-${TS}}"

TMP="/tmp/dgx-perf-$$"
CERT_DIR="${TMP}/certs"
CLIENT_PID=""
CLIENT_PERF_PID=""
SERVER_PERF_PID=""

log() { printf '[dgx-perf] %s\n' "$*" >&2; }

# 便于 perf 期间 client 崩溃时用 gdb 分析：core 写到 /tmp/core.<exe>.<pid>.<ts>
ensure_coredump() {
    ulimit -c unlimited 2>/dev/null || true
    local pattern="${CORE_PATTERN:-/tmp/core.%e.%p.%t}"
    if [[ -w /proc/sys/kernel/core_pattern ]]; then
        echo "${pattern}" > /proc/sys/kernel/core_pattern 2>/dev/null || \
            sudo sysctl -w "kernel.core_pattern=${pattern}" >/dev/null 2>&1 || true
    else
        sudo sysctl -w "kernel.core_pattern=${pattern}" >/dev/null 2>&1 || true
    fi
}

ensure_routes() {
    sudo ip route replace "${TARGET}" dev "${LOCAL_IFACE}" src "${BIND}" 2>/dev/null || true
    ssh -o BatchMode=yes "$PEER" "sudo ip route replace ${BIND} dev ${PEER_IFACE} src ${TARGET} 2>/dev/null; true"
}

remote_kill_all() {
    ssh -o BatchMode=yes "$PEER" "killall -9 tcpquic-proxy secnetperf 2>/dev/null; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
    [[ -n "${CLIENT_PID:-}" ]] && kill -9 "$CLIENT_PID" 2>/dev/null || true
    CLIENT_PID=""
}

deploy_binaries() {
    ssh -o BatchMode=yes "$PEER" "mkdir -p ${REMOTE_DIR}"
    shopt -s nullglob
    local msquic_sos=("${MSQUIC_LIB_DIR}"/libmsquic.so*)
    shopt -u nullglob
    if ((${#msquic_sos[@]})); then
        rsync -az "${BIN}" "${msquic_sos[@]}" "${PEER}:${REMOTE_DIR}/"
        ssh -o BatchMode=yes "$PEER" "cd ${REMOTE_DIR} && ln -sf \$(ls libmsquic.so.2.* 2>/dev/null | head -1) libmsquic.so.2 2>/dev/null || true"
    else
        rsync -az "${BIN}" "${PEER}:${REMOTE_DIR}/"
    fi
    rsync -az "${SECNETPERF}" "${PEER}:${REMOTE_DIR}/" 2>/dev/null || true
}

start_busybox_http() {
    ssh -o BatchMode=yes "$PEER" "fuser -k ${HTTP_PORT}/tcp 2>/dev/null || true"
    ssh -o BatchMode=yes "$PEER" "nohup busybox httpd -f -p ${TARGET}:${HTTP_PORT} -h /home/jack >/tmp/dgx-http.log 2>&1 & sleep 1"
}

ensure_payload() {
    ssh -o BatchMode=yes "$PEER" "test -f ~/tcpquic-dgx-payload.bin || dd if=/dev/zero of=~/tcpquic-dgx-payload.bin bs=1M count=512 status=none"
}

generate_certs() {
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=dgx-perf-ca" -days 1 -sha256 >/dev/null 2>&1
    for n in server client; do
        openssl req -newkey rsa:2048 -nodes \
            -keyout "$CERT_DIR/$n.key" -out "$CERT_DIR/$n.csr" \
            -subj "/CN=$n" >/dev/null 2>&1
        openssl x509 -req -in "$CERT_DIR/$n.csr" \
            -CA "$CERT_DIR/ca.crt" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
            -out "$CERT_DIR/$n.crt" -days 1 -sha256 >/dev/null 2>&1
    done
    rsync -az "$CERT_DIR/" "${PEER}:~/tcpquic-dgx-certs/"
}

proxy_args() {
    local quic_conns="${1:-${QUIC_CONNS}}"
    printf '%s' \
        "--tuning wan --iw 4000 \
        --initrtt-ms 100 --relay-io-size 1048576 \
        --connections ${quic_conns} --compress off ${PROXY_EXTRA_ARGS}"
}

start_proxy_stack() {
    remote_kill_all
    local args
    args=$(proxy_args "${QUIC_CONNS}")
    ssh -o BatchMode=yes "$PEER" "nohup ${REMOTE_BIN} server \
        --listen ${TARGET}:${QUIC_PORT} \
        --allow-targets ${TARGET}/32,127.0.0.0/8 \
        --cert ~/tcpquic-dgx-certs/server.crt \
        --key ~/tcpquic-dgx-certs/server.key \
        --ca ~/tcpquic-dgx-certs/ca.crt \
        ${args} </dev/null >/tmp/dgx-proxy-server.log 2>&1 &"
    for _ in $(seq 1 40); do
        sleep 0.3
        ssh -o BatchMode=yes "$PEER" "grep -q 'QUIC server listening' /tmp/dgx-proxy-server.log 2>/dev/null" && break
    done
    "$BIN" client \
        --peer "${TARGET}:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
        --cert "$CERT_DIR/client.crt" \
        --key "$CERT_DIR/client.key" \
        --ca "$CERT_DIR/ca.crt" \
        ${args} \
        >/tmp/dgx-proxy-client.log 2>&1 &
    CLIENT_PID=$!
    for _ in $(seq 1 50); do
        sleep 0.3
        grep -q "HTTP CONNECT listening" /tmp/dgx-proxy-client.log 2>/dev/null && return 0
    done
    return 1
}

start_secnetperf_server() {
    remote_kill_all
    local remote_ld=""
    if ssh -o BatchMode=yes "$PEER" "test -e ${REMOTE_DIR}/libmsquic.so.2 || test -e ${REMOTE_DIR}/libmsquic.so"; then
        remote_ld="LD_LIBRARY_PATH=${REMOTE_DIR} "
    fi
    ssh -o BatchMode=yes "$PEER" \
        "${remote_ld}nohup ${REMOTE_DIR}/secnetperf \
        -exec:maxtput -bind:${TARGET}:${PERF_PORT} \
        </dev/null >/tmp/dgx-secnetperf-server.log 2>&1 &"
    sleep 2
}

wait_pid() {
    local pid="$1"
    local name="$2"
    local deadline=$((SECONDS + 30))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    log "timeout waiting for $name pid=$pid"
    return 1
}

remote_server_pid() {
    local pattern="$1"
    ssh -o BatchMode=yes "$PEER" "pgrep -n -f '${pattern}' 2>/dev/null || true"
}

perf_record_local() {
    local pid="$1"
    local out="$2"
    sudo perf record -F "${FREQ}" -g --call-graph dwarf,16384 \
        -p "${pid}" -o "${out}" -- sleep "${DURATION_SEC}"
}

perf_record_remote() {
    local remote_out="/tmp/dgx-perf-server-${TS}.data"
    # -a 按 comm 过滤，避免 server pid 在 ssh 竞态下失效
    ssh -o BatchMode=yes "$PEER" \
        "sudo perf record -F ${FREQ} -g --call-graph dwarf,16384 \
        -a -o ${remote_out} -- sleep ${DURATION_SEC}; \
        sudo chmod a+r ${remote_out}"
    scp -q "${PEER}:${remote_out}" "${OUT_DIR}/server.perf.data"
    ssh -o BatchMode=yes "$PEER" "sudo rm -f ${remote_out}"
}

perf_top_report() {
    local data="$1"
    local out="$2"
    local comm="${3:-tcpquic-proxy}"
    sudo perf report -f -i "${data}" --comm "${comm}" --stdio --no-children \
        --sort=symbol --percent-limit 0.4 -n 2>/dev/null | head -150 > "${out}" || true
}

run_proxy_perf() {
    start_proxy_stack
    sleep 1
    local server_pid
    server_pid=$(remote_server_pid "tcpquic-proxy server")
    [[ -n "$server_pid" ]] || { log "server pid not found"; return 1; }
    log "server pid=${server_pid} client pid=${CLIENT_PID}"

    perf_record_local "${CLIENT_PID}" "${OUT_DIR}/client.perf.data" &
    CLIENT_PERF_PID=$!
    perf_record_remote "${server_pid}" &
    SERVER_PERF_PID=$!
    sleep 1

    # 持续下载填满采样窗口（避免 payload 传完后长时间 epoll 空转）
    (
        end=$((SECONDS + DURATION_SEC))
        while [ "$SECONDS" -lt "$end" ]; do
            local i
            for i in $(seq 1 "${CURL_PARALLEL}"); do
                curl -fsS --interface "$BIND" \
                    -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
                    "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
                    -o /dev/null --max-time 120 2>/dev/null &
            done
            wait || true
        done
    ) &
    local curl_pid=$!

    wait "$curl_pid" || true
    wait "$CLIENT_PERF_PID" 2>/dev/null || true
    wait "$SERVER_PERF_PID" 2>/dev/null || true

    # 记录最后一次 curl 速度作参考
    curl -fsS --interface "$BIND" \
        -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time 10 \
        -w 'speed_download=%{speed_download}\n' \
        >"${OUT_DIR}/throughput.txt" 2>/dev/null || true
}

run_secnetperf_perf() {
    [[ -x "$SECNETPERF" ]] || { log "missing $SECNETPERF"; exit 1; }
    start_secnetperf_server
    local -a secnetperf_env=()
    if compgen -G "${MSQUIC_LIB_DIR}/libmsquic.so"* >/dev/null; then
        secnetperf_env=(env "LD_LIBRARY_PATH=${MSQUIC_LIB_DIR}")
    fi
    "${secnetperf_env[@]}" "${SECNETPERF}" \
        -target:"${TARGET}" -bind:"${BIND}" -port:"${PERF_PORT}" \
        -exec:maxtput -conns:1 -down:"${DURATION_SEC}s" -ptput:1 \
        >"${OUT_DIR}/secnetperf.log" 2>&1 &
    CLIENT_PID=$!
    wait_pid "$CLIENT_PID" "secnetperf client"
    log "secnetperf client pid=${CLIENT_PID}"
    perf_record_local "${CLIENT_PID}" "${OUT_DIR}/client.perf.data"
}

write_summary() {
    local mode="$1"
    {
        echo "# DGX perf 热点分析"
        echo ""
        echo "- 时间: $(date -Iseconds)"
        echo "- 场景: ${CASE} (${mode})"
        echo "- QUIC 连接: ${QUIC_CONNS} | 并行 curl: ${CURL_PARALLEL}"
        echo "- 采样: ${DURATION_SEC}s @ ${FREQ}Hz, call-graph dwarf"
        echo "- 本机: ${BIND} | 对端: ${TARGET}"
        if [[ -f "${OUT_DIR}/sysctl.txt" ]]; then
            echo ""
            echo "## 内核 socket 参数"
            echo '```'
            cat "${OUT_DIR}/sysctl.txt"
            echo '```'
            echo ""
        fi
        echo ""
        if [[ -f "${OUT_DIR}/throughput.txt" ]]; then
            echo "## 吞吐"
            echo '```'
            cat "${OUT_DIR}/throughput.txt"
            echo '```'
            echo ""
        fi
        if [[ -f "${OUT_DIR}/client.top.txt" ]]; then
            echo "## Client 热点 Top"
            echo '```'
            cat "${OUT_DIR}/client.top.txt"
            echo '```'
            echo ""
        fi
        if [[ -f "${OUT_DIR}/server.top.txt" ]]; then
            echo "## Server 热点 Top"
            echo '```'
            cat "${OUT_DIR}/server.top.txt"
            echo '```'
            echo ""
        fi
    } > "${OUT_DIR}/summary.md"
}

cleanup() {
    set +e
    [[ -n "${CLIENT_PERF_PID:-}" ]] && kill -9 "$CLIENT_PERF_PID" 2>/dev/null || true
    [[ -n "${SERVER_PERF_PID:-}" ]] && kill -9 "$SERVER_PERF_PID" 2>/dev/null || true
    remote_kill_all
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

[[ -x "$BIN" ]] || { log "missing $BIN"; exit 1; }
mkdir -p "$OUT_DIR" "$TMP"
ensure_coredump
ensure_routes
deploy_binaries
start_busybox_http
ensure_payload

case "$CASE" in
    proxy-1x1)
        QUIC_CONNS=1
        CURL_PARALLEL=1
        generate_certs
        run_proxy_perf
        perf_top_report "${OUT_DIR}/client.perf.data" "${OUT_DIR}/client.top.txt" tcpquic-proxy
        perf_top_report "${OUT_DIR}/server.perf.data" "${OUT_DIR}/server.top.txt" tcpquic-proxy
        write_summary "tcpquic-proxy 单 QUIC + 单 curl"
        ;;
    proxy-4x16)
        QUIC_CONNS=16
        CURL_PARALLEL=4
        generate_certs
        run_proxy_perf
        perf_top_report "${OUT_DIR}/client.perf.data" "${OUT_DIR}/client.top.txt" tcpquic-proxy
        perf_top_report "${OUT_DIR}/server.perf.data" "${OUT_DIR}/server.top.txt" tcpquic-proxy
        write_summary "tcpquic-proxy quic=16 + 4 并行 curl"
        ;;
    proxy-custom)
        generate_certs
        run_proxy_perf
        perf_top_report "${OUT_DIR}/client.perf.data" "${OUT_DIR}/client.top.txt" tcpquic-proxy
        perf_top_report "${OUT_DIR}/server.perf.data" "${OUT_DIR}/server.top.txt" tcpquic-proxy
        write_summary "tcpquic-proxy quic=${QUIC_CONNS} + ${CURL_PARALLEL} 并行 curl"
        ;;
    secnetperf-1conn)
        [[ -x "$SECNETPERF" ]] || { log "missing $SECNETPERF"; exit 1; }
        run_secnetperf_perf
        perf_top_report "${OUT_DIR}/client.perf.data" "${OUT_DIR}/client.top.txt" secnetperf
        write_summary "secnetperf 单连接基线"
        ;;
    *)
        log "unknown CASE=${CASE}"
        exit 1
        ;;
esac

log "done -> ${OUT_DIR}/summary.md"
cat "${OUT_DIR}/summary.md"
