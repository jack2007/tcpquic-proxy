#!/usr/bin/env bash
# 双机 tcpquic-proxy 端到端冒烟：HTTP CONNECT + 可选 zstd + 连接池
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-${ROOT}/build/bin/Release/tcpquic-proxy}"
REMOTE_DIR="${REMOTE_DIR:-~/tcpquic-dgx-bin}"
REMOTE_BIN="${REMOTE_BIN:-${REMOTE_DIR}/tcpquic-proxy}"
TARGET="${TARGET:-169.254.59.196}"
BIND="${BIND:-169.254.250.230}"
PEER="${PEER:-jack@${TARGET}}"
QUIC_PORT="${QUIC_PORT:-4433}"
HTTP_PORT="${HTTP_PORT:-16001}"
PROXY_PORT="${PROXY_PORT:-18080}"
COMPRESS="${COMPRESS:-off}"
QUIC_CONNECTIONS="${QUIC_CONNECTIONS:-4}"
TUNNELS="${TUNNELS:-0}"

TMP="/tmp/tcpquic-dgx-smoke-$$"
CLIENT_PID=""

log() { printf '[tcpquic-dgx-smoke] %s\n' "$*" >&2; }

remote() { ssh "$PEER" "$@"; }

remote_kill() {
    ssh "$PEER" "killall -9 tcpquic-proxy 2>/dev/null; fuser -k ${HTTP_PORT}/tcp >/dev/null 2>&1; true" || true
    pkill -9 -f "${BIN} client" 2>/dev/null || true
}

