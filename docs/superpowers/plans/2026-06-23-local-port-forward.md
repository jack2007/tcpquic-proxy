# Local Port Forward Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `ssh -L` style local TCP port forwarding to `tcpquic-proxy` client mode.

**Architecture:** Treat port forwarding as a third client ingress type beside SOCKS5 and HTTP CONNECT. A forward listener accepts a local TCP connection, builds a fixed-target `TunnelRequest`, opens the existing QUIC tunnel, and hands the local fd directly to the existing relay after OPEN_OK. No QUIC wire protocol or server dial path changes are required.

**Tech Stack:** C++17, CMake, msquic, existing platform socket/reactor abstractions, existing unit-test executable style under `src/unittest`.

---

## File Structure

- Modify `src/config/config.h`: add `TqPortForwardConfig`, add `PortForwards` to `TqPeerConfig` and `TqConfig`.
- Modify `src/config/config.cpp`: parse and validate `--forward`, runtime JSON `port_forwards`, legacy router JSON `port_forwards`, and listener uniqueness.
- Modify `src/runtime/router_runtime.h`: add `PortForwards` to `TqPeerMetrics`.
- Modify `src/runtime/router_runtime.cpp`: serialize/parse `port_forwards`, compare forward configs in data-plane change checks, include metrics/config JSON.
- Modify `src/runtime/server_metrics.h`: add `PortForwards` to single-peer `TqClientMetrics`.
- Modify `src/runtime/server_metrics.cpp`: serialize `port_forwards` in single-peer client metrics JSON.
- Modify `src/runtime/client_peer_runtime.h`: copy `PortForwards` through primary and peer runtime config helpers.
- Modify `src/runtime/client_peer_runtime.cpp`: pass `PortForwards` to ingress, expose metrics, log forward listeners.
- Modify `src/ingress/client_ingress_reactor.h`: represent peer listeners as a vector, add `ListenProto::PortForward`, add forward test accessors.
- Modify `src/ingress/client_ingress_reactor.cpp`: create forward listeners, accept direct-open clients, skip protocol response for port forward.
- Modify `src/tunnel/tcp_tunnel.h`: document `IngressTraceProto = 3` for port-forward.
- Modify `src/unittest/config_router_test.cpp`: add config parsing/validation coverage.
- Modify `src/unittest/router_runtime_test.cpp`: add router JSON, metrics, and data-plane change coverage.
- Modify `src/unittest/client_peer_runtime_test.cpp`: add primary/peer helper propagation coverage.
- Modify `src/unittest/client_ingress_reactor_test.cpp`: add forward listener/open success/failure/timeout coverage.
- Modify `scripts/test-tcpquic-proxy.sh`: add one local forward end-to-end smoke path.
- Modify `README.md`, `docs/config_guide.md`, and `docs/config_guide_cn.md`: document CLI and JSON usage.

## Implementation Notes

- Continue to prefix shell commands with `rtk`.
- Use `apply_patch` for manual edits.
- Do not touch unrelated untracked build/docs artifacts already present in the worktree.
- Use fixed return-code blocks in the existing single-binary tests; allocate new return codes above the local range already used in each file.
- Avoid introducing a new server-side concept. The server must see the same OPEN request shape as SOCKS5/HTTP CONNECT tunnels.

### Task 1: Config Model And Parser Tests

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Test: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing CLI and JSON tests**

Add helper assertions near the existing `config_router_test.cpp` config parser cases:

```cpp
{
    const char* args[] = {
        "tcpquic-proxy", "client",
        "--peer", "127.0.0.1:14444",
        "--forward", "127.0.0.1:15432=db.example.com:5432",
        "--forward=[::1]:18080=10.0.0.15:8080",
        "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 190;
    if (cfg.PortForwards.size() != 2) return 191;
    if (cfg.PortForwards[0].Listen != "127.0.0.1:15432") return 192;
    if (cfg.PortForwards[0].TargetHost != "db.example.com") return 193;
    if (cfg.PortForwards[0].TargetPort != 5432) return 194;
    if (cfg.PortForwards[1].Listen != "[::1]:18080") return 195;
    if (cfg.PortForwards[1].TargetHost != "10.0.0.15") return 196;
    if (cfg.PortForwards[1].TargetPort != 8080) return 197;
}
{
    const char* args[] = {
        "tcpquic-proxy", "client",
        "--peer", "127.0.0.1:14444",
        "--forward", "127.0.0.1:15432",
        "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 198;
    if (err.find("--forward") == std::string::npos) return 199;
}
{
    const char* args[] = {
        "tcpquic-proxy", "server",
        "--listen", "127.0.0.1:14444",
        "--forward", "127.0.0.1:15432=db.example.com:5432",
        "--cert", "a.crt", "--key", "a.key"};
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 200;
    if (err.find("--forward") == std::string::npos) return 201;
}
```

Add legacy router JSON cases after existing duplicate listen tests:

```cpp
{
    TqRouterConfig router;
    std::string err;
    if (!Load(R"json({"version":1,"peers":[{"peer_id":"db","quic_peer":"127.0.0.1:14444","socks_listen":"","http_listen":"","port_forwards":[{"listen":"127.0.0.1:15432","target":"db.internal:5432"}]}]})json", router, err)) return 202;
    if (router.Peers.size() != 1) return 203;
    if (router.Peers[0].PortForwards.size() != 1) return 204;
    if (router.Peers[0].PortForwards[0].Listen != "127.0.0.1:15432") return 205;
    if (router.Peers[0].PortForwards[0].TargetHost != "db.internal") return 206;
    if (router.Peers[0].PortForwards[0].TargetPort != 5432) return 207;
}
{
    TqRouterConfig router;
    std::string err;
    if (Load(R"json({"version":1,"peers":[{"peer_id":"empty","quic_peer":"127.0.0.1:14444","socks_listen":"","http_listen":"","port_forwards":[]}]})json", router, err)) return 208;
    if (err.find("at least one ingress") == std::string::npos) return 209;
}
{
    TqRouterConfig router;
    std::string err;
    if (Load(R"json({"version":1,"peers":[{"peer_id":"dup","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:15432","port_forwards":[{"listen":"127.0.0.1:15432","target":"db.internal:5432"}]}]})json", router, err)) return 210;
    if (err.find("duplicate listen") == std::string::npos) return 211;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: compile fails because `TqConfig::PortForwards` and `TqPeerConfig::PortForwards` do not exist, or the executable returns one of the new failure codes.

- [ ] **Step 3: Add config types**

Patch `src/config/config.h`:

```cpp
struct TqPortForwardConfig {
    std::string Listen;
    std::string TargetHost;
    uint16_t TargetPort{0};
};

