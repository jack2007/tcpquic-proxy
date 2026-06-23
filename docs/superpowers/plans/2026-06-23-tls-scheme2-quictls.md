# TLS Scheme 2 With Vendored quictls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement scheme 2 TLS credentials: clients validate server certificates with `ca.crt`, servers do not validate client certificates, and all platforms use vendored quictls.

**Architecture:** Role-specific config validation feeds role-specific msquic credential construction. Client credentials use `QUIC_CREDENTIAL_TYPE_NONE` plus `CaCertificateFile`; server credentials use PEM certificate/key only. Build and docs state that Linux, Windows, and macOS use vendored quictls, with no Schannel or system crypto path.

**Tech Stack:** C++17, CMake, msquic C/C++ API, vendored quictls, existing no-framework C++ unit tests, Markdown docs.

---

## File Map

- Modify `src/config/config.cpp`: CLI usage text and role-specific TLS argument validation.
- Modify `CMakeLists.txt`: force vendored quictls crypto by setting `QUIC_USE_SYSTEM_LIBCRYPTO=OFF`.
- Modify `src/CMakeLists.txt`: remove `platform/quic_credentials_win.cpp` and `crypt32` from the active Windows proxy target.
- Modify `src/protocol/quic_session.h`: add `TQ_UNIT_TESTING` credential snapshot declarations and remove Schannel credential-holder members/includes.
- Modify `src/protocol/quic_session.cpp`: implement role-specific quictls credential construction, remove mTLS flags, and remove Schannel credential-holder branches.
- Modify `src/unittest/config_router_test.cpp`: add parser tests for scheme 2 CLI and runtime JSON behavior.
- Modify `src/unittest/quic_session_reconnect_test.cpp`: add credential construction tests.
- Modify `docs/config_guide.md`: update JSON examples and TLS key table.
- Modify `docs/config_guide_cn.md`: update Chinese JSON examples and TLS key table.
- Modify `src/README.md`: update quick start, CLI table, security wording, and capability list.
- Modify `README.md`: update top-level architecture and examples if still describing mTLS or older certificate requirements.
- Verify `docs/tls-cert.md`: keep it as the source of truth for scheme 2 and quictls-only policy.

## Task 1: Add Config Parser Tests

**Files:**
- Modify: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing tests for role-specific TLS requirements**

Add these checks inside `main()` near the existing usage and CLI parser tests:

```cpp
{
    const std::string usage = CaptureUsage();
    if (usage.find("Client TLS: --ca is required") == std::string::npos) return 175;
    if (usage.find("Server TLS: --cert and --key are required") == std::string::npos) return 176;
}
{
    const char* args[] = {
        "tcpquic-proxy", "client",
        "--peer", "127.0.0.1:14444",
        "--ca", "ca.crt"
    };
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 177;
    if (cfg.QuicCa != "ca.crt") return 178;
    if (!cfg.QuicCert.empty()) return 179;
    if (!cfg.QuicKey.empty()) return 180;
}
{
    const char* args[] = {
        "tcpquic-proxy", "client",
        "--peer", "127.0.0.1:14444"
    };
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 181;
    if (err.find("--ca") == std::string::npos) return 182;
}
{
    const char* args[] = {
        "tcpquic-proxy", "server",
        "--listen", "0.0.0.0:4433",
        "--allow-targets", "127.0.0.1/32",
        "--cert", "server.crt",
        "--key", "server.key"
    };
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 183;
    if (cfg.QuicCert != "server.crt") return 184;
    if (cfg.QuicKey != "server.key") return 185;
    if (!cfg.QuicCa.empty()) return 186;
}
```

- [ ] **Step 2: Add runtime JSON tests**

Add these checks near existing `--config` tests:

