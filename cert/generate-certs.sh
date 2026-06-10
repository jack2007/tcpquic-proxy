#!/usr/bin/env bash
# Generate 10-year internal mTLS test certificates for tcpquic-proxy.
set -euo pipefail

CERT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAYS="${CERT_DAYS:-3650}"

require_openssl() {
    command -v openssl >/dev/null 2>&1 || {
        echo "error: openssl not found in PATH" >&2
        exit 1
    }
}

log() {
    printf '[generate-certs] %s\n' "$*"
}

generate() {
    require_openssl
    mkdir -p "$CERT_ROOT/server" "$CERT_ROOT/client"

    log "generating CA (${DAYS} days) in $CERT_ROOT"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$CERT_ROOT/ca.key" \
        -out "$CERT_ROOT/ca.crt" \
        -subj "/CN=tcpquic-internal-test-ca/O=tcpquic-proxy/C=CN" \
        -days "$DAYS" -sha256

    cat > "$CERT_ROOT/server/server.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,DNS:tcpquic-server,IP:127.0.0.1,IP:169.254.250.230,IP:169.254.59.196
EOF

    cat > "$CERT_ROOT/client/client.ext" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:localhost,DNS:tcpquic-client,IP:127.0.0.1
EOF

    log "generating server leaf certificate"
    openssl req -newkey rsa:2048 -nodes \
        -keyout "$CERT_ROOT/server/server.key" \
        -out "$CERT_ROOT/server/server.csr" \
        -subj "/CN=tcpquic-server/O=tcpquic-proxy/C=CN"
    openssl x509 -req \
        -in "$CERT_ROOT/server/server.csr" \
        -CA "$CERT_ROOT/ca.crt" -CAkey "$CERT_ROOT/ca.key" -CAcreateserial \
        -out "$CERT_ROOT/server/server.crt" \
        -days "$DAYS" -sha256 -extfile "$CERT_ROOT/server/server.ext"

    log "generating client leaf certificate"
    openssl req -newkey rsa:2048 -nodes \
        -keyout "$CERT_ROOT/client/client.key" \
        -out "$CERT_ROOT/client/client.csr" \
        -subj "/CN=tcpquic-client/O=tcpquic-proxy/C=CN"
    openssl x509 -req \
        -in "$CERT_ROOT/client/client.csr" \
        -CA "$CERT_ROOT/ca.crt" -CAkey "$CERT_ROOT/ca.key" -CAcreateserial \
        -out "$CERT_ROOT/client/client.crt" \
        -days "$DAYS" -sha256 -extfile "$CERT_ROOT/client/client.ext"

    rm -f \
        "$CERT_ROOT/server/server.csr" "$CERT_ROOT/server/server.ext" \
        "$CERT_ROOT/client/client.csr" "$CERT_ROOT/client/client.ext" \
        "$CERT_ROOT/ca.srl"

    openssl verify -CAfile "$CERT_ROOT/ca.crt" "$CERT_ROOT/server/server.crt" >/dev/null
    openssl verify -CAfile "$CERT_ROOT/ca.crt" "$CERT_ROOT/client/client.crt" >/dev/null

    log "done"
    log "  CA:     $CERT_ROOT/ca.crt"
    log "  server: $CERT_ROOT/server/server.crt"
    log "  client: $CERT_ROOT/client/client.crt"
}

generate
