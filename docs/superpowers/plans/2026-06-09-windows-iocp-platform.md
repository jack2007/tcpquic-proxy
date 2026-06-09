# Windows IOCP Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run native Windows 10/11 x64 `tcpquic-proxy.exe` with MSVC 2022, PEM mTLS, shared socket code, and a production IOCP relay backend.

**Architecture:** Add a narrow platform socket layer used by shared listener, dialer, tunnel, and admin code. Keep Linux relay on the existing epoll worker and add a Windows-only IOCP relay worker selected by `relay.cpp` at compile time.

**Tech Stack:** C++17, CMake, MSVC 2022, Winsock2, IOCP, MsQuic, lz4, zstd, existing hand-written unit tests.

---

## File Structure

- Create `src/platform_socket.h`: cross-platform socket type, startup RAII, close/send/recv/shutdown, nonblocking, address conversion, socket options, and loopback socket pair declarations.
- Create `src/platform_socket_posix.cpp`: POSIX implementation using current Linux APIs.
- Create `src/platform_socket_win.cpp`: Winsock implementation using `WSAStartup`, `closesocket`, `ioctlsocket`, `WSAGetLastError`, `InetPtonA`, `InetNtopA`, and loopback TCP socket pair.
- Create `src/windows_relay_worker.h`: Windows relay runtime/worker public interface and stream callback entry points.
- Create `src/windows_relay_worker.cpp`: IOCP worker implementation.
- Create `src/unittest/platform_socket_test.cpp`: cross-platform tests for startup, invalid handles, socket pair, send/recv, shutdown, and nonblocking behavior.
- Create `src/unittest/windows_relay_worker_test.cpp`: Windows-only IOCP relay worker tests.
- Modify `src/CMakeLists.txt`: platform source selection, Windows library linkage, Linux-only test guards, Windows tests.
- Modify `src/relay.h` and `src/relay.cpp`: add `WindowsWorker` backend and `TqSocketHandle` in relay APIs.
- Modify `src/tcp_tunnel.h` and `src/tcp_tunnel.cpp`: replace socket `int` with `TqSocketHandle`, platform close, and backend-neutral relay.
- Modify `src/tcp_dialer.h` and `src/tcp_dialer.cpp`: replace POSIX-only declarations with platform socket types and helpers.
- Modify `src/socks5_server.h/.cpp`, `src/http_connect_server.h/.cpp`, and `src/admin_http.h/.cpp`: replace POSIX calls, migrate listener socket atomics, and remove `dup()` handoff.
- Modify `src/acl.h/.cpp`: include platform socket header and use platform address conversion.
- Modify `src/tcp_write_queue.cpp`, `src/warmup.cpp`, and affected tests: use platform socket helpers.
- Modify `src/quic_session.cpp` and `src/main.cpp`: handle `_isatty` and initialize `TqSocketStartup`.
- Modify `README.md`: document Windows build, test, runtime, and interoperability commands.

## Task 1: Add Platform Socket Layer on Linux

**Files:**
- Create: `src/platform_socket.h`
- Create: `src/platform_socket_posix.cpp`
- Create: `src/unittest/platform_socket_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the platform socket test**

Add `src/unittest/platform_socket_test.cpp`:

```cpp
#include "platform_socket.h"

#include <array>
#include <cassert>
#include <cstring>

int main() {
    TqSocketStartup startup;
    assert(startup.Ok());

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    assert(TqSocketPair(pair));
    assert(TqSocketValid(pair[0]));
    assert(TqSocketValid(pair[1]));

    const char payload[] = "tcpquic-platform-socket";
    assert(TqSend(pair[0], payload, std::strlen(payload), TqSendFlags::None) ==
        static_cast<int>(std::strlen(payload)));

    std::array<char, 64> buffer{};
    const int received = TqRecv(pair[1], buffer.data(), buffer.size(), TqRecvFlags::None);
    assert(received == static_cast<int>(std::strlen(payload)));
    assert(std::memcmp(buffer.data(), payload, std::strlen(payload)) == 0);

    assert(TqSetNonBlocking(pair[0]));
    assert(TqShutdownSend(pair[0]));

    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
    pair[0] = TqInvalidSocket;
    pair[1] = TqInvalidSocket;
    assert(!TqSocketValid(pair[0]));
    return 0;
}
```

- [ ] **Step 2: Add the public platform header**

Create `src/platform_socket.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using TqSocketHandle = SOCKET;
static constexpr TqSocketHandle TqInvalidSocket = INVALID_SOCKET;
#else
#include <netinet/in.h>
#include <sys/socket.h>
using TqSocketHandle = int;
static constexpr TqSocketHandle TqInvalidSocket = -1;
#endif

enum class TqSendFlags : int {
    None = 0,
    NoSignal = 1,
};

enum class TqRecvFlags : int {
    None = 0,
    DontWait = 1,
};

class TqSocketStartup {
public:
    TqSocketStartup();
    ~TqSocketStartup();
    TqSocketStartup(const TqSocketStartup&) = delete;
    TqSocketStartup& operator=(const TqSocketStartup&) = delete;

    bool Ok() const { return Ok_; }

private:
    bool Ok_{false};
};

bool TqSocketValid(TqSocketHandle socket);
void TqCloseSocket(TqSocketHandle socket);
bool TqSetNonBlocking(TqSocketHandle socket);
int TqLastSocketError();
bool TqSocketWouldBlock(int error);
bool TqSocketInProgress(int error);
bool TqSocketInterrupted(int error);
int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags flags);
int TqRecv(TqSocketHandle socket, void* data, size_t length, TqRecvFlags flags);
bool TqShutdownSend(TqSocketHandle socket);
bool TqShutdownBoth(TqSocketHandle socket);
bool TqInetPton(int family, const char* text, void* out);
const char* TqInetNtop(int family, const void* addr, char* text, size_t textLength);
bool TqSetReuseAddr(TqSocketHandle socket);
bool TqSetNoDelay(TqSocketHandle socket);
bool TqSetSocketBuffer(TqSocketHandle socket, int option, int bytes);
bool TqSocketPair(TqSocketHandle out[2]);
```

- [ ] **Step 3: Implement POSIX platform socket functions**

Create `src/platform_socket_posix.cpp`:

```cpp
#include "platform_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

TqSocketStartup::TqSocketStartup() : Ok_(true) {}
TqSocketStartup::~TqSocketStartup() = default;

bool TqSocketValid(TqSocketHandle socket) {
    return socket >= 0;
}

void TqCloseSocket(TqSocketHandle socket) {
    if (TqSocketValid(socket)) {
        (void)::close(socket);
    }
}