```cpp
{
    std::string file = WriteTempConfig(R"json({
        "tls":{"ca":"ca.crt"},
        "peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:11080"}]
    })json");
    const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 187;
    if (cfg.QuicCa != "ca.crt") return 188;
    if (!cfg.QuicCert.empty()) return 189;
    if (!cfg.QuicKey.empty()) return 190;
}
{
    std::string file = WriteTempConfig(R"json({
        "tls":{"cert":"server.crt","key":"server.key"},
        "server":{"proto_listen":"0.0.0.0:4433","allow_targets":["127.0.0.1/32"]}
    })json");
    const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
    TqConfig cfg;
    std::string err;
    if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 191;
    if (cfg.QuicCert != "server.crt") return 192;
    if (cfg.QuicKey != "server.key") return 193;
    if (!cfg.QuicCa.empty()) return 194;
}
```

- [ ] **Step 3: Run test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: build succeeds, test fails before implementation because usage text or validation still follows the old symmetric certificate model.

## Task 2: Implement Config Validation and Usage Text

**Files:**
- Modify: `src/config/config.cpp`

- [ ] **Step 1: Update usage text**

Replace the TLS argument descriptions in `TqPrintUsage()` with:

```cpp
"  --cert <path>                TLS server certificate PEM path\n"
"  --key <path>                 TLS server private key PEM path\n"
"  --ca <path>                  CA certificate PEM path\n"
"                              Client TLS: --ca is required; --cert/--key are ignored\n"
"                              Server TLS: --cert and --key are required; --ca is optional\n"
```

- [ ] **Step 2: Update client validation**

In `TqParseArgs()` final validation, make client mode require only `cfg.QuicCa` after peer/router validation:

```cpp
if (cfg.Mode == TqMode::Client) {
    if (cfg.Router.Peers.empty() && !RequireNonEmpty(cfg.QuicPeer, "--peer", err)) {
        return false;
    }
    if (!RequireNonEmpty(cfg.QuicCa, "--ca", err)) {
        return false;
    }
    if (cfg.SpeedTestMode != TqSpeedTestMode::None && !cfg.Router.Peers.empty()) {
        size_t enabledPeers = 0;
        for (const auto& peer : cfg.Router.Peers) {
            if (peer.Enabled) {
                ++enabledPeers;
            }
        }
        if (enabledPeers != 1) {
            err = "speed-test options require exactly one enabled peer";
            return false;
        }
    }
}
```

- [ ] **Step 3: Update server validation**

In the server branch, require `cfg.QuicListen`, `cfg.QuicCert`, and `cfg.QuicKey`, but do not require `cfg.QuicCa`:

```cpp
else {
    if (!RequireNonEmpty(cfg.QuicListen, "--listen", err)) {
        return false;
    }
    if (!RequireNonEmpty(cfg.QuicCert, "--cert", err)) {
        return false;
    }
    if (!RequireNonEmpty(cfg.QuicKey, "--key", err)) {
        return false;
    }
    if (cfg.AllowTargets.empty()) {
        cfg.AllowTargets.push_back("0.0.0.0/0");
    }
    if (!TqValidateCidrList(cfg.AllowTargets, err)) {
        return false;
    }
    if (!cfg.DenyTargets.empty() && !TqValidateCidrList(cfg.DenyTargets, err)) {
        return false;
    }
}
```

- [ ] **Step 4: Run config parser test**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected: test passes.

## Task 3: Add Credential Construction Tests

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/unittest/quic_session_reconnect_test.cpp`

- [ ] **Step 1: Add test-only snapshot declarations**

In `src/protocol/quic_session.h`, after `TqMakeMsQuicSettings`, add:

```cpp
#if defined(TQ_UNIT_TESTING)
struct TqCredentialConfigSnapshot {
    QUIC_CREDENTIAL_TYPE Type{};
    QUIC_CREDENTIAL_FLAGS Flags{};
    bool HasCertificateFile{false};
    std::string CaCertificateFile;
};

TqCredentialConfigSnapshot TqBuildCredentialConfigSnapshotForTest(
    const TqConfig& cfg,
    bool server);
