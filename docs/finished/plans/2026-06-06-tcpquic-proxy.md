# TCP-over-QUIC Proxy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `tcpquic-proxy`, a msquic-based tool that tunnels arbitrary TCP connections over pre-established QUIC streams with SOCKS5/HTTP CONNECT front-ends, mTLS, ACL, and optional zstd compression.

**Architecture:** New tool under `src/tools/tcpquic-proxy/` with shared `TunnelCore` (control protocol, relay, compression) and mode-specific front-ends (SOCKS5, HTTP CONNECT on client; QUIC listener + TCP dialer on server). One TCP connection maps to one bidirectional QUIC stream on a single long-lived QUIC connection (v1).

**Tech Stack:** C++17, msquic C++ API (`msquic.hpp`), libzstd（必需）, optional liblz4, CMake `add_quic_tool`, platform TCP sockets via existing msquic helper patterns.

**Spec:** `docs/superpowers/specs/2026-06-06-tcpquic-proxy-design.md`

---

## File Map

| File | Responsibility |
|------|----------------|
| `src/tools/tcpquic-proxy/CMakeLists.txt` | Build target, link zstd |
| `src/tools/tcpquic-proxy/control_protocol.h/.cpp` | OPEN frame encode/decode |
| `src/tools/tcpquic-proxy/acl.h/.cpp` | CIDR allow/deny |
| `src/tools/tcpquic-proxy/compress.h/.cpp` | zstd/lz4 stream wrappers |
| `src/tools/tcpquic-proxy/config.h/.cpp` | CLI argument parsing |
| `src/tools/tcpquic-proxy/relay.h/.cpp` | Stream↔TCP bidirectional relay |
| `src/tools/tcpquic-proxy/quic_session.h/.cpp` | QUIC connection lifecycle |
| `src/tools/tcpquic-proxy/tcp_tunnel.h/.cpp` | Per-tunnel state machine |
| `src/tools/tcpquic-proxy/tcp_dialer.h/.cpp` | Server-side DNS + TCP connect |
| `src/tools/tcpquic-proxy/socks5_server.h/.cpp` | SOCKS5 CONNECT listener |
| `src/tools/tcpquic-proxy/http_connect_server.h/.cpp` | HTTP CONNECT listener |
| `src/tools/tcpquic-proxy/main.cpp` | Entry point, mode dispatch |
| `src/tools/tcpquic-proxy/unittest/control_protocol_test.cpp` | Unit tests |
| `src/tools/CMakeLists.txt` | `add_subdirectory(tcpquic-proxy)` |
| `scripts/test-tcpquic-proxy.sh` | Integration smoke test |

---

### Task 1: Scaffold build and directory layout

**Files:**
- Create: `src/tools/tcpquic-proxy/CMakeLists.txt`
- Create: `src/tools/tcpquic-proxy/main.cpp` (stub)
- Modify: `src/tools/CMakeLists.txt`

- [ ] **Step 1: Create stub main.cpp**

```cpp
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: tcpquic-proxy <client|server> [options]\n");
        return 1;
    }
    fprintf(stderr, "tcpquic-proxy: mode=%s (not yet implemented)\n", argv[1]);
    return 0;
}
```

- [ ] **Step 2: Create CMakeLists.txt**

```cmake
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZSTD REQUIRED libzstd)

set(TCPQUIC_PROXY_SOURCES
    main.cpp
)

add_quic_tool(tcpquic-proxy ${TCPQUIC_PROXY_SOURCES})

target_include_directories(tcpquic-proxy PRIVATE ${LIBZSTD_INCLUDE_DIRS})
target_link_libraries(tcpquic-proxy ${LIBZSTD_LIBRARIES})
target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_ZSTD=1)
```

- [ ] **Step 3: Register subdirectory**

In `src/tools/CMakeLists.txt`, add after `add_subdirectory(forwarder)`:

```cmake
add_subdirectory(tcpquic-proxy)
```

- [ ] **Step 4: Build and verify stub**

```bash
cd /home/jack/src/msquic/build-iouring
cmake --build . --target tcpquic-proxy -j$(nproc)
./artifacts/bin/linux/Release/tcpquic-proxy
```

Expected: prints usage / not-yet-implemented message, exit 0 or 1.

---

### Task 2: Control protocol encode/decode

**Files:**
- Create: `src/tools/tcpquic-proxy/control_protocol.h`
- Create: `src/tools/tcpquic-proxy/control_protocol.cpp`
- Create: `src/tools/tcpquic-proxy/unittest/control_protocol_test.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt`

- [ ] **Step 1: Define header**

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

