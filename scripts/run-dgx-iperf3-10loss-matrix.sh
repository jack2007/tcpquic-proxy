#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
REMOTE="${REMOTE:-jack@169.254.59.196}"
REMOTE_DIR="${REMOTE_DIR:-/home/jack/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-${REMOTE_DIR}/tcpquic-proxy}"
LOCAL_IP="${LOCAL_IP:-169.254.250.230}"
REMOTE_IP="${REMOTE_IP:-169.254.59.196}"
IFACE="${IFACE:-enp1s0f0np0}"
QUIC_PORT="${QUIC_PORT:-4433}"
PROXY_PORT="${PROXY_PORT:-18080}"
SOCKS_PORT="${SOCKS_PORT:-19080}"
IPERF_PORT="${IPERF_PORT:-16001}"
IPERF_TIMEOUT_SEC="${IPERF_TIMEOUT_SEC:-90}"
RESULT_ROOT="${RESULT_ROOT:-${ROOT}/docs/dgx-iperf3-10loss-io-matrix-$(date +%Y%m%d-%H%M%S)}"
CERT_DIR="${RESULT_ROOT}/certs"
DELAYS=(${DELAYS:-10 20 30 40 50 60 70 80 90 150 300 500})

mkdir -p "$CERT_DIR"

log() {
    printf '[dgx-iperf3-10loss] %s\n' "$*" | tee -a "$RESULT_ROOT/orchestrator.log" >&2
}

run_remote() {
    ssh -o BatchMode=yes "$REMOTE" "$@"
}

cleanup_local() {
    pkill -9 -x tcpquic-proxy 2>/dev/null || true
    pkill -9 -x iperf3 2>/dev/null || true
    sudo -n tc qdisc del dev "$IFACE" root 2>/dev/null || true
}

cleanup_remote() {
    run_remote "killall -9 tcpquic-proxy 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true" || true
}

cleanup_all() {
    cleanup_local
    cleanup_remote
}

on_exit() {
    log "cleanup on exit"
    cleanup_all
}
trap on_exit EXIT INT TERM

wait_remote_log() {
    local file="$1" pattern="$2" tries="${3:-80}"
    for _ in $(seq 1 "$tries"); do
        run_remote "grep -q '${pattern}' '${file}' 2>/dev/null" && return 0
        sleep 0.5
    done
    run_remote "tail -80 '${file}' 2>/dev/null" >&2 || true
    return 1
}

wait_local_log() {
    local file="$1" pattern="$2" tries="${3:-80}"
    for _ in $(seq 1 "$tries"); do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.5
    done
    tail -80 "$file" >&2 || true
    return 1
}

generate_certs() {
    log "generating certs"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=dgx-iperf3-10loss-ca" -days 1 -sha256 >/dev/null 2>&1
    cat >"$CERT_DIR/server.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:${REMOTE_IP},IP:${LOCAL_IP}
EOF
    cat >"$CERT_DIR/client.ext" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:dgx-iperf3-client
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
    run_remote "mkdir -p ~/tcpquic-dgx-certs"
    rsync -az "$CERT_DIR/" "$REMOTE:tcpquic-dgx-certs/"
}

start_download_proxy() {
    log "starting download proxy direction"
    cleanup_all
    run_remote "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
        --listen ${REMOTE_IP}:${QUIC_PORT} \
        --allow-targets ${REMOTE_IP}/32,127.0.0.0/8 \
        --cert ~/tcpquic-dgx-certs/server.crt \
        --key ~/tcpquic-dgx-certs/server.key \
        --ca ~/tcpquic-dgx-certs/ca.crt \
        --compress off \
        --tuning wan \
        --connections 1 \
        --diag-stats \
        --diag-stats-interval 1 \
        </dev/null >/tmp/dgx-download-server.log 2>&1 &"
    wait_remote_log /tmp/dgx-download-server.log 'QUIC server listening' 100
    "$BIN" client \
        --peer "${REMOTE_IP}:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${PROXY_PORT}" \
        --socks-listen "127.0.0.1:${SOCKS_PORT}" \
        --cert "$CERT_DIR/client.crt" \
        --key "$CERT_DIR/client.key" \
        --ca "$CERT_DIR/ca.crt" \
        --connections 1 \
        --compress off \
        --tuning wan \
        --diag-stats \
        --diag-stats-interval 1 \
        >/tmp/dgx-download-client.log 2>&1 &
    wait_local_log /tmp/dgx-download-client.log 'QUIC client connected' 140
    wait_local_log /tmp/dgx-download-client.log 'HTTP CONNECT listening' 80
}

