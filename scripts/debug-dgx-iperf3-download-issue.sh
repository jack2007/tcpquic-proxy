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
IPERF_DURATION="${IPERF_DURATION:-30}"
IPERF_TIMEOUT_SEC="${IPERF_TIMEOUT_SEC:-240}"
LOW_THROUGHPUT_MBPS="${LOW_THROUGHPUT_MBPS:-500}"
ATTEMPTS="${ATTEMPTS:-6}"
SEQUENTIAL="${SEQUENTIAL:-0}"
DELAYS=(${DELAYS:-40 500 300})
RESULT_ROOT="${RESULT_ROOT:-${ROOT}/docs/dgx-iperf3-10loss-debug-$(date +%Y%m%d-%H%M%S)}"
CERT_DIR="${RESULT_ROOT}/certs"

mkdir -p "$CERT_DIR"

log() {
    printf '[dgx-debug] %s\n' "$*" | tee -a "$RESULT_ROOT/orchestrator.log" >&2
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
    if [[ "${PRESERVE_SITE:-0}" == 1 ]]; then
        log "preserving failure site: tcpquic-proxy/iperf3/netem left running"
    else
        log "cleanup on exit"
        cleanup_all
    fi
}
trap on_exit EXIT INT TERM

wait_remote_log() {
    local file="$1" pattern="$2" tries="${3:-80}"
    for _ in $(seq 1 "$tries"); do
        run_remote "grep -q '${pattern}' '${file}' 2>/dev/null" && return 0
        sleep 0.5
    done
    run_remote "tail -100 '${file}' 2>/dev/null" >&2 || true
    return 1
}

wait_local_log() {
    local file="$1" pattern="$2" tries="${3:-80}"
    for _ in $(seq 1 "$tries"); do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.5
    done
    tail -100 "$file" >&2 || true
    return 1
}

generate_certs() {
    log "generating certs"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_DIR/ca.key" -out "$CERT_DIR/ca.crt" \
        -subj "/CN=dgx-debug-ca" -days 1 -sha256 >/dev/null 2>&1
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
subjectAltName=DNS:dgx-debug-client
EOF
    for name in server client; do
        openssl req -newkey rsa:2048 -nodes \
            -keyout "$CERT_DIR/$name.key" -out "$CERT_DIR/$name.csr" \
            -subj "/CN=$name" >/dev/null 2>&1
        openssl x509 -req -in "$CERT_DIR/$name.csr" \
            -CA "$CERT_DIR/ca.crt" -CAkey "$CERT_DIR/ca.key" -CAcreateserial \
            -out "$CERT_DIR/$name.crt" -days 1 -sha256 \
            -extfile "$CERT_DIR/$name.ext" >/dev/null 2>&1
    done
    run_remote "mkdir -p ~/tcpquic-dgx-certs"
    rsync -az "$CERT_DIR/" "$REMOTE:tcpquic-dgx-certs/"
}

start_download_proxy() {
    local case_dir="$1"
    cleanup_all
    rm -rf "${ROOT}/build/bin/Release/log"
    run_remote "rm -rf ${REMOTE_DIR}/log /tmp/dgx-debug-download-server.log /tmp/dgx-debug-iperf-server.log"

    log "starting traced download proxy"
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
        --trace \
        --trace-interval 1 \
        </dev/null >/tmp/dgx-debug-download-server.log 2>&1 &"
    wait_remote_log /tmp/dgx-debug-download-server.log 'QUIC server listening' 120

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
        --trace \
        --trace-interval 1 \
        >"$case_dir/local-proxy.stderr.log" 2>&1 &
    echo "$!" >"$case_dir/local-proxy.pid"
    wait_local_log "$case_dir/local-proxy.stderr.log" 'QUIC client connected' 160
    wait_local_log "$case_dir/local-proxy.stderr.log" 'HTTP CONNECT listening' 80
}

sample_qdisc_start() {
    local out="$1"
    (
        while true; do
            date -Iseconds
            ssh -o BatchMode=yes "$REMOTE" "tc -s qdisc show dev ${IFACE}" || true
            sleep 1
        done
    ) >"$out" 2>&1 &
    echo $!
}

