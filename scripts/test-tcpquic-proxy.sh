#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LEGACY_BIN="$ROOT_DIR/build/bin/Release/tcpquic-proxy"
RENAMED_BIN="$ROOT_DIR/build/bin/Release/raypx2"
USER_BIN="${TCPQUIC_PROXY_BIN:-${BIN:-}}"
BIN=""
TMP_DIR="/tmp/tcpquic-test-$$"

SERVER_PID=""
CLIENT_PID=""
HTTP_PID=""
ECHO_PID=""
NEG_SERVER_PID=""
NEG_CLIENT_PID=""
SERVER_STDIN_FD=""
NEG_SERVER_STDIN_FD=""

log() {
    printf '[tcpquic-test] %s\n' "$*"
}

resolve_default_bin() {
    if [ -x "$LEGACY_BIN" ]; then
        BIN="$LEGACY_BIN"
    elif [ ! -e "$LEGACY_BIN" ] && [ -x "$RENAMED_BIN" ]; then
        BIN="$RENAMED_BIN"
    else
        BIN="$LEGACY_BIN"
    fi
}

if [ -n "$USER_BIN" ]; then
    BIN="$USER_BIN"
else
    resolve_default_bin
fi

cleanup() {
    local status=$?
    set +e
    for pid in "$CLIENT_PID" "$SERVER_PID" "$HTTP_PID" "$ECHO_PID" "$NEG_CLIENT_PID" "$NEG_SERVER_PID"; do
        if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
        fi
    done
    for pid in "$CLIENT_PID" "$SERVER_PID" "$HTTP_PID" "$ECHO_PID" "$NEG_CLIENT_PID" "$NEG_SERVER_PID"; do
        if [ -n "${pid:-}" ]; then
            wait "$pid" 2>/dev/null
        fi
    done
    if [ -n "${SERVER_STDIN_FD:-}" ]; then
        eval "exec ${SERVER_STDIN_FD}>&-"
    fi
    if [ -n "${NEG_SERVER_STDIN_FD:-}" ]; then
        eval "exec ${NEG_SERVER_STDIN_FD}>&-"
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

fail_with_logs() {
    log "FAILED: $*"
    for log_file in "$TMP_DIR"/*.log; do
        [ -f "$log_file" ] || continue
        printf '\n===== %s =====\n' "$(basename "$log_file")"
        sed -n '1,160p' "$log_file"
    done
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail_with_logs "missing required command: $1"
}

wait_tcp() {
    local host=$1
    local port=$2
    local name=$3
    local deadline=$((SECONDS + 15))
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
    fail_with_logs "timed out waiting for $name on $host:$port"
}

wait_for_open_port() {
    local host=$1
    local port=$2
    local deadline=$((SECONDS + 5))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
            exec 3>&- 2>/dev/null || true
            return 0
        fi
        sleep 0.2
    done
    fail_with_logs "timed out waiting for open port on $host:$port"
}

wait_for_closed_port() {
    local host=$1
    local port=$2
    local deadline=$((SECONDS + 2))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then
            exec 3>&- 2>/dev/null || true
            fail_with_logs "port is open on $host:$port"
        fi
        sleep 0.2
    done
    return 0
}

pick_free_tcp_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

wait_log() {
    local file=$1
    local pattern=$2
    local name=$3
    local deadline=$((SECONDS + 15))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if grep -q "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    fail_with_logs "timed out waiting for $name"
}

build_if_missing() {
    if [ -x "$BIN" ]; then
        return 0
    fi

    log "tcpquic-proxy binary missing; building standalone target"
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT_DIR/build" --config Release --target tcpquic-proxy -j"$(nproc)"

    if [ -z "$USER_BIN" ]; then
        resolve_default_bin
    fi
    [ -x "$BIN" ] || fail_with_logs "binary still missing after build: $BIN"
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

http_connect_status() {
    local proxy_host=$1
    local proxy_port=$2
    local target_host=$3
    local target_port=$4
    python3 - "$proxy_host" "$proxy_port" "$target_host" "$target_port" <<'PY'
import socket
import sys

proxy_host, proxy_port, target_host, target_port = sys.argv[1:5]
proxy_port = int(proxy_port)
target_port = int(target_port)

request = (
    f"CONNECT {target_host}:{target_port} HTTP/1.1\r\n"
    f"Host: {target_host}:{target_port}\r\n"
    "\r\n"
).encode("ascii")

with socket.create_connection((proxy_host, proxy_port), timeout=15) as s:
    s.sendall(request)
    data = s.recv(256)

if not data:
    print("000")
    raise SystemExit(0)

line = data.split(b"\r\n", 1)[0].decode("ascii", errors="replace")
parts = line.split()
if len(parts) < 2 or parts[0] != "HTTP/1.1":
    print("000")
else:
    print(parts[1])
PY
}

socks5_connect_rep() {
    local proxy_host=$1
    local proxy_port=$2
    local target_host=$3
    local target_port=$4
    python3 - "$proxy_host" "$proxy_port" "$target_host" "$target_port" <<'PY'
import socket
import struct
import sys

proxy_host, proxy_port, target_host, target_port = sys.argv[1:5]
target_port = int(target_port)
proxy_port = int(proxy_port)

with socket.create_connection((proxy_host, proxy_port), timeout=15) as s:
    s.sendall(b"\x05\x01\x00")
    resp = s.recv(2)
    if len(resp) != 2 or resp[0] != 0x05:
        print("255")
        raise SystemExit(0)

    host_b = target_host.encode("utf-8")
    req = (
        b"\x05\x01\x00\x03"
        + bytes([len(host_b)])
        + host_b
        + struct.pack(">H", target_port)
    )
    s.sendall(req)
    head = s.recv(4)
    if len(head) < 2:
        print("255")
        raise SystemExit(0)
    rep = head[1]
    if len(head) >= 4:
        atyp = head[3]
        if atyp == 0x01:
            s.recv(6)
        elif atyp == 0x03:
            rest = s.recv(1)
            if rest:
                s.recv(rest[0] + 2)
        elif atyp == 0x04:
            s.recv(18)
    print(rep)
PY
}

start_server() {
    local listen_port=$1
    local allow_targets=$2
    local compress_mode=$3
    local log_file=$4
    local stdin_fifo=$5

    mkfifo "$stdin_fifo"
    eval "exec {fd}<>\"$stdin_fifo\""
    "$BIN" server \
        --listen "127.0.0.1:${listen_port}" \
        --allow-targets "$allow_targets" \
        --cert "$TMP_DIR/server.crt" \
        --key "$TMP_DIR/server.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress "$compress_mode" \
        >"$log_file" 2>&1 <"$stdin_fifo" &
    echo "$! $fd"
}

start_client() {
    local quic_port=$1
    local http_port=$2
    local socks_port=$3
    local compress_mode=$4
    local log_file=$5
    local quic_connections=${6:-1}

    "$BIN" client \
        --peer "127.0.0.1:${quic_port}" \
        --http-listen "127.0.0.1:${http_port}" \
        --socks-listen "127.0.0.1:${socks_port}" \
        --cert "$TMP_DIR/client.crt" \
        --key "$TMP_DIR/client.key" \
        --ca "$TMP_DIR/ca.crt" \
        --connections "$quic_connections" \
        --compress "$compress_mode" \
        >"$log_file" 2>&1 &
    echo "$!"
}

start_client_with_forward() {
    local quic_port=$1
    local http_port=$2
    local socks_port=$3
    local forward_port=$4
    local target_port=$5
    local compress_mode=$6
    local log_file=$7

    "$BIN" client \
        --peer "127.0.0.1:${quic_port}" \
        --http-listen "127.0.0.1:${http_port}" \
        --socks-listen "127.0.0.1:${socks_port}" \
        --forward "127.0.0.1:${forward_port}=127.0.0.1:${target_port}" \
        --cert "$TMP_DIR/client.crt" \
        --key "$TMP_DIR/client.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress "$compress_mode" \
        >"$log_file" 2>&1 &
    echo "$!"
}

start_configured_client() {
    local config_file=$1
    local log_file=$2

    "$BIN" client \
        --client-config "$config_file" \
        --cert "$TMP_DIR/client.crt" \
        --key "$TMP_DIR/client.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress off \
        >"$log_file" 2>&1 &
    echo "$!"
}

require_cmd openssl
require_cmd python3
require_cmd curl
require_cmd cmake

mkdir -p "$TMP_DIR/www"
printf 'tcpquic-proxy smoke test\n' > "$TMP_DIR/www/index.html"
python3 - "$TMP_DIR/www/large.txt" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_text(("compress-me-" * 4096) + "\n", encoding="utf-8")
PY

build_if_missing

log "rejecting invalid CIDR at CLI parse time"
if "$BIN" server \
    --listen 127.0.0.1:9 \
    --allow-targets "not-a-cidr" \
    --cert "$TMP_DIR/server.crt" \
    --key "$TMP_DIR/server.key" \
    --ca "$TMP_DIR/ca.crt" \
    >"$TMP_DIR/cli-bad-cidr.log" 2>&1; then
    fail_with_logs "server accepted invalid --allow-targets"
fi
grep -q "invalid CIDR" "$TMP_DIR/cli-bad-cidr.log" ||
    fail_with_logs "expected invalid CIDR error message"

log "generating test certificates in $TMP_DIR"
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$TMP_DIR/ca.key" \
    -out "$TMP_DIR/ca.crt" \
    -subj "/CN=tcpquic-test-ca" \
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
subjectAltName=DNS:tcpquic-test-client
EOF

generate_cert server localhost server.ext
generate_cert client tcpquic-test-client client.ext

log "starting target HTTP server on 127.0.0.1:15001"
python3 -m http.server 15001 --bind 127.0.0.1 --directory "$TMP_DIR/www" \
    >"$TMP_DIR/http.log" 2>&1 &
HTTP_PID=$!
wait_tcp 127.0.0.1 15001 "target HTTP server"

HEALTHY_PEER_HTTP_PORT=8080
HEALTHY_PEER_SOCKS_PORT=1080
DOWN_PEER_HTTP_PORT=8082
DOWN_PEER_SOCKS_PORT=11882
DOWN_PEER_QUIC_PORT=$(python3 <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
    s.bind(("127.0.0.1", 0))
    print(s.getsockname()[1])
PY
)

read -r SERVER_PID SERVER_STDIN_FD < <(start_server 4433 "127.0.0.0/8" off "$TMP_DIR/proxy-server.log" "$TMP_DIR/server.stdin")
wait_log "$TMP_DIR/proxy-server.log" "QUIC server listening" "tcpquic-proxy server"

cat > "$TMP_DIR/client-config.json" <<EOF
{"version":1,"peers":[{"peer_id":"healthy","quic_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:${HEALTHY_PEER_SOCKS_PORT}","http_listen":"127.0.0.1:${HEALTHY_PEER_HTTP_PORT}","compress":"off"},{"peer_id":"down","quic_peer":"127.0.0.1:${DOWN_PEER_QUIC_PORT}","socks_listen":"127.0.0.1:${DOWN_PEER_SOCKS_PORT}","http_listen":"127.0.0.1:${DOWN_PEER_HTTP_PORT}","compress":"off"}]}
EOF

CLIENT_PID=$(start_configured_client "$TMP_DIR/client-config.json" "$TMP_DIR/proxy-client.log")
wait_for_open_port 127.0.0.1 "$HEALTHY_PEER_SOCKS_PORT"
wait_for_open_port 127.0.0.1 "$HEALTHY_PEER_HTTP_PORT"
wait_for_closed_port 127.0.0.1 "$DOWN_PEER_SOCKS_PORT"
wait_for_closed_port 127.0.0.1 "$DOWN_PEER_HTTP_PORT"
wait_tcp 127.0.0.1 8080 "HTTP CONNECT listener"
wait_tcp 127.0.0.1 1080 "SOCKS5 listener"

log "testing HTTP CONNECT proxy (happy path)"
curl -fsS \
    -x http://127.0.0.1:8080 \
    --proxytunnel \
    http://127.0.0.1:15001/ \
    --max-time 10 \
    -o "$TMP_DIR/http-connect.out" \
    >"$TMP_DIR/curl-http-connect.log" 2>&1 ||
    fail_with_logs "HTTP CONNECT curl failed"
grep -q "tcpquic-proxy smoke test" "$TMP_DIR/http-connect.out" ||
    fail_with_logs "HTTP CONNECT response did not contain expected body"

log "testing SOCKS5 proxy (happy path)"
curl -fsS \
    --socks5-hostname 127.0.0.1:1080 \
    http://127.0.0.1:15001/ \
    --max-time 10 \
    -o "$TMP_DIR/socks5.out" \
    >"$TMP_DIR/curl-socks5.log" 2>&1 ||
    fail_with_logs "SOCKS5 curl failed"
grep -q "tcpquic-proxy smoke test" "$TMP_DIR/socks5.out" ||
    fail_with_logs "SOCKS5 response did not contain expected body"

FORWARD_TARGET_PORT=$(pick_free_tcp_port)
FORWARD_LISTEN_PORT=$(pick_free_tcp_port)

log "starting forward echo target on 127.0.0.1:${FORWARD_TARGET_PORT}"
python3 - "$FORWARD_TARGET_PORT" >"$TMP_DIR/forward-echo.log" 2>&1 <<'PY' &
import socket
import sys

port = int(sys.argv[1])
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(16)
while True:
    conn, _ = srv.accept()
    data = conn.recv(4096)
    if data:
        conn.sendall(data)
    conn.close()
PY
ECHO_PID=$!
wait_tcp 127.0.0.1 "$FORWARD_TARGET_PORT" "forward echo target"

kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""
CLIENT_PID=$(start_client_with_forward 4433 8080 1080 "$FORWARD_LISTEN_PORT" "$FORWARD_TARGET_PORT" off "$TMP_DIR/proxy-client-forward.log")
wait_tcp 127.0.0.1 "$FORWARD_LISTEN_PORT" "port forward listener"

python3 - "$FORWARD_LISTEN_PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
    s.sendall(b"port-forward-ok")
    data = s.recv(4096)
if data != b"port-forward-ok":
    raise SystemExit(f"unexpected echo: {data!r}")
PY

REFUSED_PORT=59998
while python3 - "$REFUSED_PORT" <<'PY' >/dev/null 2>&1
import socket
import sys

port = int(sys.argv[1])
with socket.create_connection(("127.0.0.1", port), timeout=0.25):
    pass
PY
do
    REFUSED_PORT=$((REFUSED_PORT + 1))
done

log "testing HTTP CONNECT negative paths"
status=$(http_connect_status 127.0.0.1 8080 127.0.0.1 15001)
[ "$status" = "200" ] || fail_with_logs "expected HTTP 200 for allowed target, got $status"

read -r NEG_SERVER_PID NEG_SERVER_STDIN_FD < <(start_server 4434 "10.0.0.0/8" off "$TMP_DIR/neg-server.log" "$TMP_DIR/neg-server.stdin")
wait_log "$TMP_DIR/neg-server.log" "QUIC server listening" "restrictive server"
NEG_CLIENT_PID=$(start_client 4434 8081 11881 off "$TMP_DIR/neg-client.log")
wait_tcp 127.0.0.1 8081 "negative HTTP listener"
wait_tcp 127.0.0.1 11881 "negative SOCKS5 listener"

status=$(http_connect_status 127.0.0.1 8081 127.0.0.1 15001)
[ "$status" = "403" ] || fail_with_logs "ACL deny expected HTTP 403, got $status"

status=$(http_connect_status 127.0.0.1 8080 "tcpquic-nonexistent.invalid" 80)
[ "$status" = "502" ] || fail_with_logs "DNS fail expected HTTP 502, got $status"

status=$(http_connect_status 127.0.0.1 8080 127.0.0.1 "$REFUSED_PORT")
[ "$status" = "502" ] || fail_with_logs "TCP refused expected HTTP 502, got $status"

log "testing SOCKS5 negative paths"
rep=$(socks5_connect_rep 127.0.0.1 11881 127.0.0.1 15001)
[ "$rep" = "2" ] || fail_with_logs "ACL deny expected SOCKS5 REP 0x02, got $rep"

rep=$(socks5_connect_rep 127.0.0.1 1080 "tcpquic-nonexistent.invalid" 80)
[ "$rep" = "4" ] || fail_with_logs "DNS fail expected SOCKS5 REP 0x04, got $rep"

rep=$(socks5_connect_rep 127.0.0.1 1080 127.0.0.1 "$REFUSED_PORT")
[ "$rep" = "5" ] || fail_with_logs "TCP refused expected SOCKS5 REP 0x05, got $rep"

kill "$NEG_CLIENT_PID" "$NEG_SERVER_PID" 2>/dev/null || true
wait "$NEG_CLIENT_PID" 2>/dev/null || true
wait "$NEG_SERVER_PID" 2>/dev/null || true
NEG_CLIENT_PID=""
NEG_SERVER_PID=""
eval "exec ${NEG_SERVER_STDIN_FD}>&-"
NEG_SERVER_STDIN_FD=""

log "testing zstd compression end-to-end"
kill "$CLIENT_PID" "$SERVER_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
CLIENT_PID=""
SERVER_PID=""
eval "exec ${SERVER_STDIN_FD}>&-"
SERVER_STDIN_FD=""

read -r SERVER_PID SERVER_STDIN_FD < <(start_server 4433 "127.0.0.0/8" zstd "$TMP_DIR/proxy-server-zstd.log" "$TMP_DIR/server-zstd.stdin")
wait_log "$TMP_DIR/proxy-server-zstd.log" "QUIC server listening" "zstd server"
CLIENT_PID=$(start_client 4433 8080 1080 zstd "$TMP_DIR/proxy-client-zstd.log")
wait_tcp 127.0.0.1 8080 "zstd HTTP listener"

curl -fsS \
    -x http://127.0.0.1:8080 \
    --proxytunnel \
    "http://127.0.0.1:15001/large.txt" \
    --max-time 15 \
    -o "$TMP_DIR/http-connect-zstd.out" \
    >"$TMP_DIR/curl-http-connect-zstd.log" 2>&1 ||
    fail_with_logs "zstd HTTP CONNECT curl failed"
grep -q "compress-me-" "$TMP_DIR/http-connect-zstd.out" ||
    fail_with_logs "zstd compressed tunnel did not return large payload"

log "testing QUIC connection pool (--connections 4)"
kill "$CLIENT_PID" "$SERVER_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
CLIENT_PID=""
SERVER_PID=""
eval "exec ${SERVER_STDIN_FD}>&-"
SERVER_STDIN_FD=""

read -r SERVER_PID SERVER_STDIN_FD < <(start_server 4433 "127.0.0.0/8" off "$TMP_DIR/proxy-server-pool.log" "$TMP_DIR/server-pool.stdin")
wait_log "$TMP_DIR/proxy-server-pool.log" "QUIC server listening" "pool server"
CLIENT_PID=$(start_client 4433 8080 1080 off "$TMP_DIR/proxy-client-pool.log" 4)
wait_tcp 127.0.0.1 8080 "pool HTTP listener"

curl -fsS \
    -x http://127.0.0.1:8080 \
    --proxytunnel \
    "http://127.0.0.1:15001/index.html" \
    --max-time 15 \
    -o "$TMP_DIR/http-connect-pool.out" \
    >"$TMP_DIR/curl-http-connect-pool.log" 2>&1 ||
    fail_with_logs "pool HTTP CONNECT curl failed"
grep -q "ok" "$TMP_DIR/http-connect-pool.out" ||
    fail_with_logs "pool tunnel did not return payload"

log "PASS"