bool TqSetNonBlocking(TqSocketHandle socket) {
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

int TqLastSocketError() {
    return errno;
}

bool TqSocketWouldBlock(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool TqSocketInProgress(int error) {
    return error == EINPROGRESS;
}

bool TqSocketInterrupted(int error) {
    return error == EINTR;
}

int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags flags) {
    int nativeFlags = 0;
#ifdef MSG_NOSIGNAL
    if (flags == TqSendFlags::NoSignal) {
        nativeFlags |= MSG_NOSIGNAL;
    }
#else
    (void)flags;
#endif
    return static_cast<int>(::send(socket, data, length, nativeFlags));
}

int TqRecv(TqSocketHandle socket, void* data, size_t length, TqRecvFlags flags) {
    int nativeFlags = 0;
#ifdef MSG_DONTWAIT
    if (flags == TqRecvFlags::DontWait) {
        nativeFlags |= MSG_DONTWAIT;
    }
#else
    (void)flags;
#endif
    return static_cast<int>(::recv(socket, data, length, nativeFlags));
}

bool TqShutdownSend(TqSocketHandle socket) {
    return ::shutdown(socket, SHUT_WR) == 0;
}

bool TqShutdownBoth(TqSocketHandle socket) {
    return ::shutdown(socket, SHUT_RDWR) == 0;
}

bool TqInetPton(int family, const char* text, void* out) {
    return ::inet_pton(family, text, out) == 1;
}

const char* TqInetNtop(int family, const void* addr, char* text, size_t textLength) {
    return ::inet_ntop(family, addr, text, static_cast<socklen_t>(textLength));
}

bool TqSetReuseAddr(TqSocketHandle socket) {
    int enabled = 1;
    return ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0;
}

bool TqSetNoDelay(TqSocketHandle socket) {
    int enabled = 1;
    return ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
}

bool TqSetSocketBuffer(TqSocketHandle socket, int option, int bytes) {
    return bytes <= 0 || ::setsockopt(socket, SOL_SOCKET, option, &bytes, sizeof(bytes)) == 0;
}

bool TqSocketPair(TqSocketHandle out[2]) {
    out[0] = TqInvalidSocket;
    out[1] = TqInvalidSocket;
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, out) == 0;
}
```

- [ ] **Step 4: Wire the test into CMake**

Modify `src/CMakeLists.txt` near the source lists:

```cmake
set(TCPQUIC_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform_socket_win.cpp)
else()
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform_socket_posix.cpp)
endif()
```

Append `${TCPQUIC_PLATFORM_SOURCES}` to `TCPQUIC_PROXY_SOURCES`.

Add the test target before `set(TCPQUIC_TEST_TARGETS ...)`:

```cmake
add_executable(tcpquic_platform_socket_test
    unittest/platform_socket_test.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
target_include_directories(tcpquic_platform_socket_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_platform_socket_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_platform_socket_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(WIN32)
    target_link_libraries(tcpquic_platform_socket_test ws2_32)
endif()
```

Add `tcpquic_platform_socket_test` to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 5: Run the new Linux test**

Run:

```bash
rtk cmake --build build --target tcpquic_platform_socket_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_platform_socket_test
```

Expected: build succeeds and the test exits with status 0.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/platform_socket.h src/platform_socket_posix.cpp src/unittest/platform_socket_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add platform socket layer"
```

## Task 2: Add Windows Socket Implementation and CMake Linkage

**Files:**
- Create: `src/platform_socket_win.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `src/platform_socket.h`

- [ ] **Step 1: Implement Windows platform socket functions**

Create `src/platform_socket_win.cpp`:

```cpp
#include "platform_socket.h"

#include <mstcpip.h>

TqSocketStartup::TqSocketStartup() {
    WSADATA data{};
    Ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

TqSocketStartup::~TqSocketStartup() {
    if (Ok_) {
        ::WSACleanup();
    }
}

bool TqSocketValid(TqSocketHandle socket) {
    return socket != INVALID_SOCKET;
}

void TqCloseSocket(TqSocketHandle socket) {
    if (TqSocketValid(socket)) {
        (void)::closesocket(socket);
    }
}

bool TqSetNonBlocking(TqSocketHandle socket) {
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
}

int TqLastSocketError() {
    return ::WSAGetLastError();
}

bool TqSocketWouldBlock(int error) {
    return error == WSAEWOULDBLOCK;
}

bool TqSocketInProgress(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool TqSocketInterrupted(int error) {
    return error == WSAEINTR;
}

int TqSend(TqSocketHandle socket, const void* data, size_t length, TqSendFlags) {
    return ::send(socket, static_cast<const char*>(data), static_cast<int>(length), 0);
}

int TqRecv(TqSocketHandle socket, void* data, size_t length, TqRecvFlags) {
    return ::recv(socket, static_cast<char*>(data), static_cast<int>(length), 0);
}

bool TqShutdownSend(TqSocketHandle socket) {
    return ::shutdown(socket, SD_SEND) == 0;
}

bool TqShutdownBoth(TqSocketHandle socket) {
    return ::shutdown(socket, SD_BOTH) == 0;
}

bool TqInetPton(int family, const char* text, void* out) {
    return ::InetPtonA(family, text, out) == 1;
}

const char* TqInetNtop(int family, const void* addr, char* text, size_t textLength) {
    return ::InetNtopA(family, const_cast<void*>(addr), text, static_cast<DWORD>(textLength));
}

bool TqSetReuseAddr(TqSocketHandle socket) {
    BOOL enabled = TRUE;
    return ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

bool TqSetNoDelay(TqSocketHandle socket) {
    BOOL enabled = TRUE;
    return ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}

bool TqSetSocketBuffer(TqSocketHandle socket, int option, int bytes) {
    return bytes <= 0 || ::setsockopt(socket, SOL_SOCKET, option,
        reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool TqSocketPair(TqSocketHandle out[2]) {
    out[0] = TqInvalidSocket;
    out[1] = TqInvalidSocket;

    TqSocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(listener)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    int addrLen = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    TqSocketHandle client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(client)) {
        TqCloseSocket(listener);
        return false;
    }

    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        TqCloseSocket(client);
        TqCloseSocket(listener);
        return false;
    }

    TqSocketHandle server = ::accept(listener, nullptr, nullptr);
    TqCloseSocket(listener);
    if (!TqSocketValid(server)) {
        TqCloseSocket(client);
        return false;
    }

    out[0] = client;
    out[1] = server;
    return true;
}
```

- [ ] **Step 2: Add Windows compile definitions and library linkage**

Modify `src/CMakeLists.txt` after `add_tcpquic_executable(tcpquic-proxy ...)`:

```cmake
if(WIN32)
    target_compile_definitions(tcpquic-proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tcpquic-proxy PRIVATE ws2_32)
endif()
```

Also add the same definitions and `ws2_32` link to `tcpquic_platform_socket_test` under its existing `if(WIN32)` block:

```cmake
if(WIN32)
    target_compile_definitions(tcpquic_platform_socket_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tcpquic_platform_socket_test ws2_32)
endif()
```

- [ ] **Step 3: Verify Linux still builds the platform test**

Run:

```bash
rtk cmake --build build --target tcpquic_platform_socket_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_platform_socket_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 4: Verify Windows platform test**

On Windows x64 Developer PowerShell:

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target tcpquic_platform_socket_test
.\build-win\bin\Release\tcpquic_platform_socket_test.exe
```

Expected: CMake config succeeds, build succeeds, test exits 0.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/platform_socket_win.cpp src/platform_socket.h src/CMakeLists.txt
rtk git commit -m "build: add winsock platform socket implementation"
```

## Task 3: Migrate Shared Socket Types and Dialer

**Files:**
- Modify: `src/tcp_dialer.h`
- Modify: `src/tcp_dialer.cpp`
- Modify: `src/acl.h`
- Modify: `src/acl.cpp`
- Modify: `src/unittest/acl_test.cpp`

- [ ] **Step 1: Update dialer public types**

Change `src/tcp_dialer.h` to include platform socket:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include "platform_socket.h"

struct TqDialResult {
    TqSocketHandle Fd{TqInvalidSocket};
    bool Refused{false};
    bool TimedOut{false};
};

TqDialResult TqDialTcp(const std::vector<sockaddr_storage>& addrs, int timeoutMs);
void TqTuneTcpForThroughput(TqSocketHandle fd, int bufferBytes = 0);
```

- [ ] **Step 2: Replace POSIX calls in `tcp_dialer.cpp`**

In `src/tcp_dialer.cpp`, include `platform_socket.h` and keep platform-specific waiting local:

```cpp
#include "tcp_dialer.h"

#include "platform_socket.h"

#include <cerrno>
#include <cstring>

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif
```

Use this pattern for nonblocking setup and close:

```cpp
if (!TqSetNonBlocking(fd)) {
    TqCloseSocket(fd);
    continue;
}
```

Use this pattern for socket creation and failure:

```cpp
TqSocketHandle fd = ::socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
if (!TqSocketValid(fd)) {
    continue;
}
```

Use this platform split for waiting until connect completes:

```cpp
#if defined(_WIN32)
WSAPOLLFD pfd{};
pfd.fd = fd;
pfd.events = POLLOUT;
int pollResult = ::WSAPoll(&pfd, 1, timeoutMs);
#else
pollfd pfd{};
pfd.fd = fd;
pfd.events = POLLOUT;
int pollResult;
do {
    pollResult = ::poll(&pfd, 1, timeoutMs);
} while (pollResult < 0 && TqSocketInterrupted(TqLastSocketError()));
#endif
```

When checking `connect` errors:

```cpp
const int connectResult = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
if (connectResult != 0 && !TqSocketInProgress(TqLastSocketError())) {
    TqCloseSocket(fd);
    continue;
}
```

When tuning:

```cpp
void TqTuneTcpForThroughput(TqSocketHandle fd, int bufferBytes) {
    (void)TqSetNoDelay(fd);
    (void)TqSetSocketBuffer(fd, SO_RCVBUF, bufferBytes);
    (void)TqSetSocketBuffer(fd, SO_SNDBUF, bufferBytes);
}
```

- [ ] **Step 3: Replace address conversion in ACL**

In `src/acl.h`, replace direct socket includes with:

```cpp
#include "platform_socket.h"
```

In `src/acl.cpp`, replace `inet_pton` calls with `TqInetPton`:

```cpp
if (TqInetPton(AF_INET, addr_str.c_str(), &v4)) {
```

and:

```cpp
if (TqInetPton(AF_INET6, addr_str.c_str(), &v6)) {
```

- [ ] **Step 4: Build and run dialer-adjacent tests on Linux**

Run:

```bash
rtk cmake --build build --target tcpquic_acl_test tcpquic_tunnel_test tcpquic_http_connect_test tcpquic_socks5_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_acl_test
rtk ./build/bin/Release/tcpquic_http_connect_test
rtk ./build/bin/Release/tcpquic_socks5_test
```

Expected: all listed commands succeed.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/tcp_dialer.h src/tcp_dialer.cpp src/acl.h src/acl.cpp src/unittest/acl_test.cpp
rtk git commit -m "refactor: migrate dialer and acl to platform sockets"
```

## Task 4: Migrate Tunnel, Relay API, and Socket Ownership

**Files:**
- Modify: `src/relay.h`
- Modify: `src/relay.cpp`
- Modify: `src/tcp_tunnel.h`
- Modify: `src/tcp_tunnel.cpp`
- Modify: `src/socks5_server.h`
- Modify: `src/socks5_server.cpp`
- Modify: `src/http_connect_server.h`
- Modify: `src/http_connect_server.cpp`
- Modify: `src/tcp_write_queue.cpp`

- [ ] **Step 1: Change tunnel and relay signatures**

In `src/tcp_tunnel.h`, change the function types:

```cpp
#include "platform_socket.h"

using TunnelStartFn =
    std::function<TqTunnelStartResult(const TunnelRequest&, TqSocketHandle clientTcpFd)>;
```

and:

```cpp
TqTunnelStartResult TqStartClientTunnel(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg);
```

In `src/relay.h`, update backend and API:

```cpp
class TqLinuxRelayWorker;
class TqWindowsRelayWorker;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
    TqWindowsRelayWorker* WindowsWorker{nullptr};
    uint64_t WindowsRelayId{0};
};

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None);
```

- [ ] **Step 2: Replace close/send/recv/shutdown in tunnel and write queue**

In `src/tcp_tunnel.cpp`, replace:

```cpp
close(clientTcpFd);
```

with:

```cpp
TqCloseSocket(clientTcpFd);
```

Replace `int TcpFd` fields that hold sockets with `TqSocketHandle TcpFd{TqInvalidSocket}`.

In `src/tcp_write_queue.cpp`, replace socket sends:

```cpp
const int sent = TqSend(TcpFd, data + offset, length - offset, TqSendFlags::NoSignal);
```

and close/shutdown calls with `TqCloseSocket`, `TqShutdownSend`, or `TqShutdownBoth`.

- [ ] **Step 3: Remove `dup()` from SOCKS5 success handoff**

In `src/socks5_server.cpp`, make success ownership transfer explicit:

```cpp
const TqTunnelStartResult result = OnTunnel(request, clientFd);
if (!result.Ok) {
    const uint8_t rep = TqSocks5RepForOpenError(result.Error);
    (void)TqSendSocks5Reply(clientFd, rep);
    TqCloseSocket(clientFd);
    return;
}

if (!TqSendSocks5Reply(clientFd, TQ_SOCKS5_REP_SUCCEEDED)) {
    TqCloseSocket(clientFd);
    return;
}

clientFd = TqInvalidSocket;
```

If the current code sends success after `OnTunnel`, adjust the sequence to send success before `OnTunnel`:

```cpp
if (!TqSendSocks5Reply(clientFd, TQ_SOCKS5_REP_SUCCEEDED)) {
    TqCloseSocket(clientFd);
    return;
}
const TqTunnelStartResult result = OnTunnel(request, clientFd);
if (!result.Ok) {
    TqCloseSocket(clientFd);
    return;
}
clientFd = TqInvalidSocket;
```

Use the second sequence when `OnTunnel` only sends QUIC OPEN and does not consume TCP bytes before OPEN_OK, matching the approved spec.

- [ ] **Step 4: Remove `dup()` from HTTP CONNECT success handoff**

In `src/http_connect_server.cpp`, use the same ordering:

```cpp
if (!TqSendHttpResponse(clientFd, 200, "Connection Established")) {
    TqCloseSocket(clientFd);
    return;
}
const TqTunnelStartResult result = OnTunnel(request, clientFd);
if (!result.Ok) {
    TqCloseSocket(clientFd);
    return;
}
clientFd = TqInvalidSocket;
```

For failure before success, send the existing HTTP error response and close:

```cpp
const int status = TqHttpStatusForOpenError(result.Error);
(void)TqSendHttpResponse(clientFd, status, "Tunnel Failed");
TqCloseSocket(clientFd);
```

- [ ] **Step 5: Update listener socket atomics**

In `src/socks5_server.h` and `src/http_connect_server.h`, replace:

```cpp
std::atomic<int> ListenFd{-1};
```

with:

```cpp
std::atomic<TqSocketHandle> ListenFd{TqInvalidSocket};
```

Include `platform_socket.h` in both headers.

- [ ] **Step 6: Run Linux tunnel and listener tests**

Run:

```bash
rtk cmake --build build --target tcpquic_socks5_test tcpquic_http_connect_test tcpquic_tunnel_test tcpquic_tcp_write_queue_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_socks5_test
rtk ./build/bin/Release/tcpquic_http_connect_test
rtk ./build/bin/Release/tcpquic_tunnel_test
rtk ./build/bin/Release/tcpquic_tcp_write_queue_test
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/relay.h src/relay.cpp src/tcp_tunnel.h src/tcp_tunnel.cpp src/socks5_server.h src/socks5_server.cpp src/http_connect_server.h src/http_connect_server.cpp src/tcp_write_queue.cpp
rtk git commit -m "refactor: move tunnel sockets to platform handles"
```

## Task 5: Migrate Admin, Warmup, Main Startup, and CMake Guards

**Files:**
- Modify: `src/admin_http.h`
- Modify: `src/admin_http.cpp`
- Modify: `src/warmup.cpp`
- Modify: `src/quic_session.cpp`
- Modify: `src/main.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Migrate admin HTTP sockets**

In `src/admin_http.h`, include `platform_socket.h` and replace:

```cpp
std::atomic<int> ListenFd{-1};
```

with:

```cpp
std::atomic<TqSocketHandle> ListenFd{TqInvalidSocket};
```

In `src/admin_http.cpp`, replace direct socket operations:

```cpp
TqSetReuseAddr(fd);
TqSend(fd, data.data() + sent, data.size() - sent, TqSendFlags::NoSignal);
TqRecv(clientFd, buffer, sizeof(buffer), TqRecvFlags::None);
TqCloseSocket(fd);
TqInetNtop(AF_INET, &addr->sin_addr, host, sizeof(host));
```

- [ ] **Step 2: Migrate warmup socket pair**

In `src/warmup.cpp`, replace POSIX `socketpair`, `read`, `write`, and `close` with:

```cpp
TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
if (!TqSocketPair(pair)) {
    return false;
}
```

Use:

```cpp
TqSend(pair[0], data.data(), data.size(), TqSendFlags::None);
TqRecv(pair[1], buffer.data(), buffer.size(), TqRecvFlags::None);
TqCloseSocket(pair[0]);
TqCloseSocket(pair[1]);
```

- [ ] **Step 3: Handle Windows `_isatty`**

In `src/quic_session.cpp`, add:

```cpp
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
```

Replace `isatty(STDIN_FILENO)` with:

```cpp
#if defined(_WIN32)
const bool interactive = _isatty(_fileno(stdin)) != 0;
#else
const bool interactive = isatty(STDIN_FILENO) != 0;
#endif
```

- [ ] **Step 4: Initialize socket startup in main**

In `src/main.cpp`, include `platform_socket.h` and create startup at the beginning of `main`:

```cpp
int main(int argc, char** argv) {
    TqSocketStartup socketStartup;
    if (!socketStartup.Ok()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to initialize socket runtime\n");
        return 1;
    }
```

Keep existing config parsing and mode dispatch after this block.

- [ ] **Step 5: Guard Linux-only tests in CMake**

In `src/CMakeLists.txt`, wrap Linux relay tests:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tcpquic_linux_relay_buffer_pool_test
        unittest/linux_relay_buffer_pool_test.cpp
        linux_relay_buffer_pool.cpp
    )
    target_include_directories(tcpquic_linux_relay_buffer_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_property(TARGET tcpquic_linux_relay_buffer_pool_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_buffer_pool_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    list(APPEND TCPQUIC_LINUX_TEST_TARGETS
        tcpquic_linux_relay_buffer_pool_test
        tcpquic_linux_relay_worker_queue_test
        tcpquic_linux_relay_worker_io_test
        tcpquic_relay_backend_selection_test
    )
endif()
```

Add Linux test targets to `TCPQUIC_TEST_TARGETS` only inside the Linux block.

- [ ] **Step 6: Run migrated tests on Linux**

Run:

```bash
rtk cmake --build build --target tcpquic_admin_http_test tcpquic_router_runtime_test tcpquic_platform_socket_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_admin_http_test
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_platform_socket_test
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/admin_http.h src/admin_http.cpp src/warmup.cpp src/quic_session.cpp src/main.cpp src/CMakeLists.txt
rtk git commit -m "refactor: finish shared socket migration"
```

## Task 6: Add Windows IOCP Relay Skeleton and Selection

**Files:**
- Create: `src/windows_relay_worker.h`
- Create: `src/windows_relay_worker.cpp`
- Modify: `src/relay.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add Windows relay public header**

Create `src/windows_relay_worker.h`:

```cpp
#pragma once

#include "compress.h"
#include "platform_socket.h"
#include "relay.h"
#include "tuning.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

class TqWindowsRelayWorker {
public:
    TqWindowsRelayWorker();
    ~TqWindowsRelayWorker();

    bool Start();
    void Stop();

    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);

    void StopRelay(uint64_t relayId);
    static QUIC_STATUS QUIC_API StreamCallback(
        HQUIC stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayContext;
    struct IoOperation;

    void Run();
    void PostStop();

    void* Iocp_{nullptr};
    std::thread Thread_;
    std::atomic<bool> Stopping_{false};
    std::atomic<uint64_t> NextRelayId_{1};
    std::mutex Lock_;
    std::unordered_map<uint64_t, std::shared_ptr<RelayContext>> Relays_;
};

class TqWindowsRelayRuntime {
public:
    static TqWindowsRelayRuntime& Instance();

    bool Start(uint32_t workerCount);
    void Stop();
    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);
    void StopRelay(TqRelayHandle* handle);

private:
    std::mutex Lock_;
    std::vector<std::unique_ptr<TqWindowsRelayWorker>> Workers_;
    std::atomic<uint64_t> NextWorker_{0};
};
```

- [ ] **Step 2: Add a compiling skeleton**

Create `src/windows_relay_worker.cpp`:

```cpp
#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"

#include <windows.h>

TqWindowsRelayWorker::TqWindowsRelayWorker() = default;
TqWindowsRelayWorker::~TqWindowsRelayWorker() { Stop(); }

bool TqWindowsRelayWorker::Start() {
    Iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (Iocp_ == nullptr) {
        return false;
    }
    Stopping_.store(false);
    Thread_ = std::thread(&TqWindowsRelayWorker::Run, this);
    return true;
}

void TqWindowsRelayWorker::Stop() {
    if (Iocp_ == nullptr) {
        return;
    }
    Stopping_.store(true);
    PostStop();
    if (Thread_.joinable()) {
        Thread_.join();
    }
    ::CloseHandle(static_cast<HANDLE>(Iocp_));
    Iocp_ = nullptr;
}

void TqWindowsRelayWorker::PostStop() {
    (void)::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, nullptr);
}

void TqWindowsRelayWorker::Run() {
    while (!Stopping_.load()) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(
            static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, INFINITE);
        (void)ok;
        (void)bytes;
        (void)key;
        if (overlapped == nullptr && Stopping_.load()) {
            break;
        }
    }
}

bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle,
    MsQuicStream*,
    ITqCompressor*,
    ITqDecompressor*,
    TqRelayHandle*,
    const TqTuningConfig&,
    TqCompressAlgo) {
    return false;
}

void TqWindowsRelayWorker::StopRelay(uint64_t) {}

QUIC_STATUS QUIC_API TqWindowsRelayWorker::StreamCallback(
    HQUIC,
    void*,
    QUIC_STREAM_EVENT*) noexcept {
    return QUIC_STATUS_SUCCESS;
}

TqWindowsRelayRuntime& TqWindowsRelayRuntime::Instance() {
    static TqWindowsRelayRuntime runtime;
    return runtime;
}

bool TqWindowsRelayRuntime::Start(uint32_t workerCount) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (!Workers_.empty()) {
        return true;
    }
    if (workerCount == 0) {
        workerCount = 1;
    }
    for (uint32_t i = 0; i < workerCount; ++i) {
        auto worker = std::make_unique<TqWindowsRelayWorker>();
        if (!worker->Start()) {
            Workers_.clear();
            return false;
        }
        Workers_.push_back(std::move(worker));
    }
    return true;
}

void TqWindowsRelayRuntime::Stop() {
    std::lock_guard<std::mutex> guard(Lock_);
    Workers_.clear();
}

bool TqWindowsRelayRuntime::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (Workers_.empty()) {
        return false;
    }
    const uint64_t index = NextWorker_.fetch_add(1) % Workers_.size();
    return Workers_[static_cast<size_t>(index)]->RegisterRelay(
        tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo);
}

void TqWindowsRelayRuntime::StopRelay(TqRelayHandle* handle) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::WindowsWorker ||
        handle->WindowsWorker == nullptr) {
        return;
    }
    handle->WindowsWorker->StopRelay(handle->WindowsRelayId);
}

