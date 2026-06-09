#!/usr/bin/env bash
# DGX Spark 双机 200Gbps 互联 — 执行全部测试方案
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TS=$(date +%Y%m%d-%H%M%S)
MASTER_REPORT="/tmp/dgx-interconnect-report-${TS}.txt"

log() { printf '%s\n' "$*" | tee -a "$MASTER_REPORT"; }

main() {
    : >"$MASTER_REPORT"
    log "========================================"
    log " DGX Spark 200G 互联完整测试"
    log " 时间: $(date -Iseconds)"
    log " 本机: $(hostname) ($(hostname -I | awk '{print $1}'))"
    log "========================================"
    log ""

    log ">>> 执行方案一：链路连通性与健康检查"
    if "${ROOT}/dgx-interconnect-scheme1-connectivity.sh"; then
        log "方案一: 全部通过"
    else
        log "方案一: 存在失败项 (退出码 $?)"
    fi
    log ""

    log ">>> 执行方案二：带宽与延迟性能基准（裸链路）"
    if "${ROOT}/dgx-interconnect-scheme2-performance.sh"; then
        log "方案二: 全部通过"
    else
        log "方案二: 存在失败项 (退出码 $?)"
    fi
    log ""

    log ">>> 执行方案三：对端 200G netem 时延/丢包模拟"
    if "${ROOT}/dgx-interconnect-scheme3-netem.sh"; then
        log "方案三: 全部通过"
    else
        log "方案三: 存在失败项 (退出码 $?)"
    fi
    log ""

    log "完整报告目录: /tmp/dgx-interconnect-*"
    log "主报告: $MASTER_REPORT"
}

main "$@"
