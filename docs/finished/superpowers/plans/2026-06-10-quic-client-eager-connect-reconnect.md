# QUIC Client Eager Connect and Reconnect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make client-side SOCKS5 / HTTP CONNECT listeners available only while their peer has at least one live QUIC connection, and actively close TCP tunnels when their owning QUIC connection drops.

**Architecture:** Add explicit peer serving state around `QuicClientSession`: QUIC slots reconnect in the background, peer runtimes open listeners when `connected_slots > 0`, and close listeners when it reaches zero. Add a small tunnel registry keyed by `MsQuicConnection*` so connection shutdown callbacks can abort only the tunnels carried by that QUIC connection.

**Tech Stack:** C++17, MsQuic C++ wrappers, existing CMake targets, existing hand-written unit tests.

---

## File Structure

- Modify `src/config/config.h`: add global and per-peer reconnect interval fields.
- Modify `src/config/config.cpp`: parse, validate, inherit, and print `--quic-reconnect-interval-ms` / `"quic_reconnect_interval_ms"`.
- Modify `src/unittest/config_router_test.cpp`: cover defaults, CLI bounds, JSON parsing, and inheritance.
- Create `src/tunnel/tunnel_registry.h`: small connection-to-tunnel registration API.
- Create `src/tunnel/tunnel_registry.cpp`: thread-safe registry implementation.
- Modify `src/tunnel/tcp_tunnel.cpp`: register/unregister tunnel contexts and implement connection-shutdown abort behavior.
- Modify `src/tunnel/tcp_tunnel.h`: expose batch abort API if keeping it near tunnel layer; otherwise include `tunnel_registry.h` users directly.
- Modify `src/protocol/quic_session.h`: add connected-slot count, state callback, eager connect, reconnect loop controls.
- Modify `src/protocol/quic_session.cpp`: maintain connected count, start/stop reconnect thread, notify state callback, abort tunnels on connection shutdown.
- Modify `src/runtime/router_runtime.h`: extend `TqPeerMetrics` with `ConnectedConnections` and adapter status hooks if needed.
- Modify `src/runtime/router_runtime.cpp`: report connected count and serving state accurately.
- Modify `src/main.cpp`: gate single-peer and multi-peer listeners behind connected state; reopen/close listeners as peer QUIC state changes.
- Modify `src/CMakeLists.txt`: add `tunnel/tunnel_registry.cpp` to production and relevant tests.
- Modify `src/unittest/tcp_tunnel_test.cpp`: cover registry and disconnect abort behavior.
- Modify `src/unittest/router_runtime_test.cpp`: cover metrics field and adapter state behavior.
- Optionally create `src/unittest/client_listener_lifecycle_test.cpp` if peer listener lifecycle becomes too large to test in `router_runtime_test.cpp`.

## Task 1: Reconnect Interval Configuration

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing config tests**

Append these cases to `src/unittest/config_router_test.cpp` before `return 0;`:

```cpp
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 57;
        if (cfg.QuicReconnectIntervalMs != 3000) return 58;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "1000", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 59;
        if (cfg.QuicReconnectIntervalMs != 1000) return 60;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "60000", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 61;
        if (cfg.QuicReconnectIntervalMs != 60000) return 62;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "999", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 63;
        if (err.find("--quic-reconnect-interval-ms") == std::string::npos) return 64;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "60001", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 65;
        if (err.find("--quic-reconnect-interval-ms") == std::string::npos) return 66;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"inherit","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"},{"peer_id":"override","quic_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002","quic_reconnect_interval_ms":5000}]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--quic-reconnect-interval-ms", "4000", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 67;
        if (cfg.Router.Peers.size() != 2) return 68;
        if (cfg.Router.Peers[0].QuicReconnectIntervalMs != 4000) return 69;
        if (cfg.Router.Peers[1].QuicReconnectIntervalMs != 5000) return 70;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"bad","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","quic_reconnect_interval_ms":0}]})json", router, err)) return 71;
        if (err.find("quic_reconnect_interval_ms") == std::string::npos) return 72;
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_config_router_test -j2
rtk ./build-gcc10-fixed/bin/Release/tcpquic_config_router_test
```

