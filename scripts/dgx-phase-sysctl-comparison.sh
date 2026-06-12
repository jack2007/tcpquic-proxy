#!/usr/bin/env bash
# Phase 3 vs Phase 4 双方案：sysctl 调优后 DGX 吞吐 + perf 热点对比
#
# 用法:
#   ./scripts/dgx-phase-sysctl-comparison.sh
#   DURATION_SEC=30 PERF_SEC=25 ./scripts/dgx-phase-sysctl-comparison.sh
#
# 输出: docs/dgx-sysctl-comparison-<timestamp>/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TS="$(date +%Y%m%d-%H%M%S)"
OUT_ROOT="${OUT_ROOT:-${ROOT}/docs/dgx-sysctl-comparison-${TS}}"
DURATION_SEC="${DURATION_SEC:-30}"
PERF_SEC="${PERF_SEC:-25}"
SECNETPERF="${SECNETPERF:-${ROOT}/build/msquic/bin/Release/secnetperf}"
MSQUIC_LIB_DIR="${MSQUIC_LIB_DIR:-${ROOT}/build/msquic/bin/Release}"

log() { printf '[dgx-sysctl-cmp] %s\n' "$*" >&2; }

capture_sysctl() {
    local out="$1"
    {
        echo "# local $(hostname -s)"
        sysctl net.core.rmem_max net.core.wmem_max \
            net.core.rmem_default net.core.wmem_default \
            net.ipv4.tcp_rmem net.ipv4.tcp_wmem 2>/dev/null || true
        echo ""
        echo "# peer 169.254.59.196"
        ssh -o BatchMode=yes jack@169.254.59.196 \
            "sysctl net.core.rmem_max net.core.wmem_max \
            net.core.rmem_default net.core.wmem_default \
            net.ipv4.tcp_rmem net.ipv4.tcp_wmem" 2>/dev/null || true
    } > "$out"
}

run_bench() {
    local label="$1"
    local bin="$2"
    local dest="$3"
    log "bench ${label}"
    BIN="${bin}" \
        SECNETPERF="${SECNETPERF}" \
        MSQUIC_LIB_DIR="${MSQUIC_LIB_DIR}" \
        DURATION_SEC="${DURATION_SEC}" \
        REPORT="${dest}/bench-report.md" \
        "${ROOT}/scripts/dgx-msquic-vs-proxy-bench.sh" \
        >"${dest}/bench.log" 2>&1
    cp "${dest}/bench-report.md" "${dest}/report.md"
}

run_perf() {
    local label="$1"
    local bin="$2"
    local case_name="$3"
    local dest="$4"
    log "perf ${label} CASE=${case_name}"
    mkdir -p "${dest}"
    cp "${OUT_ROOT}/sysctl.txt" "${dest}/sysctl.txt" 2>/dev/null || true
    BIN="${bin}" \
        SECNETPERF="${SECNETPERF}" \
        MSQUIC_LIB_DIR="${MSQUIC_LIB_DIR}" \
        DURATION_SEC="${PERF_SEC}" \
        CASE="${case_name}" \
        OUT_DIR="${dest}" \
        "${ROOT}/scripts/dgx-perf-profile.sh" \
        >"${dest}/perf.log" 2>&1
}

parse_bench_gbps() {
    local report="$1"
    python3 - "$report" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
rows = {}
for m in re.finditer(r'\|\s*([^\|]+?)\s*\|\s*([^\|]+?)\s*\|\s*([^\|]+?)\s*\|\s*([^\|]+?)\s*\|\s*([^\|]+?)\s*\|', text):
    cols = [c.strip() for c in m.groups()]
    if len(cols) < 5:
        continue
    tool, scene, cfg, _mbps, gbps = cols[0], cols[1], cols[2], cols[3], cols[4]
    key = f"{tool}|{scene}|{cfg}"
    if 'Gbps' in gbps:
        rows[key] = gbps
    if '参考' in text and '裸 TCP' in scene:
        rows['direct-tcp'] = gbps
for line in text.splitlines():
    if line.startswith('- **') and 'Gbps' in line and '裸 TCP' in text.split(line)[0][-200:]:
        m = re.search(r'\*\*([\d.]+)\s*Mbps\*\*\s*\(([\d.]+)\s*Gbps\)', line)
        if m:
            rows['direct-tcp'] = m.group(2) + ' Gbps'
            break
# fallback direct tcp line
m = re.search(r'参考：裸 TCP.*?-\s*\*\*[\d.]+\s*Mbps\*\*\s*\(([\d.]+)\s*Gbps\)', text, re.S)
if m:
    rows['direct-tcp'] = m.group(1) + ' Gbps'
print('\n'.join(f"{k}={v}" for k, v in sorted(rows.items())))
PY
}

