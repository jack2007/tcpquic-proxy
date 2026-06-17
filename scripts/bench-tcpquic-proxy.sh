#!/usr/bin/env bash
# tcpquic-proxy 性能基线：对比裸 TCP、隧道无压缩、隧道 zstd
# 计划 Task 14 / 规格 §11.1 性能测试
#
# 用法:
#   ./scripts/bench-tcpquic-proxy.sh
#   SIZE_MB=64 ITERATIONS=3 NETEM=1 NETEM_DELAY=100ms NETEM_LOSS=5% ./scripts/bench-tcpquic-proxy.sh
#
# 环境变量:
#   SIZE_MB        单次传输载荷大小（默认 32）
#   ITERATIONS     每种模式重复次数（默认 2）
#   NETEM          设为 1 时在 lo 上应用 tc netem（需 sudo）
#   NETEM_DELAY    默认 100ms
#   NETEM_LOSS     默认 5%
#   SKIP_APPEND    设为 1 时不写入 research_progress.md
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/bin/Release/tcpquic-proxy"
TMP_DIR="/tmp/tcpquic-bench-$$"
LOG_FILE="${ROOT_DIR}/research_progress.md"

SIZE_MB="${SIZE_MB:-32}"
ITERATIONS="${ITERATIONS:-2}"
NETEM="${NETEM:-0}"
NETEM_DELAY="${NETEM_DELAY:-100ms}"
NETEM_LOSS="${NETEM_LOSS:-5%}"
SKIP_APPEND="${SKIP_APPEND:-0}"

TARGET_HTTP_PORT=16001
QUIC_PORT=4453
HTTP_PROXY_PORT=18080
SOCKS_PROXY_PORT=11080

find_free_udp_port() {
    python3 - <<'PY'
import socket

for port in range(4453, 4553):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", port))
    except OSError:
        continue
    finally:
        s.close()
    print(port)
    break
else:
    raise SystemExit("no free UDP port in range 4453-4552")
PY
}

cleanup_stale_proxies() {
    pkill -9 -f "${BIN}" 2>/dev/null || true
    sleep 0.5
}

SERVER_PID=""
CLIENT_PID=""
HTTP_PID=""
SERVER_STDIN_FD=""
NETEM_APPLIED=0

log() {
    printf '[tcpquic-bench] %s\n' "$*" >&2
}

cleanup() {
    local status=$?
    set +e
    for pid in "$CLIENT_PID" "$SERVER_PID" "$HTTP_PID"; do
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
        fi
    done
    for pid in "$CLIENT_PID" "$SERVER_PID" "$HTTP_PID"; do
        if [ -n "${pid:-}" ]; then
            wait "$pid" 2>/dev/null
        fi
    done
    if [ -n "${SERVER_STDIN_FD:-}" ]; then
        eval "exec ${SERVER_STDIN_FD}>&-"
    fi
    if [ "$NETEM_APPLIED" = "1" ]; then
        sudo tc qdisc del dev lo root 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        log "missing required command: $1"
        exit 1
    }
}

wait_tcp() {
    local host=$1
    local port=$2
    local name=$3
    local deadline=$((SECONDS + 20))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if python3 - "$host" "$port" <<'PY' >/dev/null 2>&1
import socket
import sys

with socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=0.25):
    pass
PY
        then
            return 0
        fi
        sleep 0.2
    done
    log "timed out waiting for $name on $host:$port"
    exit 1
}

wait_log() {
    local file=$1
    local pattern=$2
    local name=$3
    local deadline=$((SECONDS + 20))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if grep -q "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    log "timed out waiting for $name"
    exit 1
}

build_if_missing() {
    if [ -x "$BIN" ]; then
        return 0
    fi
    log "building tcpquic-proxy"
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT_DIR/build" --config Release --target tcpquic-proxy -j"$(nproc)"
    [ -x "$BIN" ] || {
        log "binary missing after build: $BIN"
        exit 1
    }
}

generate_cert() {
    local name=$1
    local cn=$2
    local ext=$3
    openssl req -newkey rsa:2048 -nodes \
        -keyout "$TMP_DIR/$name.key" \
        -out "$TMP_DIR/$name.csr" \
        -subj "/CN=$cn" \
        >"$TMP_DIR/openssl-$name.log" 2>&1
    openssl x509 -req \
        -in "$TMP_DIR/$name.csr" \
        -CA "$TMP_DIR/ca.crt" \
        -CAkey "$TMP_DIR/ca.key" \
        -CAcreateserial \
        -out "$TMP_DIR/$name.crt" \
        -days 1 \
        -sha256 \
        -extfile "$TMP_DIR/$ext" \
        >>"$TMP_DIR/openssl-$name.log" 2>&1
}

