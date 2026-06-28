#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${TCPQUIC_CONFIG_ROUTER_TEST_BIN:-}" ]]; then
  CONFIG_ROUTER_TEST="$TCPQUIC_CONFIG_ROUTER_TEST_BIN"
else
  BIN="${TCPQUIC_PROXY_BIN:-$ROOT/build/bin/Release/tcpquic-proxy}"
  BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
  CONFIG_ROUTER_TEST="$BIN_DIR/tcpquic_config_router_test"
fi

if [[ ! -x "$CONFIG_ROUTER_TEST" ]]; then
  echo "missing tcpquic_config_router_test binary: $CONFIG_ROUTER_TEST" >&2
  echo "build it with: cmake --build build --target tcpquic_config_router_test -j" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/client-paths.json" <<'JSON'
{
  "version": 1,
  "peers": [
    {
      "peer_id": "loopback-paths",
      "paths": [
        {
          "name": "lo-a",
          "local": "127.0.0.1",
          "peer": "127.0.0.1:14443",
          "connections": 1
        }
      ],
      "socks_listen": "127.0.0.1:11080"
    }
  ]
}
JSON

# There is no no-network config validation mode in tcpquic-proxy. In particular,
# client --client-config ... --usage returns before loading the config. Keep this
# script as a shell/config-example smoke and use the unit test for real parser
# validation of peer lists, paths, and invalid path constraints.
"$CONFIG_ROUTER_TEST"

echo "multi-interface paths smoke passed: generated $tmp/client-paths.json and ran tcpquic_config_router_test"
