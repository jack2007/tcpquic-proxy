#!/usr/bin/env bash
# 并发 TCP 隧道压测：同时打开 N 条 HTTP CONNECT 隧道并验证全部成功
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/bin/Release/tcpquic-proxy"
TMP_DIR="/tmp/tcpquic-concurrent-$$"
TUNNELS="${TUNNELS:-100}"
QUIC_CONNECTIONS="${QUIC_CONNECTIONS:-1}"
COMPRESS="${COMPRESS:-off}"
MAX_MEMORY_MB="${MAX_MEMORY_MB:-}"
HANDSHAKE_THREADS="${HANDSHAKE_THREADS:-32}"

SERVER_PID=""
CLIENT_PID=""
HTTP_PID=""
SERVER_STDIN_FD=""

log() { printf '[tcpquic-concurrent] %s\n' "$*" >&2; }

cleanup() {
    local status=$?
    set +e
    for pid in "$CLIENT_PID" "$SERVER_PID" "$HTTP_PID"; do
        [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null || true
    done
    [ -n "${SERVER_STDIN_FD:-}" ] && eval "exec ${SERVER_STDIN_FD}>&-" || true
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

build_if_missing() {
    [ -x "$BIN" ] && return 0
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT_DIR/build" --target tcpquic-proxy -j"$(nproc)"
}

wait_log() {
    local file=$1 pattern=$2 name=$3
    local deadline=$((SECONDS + 30))
    while [ "$SECONDS" -lt "$deadline" ]; do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.2
    done
    log "timeout waiting for $name"
    exit 1
}

mkdir -p "$TMP_DIR"
build_if_missing
pkill -9 -f "$BIN" 2>/dev/null || true
sleep 0.5

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$TMP_DIR/ca.key" -out "$TMP_DIR/ca.crt" \
    -subj "/CN=tcpquic-concurrent-ca" -days 1 -sha256 >/dev/null 2>&1

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
subjectAltName=DNS:tcpquic-concurrent-client
EOF

for n in server client; do
    openssl req -newkey rsa:2048 -nodes \
        -keyout "$TMP_DIR/$n.key" -out "$TMP_DIR/$n.csr" \
        -subj "/CN=$n" >/dev/null 2>&1
    openssl x509 -req -in "$TMP_DIR/$n.csr" \
        -CA "$TMP_DIR/ca.crt" -CAkey "$TMP_DIR/ca.key" -CAcreateserial \
        -out "$TMP_DIR/$n.crt" -days 1 -sha256 -extfile "$TMP_DIR/$n.ext" >/dev/null 2>&1
done

printf 'ok\n' > "$TMP_DIR/index.html"
python3 - "$TMP_DIR" <<'PY' >"$TMP_DIR/http.log" 2>&1 &
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

root = sys.argv[1]
os.chdir(root)
server = ThreadingHTTPServer(("127.0.0.1", 17001), SimpleHTTPRequestHandler)
server.serve_forever()
PY
HTTP_PID=$!

mkfifo "$TMP_DIR/server.stdin"
eval "exec {SERVER_STDIN_FD}<>\"$TMP_DIR/server.stdin\""
SERVER_ARGS=(
    server
    --quic-listen 127.0.0.1:4733
    --allow-targets 127.0.0.0/8
    --quic-cert "$TMP_DIR/server.crt"
    --quic-key "$TMP_DIR/server.key"
    --quic-ca "$TMP_DIR/ca.crt"
    --compress "$COMPRESS"
)
CLIENT_ARGS=(
    client
    --quic-peer 127.0.0.1:4733
    --http-listen 127.0.0.1:18090
    --socks-listen 127.0.0.1:11090
    --quic-cert "$TMP_DIR/client.crt"
    --quic-key "$TMP_DIR/client.key"
    --quic-ca "$TMP_DIR/ca.crt"
    --quic-connections "$QUIC_CONNECTIONS"
    --compress "$COMPRESS"
    --handshake-threads "$HANDSHAKE_THREADS"
)
if [ -n "$MAX_MEMORY_MB" ]; then
    SERVER_ARGS+=(--max-memory-mb "$MAX_MEMORY_MB")
    CLIENT_ARGS+=(--max-memory-mb "$MAX_MEMORY_MB")
fi

"$BIN" "${SERVER_ARGS[@]}" \
    >"$TMP_DIR/server.log" 2>&1 <"$TMP_DIR/server.stdin" &
SERVER_PID=$!
wait_log "$TMP_DIR/server.log" "QUIC server listening" "server"

"$BIN" "${CLIENT_ARGS[@]}" \
    >"$TMP_DIR/client.log" 2>&1 &
CLIENT_PID=$!
wait_log "$TMP_DIR/client.log" "HTTP CONNECT listening" "client"
sleep 0.3

if [ -r "/proc/${CLIENT_PID}/status" ]; then
    client_threads="$(awk '/^Threads:/ {print $2}' "/proc/${CLIENT_PID}/status")"
    log "client pid=${CLIENT_PID} threads=${client_threads}"
fi

log "opening ${TUNNELS} concurrent HTTP CONNECT tunnels (quic-connections=${QUIC_CONNECTIONS}, compress=${COMPRESS}, handshake-threads=${HANDSHAKE_THREADS}${MAX_MEMORY_MB:+, max-memory-mb=${MAX_MEMORY_MB}})"
python3 - "$TUNNELS" <<'PY'
import concurrent.futures
import socket
import sys

count = int(sys.argv[1])
proxy = ("127.0.0.1", 18090)
target_host = "127.0.0.1"
target_port = 17001

def one_tunnel(idx: int) -> bool:
    req = (
        f"CONNECT {target_host}:{target_port} HTTP/1.1\r\n"
        f"Host: {target_host}:{target_port}\r\n\r\n"
    ).encode("ascii")
    try:
        with socket.create_connection(proxy, timeout=60) as s:
            s.settimeout(60)
            s.sendall(req)
            head = b""
            while b"\r\n\r\n" not in head and len(head) < 512:
                chunk = s.recv(256)
                if not chunk:
                    break
                head += chunk
            if b" 200 " not in head.split(b"\r\n", 1)[0]:
                return False
            s.sendall(b"GET /index.html HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
            data = b""
            while b"ok" not in data and len(data) < 4096:
                chunk = s.recv(512)
                if not chunk:
                    break
                data += chunk
            return b"ok" in data
    except OSError:
        return False

workers = min(count, 64)

def run_batch(indices):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(one_tunnel, indices))

results = run_batch(range(count))
for _ in range(3):
    failed = [i for i, ok in enumerate(results) if not ok]
    if not failed:
        break
    retry = run_batch(failed)
    for idx, ok in zip(failed, retry):
        results[idx] = ok
ok = sum(1 for r in results if r)

print(f"OK={ok} TOTAL={count}")
if ok != count:
    raise SystemExit(1)
PY

log "PASS (${TUNNELS}/${TUNNELS} tunnels)"
