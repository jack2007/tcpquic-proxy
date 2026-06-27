#!/usr/bin/env bash
set -euo pipefail

default_token_file="/run/user/0/tcpquic-proxy/admin-22944.json"
dry_run=0

usage() {
    cat >&2 <<'EOF'
Usage: scripts/admin-allocator-dump.sh [--dry-run] [admin-token-json]

Reads tcpquic-proxy admin token JSON and POSTs /api/v1/memory/allocator:dump.
Default admin-token-json: /run/user/0/tcpquic-proxy/admin-16097.json
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            dry_run=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            printf 'unknown option: %s\n' "$1" >&2
            usage
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

token_file="${1:-$default_token_file}"
if [[ $# -gt 1 ]]; then
    usage
    exit 2
fi

if [[ ! -r "$token_file" ]]; then
    printf 'admin token file is not readable: %s\n' "$token_file" >&2
    exit 1
fi

mapfile -t admin_fields < <(
    python3 - "$token_file" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

for key in ("token_type", "token", "listen"):
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise SystemExit(f"missing or invalid {key} in {path}")
    print(value)
PY
)

token_type="${admin_fields[0]}"
token="${admin_fields[1]}"
listen="${admin_fields[2]}"
url="http://${listen}/api/v1/memory/allocator:dump"

cmd=(
    curl
    -sS
    -X POST
    -H "Authorization: ${token_type} ${token}"
    -H "Accept: application/json"
    "$url"
)

shell_quote() {
    local value=$1
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

if [[ "$dry_run" -eq 1 ]]; then
    printf 'curl -sS -X POST -H '
    shell_quote "Authorization: ${token_type} ${token}"
    printf ' -H '
    shell_quote 'Accept: application/json'
    printf ' '
    shell_quote "$url"
    printf '\n'
    exit 0
fi

"${cmd[@]}"
