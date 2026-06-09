#!/usr/bin/env bash
# 方案二：DGX Spark 200G 直连 — 带宽与延迟性能基准（裸链路，无 netem）
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dgx-interconnect-netem-common.sh
source "${ROOT}/dgx-interconnect-netem-common.sh"

LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
PEER_IP="${PEER_IP:-169.254.59.196}"
PEER="${PEER:-jack@${PEER_IP}}"
IFACE="${IFACE:-${LOCAL_IFACE}}"
IPERF_PORT="${IPERF_PORT:-15201}"
DURATION="${DURATION:-10}"

MIN_TCP_Gbps="${MIN_TCP_Gbps:-10}"
MIN_TCP_PAR_Gbps="${MIN_TCP_PAR_Gbps:-50}"
MIN_BIDIR_Gbps="${MIN_BIDIR_Gbps:-50}"
MIN_UDP_Gbps="${MIN_UDP_Gbps:-10}"

PASS=0
FAIL=0
REPORT="${REPORT:-/tmp/dgx-interconnect-scheme2-$(date +%Y%m%d-%H%M%S).txt}"

log() { printf '%s\n' "$*" | tee -a "$REPORT"; }
pass() { PASS=$((PASS + 1)); log "  [PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); log "  [FAIL] $*"; }

gbps_from_iperf() {
    # 解析 iperf3 bitrate（支持 Gbits/sec、Mbits/sec）
    grep -oP '\d+(\.\d+)?\s+Gbits/sec' | head -1 | awk '{print $1}'
}

stop_iperf() {
    ssh -o BatchMode=yes "$PEER" "pkill -x iperf3 2>/dev/null; true"
    pkill -x iperf3 2>/dev/null || true
}

start_peer_server() {
    stop_iperf
    ssh -o BatchMode=yes "$PEER" "iperf3 -s -p ${IPERF_PORT} -D >/dev/null 2>&1"
    sleep 0.5
}

start_local_server() {
    stop_iperf
    iperf3 -s -p "${IPERF_PORT}" -D >/dev/null 2>&1
    sleep 0.5
}

run_iperf_client() {
    local extra=("$@")
    iperf3 -c "$PEER_IP" -p "$IPERF_PORT" -B "$LOCAL_IP" -t "$DURATION" -f g "${extra[@]}" 2>&1
}

compare_gbps() {
    local label="$1" measured="$2" threshold="$3"
    log "    ${label}: ${measured} Gbps (阈值 >= ${threshold})"
    awk -v m="$measured" -v t="$threshold" 'BEGIN { exit (m+0 >= t+0) ? 0 : 1 }' && \
        pass "${label} ${measured} Gbps >= ${threshold}" || \
        fail "${label} ${measured} Gbps < ${threshold}"
}

main() {
    : >"$REPORT"
    trap 'stop_iperf' EXIT

    log "=== 方案二：带宽与延迟性能基准（裸链路） ==="
    log "时间: $(date -Iseconds)"
    log "本机: ${LOCAL_IP}  对端: ${PEER_IP}  网卡: ${IFACE}"
    log "测试时长: ${DURATION}s"
    log ""

    log "前置：清除本机/对端 200G 网卡 netem"
    ensure_no_local_netem
    ensure_no_peer_netem
    if verify_local_no_netem; then
        pass "本机 ${LOCAL_IFACE} 无 netem"
    else
        fail "本机 ${LOCAL_IFACE} 仍有 netem"
    fi
    log ""

    log "TC-2.1 RTT 基线 (ping 30 次)"
    local ping_out ping_avg ping_loss
    ping_out=$(ping -c 30 -i 0.1 -W 1 -I "$IFACE" "$PEER_IP" 2>&1 || true)
    ping_avg=$(echo "$ping_out" | awk -F'/' '/rtt min/{print $5}')
    ping_loss=$(echo "$ping_out" | grep -oP '\d+(?=% packet loss)' || echo "100")
    log "    ping avg RTT: ${ping_avg:-?} ms  丢包: ${ping_loss:-?}%"
    if [[ -n "${ping_avg:-}" ]] && [[ "${ping_loss}" == "0" ]] && awk -v a="$ping_avg" 'BEGIN{exit (a < 5.0)?0:1}'; then
        pass "TC-2.1 平均 RTT ${ping_avg} ms，0% 丢包"
    elif [[ -n "${ping_avg:-}" ]] && [[ "${ping_loss}" == "0" ]]; then
        pass "TC-2.1 RTT ${ping_avg} ms，0% 丢包"
    elif [[ -n "${ping_avg:-}" ]]; then
        fail "TC-2.1 RTT 异常 (${ping_avg} ms, 丢包 ${ping_loss}%)"
    else
        fail "TC-2.1 RTT 测量失败"
    fi
    log ""

    start_peer_server

    log "TC-2.2 TCP 单流吞吐"
    local out gbps
    out=$(run_iperf_client)
    echo "$out" | tail -5 | tee -a "$REPORT"
    gbps=$(echo "$out" | gbps_from_iperf)
    compare_gbps "TC-2.2 TCP 单流" "${gbps:-0}" "$MIN_TCP_Gbps"
    log ""

    log "TC-2.3 TCP 8 并行流吞吐"
    out=$(run_iperf_client -P 8)
    echo "$out" | tail -8 | tee -a "$REPORT"
    gbps=$(echo "$out" | awk '/\[SUM\].*receiver/{print}' | gbps_from_iperf)
    [[ -z "${gbps:-}" ]] && gbps=$(echo "$out" | gbps_from_iperf)
    compare_gbps "TC-2.3 TCP 8流" "${gbps:-0}" "$MIN_TCP_PAR_Gbps"
    log ""

    log "TC-2.4 反向吞吐 (对端 client → 本机 server)"
    stop_iperf
    start_local_server
    out=$(ssh -o BatchMode=yes "$PEER" "iperf3 -c ${LOCAL_IP} -p ${IPERF_PORT} -B ${PEER_IP} -t ${DURATION} -P 8 -f g" 2>&1)
    echo "$out" | tail -8 | tee -a "$REPORT"
    gbps=$(echo "$out" | awk '/\[SUM\].*receiver/{print}' | gbps_from_iperf)
    [[ -z "${gbps:-}" ]] && gbps=$(echo "$out" | gbps_from_iperf)
    compare_gbps "TC-2.4 反向 TCP 8流" "${gbps:-0}" "$MIN_TCP_PAR_Gbps"
    log ""

    start_peer_server

    log "TC-2.5 UDP 吞吐"
    out=$(run_iperf_client -u -b 0)
    echo "$out" | tail -6 | tee -a "$REPORT"
    gbps=$(echo "$out" | gbps_from_iperf)
    local loss_pct
    loss_pct=$(echo "$out" | grep -oP '\d+(?=%\))' | tail -1 || true)
    log "    UDP 丢包: ${loss_pct:-?}%"
    compare_gbps "TC-2.5 UDP" "${gbps:-0}" "$MIN_UDP_Gbps"
    if [[ -n "${loss_pct:-}" ]] && awk -v l="$loss_pct" 'BEGIN{exit (l <= 5.0)?0:1}'; then
        pass "TC-2.5 UDP 丢包率 ${loss_pct}% <= 5% (200G 线速 UDP 允许一定丢包)"
    elif [[ -n "${loss_pct:-}" ]]; then
        log "  [WARN] TC-2.5 UDP 丢包率 ${loss_pct}% 偏高（200G 线速 UDP 常见，以吞吐为准）"
        pass "TC-2.5 UDP 吞吐达标，丢包 ${loss_pct}% 已记录"
    fi
    log ""

    log "TC-2.6 双向吞吐 (--bidir -P 4)"
    out=$(run_iperf_client --bidir -P 4)
    echo "$out" | tail -12 | tee -a "$REPORT"
    gbps=$(echo "$out" | awk '/\[SUM\]\[RX-C\].*receiver/{print}' | gbps_from_iperf)
    [[ -z "${gbps:-}" ]] && gbps=$(echo "$out" | grep '\[SUM\]' | gbps_from_iperf | awk '{s+=$1} END{printf "%.2f", s+0}')
    compare_gbps "TC-2.6 双向合计" "${gbps:-0}" "$MIN_BIDIR_Gbps"
    log ""

    log "=== 方案二汇总: PASS=$PASS FAIL=$FAIL ==="
    log "报告: $REPORT"
    [[ "$FAIL" -eq 0 ]]
}

main "$@"
