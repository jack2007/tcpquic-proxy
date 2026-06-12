#!/usr/bin/env bash
# 无时延 LAN：msquic secnetperf 基线 vs tcpquic-proxy 吞吐对比
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=dgx-interconnect-netem-common.sh
source "${ROOT}/scripts/dgx-interconnect-netem-common.sh"

SECNETPERF="${SECNETPERF:-${ROOT}/build/msquic/bin/Release/secnetperf}"
MSQUIC_LIB_DIR="${MSQUIC_LIB_DIR:-${ROOT}/build/msquic/bin/Release}"
BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
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
DURATION_SEC="${DURATION_SEC:-30}"
REPORT="${REPORT:-/tmp/dgx-msquic-vs-proxy-$(date +%Y%m%d-%H%M%S).md}"

TMP="/tmp/dgx-msquic-proxy-$$"
CERT_DIR="${TMP}/certs"
CLIENT_PID=""
PERF_SERVER_PID=""

log() { printf '[msquic-vs-proxy] %s\n' "$*" >&2; }

require_sysctl_value() {
    local name="$1" min_value="$2" actual
    actual=$(sysctl -n "$name" 2>/dev/null || echo 0)
    if [[ "${actual:-0}" -lt "$min_value" ]]; then
        log "sysctl ${name}=${actual:-unset}, require >= ${min_value}"
        exit 1
    fi
}

require_sysctl_triplet_max() {
    local name="$1" min_value="$2" actual max_value
    actual=$(sysctl -n "$name" 2>/dev/null || true)
    max_value=$(awk "{print \$3}" <<<"$actual")
    if [[ -z "$max_value" || "$max_value" -lt "$min_value" ]]; then
        log "sysctl ${name}=\"${actual:-unset}\", require max >= ${min_value}"
        exit 1
    fi
}

check_required_sysctls_local() {
    require_sysctl_value net.core.rmem_max 134217728
    require_sysctl_value net.core.wmem_max 134217728
    require_sysctl_triplet_max net.ipv4.tcp_rmem 134217728
    require_sysctl_triplet_max net.ipv4.tcp_wmem 134217728
}