Expected: build fails because `QuicReconnectIntervalMs` does not exist, or the executable returns a non-zero test code for missing parsing.

- [ ] **Step 3: Add config fields**

In `src/config/config.h`, add:

```cpp
struct TqPeerConfig {
    std::string PeerId;
    std::string QuicPeer;
    std::string SocksListen;
    std::string HttpListen;
    uint32_t QuicConnections{0};
    uint32_t QuicReconnectIntervalMs{0};
    std::string Compress;
    bool Enabled{true};
};
```

In `TqConfig`, add near `QuicConnections`:

```cpp
    uint32_t QuicReconnectIntervalMs{3000};
```

- [ ] **Step 4: Parse and validate CLI / JSON**

In `src/config/config.cpp`, parse peer JSON key:

```cpp
            } else if (key == "quic_reconnect_interval_ms") {
                if (!ParseUint32(peer.QuicReconnectIntervalMs)) return Error("invalid quic_reconnect_interval_ms");
```

In `TqPrintUsage()`, add:

```cpp
        "  --quic-reconnect-interval-ms <n> Client reconnect interval in ms (1000..60000, default 3000)\n"
```

In `TqParseArgs()`, add a branch after `--quic-connections`:

```cpp
        } else if (GetOptionValue(arg, "--quic-reconnect-interval-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-reconnect-interval-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.QuicReconnectIntervalMs) ||
                cfg.QuicReconnectIntervalMs < 1000 ||
                cfg.QuicReconnectIntervalMs > 60000) {
                err = "invalid value for --quic-reconnect-interval-ms (must be 1000..60000)";
                return false;
            }
```

In `TqValidateRouterConfig()`, add:

```cpp
        if (peer.QuicReconnectIntervalMs != 0 &&
            (peer.QuicReconnectIntervalMs < 1000 || peer.QuicReconnectIntervalMs > 60000)) {
            err = "quic_reconnect_interval_ms out of range for " + peer.PeerId;
            return false;
        }
```

After loading `cfg.Router`, inherit the global value:

```cpp
    if (!cfg.ClientConfigPath.empty()) {
        if (!TqLoadClientConfig(cfg.ClientConfigPath, cfg.Router, err)) {
            return false;
        }
        for (auto& peer : cfg.Router.Peers) {
            if (peer.QuicReconnectIntervalMs == 0) {
                peer.QuicReconnectIntervalMs = cfg.QuicReconnectIntervalMs;
            }
        }
    }
```

- [ ] **Step 5: Run test to verify it passes**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_config_router_test -j2
rtk ./build-gcc10-fixed/bin/Release/tcpquic_config_router_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/config/config.h src/config/config.cpp src/unittest/config_router_test.cpp
rtk git commit -m "feat: configure quic reconnect interval"
```

## Task 2: Tunnel Registry for Per-Connection Abort

**Files:**
- Create: `src/tunnel/tunnel_registry.h`
- Create: `src/tunnel/tunnel_registry.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write failing registry tests**

Append to `src/unittest/tcp_tunnel_test.cpp`:

```cpp
static unsigned g_abort_a = 0;
static unsigned g_abort_b = 0;

static void CountAbortA(void*) { ++g_abort_a; }
static void CountAbortB(void*) { ++g_abort_b; }

static int TestTunnelRegistryAbortsOnlyMatchingConnection() {
    auto* conn1 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1001));
    auto* conn2 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1002));
    int ctx1 = 1;
    int ctx2 = 2;
    g_abort_a = 0;
    g_abort_b = 0;

    TqRegisterConnectionTunnel(conn1, &ctx1, CountAbortA);
    TqRegisterConnectionTunnel(conn2, &ctx2, CountAbortB);
    const uint32_t aborted = TqAbortConnectionTunnels(conn1);
    TqUnregisterConnectionTunnel(conn2, &ctx2);

    if (aborted != 1) return 201;
    if (g_abort_a != 1) return 202;
    if (g_abort_b != 0) return 203;
    return 0;
}
```

