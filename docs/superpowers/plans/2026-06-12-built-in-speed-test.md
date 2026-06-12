# Built-In Speed Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add built-in authenticated upload/download speed tests that exercise the existing TCP-over-QUIC tunnel data path without requiring curl or an external server.

**Architecture:** The client opens a QUIC control stream to ask the server to start a temporary loopback TCP test server. The client then opens normal tunnel streams to the returned loopback address, pumps bytes through local socket pairs, sends `SPEED_FINISH`, and reports both local and server-side byte counts. Server incoming streams are first handled by a small dispatcher that routes `TQ_CMD_OPEN` to the existing tunnel context and `TQ_CMD_SPEED_START` to the speed-test control handler.

**Tech Stack:** C++17, MsQuic bidirectional streams, existing `TqSocketPair`, `platform_socket.*`, `TqStartClientTunnel`, `TqHandleServerPeerStream`, CMake unit test targets.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/config/config.h` | Add `TqSpeedTestMode`, `SpeedTestMode`, `SpeedTestDurationSec`. |
| `src/config/config.cpp` | Parse and validate `--download-test <sec>` / `--upload-test <sec>`. |
| `src/protocol/control_protocol.h` | Add speed-test command constants, structs, error enum, fixed-size constants, encode/decode declarations. |
| `src/protocol/control_protocol.cpp` | Implement speed-test wire encoding/decoding with strict field validation. |
| `src/runtime/speed_test.h` | Public API for client runner, server controller, and ephemeral target authorizer. |
| `src/runtime/speed_test.cpp` | Control stream handler, temporary loopback TCP server, client pump workers, result formatting. |
| `src/tunnel/tcp_tunnel.h` | Replace server peer stream entry with incoming stream dispatcher API and optional ephemeral target authorizer. |
| `src/tunnel/tcp_tunnel.cpp` | Implement dispatcher, initial-byte handoff to existing tunnel context, and ACL bypass for active speed-test loopback ports. |
| `src/main.cpp` | Wire client speed mode and server speed controller into normal startup. |
| `src/CMakeLists.txt` | Add `speed_test.cpp` to proxy sources and new unit test targets. |
| `src/unittest/control_protocol_test.cpp` | Add speed message round-trip and invalid input tests. |
| `src/unittest/tuning_test.cpp` | Add CLI parsing and validation tests. |
| `src/unittest/speed_test_test.cpp` | Add local speed server/controller unit tests. |
| `src/unittest/tcp_tunnel_test.cpp` | Add dispatcher OPEN handoff and ephemeral ACL authorization tests. |
| `README.md` | Document built-in speed test usage and caveats. |

---

### Task 1: Config Parsing

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Add failing config tests**

Add cases to `src/unittest/tuning_test.cpp`:

```cpp
{
    TqConfig cfg{};
    std::string err;
    char arg0[] = "tcpquic-proxy";
    char arg1[] = "client";
    char arg2[] = "--quic-peer";
    char arg3[] = "127.0.0.1:4433";
    char arg4[] = "--quic-cert";
    char arg5[] = "cert.pem";
    char arg6[] = "--quic-key";
    char arg7[] = "key.pem";
    char arg8[] = "--quic-ca";
    char arg9[] = "ca.pem";
    char arg10[] = "--download-test";
    char arg11[] = "10";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
    assert(TqParseArgs(12, argv, cfg, err));
    assert(cfg.SpeedTestMode == TqSpeedTestMode::Download);
    assert(cfg.SpeedTestDurationSec == 10);
}
{
    TqConfig cfg{};
    std::string err;
    char arg0[] = "tcpquic-proxy";
    char arg1[] = "client";
    char arg2[] = "--quic-peer";
    char arg3[] = "127.0.0.1:4433";
    char arg4[] = "--quic-cert";
    char arg5[] = "cert.pem";
    char arg6[] = "--quic-key";
    char arg7[] = "key.pem";
    char arg8[] = "--quic-ca";
    char arg9[] = "ca.pem";
    char arg10[] = "--upload-test";
    char arg11[] = "5";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
    assert(TqParseArgs(12, argv, cfg, err));
    assert(cfg.SpeedTestMode == TqSpeedTestMode::Upload);
    assert(cfg.SpeedTestDurationSec == 5);
}
```

Also add negative cases for both flags together, zero seconds, server mode, `--client-config`, and `--warmup-mb`.

- [ ] **Step 2: Run test and verify it fails**

Run: `rtk cmake --build build --target tcpquic_tuning_test -j2 && rtk ./build/bin/Release/tcpquic_tuning_test`

Expected: compile fails because `TqSpeedTestMode` / `SpeedTestDurationSec` do not exist.

- [ ] **Step 3: Implement config fields and parsing**

Add to `src/config/config.h`:

```cpp
enum class TqSpeedTestMode {
    None,
    Download,
    Upload,
};
```

Add to `TqConfig`:

```cpp
TqSpeedTestMode SpeedTestMode{TqSpeedTestMode::None};
uint32_t SpeedTestDurationSec{0};
```

In `TqPrintUsage()`, add:

```text
  --download-test <sec>       Client: built-in end-to-end download speed test
  --upload-test <sec>         Client: built-in end-to-end upload speed test