check_required_sysctls_remote() {
    ssh -o BatchMode=yes "$PEER" "
        set -e
        check_value() { v=\$(sysctl -n \"\$1\" 2>/dev/null || echo 0); test \"\$v\" -ge \"\$2\"; }
        check_triplet() { v=\$(sysctl -n \"\$1\" 2>/dev/null || true); m=\$(awk \"{print \\\$3}\" <<<\"\$v\"); test -n \"\$m\" && test \"\$m\" -ge \"\$2\"; }
        check_value net.core.rmem_max 134217728
        check_value net.core.wmem_max 134217728
        check_triplet net.ipv4.tcp_rmem 134217728
        check_triplet net.ipv4.tcp_wmem 134217728
    " || { log "peer sysctl requirements failed on ${PEER}"; exit 1; }
}

check_required_sysctls() {
    check_required_sysctls_local
    check_required_sysctls_remote
}

mbps_from_bps() {
    python3 -c "print(f'{float(\"${1:-0}\")*8/1e6:.2f}')"
}

gbps_from_bps() {
    python3 -c "print(f'{float(\"${1:-0}\")*8/1e9:.3f}')"
}

mbps_from_kbps() {
    python3 -c "print(f'{float(\"${1:-0}\")/1000:.2f}')"
}

gbps_from_kbps() {
    python3 -c "print(f'{float(\"${1:-0}\")/1e6:.3f}')"
}

sum_secnetperf_kbps() {
    python3 - "$1" <<'PY'
import re, sys
text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
# 优先使用 secnetperf 汇总的 Result 行（多连接时勿把每连接 @ kbps 相加）
for pat in (
    r'Result:\s*Download\s+(\d+)\s+kbps',
    r'Result:\s*Upload\s+(\d+)\s+kbps',
):
    m = re.search(pat, text)
    if m:
        print(m.group(1))
        raise SystemExit(0)
total = 0.0
for m in re.finditer(r'Download:\s+\d+\s+bytes\s+@\s+(\d+)\s+kbps', text):
    total += float(m.group(1))
if total > 0:
    print(total)
    raise SystemExit(0)
for m in re.finditer(r'(?:Download|Upload)\s+(\d+)\s+kbps', text):
    total += float(m.group(1))
print(total)
PY
}

ensure_routes() {
    sudo ip route replace "${TARGET}" dev "${LOCAL_IFACE}" src "${BIND}" 2>/dev/null || true
    ssh -o BatchMode=yes "$PEER" "sudo ip route replace ${BIND} dev ${PEER_IFACE} src ${TARGET} 2>/dev/null; true"
}

clear_peer_netem() {
    ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc del dev ${PEER_IFACE} root 2>/dev/null; true"
}

remote_kill_all() {
    ssh -o BatchMode=yes "$PEER" "killall -9 tcpquic-proxy secnetperf 2>/dev/null; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
    [[ -n "${CLIENT_PID:-}" ]] && kill -9 "$CLIENT_PID" 2>/dev/null || true
    CLIENT_PID=""
    PERF_SERVER_PID=""
}

deploy_msquic_perf() {
    ssh -o BatchMode=yes "$PEER" "mkdir -p ${REMOTE_DIR}"
    rsync -az "${SECNETPERF}" "${MSQUIC_LIB_DIR}/libmsquic.so"* "${PEER}:${REMOTE_DIR}/"
    ssh -o BatchMode=yes "$PEER" "cd ${REMOTE_DIR} && ln -sf libmsquic.so.2.5.9 libmsquic.so.2 2>/dev/null || ln -sf \$(ls libmsquic.so.2.* | head -1) libmsquic.so.2"
    rsync -az "${BIN}" "${PEER}:${REMOTE_BIN}"
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
        -subj "/CN=dgx-msquic-ca" -days 1 -sha256 >/dev/null 2>&1
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

run_secnetperf_server() {
    remote_kill_all
    # -port 在同时传 -bind 时会被忽略，必须写成 bind:ip:port
    ssh -o BatchMode=yes "$PEER" \
        "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_DIR}/secnetperf \
        -exec:maxtput -bind:${TARGET}:${PERF_PORT} \
        </dev/null >/tmp/dgx-secnetperf-server.log 2>&1 &"
    sleep 2
}

run_secnetperf_client() {
    local conns="$1" streams="$2" label="$3"
    local log="${TMP}/secnetperf-${label}.log"
    local extra=()
    if [[ "$streams" -gt 0 ]]; then
        extra+=("-streams:${streams}")
    fi
    LD_LIBRARY_PATH="${MSQUIC_LIB_DIR}" "${SECNETPERF}" \
        -target:"${TARGET}" -bind:"${BIND}" -port:"${PERF_PORT}" \
        -exec:maxtput -conns:"${conns}" "${extra[@]}" \
        -down:"${DURATION_SEC}s" -ptput:1 \
        >"${log}" 2>&1 || true
    local kbps mbps gbps
    kbps=$(sum_secnetperf_kbps "${log}")
    mbps=$(mbps_from_kbps "$kbps")
    gbps=$(gbps_from_kbps "$kbps")
    log "secnetperf ${label}: ${mbps} Mbps (${gbps} Gbps) [conns=${conns} streams=${streams}]"
    echo "${label}|secnetperf|${conns}|${streams}|${mbps}|${gbps}|${log}"
}

proxy_args() {
    local initrtt="$1" quic_conns="$2"
    printf '%s' \
        "--tuning custom --quic-fcw 1073741824 --quic-srw 1073741824 --quic-iw 4000 \
        --quic-initrtt-ms ${initrtt} --relay-io-size 1048576 --relay-inflight-bytes 1073741824 \
        --max-memory-mb 4096 --quic-connections ${quic_conns} --compress off"
}

start_proxy_stack() {
    local initrtt="$1" quic_conns="$2"
    remote_kill_all
    local args
    args=$(proxy_args "$initrtt" "$quic_conns")
    ssh -o BatchMode=yes "$PEER" "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
        --quic-listen ${TARGET}:${QUIC_PORT} \
        --allow-targets ${TARGET}/32,127.0.0.0/8 \
        --quic-cert ~/tcpquic-dgx-certs/server.crt \
        --quic-key ~/tcpquic-dgx-certs/server.key \
        --quic-ca ~/tcpquic-dgx-certs/ca.crt \
        ${args} </dev/null >/tmp/dgx-proxy-server.log 2>&1 &"
    for _ in $(seq 1 40); do
        sleep 0.3
        ssh -o BatchMode=yes "$PEER" "grep -q 'QUIC server listening' /tmp/dgx-proxy-server.log 2>/dev/null" && break
    done
    "$BIN" client \
        --quic-peer "${TARGET}:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
        --quic-cert "$CERT_DIR/client.crt" \
        --quic-key "$CERT_DIR/client.key" \
        --quic-ca "$CERT_DIR/ca.crt" \
        ${args} \
        >/tmp/dgx-proxy-client.log 2>&1 &
    CLIENT_PID=$!
    for _ in $(seq 1 50); do
        sleep 0.3
        grep -q "HTTP CONNECT listening" /tmp/dgx-proxy-client.log 2>/dev/null && return 0
    done
    return 1
}

measure_direct_curl() {
    curl -fsS --interface "$BIND" \
        "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
        -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' 2>/dev/null || echo 0
}

measure_proxy_parallel() {
    local n="$1"
    local i speed_dir="${TMP}/proxy-speeds"
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
print(sum(float(open(p).read().strip() or 0) for p in glob.glob(os.path.join(d, "*.bps"))))
PY
}

run_proxy_case() {
    local label="$1" initrtt="$2" quic_conns="$3" parallel="$4"
    start_proxy_stack "$initrtt" "$quic_conns"
    sleep 1
    local bps
    if [[ "$parallel" == "1" ]]; then
        bps=$(curl -fsS --interface "$BIND" \
            -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
            "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-payload.bin" \
            -o /dev/null --max-time "${DURATION_SEC}" -w '%{speed_download}' 2>/dev/null || echo 0)
    else
        bps=$(measure_proxy_parallel "$parallel")
    fi
    local mbps gbps
    mbps=$(mbps_from_bps "$bps")
    gbps=$(gbps_from_bps "$bps")
    log "tcpquic-proxy ${label}: ${mbps} Mbps (${gbps} Gbps)"
    remote_kill_all
    echo "${label}|tcpquic-proxy|${quic_conns}|${parallel}|${mbps}|${gbps}|"
}

cleanup() {
    set +e
    remote_kill_all
    clear_peer_netem
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

[[ -x "$SECNETPERF" ]] || { log "missing $SECNETPERF — build with: cmake -B build -DQUIC_BUILD_PERF=ON && cmake --build build --target secnetperf"; exit 1; }
[[ -x "$BIN" ]] || { log "missing $BIN"; exit 1; }

mkdir -p "$TMP"
check_required_sysctls
ensure_routes
clear_peer_netem
deploy_msquic_perf
start_busybox_http
ensure_payload
generate_certs

{
    echo "# msquic secnetperf vs tcpquic-proxy（无时延 LAN）"
    echo ""
    echo "- 时间: $(date -Iseconds)"
    echo "- 本机: ${BIND} | 对端: ${TARGET}"
    echo "- secnetperf: \`-exec:maxtput -down:${DURATION_SEC}s\`（server bind 为 \`ip:port\`）"
    echo "- 同一套 libmsquic: ${MSQUIC_LIB_DIR}"
    echo ""
} > "$REPORT"

log "TCP 基线 (direct curl)"
tcp_bps=$(measure_direct_curl)
tcp_mbps=$(mbps_from_bps "$tcp_bps")
tcp_gbps=$(gbps_from_bps "$tcp_bps")
log "direct TCP: ${tcp_mbps} Mbps (${tcp_gbps} Gbps)"
echo "## 参考：裸 TCP (curl → busybox)" >> "$REPORT"
echo "- **${tcp_mbps} Mbps** (${tcp_gbps} Gbps)" >> "$REPORT"
echo "" >> "$REPORT"

echo "## 对比表" >> "$REPORT"
echo "" >> "$REPORT"
echo "| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |" >> "$REPORT"
echo "|------|------|---------------------------|------|------|------------------|" >> "$REPORT"

run_secnetperf_server
r1=$(run_secnetperf_client 1 0 "msquic-1conn")
msquic_1_mbps=$(echo "$r1" | cut -d'|' -f5)
IFS='|' read -r _ _ c _ m g _ <<< "$r1"
echo "| secnetperf | 单连接下载 | conns=1 | ${m} | ${g} | 100% |" >> "$REPORT"

r2=$(run_secnetperf_client 16 0 "msquic-16conn")
IFS='|' read -r _ _ c _ m g _ <<< "$r2"
echo "| secnetperf | 16 连接下载 | conns=16 | ${m} | ${g} | — |" >> "$REPORT"

run_secnetperf_server
r3=$(run_secnetperf_client 1 16 "msquic-1conn-16stream")
IFS='|' read -r _ _ c s m g _ <<< "$r3"
echo "| secnetperf | 单连接 16 stream | conns=1 streams=16 | ${m} | ${g} | — |" >> "$REPORT"

remote_kill_all

p1=$(run_proxy_case "proxy-1x1" 1 1 1)
IFS='|' read -r _ _ qc pc m g _ <<< "$p1"
pct=$(python3 -c "m=float('$m'); b=float('$msquic_1_mbps'); print(f'{m/b*100:.1f}%' if b>0 else 'n/a')")
echo "| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | ${m} | ${g} | ${pct} |" >> "$REPORT"

p2=$(run_proxy_case "proxy-16x16" 1 16 16)
IFS='|' read -r _ _ qc pc m g _ <<< "$p2"
echo "| tcpquic-proxy | 多流 | quic=16 curl=16 | ${m} | ${g} | — |" >> "$REPORT"

p3=$(run_proxy_case "proxy-4x16" 1 16 4)
IFS='|' read -r _ _ qc pc m g _ <<< "$p3"
echo "| tcpquic-proxy | 多流（最佳） | quic=16 curl=4 | ${m} | ${g} | — |" >> "$REPORT"

echo "" >> "$REPORT"
echo "## 说明" >> "$REPORT"
echo "" >> "$REPORT"
echo "- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。" >> "$REPORT"
echo "- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。" >> "$REPORT"
echo "- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。" >> "$REPORT"
echo "" >> "$REPORT"
echo "报告: ${REPORT}" >> "$REPORT"

log "done -> ${REPORT}"
cat "$REPORT"