Call it from `main()`:

```cpp
    if (int rc = TestTunnelRegistryAbortsOnlyMatchingConnection()) return rc;
```

Include the new header:

```cpp
#include "tunnel_registry.h"
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test -j2
```

Expected: build fails because `tunnel_registry.h` and registry functions do not exist.

- [ ] **Step 3: Create registry API**

Create `src/tunnel/tunnel_registry.h`:

```cpp
#pragma once

#include <cstdint>

struct MsQuicConnection;

using TqTunnelAbortFn = void (*)(void* context);

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn);

void TqUnregisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext);

uint32_t TqAbortConnectionTunnels(MsQuicConnection* connection);
```

Create `src/tunnel/tunnel_registry.cpp`:

```cpp
#include "tunnel_registry.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct TqRegisteredTunnel {
    void* Context{nullptr};
    TqTunnelAbortFn Abort{nullptr};
};

std::mutex g_tunnelRegistryLock;
std::unordered_map<MsQuicConnection*, std::vector<TqRegisteredTunnel>> g_tunnelsByConnection;

} // namespace

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn) {
    if (connection == nullptr || tunnelContext == nullptr || abortFn == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    g_tunnelsByConnection[connection].push_back({tunnelContext, abortFn});
}

void TqUnregisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext) {
    if (connection == nullptr || tunnelContext == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    auto it = g_tunnelsByConnection.find(connection);
    if (it == g_tunnelsByConnection.end()) {
        return;
    }
    auto& tunnels = it->second;
    for (auto tunnelIt = tunnels.begin(); tunnelIt != tunnels.end(); ++tunnelIt) {
        if (tunnelIt->Context == tunnelContext) {
            tunnels.erase(tunnelIt);
            break;
        }
    }
    if (tunnels.empty()) {
        g_tunnelsByConnection.erase(it);
    }
}

uint32_t TqAbortConnectionTunnels(MsQuicConnection* connection) {
    if (connection == nullptr) {
        return 0;
    }

    std::vector<TqRegisteredTunnel> tunnels;
    {
        std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
        auto it = g_tunnelsByConnection.find(connection);
        if (it == g_tunnelsByConnection.end()) {
            return 0;
        }
        tunnels = std::move(it->second);
        g_tunnelsByConnection.erase(it);
    }

    for (const auto& tunnel : tunnels) {
        tunnel.Abort(tunnel.Context);
    }
    return static_cast<uint32_t>(tunnels.size());
}
```

- [ ] **Step 4: Wire CMake**

In `src/CMakeLists.txt`, add `tunnel/tunnel_registry.cpp` to `TCPQUIC_PROXY_SOURCES` and `TCPQUIC_TUNNEL_TEST_SOURCES`.

- [ ] **Step 5: Run test to verify it passes**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test -j2
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/tunnel_registry.h src/tunnel/tunnel_registry.cpp src/tunnel/tcp_tunnel.cpp src/unittest/tcp_tunnel_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add quic connection tunnel registry"
```

## Task 3: Abort Tunnel Contexts on QUIC Connection Shutdown

**Files:**
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`

- [ ] **Step 1: Write failing tunnel abort test**

Add a test helper in `src/unittest/tcp_tunnel_test.cpp` that creates a socket pair, registers one side as a tunnel-owned TCP fd through a test-only helper, aborts by connection, and verifies the peer sees close.

Add this test-only declaration near the top:

```cpp
TqTunnelContext* TqCreateTestRegisteredTunnel(
    MsQuicConnection* connection,
    TqSocketHandle tcpFd);
void TqDestroyTestRegisteredTunnel(TqTunnelContext* context);
```

