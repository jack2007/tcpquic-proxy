#!/usr/bin/env bash
# 方案一：DGX Spark 200G 直连 — 链路连通性与健康检查
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dgx-interconnect-netem-common.sh
source "${ROOT}/dgx-interconnect-netem-common.sh"

LOCAL_HOST="${LOCAL_HOST:-$(hostname)}"
LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
PEER_IP="${PEER_IP:-169.254.59.196}"
PEER="${PEER:-jack@${PEER_IP}}"
IFACE="${IFACE:-enp1s0f0np0}"
IPERF_PORT="${IPERF_PORT:-15201}"

PASS=0
FAIL=0
REPORT="${REPORT:-/tmp/dgx-interconnect-scheme1-$(date +%Y%m%d-%H%M%S).txt}"

log() { printf '%s\n' "$*" | tee -a "$REPORT"; }
pass() { PASS=$((PASS + 1)); log "  [PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); log "  [FAIL] $*"; }

check_link() {
    local host="$1" ip="$2" side="$3"
    log "TC-1.1 链路层状态 ($side: $host)"
    local out speed duplex link
    if [[ "$side" == "local" ]]; then
        out=$(ethtool "$IFACE" 2>/dev/null || true)
    else
        out=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$PEER" "ethtool ${IFACE}" 2>/dev/null || true)
    fi
    speed=$(echo "$out" | awk -F': ' '/Speed:/{print $2; exit}')
    duplex=$(echo "$out" | awk -F': ' '/Duplex:/{print $2; exit}')
    link=$(echo "$out" | awk -F': ' '/Link detected:/{print $2; exit}')
    log "    Speed=$speed Duplex=$duplex Link=$link"
    if [[ "$speed" == "200000Mb/s" && "$duplex" == "Full" && "$link" == "yes" ]]; then
        pass "TC-1.1 $side 200G 全双工链路正常"
    else
        fail "TC-1.1 $side 链路异常 (期望 200000Mb/s Full yes)"
    fi
}

main() {
    : >"$REPORT"
    log "=== 方案一：链路连通性与健康检查 ==="
    log "时间: $(date -Iseconds)"
    log "本机: ${LOCAL_HOST} (${LOCAL_IP})  对端: ${PEER}"
    log "网卡: ${IFACE}"
    log ""

    log "前置：确保本机 200G 网卡无 netem（时延/丢包模拟仅在对端操作）"
    ensure_no_local_netem
    if verify_local_no_netem; then
        pass "本机 ${LOCAL_IFACE} 无 netem"
    else
        fail "本机 ${LOCAL_IFACE} 仍存在 netem，请先清除"
    fi
    log ""

    log "TC-1.2 路由与邻居"
    local route dev
    route=$(ip route get "$PEER_IP" 2>/dev/null || true)
    log "    $route"
    dev=$(echo "$route" | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}')
    if [[ "$dev" == "$IFACE" ]]; then
        pass "TC-1.2 路由走 ${IFACE}"
    else
        fail "TC-1.2 路由未走 ${IFACE} (实际 dev=$dev)"
    fi
    local neigh
    neigh=$(ip neigh show dev "$IFACE" "$PEER_IP" 2>/dev/null || true)
    log "    neigh: ${neigh:-<none>}"
    if echo "$neigh" | grep -qE 'REACHABLE|STALE|DELAY|PROBE'; then
        pass "TC-1.2 邻居表存在对端条目"
    else
        ping -c 2 -W 2 -I "$IFACE" "$PEER_IP" >/dev/null 2>&1 || true
        neigh=$(ip neigh show dev "$IFACE" "$PEER_IP" 2>/dev/null || true)
        if echo "$neigh" | grep -qE 'REACHABLE|STALE|DELAY|PROBE'; then
            pass "TC-1.2 ping 后邻居可达"
        else
            fail "TC-1.2 无法解析对端 L2 地址"
        fi
    fi
    log ""

    log "TC-1.3 ICMP 连通 (20 ping)"
    local ping_out loss
    ping_out=$(ping -c 20 -i 0.2 -W 2 -I "$IFACE" "$PEER_IP" 2>&1 || true)
    loss=$(echo "$ping_out" | grep -oP '\d+(?=% packet loss)' || echo "100")
    local rtt received
    rtt=$(echo "$ping_out" | awk -F'/' '/rtt min/{print $5}')
    received=$(echo "$ping_out" | awk -F', ' '/received/{print $2}' | awk '{print $1}')
    log "    收到: ${received:-?}/20  丢包率: ${loss:-?}%  平均 RTT: ${rtt:-?} ms"
    if [[ "${loss:-100}" == "0" ]]; then
        pass "TC-1.3 ICMP 0% 丢包"
    elif [[ "${received:-0}" -gt 0 ]] && [[ "${loss:-100}" =~ ^[0-9.]+$ ]] && awk -v l="$loss" 'BEGIN{exit (l < 100)?0:1}'; then
        pass "TC-1.3 ICMP 可达 (丢包 ${loss}%，可能受 netem 影响)"
    else
        fail "TC-1.3 ICMP 不可达"
    fi
    log ""

    log "TC-1.4 SSH 免密"
    local remote_host
    if remote_host=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$PEER" hostname 2>/dev/null); then
        pass "TC-1.4 SSH 成功，对端主机名=$remote_host"
    else
        fail "TC-1.4 SSH 失败"
    fi
    log ""

    log "TC-1.5 对端 sudo 免密"
    if ssh -o BatchMode=yes -o ConnectTimeout=5 "$PEER" 'sudo -n true' 2>/dev/null; then
        pass "TC-1.5 对端 sudo 免密正常"
    else
        fail "TC-1.5 对端 sudo 失败"
    fi
    log ""

    log "TC-1.6 TCP 端口探测 (iperf3 ${IPERF_PORT})"
    ssh -o BatchMode=yes "$PEER" "pkill -x iperf3 2>/dev/null; true"
    ssh -o BatchMode=yes "$PEER" "iperf3 -s -p ${IPERF_PORT} -D"
    sleep 1
    if nc -z -w 3 "$PEER_IP" "$IPERF_PORT" 2>/dev/null; then
        pass "TC-1.6 TCP ${IPERF_PORT} 连通"
    else
        fail "TC-1.6 TCP ${IPERF_PORT} 不可达"
    fi
    ssh -o BatchMode=yes "$PEER" "pkill -x iperf3 2>/dev/null; true"
    log ""

    log "TC-1.7 Jumbo MTU (ping payload 8972)"
    if ping -c 3 -M do -s 8972 -W 2 -I "$IFACE" "$PEER_IP" >/dev/null 2>&1; then
        pass "TC-1.7 MTU 9000 路径可用"
    else
        fail "TC-1.7 Jumbo frame 探测失败"
    fi
    log ""

    log "TC-1.8 接口错误计数"
    local rx_err tx_err
    rx_err=$(ip -s link show "$IFACE" | awk '/RX:/{getline; print $3}')
    tx_err=$(ip -s link show "$IFACE" | awk '/TX:/{getline; print $3}')
    log "    RX errors=$rx_err  TX errors=$tx_err"
    if [[ "${rx_err:-1}" == "0" && "${tx_err:-1}" == "0" ]]; then
        pass "TC-1.8 本机接口无错误计数"
    else
        fail "TC-1.8 本机接口存在错误 (RX=$rx_err TX=$tx_err)"
    fi
    log ""

    check_link "$LOCAL_HOST" "$LOCAL_IP" "local"
    check_link "spark-peer" "$PEER_IP" "remote"
    log ""

    log "=== 方案一汇总: PASS=$PASS FAIL=$FAIL ==="
    log "报告: $REPORT"
    [[ "$FAIL" -eq 0 ]]
}

main "$@"