cleanup() {
    set +e
    remote_kill
    [[ -n "${CLIENT_PID:-}" ]] && kill -9 "$CLIENT_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

ensure_remote_bin() {
    if remote "test -x ${REMOTE_BIN}"; then
        return 0
    fi
    log "deploying binary to ${PEER}:${REMOTE_DIR}"
    remote "mkdir -p ${REMOTE_DIR}"
    rsync -az "$BIN" "${ROOT}/build/bin/Release/libmsquic.so.2" \
        "${ROOT}/build/bin/Release/libmsquic.so.2.6.0" \
        "${PEER}:${REMOTE_DIR}/"
    remote "chmod +x ${REMOTE_BIN}"
}

generate_certs() {
    mkdir -p "$TMP/certs"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$TMP/certs/ca.key" -out "$TMP/certs/ca.crt" \
        -subj "/CN=tcpquic-dgx-smoke-ca" -days 1 -sha256 >/dev/null 2>&1
    cat > "$TMP/certs/server.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:169.254.59.196
EOF
    cat > "$TMP/certs/client.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:tcpquic-dgx-smoke-client
EOF
    for n in server client; do
        openssl req -newkey rsa:2048 -nodes \
            -keyout "$TMP/certs/$n.key" -out "$TMP/certs/$n.csr" \
            -subj "/CN=$n" >/dev/null 2>&1
        openssl x509 -req -in "$TMP/certs/$n.csr" \
            -CA "$TMP/certs/ca.crt" -CAkey "$TMP/certs/ca.key" -CAcreateserial \
            -out "$TMP/certs/$n.crt" -days 1 -sha256 \
            -extfile "$TMP/certs/$n.ext" >/dev/null 2>&1
    done
    rsync -az "$TMP/certs/" "${PEER}:~/tcpquic-dgx-certs/"
}

[[ -x "$BIN" ]] || { log "missing $BIN"; exit 1; }
ensure_remote_bin
remote_kill
generate_certs

printf 'tcpquic-dgx-ok\n' > "$TMP/index.html"
rsync -az "$TMP/index.html" "${PEER}:~/tcpquic-dgx-index.html"

remote "python3 -m http.server ${HTTP_PORT} --bind ${TARGET} --directory . >/tmp/tcpquic-dgx-http.log 2>&1 & sleep 0.5"
remote "test -f ~/tcpquic-dgx-index.html"

remote "LD_LIBRARY_PATH=${REMOTE_DIR} nohup ${REMOTE_BIN} server \
    --quic-listen ${TARGET}:${QUIC_PORT} \
    --allow-targets ${TARGET}/32,127.0.0.0/8 \
    --quic-cert ~/tcpquic-dgx-certs/server.crt \
    --quic-key ~/tcpquic-dgx-certs/server.key \
    --quic-ca ~/tcpquic-dgx-certs/ca.crt \
    --compress ${COMPRESS} \
    </dev/null >/tmp/tcpquic-dgx-server.log 2>&1 &"

for _ in $(seq 1 20); do
    remote "grep -q 'QUIC server listening' /tmp/tcpquic-dgx-server.log 2>/dev/null" && break
    sleep 0.3
done
remote "grep -q 'QUIC server listening' /tmp/tcpquic-dgx-server.log"

"$BIN" client \
    --quic-peer "${TARGET}:${QUIC_PORT}" \
    --http-listen "127.0.0.1:${PROXY_PORT}" \
    --socks-listen "127.0.0.1:$((PROXY_PORT + 1000))" \
    --quic-cert "$TMP/certs/client.crt" \
    --quic-key "$TMP/certs/client.key" \
    --quic-ca "$TMP/certs/ca.crt" \
    --quic-connections "$QUIC_CONNECTIONS" \
    --compress "$COMPRESS" \
    >/tmp/tcpquic-dgx-smoke-client.log 2>&1 &
CLIENT_PID=$!

for _ in $(seq 1 60); do
    grep -q "HTTP CONNECT listening" /tmp/tcpquic-dgx-smoke-client.log 2>/dev/null && break
    sleep 0.5
done
grep -q "HTTP CONNECT listening" /tmp/tcpquic-dgx-smoke-client.log || {
    log "client failed to start; log tail:"
    tail -20 /tmp/tcpquic-dgx-smoke-client.log >&2 || true
    exit 1
}

curl -fsS --interface "$BIND" \
    -x "http://127.0.0.1:${PROXY_PORT}" --proxytunnel \
    "http://${TARGET}:${HTTP_PORT}/tcpquic-dgx-index.html" \
    -m 30 -o "$TMP/out.html"
grep -q "tcpquic-dgx-ok" "$TMP/out.html"
log "HTTP CONNECT tunnel OK (compress=${COMPRESS}, quic-connections=${QUIC_CONNECTIONS})"

if [[ "$TUNNELS" != "0" && -n "$TUNNELS" ]]; then
    log "concurrent tunnel check: ${TUNNELS}"
    python3 - "$TUNNELS" "$PROXY_PORT" "$TARGET" "$HTTP_PORT" <<'PY'
import concurrent.futures, socket, sys
count = int(sys.argv[1])
proxy = ("127.0.0.1", int(sys.argv[2]))
target_host, target_port = sys.argv[3], int(sys.argv[4])

def one(_):
    req = (
        f"CONNECT {target_host}:{target_port} HTTP/1.1\r\n"
        f"Host: {target_host}:{target_port}\r\n\r\n"
    ).encode()
    with socket.create_connection(proxy, timeout=60) as s:
        s.settimeout(60)
        s.sendall(req)
        head = b""
        while b"\r\n\r\n" not in head and len(head) < 512:
            head += s.recv(256)
        if b" 200 " not in head.split(b"\r\n", 1)[0]:
            return False
        s.sendall(b"GET /tcpquic-dgx-index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = b""
        while b"tcpquic-dgx-ok" not in data and len(data) < 4096:
            chunk = s.recv(512)
            if not chunk:
                break
            data += chunk
        return b"tcpquic-dgx-ok" in data

with concurrent.futures.ThreadPoolExecutor(max_workers=min(count, 64)) as pool:
    ok = sum(1 for r in pool.map(one, range(count)) if r)
print(f"OK={ok} TOTAL={count}")
if ok != count:
    raise SystemExit(1)
PY
    log "concurrent ${TUNNELS}/${TUNNELS} OK"
fi

log "PASS"