sample_diag_start() {
    local case_dir="$1"
    (
        while true; do
            {
                echo "===== $(date -Iseconds) local ss ====="
                ss -antup | grep -E ":(${PROXY_PORT}|${SOCKS_PORT}|${QUIC_PORT}|${IPERF_PORT})\\b" || true
                echo "===== $(date -Iseconds) remote ss ====="
                ssh -o BatchMode=yes "$REMOTE" "ss -antup | grep -E ':(${PROXY_PORT}|${SOCKS_PORT}|${QUIC_PORT}|${IPERF_PORT})\\b' || true" || true
            } >>"$case_dir/socket-samples.txt" 2>&1
            sleep 2
        done
    ) >/dev/null 2>&1 &
    echo $!
}

apply_netem() {
    local delay_ms="$1"
    run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; true"
    run_remote "sudo -n tc qdisc replace dev ${IFACE} root netem delay ${delay_ms}ms loss 10% limit 5000000"
}

copy_logs() {
    local case_dir="$1"
    cp "${ROOT}/build/bin/Release/log/client.log" "$case_dir/local-trace-client.log" 2>/dev/null || true
    scp -q "$REMOTE:/tmp/dgx-debug-download-server.log" "$case_dir/remote-proxy.stderr.log" 2>/dev/null || true
    scp -q "$REMOTE:${REMOTE_DIR}/log/server.log" "$case_dir/remote-trace-server.log" 2>/dev/null || true
    scp -q "$REMOTE:/tmp/dgx-debug-iperf-server.log" "$case_dir/remote-iperf-server.log" 2>/dev/null || true
}

capture_site() {
    local case_dir="$1"
    log "capturing failure site into $case_dir"
    {
        date -Iseconds
        echo "local git=$(git rev-parse HEAD)"
        echo "msquic=$(git submodule status third_party/msquic | tr -s ' ')"
        echo "iperf3=$(iperf3 --version 2>&1 | head -1)"
        echo "bin=$BIN"
    } >"$case_dir/site.env"
    ip route get "$REMOTE_IP" >"$case_dir/local-route.txt" 2>&1 || true
    run_remote "ip route get ${LOCAL_IP}" >"$case_dir/remote-route.txt" 2>&1 || true
    tc -s qdisc show dev "$IFACE" >"$case_dir/local-qdisc.txt" 2>&1 || true
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-live.txt" 2>&1 || true
    ps -eLf | awk '/tcpquic-proxy|iperf3/ && !/awk/' >"$case_dir/local-ps.txt" 2>&1 || true
    run_remote "ps -eLf | awk '/tcpquic-proxy|iperf3/ && !/awk/'" >"$case_dir/remote-ps.txt" 2>&1 || true
    ss -tinup >"$case_dir/local-ss-all.txt" 2>&1 || true
    run_remote "ss -tinup" >"$case_dir/remote-ss-all.txt" 2>&1 || true
    for pid in $(pgrep -x tcpquic-proxy 2>/dev/null || true); do
        cat "/proc/$pid/stack" >"$case_dir/local-proxy-${pid}.stack" 2>&1 || true
        ls -l "/proc/$pid/fd" >"$case_dir/local-proxy-${pid}.fd.txt" 2>&1 || true
    done
    run_remote "for p in \$(pgrep -x tcpquic-proxy 2>/dev/null); do cat /proc/\$p/stack > /tmp/dgx-debug-proxy-\$p.stack 2>&1; ls -l /proc/\$p/fd > /tmp/dgx-debug-proxy-\$p.fd.txt 2>&1; done; true"
    scp -q "$REMOTE:/tmp/dgx-debug-proxy-*.stack" "$case_dir/" 2>/dev/null || true
    scp -q "$REMOTE:/tmp/dgx-debug-proxy-*.fd.txt" "$case_dir/" 2>/dev/null || true
    copy_logs "$case_dir"
}

received_mbps_from_iperf_text() {
    awk '
        /receiver/ {
            value=$(NF-2); unit=$(NF-1); mbps=value;
            if (unit == "Kbits/sec") mbps=value/1000;
            else if (unit == "Gbits/sec") mbps=value*1000;
            else if (unit == "bits/sec") mbps=value/1000000;
            found=mbps;
        }
        END {
            if (found == "") print "";
            else printf "%.2f\n", found;
        }
    ' "$1"
}