static constexpr uint8_t TQ_MAGIC_0 = 0x54;
static constexpr uint8_t TQ_MAGIC_1 = 0x51;
static constexpr uint8_t TQ_VERSION = 0x01;
static constexpr uint8_t TQ_CMD_OPEN = 0x01;
static constexpr uint8_t TQ_CMD_OPEN_OK = 0x02;
static constexpr uint8_t TQ_CMD_OPEN_FAIL = 0x03;

static constexpr uint8_t TQ_ADDR_IPV4 = 0x01;
static constexpr uint8_t TQ_ADDR_IPV6 = 0x02;
static constexpr uint8_t TQ_ADDR_DOMAIN = 0x03;

static constexpr uint8_t TQ_FLAG_COMPRESS = 0x01;
static constexpr uint8_t TQ_FLAG_COMPRESS_LZ4 = 0x02;
static constexpr uint8_t TQ_FLAG_DNS_REMOTE = 0x04;

enum class TqOpenError : uint8_t {
    Ok = 0x00,
    AclDenied = 0x01,
    DnsFailed = 0x02,
    TcpTimeout = 0x03,
    TcpRefused = 0x04,
    Internal = 0x05,
};

struct TqOpenRequest {
    uint8_t Flags{};
    uint8_t AddrType{};
    uint16_t Port{};
    std::vector<uint8_t> Addr; // raw IP bytes or domain octets
};

struct TqOpenResponse {
    bool Ok{};
    TqOpenError Error{TqOpenError::Ok};
    uint32_t ConnId{};
};