struct TqPeerConfig {
    std::string PeerId;
    std::string QuicPeer;
    std::string SocksListen;
    std::string HttpListen;
    std::vector<TqPortForwardConfig> PortForwards;
    uint32_t QuicConnections{0};
    std::string Compress;
    bool Enabled{true};
};
```

Add to `TqConfig`:

```cpp
std::vector<TqPortForwardConfig> PortForwards;
```

- [ ] **Step 4: Add host-port parsing helpers**

In `src/config/config.cpp`, add helper functions near `IsHostPort()`:

```cpp
bool SplitHostPortValue(const std::string& value, std::string& host, uint16_t& port) {
    std::string portText;
    if (!value.empty() && value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close == 1 || close + 2 > value.size() || value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portText = value.substr(close + 2);
    } else {
        const size_t colon = value.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size() ||
            value.find(':', colon + 1) != std::string::npos) {
            return false;
        }
        host = value.substr(0, colon);
        portText = value.substr(colon + 1);
    }

    uint32_t parsed = 0;
    if (!ParseUint32(portText.c_str(), parsed) || parsed == 0 || parsed > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return !host.empty();
}

bool ParsePortForwardValue(const std::string& value, TqPortForwardConfig& out) {
    const size_t equals = value.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= value.size()) {
        return false;
    }
    TqPortForwardConfig parsed;
    parsed.Listen = value.substr(0, equals);
    if (!IsHostPort(parsed.Listen)) {
        return false;
    }
    if (!SplitHostPortValue(value.substr(equals + 1), parsed.TargetHost, parsed.TargetPort)) {
        return false;
    }
    out = std::move(parsed);
    return true;
}
```

- [ ] **Step 5: Parse CLI and JSON fields**

In `TqPrintUsage()`, add:

```cpp
"  --forward <local=target>    Local port forward, repeatable\n"
```

In `TqParseArgs()`, add an option branch before `--peer`:

```cpp
} else if (GetOptionValue(arg, "--forward", value)) {
    if (value == nullptr) {
        value = NextArg(i, argc, argv, "--forward", err);
        if (value == nullptr) {
            return false;
        }
    }
    TqPortForwardConfig forward;
    if (!ParsePortForwardValue(value, forward)) {
        err = "invalid value for --forward";
        return false;
    }
    cfg.PortForwards.push_back(std::move(forward));
```

Add client-only validation after `--client-config` mode checks:

```cpp
if (!cfg.PortForwards.empty() && cfg.Mode != TqMode::Client) {
    err = "--forward is valid only in client mode";
    return false;
}
```

Extend both `JsonParser::ParseRuntimePeer()` and `JsonParser::ParsePeer()` with:

```cpp
} else if (key == "port_forwards") {
    if (!ParsePortForwards(peer.PortForwards)) return false;
```

Add parser methods:

```cpp
bool ParsePortForwards(std::vector<TqPortForwardConfig>& forwards) {
    forwards.clear();
    if (!Consume('[')) return Error("port_forwards must be an array");
    if (Consume(']')) return true;
    do {
        TqPortForwardConfig forward;
        if (!ParsePortForwardObject(forward)) return false;
        forwards.push_back(std::move(forward));
    } while (Consume(','));
    return Consume(']') || Error("malformed port_forwards array");
}

bool ParsePortForwardObject(TqPortForwardConfig& forward) {
    if (!Consume('{')) return Error("port_forward must be an object");
    bool hasListen = false;
    bool hasTarget = false;
    std::string target;
    if (Consume('}')) return Error("port_forward listen and target are required");
    do {
        std::string key;
        if (!ParseString(key) || !Consume(':')) return Error("malformed port_forward object");
        if (key == "listen") {
            hasListen = true;
            if (!ParseString(forward.Listen) || !IsHostPort(forward.Listen)) return Error("invalid port_forward.listen");
        } else if (key == "target") {
            hasTarget = true;
            if (!ParseString(target) || !SplitHostPortValue(target, forward.TargetHost, forward.TargetPort)) {
                return Error("invalid port_forward.target");
            }
        } else {
            return Error(("unknown port_forward key: " + key).c_str());
        }
    } while (Consume(','));
    if (!Consume('}')) return Error("malformed port_forward object");
    return (hasListen && hasTarget) || Error("port_forward listen and target are required");
}
```

- [ ] **Step 6: Validate listener uniqueness and only-forward peers**

Replace the `socks_listen is required` validation in `TqValidateRouterConfig()` with:

```cpp
const bool hasSocks = !peer.SocksListen.empty();
const bool hasHttp = !peer.HttpListen.empty();
const bool hasForwards = !peer.PortForwards.empty();
if (!hasSocks && !hasHttp && !hasForwards) {
    err = "at least one ingress is required for " + peer.PeerId;
    return false;
}
if (hasSocks) {
    if (!IsHostPort(peer.SocksListen)) { err = "invalid socks_listen for " + peer.PeerId; return false; }
    if (!listens.insert(peer.SocksListen).second) { err = "duplicate listen: " + peer.SocksListen; return false; }
}
if (hasHttp) {
    if (!IsHostPort(peer.HttpListen)) { err = "invalid http_listen for " + peer.PeerId; return false; }
    if (!listens.insert(peer.HttpListen).second) { err = "duplicate listen: " + peer.HttpListen; return false; }
}
for (const auto& forward : peer.PortForwards) {
    if (!IsHostPort(forward.Listen)) { err = "invalid port_forward.listen for " + peer.PeerId; return false; }
    if (forward.TargetHost.empty() || forward.TargetPort == 0) {
        err = "invalid port_forward.target for " + peer.PeerId;
        return false;
    }
    if (!listens.insert(forward.Listen).second) { err = "duplicate listen: " + forward.Listen; return false; }
}
```

For single-peer CLI validation, create a temporary listener set after client required fields:

```cpp
if (cfg.Router.Peers.empty()) {
    std::set<std::string> listens;
    if (!cfg.SocksListen.empty() && !listens.insert(cfg.SocksListen).second) {
        err = "duplicate listen: " + cfg.SocksListen;
        return false;
    }
    if (!cfg.HttpListen.empty() && !listens.insert(cfg.HttpListen).second) {
        err = "duplicate listen: " + cfg.HttpListen;
        return false;
    }
    for (const auto& forward : cfg.PortForwards) {
        if (!listens.insert(forward.Listen).second) {
            err = "duplicate listen: " + forward.Listen;
            return false;
        }
    }
}
```

- [ ] **Step 7: Run config test**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 8: Commit**

```bash
rtk git add src/config/config.h src/config/config.cpp src/unittest/config_router_test.cpp
rtk git commit -m "feat: parse local port forwards"
```

### Task 2: Router Runtime JSON And Metrics

**Files:**
- Modify: `src/runtime/router_runtime.h`
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/runtime/server_metrics.h`
- Modify: `src/runtime/server_metrics.cpp`
- Modify: `src/runtime/client_peer_runtime.h`
- Modify: `src/runtime/client_peer_runtime.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing runtime tests**

In `src/unittest/router_runtime_test.cpp`, update `FakeAdapter::StartPeer()` to capture the last peer:

```cpp
TqPeerConfig LastStartedPeer;

bool StartPeer(const TqPeerConfig& peer, std::string& err) override {
    Started.push_back(peer.PeerId);
    LastStartedPeer = peer;
    if (FailStarts != 0) {
        --FailStarts;
        err = "start failed";
        return false;
    }
    return true;
}
```

Add helper:

```cpp
static TqPortForwardConfig Forward(const std::string& listen, const std::string& host, uint16_t port) {
    TqPortForwardConfig f;
    f.Listen = listen;
    f.TargetHost = host;
    f.TargetPort = port;
    return f;
}
```

Add cases near the existing data-plane change tests:

```cpp
{
    FakeAdapter adapter;
    TqRouterRuntime adapterRuntime(&adapter);
    TqRouterConfig cfg;
    cfg.Peers.push_back(Peer("forward-change", "127.0.0.1:11021"));
    cfg.Peers[0].PortForwards.push_back(Forward("127.0.0.1:15432", "db.internal", 5432));
    std::string err;
    if (!adapterRuntime.ApplyConfig(cfg, err)) return 120;
    if (adapter.LastStartedPeer.PortForwards.size() != 1) return 121;
    cfg.Peers[0].PortForwards[0].TargetPort = 5433;
    if (!adapterRuntime.ApplyConfig(cfg, err)) return 122;
    if (adapter.Stopped.size() != 1 || adapter.Stopped[0] != "forward-change") return 123;
    if (adapter.AbortAll.size() != 1 || adapter.AbortAll[0] != "forward-change") return 124;
    if (adapter.Drained.size() != 1 || adapter.Drained[0] != "forward-change") return 125;
    if (adapter.Started.size() != 2 || adapter.Started[1] != "forward-change") return 126;
}
{
    TqRouterRuntime runtime;
    TqRouterConfig cfg;
    cfg.Peers.push_back(Peer("json-forward", "127.0.0.1:11022"));
    cfg.Peers[0].PortForwards.push_back(Forward("127.0.0.1:15432", "db.internal", 5432));
    std::string err;
    if (!runtime.ApplyConfig(cfg, err)) return 127;
    const std::string configJson = runtime.ConfigJson();
    if (configJson.find("\"port_forwards\"") == std::string::npos) return 128;
    if (configJson.find("\"target\":\"db.internal:5432\"") == std::string::npos) return 129;
    const std::string metricsJson = runtime.MetricsJson();
    if (metricsJson.find("\"port_forwards\"") == std::string::npos) return 130;
    if (metricsJson.find("\"listen\":\"127.0.0.1:15432\"") == std::string::npos) return 131;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_router_runtime_test
```

Expected: compile fails because runtime metrics/config serialization does not include `PortForwards`, or the executable returns one of the new codes.

- [ ] **Step 3: Add metrics fields and config helper copying**

In `src/runtime/router_runtime.h`, add:

```cpp
std::vector<TqPortForwardConfig> PortForwards;
```

to `TqPeerMetrics`.

In `src/runtime/server_metrics.h`, add includes:

```cpp
#include "config.h"

#include <vector>
```

Then add the same field to `TqClientMetrics`:

```cpp
std::vector<TqPortForwardConfig> PortForwards;
```

In `src/runtime/client_peer_runtime.h`, update helpers:

```cpp
peer.PortForwards = cfg.PortForwards;
```

inside `TqMakePrimaryPeerConfig()`, and:

```cpp
cfg.PortForwards = peer.PortForwards;
```

inside `TqMakePeerRuntimeConfig()`.

In `src/runtime/client_peer_runtime.cpp`, set metrics:

```cpp
metrics.PortForwards = Config.PortForwards;
```

in both `SnapshotPeerMetrics()` and `SnapshotClientMetrics()` where peer/client metrics carry listen addresses.

In `src/runtime/server_metrics.cpp`, add a local serializer matching router runtime output:

```cpp
static std::string TqPortForwardTargetText(const TqPortForwardConfig& forward) {
    const bool ipv6 = forward.TargetHost.find(':') != std::string::npos;
    return (ipv6 ? "[" + forward.TargetHost + "]" : forward.TargetHost) + ":" +
        std::to_string(forward.TargetPort);
}

static void TqAppendPortForwardsJson(std::ostringstream& out, const std::vector<TqPortForwardConfig>& forwards) {
    out << "\"port_forwards\":[";
    for (size_t i = 0; i < forwards.size(); ++i) {
        if (i != 0) out << ',';
        out << '{';
        TqAppendJsonString(out, "listen", forwards[i].Listen);
        out << ',';
        TqAppendJsonString(out, "target", TqPortForwardTargetText(forwards[i]));
        out << '}';
    }
    out << ']';
}
```

Call it from `TqClientMetricsJson()` after `http_listen`:

```cpp
out << ',';
TqAppendPortForwardsJson(out, metrics.PortForwards);
```

- [ ] **Step 4: Add router JSON serialization**

In `src/runtime/router_runtime.cpp`, add:

```cpp
std::string PortForwardTargetText(const TqPortForwardConfig& forward) {
    const bool ipv6 = forward.TargetHost.find(':') != std::string::npos;
    return (ipv6 ? "[" + forward.TargetHost + "]" : forward.TargetHost) + ":" +
        std::to_string(forward.TargetPort);
}

void AppendPortForwardsJson(std::ostringstream& out, const std::vector<TqPortForwardConfig>& forwards) {
    out << "\"port_forwards\":[";
    for (size_t i = 0; i < forwards.size(); ++i) {
        if (i != 0) out << ',';
        out << '{';
        AppendJsonString(out, "listen", forwards[i].Listen);
        out << ',';
        AppendJsonString(out, "target", PortForwardTargetText(forwards[i]));
        out << '}';
    }
    out << ']';
}
```

Call it from both `AppendPeerConfigJson()` and `AppendPeerMetricsJson()` after `http_listen`:

```cpp
out << ',';
AppendPortForwardsJson(out, peer.PortForwards);
```

- [ ] **Step 5: Parse admin `PUT /config` port_forwards**

In `RouterJsonParser::ParsePeer()`, add:

```cpp
} else if (key == "port_forwards") {
    if (!ParsePortForwards(peer.PortForwards)) return false;
```

Add parser methods mirroring Task 1 parser logic. Keep the target parser local to `router_runtime.cpp`:

```cpp
bool ParsePortForwardPort(const std::string& text, uint16_t& port) {
    if (text.empty()) return false;
    uint32_t value = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        value = value * 10 + static_cast<uint32_t>(ch - '0');
        if (value > 65535) return false;
    }
    if (value == 0) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

bool ParsePortForwardTargetText(const std::string& value, std::string& host, uint16_t& port) {
    std::string portText;
    if (!value.empty() && value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close == 1 || close + 2 > value.size() || value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portText = value.substr(close + 2);
    } else {
        const size_t colon = value.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size() ||
            value.find(':', colon + 1) != std::string::npos) {
            return false;
        }
        host = value.substr(0, colon);
        portText = value.substr(colon + 1);
    }
    return !host.empty() && ParsePortForwardPort(portText, port);
}

bool ParsePortForwards(std::vector<TqPortForwardConfig>& forwards) {
    forwards.clear();
    if (!Consume('[')) return Error("port_forwards must be an array");
    if (Consume(']')) return true;
    do {
        TqPortForwardConfig forward;
        if (!ParsePortForwardObject(forward)) return false;
        forwards.push_back(std::move(forward));
    } while (Consume(','));
    return Consume(']') || Error("malformed port_forwards array");
}

bool ParsePortForwardObject(TqPortForwardConfig& forward) {
    if (!Consume('{')) return Error("port_forward must be an object");
    bool hasListen = false;
    bool hasTarget = false;
    std::string target;
    if (Consume('}')) return Error("port_forward listen and target are required");
    do {
        std::string key;
        if (!ParseString(key) || !Consume(':')) return Error("malformed port_forward object");
        if (key == "listen") {
            hasListen = true;
            if (!ParseStringField(forward.Listen, "invalid port_forward.listen")) return false;
        } else if (key == "target") {
            hasTarget = true;
            if (!ParseStringField(target, "invalid port_forward.target") ||
                !ParsePortForwardTargetText(target, forward.TargetHost, forward.TargetPort)) {
                return Error("invalid port_forward.target");
            }
        } else {
            return Error(("unknown port_forward key: " + key).c_str());
        }
    } while (Consume(','));
    if (!Consume('}')) return Error("malformed port_forward object");
    return (hasListen && hasTarget) || Error("port_forward listen and target are required");
}
```

- [ ] **Step 6: Compare forward configs**

Add helper:

```cpp
bool SamePortForwards(
    const std::vector<TqPortForwardConfig>& a,
    const std::vector<TqPortForwardConfig>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].Listen != b[i].Listen ||
            a[i].TargetHost != b[i].TargetHost ||
            a[i].TargetPort != b[i].TargetPort) {
            return false;
        }
    }
    return true;
}
```

Update `SameBridgeActivePeer()` and `PeerDataPlaneChanged()` to include `SamePortForwards()`.

In `ApplyConfig()`, set:

```cpp
metrics.PortForwards = peer.PortForwards;
```

- [ ] **Step 7: Run router runtime tests**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_router_runtime_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 8: Commit**

```bash
rtk git add src/runtime/router_runtime.h src/runtime/router_runtime.cpp src/runtime/server_metrics.h src/runtime/server_metrics.cpp src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: expose port forwards in router runtime"
```

### Task 3: Ingress Listener Refactor

**Files:**
- Modify: `src/ingress/client_ingress_reactor.h`
- Modify: `src/ingress/client_ingress_reactor.cpp`
- Test: `src/unittest/client_ingress_reactor_test.cpp`

- [ ] **Step 1: Write failing listener-list test**

Update `MakePeer()` calls to keep current SOCKS behavior unchanged. Add this test before `TestDuplicateAddPeerReturnsFalse()`:

```cpp
int TestAddPeerWithForwardListenAddress() {
    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 1400;
    }

    TqClientIngressPeer peer = MakePeer("forward-listen");
    TqPortForwardConfig forward;
    forward.Listen = "127.0.0.1:0";
    forward.TargetHost = "db.internal";
    forward.TargetPort = 5432;
    peer.PortForwards.push_back(forward);

    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 1401;
    }
    const std::string address = reactor.PortForwardListenAddressForTest("forward-listen", 0);
    if (address.empty() || address == "127.0.0.1:0") {
        reactor.Stop();
        return 1402;
    }
    TqSocketHandle fd = TqInvalidSocket;
    if (!ConnectTo(address, fd)) {
        reactor.Stop();
        return 1403;
    }
    CloseFd(fd);
    reactor.Stop();
    return 0;
}
```

Add it to `main()`:

```cpp
if ((rc = TestAddPeerWithForwardListenAddress()) != 0) return rc;
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_client_ingress_reactor_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: compile fails because `TqClientIngressPeer::PortForwards` and `PortForwardListenAddressForTest()` do not exist.