#endif
```

- [ ] **Step 2: Remove Schannel credential-holder declarations from the header**

In `src/protocol/quic_session.h`, delete this include block:

```cpp
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
#include "quic_credentials_win.h"
#endif
```

Also delete these members from `QuicClientSession` and `QuicServerSession`:

```cpp
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
    std::unique_ptr<TqQuicCredentialHolder> Credentials;
#endif
```

- [ ] **Step 3: Add failing credential tests**

In `src/unittest/quic_session_reconnect_test.cpp`, add:

```cpp
static int TestScheme2CredentialConfig() {
    TqConfig client;
    client.QuicCa = "ca.crt";
    TqCredentialConfigSnapshot clientCred =
        TqBuildCredentialConfigSnapshotForTest(client, false);
    if (clientCred.Type != QUIC_CREDENTIAL_TYPE_NONE) return 60;
    if (!(clientCred.Flags & QUIC_CREDENTIAL_FLAG_CLIENT)) return 61;
    if (!(clientCred.Flags & QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE)) return 62;
    if (clientCred.Flags & QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION) return 63;
    if (clientCred.HasCertificateFile) return 64;
    if (clientCred.CaCertificateFile != "ca.crt") return 65;

    TqConfig server;
    server.QuicCert = "server.crt";
    server.QuicKey = "server.key";
    TqCredentialConfigSnapshot serverCred =
        TqBuildCredentialConfigSnapshotForTest(server, true);
    if (serverCred.Type != QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE) return 66;
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_CLIENT) return 67;
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION) return 68;
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE) return 69;
    if (!serverCred.HasCertificateFile) return 70;
    if (!serverCred.CaCertificateFile.empty()) return 71;
    return 0;
}
```

Call it from `main()`:

```cpp
if (int rc = TestScheme2CredentialConfig()) return rc;
```

- [ ] **Step 4: Run test and verify failure**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j$(nproc)
```

Expected: link fails with undefined reference to `TqBuildCredentialConfigSnapshotForTest`, or the test fails if the helper exists but still returns old mTLS credential data.

## Task 4: Implement quictls Credential Construction

**Files:**
- Modify: `src/protocol/quic_session.cpp`

- [ ] **Step 1: Remove Schannel include and function parameters**

Delete this include block from the top of `src/protocol/quic_session.cpp`:

```cpp
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
#include "quic_credentials_win.h"
#endif
```

Change the `InitConfiguration` signature from the conditional credential-holder form to:

```cpp
bool InitConfiguration(
    const TqConfig& cfg,
    const MsQuicRegistration& registration,
    bool server,
    std::unique_ptr<MsQuicConfiguration>& configuration
    ) {
```

Update all `InitConfiguration(...)` call sites in this file to remove the conditional `Credentials` argument.

- [ ] **Step 2: Make credential config role-specific**

Replace the existing `TqCredentialConfig` constructor with:

```cpp
struct TqCredentialConfig {
    QUIC_CREDENTIAL_CONFIG Config{};
    QUIC_CERTIFICATE_FILE CertFile{};

    TqCredentialConfig(const TqConfig& cfg, bool server, QUIC_CREDENTIAL_FLAGS flags) {
        Config.Flags = flags;
#if defined(__APPLE__)
        if (!server) {
            Config.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
                Config.Flags | QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION);
        }
#endif
        if (server) {
            CertFile.PrivateKeyFile = cfg.QuicKey.c_str();
            CertFile.CertificateFile = cfg.QuicCert.c_str();
            Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            Config.CertificateFile = &CertFile;
        } else {
            Config.Type = QUIC_CREDENTIAL_TYPE_NONE;
            Config.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
                Config.Flags | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
            Config.CaCertificateFile = cfg.QuicCa.c_str();
        }
    }
};
```

- [ ] **Step 3: Add role-specific flags helper**

Add near `TqCredentialConfig`:

```cpp
QUIC_CREDENTIAL_FLAGS TqMakeCredentialFlags(bool server) {
    return server ? QUIC_CREDENTIAL_FLAG_NONE : QUIC_CREDENTIAL_FLAG_CLIENT;
}
```