run_case() {
    local delay_ms="$1" attempt="$2"
    local case_dir="$RESULT_ROOT/netem-$(printf '%03d' "$delay_ms")ms-10loss-attempt-${attempt}"
    mkdir -p "$case_dir"
    log "case delay=${delay_ms}ms attempt=${attempt}"
    start_download_proxy "$case_dir"
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-before.txt" 2>&1 || true

    # Start and confirm the remote iperf server before applying netem. This keeps
    # SSH control-plane polling out of the high-delay/high-loss condition while
    # the iperf CONNECT and data path still run under netem.
    log "starting remote iperf3 server"
    run_remote "pkill -9 -x iperf3 2>/dev/null; nohup iperf3 -s -V --timestamps -B ${REMOTE_IP} -p ${IPERF_PORT} >/tmp/dgx-debug-iperf-server.log 2>&1 &"
    for _ in $(seq 1 40); do
        if run_remote "pgrep -x iperf3 >/dev/null && ss -lntp | grep -q ':${IPERF_PORT}\\b'"; then
            break
        fi
        sleep 0.25
    done
    run_remote "pgrep -a iperf3; ss -lntp | grep ':${IPERF_PORT}\\b'" >"$case_dir/remote-iperf-server-ready.txt" 2>&1

    apply_netem "$delay_ms"

    local qdisc_pid diag_pid
    qdisc_pid=$(sample_qdisc_start "$case_dir/remote-qdisc-samples.txt")
    diag_pid=$(sample_diag_start "$case_dir")
    sleep 1

    log "starting local iperf3 client"
    set +e
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
        -B "$LOCAL_IP" \
        -p "$IPERF_PORT" \
        -t "$IPERF_DURATION" \
        -i 1 \
        -V \
        -d \
        --timestamps \
        --connect-timeout 5000 \
        --reverse \
        --proxy "http://127.0.0.1:${PROXY_PORT}" \
        >"$case_dir/iperf.verbose.stdout.txt" \
        2>"$case_dir/iperf.verbose.stderr.txt"
    local rc=$?
    set -e
    echo "$rc" >"$case_dir/iperf.rc"

    kill "$qdisc_pid" "$diag_pid" 2>/dev/null || true
    wait "$qdisc_pid" "$diag_pid" 2>/dev/null || true
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-after.txt" 2>&1 || true
    copy_logs "$case_dir"

    local recv_mbps
    recv_mbps="$(received_mbps_from_iperf_text "$case_dir/iperf.verbose.stdout.txt")"
    echo "${recv_mbps:-unknown}" >"$case_dir/received-mbps.txt"
    log "case delay=${delay_ms}ms attempt=${attempt} rc=${rc} received_mbps=${recv_mbps:-unknown}"

    if [[ "$rc" != 0 ]]; then
        echo "trigger=iperf_rc_${rc}" >"$case_dir/trigger.txt"
        capture_site "$case_dir"
        PRESERVE_SITE=1
        return 100
    fi
    if [[ -n "$recv_mbps" ]]; then
        if awk -v v="$recv_mbps" -v t="$LOW_THROUGHPUT_MBPS" 'BEGIN { exit !(v < t) }'; then
            echo "trigger=low_throughput_${recv_mbps}_mbps" >"$case_dir/trigger.txt"
            capture_site "$case_dir"
            PRESERVE_SITE=1
            return 100
        fi
    fi

    cleanup_all
    return 0
}

