# Multi-Interface QUIC Binding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 server 端 `listen` 多地址/通配展开、client 端 peer 多地址和显式 `paths` 本地地址绑定，并明确新 TCP tunnel 在同一 peer 多条 QUIC connection 间的调度语义。

**Architecture:** 新增小型 QUIC 地址解析模块，负责 endpoint list、server listen 展开和 client path 规范化；`QuicServerSession` 从单 listener 改为多 listener；`QuicClientSession` 将 connection slot 稳定映射到 path，并在 `ConnectionStart()` 前设置本地地址。新 TCP tunnel 继续通过 `QuicClientSession::PickConnection()` 在同一 peer 的 connected slot 中 round-robin 分配；单 tunnel 生命周期内不跨 QUIC connection 迁移。

**Tech Stack:** C++17, MsQuic C++ wrapper, CMake, existing hand-written JSON parser, existing unit test executables under `src/unittest`.

---

## File Structure

- Modify: `src/config/config.h`  
  Add `TqQuicPathConfig`, add `std::vector<TqQuicPathConfig> QuicPaths` to `TqPeerConfig` and `TqConfig`.
- Modify: `src/config/config.cpp`  
  Parse `paths`, validate path configuration, allow comma-separated QUIC endpoint lists for `quic_peer`, `proto_peer`, `--peer`, and server `listen`.
- Create: `src/protocol/quic_address.h`  
  Public helper declarations for endpoint parsing, QUIC address conversion, listen expansion, and slot path expansion.
- Create: `src/protocol/quic_address.cpp`  
  Implement parsing and platform local-address enumeration.
- Modify: `src/protocol/quic_session.h`  
  Add path metadata to connection snapshots; replace server listener storage with vectors.
- Modify: `src/protocol/quic_session.cpp`  
  Use `quic_address` helpers, start multiple server listeners, bind client connection slots to local addresses, and keep tunnel selection round-robin over connected slots.
- Modify: `src/runtime/client_peer_runtime.cpp`  
  Log path summary and expose path-aware metrics.
- Modify: `src/runtime/router_runtime.cpp` and `src/runtime/server_admin.cpp`  
  Include path metadata and resolved listener information in JSON snapshots.
- Modify: `src/runtime/server_metrics.h` and `src/runtime/server_metrics.cpp`  
  Carry `ResolvedListens` for server metrics JSON.
- Modify: `src/CMakeLists.txt`  
  Add `protocol/quic_address.cpp` to production and relevant test targets.
- Modify: `src/unittest/config_router_test.cpp`  
  Add config parse and validation coverage.
- Modify: `src/unittest/quic_session_reconnect_test.cpp`  
  Add slot-to-path expansion and retry mapping coverage.
- Modify: `src/unittest/tcp_tunnel_test.cpp`  
  Add API-surface and behavior coverage for tunnel-to-connection selection semantics.
- Modify: `src/unittest/server_admin_test.cpp` and `src/unittest/router_runtime_test.cpp`  
  Add JSON snapshot assertions.
- Modify: `README.md` and `docs/config_guide_cn.md`  
  Document examples and behavior limits.

---

### Task 1: Add Config Model and Parser Coverage

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Test: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing tests for peer lists and paths**

Add cases near existing router config peer tests in `src/unittest/config_router_test.cpp`:

```cpp
{
    TqRouterConfig router;
    std::string err;
    if (!Load(R"json({"version":1,"peers":[{"peer_id":"isp","quic_peer":"36.1.1.10:443,59.1.1.10:443","socks_listen":"127.0.0.1:11001","quic_connections":8}]})json", router, err)) return 901;
    if (router.Peers.size() != 1) return 902;
    if (router.Peers[0].QuicPeer != "36.1.1.10:443,59.1.1.10:443") return 903;
    if (!router.Peers[0].QuicPaths.empty()) return 904;
}
{
    TqRouterConfig router;
    std::string err;
    if (!Load(R"json({"version":1,"peers":[{"peer_id":"isp","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":"10.10.1.2","peer":"36.1.1.10:443","connections":4},{"name":"ctcc","local":"10.20.1.2","peer":"59.1.1.10:443","connections":4}]}]})json", router, err)) return 905;
    if (router.Peers[0].QuicPaths.size() != 2) return 906;
    if (router.Peers[0].QuicPaths[0].Name != "cmcc") return 907;
    if (router.Peers[0].QuicPaths[0].LocalAddress != "10.10.1.2") return 908;
    if (router.Peers[0].QuicPaths[0].Peer != "36.1.1.10:443") return 909;
    if (router.Peers[0].QuicPaths[0].Connections != 4) return 910;
}
{
    TqRouterConfig router;
    std::string err;
    if (Load(R"json({"version":1,"peers":[{"peer_id":"bad","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":"10.10.1.2","peer":"36.1.1.10:443","connections":0}]}]})json", router, err)) return 911;
    if (err.find("path connections out of range") == std::string::npos) return 912;
}
{
    TqRouterConfig router;
    std::string err;
    if (Load(R"json({"version":1,"peers":[{"peer_id":"bad","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":"10.10.1.2","peer":"36.1.1.10:443","connections":4},{"name":"cmcc","local":"10.20.1.2","peer":"59.1.1.10:443","connections":4}]}]})json", router, err)) return 913;
    if (err.find("duplicate path name") == std::string::npos) return 914;
}
```