Add the test:

```cpp
static int TestConnectionAbortClosesTunnelTcp() {
    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(fds)) return 210;
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x2001));
    TqTunnelContext* ctx = TqCreateTestRegisteredTunnel(conn, fds[0]);
    if (ctx == nullptr) return 211;

    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    if (aborted != 1) return 212;

    char byte = 0;
    const int received = TqRecv(fds[1], &byte, 1, TqRecvFlags::None);
    TqCloseSocket(fds[1]);
    if (received != 0) return 213;
    return 0;
}
```

Call it from `main()`:

```cpp
    if (int rc = TestConnectionAbortClosesTunnelTcp()) return rc;
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test -j2
```

Expected: build fails because `TqCreateTestRegisteredTunnel` does not exist.

- [ ] **Step 3: Register real tunnel contexts**

In `src/tunnel/tcp_tunnel.cpp`, include:

```cpp
#include "tunnel_registry.h"
```

Add state fields to `TqTunnelContext`:

```cpp
    bool RegisteredWithConnection{false};
    bool AbortedByConnection{false};
```

Add methods:

```cpp
    void RegisterWithConnectionIfNeeded() {
        if (QuicConn != nullptr && !RegisteredWithConnection) {
            TqRegisterConnectionTunnel(QuicConn, this, &TqTunnelContext::AbortFromRegistry);
            RegisteredWithConnection = true;
        }
    }

    void UnregisterFromConnection() {
        if (QuicConn != nullptr && RegisteredWithConnection) {
            TqUnregisterConnectionTunnel(QuicConn, this);
            RegisteredWithConnection = false;
        }
    }

    static void AbortFromRegistry(void* context) {
        static_cast<TqTunnelContext*>(context)->AbortForConnectionShutdown();
    }

    void AbortForConnectionShutdown() {
        {
            std::lock_guard<std::mutex> guard(Lock);
            AbortedByConnection = true;
            SelfDeleteOnShutdown = true;
            if (Stream != nullptr && !ShutdownComplete && !StreamShutdownQueued) {
                StreamShutdownQueued = true;
                (void)Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
            }
        }
        TqRelayStop(&RelayHandle);
        CloseTcp();
        StateChanged.notify_all();
    }
```

Call `RegisterWithConnectionIfNeeded()` after `context->SetStream(stream)` in both `TqStartClientTunnel()` and `TqHandleServerPeerStream()`. For client tunnel construction, pass `conn` into the `TqTunnelContext` constructor instead of `nullptr`:

```cpp
        nullptr,
        conn);
```

In `~TqTunnelContext()`, call `UnregisterFromConnection()` before `OnComplete`.

In `FinishServerOpenAfterDial()`, after assigning `TcpFd = dial.Fd`, add:

```cpp
        bool closeAfterAbort = false;
        {
            std::lock_guard<std::mutex> guard(Lock);
            closeAfterAbort = AbortedByConnection;
        }
        if (closeAfterAbort) {
            CloseTcp();
            return;
        }
```

Keep `CloseTcp()` outside `Lock`; `CloseTcp()` mutates `TcpFd` and should not be called from code that already holds `Lock` unless the implementation is changed to make that explicitly safe.

- [ ] **Step 4: Add test-only helpers**

At the bottom of `src/tunnel/tcp_tunnel.cpp`, add helpers compiled for tests and production-safe because they are unused by production:

```cpp
TqTunnelContext* TqCreateTestRegisteredTunnel(
    MsQuicConnection* connection,
    TqSocketHandle tcpFd) {
    TqConfig cfg;
    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ClientOpen,
        nullptr,
        tcpFd,
        cfg,
        nullptr,
        connection);
    if (context != nullptr) {
        context->RegisterWithConnectionIfNeeded();
    }
    return context;
}

void TqDestroyTestRegisteredTunnel(TqTunnelContext* context) {
    delete context;
}
```