start_upload_proxy() {
    log "starting upload proxy direction"
    cleanup_all
    "$BIN" server \
        --listen "${LOCAL_IP}:${QUIC_PORT}" \
        --allow-targets "${LOCAL_IP}/32,127.0.0.0/8" \
        --cert "$CERT_DIR/server.crt" \
        --key "$CERT_DIR/server.key" \
        --ca "$CERT_DIR/ca.crt" \
        --compress off \
        --tuning wan \
        --connections 1 \
        --diag-stats \
        --diag-stats-interval 1 \
        >/tmp/dgx-upload-server.log 2>&1 &
    wait_local_log /tmp/dgx-upload-server.log 'QUIC server listening' 100
    run_remote "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} client \
        --peer ${LOCAL_IP}:${QUIC_PORT} \
        --http-listen 127.0.0.1:${PROXY_PORT} \
        --socks-listen 127.0.0.1:${SOCKS_PORT} \
        --cert ~/tcpquic-dgx-certs/client.crt \
        --key ~/tcpquic-dgx-certs/client.key \
        --ca ~/tcpquic-dgx-certs/ca.crt \
        --connections 1 \
        --compress off \
        --tuning wan \
        --diag-stats \
        --diag-stats-interval 1 \
        </dev/null >/tmp/dgx-upload-client.log 2>&1 &"
    wait_remote_log /tmp/dgx-upload-client.log 'QUIC client connected' 140
    wait_remote_log /tmp/dgx-upload-client.log 'HTTP CONNECT listening' 80
}

sample_qdisc_start() {
    local out="$1"
    (
        while true; do
            date -Iseconds
            ssh -o BatchMode=yes "$REMOTE" "tc -s qdisc show dev ${IFACE}" || true
            sleep 5
        done
    ) >"$out" 2>&1 &
    echo $!
}

apply_netem() {
    local d="$1"
    run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true"
    run_remote "sudo -n tc qdisc replace dev ${IFACE} root netem delay ${d}ms loss 10% limit 5000000"
}

capture_common_after() {
    local case_dir="$1" direction="$2"
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-after.txt" || true
    if [[ "$direction" == download ]]; then
        cp /tmp/dgx-download-client.log "$case_dir/local-proxy.log" 2>/dev/null || true
        scp -q "$REMOTE:/tmp/dgx-download-server.log" "$case_dir/remote-proxy.log" 2>/dev/null || true
    else
        cp /tmp/dgx-upload-server.log "$case_dir/local-proxy.log" 2>/dev/null || true
        scp -q "$REMOTE:/tmp/dgx-upload-client.log" "$case_dir/remote-proxy.log" 2>/dev/null || true
    fi
}

write_env() {
    local case_dir="$1" direction="$2" d="$3" netem="$4"
    {
        echo "direction=${direction}"
        echo "delay_ms=${d}"
        if [[ "$netem" == 1 ]]; then
            echo "loss=10%"
            echo "netem_limit=5000000"
        else
            echo "loss=0%"
            echo "netem_limit=none"
        fi
        echo "proxy_connections=1"
        echo "iperf_parallel=1"
        echo "started_at=$(date -Iseconds)"
    } >"$case_dir/run.env"
}

run_download_case() {
    local d="$1" case_dir="$2" label="$3" netem="${4:-1}"
    mkdir -p "$case_dir"
    write_env "$case_dir" download "$d" "$netem"
    log "download ${label}: prepare"
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-before.txt" || true
    if [[ "$netem" == 1 ]]; then
        apply_netem "$d"
    else
        run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true"
    fi
    local sampler_pid
    sampler_pid=$(sample_qdisc_start "$case_dir/remote-qdisc-samples.txt")
    run_remote "pkill -9 -x iperf3 2>/dev/null; iperf3 -s -B ${REMOTE_IP} -p ${IPERF_PORT} -D"
    log "download ${label}: iperf3 start"
    set +e
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
        -B "$LOCAL_IP" \
        -p "$IPERF_PORT" \
        -t 30 \
        --json \
        --connect-timeout 5000 \
        --reverse \
        --proxy "http://127.0.0.1:${PROXY_PORT}" \
        >"$case_dir/iperf.stdout.json" \
        2>"$case_dir/iperf.stderr.txt"
    local rc=$?
    set -e
    echo "$rc" >"$case_dir/iperf.rc"
    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    capture_common_after "$case_dir" download
    run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true"
    log "download ${label}: iperf rc=${rc}"
}