#endif
```

- [ ] **Step 3: Select the Windows backend in relay.cpp**

In `src/relay.cpp`, include Windows relay under `_WIN32`:

```cpp
#if defined(_WIN32)
#include "windows_relay_worker.h"
#endif
```

In `TqRelayStart`, add:

```cpp
#if defined(_WIN32)
if (!TqWindowsRelayRuntime::Instance().RegisterRelay(
        tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo)) {
    return false;
}
return true;
#endif
```

In `TqRelayStop`, add:

```cpp
#if defined(_WIN32)
if (handle->Backend == TqRelayBackendType::WindowsWorker) {
    TqWindowsRelayRuntime::Instance().StopRelay(handle);
}
#endif
```

- [ ] **Step 4: Add Windows relay source to CMake**

In `src/CMakeLists.txt`, add:

```cmake
if(WIN32)
    list(APPEND TCPQUIC_PROXY_SOURCES
        windows_relay_worker.cpp
    )
endif()
```

- [ ] **Step 5: Verify Linux relay build still passes**

Run:

```bash
rtk cmake --build build --target tcpquic_tunnel_test tcpquic_relay_backend_selection_test -j$(nproc)
rtk ./build/bin/Release/tcpquic_tunnel_test
rtk ./build/bin/Release/tcpquic_relay_backend_selection_test
```

Expected: both tests pass.

- [ ] **Step 6: Verify Windows config/build reaches relay skeleton**

On Windows x64 Developer PowerShell:

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target tcpquic-proxy
```