- [ ] **Step 5: Abort tunnels from QUIC callbacks**

In `src/protocol/quic_session.cpp`, include:

```cpp
#include "tunnel_registry.h"
```

In both `QuicClientSession::ConnectionCallback()` and `QuicServerSession::ConnectionCallback()`, call:

```cpp
        (void)TqAbortConnectionTunnels(connection);
```

on `QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT`, `QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER`, and `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE`. It is safe if multiple shutdown events arrive because the registry removes entries before invoking callbacks.

- [ ] **Step 6: Run tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test -j2
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
```

Expected: test exits 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/tunnel/tcp_tunnel.cpp src/protocol/quic_session.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "feat: abort tcp tunnels on quic disconnect"
```

## Task 4: Client Session Connected Count and Reconnect Loop

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`

- [ ] **Step 1: Add a small state callback test seam**

Because `QuicClientSession` needs MsQuic to produce real connection events, keep unit coverage at the method/state level and verify real behavior with integration tests later. Add this API to `src/protocol/quic_session.h`:

```cpp
    using ConnectionStateHandler = std::function<void(uint32_t connectedCount)>;
    void SetConnectionStateHandler(ConnectionStateHandler h);
    uint32_t ConnectedConnectionCount() const;
    bool EnsureAnyConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));
```

Add private fields in `ClientSharedState`:

```cpp
        ConnectionStateHandler ConnectionStateChanged;
        std::thread ReconnectThread;
        std::chrono::milliseconds ReconnectInterval{std::chrono::milliseconds(3000)};
```

Add private methods:

```cpp
    void StartReconnectLoop();
    void StopReconnectLoop(const std::shared_ptr<ClientSharedState>& state);
    static uint32_t ConnectedCountLocked(const ClientSharedState& state);
    static void NotifyConnectionStateChanged(const std::shared_ptr<ClientSharedState>& state);
```

- [ ] **Step 2: Build to verify header changes fail**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy tcpquic_tunnel_test -j2
```

Expected: build fails until methods are implemented.

- [ ] **Step 3: Implement connected count and callback**

In `src/protocol/quic_session.cpp`, implement:

```cpp
uint32_t QuicClientSession::ConnectedConnectionCount() const {
    std::lock_guard<std::mutex> guard(State->Lock);
    return ConnectedCountLocked(*State);
}

uint32_t QuicClientSession::ConnectedCountLocked(const ClientSharedState& state) {
    uint32_t count = 0;
    for (const auto& slot : state.Slots) {
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            ++count;
        }
    }
    return count;
}

void QuicClientSession::SetConnectionStateHandler(ConnectionStateHandler h) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->ConnectionStateChanged = std::move(h);
}

void QuicClientSession::NotifyConnectionStateChanged(const std::shared_ptr<ClientSharedState>& state) {
    ConnectionStateHandler handler;
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        handler = state->ConnectionStateChanged;
        count = ConnectedCountLocked(*state);
    }
    if (handler) {
        handler(count);
    }
}
```

Call `NotifyConnectionStateChanged(state)` after `OnSlotConnected()` and `OnSlotDisconnected()`.

- [ ] **Step 4: Implement eager ensure and reconnect loop**

Implement:

```cpp
bool QuicClientSession::EnsureAnyConnected(std::chrono::milliseconds timeout) {
    return EnsureConnected(timeout);
}

void QuicClientSession::StartReconnectLoop() {
    std::shared_ptr<ClientSharedState> state = State;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->ReconnectInterval = std::chrono::milliseconds(Config.QuicReconnectIntervalMs);
    }
    state->ReconnectThread = std::thread([this, state]() {
        for (;;) {
            {
                std::unique_lock<std::mutex> guard(state->Lock);
                if (state->Stopping || !state->Started) {
                    return;
                }
                state->StateChanged.wait_for(guard, state->ReconnectInterval, [&state] {
                    return state->Stopping || !state->Started;
                });
                if (state->Stopping || !state->Started) {
                    return;
                }
            }
            (void)EnsureConnected(std::chrono::milliseconds(100));
        }
    });
}

void QuicClientSession::StopReconnectLoop(const std::shared_ptr<ClientSharedState>& state) {
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->Stopping = true;
        state->StateChanged.notify_all();
        worker = std::move(state->ReconnectThread);
    }
    if (worker.joinable()) {
        worker.join();
    }
}
```

