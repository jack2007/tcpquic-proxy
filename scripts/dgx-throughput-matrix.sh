#!/usr/bin/env bash
# DGX 双机五步吞吐排查矩阵（按用户指定顺序）
# Step1 裸 curl TCP | Step2 tcpquic-proxy | Step3 100ms | Step4 100ms+5% | Step5 200ms+5%
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=dgx-interconnect-netem-common.sh
source "${ROOT}/scripts/dgx-interconnect-netem-common.sh"

BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
REMOTE_BIN="${REMOTE_BIN:-~/tcpquic-dgx-bin/tcpquic-proxy}"
REMOTE_DIR="${REMOTE_DIR:-~/tcpquic-dgx-bin}"
TARGET="${TARGET:-169.254.59.196}"
BIND="${BIND:-169.254.250.230}"
PEER="${PEER:-jack@${TARGET}}"
LOCAL_IFACE="${LOCAL_IFACE:-enp1s0f0np0}"
PEER_IFACE="${PEER_IFACE:-enp1s0f0np0}"
HTTP_PORT="${HTTP_PORT:-16001}"
QUIC_PORT="${QUIC_PORT:-4433}"
PROXY_PORT="${PROXY_PORT:-18080}"
DURATION_SEC="${DURATION_SEC:-30}"
NETEM_LIMIT="${NETEM_LIMIT:-1000000}"
REPORT="${REPORT:-/tmp/dgx-throughput-matrix-$(date +%Y%m%d-%H%M%S).md}"

TMP="/tmp/dgx-matrix-$$"
CERT_DIR="${TMP}/certs"
CLIENT_PID=""

log() { printf '[dgx-matrix] %s\n' "$*" >&2; }

mbps_from_bps() {
    python3 - "$1" <<'PY'
import sys
bps = float(sys.argv[1] or 0)
print(f"{bps * 8 / 1_000_000:.2f}")
PY
}

gbps_from_bps() {
    python3 - "$1" <<'PY'
import sys
bps = float(sys.argv[1] or 0)
print(f"{bps * 8 / 1_000_000_000:.3f}")
PY
}

ensure_routes() {
    sudo ip route replace "${TARGET}" dev "${LOCAL_IFACE}" src "${BIND}" 2>/dev/null || true
    ssh -o BatchMode=yes "$PEER" "sudo ip route replace ${BIND} dev ${PEER_IFACE} src ${TARGET} 2>/dev/null; true"
}

clear_peer_netem() {
    ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc del dev ${PEER_IFACE} root 2>/dev/null; true"
}

apply_netem() {
    local delay="$1" loss="$2"
    clear_peer_netem
    if [[ -n "$loss" && "$loss" != "0" && "$loss" != "0%" ]]; then
        apply_peer_netem "$delay" "$loss"
    else
        ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc replace dev ${PEER_IFACE} root netem delay ${delay} limit ${NETEM_LIMIT}"
    fi
    sleep 0.5
}

verify_ping() {
    local label="$1"
    local out avg loss
    out=$(ping -c 10 -i 0.2 -W 2 -I "$BIND" "$TARGET" 2>&1 || true)
    avg=$(echo "$out" | awk -F'/' '/rtt min/{print $5}')
    loss=$(echo "$out" | grep -oP '\d+(?=% packet loss)' | head -1 || echo "?")
    log "${label}: RTT avg=${avg:-?}ms loss=${loss}%"
    printf '%s|%s\n' "${avg:-?}" "${loss:-?}"
}

start_busybox_http() {
    ssh -o BatchMode=yes "$PEER" "fuser -k ${HTTP_PORT}/tcp 2>/dev/null || true"
    ssh -o BatchMode=yes "$PEER" "nohup busybox httpd -f -p ${TARGET}:${HTTP_PORT} -h /home/jack >/tmp/dgx-matrix-http.log 2>&1 & sleep 1; ss -lntp | grep ':${HTTP_PORT}'"
}

ensure_payload() {
    ssh -o BatchMode=yes "$PEER" "test -f ~/tcpquic-dgx-payload.bin || dd if=/dev/zero of=~/tcpquic-dgx-payload.bin bs=1M count=512 status=none"
}

remote_kill_proxy() {
    ssh -o BatchMode=yes "$PEER" "killall -9 tcpquic-proxy 2>/dev/null; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
    if [[ -n "${CLIENT_PID:-}" ]]; then
        kill -9 "$CLIENT_PID" 2>/dev/null || true
        CLIENT_PID=""
    fi
}

