# Admin V1 Only and Single-Peer Router Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove public legacy Admin API support and route single-peer client Admin through `TqRouterRuntime` as a one-peer router.

**Architecture:** Keep the bearer-token Admin HTTP server as the public gate, but make it v1-only. Continue using `/api/v1` to internal-path mapping so existing router and server Admin handlers stay focused on business logic. Convert normal single-peer client startup to build a one-peer `TqRouterConfig` and use the same runtime Admin handler as multi-peer mode.

**Tech Stack:** C++17, cpp-httplib, existing `TqAdminHttpServer`, `TqAdminAuth`, `TqRouterRuntime`, CMake test binaries.

---

## File Structure

- Modify `src/config/config.h`: remove the legacy config field.
- Modify `src/config/config.cpp`: remove CLI/config parsing and usage text for the removed flag.
- Modify `src/runtime/admin_http.h`: remove `AllowUnauthenticatedLegacy` from server options.
- Modify `src/runtime/admin_http.cpp`: remove legacy public route recognition and make dispatch v1-only.
- Modify `src/runtime/client_peer_runtime.h`: add a small helper that converts current single-peer config to a one-peer router config for Admin/runtime reuse.
- Modify `src/main.cpp`: remove legacy option wiring and warning; make normal single-peer client Admin use `TqRouterRuntime`.
- Modify `src/unittest/config_router_test.cpp`: replace legacy acceptance assertions with rejection assertions.
- Modify `src/unittest/admin_http_test.cpp`: replace unauthenticated legacy tests with non-v1 404 tests.
- Modify docs: `docs/admin-api-redesign-design.md`, `docs/admin-api-redesign-dev-plan.md`, `docs/config_guide.md`, `docs/config_guide_cn.md`, and `src/docs/key_parameter.md` if they mention the removed flag or legacy Admin paths.

## Task 1: Remove Config and CLI Acceptance

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Test: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing config tests**

In `src/unittest/config_router_test.cpp`, change the Admin usage/config assertions so the removed flag is rejected.

Replace the assertion that usage contains the flag:

```cpp
if (usage.find("--admin-allow-unauthenticated-legacy") != std::string::npos) return 190;
```

Replace the default config assertion:

```cpp
if (cfg.AdminThreads != 2) return 191;
```

Replace the CLI acceptance block with:

```cpp
{
    const char* args[] = {
        "tcpquic-proxy", "client",
        "--peer", "127.0.0.1:14444",
        "--ca", "ca.crt",
        "--admin-allow-unauthenticated-legacy"};
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 193;
    if (err.find("unknown argument") == std::string::npos &&
        err.find("admin-allow-unauthenticated-legacy") == std::string::npos) return 194;
}
```

Replace the JSON admin key acceptance block with:

```cpp
{
    TqConfig cfg;
    std::string err;
    if (ParseRuntimeConfig(R"json({
        "tls":{"ca":"ca.crt"},
        "admin":{"listen":"127.0.0.1:19091","allow_unauthenticated_legacy":true},
        "peers":[{"id":"agent-b","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]
    })json", cfg, err)) {
        return 199;
    }
    if (err.find("unknown admin key") == std::string::npos &&
        err.find("allow_unauthenticated_legacy") == std::string::npos) return 200;
}
{
    TqConfig cfg;
    std::string err;
    if (!ParseRuntimeConfig(R"json({
        "tls":{"ca":"ca.crt"},
        "admin":{"listen":"127.0.0.1:19091","token_file":"/tmp/tq-admin.json","threads":3},
        "peers":[{"id":"agent-b","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]
    })json", cfg, err)) {
        std::fprintf(stderr, "runtime admin config parse failed: %s\n", err.c_str());
        return 201;
    }
    if (cfg.AdminListen != "127.0.0.1:19091") return 202;
    if (cfg.AdminTokenFile != "/tmp/tq-admin.json") return 203;
    if (cfg.AdminThreads != 3) return 204;
}
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_config_router_test
rtk ./build-regression/bin/Release/tcpquic_config_router_test
```

Expected: the binary fails because usage still contains the removed flag and parsing still accepts it.

- [ ] **Step 3: Remove config field and parsing**

In `src/config/config.h`, delete:

```cpp
bool AdminAllowUnauthenticatedLegacy{false};
```

In `src/config/config.cpp`, delete the `allow_unauthenticated_legacy` branch from the admin JSON parser:

```cpp
} else if (key == "allow_unauthenticated_legacy") {
    if (!ParseBool(cfg.AdminAllowUnauthenticatedLegacy)) return Error("invalid admin.allow_unauthenticated_legacy");
```

