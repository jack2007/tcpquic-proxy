# Admin API Missing Interfaces Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐 `docs/admin-api/interface.md` 中列出的 Admin API 缺口，优先实现只读配置/状态接口、server tunnel 单项控制、relay capability 查询，并保持现有接口兼容。

**Architecture:** 新增独立 Admin config/status serializer，避免继续扩大 `router_runtime.cpp`；`/api/v1/admin` 在 `TqAdminHttpServer` 层处理，其余 runtime 相关接口由 client/server handler 通过只读 `TqConfig` snapshot 输出。新增接口使用结构化错误，旧接口保持原响应格式。

**Tech Stack:** C++17、cpp-httplib、现有 `TqJsonResponse`/Admin handler、现有 CMake unittest targets、shell/curl/k6 可选验证。

---

## 文件结构

- Modify: `src/runtime/admin_http.h`  
  增加 Admin role/status metadata，暴露 `StatusJson()` 或等价内部 helper。
- Modify: `src/runtime/admin_http.cpp`  
  扩展 `/api/v1` 白名单；在 dispatch 中优先处理 `/api/v1/admin`；补充结构化错误 helper。
- Create: `src/runtime/admin_config.h`  
  声明 runtime config/admin diagnostics/server config JSON 序列化函数。
- Create: `src/runtime/admin_config.cpp`  
  实现 `TqConfig` 到公开 schema 的 JSON 序列化和脱敏。
- Modify: `src/runtime/router_runtime.h` / `src/runtime/router_runtime.cpp`  
  让 router admin handler 支持 `/runtime/config`、`/client/config`、`/peers/{id}/config`、`/diagnostics`、relay active relay 查询。
- Modify: `src/runtime/server_admin.h` / `src/runtime/server_admin.cpp`  
  支持 `/server/config`、`/server/tunnels/{id}`、`:abort`、`:drain`、`/diagnostics`。
- Modify: `src/main.cpp`  
  构造 admin handler 时传入 frozen `TqConfig` snapshot 和 Admin metadata。
- Modify: `src/runtime/relay_metrics.h` / `src/runtime/relay_metrics.cpp`  
  增加 active relay JSON/capability 输出 helper，复用已有 `TqSnapshotActiveRelays()`。
- Modify: `src/CMakeLists.txt`  
  把新 `admin_config.cpp` 加入相关 binaries/tests。
- Modify: `src/unittest/admin_http_test.cpp`  
  覆盖新白名单、鉴权和 `/api/v1/admin`。
- Modify: `src/unittest/router_runtime_test.cpp`  
  覆盖 client/router 新接口和兼容性。
- Modify: `src/unittest/server_admin_test.cpp`  
  覆盖 server 新接口。
- Modify: `docs/admin-api/interface.md`  
  将实现后的接口从“缺少”章节迁入正式接口表。

## Task 1: Admin Config Serializer Skeleton

**Files:**
- Create: `src/runtime/admin_config.h`
- Create: `src/runtime/admin_config.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing serializer coverage**

在 `src/unittest/router_runtime_test.cpp` 增加测试 helper 级断言，先直接 include `admin_config.h`。

```cpp
#include "admin_config.h"
```

在 `main()` 中增加：

```cpp
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Client;
        cfg.ConfigPath = "client.json";
        cfg.AdminListen = "127.0.0.1:18080";
        cfg.AdminTokenFile = "/tmp/token.json";
        cfg.AdminThreads = 2;
        cfg.QuicCa = "certs/ca.crt";
        cfg.QuicProfile = TqQuicProfile::MaxThroughput;
        cfg.QuicDisable1RttEncryption = true;
        cfg.QuicConnections = 4;
        cfg.QuicConnectionStreamCount = 1024;
        cfg.QuicKeepAliveIntervalMs = 5000;
        cfg.Compress = "off";
        cfg.CompressLevel = 1;
        TqPeerConfig peer;
        peer.PeerId = "agent-a";
        peer.QuicPeer = "127.0.0.1:14444";
        peer.SocksListen = "127.0.0.1:11001";
        peer.QuicConnections = 4;
        peer.Enabled = true;
        cfg.Router.Peers.push_back(peer);

        const std::string json = TqRuntimeConfigJson(cfg, false);
        if (json.find("\"role\":\"client\"") == std::string::npos) return 930;
        if (json.find("\"id\":\"agent-a\"") == std::string::npos) return 931;
        if (json.find("\"proto_peer\":\"127.0.0.1:14444\"") == std::string::npos) return 932;
        if (json.find("\"proto_connections\":4") == std::string::npos) return 933;
        if (json.find("\"token\":\"") != std::string::npos) return 934;
    }
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target router_runtime_test -j2
```

Expected: compile fails because `admin_config.h` or `TqRuntimeConfigJson` is missing.

- [ ] **Step 3: Add serializer declarations**

Create `src/runtime/admin_config.h`:

```cpp
#pragma once