Expected: if later shared POSIX references remain, failures name those references. There should be no missing `windows_relay_worker.*` or `ws2_32` linkage failures.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/windows_relay_worker.h src/windows_relay_worker.cpp src/relay.cpp src/CMakeLists.txt
rtk git commit -m "feat: add windows relay backend skeleton"
```

## Task 7: Implement IOCP Relay Data Path

**Files:**
- Modify: `src/windows_relay_worker.h`
- Modify: `src/windows_relay_worker.cpp`
- Create: `src/unittest/windows_relay_worker_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add Windows-only relay worker test target**

Create `src/unittest/windows_relay_worker_test.cpp`:

```cpp
#include "platform_socket.h"
#include "windows_relay_worker.h"

#include <cassert>

int main() {
#if defined(_WIN32)
    TqSocketStartup startup;
    assert(startup.Ok());

    TqWindowsRelayWorker worker;
    assert(worker.Start());
    worker.Stop();
    worker.Stop();
#endif
    return 0;
}
```

In `src/CMakeLists.txt`, add under `if(WIN32)`:

```cmake
add_executable(tcpquic_windows_relay_worker_test
    unittest/windows_relay_worker_test.cpp
    windows_relay_worker.cpp
    platform_socket_win.cpp
)
target_include_directories(tcpquic_windows_relay_worker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_windows_relay_worker_test PRIVATE ws2_32 inc warnings msquic logging base_link)
target_compile_definitions(tcpquic_windows_relay_worker_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
list(APPEND TCPQUIC_TEST_TARGETS tcpquic_windows_relay_worker_test)
```