Delete this usage text:

```cpp
"  --admin-allow-unauthenticated-legacy\n"
"                              Allow old /health /metrics /config without token on loopback\n"
```

Delete this CLI parse branch:

```cpp
} else if (std::strcmp(arg, "--admin-allow-unauthenticated-legacy") == 0) {
    cfg.AdminAllowUnauthenticatedLegacy = true;
```

- [ ] **Step 4: Run the test and verify GREEN**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_config_router_test
rtk ./build-regression/bin/Release/tcpquic_config_router_test
```

Expected: `tcpquic_config_router_test` exits with status 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/config/config.h src/config/config.cpp src/unittest/config_router_test.cpp
rtk git commit -m "admin: remove legacy unauth config flag"
```

## Task 2: Make Admin HTTP Routing V1-Only

**Files:**
- Modify: `src/runtime/admin_http.h`
- Modify: `src/runtime/admin_http.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Write failing Admin HTTP tests**

In `src/unittest/admin_http_test.cpp`, remove the two blocks that set `options.AllowUnauthenticatedLegacy = true`.

Add a replacement block near the existing auth tests:

```cpp
{
    TqAdminHttpServerOptions options;
    options.TokenFile = TqSecureTokenFile("v1-only").string();
    std::string err;
    TqAdminHttpServer server("127.0.0.1:0", [&](const TqHttpRequest& req) {
        if (req.Method == "GET" && req.Path == "/health") {
            return TqJsonResponse(200, "{\"ok\":true}");
        }
        if (req.Method == "POST" && req.Path == "/peers/agent-d/enable") {
            return TqJsonResponse(200, "{\"legacy_enable\":true}");
        }
        return TqJsonResponse(404, "{}");
    }, options);
    if (!server.Start(err)) return 105;
    const uint16_t port = TqPortFromListenAddress(server.ListenAddress());
    if (port == 0) return 106;

    TqSocketHandle healthFd = TqConnectLocal(port);
    if (!TqSocketValid(healthFd)) return 107;
    if (!TqSendAll(healthFd, "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")) return 108;
    std::string healthResponse;
    if (!TqRecvUntilClosed(healthFd, healthResponse)) return 109;
    TqCloseSocket(healthFd);
    if (!TqHttpStatusIs(healthResponse, 404)) return 110;

    TqSocketHandle peerFd = TqConnectLocal(port);
    if (!TqSocketValid(peerFd)) return 111;
    if (!TqSendAll(peerFd, "POST /peers/agent-d/enable HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n")) return 112;
    std::string peerResponse;
    if (!TqRecvUntilClosed(peerFd, peerResponse)) return 113;
    TqCloseSocket(peerFd);
    server.Stop();
    if (!TqHttpStatusIs(peerResponse, 404)) return 114;
}
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_admin_http_test
rtk ./build-regression/bin/Release/tcpquic_admin_http_test
```

Expected: the binary fails because `/health` or `/peers/agent-d/enable` still reaches the handler.

- [ ] **Step 3: Remove legacy route gate**

In `src/runtime/admin_http.h`, remove:

```cpp
bool AllowUnauthenticatedLegacy{false};
```

In `src/runtime/admin_http.cpp`, delete the whole `TqIsLegacyAdminPath()` helper.

Replace the start of `dispatch` in `TqAdminHttpServer::ConfigureRoutes()` with:

```cpp
auto dispatch = [this](const httplib::Request& req, httplib::Response& res) {
    if (!TqIsV1Prefix(req.path)) {
        TqSetJson(res, 404, "{\"error\":{\"code\":\"not_found\",\"message\":\"not found\"}}");
        return;
    }
    TqHttpRequest adminReq = TqMakeAdminRequest(req);
    if (Auth && !Auth->Authorize(adminReq)) {
        TqSetJson(res, 401, TqUnauthorizedJson());
        return;
    }
    if (!TqIsV1AdminPath(req.path)) {
        TqSetJson(res, 404, "{\"error\":{\"code\":\"not_found\",\"message\":\"not found\"}}");
        return;
    }
    TqSetLegacyResponse(res, Handler ? Handler(adminReq) : TqJsonResponse(500, "{\"error\":\"no handler\"}"));
};
```

- [ ] **Step 4: Run the test and verify GREEN**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_admin_http_test
rtk ./build-regression/bin/Release/tcpquic_admin_http_test
```