#include "config.h"

#include <string>
#include <vector>

std::string TqRuntimeConfigJson(const TqConfig& cfg, bool redact);
std::string TqServerRuntimeConfigJson(const TqConfig& cfg, const std::vector<std::string>& resolvedListens, bool redact);
std::string TqDiagnosticsJson(const TqConfig& cfg);
std::string TqPeerPublicConfigJson(const TqConfig& cfg, const TqPeerConfig& peer);
std::string TqClientPublicConfigJson(const TqConfig& cfg);
std::string TqStructuredErrorJson(const std::string& code, const std::string& message);
```

- [ ] **Step 4: Implement minimal serializer**

Create `src/runtime/admin_config.cpp` with local JSON escaping and field append helpers. Implement at least:

```cpp
std::string TqStructuredErrorJson(const std::string& code, const std::string& message);
std::string TqRuntimeConfigJson(const TqConfig& cfg, bool redact);
std::string TqClientPublicConfigJson(const TqConfig& cfg);
std::string TqPeerPublicConfigJson(const TqConfig& cfg, const TqPeerConfig& peer);
std::string TqDiagnosticsJson(const TqConfig& cfg);
std::string TqServerRuntimeConfigJson(const TqConfig& cfg, const std::vector<std::string>& resolvedListens, bool redact);
```

Required mapping:

```text
TqMode::Client -> "client"
TqMode::Server -> "server"
TqQuicProfile::MaxThroughput -> "max-throughput"
TqQuicProfile::LowLatency -> "low-latency"
TqTuningMode::Auto -> "auto"
TqTuningMode::Lan -> "lan"
TqTuningMode::Wan -> "wan"
TqConfig.QuicPeer/TqPeerConfig.QuicPeer -> proto_peer
TqConfig.QuicConnections/TqPeerConfig.QuicConnections -> proto_connections
```

Sensitive handling:

```text
admin token content: never serialized
proxy_auth password: "***"
redact=true and QuicKey non-empty: tls.key="***"
```

- [ ] **Step 5: Wire CMake**

In `src/CMakeLists.txt`, insert `runtime/admin_config.cpp` immediately after `runtime/admin_memory.cpp` in the `TCPQUIC_PROXY_SOURCES` list. Insert the same source immediately after `runtime/admin_memory.cpp` in `TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES`. Insert it immediately after `runtime/admin_memory.cpp` in the `tcpquic_server_admin_test` source list. Add it to `tcpquic_admin_http_test` only if `/api/v1/admin` status JSON reuses helpers from `admin_config.cpp`; otherwise keep `tcpquic_admin_http_test` limited to `admin_auth.cpp` and `admin_http.cpp`.

- [ ] **Step 6: Run test and verify pass**

Run:

```bash
rtk cmake --build build-glibc --target router_runtime_test -j2
rtk ./build-glibc/src/router_runtime_test
```

Expected: build succeeds and test exits 0.

## Task 2: `/api/v1/admin` Server-Level Endpoint

**Files:**
- Modify: `src/runtime/admin_http.h`
- Modify: `src/runtime/admin_http.cpp`
- Modify: `src/main.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Write failing HTTP test**

In `src/unittest/admin_http_test.cpp`, add a test after token-auth success coverage:

```cpp
    {
        std::string err;
        TqAdminHttpServerOptions options;
        options.AdminThreads = 1;
        options.EnableTokenAuth = true;
        options.Role = "client";
        TqAdminHttpServer server("127.0.0.1:0", [](const TqHttpRequest&) {
            return TqJsonResponse(500, "{\"error\":\"handler should not run\"}");
        }, options);
        if (!server.Start(err)) return 120;
        const std::string token = server.AuthTokenForTesting();
        const std::string request = "GET /api/v1/admin HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
            token + "\r\n\r\n";
        const std::string response = HttpRequest(server.ListenAddress(), request);
        if (response.find("HTTP/1.1 200") == std::string::npos) return 121;
        if (response.find("\"role\":\"client\"") == std::string::npos) return 122;
        if (response.find("\"token_present\":true") == std::string::npos) return 123;
        if (response.find("\"token\":\"") != std::string::npos) return 124;
    }
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target admin_http_test -j2
```

Expected: compile fails because `TqAdminHttpServerOptions::Role` is missing, or runtime test returns 404/500.

- [ ] **Step 3: Extend options**

In `src/runtime/admin_http.h`:

```cpp
struct TqAdminHttpServerOptions {
    uint32_t AdminThreads{2};
    size_t MaxBodyBytes{1024 * 1024};
    std::string TokenFile;
    bool EnableTokenAuth{false};
    std::string Role;
};
```

- [ ] **Step 4: Add status JSON helper**

In `src/runtime/admin_http.cpp`, add local helper:

```cpp
std::string TqAdminStatusBody(
    const std::string& role,
    const std::string& listen,
    const TqAdminHttpServerOptions& options,
    bool tokenPresent,
    const std::string& tokenFile) {
    std::ostringstream out;
    out << "{\"role\":\"" << (role.empty() ? "unknown" : role) << "\"";
    out << ",\"listen\":\"" << listen << "\"";
    out << ",\"auth\":{\"enabled\":" << ((options.EnableTokenAuth || !options.TokenFile.empty()) ? "true" : "false")
        << ",\"type\":\"bearer\",\"token_file\":\"" << tokenFile << "\",\"token_present\":"
        << (tokenPresent ? "true" : "false") << "}";
    out << ",\"http\":{\"threads\":" << options.AdminThreads
        << ",\"max_body_bytes\":" << options.MaxBodyBytes
        << ",\"keep_alive\":false}";
    out << ",\"security\":{\"loopback_only\":true,\"tls\":false}}";
    return out.str();
}
```

Add a local `TqAdminStatusEscape()` helper in `admin_http.cpp` and apply it to `role`, `listen`, and `tokenFile` before appending them. Do not write raw strings into JSON.

- [ ] **Step 5: Dispatch `/api/v1/admin` before runtime handler**

Inside `TqAdminHttpServer::ConfigureRoutes()` dispatch, after auth succeeds and before `TqIsV1AdminPath()`:

```cpp
        if (req.path == "/api/v1/admin") {
            TqSetJson(res, 200, TqAdminStatusBody(
                Options.Role,
                ListenAddress(),
                Options,
                Auth != nullptr && !Auth->Token().empty(),
                TokenFilePath));
            return;
        }
```

Also add `/api/v1/admin` to `TqIsV1AdminPath()` if keeping whitelist checks unified for future tests.

- [ ] **Step 6: Set role from `main.cpp`**

In `TqMakeAdminOptions()`:

```cpp
    options.Role = role;
```

- [ ] **Step 7: Run test and verify pass**

Run:

```bash
rtk cmake --build build-glibc --target admin_http_test -j2
rtk ./build-glibc/src/admin_http_test
```

Expected: test exits 0.

## Task 3: Runtime Config and Diagnostics Endpoints

**Files:**
- Modify: `src/runtime/router_runtime.h`
- Modify: `src/runtime/router_runtime.cpp`
- Modify: `src/runtime/server_admin.h`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/main.cpp`
- Test: `src/unittest/router_runtime_test.cpp`
- Test: `src/unittest/server_admin_test.cpp`

- [ ] **Step 1: Add failing router tests**

In `src/unittest/router_runtime_test.cpp`, after existing config tests:

```cpp
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Client;
        cfg.AdminListen = "127.0.0.1:18080";
        cfg.QuicConnections = 2;
        TqPeerConfig peer;
        peer.PeerId = "agent-public";
        peer.QuicPeer = "127.0.0.1:14460";
        peer.SocksListen = "127.0.0.1:11060";
        cfg.Router.Peers.push_back(peer);
        TqRouterRuntime runtime(nullptr, cfg);

        std::string runtimeConfig = runtime.HandleAdmin(Request("GET", "/runtime/config", ""));
        if (runtimeConfig.find("HTTP/1.1 200") == std::string::npos) return 940;
        if (runtimeConfig.find("\"proto_peer\":\"127.0.0.1:14460\"") == std::string::npos) return 941;

        std::string clientConfig = runtime.HandleAdmin(Request("GET", "/client/config", ""));
        if (clientConfig.find("\"peers\"") == std::string::npos) return 942;

        std::string diag = runtime.HandleAdmin(Request("GET", "/diagnostics", ""));
        if (diag.find("\"diag_stats\"") == std::string::npos) return 943;
    }