- [ ] **Step 3: Add ingress data structures**

In `src/ingress/client_ingress_reactor.h`, extend `TqClientIngressPeer`:

```cpp
std::vector<TqPortForwardConfig> PortForwards;
```

Extend `ListenProto`:

```cpp
PortForward,
```

Replace `PeerEntry` fixed fd fields with:

```cpp
struct PeerListen {
    ListenProto Proto{ListenProto::Socks5};
    TqSocketHandle Fd{TqInvalidSocket};
    std::string Address;
    TqPortForwardConfig Forward;
};

struct PeerEntry {
    TqClientIngressPeer Peer;
    std::vector<PeerListen> Listeners;
};
```

Extend `ListenEntry`:

```cpp
TqPortForwardConfig Forward;
```

Add test accessor declaration under `TQ_UNIT_TESTING`:

```cpp
std::string PortForwardListenAddressForTest(const std::string& peerId, size_t index) const;
```

- [ ] **Step 4: Refactor AddPeer listener creation**

In `AddPeer()`, remove the hard requirement that `peer.SocksListen` is non-empty. Build a vector of `std::shared_ptr<TqListenSocket>` plus metadata:

```cpp
struct PendingListen {
    ListenProto Proto;
    TqPortForwardConfig Forward;
    std::shared_ptr<TqListenSocket> Socket;
};
std::vector<PendingListen> pending;
```