```

In `TqParseArgs()`, parse both flags as `uint32_t` in range `1..86400`, reject both being set, reject server mode, reject `--client-config`, and reject `WarmupMb > 0`.

- [ ] **Step 4: Run test and verify it passes**

Run: `rtk cmake --build build --target tcpquic_tuning_test -j2 && rtk ./build/bin/Release/tcpquic_tuning_test`

Expected: exit code 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/config/config.h src/config/config.cpp src/unittest/tuning_test.cpp
rtk git commit -m "feat: add speed test cli options"
```

---

### Task 2: Speed Control Protocol

**Files:**
- Modify: `src/protocol/control_protocol.h`
- Modify: `src/protocol/control_protocol.cpp`
- Modify: `src/unittest/control_protocol_test.cpp`

- [ ] **Step 1: Add failing protocol tests**

Extend `src/unittest/control_protocol_test.cpp` with round-trip tests for:

```cpp
TqSpeedStart start{};
start.SessionId = 7;
start.Direction = TqSpeedDirection::Download;
start.DurationSec = 10;
start.Parallel = 4;
```

```cpp
TqSpeedReady ready{};
ready.SessionId = 7;
ready.AddrType = TQ_ADDR_IPV4;
ready.Port = 54321;
ready.Addr = {127, 0, 0, 1};
```

```cpp
TqSpeedFinish finish{};
finish.SessionId = 7;
finish.ClientBytes = 123456789;
finish.ClientElapsedUs = 10000001;
```

```cpp
TqSpeedResult result{};
result.SessionId = 7;
result.ServerBytes = 123456000;
result.ServerElapsedUs = 10000002;
result.AcceptedConnections = 4;
result.ClosedConnections = 4;
result.Status = 0;
```

```cpp
TqSpeedErrorMessage error{};
error.SessionId = 7;
error.Error = TqSpeedError::InvalidRequest;
error.Message = "bad speed request";
```

Also test invalid direction, `duration_sec == 0`, `parallel == 0`, non-zero flags, and READY IPv4 with `addr_len != 4`.

- [ ] **Step 2: Run test and verify it fails**

Run: `rtk cmake --build build --target tcpquic_control_test -j2 && rtk ./build/bin/Release/tcpquic_control_test`

Expected: compile fails because speed protocol symbols do not exist.

- [ ] **Step 3: Implement protocol structs and helpers**

Add command constants:

```cpp
static constexpr uint8_t TQ_CMD_SPEED_START = 0x10;
static constexpr uint8_t TQ_CMD_SPEED_READY = 0x11;
static constexpr uint8_t TQ_CMD_SPEED_FINISH = 0x12;
static constexpr uint8_t TQ_CMD_SPEED_RESULT = 0x13;
static constexpr uint8_t TQ_CMD_SPEED_ERROR = 0x14;
```

Add strict encode/decode functions:

```cpp
bool TqEncodeSpeedStart(const TqSpeedStart& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedStart(const uint8_t* data, size_t len, TqSpeedStart& out);
bool TqEncodeSpeedReady(const TqSpeedReady& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedReady(const uint8_t* data, size_t len, TqSpeedReady& out);
bool TqEncodeSpeedFinish(const TqSpeedFinish& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedFinish(const uint8_t* data, size_t len, TqSpeedFinish& out);
bool TqEncodeSpeedResult(const TqSpeedResult& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedResult(const uint8_t* data, size_t len, TqSpeedResult& out);
bool TqEncodeSpeedError(const TqSpeedErrorMessage& msg, std::vector<uint8_t>& out);
bool TqDecodeSpeedError(const uint8_t* data, size_t len, TqSpeedErrorMessage& out);
```