run_case_existing_proxy() {
    local delay_ms="$1" attempt="$2"
    local case_dir="$RESULT_ROOT/sequential-netem-$(printf '%03d' "$delay_ms")ms-10loss-attempt-${attempt}"
    mkdir -p "$case_dir"
    log "sequential case delay=${delay_ms}ms attempt=${attempt}"
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-before.txt" 2>&1 || true

    log "starting remote iperf3 server"
    run_remote "pkill -9 -x iperf3 2>/dev/null; nohup iperf3 -s -V --timestamps -B ${REMOTE_IP} -p ${IPERF_PORT} >/tmp/dgx-debug-iperf-server.log 2>&1 &"
    for _ in $(seq 1 40); do
        if run_remote "pgrep -x iperf3 >/dev/null && ss -lntp | grep -q ':${IPERF_PORT}\\b'"; then
            break
        fi
        sleep 0.25
    done
    run_remote "pgrep -a iperf3; ss -lntp | grep ':${IPERF_PORT}\\b'" >"$case_dir/remote-iperf-server-ready.txt" 2>&1

    apply_netem "$delay_ms"
    local qdisc_pid diag_pid
    qdisc_pid=$(sample_qdisc_start "$case_dir/remote-qdisc-samples.txt")
    diag_pid=$(sample_diag_start "$case_dir")
    sleep 1

    log "starting local iperf3 client"
    set +e
    timeout "${IPERF_TIMEOUT_SEC}s" iperf3 -c "$REMOTE_IP" \
        -B "$LOCAL_IP" \
        -p "$IPERF_PORT" \
        -t "$IPERF_DURATION" \
        -i 1 \
        -V \
        -d \
        --timestamps \
        --connect-timeout 5000 \
        --reverse \
        --proxy "http://127.0.0.1:${PROXY_PORT}" \
        >"$case_dir/iperf.verbose.stdout.txt" \
        2>"$case_dir/iperf.verbose.stderr.txt"
    local rc=$?
    set -e
    echo "$rc" >"$case_dir/iperf.rc"

    kill "$qdisc_pid" "$diag_pid" 2>/dev/null || true
    wait "$qdisc_pid" "$diag_pid" 2>/dev/null || true
    run_remote "tc -s qdisc show dev ${IFACE}" >"$case_dir/remote-qdisc-after.txt" 2>&1 || true
    copy_logs "$case_dir"

    local recv_mbps
    recv_mbps="$(received_mbps_from_iperf_text "$case_dir/iperf.verbose.stdout.txt")"
    echo "${recv_mbps:-unknown}" >"$case_dir/received-mbps.txt"
    log "sequential case delay=${delay_ms}ms attempt=${attempt} rc=${rc} received_mbps=${recv_mbps:-unknown}"

    if [[ "$rc" != 0 ]]; then
        echo "trigger=iperf_rc_${rc}" >"$case_dir/trigger.txt"
        capture_site "$case_dir"
        PRESERVE_SITE=1
        return 100
    fi
    if [[ -n "$recv_mbps" ]]; then
        if awk -v v="$recv_mbps" -v t="$LOW_THROUGHPUT_MBPS" 'BEGIN { exit !(v < t) }'; then
            echo "trigger=low_throughput_${recv_mbps}_mbps" >"$case_dir/trigger.txt"
            capture_site "$case_dir"
            PRESERVE_SITE=1
            return 100
        fi
    fi

    run_remote "sudo -n tc qdisc del dev ${IFACE} root 2>/dev/null; pkill -9 -x iperf3 2>/dev/null; true"
    return 0
}

main() {
    log "result root: $RESULT_ROOT"
    log "source: $(git rev-parse HEAD)"
    log "msquic: $(git submodule status third_party/msquic | tr -s ' ')"
    iperf3 --version >"$RESULT_ROOT/local-iperf3-version.txt" 2>&1 || true
    run_remote "iperf3 --version" >"$RESULT_ROOT/remote-iperf3-version.txt" 2>&1 || true
    cleanup_all
    generate_certs

    if [[ "$SEQUENTIAL" == 1 ]]; then
        local seq_case_dir="$RESULT_ROOT/sequential-proxy"
        mkdir -p "$seq_case_dir"
        start_download_proxy "$seq_case_dir"
        local attempt=1
        for delay_ms in "${DELAYS[@]}"; do
            if ! run_case_existing_proxy "$delay_ms" "$attempt"; then
                log "problem reproduced in sequential mode at delay=${delay_ms}ms"
                echo "$RESULT_ROOT" >"$RESULT_ROOT/reproduced-result-root.txt"
                return 0
            fi
            attempt=$((attempt + 1))
        done
        log "no problem reproduced in sequential mode"
        return 0
    fi

    for delay_ms in "${DELAYS[@]}"; do
        for attempt in $(seq 1 "$ATTEMPTS"); do
            if ! run_case "$delay_ms" "$attempt"; then
                log "problem reproduced at delay=${delay_ms}ms attempt=${attempt}"
                echo "$RESULT_ROOT" >"$RESULT_ROOT/reproduced-result-root.txt"
                return 0
            fi
        done
    done
    log "no problem reproduced after configured attempts"
}

main "$@"