```

- [ ] **Step 2: Add failing server tests**

In `src/unittest/server_admin_test.cpp`:

```cpp
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Server;
        cfg.QuicListen = "0.0.0.0:4433";
        cfg.AllowTargets = {"127.0.0.1/32"};
        TqServerMetrics metrics;
        metrics.Listen = cfg.QuicListen;
        metrics.ResolvedListens = {"0.0.0.0:4433"};
        std::string config = TqHandleServerAdmin(Request("GET", "/server/config"), metrics, 10, cfg);
        if (config.find("HTTP/1.1 200") == std::string::npos) return 120;
        if (config.find("\"allow_targets\"") == std::string::npos) return 121;

        std::string diag = TqHandleServerAdmin(Request("GET", "/diagnostics"), metrics, 10, cfg);
        if (diag.find("\"trace\"") == std::string::npos) return 122;
    }
```

- [ ] **Step 3: Run tests and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target router_runtime_test server_admin_test -j2
```

Expected: compile fails because constructors/signatures are missing.

- [ ] **Step 4: Store config snapshot in router runtime**

In `src/runtime/router_runtime.h`, add constructor and member:

```cpp
explicit TqRouterRuntime(TqPeerRuntimeAdapter* adapter, TqConfig runtimeConfig);
TqConfig RuntimeConfig;
```

Keep existing constructors by initializing `RuntimeConfig` with role client.

- [ ] **Step 5: Handle router endpoints**

In `TqRouterRuntime::HandleAdmin()` before legacy `/config`:

```cpp
    if (req.Method == "GET" && req.Path == "/runtime/config") {
        return TqJsonResponse(200, TqRuntimeConfigJson(RuntimeConfig, false));
    }
    if (req.Method == "GET" && req.Path == "/client/config") {
        return TqJsonResponse(200, TqClientPublicConfigJson(RuntimeConfig));
    }
    if (req.Method == "GET" && req.Path == "/diagnostics") {
        return TqJsonResponse(200, TqDiagnosticsJson(RuntimeConfig));
    }
```

For `/peers/{id}/config`, reuse parsed peer path and return `TqPeerPublicConfigJson(RuntimeConfig, peerConfig)` from `SnapshotConfig()`.

- [ ] **Step 6: Extend server handler signature**

In `src/runtime/server_admin.h`:

```cpp
std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    const TqConfig& runtimeConfig);
```

Keep a compatibility overload calling the new function with default `TqConfig{}` for existing tests during migration.

- [ ] **Step 7: Handle server config and diagnostics**

In `src/runtime/server_admin.cpp`, before connection branches:

```cpp
    if (req.Method == "GET" && req.Path == "/server/config") {
        std::vector<std::string> resolved;
        {
            std::lock_guard<std::mutex> guard(metrics.Lock);
            resolved = metrics.ResolvedListens;
        }
        return TqJsonResponse(200, TqServerRuntimeConfigJson(runtimeConfig, resolved, false));
    }
    if (req.Method == "GET" && req.Path == "/diagnostics") {
        return TqJsonResponse(200, TqDiagnosticsJson(runtimeConfig));
    }
```

- [ ] **Step 8: Pass config snapshots from `main.cpp`**

Use the new router constructor in client paths:

```cpp
TqRouterRuntime runtime(&adapter, cfg);
```

Use the new server handler signature:

```cpp
return TqHandleServerAdmin(req, *metrics, uptimeSeconds, cfg);
```

- [ ] **Step 9: Extend HTTP whitelist**