Keep all integers big-endian, cap error message length at 1024 bytes, and require exact input lengths on decode.

- [ ] **Step 4: Run test and verify it passes**

Run: `rtk cmake --build build --target tcpquic_control_test -j2 && rtk ./build/bin/Release/tcpquic_control_test`

Expected: exit code 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/protocol/control_protocol.h src/protocol/control_protocol.cpp src/unittest/control_protocol_test.cpp
rtk git commit -m "feat: add speed test control protocol"
```

---

### Task 3: Speed Test Runtime

**Files:**
- Create: `src/runtime/speed_test.h`
- Create: `src/runtime/speed_test.cpp`
- Create: `src/unittest/speed_test_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add failing runtime unit test**

Create `src/unittest/speed_test_test.cpp` with tests for:

```cpp
TqServerSpeedTestController controller;
TqSpeedStart start{};
start.SessionId = 1;
start.Direction = TqSpeedDirection::Upload;
start.DurationSec = 2;
start.Parallel = 1;
TqSpeedReady ready{};
assert(controller.StartSession(start, ready));
assert(ready.Port != 0);
assert(ready.AddrType == TQ_ADDR_IPV4);
assert(ready.Addr == std::vector<uint8_t>({127, 0, 0, 1}));
assert(controller.IsAllowedEphemeralTarget("127.0.0.1", ready.Port));
TqSpeedResult result{};
assert(controller.FinishSession(1, 0, 0, result));
assert(!controller.IsAllowedEphemeralTarget("127.0.0.1", ready.Port));
```

Add a second test that connects a local TCP client to `ready.Port`, sends 1 MiB in upload mode, finishes the session, and asserts `result.ServerBytes == 1024 * 1024`.

- [ ] **Step 2: Run test and verify it fails**

Run: `rtk cmake --build build --target tcpquic_speed_test_test -j2`

Expected: target or headers do not exist.

- [ ] **Step 3: Implement server controller**

In `speed_test.h`, expose:

```cpp
class TqEphemeralTargetAuthorizer {
public:
    virtual ~TqEphemeralTargetAuthorizer() = default;
    virtual bool IsAllowedEphemeralTarget(const std::string& host, uint16_t port) const = 0;
};

class TqServerSpeedTestController final : public TqEphemeralTargetAuthorizer {
public:
    ~TqServerSpeedTestController();
    bool StartSession(const TqSpeedStart& start, TqSpeedReady& ready);
    bool FinishSession(uint32_t sessionId, uint64_t clientBytes, uint64_t clientElapsedUs, TqSpeedResult& result);
    void StopAll();
    bool IsAllowedEphemeralTarget(const std::string& host, uint16_t port) const override;
};
```

In `speed_test.cpp`, implement an IPv4 loopback listener using POSIX/Windows socket APIs guarded by `#if defined(_WIN32)`. Use blocking accept and one worker thread per accepted TCP connection. Upload workers `recv()` and count bytes; download workers `send()` a reusable 64 KiB buffer and count bytes. `FinishSession()` stops the listener, shuts down accepted sockets, joins session threads, fills `TqSpeedResult`, and removes the ephemeral target.

- [ ] **Step 4: Add CMake target**

Add `runtime/speed_test.cpp` to `TCPQUIC_PROXY_SOURCES`, and add:

```cmake
add_executable(tcpquic_speed_test_test
    unittest/speed_test_test.cpp
    runtime/speed_test.cpp
    protocol/control_protocol.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_speed_test_test)
target_link_libraries(tcpquic_speed_test_test Threads::Threads)
set_property(TARGET tcpquic_speed_test_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_speed_test_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

- [ ] **Step 5: Run test and verify it passes**

Run: `rtk cmake --build build --target tcpquic_speed_test_test -j2 && rtk ./build/bin/Release/tcpquic_speed_test_test`

Expected: exit code 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/speed_test.h src/runtime/speed_test.cpp src/unittest/speed_test_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add speed test runtime"
```