- [ ] **Step 2: Run config test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: build fails because `QuicPaths` / `TqQuicPathConfig` are not defined, or test fails because parser ignores `paths`.

- [ ] **Step 3: Add config types**

In `src/config/config.h`, add before `TqPeerConfig`:

```cpp
struct TqQuicPathConfig {
    std::string Name;
    std::string LocalAddress;
    std::string Peer;
    uint32_t Connections{0};
};
```

Add fields:

```cpp
struct TqPeerConfig {
    std::string PeerId;
    std::string QuicPeer;
    std::vector<TqQuicPathConfig> QuicPaths;
    std::string SocksListen;
    std::string HttpListen;
    std::vector<TqPortForwardConfig> PortForwards;
    uint32_t QuicConnections{0};
    std::string Compress;
    bool Enabled{true};
};
```

In `TqConfig`, place the new field immediately after the existing `QuicPeer` field:

```cpp
std::string QuicPeer;
std::vector<TqQuicPathConfig> QuicPaths;
```

- [ ] **Step 4: Parse paths in JSON config**

In `JsonParser`, add `ParseQuicPath()` and `ParseQuicPaths()`:

```cpp
bool ParseQuicPath(TqQuicPathConfig& path) {
    if (!Consume('{')) return Error("path must be an object");
    bool hasName = false;
    bool hasLocal = false;
    bool hasPeer = false;
    bool hasConnections = false;
    do {
        std::string key;
        if (!ParseString(key) || !Consume(':')) return Error("malformed path object");
        if (key == "name") {
            hasName = true;
            if (!ParseString(path.Name)) return Error("invalid path.name");
        } else if (key == "local") {
            hasLocal = true;
            if (!ParseString(path.LocalAddress)) return Error("invalid path.local");
        } else if (key == "peer") {
            hasPeer = true;
            if (!ParseString(path.Peer)) return Error("invalid path.peer");
        } else if (key == "connections") {
            hasConnections = true;
            if (!ParseUint32(path.Connections)) return Error("invalid path.connections");
        } else {
            return Error("unknown path key: " + key);
        }
    } while (Consume(','));
    if (!Consume('}')) return Error("malformed path object");
    if (!hasName || !hasLocal || !hasPeer || !hasConnections) return Error("path name, local, peer and connections are required");
    return true;
}

bool ParseQuicPaths(std::vector<TqQuicPathConfig>& paths) {
    if (!Consume('[')) return Error("paths must be an array");
    paths.clear();
    if (Consume(']')) return true;
    do {
        TqQuicPathConfig path;
        if (!ParseQuicPath(path)) return false;
        paths.push_back(std::move(path));
    } while (Consume(','));
    return Consume(']') || Error("malformed paths array");
}
```

Wire it into `ParsePeer()` and `ParseRuntimePeer()`:

```cpp
} else if (key == "paths") {
    if (!ParseQuicPaths(peer.QuicPaths)) return false;
}
```

- [ ] **Step 5: Validate paths**

Add helper near router validation:

```cpp
bool IsSingleIpAddress(const std::string& value) {
    QUIC_ADDR addr{};
    return !value.empty() && value.find(':') == std::string::npos
        ? QuicAddrFromString(value.c_str(), 0, &addr)
        : (!value.empty() && value.front() != '[' && QuicAddrFromString(value.c_str(), 0, &addr));
}
```

If `config.cpp` cannot include MsQuic address helpers without widening dependencies, use a local textual check for first pass:

```cpp
bool LooksLikeLocalBindIp(const std::string& value) {
    return !value.empty() && value.find(',') == std::string::npos && value.find(':') == std::string::npos;
}
```

In `TqValidateRouterConfig()`:

```cpp
uint32_t totalPathConnections = 0;
std::set<std::string> pathNames;
for (const auto& path : peer.QuicPaths) {
    if (path.Name.empty()) { err = "path name is required for " + peer.PeerId; return false; }
    if (!pathNames.insert(path.Name).second) { err = "duplicate path name for " + peer.PeerId + ": " + path.Name; return false; }
    if (path.LocalAddress.empty() || path.LocalAddress.find(',') != std::string::npos) { err = "invalid path local for " + peer.PeerId; return false; }
    if (!IsHostPort(path.Peer)) { err = "invalid path peer for " + peer.PeerId; return false; }
    if (path.Connections == 0 || path.Connections > 128) { err = "path connections out of range for " + peer.PeerId; return false; }
    if (totalPathConnections > 128 - path.Connections) { err = "path connections out of range for " + peer.PeerId; return false; }
    totalPathConnections += path.Connections;
}
if (!peer.QuicPaths.empty() && totalPathConnections == 0) { err = "path connections out of range for " + peer.PeerId; return false; }
```

Change `quic_peer` validation so path mode does not require it:

```cpp
if (peer.QuicPaths.empty()) {
    if (!IsHostPortList(peer.QuicPeer)) { err = "invalid quic_peer for " + peer.PeerId; return false; }
}
```

- [ ] **Step 6: Run tests and commit**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: PASS.

Commit:

```bash
rtk git add src/config/config.h src/config/config.cpp src/unittest/config_router_test.cpp
rtk git commit -m "feat: add QUIC path config model"
```

---