Create listeners:

```cpp
if (!peer.SocksListen.empty()) {
    auto listen = std::make_shared<TqListenSocket>();
    if (!TqCreateNonBlockingListenSocket(peer.SocksListen, *listen)) return false;
    pending.push_back(PendingListen{ListenProto::Socks5, TqPortForwardConfig{}, listen});
}
if (!peer.HttpListen.empty()) {
    auto listen = std::make_shared<TqListenSocket>();
    if (!TqCreateNonBlockingListenSocket(peer.HttpListen, *listen)) {
        for (auto& item : pending) TqCloseFd(item.Socket->Fd);
        return false;
    }
    pending.push_back(PendingListen{ListenProto::HttpConnect, TqPortForwardConfig{}, listen});
}
for (const auto& forward : peer.PortForwards) {
    auto listen = std::make_shared<TqListenSocket>();
    if (!TqCreateNonBlockingListenSocket(forward.Listen, *listen)) {
        for (auto& item : pending) TqCloseFd(item.Socket->Fd);
        return false;
    }
    pending.push_back(PendingListen{ListenProto::PortForward, forward, listen});
}
if (pending.empty()) return false;
```

Inside the `EnqueueSync()` lambda, register every pending listener:

```cpp
for (auto& item : pending) {
    Listens.emplace(item.Socket->Fd, ListenEntry{peer.PeerId, item.Proto, item.Forward});
    if (!Reactor.Add(item.Socket->Fd, TqReactorEvents::Read, [this](TqSocketHandle fd, uint32_t events) {
            if ((events & (TqReactorEvents::Read | TqReactorEvents::Error)) != 0) {
                AcceptLoop(fd);
            }
        })) {
        cleanup();
        return false;
    }
}
```

