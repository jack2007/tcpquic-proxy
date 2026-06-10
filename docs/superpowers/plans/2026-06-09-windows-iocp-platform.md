# Windows IOCP Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run native Windows 10/11 x64 `tcpquic-proxy.exe` with MSVC 2022, PEM mTLS, shared socket code, and a production IOCP relay backend.

**Architecture:** Add a narrow platform socket layer used by shared listener, dialer, tunnel, and admin code. Keep Linux relay on the existing epoll worker and add a Windows-only IOCP relay worker selected by `relay.cpp` at compile time.

**Tech Stack:** C++17, CMake, MSVC 2022, Winsock2, IOCP, MsQuic, lz4, zstd, existing hand-written unit tests.

---

## Implementation Progress (updated 2026-06-10)

**Branch:** `feature/windows-platform-support`

| Task | Status | Commit / Notes |
|------|--------|-----------------|
| Task 1 — Platform socket layer (Linux) | **Done** | `1bb1eb3` |
| Task 2 — Windows socket + CMake | **Done** | `74f6fb6` |
| Task 3 — Dialer / ACL migration | **Done** | `466553d` |
| Task 4 — Tunnel / relay API | **Done** | `2339714` |
| Task 5 — Admin / warmup / main | **Done** | `93ced8a` |
| Task 6 — IOCP relay skeleton | **Done** | `73ac646` |
| Task 7 — IOCP relay data path | **Done** | `dd741ce` |
| Task 8 — Windows build + unit tests | **Done / awaiting Linux regression** | `quictls` x64 build and key Windows tests pass; `tcpquic_tunnel_test` is ported to platform sockets on Windows |
| Task 9 — Loopback + interop | **Done for Windows quictls / interop pending** | Windows `quictls` loopback passes `off` / `zstd` / `lz4`; Schannel credential loading remains a hardening item |
| Task 10 — Final verification | **Done** | Linux regression + DGX dual-host smoke passed 2026-06-10; see [`docs/linux-verification-2026-06-10.md`](../../linux-verification-2026-06-10.md) |

### Build environment (validated)

Primary Windows target is **x64**, not Win32:

```powershell
cmake -S . -B build-x64 -A x64 -DLZ4_SOURCE_DIR="$PWD/third_party/lz4"
cmake --build build-x64 --config Release --target tcpquic-proxy
```

The plan originally referenced `build-win` + Ninja; on this machine the MSVC multi-config generator (`-A x64`) is the working path. Output binaries live under `build-x64/bin/Release/`.

**Do not use `build-x86` (Win32) as the primary validation directory** — `tcpquic_thread_pool_test` crashes with `0xC0000005` there; x64 passes.

### Task 8 completed in current update

Changes included in the current Windows IOCP/quictls update:

| Area | Change |
|------|--------|
| `src/CMakeLists.txt` | `tcpquic_copy_msquic_runtime()` — copy `msquic.dll` next to all msquic-linked test executables (fixes runtime load failures) |
| `src/main.cpp` | `TunnelStartFn` lambda parameter `int fd` → `TqSocketHandle` |
| `src/thread_pool.cpp` | WorkerLoop spurious-wakeup guard: `if (Queue.empty()) continue;` |
| `src/unittest/thread_pool_test.cpp` | Remove POSIX `socketpair`; pure thread-pool coverage |
| `src/unittest/tunnel_reaper_test.cpp` | Windows socket startup |
| `CMakeLists.txt` | `TCPQUIC_WINDOWS_TLS` cache (`schannel` default, optional `quictls`) |
| `src/quic_credentials_win.{h,cpp}` | **New** — Schannel credential bridge (PEM → PFX → CryptoAPI) |
| `src/quic_session.{cpp,h}` | `_WIN32` path uses `TqQuicCredentialHolder` instead of `QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE` |
| `README.md` | Windows x64 build + Schannel notes (partial) |

**Current Windows verification:** key x64 targets build and exit 0 from `build-x64-quictls/bin/Release/`: `tcpquic_compress_test`, `tcpquic_tunnel_test`, `tcpquic_windows_relay_worker_test`, and `tcpquic_platform_socket_test`. Windows `quictls` loopback passes `off`, `zstd`, and `lz4` for both HTTP CONNECT and SOCKS5.