bool TqEncodeOpenRequest(const TqOpenRequest& req, std::vector<uint8_t>& out);
bool TqDecodeOpenRequest(const uint8_t* data, size_t len, TqOpenRequest& out);
bool TqEncodeOpenResponse(const TqOpenResponse& resp, std::vector<uint8_t>& out);
bool TqDecodeOpenResponse(const uint8_t* data, size_t len, TqOpenResponse& out);
```

- [ ] **Step 2: Implement encode/decode in control_protocol.cpp**

Use big-endian helpers:

```cpp
static void WriteU16BE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v & 0xFF));
}
static uint16_t ReadU16BE(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
```

`TqEncodeOpenRequest` layout per spec §4.2; reject IPv4 addr length other than 4, IPv6 addr length other than 16, DOMAIN addr length outside 1..255, and DOMAIN requests without `TQ_FLAG_DNS_REMOTE`.

- [ ] **Step 3: Write unit test**

```cpp
#include "../control_protocol.h"
#include <cassert>
#include <cstring>

int main() {
    TqOpenRequest req{};
    req.Flags = TQ_FLAG_COMPRESS | TQ_FLAG_DNS_REMOTE;
    req.AddrType = TQ_ADDR_DOMAIN;
    req.Port = 3306;
    const char* host = "db.internal";
    req.Addr.assign(host, host + strlen(host));

    std::vector<uint8_t> buf;
    assert(TqEncodeOpenRequest(req, buf));

    TqOpenRequest decoded{};
    assert(TqDecodeOpenRequest(buf.data(), buf.size(), decoded));
    assert(decoded.Port == 3306);
    assert(decoded.Flags == (TQ_FLAG_COMPRESS | TQ_FLAG_DNS_REMOTE));
    assert(decoded.AddrType == TQ_ADDR_DOMAIN);
    assert(decoded.Addr.size() == strlen(host));

    TqOpenResponse ok{true, TqOpenError::Ok, 0};
    std::vector<uint8_t> rbuf;
    assert(TqEncodeOpenResponse(ok, rbuf));
    TqOpenResponse rdec{};
    assert(TqDecodeOpenResponse(rbuf.data(), rbuf.size(), rdec));
    assert(rdec.Ok);

    return 0;
}
```

- [ ] **Step 4: Add test target to CMakeLists.txt**

```cmake
add_executable(tcpquic_control_test unittest/control_protocol_test.cpp control_protocol.cpp)
target_include_directories(tcpquic_control_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 5: Run test**

```bash
cmake --build . --target tcpquic_control_test -j$(nproc)
./artifacts/bin/linux/Release/tcpquic_control_test
```

Expected: exit 0.

---

### Task 3: ACL engine

**Files:**
- Create: `src/tools/tcpquic-proxy/acl.h`
- Create: `src/tools/tcpquic-proxy/acl.cpp`
- Create: `src/tools/tcpquic-proxy/unittest/acl_test.cpp`

- [ ] **Step 1: Define ACL API**

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct TqAcl {
    std::vector<std::string> AllowCidrs;
    std::vector<std::string> DenyCidrs;
    bool IsAllowed(const std::string& host, uint16_t port) const;
};
```

- [ ] **Step 2: Implement IsAllowed**

Logic:
1. Resolve `host` to IPv4/IPv6; IP literals are normalized directly, DOMAIN names are resolved on B side
2. Evaluate every resolved A/AAAA candidate; if any candidate matches deny CIDR → false
3. If allow list empty → false (secure default)
4. If every dial candidate is outside allow CIDR → false
5. Else return true and expose only the allowed candidates to the TCP dialer

Use simple CIDR match (convert to network + mask). DOMAIN targets must not be dialed until their resolved candidates have passed ACL evaluation.

- [ ] **Step 3: Unit test**

```cpp
int main() {
    TqAcl acl;
    acl.AllowCidrs = {"10.0.0.0/8", "192.168.1.0/24"};
    acl.DenyCidrs = {"10.0.0.1/32"};
    assert(acl.IsAllowed("10.0.0.50", 80));
    assert(!acl.IsAllowed("10.0.0.1", 80));
    assert(!acl.IsAllowed("8.8.8.8", 53));
    return 0;
}
```

- [ ] **Step 4: Build and run**

```bash
cmake --build . --target tcpquic_acl_test -j$(nproc)
./artifacts/bin/linux/Release/tcpquic_acl_test
```

---

### Task 4: Compression wrapper (zstd)

**Files:**
- Create: `src/tools/tcpquic-proxy/compress.h`
- Create: `src/tools/tcpquic-proxy/compress.cpp`

- [ ] **Step 1: Define streaming compressor interface**

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <memory>

enum class TqCompressAlgo { None, Zstd, Lz4 };

struct ITqCompressor {
    virtual ~ITqCompressor() = default;
    virtual bool Compress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, bool flush) = 0;
    virtual void Reset() = 0;
};

struct ITqDecompressor {
    virtual ~ITqDecompressor() = default;
    virtual bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) = 0;
    virtual void Reset() = 0;
};

std::unique_ptr<ITqCompressor> TqCreateCompressor(TqCompressAlgo algo, int level);
std::unique_ptr<ITqDecompressor> TqCreateDecompressor(TqCompressAlgo algo);
```

- [ ] **Step 2: Implement zstd backend** (guard with `#ifdef TCPQUIC_HAS_ZSTD`)

Use `ZSTD_createCCtx` / `ZSTD_compressStream2` and `ZSTD_createDCtx` / `ZSTD_decompressStream`.

- [ ] **Step 3: Round-trip unit test**

Compress `"hello world repeated..."` × 100, decompress, assert equality.

---

### Task 5: Config / CLI parsing

**Files:**
- Create: `src/tools/tcpquic-proxy/config.h`
- Create: `src/tools/tcpquic-proxy/config.cpp`

- [ ] **Step 1: Define config struct**

```cpp
enum class TqMode { Client, Server };

struct TqConfig {
    TqMode Mode{};
    std::string SocksListen = "127.0.0.1:1080";
    std::string HttpListen = "127.0.0.1:8080";
    std::string QuicPeer;          // client
    std::string QuicListen;        // server
    std::string QuicCert;
    std::string QuicKey;
    std::string QuicCa;
    uint32_t QuicConnections = 1;
    std::string Compress = "auto";
    int CompressLevel = 1;
    std::vector<std::string> AllowTargets;
    std::vector<std::string> DenyTargets;
};

bool TqParseArgs(int argc, char** argv, TqConfig& cfg, std::string& err);
```

- [ ] **Step 2: Implement minimal getopt-style parser**

Reject `--quic-connections > 1` in v1 with clear error: "connection pool not yet supported". Do not accept `--compress-min-size`; compression state is negotiated in the OPEN flags and remains fixed for the Stream data phase per spec §6.

- [ ] **Step 3: Wire into main.cpp**

```cpp
TqConfig cfg;
std::string err;
if (!TqParseArgs(argc, argv, cfg, err)) {
    fprintf(stderr, "Error: %s\n", err.c_str());
    return 1;
}
```

---

### Task 6: Relay engine (from forwarder pattern)

**Files:**
- Create: `src/tools/tcpquic-proxy/relay.h`
- Create: `src/tools/tcpquic-proxy/relay.cpp`

- [ ] **Step 1: Port ForwardedSend pattern from forwarder.cpp**

Copy/adapt `ForwardedSend` struct and buffered send logic.

- [ ] **Step 2: Implement TcpToStreamRelay**

```cpp
void RelayTcpToStream(int tcpFd, MsQuicStream* stream, ITqCompressor* compressor);
void RelayStreamToTcp(MsQuicStream* stream, int tcpFd, ITqDecompressor* decompressor);
```

- [ ] **Step 3: Use non-blocking TCP + msquic callbacks**

Read TCP via `recv`; on `EAGAIN`, register with datapath/epoll if needed, or use dedicated relay thread per tunnel (v1 acceptable: one thread per active tunnel for simplicity).

**v1 pragmatic choice:** per-tunnel thread doing `select` on TCP fd + blocking stream ops wrapped in msquic callbacks is complex; use **two threads per tunnel** (tcp→stream, stream→tcp) for v1 YAGNI.

---

### Task 7: QUIC session (client + server)

**Files:**
- Create: `src/tools/tcpquic-proxy/quic_session.h`
- Create: `src/tools/tcpquic-proxy/quic_session.cpp`

- [ ] **Step 1: Create QuicApp base**

Hold `MsQuicApi`, `MsQuicRegistration`, `MsQuicConfiguration`, `QUIC_SETTINGS` with BBR + windows from spec §7.

- [ ] **Step 2: Client session**

```cpp
class QuicClientSession {
public:
    bool Start(const TqConfig& cfg);
    MsQuicConnection* GetConnection();
    void EnsureConnected(); // reconnect loop
};
```

Connect to `--quic-peer` with client cert + CA.

- [ ] **Step 3: Server session**

```cpp
class QuicServerSession {
public:
    bool Start(const TqConfig& cfg, std::function<void(MsQuicConnection*)> onConnection);
};
```

Listener with `QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION`.

- [ ] **Step 4: Set ALPN `tcpquic-tunnel/1`**

In `MsQuicConfiguration` constructor.

---

### Task 8: TCP tunnel state machine

**Files:**
- Create: `src/tools/tcpquic-proxy/tcp_tunnel.h`
- Create: `src/tools/tcpquic-proxy/tcp_tunnel.cpp`

- [ ] **Step 1: Define TcpTunnel**

```cpp
class TcpTunnel {
public:
    enum class State { Opening, Active, Closing, Closed };
    bool BeginOpen(MsQuicConnection* conn, const TqOpenRequest& req);
    void OnOpenResponse(const TqOpenResponse& resp);
    void AttachTcp(int fd);
    void Shutdown();
};
```

- [ ] **Step 2: Client flow**

1. `StreamOpen` + `StreamStart`
2. Send encoded OPEN on stream
3. Wait for OPEN_OK/FAIL (timeout 10s)
4. On OK, start relay threads

- [ ] **Step 3: Server flow**

On `PEER_STREAM_STARTED`:
1. Read OPEN frame from first receive
2. Resolve DOMAIN targets on B side and run ACL over all A/AAAA candidates before dialing
3. ACL check → OPEN_FAIL or TCP dial
4. On success, send OPEN_OK, start relay

---

### Task 9: TCP dialer (server)

**Files:**
- Create: `src/tools/tcpquic-proxy/tcp_dialer.h`
- Create: `src/tools/tcpquic-proxy/tcp_dialer.cpp`

- [ ] **Step 1: Implement Connect**

```cpp
enum class TqDialResult { Ok, DnsFailed, AclDenied, Timeout, Refused, Error };

struct TqDialOutcome {
    TqDialResult Result;
    int Fd = -1;
};

TqDialOutcome TqDialTcp(const TqOpenRequest& req, uint32_t timeoutMs = 10000);
```

- [ ] **Step 2: DNS on B side**

For `TQ_ADDR_DOMAIN`, call `getaddrinfo` without `AI_NUMERICHOST`, pass all A/AAAA candidates through `TqAcl`, and connect only to addresses that survived ACL filtering. If DNS fails, return `DnsFailed`; if any resolved candidate is denied or no candidate is allowed, return `AclDenied` so the server sends `OPEN_FAIL(0x01)` instead of attempting TCP connect.

- [ ] **Step 3: Non-blocking connect with timeout**

Use `fcntl(O_NONBLOCK)` + `select` or `poll`.

---

### Task 10: SOCKS5 server

**Files:**
- Create: `src/tools/tcpquic-proxy/socks5_server.h`
- Create: `src/tools/tcpquic-proxy/socks5_server.cpp`

- [ ] **Step 1: Implement minimal SOCKS5 handshake**

```
Client: VER(5) NMETHODS METHODS
Server: VER(5) METHOD(0=NO AUTH)
Client: VER CMD RSV ATYP DST PORT
Server: VER REP RSV ATYP BND ADDR BND PORT
```

- [ ] **Step 2: Parse CONNECT to TunnelRequest**

Support ATYP 1 (IPv4), 3 (domain), 4 (IPv6).

- [ ] **Step 3: Callback into TunnelCore**

```cpp
using TunnelStartFn = std::function<bool(const TunnelRequest&, int clientTcpFd)>;
void RunSocks5Server(const std::string& listen, TunnelStartFn onTunnel);
```

- [ ] **Step 4: Map OPEN_FAIL to SOCKS REP**

| Error | REP |
|-------|-----|
| ACL | 0x02 |
| DNS | 0x04 |
| Timeout/Refused | 0x05 |
| Internal | 0x01 |

---

### Task 11: HTTP CONNECT server

**Files:**
- Create: `src/tools/tcpquic-proxy/http_connect_server.h`
- Create: `src/tools/tcpquic-proxy/http_connect_server.cpp`

- [ ] **Step 1: Parse CONNECT request line**

```
CONNECT host:port HTTP/1.1\r\n
... headers ...
\r\n
```

- [ ] **Step 2: Respond**

Success: `HTTP/1.1 200 Connection Established\r\n\r\n`
Failure: map error_code to 403/502/504/500 per spec §4.3.

- [ ] **Step 3: Share TunnelStartFn with SOCKS5**

---

### Task 12: Wire main client/server modes

**Files:**
- Modify: `src/tools/tcpquic-proxy/main.cpp`
- Modify: `src/tools/tcpquic-proxy/CMakeLists.txt` (add all sources)

- [ ] **Step 1: Client main**

```cpp
static bool RunClient(const TqConfig& cfg) {
    QuicClientSession quic;
    if (!quic.Start(cfg)) return false;
    auto startTunnel = [&](const TunnelRequest& req, int fd) {
        return StartClientTunnel(quic.GetConnection(), req, fd, cfg);
    };
    std::thread socks([&]{ RunSocks5Server(cfg.SocksListen, startTunnel); });
    std::thread http([&]{ RunHttpConnectServer(cfg.HttpListen, startTunnel); });
    socks.join(); http.join();
    return true;
}
```

- [ ] **Step 2: Server main**

```cpp
static bool RunServer(const TqConfig& cfg) {
    TqAcl acl;
    acl.AllowCidrs = cfg.AllowTargets;
    acl.DenyCidrs = cfg.DenyTargets;
    QuicServerSession quic;
    return quic.Start(cfg, [&](MsQuicConnection* conn) {
        // connection callback registers PEER_STREAM_STARTED → server tunnel
    });
}
```

- [ ] **Step 3: Full build**

```bash
cmake --build . --target tcpquic-proxy -j$(nproc)
```

---

### Task 13: Integration smoke test script

**Files:**
- Create: `scripts/test-tcpquic-proxy.sh`

- [ ] **Step 1: Write script**

```bash
#!/usr/bin/env bash
set -euo pipefail
# 1. Generate test certs (openssl req ...)
# 2. Start netcat TCP server on 15001
# 3. Start tcpquic-proxy server
# 4. Start tcpquic-proxy client
# 5. curl --proxy http://127.0.0.1:8080 http://127.0.0.1:15001/ (via tunnel target)
# 6. echo via nc --proxy socks5://127.0.0.1:1080
```

- [ ] **Step 2: Run**

```bash
chmod +x scripts/test-tcpquic-proxy.sh
./scripts/test-tcpquic-proxy.sh
```

Expected: both HTTP CONNECT and SOCKS5 succeed.

---

### Task 14: Performance baseline (optional v1.1)

**Files:**
- Create: `scripts/bench-tcpquic-proxy.sh`

- [x] **Step 1: Reuse netem from existing bench scripts**

Compare:
- bare TCP iperf/nc
- tcpquic-proxy without compression
- tcpquic-proxy with zstd

- [x] **Step 2: Record in `research_progress.md`**

---

## Plan Self-Review

| Spec requirement | Task |
|------------------|------|
| SOCKS5 + HTTP CONNECT | Task 10, 11, 12 |
| Dynamic target / B-side dial | Task 8, 9 |
| Control protocol | Task 2 |
| mTLS | Task 7 |
| ACL | Task 3, 9 |
| Compression zstd | Task 4 |
| Single QUIC connection | Task 7 (`quic-connections=1` enforced in Task 5) |
| Relay / forwarder pattern | Task 6 |
| Integration tests | Task 13 |
| Performance | Task 14 |

No TBD placeholders remain. Type names consistent (`TqOpenRequest`, `TunnelRequest` alias in socks/http).

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-06-tcpquic-proxy.md`. Two execution options:

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
Which approach?
