# Windows IOCP Platform Design

Status: approved design
Date: 2026-06-09
Scope: native Windows 10/11 x64 support for `tcpquic-proxy`

## Context

`tcpquic-proxy` is currently a Linux/POSIX C++17 application. The protocol, config, compression, routing, and QUIC code are mostly portable, but the TCP side depends on POSIX socket APIs and the production relay path depends on Linux `epoll`, `eventfd`, `readv`, and `writev`.

An earlier Windows support draft exists at `docs/specs/2026-06-09-windows-platform-support-design.md`. This design supersedes its conclusion that Windows v1 should avoid IOCP. The selected direction is native Windows 10/11 x64 with Visual Studio 2022/MSVC, CMake/Ninja, PEM mTLS files, and a production Windows IOCP relay backend.

## Goals

- Build `tcpquic-proxy.exe` natively on Windows 10/11 x64 with MSVC 2022 and CMake/Ninja.
- Keep the existing CLI and PEM certificate parameters: `--quic-cert`, `--quic-key`, and `--quic-ca`.
- Support both `client` and `server` modes on Windows.
- Preserve SOCKS5, HTTP CONNECT, ACL, compression, router runtime, admin HTTP, and QUIC semantics.
- Keep Linux behavior and the existing Linux production relay path intact.
- Add a Windows production relay backend based on Winsock overlapped I/O and IOCP.
- Verify Windows with real MSVC builds, unit tests, loopback end-to-end tests, and Windows/Linux interoperability tests.

## Non-Goals

- Windows service / SCM integration.
- Windows certificate store or SChannel certificate enrollment UX.
- MinGW, clang-cl, ARM64 Windows, or 32-bit Windows support in the first implementation.
- A generic networking framework. The socket abstraction should cover this project only.
- Replacing the Linux `TqLinuxRelayWorker` with a cross-platform event loop.

## Selected Approach

Use a narrow cross-platform socket layer plus OS-specific relay backends.

The platform socket layer absorbs differences in socket handle types, close calls, nonblocking mode, address conversion, DNS, socket options, shutdown constants, and error mapping. Shared business logic uses this layer instead of direct POSIX or Winsock calls.

The relay backend remains platform-specific. Linux continues to use `TqLinuxRelayWorker`. Windows adds `TqWindowsRelayRuntime` and `TqWindowsRelayWorker`, implemented with IOCP and Winsock overlapped I/O.

This keeps the abstraction boundary small. Socket API differences are centralized, while fundamentally different eventing models stay in separate backend implementations.

## Architecture

### Platform Socket Layer

Add:

- `src/platform_socket.h`
- `src/platform_socket_posix.cpp`
- `src/platform_socket_win.cpp`

The public API should include:

- `TqSocketHandle`
- `TqInvalidSocket`
- `TqSocketStartup`
- `TqSocketValid`
- `TqCloseSocket`
- `TqSetNonBlocking`
- `TqLastSocketError`
- `TqSocketWouldBlock`
- `TqSend`
- `TqRecv`
- `TqShutdownSend`
- `TqInetPton`
- `TqInetNtop`
- `TqSetReuseAddr`
- `TqSetNoDelay`
- receive/send buffer tuning helpers

On POSIX, `TqSocketHandle` is `int` and invalid is `-1`. On Windows, it is `SOCKET` and invalid is `INVALID_SOCKET`. Production interfaces that currently pass `int fd` for sockets should migrate to `TqSocketHandle` to avoid truncation on Win64.

`TqSocketStartup` is a no-op on POSIX and an RAII wrapper around `WSAStartup(2, 2)` / `WSACleanup()` on Windows. `main.cpp` should initialize it before project code uses Winsock. This is required even though msquic may also initialize Winsock internally.

The platform layer is intentionally narrow. It should not wrap `std::thread`, mutexes, timers, or other C++ standard library APIs that are already portable.

### Shared Protocol Code

The following modules should remain shared and behaviorally unchanged except for replacing direct socket calls with the platform layer:

- `config`
- `control_protocol`
- `acl`
- `compress`
- `quic_session`
- `tcp_dialer`
- `tcp_tunnel`
- `socks5_server`
- `http_connect_server`
- `admin_http`
- `router_runtime`
- `server_metrics`
- `thread_pool`
- `tunnel_reaper`
- `warmup`

Linux-only relay files stay Linux-only:

- `linux_relay_buffer_pool.*`
- `linux_relay_worker.*`

Windows adds:

- `windows_relay_worker.*`

### Relay Backend Selection

`relay.cpp` should select the backend at compile time:

- Linux: `TqLinuxRelayRuntime`
- Windows: `TqWindowsRelayRuntime`

The external relay contract remains equivalent: register a TCP socket with a QUIC stream, receive MsQuic stream callbacks, relay data in both directions, honor FIN/abort, and release resources safely.

## Windows IOCP Relay Design

### Worker Model

Windows starts a fixed number of relay workers. Each worker owns one IOCP handle and one worker thread. A relay is assigned to an owner worker by socket value or an incrementing relay id hash.

The owner worker owns:

- TCP socket
- QUIC stream pointer
- compressor/decompressor instances
- TCP read buffers
- TCP write queue
- in-flight QUIC send accounting
- overlapped operation state
- close/drain state
- relay reference count

### TCP to QUIC

When a relay is registered, the owner worker associates the TCP socket with its IOCP and posts one or more `WSARecv` operations.

When an IOCP receive completion arrives:

1. If byte count is zero, treat it as TCP EOF and send QUIC FIN.
2. Otherwise optionally compress the bytes on the worker thread.
3. Send the resulting buffers with `Stream->Send()`.
4. Account for in-flight QUIC sends.
5. If the in-flight limit is reached, stop posting more `WSARecv` until `QUIC_STREAM_EVENT_SEND_COMPLETE` events return capacity.

This preserves the Linux production rule that TCP reads and compression happen on relay workers, not on MsQuic callback threads.

### QUIC to TCP

MsQuic `QUIC_STREAM_EVENT_RECEIVE` callbacks can run on QUIC worker threads. The Windows relay must not do socket I/O or decompression in that callback.

The callback should:

1. Acquire a relay reference.
2. Copy the receive buffers into relay-owned memory.
3. Post an internal event to the owner IOCP with `PostQueuedCompletionStatus`.
4. Release the callback reference.

The owner worker processes the internal event, optionally decompresses the payload, and writes it to TCP using overlapped `WSASend`.

If QUIC receive carries FIN, the owner worker flushes queued TCP writes and then calls `shutdown(socket, SD_SEND)`.

### Internal Events

The IOCP loop handles both Winsock completions and internal relay events. Internal events are posted with `PostQueuedCompletionStatus`.

Internal event types:

- `QuicReceiveQueued`
- `QuicSendComplete`
- `CloseRelay`
- `StopWorker`

This avoids an additional wake fd or Windows event object and keeps relay state transitions serialized through the owner worker.

### Close and Lifetime

Windows close is asynchronous in practice: `CancelIoEx` or `closesocket` does not guarantee that all overlapped completions have already been delivered. The relay context must not be deleted synchronously when close starts.

Each relay context uses references for:

- owner registration table entry
- pending overlapped reads
- pending overlapped writes
- queued internal IOCP events
- temporary callback access

Closing marks the relay as draining/cancelled, cancels pending operations where possible, shuts down the QUIC stream or socket as needed, removes the registration reference, and releases memory only when the reference count reaches zero after all completions are observed.

### Error and Half-Close Rules

- TCP receive completion with zero bytes sends QUIC FIN.
- QUIC receive FIN flushes pending TCP writes, then calls `shutdown(SD_SEND)`.
- TCP fatal errors abort the QUIC stream.
- QUIC abort cancels pending TCP operations and closes the TCP socket.
- Repeated or late completions after close are ignored after releasing their operation reference.

### Compression

Compression state remains per relay.

- TCP to QUIC compression happens on the owner IOCP worker.
- QUIC to TCP decompression happens on the owner IOCP worker.
- MsQuic callbacks only copy and enqueue data.

This keeps CPU-heavy work off QUIC callback threads and matches the Linux worker model.

## Socket Ownership Change

The current Linux listener flow uses `dup()` to keep one fd for the handshake reply and one fd for tunnel ownership. Windows does not have a compatible socket `dup()` model for this use case.

The shared flow should change to:

1. Finish SOCKS5 or HTTP CONNECT validation.
2. Send the success response on the client socket.
3. Call `onTunnel(..., socket)` and transfer socket ownership to the tunnel.
4. Do not close the socket in the listener after successful transfer.

Failure paths still send the error response when possible and close the socket immediately.

This is valid because the tunnel start path sends the QUIC OPEN control frame and does not read application TCP data until OPEN_OK. It also reduces lifetime ambiguity on Linux.

## CMake Design

Keep CMake as the only build system.

