#!/usr/bin/env bash
# Step2 专项：initrtt 对照 + 多 curl / 多 QUIC 连接（无 netem）
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
REPORT="${REPORT:-/tmp/dgx-step2-investigate-$(date +%Y%m%d-%H%M%S).md}"

TMP="/tmp/dgx-step2-$$"
CERT_DIR="${TMP}/certs"
CLIENT_PID=""

log() { printf '[step2-inv] %s\n' "$*" >&2; }

mbps_from_bps() {
    python3 -c "bps=float('${1:-0}'); print(f'{bps*8/1e6:.2f}')"
}

gbps_from_bps() {
    python3 -c "bps=float('${1:-0}'); print(f'{bps*8/1e9:.3f}')"
}

ensure_routes() {
    sudo ip route replace "${TARGET}" dev "${LOCAL_IFACE}" src "${BIND}" 2>/dev/null || true
    ssh -o BatchMode=yes "$PEER" "sudo ip route replace ${BIND} dev ${PEER_IFACE} src ${TARGET} 2>/dev/null; true"
}

clear_peer_netem() {
    ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc del dev ${PEER_IFACE} root 2>/dev/null; true"
}

remote_kill_proxy() {
    ssh -o BatchMode=yes "$PEER" "killall -9 tcpquic-proxy 2>/dev/null; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
    if [[ -n "${CLIENT_PID:-}" ]]; then
        kill -9 "$CLIENT_PID" 2>/dev/null || true
        CLIENT_PID=""
    fi
}

start_busybox_http() {
    ssh -o BatchMode=yes "$PEER" "fuser -k ${HTTP_PORT}/tcp 2>/dev/null || true"
    ssh -o BatchMode=yes "$PEER" "nohup busybox httpd -f -p ${TARGET}:${HTTP_PORT} -h /home/jack >/tmp/dgx-step2-http.log 2>&1 & sleep 1"
}

ensure_payload() {
    ssh -o BatchMode=yes "$PEER" "test -f ~/tcpquic-dgx-payload.bin || dd if=/dev/zero of=~/tcpquic-dgx-payload.bin bs=1M count=512 status=none"
}