run_upload_case() {
    local d="$1" case_dir="$2" label="$3" netem="${4:-1}"
    mkdir -p "$case_dir"
    write_env "$case_dir" upload "$d" "$netem"
    log "upload ${label}: prepare"
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-before.txt" || true
    if [[ "$netem" == 1 ]]; then
        apply_netem "$d"
    else
        run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true"
    fi
    local sampler_pid
    sampler_pid=$(sample_qdisc_start "$case_dir/remote-qdisc-samples.txt")
    pkill -9 -x iperf3 2>/dev/null || true
    iperf3 -s -B "$LOCAL_IP" -p "$IPERF_PORT" -D
    log "upload ${label}: iperf3 start"
    set +e
    timeout "${IPERF_TIMEOUT_SEC}s" ssh -o BatchMode=yes "$REMOTE" "timeout ${IPERF_TIMEOUT_SEC}s iperf3 -c ${LOCAL_IP} \
        -B ${REMOTE_IP} \
        -p ${IPERF_PORT} \
        -t 30 \
        --json \
        --connect-timeout 5000 \
        --proxy http://127.0.0.1:${PROXY_PORT}" \
        >"$case_dir/iperf.stdout.json" \
        2>"$case_dir/iperf.stderr.txt"
    local rc=$?
    set -e
    echo "$rc" >"$case_dir/iperf.rc"
    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    capture_common_after "$case_dir" upload
    run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true"
    log "upload ${label}: iperf rc=${rc}"
}

parse_summary() {
    python3 - "$RESULT_ROOT" <<'PY'
import csv
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])

def read_text(path):
    try:
        return path.read_text(errors="ignore")
    except FileNotFoundError:
        return ""

def parse_iperf(case):
    rc = read_text(case / "iperf.rc").strip() or "missing"
    sent = recv = retrans = jitter = ""
    note = ""
    path = case / "iperf.stdout.json"
    if path.exists() and path.stat().st_size:
        try:
            data = json.loads(path.read_text())
            end = data.get("end", {})
            ss = end.get("sum_sent", {}) or {}
            rr = end.get("sum_received", {}) or {}
            sent = f"{float(ss.get('bits_per_second', 0)) / 1_000_000:.2f}"
            recv = f"{float(rr.get('bits_per_second', 0)) / 1_000_000:.2f}"
            retrans = ss.get("retransmits", "")
            jitter = rr.get("jitter_ms", "") or ss.get("jitter_ms", "") or ""
        except Exception as exc:
            note = f"json-parse-error:{exc}"
    else:
        note = "missing-json"
    return rc, sent, recv, retrans, jitter, note

def dropped(text):
    match = re.search(r"dropped\s+(\d+)", text)
    return int(match.group(1)) if match else 0

def backlog(text):
    max_bytes = 0
    max_packets = 0
    for match in re.finditer(r"backlog\s+([0-9.]+)([KMG]?b)\s+(\d+)p", text, re.I):
        value = float(match.group(1))
        unit = match.group(2).lower()
        mult = {"b": 1, "kb": 1000, "mb": 1000000, "gb": 1000000000}.get(unit, 1)
        max_bytes = max(max_bytes, int(value * mult))
        max_packets = max(max_packets, int(match.group(3)))
    return max_bytes, max_packets

def nums(log, name):
    out = []
    for match in re.finditer(rf"{re.escape(name)}=([0-9.]+)", log):
        try:
            out.append(float(match.group(1)))
        except ValueError:
            pass
    return out

def diag(case):
    log = read_text(case / "remote-proxy.log")
    srtt = nums(log, "srtt")
    bbr = nums(log, "bbr_bw_mbps")
    bif = nums(log, "bytes_in_flight_max")
    lost = nums(log, "lost_retransmittable_bytes")
    eq = nums(log, "event_queue_full_errors")
    return {
        "srtt_avg_ms": f"{sum(srtt) / len(srtt):.3f}" if srtt else "",
        "srtt_max_ms": f"{max(srtt):.3f}" if srtt else "",
        "bbr_bw_avg_mbps": f"{sum(bbr) / len(bbr):.2f}" if bbr else "",
        "bytes_in_flight_max": str(int(max(bif))) if bif else "",
        "lost_retransmittable_bytes_max": str(int(max(lost))) if lost else "",
        "event_queue_full_errors_max": str(int(max(eq))) if eq else "",
    }

