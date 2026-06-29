#!/usr/bin/env bash
set -euo pipefail

impl="${1:-src/runtime/admin_console.cpp}"
ref="docs/superpowers/specs/2026-06-29-admin-console-interaction-v4.html"

grep -q "raypx2 Admin Console" "$impl"
grep -q "Create/Edit Peer" "$impl"
grep -q "server-acl" "$impl"
grep -q "client-tunnels" "$impl"
grep -q "server-tunnels" "$impl"

if grep -q "<strong>listen</strong>" "$impl"; then
  echo "admin console must not show admin listen in topbar" >&2
  exit 1
fi

if grep -q "remote_identity\\|first_seen\\|last_seen\\|transferred_bytes\\|connected_at\\|disconnected_at" "$impl"; then
  echo "admin console contains fields outside current API contract" >&2
  exit 1
fi

grep -q "Interaction Design v4" "$ref"
echo "admin console v4 guard passed"