---

### Task 4: Server Incoming Stream Dispatcher

**Files:**
- Modify: `src/tunnel/tcp_tunnel.h`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`

- [ ] **Step 1: Add failing dispatcher tests**

Extend `src/unittest/tcp_tunnel_test.cpp` with a fake stream callback test that sends an encoded OPEN request into the new incoming dispatcher and verifies the existing ACL/open path still runs. Add an ephemeral target test where normal ACL denies `127.0.0.1`, but a fake `TqEphemeralTargetAuthorizer` allows the exact port and the OPEN is not counted as ACL denied.

- [ ] **Step 2: Run test and verify it fails**

Run: `rtk cmake --build build --target tcpquic_tcp_tunnel_test -j2 && rtk ./build/bin/Release/tcpquic_tcp_tunnel_test`

Expected: compile fails because `TqHandleServerIncomingStream()` does not exist.

- [ ] **Step 3: Implement dispatcher and initial-byte handoff**

Replace public server stream entry with:

```cpp
void TqHandleServerIncomingStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqServerSpeedTestController* speed,
    TqTunnelCompletionFn onComplete = {},
    TqTunnelAclDeniedFn onAclDenied = {});
```

Keep `TqHandleServerPeerStream()` as a compatibility wrapper that calls the new function with `speed == nullptr`.

Inside `tcp_tunnel.cpp`:

- Add `TqIncomingStreamDispatchContext`.
- Accumulate receive bytes until at least 4 bytes are available.
- Validate magic/version.
- If `cmd == TQ_CMD_OPEN`, create a `TqTunnelContext`, call `AdoptInitialBytes(OpeningRx)`, switch the existing `MsQuicStream` callback/context to `TqTunnelContext::Callback`, and call `TryHandleServerOpen()`.
- If `cmd == TQ_CMD_SPEED_START`, pass the stream to a speed control handler.
- Otherwise send `SPEED_ERROR Unsupported` when possible, then abort the stream.

In `TryHandleServerOpen()`, before normal ACL resolution, check:

```cpp
if (EphemeralAuthorizer != nullptr &&
    EphemeralAuthorizer->IsAllowedEphemeralTarget(host, req.Port)) {
    addrs = loopback sockaddr for host/port;
    allowed = true;
}
```

Only exact IPv4/IPv6 loopback literals may use this bypass.

- [ ] **Step 4: Run test and verify it passes**

Run: `rtk cmake --build build --target tcpquic_tcp_tunnel_test -j2 && rtk ./build/bin/Release/tcpquic_tcp_tunnel_test`

Expected: exit code 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/tcp_tunnel.h src/tunnel/tcp_tunnel.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "feat: dispatch server control streams"
```

---

### Task 5: Client Runner and Main Wiring

**Files:**
- Modify: `src/runtime/speed_test.h`
- Modify: `src/runtime/speed_test.cpp`
- Modify: `src/main.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add client runner API**

Expose:

```cpp
bool TqRunClientSpeedTest(QuicClientSession& quic, const TqConfig& cfg);
void TqHandleServerSpeedControlStream(TqServerSpeedTestController& controller, MsQuicConnection* conn, HQUIC rawStream);
```

- [ ] **Step 2: Implement client control stream**

`TqRunClientSpeedTest()` should:

1. Call `quic.EnsureAnyConnected()`.
2. Pick one connected QUIC connection.
3. Open a client-initiated bidirectional control stream.
4. Send `SPEED_START`.
5. Wait for `SPEED_READY`.
6. Create `cfg.QuicConnections` socket pairs.
7. For each pair, call `TqStartClientTunnel(quic.PickConnection(), req, tunnelFd, cfg)`.
8. Run upload/download pump workers on the other socket.
9. Send `SPEED_FINISH`.
10. Wait for `SPEED_RESULT`.
11. Print local/server bytes, elapsed seconds, Gbps, MiB/s, accepted/closed counts.

Use server bytes as the primary upload result; warn and return false when local/server bytes differ by more than 1% or 16 MiB.

- [ ] **Step 3: Wire main**

In `RunSinglePeerClient()`, after QUIC startup and before opening SOCKS/HTTP listeners:

```cpp
if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
    const bool ok = TqRunClientSpeedTest(quic, cfg);
    quic.Stop();
    return ok ? 0 : 1;
}
```

In `RunServer()`, create:

```cpp
auto speed = std::make_shared<TqServerSpeedTestController>();
```

Pass `speed.get()` into `TqHandleServerIncomingStream()`. Ensure `speed->StopAll()` runs before server exit.

- [ ] **Step 4: Build**

Run: `rtk cmake --build build --target tcpquic-proxy tcpquic_speed_test_test tcpquic_tcp_tunnel_test tcpquic_control_test tcpquic_tuning_test -j2`

Expected: all targets build successfully.

- [ ] **Step 5: Run tests**

Run:

```bash
rtk ./build/bin/Release/tcpquic_speed_test_test
rtk ./build/bin/Release/tcpquic_tcp_tunnel_test
rtk ./build/bin/Release/tcpquic_control_test
rtk ./build/bin/Release/tcpquic_tuning_test
```

Expected: all exit code 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/speed_test.h src/runtime/speed_test.cpp src/main.cpp src/CMakeLists.txt
rtk git commit -m "feat: wire built-in speed test mode"
```