- [ ] **Step 2: Define IO operation and relay context**

In `src/windows_relay_worker.cpp`, replace the skeleton private structs with:

```cpp
enum class TqWindowsRelayEvent : uint32_t {
    TcpRecv,
    TcpSend,
    QuicReceiveQueued,
    QuicSendComplete,
    CloseRelay,
    StopWorker,
};

struct TqWindowsRelayWorker::IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsRelayEvent Event{TqWindowsRelayEvent::TcpRecv};
    std::shared_ptr<RelayContext> Relay;
    std::vector<uint8_t> Buffer;
};

struct TqWindowsRelayWorker::RelayContext {
    uint64_t Id{0};
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqRelayHandle* PublicHandle{nullptr};
    TqTuningConfig Tuning;
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::atomic<bool> Closing{false};
    std::atomic<uint32_t> InFlightQuicSends{0};
};
```

- [ ] **Step 3: Associate sockets and post initial receive**

Implement `RegisterRelay`:

```cpp
bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (!TqSocketValid(tcpFd) || stream == nullptr || handle == nullptr || Iocp_ == nullptr) {
        return false;
    }
    if (::CreateIoCompletionPort(
            reinterpret_cast<HANDLE>(tcpFd),
            static_cast<HANDLE>(Iocp_),
            0,
            0) == nullptr) {
        return false;
    }

    auto relay = std::make_shared<RelayContext>();
    relay->Id = NextRelayId_.fetch_add(1);
    relay->TcpFd = tcpFd;
    relay->Stream = stream;
    relay->Compressor = compressor;
    relay->Decompressor = decompressor;
    relay->PublicHandle = handle;
    relay->Tuning = tuning;
    relay->CompressAlgo = compressAlgo;

    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_[relay->Id] = relay;
    }

    handle->Backend = TqRelayBackendType::WindowsWorker;
    handle->WindowsWorker = this;
    handle->WindowsRelayId = relay->Id;

    return PostTcpRecv(relay);
}
```