Expected: `tcpquic_admin_http_test` exits with status 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/admin_http.h src/runtime/admin_http.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "admin: restrict public routes to api v1"
```

## Task 3: Add a Testable Single-Peer Router Config Helper

**Files:**
- Modify: `src/runtime/client_peer_runtime.h`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing helper test**

In `src/unittest/router_runtime_test.cpp`, add a block near the existing bridge validation tests:

```cpp
{
    TqConfig single;
    single.QuicPeer = "127.0.0.1:14444";
    single.SocksListen = "127.0.0.1:11001";
    single.HttpListen = "127.0.0.1:18001";
    single.QuicConnections = 4;
    single.Compress = "zstd";
    single.Router.ProxyAuth.push_back({"alice", "secret"});

    TqRouterConfig router = TqMakeSinglePeerRouterConfig(single);
    if (router.Peers.size() != 1) return 63;
    if (router.Peers[0].PeerId != "primary") return 64;
    if (router.Peers[0].QuicPeer != "127.0.0.1:14444") return 65;
    if (router.Peers[0].SocksListen != "127.0.0.1:11001") return 66;
    if (router.Peers[0].HttpListen != "127.0.0.1:18001") return 67;
    if (router.Peers[0].QuicConnections != 4) return 68;
    if (router.Peers[0].Compress != "zstd") return 69;
    if (router.ProxyAuth.size() != 1 || router.ProxyAuth[0].Username != "alice") return 70;
}
```

- [ ] **Step 2: Run the test and verify behavior**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: build or test fails because `TqMakeSinglePeerRouterConfig` does not exist.

- [ ] **Step 3: Implement the helper**

In `src/runtime/client_peer_runtime.h`, add this helper immediately after `TqMakePrimaryPeerConfig()`:

```cpp
inline TqRouterConfig TqMakeSinglePeerRouterConfig(const TqConfig& cfg) {
    TqRouterConfig router;
    router.ProxyAuth = cfg.Router.ProxyAuth;
    router.Peers.push_back(TqMakePrimaryPeerConfig(cfg));
    return router;
}
```

- [ ] **Step 4: Run the test and verify GREEN**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: `tcpquic_router_runtime_test` exits with status 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/runtime/client_peer_runtime.h src/unittest/router_runtime_test.cpp
rtk git commit -m "admin: add single peer router config helper"
```

## Task 4: Route Normal Single-Peer Admin Through Router Runtime

**Files:**
- Modify: `src/main.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Confirm helper and bridge semantics are green**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: `tcpquic_router_runtime_test` exits with status 0 before startup wiring changes.

- [ ] **Step 2: Replace single-peer Admin handler**

In `src/main.cpp`, update `TqMakeAdminOptions()` to remove legacy option wiring:

```cpp
TqAdminHttpServerOptions TqMakeAdminOptions(const TqConfig& cfg) {
    TqAdminHttpServerOptions options;
    options.AdminThreads = cfg.AdminThreads;
    options.TokenFile = cfg.AdminTokenFile.empty() ? TqAdminAuth::DefaultTokenFilePath() : cfg.AdminTokenFile;
    options.EnableTokenAuth = true;
    return options;
}
```

Update `TqPrintAdminStarted()`:

```cpp
void TqPrintAdminStarted(const TqAdminHttpServer& admin, const TqAdminHttpServerOptions& options) {
    std::fprintf(stderr, "tcpquic-proxy: admin listening on %s\n", admin.ListenAddress().c_str());
    std::fprintf(stderr, "tcpquic-proxy: admin token file %s\n", options.TokenFile.c_str());
}
```

Replace the normal non-speed-test body of `RunSinglePeerClient()` after the speed-test block with router runtime wiring:

```cpp
const TqRouterConfig router = TqMakeSinglePeerRouterConfig(cfg);

TqMultiPeerRuntimeAdapter adapter(cfg);
TqRouterRuntime runtime(&adapter);
if (!runtime.ApplyConfig(router, err)) {
    std::fprintf(stderr, "tcpquic-proxy: invalid router config: %s\n", err.c_str());
    return 1;
}

TqPeerMetrics startupMetrics;
if (!runtime.GetPeer(primary.PeerId, startupMetrics)) {
    std::fprintf(stderr, "tcpquic-proxy: client runtime unavailable\n");
    return 1;
}
std::fprintf(stderr, "tcpquic-proxy: QUIC peer %s (%u connections)\n",
    cfg.QuicPeer.c_str(), startupMetrics.ConnectionCount);

