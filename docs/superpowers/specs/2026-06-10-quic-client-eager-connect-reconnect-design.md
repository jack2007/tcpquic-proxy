# QUIC Client Eager Connect and Reconnect Design

## Goal

Change the client so a local SOCKS5 / HTTP CONNECT listener is open only when
the corresponding peer can actually provide proxy service. A peer is considered
able to provide service when at least one QUIC connection to that peer is
currently connected.

When a QUIC connection drops, every TCP tunnel carried by that QUIC connection
must be actively closed on both sides. This forces the proxied application to
open a new TCP connection and lets recovery happen at the application layer.

## Current Behavior

The client supports one or more QUIC connections per peer:

- Single peer mode uses `--quic-peer`.
- Multi-peer mode uses `--client-config`.
- `--quic-connections <n>` controls the default connection count for a peer.
- A peer in client config can override it with `"quic_connections": n`.
- The default is `1`; the maximum is `128`.

Today the client can expose local TCP listeners before the QUIC peer is
connected. A tunnel request later calls `EnsureConnected()` and then picks a
connected QUIC connection.

## Required Behavior

### Startup

For every enabled peer:

1. Create the QUIC client session from the peer's known connection parameters.
2. Immediately attempt to connect the peer's QUIC connection pool.
3. Open the peer's SOCKS5 / HTTP CONNECT listeners only after at least one QUIC
   connection is connected.
4. If every initial connection attempt fails, keep the peer in a non-serving
   state and do not bind the local TCP listeners.

The listener gate is per peer. One peer being down must not prevent another peer
with an active QUIC connection from serving.

### Connection Pool Semantics

`quic_connections = N` means the client should try to maintain N QUIC
connections to the same peer.

The peer can serve traffic when `connected_slots > 0`. It does not require all N
connections to be connected. New tunnels continue to use the existing
round-robin selection across connected slots.

### QUIC Disconnect

When a specific QUIC connection disconnects:

1. Mark that connection slot disconnected.
2. Abort all tunnels registered under that exact QUIC connection.
3. Close each tunnel's local TCP socket:
   - On the client side, close the application-facing TCP socket accepted by the
     SOCKS5 / HTTP CONNECT listener.
   - On the server side, close the TCP socket returned by `tcp_dialer`.
4. If a server-side tunnel is still resolving or dialing, mark it aborted. If
   the dial later returns a valid TCP socket, close it immediately and do not
   send `OPEN_OK`.
5. If the peer has no connected QUIC slots left, close that peer's SOCKS5 /
   HTTP CONNECT listeners.

This behavior intentionally breaks the proxied TCP connections quickly instead
of waiting for TCP or application timeouts.

### Reconnect

The client must continuously reconnect disconnected QUIC slots for every enabled
peer.

Add a reconnect interval setting:

- CLI: `--quic-reconnect-interval-ms <n>`
- Client config JSON: `"quic_reconnect_interval_ms": n`
- Default: `3000`
- Minimum: `1000`
- Maximum: `60000`

The JSON value is per peer. If a peer omits it, the peer inherits the CLI value.
If the CLI value is omitted, it uses the default of `3000`.

Values outside `1000..60000` are configuration errors. `0` does not disable
reconnect; enabled peers always keep retrying until disabled or process exit.

When a reconnect succeeds and the peer previously had zero connected slots, the
peer's listeners are reopened.

### Listener Lifecycle

Each peer runtime owns its listener lifecycle:

- `connected_slots > 0`: listeners should be open.
- `connected_slots == 0`: listeners should be closed.
- Peer disabled or removed: close listeners, stop reconnecting, stop QUIC, and
  abort all active tunnels for that peer.

Closing a listener stops accepting new local TCP connections. Existing tunnels
are closed by the QUIC disconnect path for their owning connection, or by peer
disable/removal cleanup.

## Admin and Metrics

Admin operations use the same serving gate:

- `PUT /config` adding or enabling a peer starts QUIC first and opens listeners
  only after at least one connection succeeds.
- `POST /peers/<id>/disable` closes listeners, stops reconnecting, stops the
  QUIC session, and closes active tunnels for that peer.
- A peer that cannot connect remains visible in metrics with a non-serving
  state and no bound TCP listeners.

Metrics should distinguish configured connection count from current connected
count. Keep the existing `connection_count` as the configured target count and
add `connected_connections` for live connected slots.

Suggested peer states:

- `healthy`: at least one connected QUIC slot and listeners open.
- `connecting`: reconnect is running but no QUIC slot is connected yet.
- `down`: the last start or reconnect attempt failed and no listener is open.
- `disabled`: peer is configured but disabled.
- `draining`: peer was removed or replaced and cleanup is in progress.

## Implementation Outline

1. Extend `TqConfig` and `TqPeerConfig` with reconnect interval settings.
2. Parse and validate `--quic-reconnect-interval-ms` and
   `"quic_reconnect_interval_ms"` with the `1000..60000` range.
3. Extend `QuicClientSession` with:
   - live connected slot count,
   - per-slot reconnect attempts,
   - a background reconnect loop,
   - connection-state callbacks for peer runtime listener gating.
4. Add tunnel registration by `MsQuicConnection`.
5. Add a batch abort API for all tunnels attached to a QUIC connection.
6. Call the batch abort API from client and server QUIC connection shutdown
   events.
7. Move peer listener startup behind the connected-slot gate and add listener
   reopen/close handling as connection state changes.
8. Update admin metrics JSON.

## Test Plan

Add focused tests for:

- CLI reconnect interval default, valid range, and invalid values.
- JSON reconnect interval parsing and inheritance from CLI.
- A peer does not open listeners if no initial QUIC connection succeeds.
- A peer opens listeners when at least one connection in the pool succeeds.
- A peer closes listeners when its last connected QUIC slot disconnects.
- A peer reopens listeners after background reconnect restores a QUIC slot.
- Disconnecting one QUIC connection aborts only tunnels registered on that
  connection.
- Server-side in-flight dial closes the TCP fd and suppresses `OPEN_OK` if its
  QUIC connection disconnects before dial completion.

Integration verification should include a multi-peer config where one peer is
down and one peer is healthy, confirming only the healthy peer binds its local
SOCKS5 / HTTP CONNECT ports.