Add a private declaration to `windows_relay_worker.h`:

```cpp
bool PostTcpRecv(const std::shared_ptr<RelayContext>& relay);
```

Implement `PostTcpRecv`:

```cpp
bool TqWindowsRelayWorker::PostTcpRecv(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load()) {
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsRelayEvent::TcpRecv;
    op->Relay = relay;
    op->Buffer.resize(relay->Tuning.RelayIoSize);

    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(op->Buffer.data());
    buf.len = static_cast<ULONG>(op->Buffer.size());
    DWORD flags = 0;
    DWORD received = 0;
    IoOperation* raw = op.release();
    const int rc = ::WSARecv(relay->TcpFd, &buf, 1, &received, &flags, &raw->Overlapped, nullptr);
    if (rc == 0 || ::WSAGetLastError() == WSA_IO_PENDING) {
        return true;
    }
    delete raw;
    return false;
}
```

- [ ] **Step 4: Handle IOCP completions**

In `Run`, dispatch operations:

```cpp
if (overlapped == nullptr) {
    continue;
}
std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
if (!ok) {
    CloseRelay(op->Relay);
    continue;
}
switch (op->Event) {
case TqWindowsRelayEvent::TcpRecv:
    HandleTcpRecv(std::move(op), bytes);
    break;
case TqWindowsRelayEvent::TcpSend:
    HandleTcpSend(std::move(op), bytes);
    break;
case TqWindowsRelayEvent::QuicReceiveQueued:
    HandleQuicReceiveQueued(std::move(op));
    break;
case TqWindowsRelayEvent::CloseRelay:
    CloseRelay(op->Relay);
    break;
default:
    break;
}
```

Add private declarations:

```cpp
void HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes);
void HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes);
void HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op);
void CloseRelay(const std::shared_ptr<RelayContext>& relay);
```

- [ ] **Step 5: Implement close semantics**

Implement:

```cpp
void TqWindowsRelayWorker::CloseRelay(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.exchange(true)) {
        return;
    }
    TqCloseSocket(relay->TcpFd);
    relay->TcpFd = TqInvalidSocket;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
    }
    if (relay->PublicHandle != nullptr) {
        relay->PublicHandle->Stop.store(true);
    }
}
```

- [ ] **Step 6: Implement TCP receive handling**

Implement a minimal uncompressed path first:

```cpp
void TqWindowsRelayWorker::HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        return;
    }
    if (bytes == 0) {
        relay->Stream->Send(nullptr, 0, QUIC_SEND_FLAG_FIN, nullptr);
        CloseRelay(relay);
        return;
    }
    op->Buffer.resize(bytes);
    QUIC_BUFFER buffer{};
    buffer.Buffer = op->Buffer.data();
    buffer.Length = bytes;
    relay->InFlightQuicSends.fetch_add(1);
    const QUIC_STATUS status = relay->Stream->Send(&buffer, 1, QUIC_SEND_FLAG_NONE, op.release());
    if (QUIC_FAILED(status)) {
        relay->InFlightQuicSends.fetch_sub(1);
        CloseRelay(relay);
    }
}
```

If `relay->Compressor` is non-null, replace the direct `Stream->Send()` payload with this compression block before constructing the `QUIC_BUFFER`:

```cpp
std::vector<uint8_t> compressed;
const uint8_t* sendData = op->Buffer.data();
uint32_t sendLength = static_cast<uint32_t>(op->Buffer.size());
if (relay->Compressor != nullptr) {
    if (!relay->Compressor->Compress(
            op->Buffer.data(),
            static_cast<uint32_t>(op->Buffer.size()),
            compressed,
            false)) {
        CloseRelay(relay);
        return;
    }
    sendData = compressed.data();
    sendLength = static_cast<uint32_t>(compressed.size());
}
op->Buffer.assign(sendData, sendData + sendLength);
```

Then construct `QUIC_BUFFER` from `op->Buffer`. This keeps the send context alive until MsQuic reports send completion.

- [ ] **Step 7: Implement QUIC receive enqueue**

In `StreamCallback`, when receiving `QUIC_STREAM_EVENT_RECEIVE`, allocate an operation, copy buffers, and post:

```cpp
auto* callback = static_cast<CallbackContext*>(context);
if (callback == nullptr || callback->Worker == nullptr || event == nullptr) {
    return QUIC_STATUS_SUCCESS;
}
auto relay = callback->Relay.lock();
if (!relay) {
    return QUIC_STATUS_SUCCESS;
}
if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsRelayEvent::QuicReceiveQueued;
    op->Relay = relay;
    for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER& buffer = event->RECEIVE.Buffers[i];
        op->Buffer.insert(op->Buffer.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
    }
    IoOperation* raw = op.release();
    (void)::PostQueuedCompletionStatus(
        static_cast<HANDLE>(callback->Worker->Iocp_), 0, 0, &raw->Overlapped);
}
return QUIC_STATUS_SUCCESS;
```