### Task 2: Add QUIC Address Helper

**Files:**
- Create: `src/protocol/quic_address.h`
- Create: `src/protocol/quic_address.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing helper tests**

Add test cases in `src/unittest/config_router_test.cpp` under a new function `TestQuicAddressHelpers()`:

```cpp
static int TestQuicAddressHelpers() {
    std::vector<TqEndpoint> endpoints;
    std::string err;
    if (!TqParseEndpointList("36.1.1.10:443,59.1.1.10:443", endpoints, err)) return 920;
    if (endpoints.size() != 2) return 921;
    if (endpoints[0].Host != "36.1.1.10" || endpoints[0].Port != 443) return 922;
    if (endpoints[1].Host != "59.1.1.10" || endpoints[1].Port != 443) return 923;
    endpoints.clear();
    if (!TqParseEndpointList("[2001:db8::1]:443,[2001:db8::2]:443", endpoints, err)) return 924;
    if (endpoints.size() != 2) return 925;
    if (TqParseEndpointList("2001:db8::1:443", endpoints, err)) return 926;
    if (err.find("invalid endpoint") == std::string::npos) return 927;
    return 0;
}
```

Call it from `main()`:

```cpp
if (int rc = TestQuicAddressHelpers()) return rc;
```

Include the new header:

```cpp
#include "quic_address.h"
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j
```

Expected: compile fails because `quic_address.h` does not exist.

- [ ] **Step 3: Create helper header**

Create `src/protocol/quic_address.h`:

```cpp
#pragma once

#include "msquic.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct TqEndpoint {
    std::string Host;
    uint16_t Port{0};
};

struct TqResolvedListen {
    std::string Text;
    QUIC_ADDR Address{};
};

bool TqParseEndpoint(const std::string& value, TqEndpoint& endpoint);
bool TqParseEndpointList(const std::string& value, std::vector<TqEndpoint>& endpoints, std::string& err);
bool TqMakeQuicAddr(const TqEndpoint& endpoint, QUIC_ADDR& address);
bool TqResolveServerListenList(const std::string& value, std::vector<TqResolvedListen>& listens, std::string& err);
std::string TqFormatEndpoint(const TqEndpoint& endpoint);
```

- [ ] **Step 4: Implement endpoint parsing**

Create `src/protocol/quic_address.cpp` with endpoint parser:

```cpp
#include "quic_address.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

namespace {

bool ParsePort(const std::string& value, uint16_t& port) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

}

bool TqParseEndpoint(const std::string& value, TqEndpoint& endpoint) {
    endpoint = TqEndpoint{};
    if (value.empty()) return false;

    std::string host;
    std::string portText;
    if (value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close + 1 >= value.size() || value[close + 1] != ':') return false;
        host = value.substr(1, close - 1);
        portText = value.substr(close + 2);
    } else {
        const size_t colon = value.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) return false;
        if (value.find(':', colon + 1) != std::string::npos) return false;
        host = value.substr(0, colon);
        portText = value.substr(colon + 1);
    }

    uint16_t port = 0;
    if (host.empty() || !ParsePort(portText, port)) return false;
    endpoint.Host = std::move(host);
    endpoint.Port = port;
    return true;
}

bool TqParseEndpointList(const std::string& value, std::vector<TqEndpoint>& endpoints, std::string& err) {
    endpoints.clear();
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        std::string item = value.substr(start, comma - start);
        const size_t begin = item.find_first_not_of(" \t");
        const size_t end = item.find_last_not_of(" \t");
        if (begin == std::string::npos) {
            err = "invalid endpoint list";
            return false;
        }
        item = item.substr(begin, end - begin + 1);
        TqEndpoint endpoint;
        if (!TqParseEndpoint(item, endpoint)) {
            err = "invalid endpoint: " + item;
            return false;
        }
        endpoints.push_back(std::move(endpoint));
        start = comma + 1;
    }
    return !endpoints.empty();
}

std::string TqFormatEndpoint(const TqEndpoint& endpoint) {
    if (endpoint.Host.find(':') != std::string::npos) {
        return "[" + endpoint.Host + "]:" + std::to_string(endpoint.Port);
    }
    return endpoint.Host + ":" + std::to_string(endpoint.Port);
}
```

- [ ] **Step 5: Add QUIC_ADDR conversion and initial listen resolver**

Add:

```cpp
bool TqMakeQuicAddr(const TqEndpoint& endpoint, QUIC_ADDR& address) {
    if (endpoint.Host == "*") {
        std::memset(&address, 0, sizeof(address));
        QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(&address, endpoint.Port);
        return true;
    }
    return QuicAddrFromString(endpoint.Host.c_str(), endpoint.Port, &address);
}

