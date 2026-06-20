# Unified Client Peer Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace separate single-peer and multi-peer client runtime orchestration with one shared peer runtime and one shared client runtime manager.

**Architecture:** Add `TqClientPeerRuntime` to own one peer's QUIC session, ingress gating, tunnel start path, reconnect scheduler binding, and metrics. Add `TqClientRuntimeManager` to own the shared ingress reactor and peer map; single-peer becomes a manager with one fixed `primary` peer, while router multi-peer uses the same manager through a thin adapter.

**Tech Stack:** C++17, existing `QuicClientSession`, `TqClientIngressReactor`, `TqRouterRuntime`, CMake, assert-style unit tests, DGX bench scripts.

---

## File Structure

Create:

- `src/runtime/client_peer_runtime.h`  
  Declares `TqClientPeerRuntime`, `TqClientRuntimeManager`, and inline config conversion helpers.
- `src/runtime/client_peer_runtime.cpp`  
  Implements shared peer lifecycle, ingress gating, StartTunnel, metrics snapshots, and manager lifecycle.
- `src/unittest/client_peer_runtime_test.cpp`  
  Focused tests for config overlay helpers; runtime behavior is covered by router, ingress, tunnel, reconnect, speedtest, and DGX smoke tests.

Modify:

- `src/main.cpp`  
  Remove duplicated `TqSinglePeerClientRuntime` and `TqMultiPeerRuntimeAdapter::PeerRuntime`; use `TqClientRuntimeManager`.
- `src/CMakeLists.txt`  
  Add new runtime source and unit test target.
- `src/unittest/router_runtime_test.cpp`  
  Keep existing adapter tests unchanged; this plan relies on those tests to verify the adapter contract remains compatible.

---

### Task 1: Add Shared Runtime Types and Pure Config Tests

**Files:**
- Create: `src/runtime/client_peer_runtime.h`
- Create: `src/unittest/client_peer_runtime_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing config overlay test**

Create `src/unittest/client_peer_runtime_test.cpp`:

```cpp
#include "client_peer_runtime.h"

#include <string>

static int TestPrimaryPeerConfigUsesCliFields() {
    TqConfig cfg;
    cfg.QuicPeer = "10.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:1080";
    cfg.HttpListen = "127.0.0.1:18080";
    cfg.QuicConnections = 4;
    cfg.Compress = "off";

    const TqPeerConfig peer = TqMakePrimaryPeerConfig(cfg);
    if (peer.PeerId != "primary") return 10;
    if (peer.QuicPeer != cfg.QuicPeer) return 11;
    if (peer.SocksListen != cfg.SocksListen) return 12;
    if (peer.HttpListen != cfg.HttpListen) return 13;
    if (peer.QuicConnections != cfg.QuicConnections) return 14;
    if (peer.Compress != cfg.Compress) return 15;
    if (!peer.Enabled) return 16;
    return 0;
}

static int TestPeerConfigOverlayUsesPeerOverrides() {
    TqConfig base;
    base.QuicConnections = 2;
    base.Compress = "zstd";

    TqPeerConfig peer;
    peer.PeerId = "agent-a";
    peer.QuicPeer = "10.0.0.2:4433";
    peer.SocksListen = "127.0.0.1:11001";
    peer.HttpListen = "127.0.0.1:18081";
    peer.QuicConnections = 8;
    peer.Compress = "off";

    const TqConfig out = TqMakePeerRuntimeConfig(base, peer);
    if (out.QuicPeer != peer.QuicPeer) return 20;
    if (out.SocksListen != peer.SocksListen) return 21;
    if (out.HttpListen != peer.HttpListen) return 22;
    if (out.QuicConnections != peer.QuicConnections) return 23;
    if (out.Compress != peer.Compress) return 24;
    if (!out.ClientConfigPath.empty()) return 25;
    if (!out.AdminListen.empty()) return 26;
    return 0;
}

static int TestPeerConfigOverlayUsesBaseDefaults() {
    TqConfig base;
    base.QuicConnections = 3;
    base.Compress = "zstd";

    TqPeerConfig peer;
    peer.PeerId = "agent-b";
    peer.QuicPeer = "10.0.0.3:4433";
    peer.SocksListen = "127.0.0.1:11002";

    const TqConfig out = TqMakePeerRuntimeConfig(base, peer);
    if (out.QuicConnections != base.QuicConnections) return 30;
    if (out.Compress != base.Compress) return 31;
    return 0;
}