---

### Task 6: Documentation and End-to-End Verification

**Files:**
- Modify: `README.md`
- Create: `docs/built-in-speed-test-20260612.md`

- [ ] **Step 1: Update README**

Add examples:

```bash
tcpquic-proxy client --quic-peer 127.0.0.1:14443 --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --quic-connections 1 --compress off --download-test 10
tcpquic-proxy client --quic-peer 127.0.0.1:14443 --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --quic-connections 1 --compress off --upload-test 10
tcpquic-proxy client --quic-peer 127.0.0.1:14443 --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --quic-connections 4 --compress lz4 --download-test 10
```

Document that upload throughput should be read from server bytes, not only local write bytes.

- [ ] **Step 2: Run local end-to-end smoke test**

Run a local server/client pair with test certs used by the existing integration workflow:

```bash
rtk ./build/bin/Release/tcpquic-proxy server --quic-listen 127.0.0.1:14443 --quic-cert cert/server/server.crt --quic-key cert/server/server.key --quic-ca cert/ca.crt --allow-targets 10.0.0.0/8
rtk ./build/bin/Release/tcpquic-proxy client --quic-peer 127.0.0.1:14443 --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --download-test 2
rtk ./build/bin/Release/tcpquic-proxy client --quic-peer 127.0.0.1:14443 --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --upload-test 2
```

Expected: both tests print local/server results and exit 0 even though normal ACL does not allow `127.0.0.1/32`, proving the ephemeral target bypass is scoped and active.

- [ ] **Step 3: Run DGX validation**

Run on the two DGX machines:

```bash
DGX_QUIC_PEER="${DGX_QUIC_PEER:?set to host:port}"
rtk ./build/bin/Release/tcpquic-proxy client --quic-peer "$DGX_QUIC_PEER" --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --quic-connections 1 --compress off --download-test 12
rtk ./build/bin/Release/tcpquic-proxy client --quic-peer "$DGX_QUIC_PEER" --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --quic-connections 1 --compress off --upload-test 12
rtk ./build/bin/Release/tcpquic-proxy client --quic-peer "$DGX_QUIC_PEER" --quic-cert cert/client/client.crt --quic-key cert/client/client.key --quic-ca cert/ca.crt --quic-connections 4 --compress off --download-test 12
```

Expected: upload reports server bytes close to local bytes, download reports local/server bytes close, and throughput is within the same order as the current external scripts.

- [ ] **Step 4: Final verification**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy tcpquic_speed_test_test tcpquic_tcp_tunnel_test tcpquic_control_test tcpquic_tuning_test -j2
rtk ./build/bin/Release/tcpquic_speed_test_test
rtk ./build/bin/Release/tcpquic_tcp_tunnel_test
rtk ./build/bin/Release/tcpquic_control_test
rtk ./build/bin/Release/tcpquic_tuning_test
rtk git diff --check
```

Expected: all commands exit 0.

- [ ] **Step 5: Commit**

```bash
rtk git add README.md docs/built-in-speed-test-20260612.md
rtk git commit -m "docs: describe built-in speed test"
```