In `TqIsV1AdminPath()` add:

```cpp
path == "/api/v1/runtime/config" ||
path == "/api/v1/client/config" ||
path == "/api/v1/diagnostics" ||
path == "/api/v1/server/config" ||
```

Extend peer path whitelist to allow `/api/v1/peers/{peer_id}/config`.

- [ ] **Step 10: Run tests and verify pass**

Run:

```bash
rtk cmake --build build-glibc --target router_runtime_test server_admin_test admin_http_test -j2
rtk ./build-glibc/src/router_runtime_test
rtk ./build-glibc/src/server_admin_test
rtk ./build-glibc/src/admin_http_test
```

Expected: all three tests exit 0.

## Task 4: Relay Active Relay and Worker Capability APIs

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/router_runtime.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing relay API tests**

In `src/unittest/router_runtime_test.cpp`:

```cpp
    {
        TqRouterRuntime runtime;
        std::string relays = runtime.HandleAdmin(Request("GET", "/relay/active-relays", ""));
        if (relays.find("HTTP/1.1 200") == std::string::npos) return 950;
        if (relays.find("\"capabilities\"") == std::string::npos) return 951;
        if (relays.find("\"relays\"") == std::string::npos) return 952;

        std::string relay = runtime.HandleAdmin(Request("GET", "/relay/active-relays/relay-missing", ""));
        if (relay.find("HTTP/1.1 404") == std::string::npos &&
            relay.find("not_supported") == std::string::npos) return 955;

        std::string aggregate = runtime.HandleAdmin(Request("GET", "/relay/workers/aggregate", ""));
        if (aggregate.find("HTTP/1.1 200") == std::string::npos) return 953;

        std::string missing = runtime.HandleAdmin(Request("GET", "/relay/workers/worker-1", ""));
        if (missing.find("not_supported") == std::string::npos &&
            missing.find("not found") == std::string::npos) return 954;
    }
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target router_runtime_test -j2
rtk ./build-glibc/src/router_runtime_test
```

Expected: active relays endpoint returns 404.

- [ ] **Step 3: Add relay JSON helper**

In `src/runtime/relay_metrics.h`:

```cpp
std::string TqRelayActiveRelaysJson();
std::string TqRelayActiveRelayJson(const std::string& relayId, bool& found, bool& supported);
std::string TqRelayWorkerDetailJson(const std::string& workerId, bool& found, bool& supported);
```

In `src/runtime/relay_metrics.cpp`, implement using `TqSnapshotActiveRelays()`:

```cpp
std::string TqRelayActiveRelaysJson() {
    const auto active = TqSnapshotActiveRelays();
    std::ostringstream out;
    out << "{\"capabilities\":{\"per_worker\":false,\"active_relay_detail\":"
        << (active.empty() ? "false" : "true") << "},\"relays\":[";
    // append known TqRelayActiveSnapshot fields
    out << "]}";
    return out.str();
}
```

For `workerId == "aggregate"`, return existing aggregate metrics and set `found=true, supported=true`; for other ids set `supported=false`.
For `TqRelayActiveRelayJson()`, search `TqSnapshotActiveRelays()` by `RelayId`; if the backend cannot provide active relay details, set `supported=false`.

- [ ] **Step 4: Handle router endpoint**

In `TqRouterRuntime::HandleAdmin()`:

```cpp
    if (req.Method == "GET" && req.Path == "/relay/active-relays") {
        return TqJsonResponse(200, TqRelayActiveRelaysJson());
    }
    if (req.Method == "GET" && req.Path.compare(0, 21, "/relay/active-relays/") == 0) {
        bool found = false;
        bool supported = false;
        std::string body = TqRelayActiveRelayJson(req.Path.substr(21), found, supported);
        if (!supported) {
            return TqJsonResponse(503, TqStructuredErrorJson("not_supported", "active relay detail is not supported by this backend"));
        }
        return TqJsonResponse(found ? 200 : 404, found ? body : TqStructuredErrorJson("not_found", "not found"));
    }
```

Update `/relay/workers/{id}` branch to use `TqRelayWorkerDetailJson()`.

- [ ] **Step 5: Extend whitelist**

In `TqIsV1AdminPath()` add:

```cpp
path == "/api/v1/relay/active-relays" ||
path.compare(0, 29, "/api/v1/relay/active-relays/") == 0 ||
```

- [ ] **Step 6: Run test and verify pass**

Run:

```bash
rtk cmake --build build-glibc --target router_runtime_test admin_http_test -j2
rtk ./build-glibc/src/router_runtime_test
rtk ./build-glibc/src/admin_http_test
```

Expected: tests exit 0.

## Task 5: Server Tunnel Single-Item Query and Control

**Files:**
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/runtime/admin_http.cpp`
- Test: `src/unittest/server_admin_test.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Write failing server tunnel tests**

In `src/unittest/server_admin_test.cpp`, extend the existing tunnel stubs near `TqSnapshotTunnels()`:

```cpp
bool g_tunnelAbortCalled = false;
bool g_tunnelDrainCalled = false;

bool TqGetTunnelSnapshot(const std::string& tunnelId, TqTunnelSnapshot& out) {
    if (tunnelId != "tun-5") {
        return false;
    }
    out = TqSnapshotTunnels()[0];
    return true;
}

bool TqAbortTunnelById(const std::string& tunnelId) {
    if (tunnelId != "tun-5") {
        return false;
    }
    g_tunnelAbortCalled = true;
    return true;
}

bool TqDrainTunnelById(const std::string& tunnelId) {
    if (tunnelId != "tun-5") {
        return false;
    }
    g_tunnelDrainCalled = true;
    return true;
}
```

After server tunnel list coverage, add:

```cpp
    std::string getTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun-5"), metrics, 10);
    if (getTunnel.find("HTTP/1.1 200 OK") == std::string::npos) return 130;
    if (getTunnel.find("\"tunnel_id\":\"tun-5\"") == std::string::npos) return 131;

    std::string abortTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/tun-5:abort"), metrics, 10);
    if (abortTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 132;
    if (!g_tunnelAbortCalled) return 133;

    std::string drainTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/tun-5:drain"), metrics, 10);
    if (drainTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 134;
    if (!g_tunnelDrainCalled) return 135;
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
rtk cmake --build build-glibc --target server_admin_test -j2
rtk ./build-glibc/src/server_admin_test
```

Expected: endpoint returns 404 through generic branch or whitelist blocks public HTTP path.

- [ ] **Step 3: Add server tunnel path parser**

In `src/runtime/server_admin.cpp`, add parser mirroring router tunnel parser:

```cpp
struct TqServerTunnelAdminPath {
    std::string TunnelId;
    std::string Action;
};
```

Decode percent-encoded single segment and split colon action.

- [ ] **Step 4: Implement query/control**

Before `/server/tunnels` collection branch:

```cpp
    if (req.Path.compare(0, 16, "/server/tunnels/") == 0) {
        TqServerTunnelAdminPath tunnelPath;
        if (!ParseServerTunnelAdminPath(req.Path, tunnelPath)) {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        TqTunnelSnapshot tunnel;
        if (!TqGetTunnelSnapshot(tunnelPath.TunnelId, tunnel) || tunnel.Role != "server") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (tunnelPath.Action.empty() && req.Method == "GET") {
            return TqJsonResponse(200, ServerTunnelJson(tunnel));
        }
        if ((tunnelPath.Action == "abort" && req.Method == "POST") ||
            (tunnelPath.Action.empty() && req.Method == "DELETE")) {
            const bool aborted = TqAbortTunnelById(tunnelPath.TunnelId);
            return TqJsonResponse(aborted ? 202 : 404,
                aborted ? "{\"status\":\"aborting\"}" : ErrorJson("not found"));
        }
        if (tunnelPath.Action == "drain" && req.Method == "POST") {
            const bool drained = TqDrainTunnelById(tunnelPath.TunnelId);
            return TqJsonResponse(drained ? 202 : 404,
                drained ? "{\"status\":\"draining\"}" : ErrorJson("not found"));
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }
```

- [ ] **Step 5: Extend whitelist**

`TqIsV1AdminPath()` already allows `/api/v1/server/tunnels` exact only. Add prefix:

```cpp
path.compare(0, 23, "/api/v1/server/tunnels/") == 0 ||
```

- [ ] **Step 6: Run tests and verify pass**

Run:

```bash
rtk cmake --build build-glibc --target server_admin_test admin_http_test -j2
rtk ./build-glibc/src/server_admin_test
rtk ./build-glibc/src/admin_http_test
```

Expected: tests exit 0.

## Task 6: Documentation Update

**Files:**
- Modify: `docs/admin-api/interface.md`
- Test: documentation grep checks

- [ ] **Step 1: Move implemented endpoints into formal tables**

Add these rows to `docs/admin-api/interface.md`:

```markdown
| `GET` | `/api/v1/admin` | `200` | 查询 Admin HTTP server 自身状态。 |
| `GET` | `/api/v1/runtime/config` | `200` | 查询完整运行时配置快照。 |
| `GET` | `/api/v1/client/config` | `200` | 查询 client 推荐 schema 配置快照。 |
| `GET` | `/api/v1/peers/{peer_id}/config` | `200`/`404` | 查询单 peer 推荐 schema 配置。 |
| `GET` | `/api/v1/diagnostics` | `200` | 查询 trace/diag-stats 状态。 |
| `GET` | `/api/v1/relay/active-relays` | `200` | 查询 active relay capability 和明细。 |
| `GET` | `/api/v1/server/config` | `200` | 查询 server 配置快照。 |
| `GET` | `/api/v1/server/tunnels/{tunnel_id}` | `200`/`404` | 查询 server tunnel。 |
| `DELETE` | `/api/v1/server/tunnels/{tunnel_id}` | `202`/`404` | 中止 server tunnel。 |
| `POST` | `/api/v1/server/tunnels/{tunnel_id}:abort` | `202`/`404` | 中止 server tunnel。 |
| `POST` | `/api/v1/server/tunnels/{tunnel_id}:drain` | `202`/`404` | drain server tunnel。 |
```

- [ ] **Step 2: Update gap section**

将已实现项标记为“已补齐”，保留第二阶段事项：

```markdown
- 动态修改 TLS/proto/relay/Admin listen 仍不支持。
- `PATCH /api/v1/diagnostics` 暂不支持，返回 `not_supported`。
- Linux per-worker relay 明细需要后续 worker snapshot 支持。
```

- [ ] **Step 3: Run documentation checks**

Run:

```bash
rtk bash -lc 'python3 - << "PY"
from pathlib import Path
text = Path("docs/admin-api/interface.md").read_text()
required = [
    "/api/v1/admin",
    "/api/v1/runtime/config",
    "/api/v1/server/config",
    "/api/v1/diagnostics",
    "/api/v1/relay/active-relays",
    "/api/v1/server/tunnels/{tunnel_id}:abort",
]
missing = [p for p in required if p not in text]
print("missing=" + ",".join(missing))
raise SystemExit(1 if missing else 0)
PY'
```

Expected: `missing=` and exit 0.

## Task 7: Final Verification

**Files:**
- Verify all touched source, tests, and docs.

- [ ] **Step 1: Build all relevant tests**

Run:

```bash
rtk cmake --build build-glibc --target admin_http_test router_runtime_test server_admin_test -j2
```

Expected: exit 0.

- [ ] **Step 2: Run all relevant tests**

Run:

```bash
rtk ./build-glibc/src/admin_http_test
rtk ./build-glibc/src/router_runtime_test
rtk ./build-glibc/src/server_admin_test
```

Expected: each command exits 0.

- [ ] **Step 3: Optional k6 baseline**

If a local client/server fixture is available, run the readonly Admin API k6 scenario described in `docs/admin-api/missing-interfaces-design.md`.

Expected gates:

```text
http_req_failed < 0.01
p95 readonly latency < 20ms
p99 readonly latency < 100ms
```

- [ ] **Step 4: Review diff**

Run:

```bash
rtk git diff -- src/runtime src/unittest src/CMakeLists.txt docs/admin-api/interface.md
```

Expected: diff only contains Admin API missing-interface implementation, tests, and docs.

## Scope Deferred After This Plan

- Runtime mutation of TLS/proto/relay/Admin listen remains unsupported.
- `PATCH /api/v1/diagnostics` remains disabled unless trace/diag lifecycle is explicitly refactored.
- Real Linux per-worker relay snapshots are a follow-up if aggregate metrics are insufficient.