int main() {
    if (int rc = TestPrimaryPeerConfigUsesCliFields()) return rc;
    if (int rc = TestPeerConfigOverlayUsesPeerOverrides()) return rc;
    if (int rc = TestPeerConfigOverlayUsesBaseDefaults()) return rc;
    return 0;
}
```

- [ ] **Step 2: Add the test target and verify it fails**

Modify `src/CMakeLists.txt` to add this target near the other unit test executables:

```cmake
add_executable(tcpquic_client_peer_runtime_test
    unittest/client_peer_runtime_test.cpp
)
tcpquic_target_include_dirs(tcpquic_client_peer_runtime_test)
target_compile_definitions(tcpquic_client_peer_runtime_test PRIVATE TQ_UNIT_TESTING=1)
set_property(TARGET tcpquic_client_peer_runtime_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_peer_runtime_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
rtk cmake --build build-regression --target tcpquic_client_peer_runtime_test -j4
```

Expected: build fails because `client_peer_runtime.h` does not exist.

- [ ] **Step 3: Add public declarations**

Create `src/runtime/client_peer_runtime.h`:

```cpp
#pragma once

#include "config.h"
#include "router_runtime.h"

#include <string>

class TqClientIngressReactor;

inline TqPeerConfig TqMakePrimaryPeerConfig(const TqConfig& cfg) {
    TqPeerConfig peer;
    peer.PeerId = "primary";
    peer.Enabled = true;
    peer.QuicPeer = cfg.QuicPeer;
    peer.SocksListen = cfg.SocksListen;
    peer.HttpListen = cfg.HttpListen;
    peer.QuicConnections = cfg.QuicConnections;
    peer.Compress = cfg.Compress;
    return peer;
}

inline TqConfig TqMakePeerRuntimeConfig(const TqConfig& baseConfig, const TqPeerConfig& peer) {
    TqConfig cfg = baseConfig;
    cfg.ClientConfigPath.clear();
    cfg.AdminListen.clear();
    cfg.QuicPeer = peer.QuicPeer;
    cfg.SocksListen = peer.SocksListen;
    cfg.HttpListen = peer.HttpListen;
    cfg.QuicConnections = peer.QuicConnections == 0 ? baseConfig.QuicConnections : peer.QuicConnections;
    cfg.Compress = peer.Compress.empty() ? baseConfig.Compress : peer.Compress;
    return cfg;
}
```

- [ ] **Step 4: Run the test and commit**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_client_peer_runtime_test -j4
rtk ./build-regression/bin/Release/tcpquic_client_peer_runtime_test
```

Expected: exit 0.

Commit:

```bash
rtk git add src/runtime/client_peer_runtime.h src/unittest/client_peer_runtime_test.cpp src/CMakeLists.txt
rtk git commit -m "refactor(client): add shared peer runtime config helpers"
```

### Task 2: Move Multi-Peer PeerRuntime Into Shared Runtime

**Files:**
- Modify: `src/runtime/client_peer_runtime.h`
- Create: `src/runtime/client_peer_runtime.cpp`
- Modify: `src/main.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add shared runtime class declarations**

Extend `src/runtime/client_peer_runtime.h`:

```cpp
#include "admin_http.h"
#include "client_ingress_reactor.h"
#include "client_tunnel_open.h"
#include "quic_session.h"
#include "server_metrics.h"
#include "tcp_tunnel.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

class TqClientPeerRuntime : public std::enable_shared_from_this<TqClientPeerRuntime> {
public:
    TqClientPeerRuntime(std::string peerId, TqConfig config, TqClientIngressReactor* ingress);
    ~TqClientPeerRuntime();

    bool Start(std::string& err);
    void StopAccepting();
    void StopAll();
    void AbortTunnels();
    TqPeerMetrics SnapshotPeerMetrics() const;
    TqClientMetrics SnapshotClientMetrics() const;
    bool EnableAcceptingAndApplyCurrentConnectionState(std::string& err, bool requireConnected);

private:
    bool OpenListenersLocked(std::string& err);
    bool ApplyConnectionState(uint32_t connectedCount, std::string& err, bool requireConnected);
    bool ApplyCurrentConnectionState(std::string& err, bool requireConnected);
    bool ApplyConnectionStateLocked(uint32_t connectedCount, std::string& err, bool requireConnected);
    bool ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected);
    void CloseListenersLocked();
    void DisableAccepting();
    TqClientTunnelOpenHandle* StartTunnel(
        const TunnelRequest& req,
        TqSocketHandle fd,
        TqClientTunnelOpenComplete onComplete);

    std::string PeerId;
    TqConfig Config;
    mutable std::mutex TunnelStartMutex;
    mutable std::mutex ListenerMutex;
    std::unique_ptr<QuicClientSession> Quic;
    TqClientIngressReactor* Ingress{nullptr};
    bool AcceptingEnabled{false};
    bool ListenersOpen{false};
};
```

- [ ] **Step 2: Add runtime source to tcpquic-proxy**

Modify `src/CMakeLists.txt` and add `runtime/client_peer_runtime.cpp` to `TCPQUIC_PROXY_SOURCES` near the other runtime sources:

```cmake
    runtime/client_peer_runtime.cpp