generate_certs() {
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=dgx-step2-ca" -days 1 -sha256 >/dev/null 2>&1
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

proxy_extra_args() {
    local initrtt="$1" quic_conns="$2"
    printf '%s' \
        "--tuning custom --fcw 1073741824 --srw 1073741824 --iw 4000 \
        --initrtt-ms ${initrtt} --relay-io-size 1048576 \
        --connections ${quic_conns} --compress off"
}

start_proxy_stack() {
    local initrtt="$1" quic_conns="$2"
    remote_kill_proxy
    local args
    args=$(proxy_extra_args "$initrtt" "$quic_conns")
    ssh -o BatchMode=yes "$PEER" "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
        --listen ${TARGET}:${QUIC_PORT} \
        --allow-targets ${TARGET}/32,127.0.0.0/8 \
        --cert ~/tcpquic-dgx-certs/server.crt \
        --key ~/tcpquic-dgx-certs/server.key \
        --ca ~/tcpquic-dgx-certs/ca.crt \
        ${args} </dev/null >/tmp/dgx-step2-server.log 2>&1 &"
    for _ in $(seq 1 40); do
        sleep 0.3
        ssh -o BatchMode=yes "$PEER" "grep -q 'QUIC server listening' /tmp/dgx-step2-server.log 2>/dev/null" && break
    done
    "$BIN" client \
        --peer "${TARGET}:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
        --cert "$CERT_DIR/client.crt" \
        --key "$CERT_DIR/client.key" \
        --ca "$CERT_DIR/ca.crt" \
        ${args} \
        >/tmp/dgx-step2-client.log 2>&1 &
    CLIENT_PID=$!
    for _ in $(seq 1 50); do
        sleep 0.3
        grep -q "HTTP CONNECT listening" /tmp/dgx-step2-client.log 2>/dev/null && return 0
    done
    tail -10 /tmp/dgx-step2-client.log >&2 || true
    return 1
}

measure_direct() {
    curl -fsS --interface "$BIND" \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' 2>/dev/null || echo 0
}

measure_proxy_one() {
    curl -fsS --interface "$BIND" \
        -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' 2>/dev/null || echo 0
}

measure_proxy_parallel() {
    local n="$1"
    local i speed_dir="${TMP}/parallel-$$"
    mkdir -p "$speed_dir"
    for i in $(seq 1 "$n"); do
        curl -fsS --interface "$BIND" \
            -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
            "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
            -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' \
            >"${speed_dir}/${i}.bps" 2>/dev/null &
    done
    wait
    python3 - "$speed_dir" <<'PY'
import glob, os, sys
d = sys.argv[1]
total = 0.0
for p in sorted(glob.glob(os.path.join(d, "*.bps"))):
    with open(p) as f:
        total += float(f.read().strip() or 0)
print(total)
PY
}

get_tuning_line() {
    grep -E '^tcpquic-proxy tuning:' /tmp/dgx-step2-client.log 2>/dev/null | tail -1 || true
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
clear_peer_netem
start_busybox_http
ensure_payload
generate_certs

{
    echo "# DGX Step2 专项排查"
    echo ""
    echo "- 时间: $(date -Iseconds)"
    echo "- 本机: ${BIND} | 对端: ${TARGET}"
    echo "- 无 netem | busybox httpd | 下载 max-time ${DURATION_SEC}s"
    echo ""
} > "$REPORT"

log "Step1 基线 (direct)"
s1=$(measure_direct)
log "Step1: $(mbps_from_bps "$s1") Mbps ($(gbps_from_bps "$s1") Gbps)"
echo "## Step1 基线 (direct)" >> "$REPORT"
echo "- 吞吐: **$(mbps_from_bps "$s1") Mbps** ($(gbps_from_bps "$s1") Gbps)" >> "$REPORT"
echo "" >> "$REPORT"

echo "## initrtt 对照（单 curl，quic-connections=1）" >> "$REPORT"
echo "" >> "$REPORT"
echo "| initrtt-ms | Mbps | Gbps | vs Step1 | tuning relay_pending |" >> "$REPORT"
echo "|------------|------|------|----------|----------------------|" >> "$REPORT"

for initrtt in 1 10 200; do
    log "initrtt=${initrtt} single curl"
    start_proxy_stack "$initrtt" 1
    sleep 1
    spd=$(measure_proxy_one)
    tuning=$(get_tuning_line)
    pending=$(echo "$tuning" | grep -oP 'relay_pending=\K[0-9]+' || echo "?")
    ratio=$(python3 -c "s1=float('$s1'); s2=float('$spd'); print(f'{s1/s2:.2f}x' if s2>0 else 'n/a')")
    mbps=$(mbps_from_bps "$spd")
    gbps=$(gbps_from_bps "$spd")
    log "  -> ${mbps} Mbps (${gbps} Gbps)"
    echo "| ${initrtt} | ${mbps} | ${gbps} | ${ratio} | ${pending} |" >> "$REPORT"
    remote_kill_proxy
done

echo "" >> "$REPORT"
echo "## 多 curl × 多 QUIC 连接（initrtt=1）" >> "$REPORT"
echo "" >> "$REPORT"
echo "| parallel_curl | quic_connections | 聚合 Mbps | 聚合 Gbps | vs Step1 |" >> "$REPORT"
echo "|---------------|------------------|-----------|-----------|----------|" >> "$REPORT"

for pair in "1:1" "4:4" "8:8" "16:16" "16:4" "4:16"; do
    pc="${pair%%:*}"
    qc="${pair##*:}"
    log "parallel=${pc} quic_conns=${qc}"
    start_proxy_stack 1 "$qc"
    sleep 1
    if [[ "$pc" == "1" ]]; then
        agg=$(measure_proxy_one)
    else
        agg=$(measure_proxy_parallel "$pc")
    fi
    mbps=$(mbps_from_bps "$agg")
    gbps=$(gbps_from_bps "$agg")
    ratio=$(python3 -c "s1=float('$s1'); s2=float('$agg'); print(f'{s1/s2:.2f}x' if s2>0 else 'n/a')")
    log "  -> aggregate ${mbps} Mbps (${gbps} Gbps)"
    echo "| ${pc} | ${qc} | ${mbps} | ${gbps} | ${ratio} |" >> "$REPORT"
    remote_kill_proxy
done

echo "" >> "$REPORT"
echo "报告: ${REPORT}" >> "$REPORT"
log "done -> ${REPORT}"
cat "$REPORT"