- [ ] **Step 4: Add test snapshot implementation**

After `TqMakeMsQuicSettings`, add:

```cpp
#if defined(TQ_UNIT_TESTING)
TqCredentialConfigSnapshot TqBuildCredentialConfigSnapshotForTest(
    const TqConfig& cfg,
    bool server) {
    TqCredentialConfig credential(cfg, server, TqMakeCredentialFlags(server));
    TqCredentialConfigSnapshot snapshot{};
    snapshot.Type = credential.Config.Type;
    snapshot.Flags = credential.Config.Flags;
    snapshot.HasCertificateFile = credential.Config.CertificateFile != nullptr;
    if (credential.Config.CaCertificateFile != nullptr) {
        snapshot.CaCertificateFile = credential.Config.CaCertificateFile;
    }
    return snapshot;
}
#endif
```

- [ ] **Step 5: Use the new helper in configuration initialization**

In `InitConfiguration`, replace platform-specific old flag selection and credential-holder construction with:

```cpp
const auto flags = TqMakeCredentialFlags(server);
```

Then construct the quictls credential for every platform:

```cpp
const QUIC_CREDENTIAL_CONFIG* credentialConfig = nullptr;
TqCredentialConfig credential(cfg, server, flags);
credentialConfig = &credential.Config;
```

- [ ] **Step 6: Run credential test**

Run:

```bash
rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: test passes.

## Task 5: Enforce quictls-Only Platform Policy

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Inspect: `src/platform/quic_credentials_win.cpp`

- [ ] **Step 1: Confirm CMake forces quictls**

Run:

```bash
rtk rg -n "QUIC_TLS_LIB|QUIC_USE_SYSTEM_LIBCRYPTO|TCPQUIC_WINDOWS_TLS_QUICTLS|quictls|Schannel" CMakeLists.txt src/CMakeLists.txt
```

Expected before implementation: root CMake already forces `QUIC_TLS_LIB=quictls`; if it does not force `QUIC_USE_SYSTEM_LIBCRYPTO=OFF`, the next step adds it.

- [ ] **Step 2: Force vendored crypto in root CMake**

In root `CMakeLists.txt`, keep the existing `QUIC_TLS_LIB` line and add `QUIC_USE_SYSTEM_LIBCRYPTO=OFF` immediately after it:

```cmake
set(QUIC_TLS_LIB "quictls" CACHE STRING "MsQuic TLS backend" FORCE)
set(QUIC_USE_SYSTEM_LIBCRYPTO OFF CACHE BOOL "Use vendored quictls crypto" FORCE)
```

- [ ] **Step 3: Remove Schannel helper from the active Windows proxy target**

In `src/CMakeLists.txt`, remove this source from the Windows-only `TCPQUIC_PROXY_SOURCES` block:

```cmake
platform/quic_credentials_win.cpp
```

In the same Windows target block, change:

```cmake
target_link_libraries(tcpquic-proxy PRIVATE ws2_32 crypt32)
```

to:

```cmake
target_link_libraries(tcpquic-proxy PRIVATE ws2_32)
```

Keep `TCPQUIC_WINDOWS_TLS_QUICTLS` compile definitions for Windows targets. They document and enforce that the Windows build uses the quictls path.

- [ ] **Step 4: Verify no active insecure or Schannel credential path remains**

Run:

```bash
rtk rg -n "NO_CERTIFICATE_VALIDATION|REQUIRE_CLIENT_AUTHENTICATION|TqQuicCredentialHolder|quic_credentials_win|crypt32" src/protocol src/platform CMakeLists.txt src/CMakeLists.txt
```

Expected: `NO_CERTIFICATE_VALIDATION` is not used for client credential construction, `REQUIRE_CLIENT_AUTHENTICATION` is not used for server credential construction, `TqQuicCredentialHolder` is not referenced by `src/protocol/quic_session.*`, and `platform/quic_credentials_win.cpp`/`crypt32` are not part of the active proxy target.

## Task 6: Update Documentation

**Files:**
- Modify: `docs/config_guide.md`
- Modify: `docs/config_guide_cn.md`
- Modify: `src/README.md`
- Modify: `README.md`
- Verify: `docs/tls-cert.md`

- [ ] **Step 1: Update English config guide client TLS example**

In `docs/config_guide.md`, use:

```json
{
  "tls": {
    "ca": "certs/ca.crt"
  }
}
```

Explain that this CA verifies `server.crt`.

- [ ] **Step 2: Update English config guide server TLS example**

In `docs/config_guide.md`, use:

```json
{
  "tls": {
    "cert": "certs/server.crt",
    "key": "certs/server.key"
  }
}
```

- [ ] **Step 3: Update config key tables**

Use these row meanings in both English and Chinese guides:

```text
tls.cert: server only, required in server mode, ignored in client mode
tls.key: server only, required in server mode, ignored in client mode
tls.ca: client only, required in client mode, optional and unused in server mode
```

- [ ] **Step 4: Update README quick start**

Server example:

```bash
tcpquic-proxy server \
  --listen 0.0.0.0:443 \
  --cert /path/server.crt \
  --key  /path/server.key \
  --allow-targets 10.0.0.0/8,192.168.0.0/16 \
  --deny-targets 127.0.0.0/8 \
  --compress off