Move fds into `PeerEntry::Listeners`:

```cpp
for (auto& item : pending) {
    PeerListen listener;
    listener.Proto = item.Proto;
    listener.Fd = item.Socket->Fd;
    listener.Address = item.Socket->Address;
    listener.Forward = item.Forward;
    entry.Listeners.push_back(listener);
    item.Socket->Fd = TqInvalidSocket;
}
```

- [ ] **Step 5: Update accessors and cleanup**

Update test accessors:

```cpp
std::string TqClientIngressReactor::SocksListenAddressForTest(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(Mutex);
    const auto it = Peers.find(peerId);
    if (it == Peers.end()) return {};
    for (const auto& listener : it->second.Listeners) {
        if (listener.Proto == ListenProto::Socks5) return listener.Address;
    }
    return {};
}
```

Add the forward accessor:

```cpp
std::string TqClientIngressReactor::PortForwardListenAddressForTest(
    const std::string& peerId,
    size_t index) const {
    std::lock_guard<std::mutex> lock(Mutex);
    const auto it = Peers.find(peerId);
    if (it == Peers.end()) return {};
    size_t seen = 0;
    for (const auto& listener : it->second.Listeners) {
        if (listener.Proto != ListenProto::PortForward) continue;
        if (seen == index) return listener.Address;
        ++seen;
    }
    return {};
}
```

Update `RemovePeerLocked()` and `CloseAllLocked()` to iterate `entry.Listeners`, remove each fd from `Reactor`, erase `Listens`, and close the fd.

- [ ] **Step 6: Run ingress tests**

Run:

```bash
rtk cmake --build build --target tcpquic_client_ingress_reactor_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: build succeeds and existing SOCKS/HTTP tests still exit 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp src/unittest/client_ingress_reactor_test.cpp
rtk git commit -m "refactor: generalize client ingress listeners"
```

### Task 4: Port Forward Direct Open Flow

**Files:**
- Modify: `src/ingress/client_ingress_reactor.h`
- Modify: `src/ingress/client_ingress_reactor.cpp`
- Modify: `src/tunnel/tcp_tunnel.h`
- Test: `src/unittest/client_ingress_reactor_test.cpp`

- [ ] **Step 1: Write failing direct-open tests**

Add success test:

```cpp
int TestPortForwardOpenAcceptsWithoutProxyResponse() {
    std::atomic<int> startCalls{0};
    std::atomic<int> acceptCalls{0};
    std::atomic<int> rejectCalls{0};
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x40));
    TunnelRequest captured{};

    TqClientIngressPeer peer = MakePeer("forward-open");
    TqPortForwardConfig forward;
    forward.Listen = "127.0.0.1:0";
    forward.TargetHost = "db.internal";
    forward.TargetPort = 5432;
    peer.PortForwards.push_back(forward);
    peer.StartTunnel = [&](const TunnelRequest& req, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (!TqSocketValid(fd)) return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        captured = req;
        ++startCalls;
        std::thread([onComplete, fakeHandle]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            onComplete(fakeHandle, TqTunnelStartResult{true, TqOpenError::Ok, 300});
        }).detach();
        return fakeHandle;
    };
    peer.AcceptTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle == fakeHandle) ++acceptCalls;
        return handle == fakeHandle;
    };
    peer.RejectTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle == fakeHandle) ++rejectCalls;
    };
    peer.CancelTunnel = [](TqClientTunnelOpenHandle*) {};

    TqClientIngressReactor reactor;
    if (!reactor.Start()) return 1500;
    if (!reactor.AddPeer(peer)) { reactor.Stop(); return 1501; }
    TqSocketHandle fd = TqInvalidSocket;
    if (!ConnectTo(reactor.PortForwardListenAddressForTest("forward-open", 0), fd)) {
        reactor.Stop();
        return 1502;
    }
    if (!WaitUntil([&]() { return acceptCalls == 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 1503;
    }
    const std::vector<uint8_t> unexpected = ReadAvailable(fd);
    if (!unexpected.empty()) {
        CloseFd(fd);
        reactor.Stop();
        return 1504;
    }
    if (startCalls != 1 || rejectCalls != 0) {
        CloseFd(fd);
        reactor.Stop();
        return 1505;
    }
    if (captured.AddrType != TQ_ADDR_DOMAIN || std::string(captured.Host) != "db.internal" ||
        captured.Port != 5432 || captured.IngressTraceProto != 3) {
        CloseFd(fd);
        reactor.Stop();
        return 1506;
    }
    CloseFd(fd);
    reactor.Stop();
    return 0;
}
```