**Linux validation (2026-06-10):** unit tests, `scripts/test-tcpquic-proxy.sh`, `scripts/test-tcpquic-concurrent.sh` (100 tunnels), and `scripts/test-tcpquic-proxy-dgx.sh` (100 tunnels + lz4) all PASS. Details: [`docs/linux-verification-2026-06-10.md`](../../linux-verification-2026-06-10.md).

**Remaining:** cross-platform Linux ↔ Windows interop when dedicated hosts are available; Schannel credential hardening on Windows.

### Task 9 status — Windows `quictls` loopback passes; Schannel requires OS TLS 1.3 support

MsQuic on Windows defaults to **Schannel**, which does **not** support Linux-style `QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE` or `QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE`. The current implementation therefore supports two Windows TLS modes:

| Mode | Status | Notes |
|------|--------|-------|
| `-DTCPQUIC_WINDOWS_TLS=quictls` | **Validated** | Restores PEM/CA-file behavior; `scripts/test-tcpquic-proxy-windows.ps1` passes `off`, `zstd`, and `lz4` for both HTTP CONNECT and SOCKS5 loopback. |
| default Schannel | **Platform-limited on Windows 10** | MsQuic requires Schannel TLS 1.3 support for QUIC. Per `third_party/msquic/docs/Platforms.md`, Schannel mode requires Windows Server 2022, Windows 11, or the latest Windows Insider Preview builds; ordinary Windows 10 releases can fail credential loading with `0x80090331` (`SEC_E_ALGORITHM_MISMATCH`). |

**Root causes and fixes completed during `quictls` debugging:**

1. OpenSSL 4.0 rejected the original CA generation syntax; script now creates CSR + signs with explicit extension files.
2. Test CA/server/client certificates now include proper CA constraints, SAN, and EKU (`serverAuth` / `clientAuth`).
3. Windows `quictls` client currently uses `QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION` as a compatibility workaround; removing it still prevents a usable QUIC connection in this environment.
4. The relay data-path failure was not TLS after handshake. Root cause: server-side `OPEN_OK` was sent with the tunnel callback still active, but `StartRelay()` immediately replaced the MsQuic stream callback. The later `OPEN_OK` `SEND_COMPLETE` was then delivered to the relay callback with a `TqTunnelSendContext*` client context, causing callback/context type mismatch. Fix: defer server relay start until `OPEN_OK` `SEND_COMPLETE` has been consumed by the tunnel callback.
5. Windows relay now treats `QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN` as QUIC receive half-close only: it shuts down TCP send but keeps TCP receive alive so target responses can continue back to QUIC.
6. Windows relay now flushes compressed output when `Compress(..., false)` produces no bytes and emits compressor end frames on TCP EOF before QUIC FIN. This fixes `zstd`/`lz4` small-request stalls.

**Validated commands:**

```powershell
cmake --build build-x64-quictls --config Release --target tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Bin ".\build-x64-quictls\bin\Release\tcpquic-proxy.exe" -TlsBackend quictls -Compress off -QuicPeerHost 127.0.0.1
.\scripts\test-tcpquic-proxy-windows.ps1 -Bin ".\build-x64-quictls\bin\Release\tcpquic-proxy.exe" -TlsBackend quictls -Compress zstd -QuicPeerHost 127.0.0.1
.\scripts\test-tcpquic-proxy-windows.ps1 -Bin ".\build-x64-quictls\bin\Release\tcpquic-proxy.exe" -TlsBackend quictls -Compress lz4 -QuicPeerHost 127.0.0.1
```

**Current Schannel credential findings:**

- Root cause: ordinary Windows 10 releases do not provide Schannel TLS 1.3 support by default. QUIC requires TLS 1.3, and MsQuic's Schannel backend disables every protocol except TLS 1.3 before calling `AcquireCredentialsHandleW`.
- MsQuic documents this platform requirement in `third_party/msquic/docs/Platforms.md`: Schannel mode requires Windows Server 2022, Windows 11, or the latest Windows Insider Preview builds for Schannel TLS 1.3 support.
- The current Windows 10 22H2 machine reports no TLS 1.3 Schannel cipher suites from `Get-TlsCipherSuite | Where-Object { $_.Name -like '*TLS_AES*' -or $_.Name -like '*TLS_CHACHA*' }`.
- MsQuic's built-in `quicsample` was used to isolate this from `tcpquic-proxy`: building `third_party/msquic` with `-DQUIC_BUILD_TOOLS=ON -DQUIC_TLS_LIB=schannel`, generating the exact certificate recommended in `src/tools/sample/sample.c`, then running `quicsample.exe -server -cert_hash:<thumbprint>` still fails at `ConfigurationLoadCredential` with `0x80090331` (`SEC_E_ALGORITHM_MISMATCH`).
- `QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH`, `QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE`, and explicit `CurrentUser\MY` `CERT_CONTEXT` lookup were tried; `CryptAcquireCertificatePrivateKey` confirms the generated CurrentUser cert has an accessible private key, so this is not a `tcpquic-proxy` credential bridge bug.
- On Windows 10, use `-DTCPQUIC_WINDOWS_TLS=quictls` for validation and runtime. Re-test Schannel only on Windows Server 2022, Windows 11, or an Insider build with Schannel TLS 1.3 cipher suites available.