```

Keep `tcpquic_client_peer_runtime_test` source list unchanged. It intentionally tests only inline config helpers, so it does not link the full runtime dependency graph.

- [ ] **Step 3: Move existing multi-peer logic**

In `src/runtime/client_peer_runtime.cpp`, move behavior from `TqMultiPeerRuntimeAdapter::PeerRuntime` in `src/main.cpp` into `TqClientPeerRuntime` using the mapping below. Do not introduce new listener or reconnect semantics in this task.

Use this mapping:

| Old location | New location |
|--------------|--------------|
| `PeerRuntime::OpenListenersLocked` | `TqClientPeerRuntime::OpenListenersLocked` |
| `PeerRuntime::ApplyCurrentConnectionState` | `TqClientPeerRuntime::ApplyCurrentConnectionState` |
| `PeerRuntime::ApplyConnectionState` | `TqClientPeerRuntime::ApplyConnectionState` |
| `PeerRuntime::CloseListenersLocked` | `TqClientPeerRuntime::CloseListenersLocked` |
| `PeerRuntime::DisableAccepting` | `TqClientPeerRuntime::DisableAccepting` |
| `PeerRuntime::StopAccepting` | `TqClientPeerRuntime::StopAccepting` |
| `PeerRuntime::StopAll` | `TqClientPeerRuntime::StopAll` |
| `StartPeer()` local `StartTunnel` lambda | `TqClientPeerRuntime::StartTunnel` |
| `StartPeer()` scheduler / connection-state wiring | `TqClientPeerRuntime::Start` |

`TqClientPeerRuntime::Start()` must perform this sequence:

```cpp
Quic = std::make_unique<QuicClientSession>();
std::weak_ptr<TqClientPeerRuntime> weakSelf = shared_from_this();
Quic->SetDelayedTaskScheduler([weakSelf](std::chrono::milliseconds delay, std::function<void()> task) {
    auto self = weakSelf.lock();
    if (!self || self->Ingress == nullptr) {
        return false;
    }
    return self->Ingress->EnqueueDelayed(delay, std::move(task));
});
Quic->SetConnectionStateHandler([weakSelf](uint32_t connectedCount) {
    auto self = weakSelf.lock();
    if (!self) {
        return;
    }
    std::string listenerErr;
    if (!self->ApplyConnectionState(connectedCount, listenerErr, false)) {
        if (self->PeerId == "primary") {
            std::fprintf(stderr, "tcpquic-proxy: %s\n", listenerErr.c_str());
        } else {
            std::fprintf(stderr, "tcpquic-proxy: peer %s %s\n", self->PeerId.c_str(), listenerErr.c_str());
        }
    }
});
if (!Quic->Start(Config)) {
    err = "failed to start QUIC client for " + PeerId;
    return false;
}
if (!Quic->EnsureAnyConnected()) {
    if (PeerId == "primary") {
        std::fprintf(stderr, "tcpquic-proxy: no connected QUIC peer at startup; listeners remain closed until reconnect\n");
    } else {
        std::fprintf(stderr,
            "tcpquic-proxy: peer %s has no connected QUIC connection; listeners remain closed until reconnect\n",
            PeerId.c_str());
    }
}
if (!EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
    return false;
}
```

Keep this exact connection-state gating semantic:

```cpp
bool TqClientPeerRuntime::ApplyConnectionStateLocked(
    uint32_t connectedCount,
    std::string& err,
    bool requireConnected) {
    if (!AcceptingEnabled || connectedCount == 0) {
        CloseListenersLocked();
        if (requireConnected) {
            err = AcceptingEnabled ? "no connected QUIC connection" : "listener accepting is disabled";
            return false;
        }
        return true;
    }
    return OpenListenersLocked(err);
}
```

Use `PeerId == "primary"` only for log wording. Single-peer logs should remain:

```cpp
tcpquic-proxy: SOCKS5 listening on <addr>
tcpquic-proxy: HTTP CONNECT listening on <addr>
```

Multi-peer logs should remain:

```cpp
tcpquic-proxy: peer <id> SOCKS5 listening on <addr>
tcpquic-proxy: peer <id> HTTP CONNECT listening on <addr>
```

- [ ] **Step 4: Replace multi-peer nested PeerRuntime**

In `src/main.cpp`, keep the adapter's existing shared ingress reactor and map, but replace nested `PeerRuntime` references with `TqClientPeerRuntime`:

```cpp
std::unordered_map<std::string, std::shared_ptr<TqClientPeerRuntime>> Peers;
```

`StartPeer()` should call:

```cpp
TqConfig peerCfg = TqMakePeerRuntimeConfig(BaseConfig, peer);
auto runtime = std::make_shared<TqClientPeerRuntime>(peer.PeerId, peerCfg, Ingress.get());
if (!runtime->Start(err)) {
    runtime->StopAll();
    return false;
}
```

- [ ] **Step 5: Build and run router regression**

Run:

```bash
rtk cmake --build build-regression --target tcpquic-proxy tcpquic_router_runtime_test -j4
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp src/main.cpp src/CMakeLists.txt
rtk git commit -m "refactor(client): move multi-peer runtime to shared class"
```

### Task 3: Add Client Runtime Manager

**Files:**
- Modify: `src/runtime/client_peer_runtime.h`
- Modify: `src/runtime/client_peer_runtime.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add manager declarations**