setup_netem() {
    if [ "$NETEM" != "1" ]; then
        return 0
    fi
    local tc_args="delay ${NETEM_DELAY}"
    if [ -n "$NETEM_LOSS" ] && [ "$NETEM_LOSS" != "0" ] && [ "$NETEM_LOSS" != "0%" ]; then
        tc_args="${tc_args} loss ${NETEM_LOSS}"
    fi
    log "applying netem on lo: ${tc_args}"
    sudo tc qdisc replace dev lo root netem ${tc_args}
    NETEM_APPLIED=1
}

stop_proxy() {
    if [ -n "${CLIENT_PID:-}" ] && kill -0 "$CLIENT_PID" 2>/dev/null; then
        kill -9 "$CLIENT_PID" 2>/dev/null || true
        wait "$CLIENT_PID" 2>/dev/null || true
    fi
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -9 "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    CLIENT_PID=""
    SERVER_PID=""
    if [ -n "${SERVER_STDIN_FD:-}" ]; then
        eval "exec ${SERVER_STDIN_FD}>&-"
        SERVER_STDIN_FD=""
    fi
    sleep 0.5
}

start_proxy() {
    local compress_mode=$1
    local stdin_fifo="$TMP_DIR/server-${compress_mode}.stdin"
    rm -f "$stdin_fifo"
    mkfifo "$stdin_fifo"
    eval "exec {fd}<>\"$stdin_fifo\""
    SERVER_STDIN_FD=$fd

    "$BIN" server \
        --listen "127.0.0.1:${QUIC_PORT}" \
        --allow-targets "127.0.0.0/8" \
        --cert "$TMP_DIR/server.crt" \
        --key "$TMP_DIR/server.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress "$compress_mode" \
        >"$TMP_DIR/server-${compress_mode}.log" 2>&1 <"$stdin_fifo" &
    SERVER_PID=$!
    wait_log "$TMP_DIR/server-${compress_mode}.log" "QUIC server listening" "server"

    "$BIN" client \
        --peer "127.0.0.1:${QUIC_PORT}" \
        --http-listen "127.0.0.1:${HTTP_PROXY_PORT}" \
        --socks-listen "127.0.0.1:${SOCKS_PROXY_PORT}" \
        --cert "$TMP_DIR/client.crt" \
        --key "$TMP_DIR/client.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress "$compress_mode" \
        >"$TMP_DIR/client-${compress_mode}.log" 2>&1 &
    CLIENT_PID=$!
    wait_log "$TMP_DIR/client-${compress_mode}.log" "HTTP CONNECT listening" "client"
}

# 输出 Mbps（浮点，基于 curl speed_download 字节/秒）
measure_direct() {
    curl -fsS \
        "http://127.0.0.1:${TARGET_HTTP_PORT}/payload.bin" \
        -o /dev/null \
        --max-time 120 \
        -w '%{speed_download}' \
        2>"$TMP_DIR/curl-direct.log"
}

measure_tunnel() {
    curl -fsS \
        -x "http://127.0.0.1:${HTTP_PROXY_PORT}" \
        --proxytunnel \
        "http://127.0.0.1:${TARGET_HTTP_PORT}/payload.bin" \
        -o /dev/null \
        --max-time 120 \
        -w '%{speed_download}' \
        2>"$TMP_DIR/curl-tunnel.log"
}

bytes_to_mbps() {
    python3 - "$1" <<'PY'
import sys

bps = float(sys.argv[1])
print(f"{bps * 8 / 1_000_000:.2f}")
PY
}

avg_bytes_per_sec() {
    python3 - "$@" <<'PY'
import sys

vals = [float(v) for v in sys.argv[1:] if v]
if not vals:
    print("0")
else:
    print(f"{sum(vals) / len(vals):.0f}")
PY
}

run_mode() {
    local mode=$1
    local i speed avg_bps mbps
    local -a samples=()

    log "benchmark mode=${mode} iterations=${ITERATIONS} payload=${SIZE_MB}MB"
    for i in $(seq 1 "$ITERATIONS"); do
        case "$mode" in
            direct)
                speed=$(measure_direct)
                ;;
            tunnel_off)
                stop_proxy
                start_proxy off
                speed=$(measure_tunnel)
                stop_proxy
                ;;
            tunnel_zstd)
                stop_proxy
                start_proxy zstd
                speed=$(measure_tunnel)
                stop_proxy
                ;;
            *)
                log "unknown mode: $mode"
                exit 1
                ;;
        esac
        samples+=("$speed")
        mbps=$(bytes_to_mbps "$speed")
        log "  run ${i}/${ITERATIONS}: ${mbps} Mbps (${speed} B/s)"
    done

    avg_bps=$(avg_bytes_per_sec "${samples[@]}")
    mbps=$(bytes_to_mbps "$avg_bps")
    printf '%s,%s,%s\n' "$mode" "$avg_bps" "$mbps"
}