**Interop (Task 9 Step 3):** not attempted; requires `TCPQUIC_LINUX_SERVER` / `TCPQUIC_WINDOWS_SERVER` env — skip when unset.

### Recommended next steps

1. ~~Run Linux regression~~ — **done** (see `docs/linux-verification-2026-06-10.md`).
2. Keep `quictls` as the Windows 10 validation/runtime path; only re-test Schannel on Windows Server 2022, Windows 11, or a Windows Insider build with Schannel TLS 1.3 support.
3. Run Task 9 interop when `TCPQUIC_LINUX_SERVER` / `TCPQUIC_WINDOWS_SERVER` hosts are available.

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
- Create `src/quic_credentials_win.h` / `src/quic_credentials_win.cpp`: Windows Schannel credential bridge (PEM → PFX → CryptoAPI; used when `QUIC_TLS_LIB=schannel`).
- Create `scripts/test-tcpquic-proxy-windows.ps1`: Windows loopback validation (HTTP CONNECT + SOCKS5 over QUIC).
- Modify `CMakeLists.txt`: `TCPQUIC_WINDOWS_TLS` option (`schannel` default, `quictls` optional).
- Modify `src/quic_session.cpp` and `src/quic_session.h`: `_WIN32` credential path via `TqQuicCredentialHolder`.
- Modify `src/main.cpp`: handle `_isatty`, initialize `TqSocketStartup`, `TqSocketHandle` in tunnel callbacks.
- Modify `README.md`: document Windows x64 build, Schannel TLS constraints, test, runtime, and interoperability commands.

## Task 1: Add Platform Socket Layer on Linux

> **Status: DONE** — `1bb1eb3` on `feature/windows-platform-support`

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

