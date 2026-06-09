#!/usr/bin/env bash
# DGX 双机互联 netem 公共函数
# 原则：本机 200G 网卡禁止 netem；时延/丢包模拟仅在对端 200G 网卡操作。

LOCAL_IFACE="${LOCAL_IFACE:-enp1s0f0np0}"
PEER_IFACE="${PEER_IFACE:-enp1s0f0np0}"
PEER="${PEER:-jack@169.254.59.196}"
NETEM_LIMIT="${NETEM_LIMIT:-1000000}"

ensure_no_local_netem() {
    local q
    q=$(tc qdisc show dev "$LOCAL_IFACE" 2>/dev/null | head -1 || true)
    if echo "$q" | grep -q netem; then
        echo "[netem] 清除本机 ${LOCAL_IFACE} netem: $q" >&2
        sudo -n tc qdisc del dev "$LOCAL_IFACE" root 2>/dev/null || true
    fi
}

ensure_no_peer_netem() {
    local q
    q=$(ssh -o BatchMode=yes "$PEER" "tc qdisc show dev ${PEER_IFACE}" 2>/dev/null | head -1 || true)
    if echo "$q" | grep -q netem; then
        echo "[netem] 清除对端 ${PEER_IFACE} netem: $q" >&2
        ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc del dev ${PEER_IFACE} root 2>/dev/null; true"
    fi
}

verify_local_no_netem() {
    local q
    q=$(tc qdisc show dev "$LOCAL_IFACE" 2>/dev/null | head -1 || true)
    ! echo "$q" | grep -q netem
}

peer_netem_args() {
    local delay="${1:-100ms}"
    local loss="${2:-5%}"
    local args="delay ${delay}"
    if [[ -n "$loss" && "$loss" != "0" && "$loss" != "0%" ]]; then
        args="${args} loss ${loss}"
    fi
    args="${args} limit ${NETEM_LIMIT}"
    printf '%s' "$args"
}

apply_peer_netem() {
    local delay="${1:-100ms}"
    local loss="${2:-5%}"
    local args
    args=$(peer_netem_args "$delay" "$loss")
    echo "[netem] 对端 ${PEER_IFACE} 应用: tc qdisc replace dev ${PEER_IFACE} root netem ${args}" >&2
    ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc replace dev ${PEER_IFACE} root netem ${args}"
}

cleanup_peer_netem() {
    ssh -o BatchMode=yes "$PEER" "sudo -n tc qdisc del dev ${PEER_IFACE} root 2>/dev/null; true"
}

verify_peer_netem() {
    local delay="${1:-100ms}"
    ssh -o BatchMode=yes "$PEER" "tc qdisc show dev ${PEER_IFACE}" | grep -q "netem.*delay ${delay}"
}