Recommended Windows command:

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target tcpquic-proxy
```

Windows target changes:

- Link `ws2_32`.
- Compile `platform_socket_win.cpp`.
- Compile `windows_relay_worker.cpp`.
- Define `NOMINMAX` and `WIN32_LEAN_AND_MEAN`.
- Exclude Linux relay sources and Linux-only tests.

Linux target changes:

- Compile `platform_socket_posix.cpp`.
- Keep `linux_relay_buffer_pool.cpp` and `linux_relay_worker.cpp`.
- Keep the current Linux relay tests.

The vendored dependency strategy stays unchanged. msquic, lz4, and zstd continue to build through submodules. Windows uses the msquic Windows build path and PEM credential parameters remain part of the application contract.

## Test Plan

### Windows Unit Tests

Windows should build and run:

- `tcpquic_acl_test`
- `tcpquic_control_test`
- `tcpquic_compress_test`
- `tcpquic_config_router_test`
- `tcpquic_thread_pool_test`
- `tcpquic_tunnel_reaper_test`
- `tcpquic_http_connect_test`
- `tcpquic_socks5_test`
- `tcpquic_admin_http_test`
- `tcpquic_windows_relay_worker_test`

Tests that depend on `socketpair`, `pipe`, `fcntl`, `poll`, `MSG_NOSIGNAL`, or Linux relay files need platform helpers or platform-specific exclusion.

### Windows End-to-End Tests

Run on a real Windows 10/11 x64 machine or CI runner:

1. Generate CA, server, and client PEM files.
2. Start a Windows `tcpquic-proxy server` on loopback.
3. Start a Windows `tcpquic-proxy client` with SOCKS5 and HTTP CONNECT listeners.
4. Start a local HTTP echo server or equivalent TCP test server.
5. Verify HTTP CONNECT and SOCKS5 requests through the tunnel.
6. Repeat with `--compress off`, `--compress zstd`, and `--compress lz4`.
7. Verify ACL allow, ACL deny, refused target, and concurrent tunnels.

### Interoperability Tests

Required before calling Windows support complete:

- Windows client to Linux server.
- Linux client to Windows server.

Both directions must verify mTLS, OPEN/OPEN_OK protocol, compression negotiation, payload integrity, and clean shutdown.

### Linux Regression Tests

Linux must continue to run the existing complete test set. This is required after platform socket migration because shared listener, dialer, ACL, and tunnel code will change.

## Migration Plan

1. Update CMake platform conditions so Windows excludes Linux-only relay files and tests, links `ws2_32`, and includes the new platform socket source files.
2. Add `platform_socket.*` and initialize `TqSocketStartup` at program entry.
3. Migrate shared socket users from POSIX APIs to the platform layer and replace socket `int fd` with `TqSocketHandle`.
4. Remove `dup()` from SOCKS5 and HTTP CONNECT handoff by sending the success response before transferring socket ownership.
5. Add Windows IOCP relay runtime and worker with safe overlapped lifetime management.
6. Add Windows relay unit tests and platform-specific test exclusions or helpers.
7. Run Windows unit and loopback end-to-end tests.
8. Run Windows/Linux interoperability tests in both directions.
9. Run Linux full regression tests.
10. Update README with Windows build, runtime, and test instructions.

## Risks and Mitigations

### IOCP Lifetime

Risk: `closesocket` or cancellation can still produce late completions.

Mitigation: use relay context reference counting and release only after all overlapped and queued events complete.

### QUIC Callback Blocking

Risk: doing socket writes or decompression in MsQuic callbacks can block QUIC workers.

Mitigation: callbacks only copy data and post IOCP internal events.

### Linux Regression

Risk: platform socket migration changes existing Linux listener and dialer behavior.

Mitigation: keep Linux relay backend unchanged and require full Linux regression tests.

### Handle Truncation

Risk: Win64 `SOCKET` cannot safely pass through `int`.

Mitigation: migrate socket-carrying APIs to `TqSocketHandle`.

### Document Drift

Risk: older Windows docs describe a non-IOCP first phase.

Mitigation: this spec explicitly supersedes that conclusion; README should point to this design once implementation begins.

## Acceptance Criteria

- `tcpquic-proxy.exe` builds on Windows 10/11 x64 with MSVC 2022 and CMake/Ninja.
- Windows client and server modes start with PEM mTLS parameters.
- Windows IOCP relay handles TCP to QUIC, QUIC to TCP, FIN, abort, compression, and cleanup without blocking MsQuic callbacks.
- Windows loopback HTTP CONNECT and SOCKS5 end-to-end tests pass for compression off, zstd, and lz4.
- Windows client interoperates with Linux server.
- Linux client interoperates with Windows server.
- Existing Linux unit and integration tests continue to pass.