bool TqResolveServerListenList(const std::string& value, std::vector<TqResolvedListen>& listens, std::string& err) {
    std::vector<TqEndpoint> endpoints;
    if (!TqParseEndpointList(value, endpoints, err)) return false;
    listens.clear();
    std::set<std::string> seen;
    for (const auto& endpoint : endpoints) {
        QUIC_ADDR address{};
        if (!TqMakeQuicAddr(endpoint, address)) {
            err = "invalid listen address: " + TqFormatEndpoint(endpoint);
            return false;
        }
        const std::string text = TqFormatEndpoint(endpoint);
        if (!seen.insert(text).second) {
            err = "duplicate listen address: " + text;
            return false;
        }
        listens.push_back(TqResolvedListen{text, address});
    }
    return !listens.empty();
}
```

Task 3 replaces the direct wildcard entry with interface expansion.

- [ ] **Step 6: Wire CMake and run tests**

Add `protocol/quic_address.cpp` to the main source list and to `tcpquic_config_router_test` sources in `src/CMakeLists.txt`.

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: PASS.

Commit:

```bash
rtk git add src/protocol/quic_address.h src/protocol/quic_address.cpp src/CMakeLists.txt src/unittest/config_router_test.cpp
rtk git commit -m "feat: add QUIC endpoint parsing helpers"
```

---

### Task 3: Implement Server Listen Expansion

**Files:**
- Modify: `src/protocol/quic_address.cpp`
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/runtime/server_metrics.h`
- Modify: `src/runtime/server_metrics.cpp`
- Test: `src/unittest/server_admin_test.cpp`

- [ ] **Step 1: Add server metrics test**

In `src/unittest/server_admin_test.cpp`, add an assertion for resolved listens JSON after constructing a metrics object:

```cpp
metrics.Listen = "0.0.0.0:443";
metrics.ResolvedListens = {"36.1.1.10:443", "59.1.1.10:443"};
const std::string health = TqServerHealthJson(metrics, 10);
if (health.find("\"resolved_listens\":[\"36.1.1.10:443\",\"59.1.1.10:443\"]") == std::string::npos) return 301;
```

- [ ] **Step 2: Run server admin test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test -j
```

Expected: compile fails because `ResolvedListens` is not defined.

- [ ] **Step 3: Add resolved listen metrics field**

In `src/runtime/server_metrics.h`, add:

```cpp
std::vector<std::string> ResolvedListens;
```

In `src/runtime/server_metrics.cpp`, emit:

```cpp
out << ",\"resolved_listens\":[";
for (size_t i = 0; i < metrics.ResolvedListens.size(); ++i) {
    if (i != 0) out << ',';
    TqAppendJsonStringValue(out, metrics.ResolvedListens[i]);
}
out << "]";
```

- [ ] **Step 4: Add platform enumeration**

In `src/protocol/quic_address.cpp`, implement local address enumeration.

POSIX branch:

```cpp
#if !defined(_WIN32)
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>

static void TqEnumerateLocalAddressTexts(int family, std::vector<std::string>& out) {
    ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return;
    std::set<std::string> seen;
    for (ifaddrs* it = addrs; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr) continue;
        if ((it->ifa_flags & IFF_UP) == 0) continue;
        if ((it->ifa_flags & IFF_LOOPBACK) != 0) continue;
        if (it->ifa_addr->sa_family != family) continue;
        char buffer[INET6_ADDRSTRLEN]{};
        if (family == AF_INET) {
            const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
            const uint32_t host = ntohl(addr->sin_addr.s_addr);
            if (host == 0 || (host >> 24) == 127 || (host >= 0xE0000000u)) continue;
            if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) == nullptr) continue;
        } else {
            const auto* addr = reinterpret_cast<const sockaddr_in6*>(it->ifa_addr);
            if (IN6_IS_ADDR_UNSPECIFIED(&addr->sin6_addr) ||
                IN6_IS_ADDR_LOOPBACK(&addr->sin6_addr) ||
                IN6_IS_ADDR_MULTICAST(&addr->sin6_addr) ||
                IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) continue;
            if (inet_ntop(AF_INET6, &addr->sin6_addr, buffer, sizeof(buffer)) == nullptr) continue;
        }
        if (seen.insert(buffer).second) out.push_back(buffer);
    }
    freeifaddrs(addrs);
}
#endif
```

Windows branch uses `GetAdaptersAddresses()` with the same filters.

- [ ] **Step 5: Expand wildcard listens**

In `TqResolveServerListenList()`:

```cpp
if (endpoint.Host == "0.0.0.0" || endpoint.Host == "*") {
    std::vector<std::string> hosts;
    TqEnumerateLocalAddressTexts(AF_INET, hosts);
    if (hosts.empty()) {
        err = "listen " + TqFormatEndpoint(endpoint) + " expanded to no usable local addresses";
        return false;
    }
    for (const auto& host : hosts) appendResolved(TqEndpoint{host, endpoint.Port});
    continue;
}
if (endpoint.Host == "::") {
    std::vector<std::string> hosts;
    TqEnumerateLocalAddressTexts(AF_INET6, hosts);
    if (hosts.empty()) {
        err = "listen [::]:" + std::to_string(endpoint.Port) + " expanded to no usable local addresses";
        return false;
    }
    for (const auto& host : hosts) appendResolved(TqEndpoint{host, endpoint.Port});
    continue;
}
appendResolved(endpoint);
```

- [ ] **Step 6: Convert server session to multiple listeners**

In `src/protocol/quic_session.h`, replace:

```cpp
std::unique_ptr<MsQuicListener> Listener;
```

with:

```cpp
std::vector<std::unique_ptr<MsQuicListener>> Listeners;
std::vector<std::string> ResolvedListens;
```

In `QuicServerSession::Start()`:

```cpp
std::vector<TqResolvedListen> listens;
std::string listenErr;
if (!TqResolveServerListenList(cfg.QuicListen, listens, listenErr)) {
    std::fprintf(stderr, "Invalid --quic-listen address: %s\n", listenErr.c_str());
    return false;
}
for (const auto& listen : listens) {
    auto listener = std::make_unique<MsQuicListener>(*Registration, CleanUpManual, QuicServerSession::ListenerCallback, this);
    if (!listener || !listener->IsValid()) {
        std::fprintf(stderr, "ListenerOpen failed for %s, 0x%x\n", listen.Text.c_str(), listener ? listener->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        Stop();
        return false;
    }
    const QUIC_STATUS status = listener->Start(alpn, &listen.Address);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "ListenerStart failed for %s, 0x%x\n", listen.Text.c_str(), status);
        Stop();
        return false;
    }
    ResolvedListens.push_back(listen.Text);
    Listeners.push_back(std::move(listener));
}
```

In `Stop()`, move and clear `Listeners`.

- [ ] **Step 7: Run tests and commit**

Run:

```bash
rtk cmake --build build --target tcpquic_server_admin_test tcpquic_tunnel_test -j
rtk ./build/bin/Release/tcpquic_server_admin_test
rtk ./build/bin/Release/tcpquic_tunnel_test
```

Expected: PASS.

Commit:

```bash
rtk git add src/protocol/quic_address.cpp src/protocol/quic_session.h src/protocol/quic_session.cpp src/runtime/server_metrics.h src/runtime/server_metrics.cpp src/unittest/server_admin_test.cpp
rtk git commit -m "feat: expand server QUIC listen addresses"
```

---

### Task 4: Implement Client Path Expansion and Local Binding

**Files:**
- Modify: `src/protocol/quic_address.h`
- Modify: `src/protocol/quic_address.cpp`
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Test: `src/unittest/quic_session_reconnect_test.cpp`

- [ ] **Step 1: Write failing slot path test**

In `src/unittest/quic_session_reconnect_test.cpp`, add under `TQ_UNIT_TESTING`:

```cpp
static int TestQuicPathSlotExpansion() {
    TqConfig cfg;
    cfg.QuicCa = "ca.crt";
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});
    std::vector<TqClientSlotPath> slots;
    std::string err;
    if (!TqBuildClientSlotPaths(cfg, slots, err)) return 401;
    if (slots.size() != 3) return 402;
    if (slots[0].Name != "cmcc" || slots[0].LocalAddress != "10.10.1.2" || slots[0].PeerHost != "36.1.1.10") return 403;
    if (slots[1].Name != "cmcc") return 404;
    if (slots[2].Name != "ctcc" || slots[2].PeerHost != "59.1.1.10") return 405;
    return 0;
}
```

Call it from `main()`.

- [ ] **Step 2: Run reconnect test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j
```

