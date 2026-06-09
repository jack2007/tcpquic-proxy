#!/usr/bin/env bash
# 方案三：对端 200G 网卡 netem 时延/丢包模拟验证
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dgx-interconnect-netem-common.sh
source "${ROOT}/dgx-interconnect-netem-common.sh"

LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
PEER_IP="${PEER_IP:-169.254.59.196}"
IPERF_PORT="${IPERF_PORT:-15203}"
DURATION="${DURATION:-10}"
DELAY="${DELAY:-100ms}"
LOSS="${LOSS:-5%}"
MIN_NETEM_RTT_MS="${MIN_NETEM_RTT_MS:-80}"
MIN_NETEM_LOSS_PCT="${MIN_NETEM_LOSS_PCT:-2}"
MAX_NETEM_TCP_Gbps="${MAX_NETEM_TCP_Gbps:-30}"

PASS=0
FAIL=0
REPORT="${REPORT:-/tmp/dgx-interconnect-scheme3-$(date +%Y%m%d-%H%M%S).txt}"

log() { printf '%s\n' "$*" | tee -a "$REPORT"; }
pass() { PASS=$((PASS + 1)); log "  [PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); log "  [FAIL] $*"; }

gbps_from_iperf() {
    grep -oP '\d+(\.\d+)?\s+Gbits/sec' | head -1 | awk '{print $1}'
}

stop_iperf() {
    ssh -o BatchMode=yes "$PEER" "pkill -x iperf3 2>/dev/null; true"
    pkill -x iperf3 2>/dev/null || true
}

main() {
    : >"$REPORT"
    trap 'cleanup_peer_netem; stop_iperf' EXIT

    log "=== 方案三：对端 200G netem 时延/丢包模拟 ==="
    log "时间: $(date -Iseconds)"
    log "本机: ${LOCAL_IP} (${LOCAL_IFACE})"
    log "对端: ${PEER_IP} (${PEER_IFACE})"
    log "netem 参数: delay=${DELAY} loss=${LOSS} limit=${NETEM_LIMIT}"
    log ""

    log "TC-3.1 本机 200G 网卡无 netem"
    ensure_no_local_netem
    if verify_local_no_netem; then
        pass "TC-3.1 本机 ${LOCAL_IFACE} 无 netem"
    else
        fail "TC-3.1 本机 ${LOCAL_IFACE} 仍存在 netem"
    fi
    log ""

    log "TC-3.2 对端 ${PEER_IFACE} 应用 netem"
    ensure_no_peer_netem
    if apply_peer_netem "$DELAY" "$LOSS" && verify_peer_netem "$DELAY"; then
        pass "TC-3.2 对端 netem 已应用 (${DELAY} ${LOSS})"
    else
        fail "TC-3.2 对端 netem 应用失败"
    fi
    ssh -o BatchMode=yes "$PEER" "tc qdisc show dev ${PEER_IFACE}" | tee -a "$REPORT"
    log ""

    log "TC-3.3 netem 下 RTT (ping 30)"
    sleep 1
    local ping_out ping_avg
    ping_out=$(ping -c 30 -i 0.2 -W 2 -I "$LOCAL_IFACE" "$PEER_IP" 2>&1 || true)
    ping_avg=$(echo "$ping_out" | awk -F'/' '/rtt min/{print $5}')
    log "    平均 RTT: ${ping_avg:-?} ms (期望 >= ${MIN_NETEM_RTT_MS} ms)"
    if [[ -n "${ping_avg:-}" ]] && awk -v a="$ping_avg" -v m="$MIN_NETEM_RTT_MS" 'BEGIN{exit (a+0 >= m+0)?0:1}'; then
        pass "TC-3.3 RTT ${ping_avg} ms 反映 ${DELAY} 时延"
    else
        fail "TC-3.3 RTT ${ping_avg:-?} ms 未达预期 (>= ${MIN_NETEM_RTT_MS})"
    fi
    log ""

    log "TC-3.4 netem 下 ICMP 丢包 (ping 100)"
    local ping_loss
    ping_out=$(ping -c 100 -i 0.1 -W 2 -I "$LOCAL_IFACE" "$PEER_IP" 2>&1 || true)
    ping_loss=$(echo "$ping_out" | grep -oP '\d+(?=% packet loss)' || echo "0")
    log "    丢包率: ${ping_loss}% (期望 >= ${MIN_NETEM_LOSS_PCT}%)"
    if awk -v l="$ping_loss" -v m="$MIN_NETEM_LOSS_PCT" 'BEGIN{exit (l+0 >= m+0)?0:1}'; then
        pass "TC-3.4 丢包 ${ping_loss}% 反映 ${LOSS} 模拟"
    else
        fail "TC-3.4 丢包 ${ping_loss}% 低于预期 (>= ${MIN_NETEM_LOSS_PCT}%)"
    fi
    log ""

    log "TC-3.5 netem 下 TCP 吞吐 (对端 client→本机 server，走对端 egress netem)"
    stop_iperf
    iperf3 -s -p "${IPERF_PORT}" -D
    sleep 1
    local out gbps
    out=$(ssh -o BatchMode=yes "$PEER" "iperf3 -c ${LOCAL_IP} -p ${IPERF_PORT} -B ${PEER_IP} -t ${DURATION} -P 4 -f g" 2>&1 || true)
    echo "$out" | tail -6 | tee -a "$REPORT"
    gbps=$(echo "$out" | awk '/\[SUM\].*receiver/{print}' | gbps_from_iperf)
    [[ -z "${gbps:-}" ]] && gbps=$(echo "$out" | gbps_from_iperf)
    log "    TCP 4流: ${gbps:-0} Gbps (期望 < ${MAX_NETEM_TCP_Gbps} Gbps)"
    if [[ -n "${gbps:-}" ]] && awk -v g="$gbps" -v m="$MAX_NETEM_TCP_Gbps" 'BEGIN{exit (g+0 < m+0)?0:1}'; then
        pass "TC-3.5 netem 下吞吐 ${gbps} Gbps 受限于时延/丢包"
    else
        fail "TC-3.5 netem 下吞吐 ${gbps:-0} Gbps 未体现限制"
    fi
    log ""

    log "TC-3.6 清除对端 netem"
    cleanup_peer_netem
    sleep 0.5
    local pq
    pq=$(ssh -o BatchMode=yes "$PEER" "tc qdisc show dev ${PEER_IFACE}" 2>/dev/null | head -1 || true)
    log "    对端 qdisc: $pq"
    if ! echo "$pq" | grep -q netem; then
        pass "TC-3.6 对端 netem 已清除"
    else
        fail "TC-3.6 对端 netem 清除失败"
    fi
    log ""

    log "=== 方案三汇总: PASS=$PASS FAIL=$FAIL ==="
    log "报告: $REPORT"
    [[ "$FAIL" -eq 0 ]]
}

main "$@"