Add to `src/runtime/client_peer_runtime.h`:

```cpp
class TqClientRuntimeManager {
public:
    explicit TqClientRuntimeManager(TqConfig baseConfig);
    ~TqClientRuntimeManager();

    bool StartPeer(const TqPeerConfig& peer, std::string& err);
    void StopAccepting(const std::string& peerId);
    void StopAll();
    void AbortPeerTunnels(const std::string& peerId);
    void DrainPeer(const std::string& peerId, uint32_t graceSeconds);
    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) const;
    bool SnapshotClientMetrics(const std::string& peerId, TqClientMetrics& out) const;

private:
    bool EnsureIngressStarted(std::string& err);
    std::shared_ptr<TqClientPeerRuntime> Find(const std::string& peerId) const;

    TqConfig BaseConfig;
    mutable std::mutex Lock;
    mutable std::mutex IngressLock;
    std::unique_ptr<TqClientIngressReactor> Ingress;
    std::unordered_map<std::string, std::shared_ptr<TqClientPeerRuntime>> Peers;
};
```

- [ ] **Step 2: Implement manager lifecycle**

In `src/runtime/client_peer_runtime.cpp`, implement:

```cpp
bool TqClientRuntimeManager::StartPeer(const TqPeerConfig& peer, std::string& err) {
    if (!peer.Enabled) {
        err = "peer is disabled: " + peer.PeerId;
        return false;
    }
    if (!EnsureIngressStarted(err)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(Lock);
        if (Peers.find(peer.PeerId) != Peers.end()) {
            err = "peer already running: " + peer.PeerId;
            return false;
        }
    }
    TqConfig peerCfg = TqMakePeerRuntimeConfig(BaseConfig, peer);
    auto runtime = std::make_shared<TqClientPeerRuntime>(peer.PeerId, peerCfg, Ingress.get());
    if (!runtime->Start(err)) {
        runtime->StopAll();
        return false;
    }
    std::lock_guard<std::mutex> guard(Lock);
    Peers[peer.PeerId] = std::move(runtime);
    return true;
}
```

