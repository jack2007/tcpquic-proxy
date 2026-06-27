#!/usr/bin/env bash
set -euo pipefail

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

token_file="$tmpdir/admin-test.json"
cat >"$token_file" <<'JSON'
{
  "version": 1,
  "token_type": "Bearer",
  "token": "test-token",
  "listen": "127.0.0.1:2345",
  "pid": 12345,
  "created_at_unix": 1800000000
}
JSON

actual="$(scripts/admin-allocator-dump.sh --dry-run "$token_file")"
expected="curl -sS -X POST -H 'Authorization: Bearer test-token' -H 'Accept: application/json' 'http://127.0.0.1:2345/api/v1/memory/allocator:dump'"

if [[ "$actual" != "$expected" ]]; then
    printf 'unexpected dry-run output\nexpected: %s\nactual:   %s\n' "$expected" "$actual" >&2
    exit 1
fi