Expected: compile fails because `TqClientSlotPath` / `TqBuildClientSlotPaths` are undefined.

- [ ] **Step 3: Add slot path helper**

In `src/protocol/quic_address.h`:

```cpp
#include "config.h"

struct TqClientSlotPath {
    std::string Name;
    std::string LocalAddress;
    std::string PeerHost;
    uint16_t PeerPort{0};
    std::string PeerText;
};

bool TqBuildClientSlotPaths(const TqConfig& cfg, std::vector<TqClientSlotPath>& slots, std::string& err);
```

In `src/protocol/quic_address.cpp`:

```cpp
bool TqBuildClientSlotPaths(const TqConfig& cfg, std::vector<TqClientSlotPath>& slots, std::string& err) {
    slots.clear();
    if (!cfg.QuicPaths.empty()) {
        uint32_t total = 0;
        for (const auto& path : cfg.QuicPaths) {
            TqEndpoint endpoint;
            if (!TqParseEndpoint(path.Peer, endpoint)) {
                err = "invalid path peer: " + path.Peer;
                return false;
            }
            for (uint32_t i = 0; i < path.Connections; ++i) {
                slots.push_back(TqClientSlotPath{path.Name, path.LocalAddress, endpoint.Host, endpoint.Port, TqFormatEndpoint(endpoint)});
            }
            total += path.Connections;
        }
        return total > 0;
    }

    std::vector<TqEndpoint> peers;
    if (!TqParseEndpointList(cfg.QuicPeer, peers, err)) return false;
    for (uint32_t i = 0; i < cfg.QuicConnections; ++i) {
        const auto& endpoint = peers[i % peers.size()];
        slots.push_back(TqClientSlotPath{"default", "", endpoint.Host, endpoint.Port, TqFormatEndpoint(endpoint)});
    }
    return !slots.empty();
}
```

- [ ] **Step 4: Store path metadata in client session**

In `QuicClientSession`, replace `PeerHost`, `PeerPort`, and `Config.QuicConnections`-only slot sizing with:

```cpp
std::vector<TqClientSlotPath> SlotPaths;
```

In `Start()`:

```cpp
std::vector<TqClientSlotPath> slotPaths;
std::string pathErr;
if (!TqBuildClientSlotPaths(cfg, slotPaths, pathErr)) {
    std::fprintf(stderr, "Invalid QUIC peer/path config: %s\n", pathErr.c_str());
    return false;
}
SlotPaths = std::move(slotPaths);
State->Slots.resize(SlotPaths.size());
Config.QuicConnections = static_cast<uint32_t>(SlotPaths.size());
```