When wiring the stream callback in `RegisterRelay`, create a callback context owned by `RelayContext`:

```cpp
struct CallbackContext {
    TqWindowsRelayWorker* Worker{nullptr};
    std::weak_ptr<RelayContext> Relay;
};
```

Add `std::shared_ptr<CallbackContext> Callback;` to `RelayContext`, initialize it after `relay` is created, and pass `relay->Callback.get()` as the MsQuic callback context. In `StreamCallback`, cast `context` to `CallbackContext*`, lock `Relay`, and store that `std::shared_ptr<RelayContext>` in each queued `IoOperation`.

- [ ] **Step 8: Implement QUIC to TCP send**

Implement `HandleQuicReceiveQueued`:

```cpp
void TqWindowsRelayWorker::HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Buffer.empty()) {
        return;
    }
    op->Event = TqWindowsRelayEvent::TcpSend;
    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(op->Buffer.data());
    buf.len = static_cast<ULONG>(op->Buffer.size());
    DWORD sent = 0;
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &buf, 1, &sent, 0, &raw->Overlapped, nullptr);
    if (rc != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
        auto failedRelay = raw->Relay;
        delete raw;
        CloseRelay(failedRelay);
    }
}
```

If `relay->Decompressor` is non-null, replace `op->Buffer` before `WSASend` with:

```cpp
std::vector<uint8_t> output;
if (relay->Decompressor != nullptr) {
    if (!relay->Decompressor->Decompress(
            op->Buffer.data(),
            static_cast<uint32_t>(op->Buffer.size()),
            output)) {
        CloseRelay(relay);
        return;
    }
    op->Buffer.swap(output);
}
```

If `op->Buffer` is empty after decompression, return without posting `WSASend`.

- [ ] **Step 9: Build and run Windows relay worker test**

On Windows x64 Developer PowerShell:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test
.\build-win\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: build succeeds and test exits 0.

- [ ] **Step 10: Commit**

Run:

```bash
rtk git add src/windows_relay_worker.h src/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: implement windows iocp relay worker"
```

## Task 8: Windows Native Build and Unit Test Pass

**Files:**
- Modify: `src/CMakeLists.txt`
- Modify: `src/admin_http.cpp`
- Modify: `src/http_connect_server.cpp`
- Modify: `src/socks5_server.cpp`
- Modify: `src/tcp_dialer.cpp`
- Modify: `src/tcp_tunnel.cpp`
- Modify: `src/tcp_write_queue.cpp`
- Modify: `src/warmup.cpp`

- [ ] **Step 1: Run Windows build**

On Windows x64 Developer PowerShell:

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target tcpquic-proxy
```

Expected: build succeeds. If it fails, fix only one of these concrete Windows migration issues and rerun this step before continuing: add missing `platform_socket.h`, replace a remaining socket `close` with `TqCloseSocket`, replace `inet_pton` with `TqInetPton`, replace `MSG_NOSIGNAL` sends with `TqSendFlags::NoSignal`, guard `fcntl` behind `!defined(_WIN32)`, or move a Linux-only test target behind `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`.

- [ ] **Step 2: Build Windows unit tests**

Run:

```powershell
cmake --build build-win --target `
  tcpquic_acl_test `
  tcpquic_control_test `
  tcpquic_compress_test `
  tcpquic_config_router_test `
  tcpquic_thread_pool_test `
  tcpquic_tunnel_reaper_test `
  tcpquic_http_connect_test `
  tcpquic_socks5_test `
  tcpquic_admin_http_test `
  tcpquic_platform_socket_test `
  tcpquic_windows_relay_worker_test
```

Expected: all listed targets build.

- [ ] **Step 3: Run Windows unit tests**

Run:

```powershell
.\build-win\bin\Release\tcpquic_acl_test.exe
.\build-win\bin\Release\tcpquic_control_test.exe
.\build-win\bin\Release\tcpquic_compress_test.exe
.\build-win\bin\Release\tcpquic_config_router_test.exe
.\build-win\bin\Release\tcpquic_thread_pool_test.exe
.\build-win\bin\Release\tcpquic_tunnel_reaper_test.exe
.\build-win\bin\Release\tcpquic_http_connect_test.exe
.\build-win\bin\Release\tcpquic_socks5_test.exe
.\build-win\bin\Release\tcpquic_admin_http_test.exe
.\build-win\bin\Release\tcpquic_platform_socket_test.exe
.\build-win\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: every executable exits with status 0.

- [ ] **Step 4: Run Linux regression build**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
rtk cmake --build build --target tcpquic_acl_test tcpquic_admin_http_test tcpquic_compress_test tcpquic_config_router_test tcpquic_control_test tcpquic_http_connect_test tcpquic_router_runtime_test tcpquic_socks5_test tcpquic_tcp_write_queue_test tcpquic_thread_pool_test tcpquic_tuning_test tcpquic_tunnel_reaper_test tcpquic_tunnel_test -j$(nproc)
```

Expected: all listed targets build.

- [ ] **Step 5: Run Linux unit regression**

Run:

```bash
for t in build/bin/Release/tcpquic_*_test; do rtk "$t"; done
```

Expected: every test exits 0.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src CMakeLists.txt
rtk git commit -m "build: pass windows native build and unit tests"
```

## Task 9: End-to-End Windows and Interop Validation

**Files:**
- Create: `scripts/test-tcpquic-proxy-windows.ps1`
- Modify: `README.md`

- [ ] **Step 1: Add Windows loopback test script**

Create `scripts/test-tcpquic-proxy-windows.ps1`:

```powershell
param(
  [string]$Bin = ".\build-win\bin\Release\tcpquic-proxy.exe",
  [string]$Compress = "off"
)