Implement `StopAccepting`, `StopAll`, `AbortPeerTunnels`, `DrainPeer`, `SnapshotPeerMetrics`, and `SnapshotClientMetrics` by looking up the peer and forwarding to `TqClientPeerRuntime`.

`~TqClientRuntimeManager()` must call `StopAll()` before destroying `Ingress`.

- [ ] **Step 3: Make multi-peer adapter a thin wrapper**

In `src/main.cpp`, replace `TqMultiPeerRuntimeAdapter` fields with:

```cpp
TqClientRuntimeManager Manager;
```

Constructor:

```cpp
explicit TqMultiPeerRuntimeAdapter(const TqConfig& baseConfig) : Manager(baseConfig) {}
```

Methods forward to manager:

```cpp
bool StartPeer(const TqPeerConfig& peer, std::string& err) override {
    return Manager.StartPeer(peer, err);
}
```

- [ ] **Step 4: Build and test**

Run:

```bash
rtk cmake --build build-regression --target tcpquic-proxy tcpquic_router_runtime_test tcpquic_client_peer_runtime_test -j4
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_client_peer_runtime_test
```

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp src/main.cpp
rtk git commit -m "refactor(client): add shared runtime manager"
```

### Task 4: Move Single-Peer Client Onto Manager

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/runtime/client_peer_runtime.h`
- Modify: `src/runtime/client_peer_runtime.cpp`

- [ ] **Step 1: Replace `RunSinglePeerClient` runtime creation**

In `RunSinglePeerClient`, remove direct `QuicClientSession quic` and `TqSinglePeerClientRuntime`.

Use:

```cpp
TqClientRuntimeManager manager(cfg);
std::string err;
const TqPeerConfig primary = TqMakePrimaryPeerConfig(cfg);
if (!manager.StartPeer(primary, err)) {
    std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
    return 1;
}
```

`RunSinglePeerClient` should keep using `const auto started = std::chrono::steady_clock::now();` for admin uptime.

- [ ] **Step 2: Preserve speedtest control connection access**

Add to `TqClientRuntimeManager`:

```cpp
MsQuicConnection* PickConnection(const std::string& peerId);
bool EnsureAnyConnected(const std::string& peerId, std::chrono::milliseconds timeout);
```

Forward to peer runtime methods:

```cpp
MsQuicConnection* TqClientPeerRuntime::PickConnection();
bool TqClientPeerRuntime::EnsureAnyConnected(std::chrono::milliseconds timeout);
```

Use these in single-peer speedtest:

```cpp
if (!manager.EnsureAnyConnected("primary", std::chrono::seconds(10))) {
    std::fprintf(stderr, "tcpquic-proxy: speed test could not connect to QUIC peer\n");
    return 1;
}
MsQuicConnection* controlConn = manager.PickConnection("primary");
```

After `TqRunIngressClientSpeedTest(*controlConn, cfg)` returns, call `manager.StopAll()` before returning from speedtest mode.

- [ ] **Step 3: Preserve single-peer admin metrics**

In admin handler:

```cpp
TqClientMetrics metrics;
if (!manager.SnapshotClientMetrics("primary", metrics)) {
    return TqJsonResponse(503, "{\"error\":\"client runtime unavailable\"}");
}
```

Keep response builders unchanged:

```cpp
return TqJsonResponse(200, TqClientHealthJson(metrics, uptimeSeconds));
return TqJsonResponse(200, TqClientMetricsJson(metrics, uptimeSeconds));
```

- [ ] **Step 4: Delete `TqSinglePeerClientRuntime`**

Remove the entire `TqSinglePeerClientRuntime` struct from `src/main.cpp`.

- [ ] **Step 5: Build and test**

Run:

```bash
rtk cmake --build build-regression --target tcpquic-proxy tcpquic_speed_test_test tcpquic_tunnel_test -j4
rtk ./build-regression/bin/Release/tcpquic_speed_test_test
rtk ./build-regression/bin/Release/tcpquic_tunnel_test
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/main.cpp src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp
rtk git commit -m "refactor(client): route single-peer through runtime manager"
```

### Task 5: Final Cleanup and Regression

**Files:**
- Modify: `src/main.cpp`
- Modify: `docs/thread-model_cn.md`

- [ ] **Step 1: Check duplicate runtime code is gone**

Run:

```bash
rtk rg -n "TqSinglePeerClientRuntime|struct PeerRuntime|class TqMultiPeerRuntimeAdapter|TqClientRuntimeManager" src/main.cpp src/runtime/client_peer_runtime.*
```

Expected:

- no `TqSinglePeerClientRuntime`
- no nested `struct PeerRuntime` in `src/main.cpp`
- `TqMultiPeerRuntimeAdapter` remains only as a thin wrapper
- `TqClientRuntimeManager` owns peer map

- [ ] **Step 2: Update thread model wording**

Modify `docs/thread-model_cn.md` lines that currently describe:

```text
single-peer client 的 runtime 持有一个 TqClientIngressReactor 成员
multi-peer client 的 runtime adapter 持有一个共享 TqClientIngressReactor
```

Replace them with:

```text
Client runtime manager 持有一个共享 `TqClientIngressReactor`。single-peer 是一个固定 `primary` peer，multi-peer 是多个 router peers；两者都通过 `TqClientPeerRuntime` 注册 / 移除各自的 SOCKS5 / HTTP CONNECT listen fd。每个 peer 仍有自己的 `QuicClientSession`，但 listener lifecycle、delayed reconnect scheduler 和 tunnel start 编排共用同一套 peer runtime 代码。
```

- [ ] **Step 3: Run full relevant regression**

Run:

```bash
rtk bash -n scripts/dgx-msquic-vs-proxy-bench.sh
rtk cmake --build build-regression --target tcpquic-proxy tcpquic_client_peer_runtime_test tcpquic_client_ingress_reactor_test tcpquic_quic_session_reconnect_test tcpquic_tunnel_test tcpquic_speed_test_test tcpquic_router_runtime_test tcpquic_config_router_test -j4
rtk ./build-regression/bin/Release/tcpquic_client_peer_runtime_test
rtk ./build-regression/bin/Release/tcpquic_client_ingress_reactor_test
rtk ./build-regression/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build-regression/bin/Release/tcpquic_tunnel_test
rtk ./build-regression/bin/Release/tcpquic_speed_test_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_config_router_test
```

Expected: all commands exit 0.

- [ ] **Step 4: Run DGX smoke**

Run single-peer 1x1 smoke:

```bash
rtk env BIN=/home/jack/src/tcpquic-proxy/build-regression/bin/Release/tcpquic-proxy REPORT=/home/jack/src/tcpquic-proxy/docs/dgx-unified-client-peer-runtime-20260620/single-1x1.md PROXY_CASES='proxy-1x1:1:1:1' RUN_SECNETPERF=0 PROXY_START_WAIT_SEC=60 DURATION_SEC=10 scripts/dgx-msquic-vs-proxy-bench.sh
```

Expected: report contains one `tcpquic-proxy | HTTP CONNECT 单流` row with non-zero Gbps.

Create a two-peer DGX smoke config and run the client long enough to verify both ingress listeners open. Use the current DGX target values from `scripts/dgx-msquic-vs-proxy-bench.sh`.