Add failure test:

```cpp
int TestPortForwardOpenFailureClosesClient() {
    std::atomic<int> rejectCalls{0};
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x41));

    TqClientIngressPeer peer = MakePeer("forward-fail");
    peer.PortForwards.push_back(TqPortForwardConfig{"127.0.0.1:0", "db.internal", 5432});
    peer.StartTunnel = [fakeHandle](const TunnelRequest&, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (!TqSocketValid(fd)) return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        std::thread([onComplete, fakeHandle]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            onComplete(fakeHandle, TqTunnelStartResult{false, TqOpenError::AclDenied, 0});
        }).detach();
        return fakeHandle;
    };
    peer.AcceptTunnel = [](TqClientTunnelOpenHandle*) { return false; };
    peer.RejectTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle == fakeHandle) ++rejectCalls;
    };
    peer.CancelTunnel = [](TqClientTunnelOpenHandle*) {};

    TqClientIngressReactor reactor;
    if (!reactor.Start()) return 1510;
    if (!reactor.AddPeer(peer)) { reactor.Stop(); return 1511; }
    TqSocketHandle fd = TqInvalidSocket;
    if (!ConnectTo(reactor.PortForwardListenAddressForTest("forward-fail", 0), fd)) {
        reactor.Stop();
        return 1512;
    }
    if (!WaitUntil([&]() { return rejectCalls == 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 1513;
    }
    std::vector<uint8_t> data;
    if (ReadExactWithTimeout(fd, 1, data)) {
        CloseFd(fd);
        reactor.Stop();
        return 1514;
    }
    CloseFd(fd);
    reactor.Stop();
    return 0;
}
```

Add both tests to `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake --build build --target tcpquic_client_ingress_reactor_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: direct-open tests fail because forward accept still enters handshake/read logic.

- [ ] **Step 3: Store fixed request on client entries**

In `ClientEntry`, add:

```cpp
TunnelRequest FixedRequest{};
bool HasFixedRequest{false};
```

Add helper in `client_ingress_reactor.cpp`:

```cpp
uint8_t TqAddrTypeForForwardHost(const std::string& host) {
    in_addr ipv4{};
    if (TqInetPton(AF_INET, host.c_str(), &ipv4)) {
        return TQ_ADDR_IPV4;
    }

    in6_addr ipv6{};
    if (TqInetPton(AF_INET6, host.c_str(), &ipv6)) {
        return TQ_ADDR_IPV6;
    }

    return TQ_ADDR_DOMAIN;
}