std::unique_ptr<TqAdminHttpServer> admin;
if (!cfg.AdminListen.empty()) {
    if (!TqValidateAdminListen(cfg.AdminListen, err)) {
        std::fprintf(stderr, "tcpquic-proxy: invalid admin listen: %s\n", err.c_str());
        return 1;
    }
    const TqAdminHttpServerOptions adminOptions = TqMakeAdminOptions(cfg);
    admin.reset(new TqAdminHttpServer(cfg.AdminListen, [&runtime](const TqHttpRequest& req) {
        return runtime.HandleAdmin(req);
    }, adminOptions));
    if (!admin->Start(err)) {
        std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
        return 1;
    }
    TqPrintAdminStarted(*admin, adminOptions);
}
```

Remove the old `TqClientRuntimeManager manager(...)` normal Admin path. Keep the speed-test path using the direct manager because it exits after the test.

- [ ] **Step 3: Build and run router runtime test**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: `tcpquic_router_runtime_test` exits with status 0.

- [ ] **Step 4: Build the main binary**

Run:

```bash
rtk cmake --build build-regression --target tcpquic-proxy
```

Expected: build succeeds with no reference to `AdminAllowUnauthenticatedLegacy` or `AllowUnauthenticatedLegacy`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/main.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "admin: use router runtime for single peer"
```

## Task 5: Update Documentation

**Files:**
- Modify: `docs/admin-api-redesign-design.md`
- Modify: `docs/admin-api-redesign-dev-plan.md`
- Modify: `docs/config_guide.md`
- Modify: `docs/config_guide_cn.md`
- Modify: `src/docs/key_parameter.md`

- [ ] **Step 1: Find stale documentation**

Run:

```bash
rtk rg -n "admin-allow-unauthenticated-legacy|allow_unauthenticated_legacy|legacy unauthenticated|旧路径|旧 API|/health|/metrics|/config" docs src/docs
```

Expected: output lists the Admin redesign docs and config docs that still mention legacy public paths or the removed switch.

- [ ] **Step 2: Edit the docs**

Apply these documentation rules:

- Remove `--admin-allow-unauthenticated-legacy`.
- Remove `admin.allow_unauthenticated_legacy`.
- Replace compatibility language with: "Admin public API is `/api/v1/*` only. Legacy paths such as `/health`, `/metrics`, `/config`, and `/peers/{id}/enable` are removed."
- Keep token workflow docs.
- Keep server-only v1 docs.
- State that single-peer client Admin is represented as `/api/v1/peers` containing one peer with id `primary`.

- [ ] **Step 3: Verify stale references are gone**

Run:

```bash
rtk rg -n "admin-allow-unauthenticated-legacy|allow_unauthenticated_legacy|legacy unauthenticated" src docs
```

If this command only reports this implementation plan or the design spec, rerun the product-doc check:

```bash
rtk rg -n "admin-allow-unauthenticated-legacy|allow_unauthenticated_legacy|legacy unauthenticated" src docs --glob '!docs/superpowers/**'
```

Expected: no output from the product-doc check.

- [ ] **Step 4: Commit**

```bash
rtk git add docs/admin-api-redesign-design.md docs/admin-api-redesign-dev-plan.md docs/config_guide.md docs/config_guide_cn.md src/docs/key_parameter.md
rtk git commit -m "docs: document admin v1 only mode"
```

## Task 6: Final Verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Search for removed symbols**

Run:

```bash
rtk rg -n "AdminAllowUnauthenticatedLegacy|AllowUnauthenticatedLegacy|admin-allow-unauthenticated-legacy|allow_unauthenticated_legacy" src docs
```

If this command only reports this implementation plan or the design spec, rerun the product-doc check:

```bash
rtk rg -n "AdminAllowUnauthenticatedLegacy|AllowUnauthenticatedLegacy|admin-allow-unauthenticated-legacy|allow_unauthenticated_legacy" src docs --glob '!docs/superpowers/**'
```

Expected: no output from the product-doc check.

- [ ] **Step 2: Build targeted tests**

Run:

```bash
rtk cmake --build build-regression --target tcpquic_config_router_test tcpquic_admin_http_test tcpquic_router_runtime_test tcpquic-proxy
```

Expected: build succeeds.

- [ ] **Step 3: Run targeted tests**

Run:

```bash
rtk ./build-regression/bin/Release/tcpquic_config_router_test
rtk ./build-regression/bin/Release/tcpquic_admin_http_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: all three commands exit with status 0.

- [ ] **Step 4: Review diff**

Run:

```bash
rtk git diff --stat HEAD~5..HEAD
rtk git diff HEAD~5..HEAD -- src/config src/runtime src/main.cpp src/unittest docs/admin-api-redesign-design.md docs/admin-api-redesign-dev-plan.md docs/config_guide.md docs/config_guide_cn.md src/docs/key_parameter.md
```

Expected: diff only removes legacy Admin public support, switches single-peer Admin to router runtime, updates tests, and updates docs.