```bash
rtk mkdir -p /tmp/tcpquic-unified-runtime/certs docs/dgx-unified-client-peer-runtime-20260620
rtk openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/tcpquic-unified-runtime/certs/ca.key -out /tmp/tcpquic-unified-runtime/certs/ca.crt -subj /CN=tcpquic-unified-runtime-ca -days 1 -sha256
rtk openssl req -newkey rsa:2048 -nodes -keyout /tmp/tcpquic-unified-runtime/certs/server.key -out /tmp/tcpquic-unified-runtime/certs/server.csr -subj /CN=server
rtk openssl x509 -req -in /tmp/tcpquic-unified-runtime/certs/server.csr -CA /tmp/tcpquic-unified-runtime/certs/ca.crt -CAkey /tmp/tcpquic-unified-runtime/certs/ca.key -CAcreateserial -out /tmp/tcpquic-unified-runtime/certs/server.crt -days 1 -sha256
rtk openssl req -newkey rsa:2048 -nodes -keyout /tmp/tcpquic-unified-runtime/certs/client.key -out /tmp/tcpquic-unified-runtime/certs/client.csr -subj /CN=client
rtk openssl x509 -req -in /tmp/tcpquic-unified-runtime/certs/client.csr -CA /tmp/tcpquic-unified-runtime/certs/ca.crt -CAkey /tmp/tcpquic-unified-runtime/certs/ca.key -CAcreateserial -out /tmp/tcpquic-unified-runtime/certs/client.crt -days 1 -sha256
rtk rsync -az /tmp/tcpquic-unified-runtime/certs/ jack@169.254.59.196:~/tcpquic-dgx-certs/
rtk rsync -az build-regression/bin/Release/tcpquic-proxy jack@169.254.59.196:~/tcpquic-dgx-bin/tcpquic-proxy
rtk ssh -o BatchMode=yes jack@169.254.59.196 'killall -9 tcpquic-proxy 2>/dev/null; LD_LIBRARY_PATH=~/tcpquic-dgx-bin nohup ~/tcpquic-dgx-bin/tcpquic-proxy server --listen 169.254.59.196:4433 --allow-targets 169.254.59.196/32,127.0.0.0/8 --cert ~/tcpquic-dgx-certs/server.crt --key ~/tcpquic-dgx-certs/server.key --ca ~/tcpquic-dgx-certs/ca.crt --compress off >/tmp/tcpquic-unified-server.log 2>&1 &'
```

Create `/tmp/tcpquic-unified-runtime/client.json`:

```json
{
  "version": 1,
  "peers": [
    {
      "peer_id": "peer-a",
      "quic_peer": "169.254.59.196:4433",
      "socks_listen": "127.0.0.1:18101",
      "http_listen": "127.0.0.1:18181",
      "quic_connections": 1,
      "compress": "off"
    },
    {
      "peer_id": "peer-b",
      "quic_peer": "169.254.59.196:4433",
      "socks_listen": "127.0.0.1:18102",
      "http_listen": "127.0.0.1:18182",
      "quic_connections": 1,
      "compress": "off"
    }
  ]
}
```

Run:

```bash
set +e
rtk timeout 20s build-regression/bin/Release/tcpquic-proxy client --client-config /tmp/tcpquic-unified-runtime/client.json --cert /tmp/tcpquic-unified-runtime/certs/client.crt --key /tmp/tcpquic-unified-runtime/certs/client.key --ca /tmp/tcpquic-unified-runtime/certs/ca.crt >docs/dgx-unified-client-peer-runtime-20260620/multi-peer-smoke.log 2>&1
rc=$?
set -e
test "$rc" -eq 124
rtk rg -n "peer peer-a HTTP CONNECT listening|peer peer-b HTTP CONNECT listening" docs/dgx-unified-client-peer-runtime-20260620/multi-peer-smoke.log
```

Expected: `timeout` returns 124 because the client stays running; `rg` finds both peer listener lines. Record the command output in:

```text
docs/dgx-unified-client-peer-runtime-20260620/summary.md
```

- [ ] **Step 5: Commit final docs and cleanup**

```bash
rtk git add src/main.cpp src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp src/unittest/client_peer_runtime_test.cpp src/CMakeLists.txt docs/dgx-unified-client-peer-runtime-20260620 docs/thread-model_cn.md
rtk git commit -m "test(client): verify unified peer runtime"
```

---

## Self-Review

- Spec coverage: the tasks cover helper extraction, shared peer runtime, manager, single-peer migration, multi-peer adapter migration, tests, DGX smoke, and documentation.
- Placeholder scan: no implementation step depends on unspecified behavior; each task names concrete files, commands, and expected outcomes.
- Type consistency: class and method names are consistent across tasks: `TqClientPeerRuntime`, `TqClientRuntimeManager`, `TqMakePrimaryPeerConfig`, and `TqMakePeerRuntimeConfig`.