rows = []
for parent in sorted(root.glob("netem-*ms-10loss")):
    match = re.search(r"netem-(\d+)ms-10loss", parent.name)
    if not match:
        continue
    delay = int(match.group(1))
    for direction in ("download", "upload"):
        case = parent / direction
        rc, sent, recv, retrans, jitter, note = parse_iperf(case)
        before = dropped(read_text(case / "remote-qdisc-before.txt"))
        after = dropped(read_text(case / "remote-qdisc-after.txt"))
        max_backlog_bytes, max_backlog_packets = backlog(read_text(case / "remote-qdisc-samples.txt"))
        notes = []
        if note:
            notes.append(note)
        if rc not in ("0", ""):
            notes.append(f"iperf_rc_{rc}")
        rows.append({
            "delay_ms": delay,
            "direction": direction,
            "loss": "10%",
            "iperf_rc": rc,
            "sent_mbps": sent,
            "received_mbps": recv,
            "retransmits": retrans,
            "jitter_ms": jitter,
            "netem_limit": "5000000",
            "qdisc_drop_delta": str(after - before),
            "qdisc_backlog_max_bytes": str(max_backlog_bytes),
            "qdisc_backlog_max_packets": str(max_backlog_packets),
            **diag(case),
            "notes": ";".join(notes),
        })

fields = [
    "delay_ms", "direction", "loss", "iperf_rc", "sent_mbps", "received_mbps",
    "retransmits", "jitter_ms", "netem_limit", "qdisc_drop_delta",
    "qdisc_backlog_max_bytes", "qdisc_backlog_max_packets", "srtt_avg_ms",
    "srtt_max_ms", "bbr_bw_avg_mbps", "bytes_in_flight_max",
    "lost_retransmittable_bytes_max", "event_queue_full_errors_max", "notes",
]
with (root / "summary.csv").open("w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)

with (root / "summary.md").open("w") as f:
    f.write("# DGX iperf3 10% Loss IO Matrix\n\n")
    f.write(f"- Result root: `{root}`\n")
    f.write("- Tool: system `iperf3` with HTTP CONNECT proxy\n")
    f.write("- QUIC connections: 1; iperf parallel streams: 1\n")
    f.write("- netem: remote egress, `loss 10% limit 5000000`\n\n")
    f.write("| Delay | Direction | Loss | Received Mbps | Sent Mbps | iperf rc | Retransmits | qdisc drops | qdisc backlog max bytes | srtt avg/max | bytes_in_flight_max | lost bytes max | Notes |\n")
    f.write("|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---|\n")
    for row in rows:
        srtt = f"{row['srtt_avg_ms'] or '?'}/{row['srtt_max_ms'] or '?'}"
        f.write(
            f"| {row['delay_ms']} | {row['direction']} | {row['loss']} | "
            f"{row['received_mbps']} | {row['sent_mbps']} | {row['iperf_rc']} | "
            f"{row['retransmits']} | {row['qdisc_drop_delta']} | "
            f"{row['qdisc_backlog_max_bytes']} | {srtt} | "
            f"{row['bytes_in_flight_max']} | {row['lost_retransmittable_bytes_max']} | "
            f"{row['notes']} |\n"
        )
print(root)
PY
}

main() {
    log "result root: $RESULT_ROOT"
    log "source: $(git rev-parse HEAD)"
    log "msquic: $(git submodule status third_party/msquic | tr -s ' ')"
    log "verifying remote binary"
    run_remote "mkdir -p ${REMOTE_DIR} && chmod +x ${REMOTE_BIN} && ls -l ${REMOTE_BIN}" >"$RESULT_ROOT/remote-binary.txt"

    log "preflight cleanup"
    cleanup_all
    ip route get "$REMOTE_IP" >"$RESULT_ROOT/local-route.txt"
    run_remote "ip route get ${LOCAL_IP}" >"$RESULT_ROOT/remote-route.txt"
    iperf3 --version >"$RESULT_ROOT/iperf3-version.txt" 2>&1 || true
    iperf3 --help >"$RESULT_ROOT/iperf3-help.txt" 2>&1 || true

    generate_certs

    log "baseline download"
    start_download_proxy
    run_download_case 0 "$RESULT_ROOT/baseline/download" baseline-no-netem 0

    log "baseline upload"
    start_upload_proxy
    run_upload_case 0 "$RESULT_ROOT/baseline/upload" baseline-no-netem 0

    log "download matrix"
    start_download_proxy
    for d in "${DELAYS[@]}"; do
        run_download_case "$d" "$RESULT_ROOT/netem-$(printf '%03d' "$d")ms-10loss/download" "${d}ms-10loss" 1
    done

    log "upload matrix"
    start_upload_proxy
    for d in "${DELAYS[@]}"; do
        run_upload_case "$d" "$RESULT_ROOT/netem-$(printf '%03d' "$d")ms-10loss/upload" "${d}ms-10loss" 1
    done

    log "parsing summaries"
    parse_summary
    log "matrix complete: $RESULT_ROOT"
    cat "$RESULT_ROOT/summary.md"
}

main "$@"