generate_certs() {
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=dgx-matrix-ca" -days 1 -sha256 >/dev/null 2>&1
    cat > "$CERT_DIR/server.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:${TARGET}
EOF
    cat > "$CERT_DIR/client.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:dgx-matrix-client
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

proxy_args() {
    printf '%s' "--tuning custom --fcw 1073741824 --srw 1073741824 --iw 4000 --initrtt-ms 200 --relay-io-size 1048576 --connections 1"
}

start_proxy_stack() {
    remote_kill_proxy
    local args
    args=$(proxy_args)
    ssh -o BatchMode=yes "$PEER" "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
        --listen ${TARGET}:${QUIC_PORT} \
        --allow-targets ${TARGET}/32,127.0.0.0/8 \
        --cert ~/tcpquic-dgx-certs/server.crt \
        --key ~/tcpquic-dgx-certs/server.key \
        --ca ~/tcpquic-dgx-certs/ca.crt \
        --compress off \
        ${args} </dev/null >/tmp/dgx-matrix-server.log 2>&1 &"
    for _ in $(seq 1 30); do
        sleep 0.3
        ssh -o BatchMode=yes "$PEER" "grep -q 'QUIC server listening' /tmp/dgx-matrix-server.log 2>/dev/null" && break
    done
    "$BIN" client \
        --peer "${TARGET}:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
        --cert "$CERT_DIR/client.crt" \
        --key "$CERT_DIR/client.key" \
        --ca "$CERT_DIR/ca.crt" \
        --compress off \
        ${args} \
        >/tmp/dgx-matrix-client.log 2>&1 &
    CLIENT_PID=$!
    for _ in $(seq 1 40); do
        sleep 0.3
        grep -q "HTTP CONNECT listening" /tmp/dgx-matrix-client.log 2>/dev/null && return 0
    done
    log "client failed to start"; tail -15 /tmp/dgx-matrix-client.log >&2 || true
    return 1
}

measure_direct_curl() {
    local speed rc
    set +e
    speed=$(curl -fsS --interface "$BIND" \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}')
    rc=$?
    set -e
    printf '%s|%s\n' "${speed:-0}" "$rc"
}

measure_proxy_curl() {
    local speed rc
    set +e
    speed=$(curl -fsS --interface "$BIND" \
        -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}')
    rc=$?
    set -e
    printf '%s|%s|%s\n' "${speed:-0}" "$rc" "$(grep -E '^tcpquic-proxy tuning:' /tmp/dgx-matrix-client.log 2>/dev/null | tail -1 || true)"
}

run_step() {
    local step="$1" name="$2" mode="$3" delay="$4" loss="$5"
    log "===== Step ${step}: ${name} ====="
    ensure_no_local_netem
    if [[ -n "$delay" ]]; then
        apply_netem "$delay" "$loss"
    else
        clear_peer_netem
    fi
    local ping_stats rtt loss_pct
    ping_stats=$(verify_ping "Step${step}")
    rtt=$(echo "$ping_stats" | cut -d'|' -f1)
    loss_pct=$(echo "$ping_stats" | cut -d'|' -f2)

    local measure tuning_line="" speed rc
    if [[ "$mode" == "direct" ]]; then
        remote_kill_proxy
        measure=$(measure_direct_curl)
        speed=$(echo "$measure" | cut -d'|' -f1)
        rc=$(echo "$measure" | cut -d'|' -f2)
    else
        start_proxy_stack
        measure=$(measure_proxy_curl)
        speed=$(echo "$measure" | cut -d'|' -f1)
        rc=$(echo "$measure" | cut -d'|' -f2)
        tuning_line=$(echo "$measure" | cut -d'|' -f3-)
        remote_kill_proxy
    fi
    local mbps gbps
    mbps=$(mbps_from_bps "$speed")
    gbps=$(gbps_from_bps "$speed")
    log "Step${step} result: ${mbps} Mbps (${gbps} Gbps) curl_exit=${rc}"

    {
        echo "| ${step} | ${name} | ${mode} | ${delay:-none} | ${loss:-0%} | ${rtt} | ${loss_pct} | ${mbps} | ${gbps} | ${rc} |"
        if [[ -n "$tuning_line" ]]; then
            echo ""
            echo "<!-- step${step} tuning: ${tuning_line} -->"
        fi
    } >> "$REPORT"
}

cleanup() {
    set +e
    remote_kill_proxy
    clear_peer_netem
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

[[ -x "$BIN" ]] || { log "missing $BIN"; exit 1; }

mkdir -p "$TMP"
ensure_routes
ensure_payload
start_busybox_http
generate_certs

{
    echo "# DGX 双机五步吞吐排查"
    echo ""
    echo "- 时间: $(date -Iseconds)"
    echo "- 本机: ${BIND} (${LOCAL_IFACE})"
    echo "- 对端: ${TARGET} (${PEER_IFACE})"
    echo "- HTTP 源站: busybox httpd"
    echo "- 下载时长: ${DURATION_SEC}s / 步"
    echo "- netem: 仅对端 ${PEER_IFACE}, limit=${NETEM_LIMIT}"
    echo "- 代理调优: 20Gbps preset (单 QUIC 连接)"
    echo ""
    echo "| Step | 场景 | 模式 | netem delay | netem loss | ping RTT(ms) | ping loss | 吞吐(Mbps) | 吞吐(Gbps) | curl exit |"
    echo "|------|------|------|-------------|------------|--------------|-----------|------------|------------|-----------|"
} > "$REPORT"

run_step 1 "无 netem 裸 curl TCP 基线" direct "" ""
run_step 2 "无 netem tcpquic-proxy" proxy "" ""
run_step 3 "netem 100ms 无丢包" proxy "100ms" ""
run_step 4 "netem 100ms + 5%丢包" proxy "100ms" "5%"
run_step 5 "netem 200ms + 5%丢包" proxy "200ms" "5%"

{
    echo ""
    echo "## 判定说明"
    echo ""
    echo "- Step1 ≥ 5Gbps：说明裸 curl + busybox HTTP 链路可达目标，测试工具不是瓶颈。"
    echo "- Step2 vs Step1：代理引入的损耗。"
    echo "- Step3/4/5：时延与丢包叠加后的退化曲线。"
    echo ""
    echo "报告文件: ${REPORT}"
} >> "$REPORT"

log "matrix complete -> ${REPORT}"
cat "$REPORT"