In `Start()`, after `State->Started = true`, call `StartReconnectLoop()` outside the state lock. In `Stop()`, call `StopReconnectLoop(state)` before moving API/configuration resources.

If holding the lock while moving `ReconnectThread` conflicts with existing lock order, split the state update and thread move into two guarded blocks. Do not join while holding `state->Lock`.

- [ ] **Step 5: Build focused targets**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy tcpquic_tunnel_test tcpquic_router_runtime_test -j2
```

Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp
rtk git commit -m "feat: reconnect quic client slots"
```

## Task 5: Peer Listener Lifecycle and Metrics

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/runtime/router_runtime.h`
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing metrics test**

In `src/unittest/router_runtime_test.cpp`, extend `FakeAdapter`:

```cpp
    uint32_t ConnectedConnections{0};

    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) override {
        out.PeerId = peerId;
        out.ConnectionCount = 4;
        out.ConnectedConnections = ConnectedConnections;
        out.State = ConnectedConnections > 0 ? "healthy" : "connecting";
        return true;
    }
```

Add a test case:

```cpp
    {
        FakeAdapter adapter;
        adapter.ConnectedConnections = 2;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-live", "127.0.0.1:12001"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 106;
        std::string metrics = adapterRuntime.MetricsJson();
        if (metrics.find("\"connected_connections\":2") == std::string::npos) return 107;
        if (metrics.find("\"state\":\"healthy\"") == std::string::npos) return 108;
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_router_runtime_test -j2
```

Expected: build fails because `ConnectedConnections` does not exist.

- [ ] **Step 3: Add metrics field**

In `src/runtime/router_runtime.h`, add:

```cpp
    uint32_t ConnectedConnections{0};
```

In `AppendPeerMetricsJson()` in `src/runtime/router_runtime.cpp`, add after `connection_count`:

```cpp
    out << ",\"connected_connections\":" << peer.ConnectedConnections;
```

In `SnapshotMetrics()`, when adapter live metrics are available, copy `ConnectedConnections`. When no adapter is present, set it to `peer.Enabled ? peer.ConnectionCount : 0` only in tests without real runtime, or leave `0` if state is not known.

- [ ] **Step 4: Refactor multi-peer runtime listener lifecycle**

In `src/main.cpp`, change `PeerRuntime` so `StartPeer()` starts QUIC first and opens listeners only after `EnsureAnyConnected()`:

```cpp
        if (!runtime->Quic->Start(peerCfg)) {
            err = "failed to start QUIC client for " + peer.PeerId;
            return false;
        }
        if (!runtime->Quic->EnsureAnyConnected()) {
            err = "failed to connect QUIC peer " + peer.PeerId;
            runtime->StopAll();
            return false;
        }
```

Move listener creation into a method:

```cpp
        bool OpenListeners(std::string& err, TunnelStartFn startTunnel) {
            if (Socks || Http) {
                return true;
            }
            Socks = std::make_unique<TqSocks5Server>(Config.SocksListen, startTunnel, Pool.get());
            if (!Socks->Start(err)) {
                Socks.reset();
                return false;
            }
            if (!Config.HttpListen.empty()) {
                Http = std::make_unique<TqHttpConnectServer>(Config.HttpListen, startTunnel, Pool.get());
                if (!Http->Start(err)) {
                    StopAccepting();
                    return false;
                }
            }
            return true;
        }
```

Add a state handler after `runtime->Quic->Start(peerCfg)`:

```cpp
        runtime->Quic->SetConnectionStateHandler([runtimeWeak = std::weak_ptr<PeerRuntime>(runtime), startTunnel](uint32_t connected) {
            if (auto runtime = runtimeWeak.lock()) {
                std::string ignored;
                if (connected > 0) {
                    (void)runtime->OpenListeners(ignored, startTunnel);
                } else {
                    runtime->StopAccepting();
                }
            }
        });
```

Because `startTunnel` captures `runtime.get()`, construct the weak handler after `runtime` is in a `std::shared_ptr`. If the lambda capture order is awkward, store `TunnelStartFn StartTunnel` as a member of `PeerRuntime` and let `OpenListeners()` use it.

- [ ] **Step 5: Gate single-peer listener startup**

In `RunSinglePeerClient()` in `src/main.cpp`, call:

```cpp
    if (!quic.EnsureAnyConnected()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to connect QUIC peer %s\n", cfg.QuicPeer.c_str());
        return 1;
    }
```

before printing/listening on SOCKS5/HTTP.

Single-peer runtime can keep listeners open after later disconnect for this iteration only if the full close/reopen lifecycle is implemented exclusively in `PeerRuntime`. If single-peer remains supported, introduce a small `SinglePeerListeners` helper with `Open()` and `Close()` and use the same `SetConnectionStateHandler()` rule as multi-peer.

- [ ] **Step 6: Run tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy tcpquic_router_runtime_test -j2
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/main.cpp src/runtime/router_runtime.h src/runtime/router_runtime.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: gate client listeners on quic connectivity"
```

## Task 6: Admin Config Application and Peer Disable Cleanup

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing adapter cleanup test**

In `src/unittest/router_runtime_test.cpp`, extend `FakeAdapter`:

```cpp
    std::vector<std::string> AbortAll;

    void AbortPeerTunnels(const std::string& peerId) override {
        AbortAll.push_back(peerId);
    }
```

Add `AbortPeerTunnels` to `TqPeerRuntimeAdapter` in the test expectation later. Add this test:

```cpp
    {
        FakeAdapter adapter;
        TqRouterRuntime runtime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-cleanup", "127.0.0.1:12101"));
        std::string err;
        if (!runtime.ApplyConfig(cfg, err)) return 109;
        cfg.Peers[0].Enabled = false;
        if (!runtime.ApplyConfig(cfg, err)) return 110;
        if (adapter.Stopped.empty() || adapter.Stopped.back() != "agent-cleanup") return 111;
        if (adapter.AbortAll.empty() || adapter.AbortAll.back() != "agent-cleanup") return 112;
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_router_runtime_test -j2
```

Expected: build fails because `AbortPeerTunnels` is not defined.

- [ ] **Step 3: Add adapter cleanup hook**

In `src/runtime/router_runtime.h`, add default hook:

```cpp
    virtual void AbortPeerTunnels(const std::string& peerId) {
        (void)peerId;
    }
```

In `TqRouterRuntime::ApplyConfig()`, when a running peer becomes disabled, changed, or removed, call:

```cpp
                Adapter->AbortPeerTunnels(peer.PeerId);
```

before or after `StopAccepting()`. Keep `DrainPeer()` for delayed resource shutdown.

- [ ] **Step 4: Implement multi-peer cleanup**

In `TqMultiPeerRuntimeAdapter`, implement:

```cpp
    void AbortPeerTunnels(const std::string& peerId) override {
        std::shared_ptr<PeerRuntime> runtime = Find(peerId);
        if (runtime && runtime->Quic) {
            runtime->Quic->AbortAllTunnels();
        }
    }
```

Add `QuicClientSession::AbortAllTunnels()` in Task 4 or here:

```cpp
void QuicClientSession::AbortAllTunnels() {
    std::vector<MsQuicConnection*> connections;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        for (auto& slot : State->Slots) {
            if (slot.Connection) {
                connections.push_back(slot.Connection.get());
            }
        }
    }
    for (MsQuicConnection* connection : connections) {
        (void)TqAbortConnectionTunnels(connection);
    }
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy tcpquic_router_runtime_test -j2
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/main.cpp src/protocol/quic_session.h src/protocol/quic_session.cpp src/runtime/router_runtime.h src/runtime/router_runtime.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: cleanup peer tunnels on disable"
```

## Task 7: Integration Verification Script Update

**Files:**
- Modify: `scripts/test-tcpquic-proxy.sh`
- Modify: `README.md`

- [ ] **Step 1: Add script checks**

In `scripts/test-tcpquic-proxy.sh`, add a section after normal client/server startup that:

```bash
wait_for_closed_port 127.0.0.1 "${DOWN_PEER_SOCKS_PORT}"
wait_for_open_port 127.0.0.1 "${HEALTHY_PEER_SOCKS_PORT}"
```

If the script does not already have port helpers, add:

```bash
wait_for_open_port() {
  local host="$1"
  local port="$2"
  for _ in $(seq 1 50); do
    if (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "port ${host}:${port} did not open" >&2
  return 1
}

wait_for_closed_port() {
  local host="$1"
  local port="$2"
  for _ in $(seq 1 20); do
    if ! (echo >"/dev/tcp/${host}/${port}") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "port ${host}:${port} unexpectedly open" >&2
  return 1
}
```

- [ ] **Step 2: Document behavior**

In `README.md`, update the CLI table with:

```markdown
| `--quic-reconnect-interval-ms` | client | `3000` | QUIC slot reconnect interval, range `1000..60000` |
```

Add a short bullet under client behavior:

```markdown
- Client SOCKS5 / HTTP CONNECT listeners are open only while the peer has at least one connected QUIC connection. If all QUIC connections for a peer drop, that peer's local listeners close and reopen after reconnect.
```

- [ ] **Step 3: Run script or focused manual verification**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy -j2
rtk ./scripts/test-tcpquic-proxy.sh
```

Expected: script passes. If local certificates or ports prevent the script from running, record the exact blocker in the final implementation notes and run all unit tests from Task 8.

- [ ] **Step 4: Commit**

```bash
rtk git add scripts/test-tcpquic-proxy.sh README.md
rtk git commit -m "docs: describe quic listener gating"
```

## Task 8: Final Verification

**Files:**
- No source changes unless verification finds a bug.

- [ ] **Step 1: Build focused targets**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy tcpquic_config_router_test tcpquic_tunnel_test tcpquic_router_runtime_test tcpquic_http_connect_test tcpquic_socks5_test -j2
```

Expected: build succeeds.

- [ ] **Step 2: Run focused tests**

Run:

```bash
rtk ./build-gcc10-fixed/bin/Release/tcpquic_config_router_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_http_connect_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_socks5_test
```

Expected: every command exits 0.

- [ ] **Step 3: Run broader regression set**

Run:

```bash
rtk bash -lc 'set -e; for t in tcpquic_acl_test tcpquic_admin_http_test tcpquic_compress_test tcpquic_config_router_test tcpquic_control_test tcpquic_http_connect_test tcpquic_linux_relay_buffer_pool_test tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_relay_backend_selection_test tcpquic_router_runtime_test tcpquic_socks5_test tcpquic_tcp_write_queue_test tcpquic_thread_pool_test tcpquic_tuning_test tcpquic_tunnel_reaper_test tcpquic_tunnel_test tcpquic_platform_socket_test tcpquic_production_linkage_guard_test; do if [ -x "./build-gcc10-fixed/bin/Release/$t" ]; then echo RUN $t; "./build-gcc10-fixed/bin/Release/$t"; fi; done'
```

Expected: every available test exits 0.

- [ ] **Step 4: Check git status**

Run:

```bash
rtk git status --short
```

Expected: clean working tree.

- [ ] **Step 5: Push branch**

Run:

```bash
rtk git push
```

Expected: push succeeds.