$ErrorActionPreference = "Stop"
$Work = Join-Path $env:TEMP ("tcpquic-win-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Work | Out-Null

try {
  $CaKey = Join-Path $Work "ca.key"
  $CaCrt = Join-Path $Work "ca.crt"
  $ServerKey = Join-Path $Work "server.key"
  $ServerCsr = Join-Path $Work "server.csr"
  $ServerCrt = Join-Path $Work "server.crt"
  $ClientKey = Join-Path $Work "client.key"
  $ClientCsr = Join-Path $Work "client.csr"
  $ClientCrt = Join-Path $Work "client.crt"

  openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=tcpquic-test-ca" -keyout $CaKey -out $CaCrt | Out-Null
  openssl req -newkey rsa:2048 -nodes -subj "/CN=localhost" -keyout $ServerKey -out $ServerCsr | Out-Null
  openssl x509 -req -in $ServerCsr -CA $CaCrt -CAkey $CaKey -CAcreateserial -days 1 -out $ServerCrt | Out-Null
  openssl req -newkey rsa:2048 -nodes -subj "/CN=tcpquic-client" -keyout $ClientKey -out $ClientCsr | Out-Null
  openssl x509 -req -in $ClientCsr -CA $CaCrt -CAkey $CaKey -CAcreateserial -days 1 -out $ClientCrt | Out-Null

  $HttpPort = 18080
  $QuicPort = 18443
  $SocksPort = 11080
  $ConnectPort = 18081

  $Http = Start-Process -PassThru python -ArgumentList "-m", "http.server", "$HttpPort", "--bind", "127.0.0.1"
  Start-Sleep -Seconds 1

  $ServerArgs = @(
    "server",
    "--quic-listen", "127.0.0.1:$QuicPort",
    "--quic-cert", $ServerCrt,
    "--quic-key", $ServerKey,
    "--quic-ca", $CaCrt,
    "--allow-targets", "127.0.0.0/8",
    "--compress", $Compress
  )
  $Server = Start-Process -PassThru $Bin -ArgumentList $ServerArgs
  Start-Sleep -Seconds 1

  $ClientArgs = @(
    "client",
    "--quic-peer", "127.0.0.1:$QuicPort",
    "--quic-cert", $ClientCrt,
    "--quic-key", $ClientKey,
    "--quic-ca", $CaCrt,
    "--socks-listen", "127.0.0.1:$SocksPort",
    "--http-listen", "127.0.0.1:$ConnectPort",
    "--compress", $Compress
  )
  $Client = Start-Process -PassThru $Bin -ArgumentList $ClientArgs
  Start-Sleep -Seconds 2

  curl.exe -f -x "http://127.0.0.1:$ConnectPort" --proxytunnel "http://127.0.0.1:$HttpPort/" | Out-Null
  curl.exe -f --socks5-hostname "127.0.0.1:$SocksPort" "http://127.0.0.1:$HttpPort/" | Out-Null
}
finally {
  if ($Client) { Stop-Process -Id $Client.Id -Force -ErrorAction SilentlyContinue }
  if ($Server) { Stop-Process -Id $Server.Id -Force -ErrorAction SilentlyContinue }
  if ($Http) { Stop-Process -Id $Http.Id -Force -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}
```

- [ ] **Step 2: Run Windows loopback validation**

On Windows x64 Developer PowerShell:

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress lz4
```

Expected: all three commands exit 0.

- [ ] **Step 3: Run interop validation**

Windows client to Linux server. Before running this, set `TCPQUIC_LINUX_SERVER` to the Linux server address and `TCPQUIC_ALLOWED_URL` to an HTTP URL allowed by that server ACL:

```powershell
if ([string]::IsNullOrWhiteSpace($env:TCPQUIC_LINUX_SERVER)) { throw "TCPQUIC_LINUX_SERVER is required" }
if ([string]::IsNullOrWhiteSpace($env:TCPQUIC_ALLOWED_URL)) { throw "TCPQUIC_ALLOWED_URL is required" }
$Client = Start-Process -PassThru .\build-win\bin\Release\tcpquic-proxy.exe -ArgumentList @(
  "client", "--quic-peer", "$env:TCPQUIC_LINUX_SERVER`:443",
  "--quic-cert", "client.crt", "--quic-key", "client.key", "--quic-ca", "ca.crt",
  "--socks-listen", "127.0.0.1:11080", "--http-listen", "127.0.0.1:18081",
  "--compress", "off"
)
Start-Sleep -Seconds 2
curl.exe -f --socks5-hostname 127.0.0.1:11080 "$env:TCPQUIC_ALLOWED_URL"
Stop-Process -Id $Client.Id -Force
```

Linux client to Windows server. Before running this, export `TCPQUIC_WINDOWS_SERVER` to the Windows server address and `TCPQUIC_ALLOWED_URL` to an HTTP URL allowed by that server ACL:

```bash
[ -n "$TCPQUIC_WINDOWS_SERVER" ] || { echo "TCPQUIC_WINDOWS_SERVER is required" >&2; exit 1; }
[ -n "$TCPQUIC_ALLOWED_URL" ] || { echo "TCPQUIC_ALLOWED_URL is required" >&2; exit 1; }
rtk ./build/bin/Release/tcpquic-proxy client --quic-peer "${TCPQUIC_WINDOWS_SERVER}:443" --quic-cert client.crt --quic-key client.key --quic-ca ca.crt --socks-listen 127.0.0.1:11080 --http-listen 127.0.0.1:18081 --compress off &
TCPQUIC_CLIENT_PID=$!
sleep 2
rtk curl -f --socks5-hostname 127.0.0.1:11080 "$TCPQUIC_ALLOWED_URL"
kill "$TCPQUIC_CLIENT_PID"
```

Expected: curl receives the upstream response in both directions.

- [ ] **Step 4: Document Windows commands**

In `README.md`, add a Windows build section:

````markdown
### Windows 10/11 x64

Install Visual Studio 2022 Build Tools with the MSVC C++ workload, CMake, Ninja, Git, OpenSSL, Python, and curl.

```powershell
git submodule update --init --recursive
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target tcpquic-proxy
```

Run the Windows loopback validation:

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress lz4
```
````

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add scripts/test-tcpquic-proxy-windows.ps1 README.md
rtk git commit -m "test: add windows loopback validation"
```

## Task 10: Final Verification and Completion

**Files:**
- Modify only files required by final verification failures.

- [ ] **Step 1: Run Linux full integration test**

Run:

```bash
rtk ./scripts/test-tcpquic-proxy.sh
```

Expected: script exits 0.

- [ ] **Step 2: Run Linux concurrent test**

Run:

```bash
rtk ./scripts/test-tcpquic-concurrent.sh
```

Expected: script exits 0.

- [ ] **Step 3: Run Windows build and loopback tests one final time**

On Windows x64 Developer PowerShell:

```powershell
cmake --build build-win --target tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress lz4
```

Expected: all commands exit 0.

- [ ] **Step 4: Check git status**

Run:

```bash
rtk git status --short
```

Expected: no output.

- [ ] **Step 5: Record final verification in the task handoff**

In the final response for implementation, include the actual verification commands and pass/fail result, for example:

```text
Verified:
- Linux unit tests: `for t in build/bin/Release/tcpquic_*_test; do rtk "$t"; done` passed.
- Linux integration: `rtk ./scripts/test-tcpquic-proxy.sh` passed.
- Windows unit tests: `.\build-win\bin\Release\tcpquic_platform_socket_test.exe` and the other listed Windows test executables passed.
- Windows loopback: `.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off`, `zstd`, and `lz4` passed.
- Interop: Windows client to Linux server and Linux client to Windows server curl checks passed.
```