write_summary() {
    local summary="${OUT_ROOT}/summary.md"
    {
        echo "# Phase 3 vs Phase 4 — sysctl 调优后 DGX 吞吐 + perf（${TS}）"
        echo ""
        echo "- bench 时长: ${DURATION_SEC}s | perf 采样: ${PERF_SEC}s"
        echo "- 输出目录: \`${OUT_ROOT#${ROOT}/}\`"
        echo ""
        echo "## 内核 socket 参数"
        echo '```'
        cat "${OUT_ROOT}/sysctl.txt"
        echo '```'
        echo ""
        echo "## 吞吐对比"
        echo ""
        echo "| 指标 | Phase 3 | Phase 4 always-pending | Phase 4 sync-write |"
        echo "|------|---------|------------------------|---------------------|"
        for key in direct-tcp proxy-1x1 proxy-16x16 proxy-4x16; do
            local p3 p4a p4b
            p3=$(grep -E "^${key}=" "${OUT_ROOT}/phase3/throughput.txt" 2>/dev/null | cut -d= -f2- || echo "—")
            p4a=$(grep -E "^${key}=" "${OUT_ROOT}/phase4-always-pending/throughput.txt" 2>/dev/null | cut -d= -f2- || echo "—")
            p4b=$(grep -E "^${key}=" "${OUT_ROOT}/phase4-sync-write/throughput.txt" 2>/dev/null | cut -d= -f2- || echo "—")
            case "$key" in
                direct-tcp) label="裸 TCP" ;;
                proxy-1x1) label="proxy-1×1" ;;
                proxy-16x16) label="proxy-16×16" ;;
                proxy-4x16) label="proxy-4×16" ;;
            esac
            echo "| ${label} | ${p3} | ${p4a} | ${p4b} |"
        done
        echo ""
        echo "## Perf 热点目录"
        echo ""
        for variant in phase3 phase4-always-pending phase4-sync-write; do
            echo "### ${variant}"
            for case_dir in perf-proxy-1x1 perf-proxy-4x16; do
                if [[ -f "${OUT_ROOT}/${variant}/${case_dir}/summary.md" ]]; then
                    echo "- \`${variant}/${case_dir}/summary.md\`"
                fi
            done
            echo ""
        done
    } > "$summary"
}

extract_throughput_txt() {
    local variant_dir="$1"
    local report="${variant_dir}/bench-report.md"
    python3 - "$report" "${variant_dir}/throughput.txt" <<'PY'
import re, sys
report, out = sys.argv[1], sys.argv[2]
text = open(report).read()
rows = {}
m = re.search(r'参考：裸 TCP.*?-\s*\*\*[\d.]+\s*Mbps\*\*\s*\(([\d.]+)\s*Gbps\)', text, re.S)
if m:
    rows['direct-tcp'] = m.group(1) + ' Gbps'
mapping = {
    ('tcpquic-proxy', 'HTTP CONNECT 单流'): 'proxy-1x1',
    ('tcpquic-proxy', '多流'): 'proxy-16x16',
    ('tcpquic-proxy', '多流（最佳）'): 'proxy-4x16',
}
for line in text.splitlines():
    if not line.startswith('| tcpquic-proxy'):
        continue
    parts = [p.strip() for p in line.strip('|').split('|')]
    if len(parts) < 5:
        continue
    tool, scene, cfg, _mbps, gbps = parts[0], parts[1], parts[2], parts[3], parts[4]
    key = mapping.get((tool, scene))
    if key:
        rows[key] = gbps
with open(out, 'w') as f:
    for k in ('direct-tcp', 'proxy-1x1', 'proxy-16x16', 'proxy-4x16'):
        if k in rows:
            f.write(f"{k}={rows[k]}\n")
PY
}

main() {
    mkdir -p "${OUT_ROOT}"
    capture_sysctl "${OUT_ROOT}/sysctl.txt"

    declare -A BINS=(
        [phase3]="${ROOT}/.worktrees/phase3-event-queue-baseline/build/bin/Release/tcpquic-proxy"
        [phase4-always-pending]="${ROOT}/build/bin/Release/tcpquic-proxy"
        [phase4-sync-write]="${ROOT}/.worktrees/phase4-callback-sync-write/build/bin/Release/tcpquic-proxy"
    )

    for variant in phase3 phase4-always-pending phase4-sync-write; do
        local bin="${BINS[$variant]}"
        local vdir="${OUT_ROOT}/${variant}"
        [[ -x "$bin" ]] || { log "missing binary: $bin"; exit 1; }
        mkdir -p "$vdir"
        run_bench "$variant" "$bin" "$vdir"
        extract_throughput_txt "$vdir"
        run_perf "$variant" "$bin" "proxy-1x1" "${vdir}/perf-proxy-1x1"
        run_perf "$variant" "$bin" "proxy-4x16" "${vdir}/perf-proxy-4x16"
    done

    write_summary
    log "done -> ${OUT_ROOT}/summary.md"
    cat "${OUT_ROOT}/summary.md"
}

main "$@"