Keep the existing handler, scheduler, registration, and configuration initialization logic around this block unchanged.

- [ ] **Step 5: Bind local address before ConnectionStart**

In `StartSlot()` before calling `connectionToStart->Start`:

```cpp
const TqClientSlotPath path = SlotPaths[index];
if (!path.LocalAddress.empty()) {
    TqEndpoint localEndpoint{path.LocalAddress, 0};
    QUIC_ADDR localAddr{};
    if (!TqMakeQuicAddr(localEndpoint, localAddr)) {
        slot.LastError = "invalid local address for path " + path.Name + ": " + path.LocalAddress;
        scheduleRetry = true;
    } else {
        const QUIC_STATUS bindStatus = connectionToStart->SetLocalAddr(localAddr);
        if (QUIC_FAILED(bindStatus)) {
            slot.LastError = TqQuicStatusText(("SetLocalAddr " + path.Name).c_str(), bindStatus);
            scheduleRetry = true;
        }
    }
}
```

Then start with the path peer:

```cpp
const QUIC_STATUS startStatus =
    connectionToStart->Start(*Configuration, path.PeerHost.c_str(), path.PeerPort);
```

- [ ] **Step 6: Run tests and commit**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test tcpquic_tunnel_test -j
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build/bin/Release/tcpquic_tunnel_test
```

Expected: PASS.

Commit:

```bash
rtk git add src/protocol/quic_address.h src/protocol/quic_address.cpp src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp
rtk git commit -m "feat: bind client QUIC slots to configured paths"
```

---

### Task 5: Add Path Metadata to Admin and Runtime Output

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/runtime/client_peer_runtime.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing JSON test**

In `src/unittest/router_runtime_test.cpp`, extend connection JSON assertions:

```cpp
TqConnectionSnapshot pathConn;
pathConn.ConnectionId = "conn-0";
pathConn.SlotIndex = 0;
pathConn.Generation = 1;
pathConn.Connected = true;
pathConn.State = "connected";
pathConn.PathName = "cmcc";
pathConn.LocalAddress = "10.10.1.2";
pathConn.PeerAddress = "36.1.1.10:443";
const std::string body = ConnectionsJson({pathConn});
if (body.find("\"path\":\"cmcc\"") == std::string::npos) return 930;
if (body.find("\"local\":\"10.10.1.2\"") == std::string::npos) return 931;
if (body.find("\"peer\":\"36.1.1.10:443\"") == std::string::npos) return 932;
```

- [ ] **Step 2: Run router runtime test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j
```

Expected: compile fails because snapshot fields do not exist.

- [ ] **Step 3: Extend snapshot type**

In `src/protocol/quic_session.h`:

```cpp
struct TqConnectionSnapshot {
    std::string ConnectionId;
    uint32_t SlotIndex{0};
    uint64_t Generation{0};
    bool Connected{false};
    bool RetryScheduled{false};
    std::string State;
    uint64_t ActiveTunnels{0};
    uint64_t TotalTunnels{0};
    std::string LastError;
    std::string PathName;
    std::string LocalAddress;
    std::string PeerAddress;
};
```

- [ ] **Step 4: Populate snapshot**

In `QuicClientSession::SnapshotConnections()`:

```cpp
if (i < SlotPaths.size()) {
    snapshot.PathName = SlotPaths[i].Name;
    snapshot.LocalAddress = SlotPaths[i].LocalAddress;
    snapshot.PeerAddress = SlotPaths[i].PeerText;
}
```

- [ ] **Step 5: Emit JSON**

In `AppendConnectionJson()` inside `src/runtime/router_runtime.cpp`:

```cpp
AppendJsonString(out, "path", snapshot.PathName);
AppendJsonString(out, "local", snapshot.LocalAddress);
AppendJsonString(out, "peer", snapshot.PeerAddress);
```

- [ ] **Step 6: Add startup path logs**

In `src/runtime/client_peer_runtime.cpp`, after peer config finalization:

```cpp
for (const auto& path : peerCfg.QuicPaths) {
    std::fprintf(stderr,
        "tcpquic-proxy: peer %s path %s local %s -> %s (%u connections)\n",
        peer.PeerId.c_str(),
        path.Name.c_str(),
        path.LocalAddress.c_str(),
        path.Peer.c_str(),
        path.Connections);
}
```

For non-path peer lists, log the raw peer list:

```cpp
std::fprintf(stderr, "tcpquic-proxy: peer %s QUIC peers %s (%u connections)\n",
    peer.PeerId.c_str(), peerCfg.QuicPeer.c_str(), metrics.ConnectionCount);
```

- [ ] **Step 7: Run tests and commit**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test tcpquic_client_peer_runtime_test -j
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_client_peer_runtime_test
```

Expected: PASS.

Commit:

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/runtime/router_runtime.cpp src/runtime/client_peer_runtime.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: expose QUIC path metadata"
```

---