append_research_log() {
    local direct_bps=$1
    local direct_mbps=$2
    local off_bps=$3
    local off_mbps=$4
    local zstd_bps=$5
    local zstd_mbps=$6
    local overhead_pct zstd_pct netem_label

    overhead_pct=$(python3 - "$direct_bps" "$off_bps" <<'PY'
import sys

base, tun = float(sys.argv[1]), float(sys.argv[2])
if base <= 0:
    print("n/a")
else:
    print(f"{(1 - tun / base) * 100:.1f}")
PY
)

    zstd_pct=$(python3 - "$off_bps" "$zstd_bps" <<'PY'
import sys

plain, comp = float(sys.argv[1]), float(sys.argv[2])
if plain <= 0:
    print("n/a")
elif comp >= plain:
    print(f"带宽提升 {(comp / plain - 1) * 100:.1f}%（可压缩载荷时 zstd 减少传输字节）")
else:
    print(f"带宽下降 {(1 - comp / plain) * 100:.1f}%")
PY
)

    if [ "$NETEM" = "1" ]; then
        netem_label="${NETEM_DELAY}"
        if [ -n "$NETEM_LOSS" ] && [ "$NETEM_LOSS" != "0" ] && [ "$NETEM_LOSS" != "0%" ]; then
            netem_label="${netem_label} + ${NETEM_LOSS} loss"
        fi
    else
        netem_label="loopback (no netem)"
    fi

    {
        echo ""
        echo "## tcpquic-proxy 性能基线 ($(date -Iseconds))"
        echo ""
        echo "| 项目 | 值 |"
        echo "|------|-----|"
        echo "| 载荷 | ${SIZE_MB} MB × ${ITERATIONS} 次 |"
        echo "| 网络 | ${netem_label} |"
        echo "| 裸 TCP (HTTP) | ${direct_mbps} Mbps |"
        echo "| 隧道 (--compress off) | ${off_mbps} Mbps |"
        echo "| 隧道 (--compress zstd) | ${zstd_mbps} Mbps |"
        echo "| 隧道相对裸 TCP 开销 | ${overhead_pct}% |"
        echo "| zstd 相对无压缩 | ${zstd_pct}% |"
        echo ""
    } >> "$LOG_FILE"
}

require_cmd openssl
require_cmd python3
require_cmd curl
require_cmd cmake

mkdir -p "$TMP_DIR/www"
build_if_missing
cleanup_stale_proxies
QUIC_PORT=$(find_free_udp_port)
setup_netem

log "generating ${SIZE_MB}MB payload"
python3 - "$TMP_DIR/www/payload.bin" "$SIZE_MB" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
size_mb = int(sys.argv[2])
chunk = b"x" * (1024 * 1024)
with path.open("wb") as f:
    for _ in range(size_mb):
        f.write(chunk)
PY

log "generating test certificates"
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$TMP_DIR/ca.key" \
    -out "$TMP_DIR/ca.crt" \
    -subj "/CN=tcpquic-bench-ca" \
    -days 1 \
    -sha256 \
    >"$TMP_DIR/openssl-ca.log" 2>&1

cat > "$TMP_DIR/server.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF

cat > "$TMP_DIR/client.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:tcpquic-bench-client
EOF

generate_cert server localhost server.ext
generate_cert client tcpquic-bench-client client.ext

log "starting target HTTP server on 127.0.0.1:${TARGET_HTTP_PORT}"
python3 -m http.server "$TARGET_HTTP_PORT" --bind 127.0.0.1 --directory "$TMP_DIR/www" \
    >"$TMP_DIR/http.log" 2>&1 &
HTTP_PID=$!
wait_tcp 127.0.0.1 "$TARGET_HTTP_PORT" "target HTTP server"

IFS=',' read -r _ DIRECT_BPS DIRECT_MBPS < <(run_mode direct)
IFS=',' read -r _ OFF_BPS OFF_MBPS < <(run_mode tunnel_off)
IFS=',' read -r _ ZSTD_BPS ZSTD_MBPS < <(run_mode tunnel_zstd)
stop_proxy

log "summary:"
log "  direct:      ${DIRECT_MBPS} Mbps"
log "  tunnel off:  ${OFF_MBPS} Mbps"
log "  tunnel zstd: ${ZSTD_MBPS} Mbps"

if [ "$SKIP_APPEND" != "1" ]; then
    append_research_log "$DIRECT_BPS" "$DIRECT_MBPS" "$OFF_BPS" "$OFF_MBPS" "$ZSTD_BPS" "$ZSTD_MBPS"
    log "appended results to ${LOG_FILE}"
fi

log "DONE"
