# Admin V1 Only and Single-Peer Router Design

## Context

Admin HTTP currently supports two public URL shapes:

- legacy paths such as `/health`, `/metrics`, `/config`, and `/peers/{id}/enable`
- v1 paths under `/api/v1/*`

The v1 layer already authenticates with bearer tokens and internally strips `/api/v1` before calling existing handlers. The legacy compatibility switch `--admin-allow-unauthenticated-legacy` allows a small set of old paths to skip token authentication on loopback. The single-peer client path also has a separate Admin handler that only exposes health and metrics, while multi-peer client mode uses `TqRouterRuntime::HandleAdmin`.

The target state is simpler: Admin has one public API, `/api/v1/*`, protected by bearer token. Single-peer mode uses the same router Admin model as multi-peer mode, with one peer in the peers collection.

## Goals

- Remove `--admin-allow-unauthenticated-legacy` and `admin.allow_unauthenticated_legacy`.
- Remove public legacy Admin paths.
- Keep bearer-token authentication for every recognized `/api/v1/*` request.
- Keep server-only Admin support through `/api/v1/health`, `/api/v1/metrics`, and `/api/v1/server/*`.
- Route single-peer client Admin through `TqRouterRuntime`, where the only configured peer is `primary`.
- Preserve speed-test startup behavior; speed tests still use the direct single-peer path and do not need Admin.

## Non-Goals

- Do not redesign token file generation, token file permissions, or bearer-token comparison.
- Do not change the JSON shape of router peer, connection, tunnel, relay, or server resource responses.
- Do not add redirects from legacy paths to v1 paths.
- Do not remove internal handler paths such as `/health` and `/peers/{id}:enable`; they remain implementation details behind the v1 adapter.

## Public API Behavior

Only `/api/v1/*` is public.

Authentication behavior:

- Missing or wrong token on recognized `/api/v1/*` returns `401`.
- Correct token on a recognized v1 path reaches the handler.
- Correct token on an unknown `/api/v1/*` returns `404`.
- Non-v1 paths such as `/health`, `/metrics`, `/config`, and `/peers/primary/enable` return `404`.

Legacy paths do not become "token-required legacy" paths. They are removed from the public surface.

## Admin HTTP Routing Design

`TqAdminHttpServer::ConfigureRoutes()` becomes v1-only:

1. If `req.path` is not under `/api/v1`, return `404`.
2. If token auth exists and `Authorize()` fails, return `401`.
3. If `req.path` is not a known v1 Admin path, return `404`.
4. Convert the v1 path to the internal handler path with `TqV1ToLegacyPath()`.
5. Call the configured handler and adapt its raw HTTP response to `httplib::Response`.

`TqIsLegacyAdminPath()` and `TqAdminHttpServerOptions::AllowUnauthenticatedLegacy` are removed.

`TqV1ToLegacyPath()` is kept for now because `TqRouterRuntime::HandleAdmin()` and `TqHandleServerAdmin()` already operate on internal paths. This keeps the change focused on public routing and startup wiring.

## Single-Peer Client Design

`RunSinglePeerClient()` no longer creates a separate Admin lambda for health and metrics.

For normal non-speed-test single-peer client runs:

1. Build a `TqRouterConfig` containing exactly one peer from `TqMakePrimaryPeerConfig(cfg)`.
2. Preserve router-level proxy auth from `cfg.Router.ProxyAuth`.
3. Create `TqMultiPeerRuntimeAdapter adapter(cfg)`.
4. Create `TqRouterRuntime runtime(&adapter)`.
5. Apply the single-peer router config.
6. Start Admin with `runtime.HandleAdmin(req)`.

The peer id is `primary`, matching existing `TqMakePrimaryPeerConfig()`. Therefore `/api/v1/peers` returns one peer with `"peer_id":"primary"`.

Speed-test mode remains on the current direct `RunSinglePeerClient()` path because it needs immediate control-connection access and exits after the test. Admin is not part of that flow.

## Server-Only Admin Design

Server-only Admin remains supported. The same v1-only HTTP layer maps:

- `/api/v1/health` to internal `/health`
- `/api/v1/metrics` to internal `/metrics`
- `/api/v1/server/*` to internal `/server/*`

The server handler implementation in `src/runtime/server_admin.cpp` remains the role-specific business logic.

## Configuration and CLI Design

Remove these inputs:

- `--admin-allow-unauthenticated-legacy`
- `admin.allow_unauthenticated_legacy`

After removal:

- Passing the CLI flag fails argument parsing as an unknown argument.
- Providing the JSON config field fails config parsing as an unknown admin key.
- Help text no longer contains the flag.
- Startup logging no longer prints the legacy unauthenticated warning.

## Tests

Required regression coverage:

- `tcpquic_config_router_test`
  - usage text does not contain `--admin-allow-unauthenticated-legacy`
  - the removed CLI flag is rejected
  - the removed JSON admin key is rejected
  - normal `admin.listen`, `admin.token_file`, and `admin.threads` still parse

- `tcpquic_admin_http_test`
  - `/health` returns `404`
  - `/metrics` returns `404`
  - `/peers/agent-d/enable` returns `404`
  - `/api/v1/health` without token returns `401`
  - `/api/v1/health` with token returns `200`
  - `/api/v1/peers/{id}:enable` remains valid with token
  - `/api/v1/peers/{id}/enable` remains invalid with token

- `tcpquic_router_runtime_test`
  - bridge validation still enforces one active peer for bridge mode
  - router Admin already supports peer collection semantics; no separate single-peer Admin handler is needed

## Documentation

Update Admin API and config docs so they no longer advertise legacy compatibility or the removed flag. The docs should state that legacy paths are removed and all public Admin calls use `/api/v1/*` plus bearer token.

## Risks

- Existing scripts using `/health`, `/metrics`, or `/peers/{id}/enable` will break. This is intentional. They must move to `/api/v1/health`, `/api/v1/metrics`, and `/api/v1/peers/{id}:enable`.
- Single-peer Admin metrics move from the old single-peer health/metrics body to router-style peer collection metrics. This is intentional because single-peer is now represented as a router with one peer.
- Keeping internal handler paths means the code still contains names that look legacy. They are not public API after the routing change.

## Acceptance Criteria

- The removed CLI flag and JSON key are no longer accepted.
- No non-v1 Admin path reaches a handler.
- All recognized `/api/v1/*` paths require bearer token.
- Single-peer normal client startup uses `TqRouterRuntime::HandleAdmin`.
- Server-only Admin remains reachable through v1 paths.
- `tcpquic_config_router_test`, `tcpquic_admin_http_test`, and `tcpquic_router_runtime_test` pass.