### Task 6: Document and Test TCP Tunnel Scheduling Across Connections

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/unittest/quic_session_reconnect_test.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`

- [ ] **Step 1: Write failing round-robin selection test**

In `src/unittest/quic_session_reconnect_test.cpp`, add a unit-test-only hook that can mark specific slots as connected and then verify `PickConnection()` skips disconnected slots while preserving round-robin behavior.

Add test code:

```cpp
static int TestPickConnectionRoundRobinSkipsDisconnectedSlots() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(4);
    session.MarkSlotConnectedForTest(0, reinterpret_cast<MsQuicConnection*>(0x1000));
    session.MarkSlotConnectedForTest(2, reinterpret_cast<MsQuicConnection*>(0x3000));

    if (session.PickConnectionForTest() != reinterpret_cast<MsQuicConnection*>(0x1000)) return 501;
    if (session.PickConnectionForTest() != reinterpret_cast<MsQuicConnection*>(0x3000)) return 502;
    if (session.PickConnectionForTest() != reinterpret_cast<MsQuicConnection*>(0x1000)) return 503;

    session.MarkSlotDisconnectedForTest(0);
    if (session.PickConnectionForTest() != reinterpret_cast<MsQuicConnection*>(0x3000)) return 504;
    if (session.PickConnectionForTest() != reinterpret_cast<MsQuicConnection*>(0x3000)) return 505;
    return 0;
}
```

Call it from `main()`:

```cpp
if (int rc = TestPickConnectionRoundRobinSkipsDisconnectedSlots()) return rc;
```

- [ ] **Step 2: Run reconnect test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j
```

Expected: compile fails because `MarkSlotConnectedForTest`, `MarkSlotDisconnectedForTest`, and `PickConnectionForTest` do not exist.

- [ ] **Step 3: Add test-only selection hooks**

In `src/protocol/quic_session.h`, under `#if defined(TQ_UNIT_TESTING)`:

```cpp
void MarkSlotConnectedForTest(size_t index, MsQuicConnection* connection);
void MarkSlotDisconnectedForTest(size_t index);
MsQuicConnection* PickConnectionForTest();
```

In `src/protocol/quic_session.h`, add a test-only pointer to `ConnectionSlot`:

```cpp
struct ConnectionSlot {
    ClientConnContext* Context{nullptr};
    std::unique_ptr<MsQuicConnection> Connection;
#if defined(TQ_UNIT_TESTING)
    MsQuicConnection* TestConnectionOverride{nullptr};
#endif
    std::string ConnectionId;
    uint64_t Generation{0};
    bool Connected{false};
    bool RetryScheduled{false};
    std::string LastError;
};
```

In `QuicClientSession::PickConnection()`, return the test pointer only when it is set:

```cpp
if (slot.Connected) {
#if defined(TQ_UNIT_TESTING)
    if (slot.TestConnectionOverride != nullptr) {
        return slot.TestConnectionOverride;
    }
#endif
    if (slot.Connection && slot.Connection->IsValid()) {
        return slot.Connection.get();
    }
}
```

Apply the same test-only override branch to `PickConnectionAt()` and `PickConnectionFrom()`, so all connection-picking APIs share the same behavior in tests.

In `src/protocol/quic_session.cpp`, implement the hooks:

```cpp
#if defined(TQ_UNIT_TESTING)
void QuicClientSession::MarkSlotConnectedForTest(size_t index, MsQuicConnection* connection) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) {
        State->Slots.resize(index + 1);
        for (size_t i = 0; i < State->Slots.size(); ++i) {
            if (State->Slots[i].ConnectionId.empty()) {
                State->Slots[i].ConnectionId = MakeConnectionId(i);
            }
        }
    }
    auto& slot = State->Slots[index];
    slot.Connected = true;
    slot.TestConnectionOverride = connection;
}

void QuicClientSession::MarkSlotDisconnectedForTest(size_t index) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) return;
    auto& slot = State->Slots[index];
    slot.Connected = false;
    slot.TestConnectionOverride = nullptr;
}

MsQuicConnection* QuicClientSession::PickConnectionForTest() {
    return PickConnection();
}
#endif
```

- [ ] **Step 4: Add API surface test for tunnel scheduling contract**

In `src/unittest/tcp_tunnel_test.cpp`, extend `TestQuicClientSessionReconnectApiSurface()`:

```cpp
(void)static_cast<MsQuicConnection* (QuicClientSession::*)()>(&QuicClientSession::PickConnection);
```

Add a comment near the assertion:

```cpp
// Client ingress opens one TCP tunnel on one selected QUIC connection. The
// scheduler contract is connection-level selection, not per-tunnel multipath.
```

- [ ] **Step 5: Run tests and commit**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test tcpquic_tunnel_test -j
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build/bin/Release/tcpquic_tunnel_test
```

Expected: PASS.

Commit:

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "test: cover QUIC connection selection for tunnels"
```

---

### Task 7: End-to-End Validation Scripts and Documentation

**Files:**
- Modify: `README.md`
- Modify: `docs/config_guide_cn.md`
- Modify: `scripts/test-tcpquic-proxy.sh`
- Create: `scripts/test-tcpquic-multi-interface-paths.sh`

- [ ] **Step 1: Add README examples**

Add a section near current QUIC connection pool docs:

````markdown
### 多网口 QUIC 绑定

server 端仍使用 `--listen`。当配置 `0.0.0.0:443` 时，程序会枚举本机非本地 IPv4 地址，并对每个地址分别启动 MsQuic listener：

```bash
tcpquic-proxy server --listen 0.0.0.0:443 --cert server.crt --key server.key --allow-targets 10.0.0.0/8
```