```

Client example:

```bash
tcpquic-proxy client \
  --peer proxy-b.example.com:443 \
  --ca   /path/ca.crt \
  --socks-listen 127.0.0.1:1080 \
  --http-listen 127.0.0.1:8080 \
  --compress off
```

- [ ] **Step 5: Update security wording**

Replace mTLS wording with:

```text
TLS: client verifies the server certificate using the configured CA; server does not require client certificates.
```

- [ ] **Step 6: Search for stale mTLS/client certificate examples**

Run:

```bash
rtk rg -n -- "mTLS|client\\.crt|client\\.key|--quic-cert|--quic-key|--quic-ca|--cert|--key|--ca|Schannel|system crypto|libcrypto" README.md src/README.md docs/config_guide.md docs/config_guide_cn.md docs/tls-cert.md
```

Expected: any remaining hits either describe scheme 1 as historical/reference material in `docs/tls-cert.md` or accurately describe scheme 2.

## Task 7: Run Focused Verification

**Files:**
- No edits.

- [ ] **Step 1: Build focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_quic_session_reconnect_test -j$(nproc)
```

Expected: both targets build.

- [ ] **Step 2: Run focused tests**

Run:

```bash
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_quic_session_reconnect_test
```

Expected: both commands exit 0.

- [ ] **Step 3: Build main binary**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
```

Expected: target builds.

- [ ] **Step 4: Inspect final diff**

Run:

```bash
rtk git diff -- CMakeLists.txt src/CMakeLists.txt src/config/config.cpp src/protocol/quic_session.cpp src/protocol/quic_session.h src/unittest/config_router_test.cpp src/unittest/quic_session_reconnect_test.cpp docs/config_guide.md docs/config_guide_cn.md src/README.md README.md docs/tls-cert.md
```

Expected: diff is scoped to TLS scheme 2, quictls-only policy, tests, and documentation.

## Task 8: Final Review

**Files:**
- No edits unless review finds a mismatch.

- [ ] **Step 1: Confirm scheme 2 behavior**

Check the final implementation against this matrix:

```text
client files required: ca.crt
server files required: server.crt, server.key
client validates server: yes
server validates client: no
client cert/key loaded: no
NO_CERTIFICATE_VALIDATION: no
REQUIRE_CLIENT_AUTHENTICATION: no
TLS backend: vendored quictls on Linux, Windows, macOS
system crypto: no
Windows Schannel: no
```

- [ ] **Step 2: Report verification**

In the final implementation response, include:

```text
Verified:
- tcpquic_config_router_test
- tcpquic_quic_session_reconnect_test
- tcpquic-proxy build
```

If any command cannot run, report the exact command and reason.