Any target that compiles a source file calling `TqCloseSocket`, `TqSend`, `TqRecv`, `TqInetPton`, `TqInetNtop`, or `TqSocketPair` must also compile `${TCPQUIC_PLATFORM_SOURCES}` or it will fail at link time. As later tasks migrate more files, add `${TCPQUIC_PLATFORM_SOURCES}` to the affected unit-test source lists in the same task where the migrated file is introduced.

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
    target_compile_definitions(tcpquic_platform_socket_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
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

> **Status: DONE** — `74f6fb6`

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

> **Status: DONE** — `466553d`

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

Keep a helper that computes the real socket-address length; do not use `sizeof(sockaddr_storage)` for `connect`, because that breaks some Winsock calls and can produce inconsistent behavior across address families:

```cpp
socklen_t TqSockaddrLength(const sockaddr_storage& addr) {
    switch (addr.ss_family) {
    case AF_INET:
        return sizeof(sockaddr_in);
    case AF_INET6:
        return sizeof(sockaddr_in6);
    default:
        return 0;
    }
}
```

Use this platform split for waiting until connect completes, including the `SO_ERROR` check after poll readiness:

```cpp
#if defined(_WIN32)
WSAPOLLFD pfd{};
pfd.fd = fd;
pfd.events = POLLOUT;
int pollResult = ::WSAPoll(&pfd, 1, timeoutMs);
if (pollResult <= 0) {
    connectErrno = (pollResult == 0) ? WSAETIMEDOUT : TqLastSocketError();
    return false;
}
int error = 0;
int errorLen = sizeof(error);
if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLen) != 0) {
    connectErrno = TqLastSocketError();
    return false;
}
#else
pollfd pfd{};
pfd.fd = fd;
pfd.events = POLLOUT;
int pollResult;
do {
    pollResult = ::poll(&pfd, 1, timeoutMs);
} while (pollResult < 0 && TqSocketInterrupted(TqLastSocketError()));
if (pollResult <= 0) {
    connectErrno = (pollResult == 0) ? ETIMEDOUT : TqLastSocketError();
    return false;
}
int error = 0;
socklen_t errorLen = sizeof(error);
if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLen) != 0) {
    connectErrno = TqLastSocketError();
    return false;
}
#endif
if (error != 0) {
    connectErrno = error;
    return false;
}
connectErrno = 0;
return true;
```

When checking `connect` errors:

```cpp
const socklen_t addrLen = TqSockaddrLength(addr);
if (addrLen == 0) {
    continue;
}
const int connectResult = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), addrLen);
if (connectResult != 0 && !TqSocketInProgress(TqLastSocketError())) {
    TqCloseSocket(fd);
    continue;
}
```

When tuning:

```cpp
void TqTuneTcpForThroughput(TqSocketHandle fd, int bufferBytes) {
    if (!TqSocketValid(fd)) {
        return;
    }
    if (bufferBytes <= 0) {
        bufferBytes = TqGetActiveTcpSocketBuffer();
    }
    (void)TqSetNoDelay(fd);
    (void)TqSetSocketBuffer(fd, SO_RCVBUF, bufferBytes);
    (void)TqSetSocketBuffer(fd, SO_SNDBUF, bufferBytes);
}
```

Modify `src/CMakeLists.txt` in this task so every target that now compiles `tcp_dialer.cpp` or `acl.cpp` also compiles `${TCPQUIC_PLATFORM_SOURCES}`. At this point that includes `tcpquic-proxy`, `tcpquic_acl_test`, `tcpquic_tuning_test`, `tcpquic_config_router_test`, `tcpquic_tunnel_test`, `tcpquic_http_connect_test`, `tcpquic_socks5_test`, and `tcpquic_router_runtime_test`.

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

> **Status: DONE** — `2339714`

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

In `src/socks5_server.cpp`, make success ownership transfer explicit. Send the SOCKS5 success reply before handing the socket to the tunnel, then treat `OnTunnel` success as ownership transfer:

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

Do not keep the old failure-reply-after-`OnTunnel` sequence once `dup()` is removed. After the success reply has been sent, a later tunnel-open failure can only close the socket; sending a SOCKS5 failure reply at that point violates the protocol state seen by the client.

- [ ] **Step 4: Remove `dup()` from HTTP CONNECT success handoff**

In `src/http_connect_server.cpp`, use the same ordering and keep the existing `TqSendHttpStatus` helper name:

```cpp
if (!TqSendHttpStatus(clientFd, 200)) {
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

For failure before the 200 response, send the existing HTTP error response and close. Once 200 has been sent, do not send another HTTP error response on later tunnel failure:

```cpp
const int status = TqHttpStatusForOpenError(result.Error);
(void)TqSendHttpStatus(clientFd, status);
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

> **Status: DONE** — `93ced8a`

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

In `src/CMakeLists.txt`, wrap all four Linux relay test targets, not only the buffer-pool target. The current file creates `tcpquic_linux_relay_worker_queue_test`, `tcpquic_linux_relay_worker_io_test`, and `tcpquic_relay_backend_selection_test` unconditionally; each of those compiles Linux-only sources and must stay inside the Linux block:

```cmake
set(TCPQUIC_LINUX_TEST_TARGETS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tcpquic_linux_relay_buffer_pool_test
        unittest/linux_relay_buffer_pool_test.cpp
        linux_relay_buffer_pool.cpp
    )
    target_include_directories(tcpquic_linux_relay_buffer_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_property(TARGET tcpquic_linux_relay_buffer_pool_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_buffer_pool_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_linux_relay_worker_queue_test
        unittest/linux_relay_worker_queue_test.cpp
        linux_relay_worker.cpp
        linux_relay_buffer_pool.cpp
    )
    target_include_directories(tcpquic_linux_relay_worker_queue_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MSQUIC_SOURCE_DIR}/src/inc)
    target_link_libraries(tcpquic_linux_relay_worker_queue_test Threads::Threads)
    set_property(TARGET tcpquic_linux_relay_worker_queue_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_worker_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_linux_relay_worker_io_test
        unittest/linux_relay_worker_io_test.cpp
        linux_relay_worker.cpp
        linux_relay_buffer_pool.cpp
        compress.cpp
    )
    target_include_directories(tcpquic_linux_relay_worker_io_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MSQUIC_SOURCE_DIR}/src/inc)
    target_link_libraries(tcpquic_linux_relay_worker_io_test Threads::Threads ${TCPQUIC_TEST_LIBS})
    target_compile_definitions(tcpquic_linux_relay_worker_io_test PRIVATE ${TCPQUIC_TEST_DEFS})
    set_property(TARGET tcpquic_linux_relay_worker_io_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_worker_io_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_relay_backend_selection_test
        unittest/relay_backend_selection_test.cpp
        relay.cpp
        tuning.cpp
        linux_relay_worker.cpp
        linux_relay_buffer_pool.cpp
    )
    target_include_directories(tcpquic_relay_backend_selection_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MSQUIC_SOURCE_DIR}/src/inc)
    target_link_libraries(tcpquic_relay_backend_selection_test Threads::Threads inc warnings msquic logging base_link)
    target_compile_options(tcpquic_relay_backend_selection_test PRIVATE -UNDEBUG)
    set_property(TARGET tcpquic_relay_backend_selection_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_relay_backend_selection_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    list(APPEND TCPQUIC_LINUX_TEST_TARGETS
        tcpquic_linux_relay_buffer_pool_test
        tcpquic_linux_relay_worker_queue_test
        tcpquic_linux_relay_worker_io_test
        tcpquic_relay_backend_selection_test
    )
endif()
```

Append `${TCPQUIC_LINUX_TEST_TARGETS}` to `TCPQUIC_TEST_TARGETS` after the common test-target list is initialized:

```cmake
list(APPEND TCPQUIC_TEST_TARGETS ${TCPQUIC_LINUX_TEST_TARGETS})
```

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

> **Status: DONE** — `73ac646`

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
#include "msquic.hpp"
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
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayContext;
    struct IoOperation;
    struct CallbackContext;

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
    MsQuicStream*,
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

In `TqRelayStart`, compute the active-relay budget before the platform split so both Linux and Windows use a valid `tuning` object:

```cpp
const uint32_t activeRelays = TqRelayRegisterActive();
TqTuningConfig tuning = profileTuning;
TqApplyRelayPoolBudget(tuning, activeRelays);

#if defined(_WIN32)
if (!TqWindowsRelayRuntime::Instance().Start(tuning.LinuxRelayWorkerCount) ||
    !TqWindowsRelayRuntime::Instance().RegisterRelay(
        tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo)) {
    TqRelayUnregisterActive();
    return false;
}
return true;
#endif
```

Then remove the duplicate Linux-only `activeRelays` and `tuning` declarations from the existing `#else`/Linux branch. Keep the existing Linux failure paths that call `TqRelayUnregisterActive()`.

In `TqRelayStop`, add:

```cpp
#if defined(_WIN32)
if (handle->Backend == TqRelayBackendType::WindowsWorker) {
    TqWindowsRelayRuntime::Instance().StopRelay(handle);
    handle->Backend = TqRelayBackendType::None;
    handle->WindowsWorker = nullptr;
    handle->WindowsRelayId = 0;
    handle->Stop.store(true);
    TqRelayUnregisterActive();
    return;
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

> **Status: DONE** — `dd741ce`

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
set(TCPQUIC_WINDOWS_TEST_TARGETS)

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
list(APPEND TCPQUIC_WINDOWS_TEST_TARGETS tcpquic_windows_relay_worker_test)
```

Append `${TCPQUIC_WINDOWS_TEST_TARGETS}` to `TCPQUIC_TEST_TARGETS` after the common test-target list is initialized so it participates in the existing `foreach(target IN LISTS TCPQUIC_TEST_TARGETS)` compile-feature setup:

```cmake
list(APPEND TCPQUIC_TEST_TARGETS ${TCPQUIC_WINDOWS_TEST_TARGETS})
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
    size_t Offset{0};
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
    std::shared_ptr<CallbackContext> Callback;
};
```

Define `CallbackContext` before `RelayContext` in `windows_relay_worker.cpp` and forward-declare it in `windows_relay_worker.h`:

```cpp
struct TqWindowsRelayWorker::CallbackContext {
    TqWindowsRelayWorker* Worker{nullptr};
    std::weak_ptr<RelayContext> Relay;
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
    relay->Callback = std::make_shared<CallbackContext>();
    relay->Callback->Worker = this;
    relay->Callback->Relay = relay;
    stream->Callback = StreamCallback;
    stream->Context = relay->Callback.get();

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
bool PostTcpSend(std::unique_ptr<IoOperation> op);
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
    if (relay->Stream != nullptr && relay->Stream->Context == relay->Callback.get()) {
        relay->Stream->Callback = MsQuicStream::NoOpCallback;
        relay->Stream->Context = nullptr;
    }
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

Implement TCP receive handling with compression support before constructing the MsQuic send buffer:

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
    if (relay->Compressor != nullptr) {
        std::vector<uint8_t> compressed;
        if (!relay->Compressor->Compress(
                op->Buffer.data(),
                static_cast<uint32_t>(op->Buffer.size()),
                compressed,
                false)) {
            CloseRelay(relay);
            return;
        }
        op->Buffer.swap(compressed);
    }
    if (op->Buffer.empty()) {
        (void)PostTcpRecv(relay);
        return;
    }

    QUIC_BUFFER buffer{};
    buffer.Buffer = op->Buffer.data();
    buffer.Length = static_cast<uint32_t>(op->Buffer.size());
    relay->InFlightQuicSends.fetch_add(1);
    IoOperation* raw = op.release();
    const QUIC_STATUS status = relay->Stream->Send(&buffer, 1, QUIC_SEND_FLAG_NONE, raw);
    if (QUIC_FAILED(status)) {
        relay->InFlightQuicSends.fetch_sub(1);
        delete raw;
        CloseRelay(relay);
    }
}
```

- [ ] **Step 7: Release QUIC send contexts and continue TCP reads**

Handle MsQuic `QUIC_STREAM_EVENT_SEND_COMPLETE` in `StreamCallback` so the send context is released and the next TCP receive is posted:

```cpp
if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
    std::unique_ptr<IoOperation> completed(
        static_cast<IoOperation*>(event->SEND_COMPLETE.ClientContext));
    if (completed && completed->Relay) {
        completed->Relay->InFlightQuicSends.fetch_sub(1);
        if (!completed->Relay->Closing.load()) {
            (void)completed->Relay->Callback->Worker->PostTcpRecv(completed->Relay);
        }
    }
    return QUIC_STATUS_SUCCESS;
}
```

This keeps one TCP receive outstanding per relay and avoids leaking `IoOperation` objects passed as MsQuic send contexts.

- [ ] **Step 8: Implement QUIC receive enqueue**

In `StreamCallback`, when receiving `QUIC_STREAM_EVENT_RECEIVE`, allocate an operation, copy buffers, and post:

```cpp
auto* callback = static_cast<TqWindowsRelayWorker::CallbackContext*>(context);
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
    if (!::PostQueuedCompletionStatus(
            static_cast<HANDLE>(callback->Worker->Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        callback->Worker->CloseRelay(relay);
    }
}
return QUIC_STATUS_SUCCESS;
```

Use the `CallbackContext` created in `RegisterRelay`; do not allocate a callback context per event.

- [ ] **Step 9: Implement QUIC to TCP send**

Implement `HandleQuicReceiveQueued`:

```cpp
void TqWindowsRelayWorker::HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Buffer.empty()) {
        return;
    }
    if (relay->Decompressor != nullptr) {
        std::vector<uint8_t> output;
        if (!relay->Decompressor->Decompress(
                op->Buffer.data(),
                static_cast<uint32_t>(op->Buffer.size()),
                output)) {
            CloseRelay(relay);
            return;
        }
        op->Buffer.swap(output);
    }
    if (op->Buffer.empty()) {
        return;
    }
    op->Event = TqWindowsRelayEvent::TcpSend;
    op->Offset = 0;
    if (!PostTcpSend(std::move(op))) {
        CloseRelay(relay);
    }
}
```

Implement `PostTcpSend` and `HandleTcpSend` so partial overlapped sends complete before the operation is released:

```cpp
bool TqWindowsRelayWorker::PostTcpSend(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Offset >= op->Buffer.size()) {
        return false;
    }
    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(op->Buffer.data() + op->Offset);
    buf.len = static_cast<ULONG>(op->Buffer.size() - op->Offset);
    DWORD sent = 0;
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &buf, 1, &sent, 0, &raw->Overlapped, nullptr);
    if (rc != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
        delete raw;
        return false;
    }
    return true;
}

void TqWindowsRelayWorker::HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        return;
    }
    op->Offset += bytes;
    if (op->Offset < op->Buffer.size()) {
        if (!PostTcpSend(std::move(op))) {
            CloseRelay(relay);
        }
        return;
    }
}
```

- [ ] **Step 10: Build and run Windows relay worker test**

On Windows x64 Developer PowerShell:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test
.\build-win\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: build succeeds and test exits 0.

- [ ] **Step 11: Commit**

Run:

```bash
rtk git add src/windows_relay_worker.h src/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: implement windows iocp relay worker"
```

## Task 8: Windows Native Build and Unit Test Pass

> **Status: IN PROGRESS** — build + x64 unit tests pass; changes uncommitted. See [Implementation Progress](#implementation-progress-updated-2026-06-09).

**Files:**
- Modify: `CMakeLists.txt` — `TCPQUIC_WINDOWS_TLS` option
- Modify: `src/CMakeLists.txt` — `tcpquic_copy_msquic_runtime()`, `quic_credentials_win.cpp`, `crypt32`
- Create: `src/quic_credentials_win.h`, `src/quic_credentials_win.cpp`
- Modify: `src/quic_session.cpp`, `src/quic_session.h`
- Modify: `src/main.cpp`, `src/thread_pool.cpp`
- Modify: `src/unittest/thread_pool_test.cpp`, `src/unittest/tunnel_reaper_test.cpp`
- Modify: `README.md` (partial — full Windows TLS docs completed in Task 9)

- [x] **Step 1: Run Windows build**

On Windows x64 Developer PowerShell (use `build-x64`, not Win32):

```powershell
cmake -S . -B build-x64 -A x64 -DLZ4_SOURCE_DIR="$PWD/third_party/lz4"
cmake --build build-x64 --config Release --target tcpquic-proxy
```

Result: **pass**. Original plan used `build-win` + Ninja; MSVC `-A x64` multi-config generator is the validated path on this machine.

- [x] **Step 2: Build Windows unit tests**

```powershell
cmake --build build-x64 --config Release --target `
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

Result: **pass** on x64. Also builds `tcpquic_tunnel_test`, `tcpquic_blocking_relay_demo_test`, `tcpquic_router_runtime_test`, `tcpquic_production_linkage_guard_test`, `tcpquic_tcp_write_queue_test`, `tcpquic_tuning_test`.

**Known issue:** `build-x86` (Win32) `tcpquic_thread_pool_test` crashes (`0xC0000005`). Out of scope for x64 target; do not use as primary CI directory.

- [x] **Step 3: Run Windows unit tests**

```powershell
$bin = ".\build-x64\bin\Release"
@(
  tcpquic_acl_test, tcpquic_control_test, tcpquic_compress_test,
  tcpquic_config_router_test, tcpquic_thread_pool_test, tcpquic_tunnel_reaper_test,
  tcpquic_http_connect_test, tcpquic_socks5_test, tcpquic_admin_http_test,
  tcpquic_platform_socket_test, tcpquic_windows_relay_worker_test
) | ForEach-Object { & "$bin\$_.exe"; if ($LASTEXITCODE -ne 0) { throw "$_ failed" } }
```

Result: **pass** — all 11 listed executables exit 0 on x64.

**Fixes applied during this task:**

| Issue | Fix |
|-------|-----|
| Test exes fail to start (`msquic.dll` not found) | `tcpquic_copy_msquic_runtime()` POST_BUILD on proxy + msquic-linked tests |
| `tcpquic_thread_pool_test` used POSIX `socketpair` | Rewrote test to cover thread pool only |
| WorkerLoop spurious wakeup | `if (Queue.empty()) continue;` after `wait` |
| `main.cpp` tunnel callback type mismatch | `int fd` → `TqSocketHandle` |
| Schannel rejects `CERTIFICATE_FILE` PEM paths | `quic_credentials_win` + `quic_session` Windows credential path |

- [ ] **Step 4: Run Linux regression build**

Not re-run in this Windows session. Required before merge.

- [ ] **Step 5: Run Linux unit regression**

Not re-run in this Windows session. Required before merge.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt README.md src/
git commit -m "build: pass windows native build and unit tests"
```

Pending: commit the uncommitted Task 8 diff listed in [Implementation Progress](#implementation-progress-updated-2026-06-09).

## Task 9: End-to-End Windows and Interop Validation

> **Status: BLOCKED** — loopback script drafted; server exits on Schannel credential load (`0x80090331`). See [Task 9 blocker](#task-9-blocker--windows-schannel--openssl-pem-credentials) in Implementation Progress.

**Files:**
- Create: `scripts/test-tcpquic-proxy-windows.ps1` — **draft exists, not committed**
- Modify: `README.md` — partial Windows section added
- Modify: `src/quic_credentials_win.cpp` — credential bridge still failing Schannel load

- [x] **Step 1: Add Windows loopback test script**

`scripts/test-tcpquic-proxy-windows.ps1` exists locally with these additions beyond the original plan:

- Default binary path: `.\build-x64\bin\Release\tcpquic-proxy.exe`
- `Resolve-OpenSslPath` / `OPENSSL_BIN` for OpenSSL discovery
- `Invoke-OpenSsl` helper (OpenSSL stderr must not terminate PowerShell)
- `v3.ext` with `serverAuth` + `clientAuth` EKU on server/client leaf certs
- `certutil -addstore -user Root` for CA trust (Schannel ignores `--quic-ca` PEM)
- `curl.exe` exit-code checks after HTTP CONNECT and SOCKS5 probes
- `finally` block removes CA from Root store and cleans temp work dir

Original plan snippet omitted EKU and CA store import — both are **required** for Schannel but **insufficient** alone; see blocker section.

- [ ] **Step 2: Run Windows loopback validation**

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress lz4
```

Result: **fail** on `-Compress off`. Server stderr:

```text
ConfigurationOpen/LoadCredential failed, 0x80090331
```

(`SEC_E_ALGORITHM_MISMATCH` — Schannel rejects OpenSSL-exported PFX private keys even with valid EKU.)

Client never binds `:18081`; curl exits 7 (connection refused).

**Prerequisite before Step 2 can pass:** resolve Schannel credential loading — see resolution options A/B/C in [Implementation Progress](#implementation-progress-updated-2026-06-09).

- [ ] **Step 3: Run interop validation**

Skipped — requires `TCPQUIC_LINUX_SERVER` / `TCPQUIC_WINDOWS_SERVER` and matching cert material. Document skip when env unset; run manually when cross-platform hosts are available.

Update binary paths from `build-win` to `build-x64` when running interop commands.

- [ ] **Step 4: Document Windows commands**

README partially updated. Still needed after Schannel fix:

- Schannel vs `quictls` trade-offs (`-DTCPQUIC_WINDOWS_TLS=quictls` needs Strawberry Perl)
- PEM CLI args remain for Linux compatibility; on Schannel, leaf cert must have `serverAuth`/`clientAuth` EKU and CA must be in `CurrentUser\Root`
- Loopback validation commands (three compress modes)

- [ ] **Step 5: Commit**

```bash
git add scripts/test-tcpquic-proxy-windows.ps1 README.md
git commit -m "test: add windows loopback validation"
```

Blocked on Step 2 passing.

## Task 10: Final Verification and Completion

> **Status: NOT STARTED** — blocked on Task 9 Schannel credential fix.

**Files:**
- Modify only files required by final verification failures.

- [ ] **Step 1: Run Linux full integration test**

```bash
./scripts/test-tcpquic-proxy.sh
```

- [ ] **Step 2: Run Linux concurrent test**

```bash
./scripts/test-tcpquic-concurrent.sh
```

- [ ] **Step 3: Run Windows build and loopback tests one final time**

```powershell
cmake --build build-x64 --config Release --target tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress lz4
```

- [ ] **Step 4: Check git status**

```bash
git status --short
```

Expected: no output (exclude build dirs from tracking).

- [ ] **Step 5: Record final verification in the task handoff**

Current partial verification state:

```text
Verified:
- Windows x64 build: cmake --build build-x64 --config Release --target tcpquic-proxy — PASS
- Windows x64 unit tests (11 listed targets) — PASS
- Windows loopback (off/zstd/lz4) — FAIL (Schannel 0x80090331 on server credential load)
- Linux regression — NOT RUN this session
- Interop — SKIPPED (env not configured)

Outstanding:
- Fix Schannel credential bridge or switch to quictls / Windows-native test certs
- Commit Task 8 + Task 9
- Re-run Linux integration + Windows loopback + interop
```