client 未配置 `paths` 时，`--peer` 支持地址列表，出口由系统路由决定：

```bash
tcpquic-proxy client --peer 36.1.1.10:443,59.1.1.10:443 --connections 8 --ca ca.crt
```

client 配置 `paths` 时，显式绑定本地网口和 server 网口：

```json
{
  "version": 1,
  "peers": [
    {
      "peer_id": "server-b",
      "paths": [
        { "name": "cmcc", "local": "10.10.1.2", "peer": "36.1.1.10:443", "connections": 4 },
        { "name": "ctcc", "local": "10.20.1.2", "peer": "59.1.1.10:443", "connections": 4 }
      ],
      "socks_listen": "127.0.0.1:1080"
    }
  ]
}
```

配置 `paths` 后，peer 级 `quic_peer` 和 `quic_connections` 不参与连接槽分配。

TCP 接入流量的调度粒度是 tunnel。一个 TCP 连接会绑定到一个 QUIC connection slot，生命周期内不跨 slot 迁移；多个并发 TCP 连接会在同一 peer 的已连接 slot 中 round-robin 分摊。因此，`connections` 同时决定 path 上的 QUIC 连接数和默认 tunnel 分配权重。
````

- [ ] **Step 2: Add smoke script**

Create `scripts/test-tcpquic-multi-interface-paths.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${TCPQUIC_PROXY_BIN:-$ROOT/build/bin/Release/tcpquic-proxy}"

if [[ ! -x "$BIN" ]]; then
  echo "missing tcpquic-proxy binary: $BIN" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/client.json" <<'JSON'
{
  "version": 1,
  "peers": [
    {
      "peer_id": "loopback-paths",
      "paths": [
        { "name": "lo-a", "local": "127.0.0.1", "peer": "127.0.0.1:14443", "connections": 1 }
      ],
      "socks_listen": "127.0.0.1:11080"
    }
  ]
}
JSON

"$BIN" client --client-config "$tmp/client.json" --ca "$tmp/ca.crt" --usage >/dev/null
echo "multi-interface path config smoke passed"
```

- [ ] **Step 3: Run docs/script validation**

Run:

```bash
rtk bash -n scripts/test-tcpquic-multi-interface-paths.sh
rtk cmake --build build --target tcpquic_config_router_test -j
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
rtk git add README.md docs/config_guide_cn.md scripts/test-tcpquic-multi-interface-paths.sh
rtk git commit -m "docs: document multi-interface QUIC paths"
```

---

### Task 8: Full Verification and Release Gate

**Files:**
- No source files expected unless verification finds a defect.

- [ ] **Step 1: Build core test targets**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_quic_session_reconnect_test tcpquic_router_runtime_test tcpquic_server_admin_test tcpquic_tunnel_test -j
```

Expected: all targets build successfully.

- [ ] **Step 2: Run focused unit tests**

Run:

```bash
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_server_admin_test
rtk ./build/bin/Release/tcpquic_tunnel_test
```

Expected: all commands exit 0.

- [ ] **Step 3: Run existing proxy smoke test**

Run:

```bash
rtk ./scripts/test-tcpquic-proxy.sh
```

Expected: script exits 0.

- [ ] **Step 4: Run path smoke script**

Run:

```bash
rtk bash -n scripts/test-tcpquic-multi-interface-paths.sh
```

Expected: script syntax is valid.

- [ ] **Step 5: Manual multi-interface validation on Linux host**

Run on a host where adding loopback aliases is acceptable:

```bash
sudo ip addr add 127.10.10.1/8 dev lo
sudo ip addr add 127.20.20.1/8 dev lo
```

Start server:

```bash
rtk ./build/bin/Release/tcpquic-proxy server --listen 127.10.10.1:14443,127.20.20.1:14443 --cert server.crt --key server.key --allow-targets 127.0.0.0/8
```

Start client with paths:

```json
{
  "version": 1,
  "peers": [
    {
      "peer_id": "loopback-paths",
      "paths": [
        { "name": "a", "local": "127.10.10.1", "peer": "127.10.10.1:14443", "connections": 1 },
        { "name": "b", "local": "127.20.20.1", "peer": "127.20.20.1:14443", "connections": 1 }
      ],
      "socks_listen": "127.0.0.1:11080"
    }
  ]
}
```

Expected: admin connection snapshot shows path `a` local `127.10.10.1` peer `127.10.10.1:14443`, and path `b` local `127.20.20.1` peer `127.20.20.1:14443`.

- [ ] **Step 6: Commit verification fixes if any**

If verification required fixes:

```bash
rtk git add <changed-files>
rtk git commit -m "fix: stabilize multi-interface QUIC binding"
```

If no fixes were required, do not create an empty commit.

---

## Self-Review

- Spec coverage: server `listen` list and wildcard expansion are covered by Tasks 2 and 3; client peer list and path mode are covered by Tasks 1 and 4; admin visibility is covered by Task 5; TCP tunnel scheduling semantics are covered by Task 6; docs and validation are covered by Tasks 7 and 8.
- Placeholder scan: the plan uses concrete files, commands, snippets, expected results, and commit boundaries.
- Type consistency: `TqQuicPathConfig`, `TqClientSlotPath`, `TqEndpoint`, `TqResolvedListen`, and snapshot fields are introduced before later tasks reference them.