TunnelRequest TqBuildPortForwardRequest(const TqPortForwardConfig& forward) {
    TunnelRequest req{};
    req.Port = forward.TargetPort;
    req.IngressTraceProto = 3;
    std::snprintf(req.Host, sizeof(req.Host), "%s", forward.TargetHost.c_str());
    req.AddrType = TqAddrTypeForForwardHost(forward.TargetHost);
    return req;
}
```

Add includes if they are not already present:

```cpp
#include <cstdio>
```

`client_ingress_reactor.h` already includes `platform_socket.h`, which provides `in_addr`, `in6_addr`, `AF_INET`, `AF_INET6`, and `TqInetPton` on all supported platforms.

- [ ] **Step 4: Start forward open immediately after accept**

In `AcceptLoop()`, copy the forward config while reading the listen table:

```cpp
TqPortForwardConfig forward;
{
    std::lock_guard<std::mutex> lock(Mutex);
    const auto listenIt = Listens.find(listenFd);
    if (listenIt == Listens.end()) {
        return;
    }
    peerId = listenIt->second.PeerId;
    proto = listenIt->second.Proto;
    forward = listenIt->second.Forward;
}
```

When accepting a client fd, avoid calling `StartClientOpen()` while holding `Mutex`. Use a local flag:

```cpp
bool startDirectOpen = false;
```

Inside the existing client creation lock, after constructing `ClientEntry client`, branch on proto:

```cpp
if (proto == ListenProto::PortForward) {
    client.State = TqClientIngressState(TqClientIngressProto::Socks5);
    client.FixedRequest = TqBuildPortForwardRequest(forward);
    client.HasFixedRequest = true;
    startDirectOpen = true;
} else {
    std::shared_ptr<const TqProxyAuthTable> auth =
        std::make_shared<TqProxyAuthTable>(peerIt->second.Peer.Config.Router.ProxyAuth);
    client.State = TqClientIngressState(TqToIngressProto(proto == ListenProto::Socks5), auth);
}
```

After leaving the lock, call:

```cpp
if (startDirectOpen) {
    StartClientOpen(clientFd);
}
```

Use `if (startDirectOpen)` rather than re-reading shared state. The reactor event registration for port-forward clients can still register `Read`; `StartClientOpen()` immediately modifies the fd to `Error` while OPEN is pending. It must not wait for a read event before opening.

- [ ] **Step 5: Use fixed request in StartClientOpen**

Change:

```cpp
request = client.State.Request();
```

to:

```cpp
request = client.HasFixedRequest ? client.FixedRequest : client.State.Request();
```

When setting default `PendingWrite`, skip it for port-forward:

```cpp
if (it->second.PendingWrite.empty() && it->second.Proto != ListenProto::PortForward) {
    it->second.PendingWrite = TqBuildOpenResponse(socks, TqOpenError::Internal);
}
```

- [ ] **Step 6: Complete port-forward open without response**

In `CompleteClientOpen()`, branch:

```cpp
if (client.Proto == ListenProto::PortForward) {
    client.OpenCompletion = completionState;
    client.OpenSucceeded = result.Ok;
    client.Phase = ClientPhase::WritingOpenResponse;
    if (result.Ok) {
        (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
    } else {
        client.PendingWrite.clear();
        (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
    }
} else {
    const TqOpenError error = result.Ok ? TqOpenError::Ok : result.Error;
    client.OpenCompletion = completionState;
    client.OpenSucceeded = result.Ok;
    client.PendingWrite = TqBuildOpenResponse(client.Proto == ListenProto::Socks5, error);
    client.Phase = ClientPhase::WritingOpenResponse;
    (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
}
```

The existing `HandleClientWrite()` path already accepts a completed handle when `PendingWrite` is empty. For failed port-forward opens, ensure it rejects the handle and closes the fd.

In `TimeoutClientOpen()`, skip timeout response for port-forward:

```cpp
if (client.Proto == ListenProto::PortForward) {
    client.PendingWrite.clear();
} else {
    client.PendingWrite = TqBuildOpenResponse(client.Proto == ListenProto::Socks5, TqOpenError::TcpTimeout);
}
```

- [ ] **Step 7: Document trace proto**

In `src/tunnel/tcp_tunnel.h`, change the comment:

```cpp
uint8_t IngressTraceProto{0}; // 1=socks5, 2=http, 3=port-forward (trace)
```

- [ ] **Step 8: Run ingress tests**

Run:

```bash
rtk cmake --build build --target tcpquic_client_ingress_reactor_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: test exits 0.

- [ ] **Step 9: Commit**

```bash
rtk git add src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp src/tunnel/tcp_tunnel.h src/unittest/client_ingress_reactor_test.cpp
rtk git commit -m "feat: open fixed-target port forward tunnels"
```

### Task 5: Client Runtime Wiring And Logs

**Files:**
- Modify: `src/runtime/client_peer_runtime.cpp`
- Modify: `src/runtime/client_peer_runtime.h`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing adapter propagation assertion**

Extend the Task 2 `forward-change` test in `router_runtime_test.cpp` with:

```cpp
if (adapter.LastStartedPeer.PortForwards[0].Listen != "127.0.0.1:15432") return 132;
if (adapter.LastStartedPeer.PortForwards[0].TargetHost != "db.internal") return 133;
if (adapter.LastStartedPeer.PortForwards[0].TargetPort != 5432) return 134;
```

Add helper propagation assertions in `src/unittest/client_peer_runtime_test.cpp`. In `TestPrimaryPeerConfigUsesCliFields()`, set:

```cpp
cfg.PortForwards.push_back(TqPortForwardConfig{"127.0.0.1:15432", "db.internal", 5432});
```

and assert:

```cpp
if (peer.PortForwards.size() != 1) return 17;
if (peer.PortForwards[0].Listen != "127.0.0.1:15432") return 18;
```

In `TestPeerConfigOverlayUsesPeerOverrides()`, set:

```cpp
peer.PortForwards.push_back(TqPortForwardConfig{"127.0.0.1:15433", "cache.internal", 6379});
```

and assert:

```cpp
if (out.PortForwards.size() != 1) return 27;
if (out.PortForwards[0].TargetHost != "cache.internal") return 28;
if (out.PortForwards[0].TargetPort != 6379) return 29;
```

- [ ] **Step 2: Run tests to verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test tcpquic_client_peer_runtime_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_client_peer_runtime_test
```

Expected: assertions fail if helper propagation or metrics are missing.

- [ ] **Step 3: Pass forwards into ingress peer**

In `TqClientPeerRuntime::OpenListenersLocked()`:

```cpp
peer.PortForwards = Config.PortForwards;
```

Add logs after existing SOCKS/HTTP logs:

```cpp
for (const auto& forward : Config.PortForwards) {
    const std::string target = forward.TargetHost + ":" + std::to_string(forward.TargetPort);
    if (LogMode == TqClientPeerLogMode::Primary) {
        std::fprintf(stderr, "tcpquic-proxy: port forward listening on %s -> %s\n",
            forward.Listen.c_str(), target.c_str());
    } else {
        std::fprintf(stderr, "tcpquic-proxy: peer %s port forward listening on %s -> %s\n",
            PeerId.c_str(), forward.Listen.c_str(), target.c_str());
    }
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test tcpquic_client_peer_runtime_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_client_peer_runtime_test
```

Expected: both tests exit 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp src/unittest/router_runtime_test.cpp src/unittest/client_peer_runtime_test.cpp
rtk git commit -m "feat: wire port forwards into client runtime"
```

### Task 6: End-To-End Script Coverage

**Files:**
- Modify: `scripts/test-tcpquic-proxy.sh`

- [ ] **Step 1: Add failing e2e check**

Open `scripts/test-tcpquic-proxy.sh` and add helpers after `wait_for_closed_port()`:

```bash
pick_free_tcp_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}
```

Add a client launcher after `start_client()`:

```bash
start_client_with_forward() {
    local quic_port=$1
    local http_port=$2
    local socks_port=$3
    local forward_port=$4
    local target_port=$5
    local compress_mode=$6
    local log_file=$7

    "$BIN" client \
        --peer "127.0.0.1:${quic_port}" \
        --http-listen "127.0.0.1:${http_port}" \
        --socks-listen "127.0.0.1:${socks_port}" \
        --forward "127.0.0.1:${forward_port}=127.0.0.1:${target_port}" \
        --cert "$TMP_DIR/client.crt" \
        --key "$TMP_DIR/client.key" \
        --ca "$TMP_DIR/ca.crt" \
        --compress "$compress_mode" \
        >"$log_file" 2>&1 &
    echo "$!"
}
```

Add `ECHO_PID=""` next to the other pid globals, and include it in both cleanup loops:

```bash
for pid in "$CLIENT_PID" "$SERVER_PID" "$HTTP_PID" "$ECHO_PID" "$NEG_CLIENT_PID" "$NEG_SERVER_PID"; do
```

Add a local forward check after the existing SOCKS5 happy path:

```bash
FORWARD_TARGET_PORT=$(pick_free_tcp_port)
FORWARD_LISTEN_PORT=$(pick_free_tcp_port)

log "starting forward echo target on 127.0.0.1:${FORWARD_TARGET_PORT}"
python3 - "$FORWARD_TARGET_PORT" >"$TMP_DIR/forward-echo.log" 2>&1 <<'PY' &
import socket
import sys

port = int(sys.argv[1])
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(16)
while True:
    conn, _ = srv.accept()
    data = conn.recv(4096)
    if data:
        conn.sendall(data)
    conn.close()
PY
ECHO_PID=$!
wait_tcp 127.0.0.1 "$FORWARD_TARGET_PORT" "forward echo target"

kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=$(start_client_with_forward 4433 8080 1080 "$FORWARD_LISTEN_PORT" "$FORWARD_TARGET_PORT" off "$TMP_DIR/proxy-client-forward.log")
wait_tcp 127.0.0.1 "$FORWARD_LISTEN_PORT" "port forward listener"

python3 - "$FORWARD_LISTEN_PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
    s.sendall(b"port-forward-ok")
    data = s.recv(4096)
if data != b"port-forward-ok":
    raise SystemExit(f"unexpected echo: {data!r}")
PY
```

- [ ] **Step 2: Run script to verify it fails**

Run:

```bash
rtk ./scripts/test-tcpquic-proxy.sh
```

Expected before implementation: client rejects `--forward` or the forward connection fails.

- [ ] **Step 3: Keep the script in the existing style**

Confirm the e2e block uses existing script variables for the binary, cert paths, server listen, and compression. The new helper must invoke:

```bash
"$BIN" client \
    --peer "127.0.0.1:${quic_port}" \
    --http-listen "127.0.0.1:${http_port}" \
    --socks-listen "127.0.0.1:${socks_port}" \
    --forward "127.0.0.1:${forward_port}=127.0.0.1:${target_port}" \
    --cert "$TMP_DIR/client.crt" \
    --key "$TMP_DIR/client.key" \
    --ca "$TMP_DIR/ca.crt" \
    --compress "$compress_mode"
```

Cleanup must include `ECHO_PID` in the existing `cleanup()` loops, so no extra inline cleanup is needed in the happy path. When restarting the client for the forward check, keep the existing guarded kill/wait pattern:

```bash
kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""
```

- [ ] **Step 4: Run e2e script**

Run:

```bash
rtk ./scripts/test-tcpquic-proxy.sh
```

Expected: script exits 0 and includes the new local forward check.

- [ ] **Step 5: Commit**

```bash
rtk git add scripts/test-tcpquic-proxy.sh
rtk git commit -m "test: cover local port forward e2e"
```

### Task 7: Documentation And Full Verification

**Files:**
- Modify: `README.md`
- Modify: `docs/config_guide.md`
- Modify: `docs/config_guide_cn.md`

- [ ] **Step 1: Update README CLI and examples**

In `README.md`, add to the CLI table:

```markdown
| `--forward` | client | 空 | 本地端口转发，格式 `local=target`，可重复配置 |
```

Add usage example near SOCKS/HTTP examples:

````markdown
### 本地端口转发

```bash
./build/bin/Release/tcpquic-proxy client \
  --peer proxy-b.example.com:443 \
  --ca cert/ca.crt \
  --forward 127.0.0.1:15432=db.internal.example.com:5432

psql -h 127.0.0.1 -p 15432
```
````

Update Admin config JSON examples to include:

```json
"port_forwards": [
  {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
]
```

- [ ] **Step 2: Update config guides**

In `docs/config_guide.md`, add peer field documentation:

```markdown
| `port_forwards` | client peer | `[]` | Local port-forward rules. Each item contains `listen` and `target`. |
```

Add JSON example:

```json
{
  "id": "db",
  "proto_peer": "proxy-b.example.com:443",
  "socks_listen": "",
  "http_listen": "",
  "port_forwards": [
    {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
  ]
}
```

In `docs/config_guide_cn.md`, add peer field documentation:

```markdown
| `port_forwards` | client peer | `[]` | 本地端口转发列表，每项包含 `listen` 和 `target` |
```

Add JSON example:

```json
{
  "id": "db",
  "proto_peer": "proxy-b.example.com:443",
  "socks_listen": "",
  "http_listen": "",
  "port_forwards": [
    {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
  ]
}
```

- [ ] **Step 3: Run focused tests**

Run:

```bash
rtk cmake --build build --target \
  tcpquic_config_router_test \
  tcpquic_router_runtime_test \
  tcpquic_client_ingress_reactor_test \
  -j$(nproc)
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: all commands exit 0.

- [ ] **Step 4: Run e2e**

Run:

```bash
rtk ./scripts/test-tcpquic-proxy.sh
```

Expected: script exits 0.

- [ ] **Step 5: Run formatting/whitespace checks**

Run:

```bash
rtk git diff --check
rtk git status --short
```

Expected: `git diff --check` exits 0. `git status --short` shows only intentional tracked changes plus pre-existing unrelated untracked files.

- [ ] **Step 6: Commit**

```bash
rtk git add README.md docs/config_guide.md docs/config_guide_cn.md
rtk git commit -m "docs: document local port forwarding"
```

## Final Verification Checklist

- [ ] `rtk cmake --build build --target tcpquic-proxy -j$(nproc)` exits 0.
- [ ] `rtk cmake --build build --target tcpquic_config_router_test tcpquic_router_runtime_test tcpquic_client_ingress_reactor_test -j$(nproc)` exits 0.
- [ ] `rtk ./build/bin/Release/tcpquic_config_router_test` exits 0.
- [ ] `rtk ./build/bin/Release/tcpquic_router_runtime_test` exits 0.
- [ ] `rtk ./build/bin/Release/tcpquic_client_ingress_reactor_test` exits 0.
- [ ] `rtk ./scripts/test-tcpquic-proxy.sh` exits 0.
- [ ] `rtk git diff --check` exits 0.

## Spec Coverage Review

- CLI `--forward`: Task 1 and Task 7.
- Router JSON `port_forwards`: Task 1, Task 2, and Task 7.
- Listener lifecycle tied to peer connection state: Task 3 and Task 5 reuse `TqClientPeerRuntime::OpenListenersLocked()`.
- Existing tunnel protocol reuse: Task 4 builds `TunnelRequest` and does not change control protocol.
- Dynamic admin config data-plane change: Task 2 compares `PortForwards` in router runtime.
- Metrics exposure: Task 2.
- Error handling and timeouts: Task 4 ingress tests.
- End-to-end behavior: Task 6.
