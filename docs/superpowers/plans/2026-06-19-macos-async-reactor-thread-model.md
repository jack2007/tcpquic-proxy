# macOS Async Reactor Thread Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 macOS client 侧 accept + SOCKS5/HTTP CONNECT handshake 收敛到单一 ingress reactor 线程，并将 macOS server 侧普通 OPEN 的 DNS + TCP connect 收敛到单一 dial reactor 线程。

**Architecture:** 在不改动 `TqDarwinRelayWorker` 数据面的前提下，新增 Darwin `kqueue` 控制面 reactor，实现已有 `ITqSocketReactor`；复用 Linux/Windows 已落地的 `TqClientIngressReactor`、`TqAresDnsResolver`、`TqServerDialReactor`，并把 Darwin CMake/runtime 接入统一异步控制面路径。

**Tech Stack:** C++17, CMake, Darwin `kqueue` / `EVFILT_USER`, POSIX sockets, c-ares v1.34.6, MsQuic callbacks, existing assert-style unittest executable style.

---

## File Structure

新增文件：

- `src/runtime/darwin_reactor.h`  
  Darwin 控制面 reactor，实现 `ITqSocketReactor`，用 kqueue 管理 read/write/error readiness 和 wake。
- `src/runtime/darwin_reactor.cpp`  
  `TqDarwinReactor` 实现。
- `src/unittest/darwin_reactor_test.cpp`  
  覆盖 wake、read readiness、write readiness、modify、remove、peer close/error。

修改文件：

- `src/tunnel/server_dial_reactor.cpp`  
  `MakeSocketReactor()` 增加 Darwin 分支，返回 `TqDarwinReactor`。
- `src/tunnel/ares_dns_resolver.cpp`  
  c-ares resolver 内部 reactor 类型增加 Darwin 分支。
- `src/CMakeLists.txt`  
  Darwin proxy sources 增加控制面源文件；Darwin 测试 target 增加 reactor、ingress、dial、ares 相关目标。
- `src/runtime/listen_socket.cpp`  
  如果 macOS 编译暴露 `SOCK_NONBLOCK` / `SOCK_CLOEXEC` 不兼容，补 POSIX fallback。
- `src/ingress/client_ingress_reactor.cpp`  
  如果 macOS 编译暴露 `accept4()` 等 Linux-only 依赖，改为跨平台 accept fallback。
- `src/platform/platform_socket*.cpp`  
  如有必要，补齐 macOS socket error / no-signal send 语义。
- `docs/thread-model_cn.md`  
  更新 macOS 控制面线程模型。
- `docs/superpowers/specs/2026-06-19-macos-async-reactor-thread-model-design.md`  
  实现过程中若发现约束变化，同步更新设计文档。

---

### Task 1: Add Darwin Reactor Primitive

**Files:**
- Create: `src/runtime/darwin_reactor.h`
- Create: `src/runtime/darwin_reactor.cpp`
- Create: `src/unittest/darwin_reactor_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create Darwin reactor header**

Create `src/runtime/darwin_reactor.h`:

```cpp
#pragma once

#include "socket_reactor.h"

#if defined(__APPLE__)
#include <unordered_map>
#endif

class TqDarwinReactor final : public ITqSocketReactor {
public:
    TqDarwinReactor() = default;
    ~TqDarwinReactor() override;

    TqDarwinReactor(const TqDarwinReactor&) = delete;
    TqDarwinReactor& operator=(const TqDarwinReactor&) = delete;

    bool Start() override;
    void Stop() override;
    bool Add(TqSocketHandle fd, uint32_t events, Handler handler) override;
    bool Modify(TqSocketHandle fd, uint32_t events) override;
    bool Remove(TqSocketHandle fd) override;
    bool Wake() override;
    bool RunOnce(int timeoutMs) override;

private:
#if defined(__APPLE__)
    bool ApplyFilters(TqSocketHandle fd, uint32_t events, bool addMissing);
    bool DeleteFilter(TqSocketHandle fd, int16_t filter);

    int KqueueFd{-1};
    std::unordered_map<TqSocketHandle, Handler> Handlers;
#else
    bool Started{false};
#endif
};
```

- [ ] **Step 2: Create Darwin reactor implementation**

Create `src/runtime/darwin_reactor.cpp`:

```cpp
#include "darwin_reactor.h"

#if defined(__APPLE__)

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <vector>

namespace {
constexpr uintptr_t kWakeIdent = 1;
constexpr int kMaxEvents = 64;

void SetEvent(struct kevent& event, uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags) {
    EV_SET(&event, ident, filter, flags, fflags, 0, nullptr);
}

uint32_t ToReactorEvents(const struct kevent& event) {
    uint32_t result = 0;
    if (event.filter == EVFILT_READ) {
        result |= TqReactorEvents::Read;
    }
    if (event.filter == EVFILT_WRITE) {
        result |= TqReactorEvents::Write;
    }
    if ((event.flags & (EV_EOF | EV_ERROR)) != 0) {
        result |= TqReactorEvents::Error;
    }
    return result;
}

} // namespace

TqDarwinReactor::~TqDarwinReactor() {
    Stop();
}

bool TqDarwinReactor::Start() {
    if (KqueueFd >= 0) {
        return true;
    }

    KqueueFd = ::kqueue();
    if (KqueueFd < 0) {
        return false;
    }

    struct kevent change;
    SetEvent(change, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0);
    if (::kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) != 0) {
        ::close(KqueueFd);
        KqueueFd = -1;
        return false;
    }
    return true;
}

void TqDarwinReactor::Stop() {
    if (KqueueFd >= 0) {
        ::close(KqueueFd);
        KqueueFd = -1;
    }
    Handlers.clear();
}

bool TqDarwinReactor::DeleteFilter(TqSocketHandle fd, int16_t filter) {
    if (KqueueFd < 0) {
        return false;
    }
    struct kevent change;
    SetEvent(change, static_cast<uintptr_t>(fd), filter, EV_DELETE, 0);
    if (::kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) == 0) {
        return true;
    }
    return errno == ENOENT;
}

bool TqDarwinReactor::ApplyFilters(TqSocketHandle fd, uint32_t events, bool addMissing) {
    if (KqueueFd < 0 || !TqSocketValid(fd)) {
        return false;
    }

    std::vector<struct kevent> changes;
    changes.reserve(2);

    if ((events & TqReactorEvents::Read) != 0) {
        struct kevent change;
        SetEvent(change, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0);
        changes.push_back(change);
    } else if (!addMissing) {
        (void)DeleteFilter(fd, EVFILT_READ);
    }

    if ((events & TqReactorEvents::Write) != 0) {
        struct kevent change;
        SetEvent(change, static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0);
        changes.push_back(change);
    } else if (!addMissing) {
        (void)DeleteFilter(fd, EVFILT_WRITE);
    }

    if (changes.empty()) {
        return true;
    }
    return ::kevent(KqueueFd, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) == 0;
}

bool TqDarwinReactor::Add(TqSocketHandle fd, uint32_t events, Handler handler) {
    if (!TqSocketValid(fd) || !handler || events == 0 || Handlers.find(fd) != Handlers.end()) {
        return false;
    }
    if (!ApplyFilters(fd, events, true)) {
        return false;
    }
    Handlers.emplace(fd, std::move(handler));
    return true;
}

bool TqDarwinReactor::Modify(TqSocketHandle fd, uint32_t events) {
    if (!TqSocketValid(fd) || events == 0 || Handlers.find(fd) == Handlers.end()) {
        return false;
    }
    return ApplyFilters(fd, events, false);
}

bool TqDarwinReactor::Remove(TqSocketHandle fd) {
    const bool existed = Handlers.erase(fd) > 0;
    (void)DeleteFilter(fd, EVFILT_READ);
    (void)DeleteFilter(fd, EVFILT_WRITE);
    return existed;
}

bool TqDarwinReactor::Wake() {
    if (KqueueFd < 0) {
        return false;
    }
    struct kevent change;
    SetEvent(change, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER);
    return ::kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) == 0;
}

bool TqDarwinReactor::RunOnce(int timeoutMs) {
    if (KqueueFd < 0) {
        return false;
    }

    struct timespec timeout{};
    struct timespec* timeoutPtr = nullptr;
    if (timeoutMs >= 0) {
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_nsec = static_cast<long>(timeoutMs % 1000) * 1000000L;
        timeoutPtr = &timeout;
    }

    struct kevent events[kMaxEvents];
    const int count = ::kevent(KqueueFd, nullptr, 0, events, kMaxEvents, timeoutPtr);
    if (count < 0) {
        return errno == EINTR;
    }

    bool didWork = false;
    for (int i = 0; i < count; ++i) {
        const auto& event = events[i];
        if (event.filter == EVFILT_USER && event.ident == kWakeIdent) {
            didWork = true;
            continue;
        }

        const auto fd = static_cast<TqSocketHandle>(event.ident);
        auto it = Handlers.find(fd);
        if (it == Handlers.end()) {
            continue;
        }

        const uint32_t reactorEvents = ToReactorEvents(event);
        if (reactorEvents == 0) {
            continue;
        }
        auto handler = it->second;
        handler(fd, reactorEvents);
        didWork = true;
    }
    return didWork;
}

#else

TqDarwinReactor::~TqDarwinReactor() = default;
bool TqDarwinReactor::Start() { Started = true; return true; }
void TqDarwinReactor::Stop() { Started = false; }
bool TqDarwinReactor::Add(TqSocketHandle, uint32_t, Handler) { return false; }
bool TqDarwinReactor::Modify(TqSocketHandle, uint32_t) { return false; }
bool TqDarwinReactor::Remove(TqSocketHandle) { return false; }
bool TqDarwinReactor::Wake() { return false; }
bool TqDarwinReactor::RunOnce(int) { return false; }

#endif
```

- [ ] **Step 3: Add Darwin reactor test**

Create `src/unittest/darwin_reactor_test.cpp`:

```cpp
#include "darwin_reactor.h"
#include "platform_socket.h"

#include <cassert>
#include <chrono>
#include <thread>

#if defined(__APPLE__)
namespace {

bool WaitUntil(TqDarwinReactor& reactor, bool& flag, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!flag && std::chrono::steady_clock::now() < deadline) {
        (void)reactor.RunOnce(25);
    }
    return flag;
}

void MakeSocketPair(TqSocketHandle out[2]) {
    assert(TqSocketPair(out));
    assert(TqSocketValid(out[0]));
    assert(TqSocketValid(out[1]));
    assert(TqSetNonBlocking(out[0]));
    assert(TqSetNonBlocking(out[1]));
}

void TestWake() {
    TqDarwinReactor reactor;
    assert(reactor.Start());
    assert(reactor.Wake());
    assert(reactor.RunOnce(100));
    reactor.Stop();
}

void TestReadReadiness() {
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    MakeSocketPair(pair);

    TqDarwinReactor reactor;
    assert(reactor.Start());
    bool readable = false;
    assert(reactor.Add(pair[0], TqReactorEvents::Read, [&](TqSocketHandle fd, uint32_t events) {
        assert(fd == pair[0]);
        if ((events & TqReactorEvents::Read) != 0) {
            readable = true;
        }
    }));

    const char byte = 'x';
    assert(TqSend(pair[1], &byte, 1, TqSendFlags::None) == 1);
    assert(WaitUntil(reactor, readable, 1000));

    reactor.Stop();
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
}

void TestWriteReadiness() {
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    MakeSocketPair(pair);

    TqDarwinReactor reactor;
    assert(reactor.Start());
    bool writable = false;
    assert(reactor.Add(pair[0], TqReactorEvents::Write, [&](TqSocketHandle fd, uint32_t events) {
        assert(fd == pair[0]);
        if ((events & TqReactorEvents::Write) != 0) {
            writable = true;
        }
    }));
    assert(WaitUntil(reactor, writable, 1000));

    reactor.Stop();
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
}

void TestModifyReadToWrite() {
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    MakeSocketPair(pair);

    TqDarwinReactor reactor;
    assert(reactor.Start());
    bool readable = false;
    bool writable = false;
    assert(reactor.Add(pair[0], TqReactorEvents::Read, [&](TqSocketHandle, uint32_t events) {
        if ((events & TqReactorEvents::Read) != 0) {
            readable = true;
        }
        if ((events & TqReactorEvents::Write) != 0) {
            writable = true;
        }
    }));
    assert(reactor.Modify(pair[0], TqReactorEvents::Write));
    assert(WaitUntil(reactor, writable, 1000));
    assert(!readable);

    reactor.Stop();
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
}

void TestRemoveSuppressesDispatch() {
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    MakeSocketPair(pair);

    TqDarwinReactor reactor;
    assert(reactor.Start());
    bool called = false;
    assert(reactor.Add(pair[0], TqReactorEvents::Read, [&](TqSocketHandle, uint32_t) {
        called = true;
    }));
    assert(reactor.Remove(pair[0]));

    const char byte = 'x';
    assert(TqSend(pair[1], &byte, 1, TqSendFlags::None) == 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)reactor.RunOnce(10);
    }
    assert(!called);

    reactor.Stop();
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
}

} // namespace
#endif

int main() {
#if defined(__APPLE__)
    TqSocketStartup startup;
    assert(startup.Ok());
    TestWake();
    TestReadReadiness();
    TestWriteReadiness();
    TestModifyReadToWrite();
    TestRemoveSuppressesDispatch();
#endif
    return 0;
}
```

- [ ] **Step 4: Add CMake target and verify failure first**

Modify `src/CMakeLists.txt` inside the Darwin test section:

```cmake
add_executable(tcpquic_darwin_reactor_test
    unittest/darwin_reactor_test.cpp
    runtime/darwin_reactor.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_darwin_reactor_test)
target_link_libraries(tcpquic_darwin_reactor_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_darwin_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_darwin_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run on macOS:

```bash
cmake --build build --target tcpquic_darwin_reactor_test -j2
```

Expected: build succeeds after the implementation is present.

- [ ] **Step 5: Run Darwin reactor test**

```bash
./build/src/tcpquic_darwin_reactor_test
```

Expected: executable exits `0`.

- [ ] **Step 6: Commit**

```bash
git add src/runtime/darwin_reactor.h src/runtime/darwin_reactor.cpp src/unittest/darwin_reactor_test.cpp src/CMakeLists.txt
git commit -m "feat(runtime): add Darwin socket reactor"
```

---

### Task 2: Wire Darwin Reactor Into Dial and DNS

**Files:**
- Modify: `src/tunnel/server_dial_reactor.cpp`
- Modify: `src/tunnel/ares_dns_resolver.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Update server dial reactor includes and factory**

In `src/tunnel/server_dial_reactor.cpp`, replace the reactor include section and factory with Darwin-aware selection:

```cpp
#if defined(_WIN32)
#include "windows_reactor.h"
#elif defined(__APPLE__)
#include "darwin_reactor.h"
#else
#include "linux_reactor.h"
#endif
```

```cpp
std::unique_ptr<ITqSocketReactor> MakeSocketReactor() {
#if defined(_WIN32)
    return std::make_unique<TqWindowsReactor>();
#elif defined(__APPLE__)
    return std::make_unique<TqDarwinReactor>();
#else
    return std::make_unique<TqLinuxReactor>();
#endif
}
```

- [ ] **Step 2: Update c-ares resolver reactor selection**

In `src/tunnel/ares_dns_resolver.cpp`, replace the reactor include and alias block with:

```cpp
#if defined(_WIN32)
#include "windows_reactor.h"
#elif defined(__APPLE__)
#include "darwin_reactor.h"
#else
#include "linux_reactor.h"
#endif
```

```cpp
#if defined(_WIN32)
using TqAresReactor = TqWindowsReactor;
#elif defined(__APPLE__)
using TqAresReactor = TqDarwinReactor;
#else
using TqAresReactor = TqLinuxReactor;
#endif
```

- [ ] **Step 3: Update CMake test source selection**

For `TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES`, change platform selection to:

```cmake
if(WIN32)
    list(APPEND TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES runtime/windows_reactor.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES runtime/darwin_reactor.cpp)
else()
    list(APPEND TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES runtime/linux_reactor.cpp)
endif()
```

For `TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES`, change platform selection to:

```cmake
if(WIN32)
    list(APPEND TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES runtime/windows_reactor.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES runtime/darwin_reactor.cpp)
else()
    list(APPEND TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES runtime/linux_reactor.cpp)
endif()
```

- [ ] **Step 4: Build DNS and dial tests on macOS**

```bash
cmake --build build --target tcpquic_ares_dns_resolver_test -j2
cmake --build build --target tcpquic_server_dial_reactor_test -j2
```

Expected: both targets build successfully.

- [ ] **Step 5: Run DNS and dial tests on macOS**

```bash
./build/src/tcpquic_ares_dns_resolver_test
./build/src/tcpquic_server_dial_reactor_test
```

Expected: both executables exit `0`.

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/server_dial_reactor.cpp src/tunnel/ares_dns_resolver.cpp src/CMakeLists.txt
git commit -m "feat(tunnel): use Darwin reactor for async DNS and dial"
```

---

### Task 3: Wire Darwin Client Ingress Reactor Tests

**Files:**
- Modify: `src/CMakeLists.txt`
- Possibly Modify: `src/ingress/client_ingress_reactor.cpp`
- Possibly Modify: `src/runtime/listen_socket.cpp`

- [ ] **Step 1: Add Darwin client ingress reactor test target**

Inside `if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")`, add:

```cmake
add_executable(tcpquic_client_ingress_reactor_test
    unittest/client_ingress_reactor_test.cpp
    unittest/trace_proxy_stub.cpp
    ingress/client_ingress_reactor.cpp
    runtime/listen_socket.cpp
    ingress/client_ingress_state.cpp
    ingress/socks5_server.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    protocol/control_protocol.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
    runtime/darwin_reactor.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_client_ingress_reactor_test)
target_link_libraries(tcpquic_client_ingress_reactor_test PRIVATE Threads::Threads)
target_compile_definitions(tcpquic_client_ingress_reactor_test PRIVATE TQ_UNIT_TESTING=1)
set_property(TARGET tcpquic_client_ingress_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_ingress_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

- [ ] **Step 2: Build client ingress reactor test on macOS**

```bash
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
```

Expected: build succeeds. If it fails on `accept4()`, `SOCK_NONBLOCK`, `SOCK_CLOEXEC`, or no-signal send differences, continue with Step 3.

- [ ] **Step 3: Fix macOS POSIX socket differences if needed**

If `src/runtime/listen_socket.cpp` fails because macOS does not expose Linux-style socket flags, change the non-Windows socket creation path to prefer atomic flags only when available and otherwise use `TqPrepareListenSocket()`:

```cpp
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    TqScopedSocket fd(::socket(
        ai->ai_family,
        ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
        ai->ai_protocol));
    if (TqSocketValid(fd.Get())) {
        return fd.Release();
    }

    const int socketError = errno;
    if (socketError != EINVAL && socketError != EPROTONOSUPPORT) {
        return TqInvalidSocket;
    }
#endif

    TqScopedSocket fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (!TqSocketValid(fd.Get()) || !TqPrepareListenSocket(fd.Get())) {
        return TqInvalidSocket;
    }
    return fd.Release();
```

If ingress uses `accept4()`, replace the Darwin path with `accept()` followed by `TqSetNonBlocking()` and close-on-exec setup.

- [ ] **Step 4: Run client ingress reactor test on macOS**

```bash
./build/src/tcpquic_client_ingress_reactor_test
```

Expected: executable exits `0`.

- [ ] **Step 5: Commit**

```bash
git add src/CMakeLists.txt src/runtime/listen_socket.cpp src/ingress/client_ingress_reactor.cpp
git commit -m "test(ingress): enable client ingress reactor on Darwin"
```

Only include `listen_socket.cpp` / `client_ingress_reactor.cpp` if they were changed.

---

### Task 4: Wire Darwin Runtime Sources Into tcpquic-proxy

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add Darwin control-plane sources to proxy target**

Change the Darwin `TCPQUIC_PROXY_SOURCES` block to include both control plane and relay data plane:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_PROXY_SOURCES
        ingress/client_ingress_reactor.cpp
        ingress/client_ingress_state.cpp
        tunnel/client_tunnel_open.cpp
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
        runtime/darwin_reactor.cpp
    )
endif()
```

- [ ] **Step 2: Ensure Darwin links Threads and c-ares**

If current link rules only add `Threads::Threads` for Linux, broaden to Darwin:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(tcpquic-proxy PRIVATE Threads::Threads)
endif()
```

`tcpquic-proxy` already links `${TCPQUIC_CARES_TARGET}` unconditionally; keep that behavior.

- [ ] **Step 3: Build proxy on macOS**

```bash
cmake --build build --target tcpquic-proxy -j2
```

Expected: target builds successfully.

- [ ] **Step 4: Commit**

```bash
git add src/CMakeLists.txt
git commit -m "build: enable async control plane on Darwin"
```

---

### Task 5: Enable Darwin Speed-Test / Worker Smoke Coverage

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add Darwin sources to speed-test test**

For `tcpquic_speed_test_test`, add a Darwin branch:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(tcpquic_speed_test_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/darwin_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
    )
endif()
```

- [ ] **Step 2: Link c-ares for Darwin speed-test target**

Change:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
    target_link_libraries(tcpquic_speed_test_test PRIVATE ${TCPQUIC_CARES_TARGET})
endif()
```

to:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
    target_link_libraries(tcpquic_speed_test_test PRIVATE ${TCPQUIC_CARES_TARGET})
endif()
```

- [ ] **Step 3: Build and run speed-test unit target on macOS**

```bash
cmake --build build --target tcpquic_speed_test_test -j2
./build/src/tcpquic_speed_test_test
```

Expected: target builds and exits `0`.

- [ ] **Step 4: Commit**

```bash
git add src/CMakeLists.txt
git commit -m "test: cover Darwin async control plane in speed tests"
```

---

### Task 6: Update Thread Model Documentation

**Files:**
- Modify: `docs/thread-model_cn.md`
- Modify: `docs/superpowers/specs/2026-06-19-macos-async-reactor-thread-model-design.md` if implementation constraints changed

- [ ] **Step 1: Update overview**

In `docs/thread-model_cn.md`, update the overview bullets to include macOS:

```markdown
- **Client ingress 控制面**：所有 peer 的 SOCKS5 / HTTP CONNECT `listen`、`accept` 和握手状态机由一个 `TqClientIngressReactor` 线程处理。Linux 底层使用 `TqLinuxReactor`，Windows 底层使用 `TqWindowsReactor`，macOS 底层使用 `TqDarwinReactor`。
```

- [ ] **Step 2: Update platform reactor boundary**

Add macOS to the platform reactor section:

```markdown
- **macOS**：`TqDarwinReactor` 使用 `kqueue` / `EVFILT_USER`。
```

- [ ] **Step 3: Update relay worker section**

Add macOS data plane wording:

```markdown
- **macOS**：固定数量 `TqDarwinRelayWorker`，使用 kqueue 处理 relay fd readiness 和跨线程唤醒。
```

- [ ] **Step 4: Update debugging boundary**

Change the final boundary note from Windows-only wording to three-platform wording:

```markdown
Linux / Windows / macOS 上的旧 listener thread / handshake pool / per-OPEN dial worker 路径均不再是主路径；当前三平台 client 和 server 控制面都走跨平台 reactor 模型。
```

- [ ] **Step 5: Commit**

```bash
git add docs/thread-model_cn.md docs/superpowers/specs/2026-06-19-macos-async-reactor-thread-model-design.md
git commit -m "docs: document Darwin async reactor thread model"
```

Only include the design doc if implementation changed the recorded design.

---

### Task 7: Full Verification

**Files:**
- No source changes expected unless verification exposes issues.

- [ ] **Step 1: Run macOS control-plane targets**

```bash
cmake --build build --target tcpquic_darwin_reactor_test -j2
cmake --build build --target tcpquic_ares_dns_resolver_test -j2
cmake --build build --target tcpquic_server_dial_reactor_test -j2
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
cmake --build build --target tcpquic-proxy -j2
```

Expected: all builds succeed.

- [ ] **Step 2: Run macOS control-plane tests**

```bash
./build/src/tcpquic_darwin_reactor_test
./build/src/tcpquic_ares_dns_resolver_test
./build/src/tcpquic_server_dial_reactor_test
./build/src/tcpquic_client_ingress_reactor_test
```

Expected: all executables exit `0`.

- [ ] **Step 3: Run Darwin relay regression tests**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_queue_test -j2
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j2
cmake --build build --target tcpquic_darwin_relay_metrics_test -j2
./build/src/tcpquic_darwin_relay_worker_queue_test
./build/src/tcpquic_darwin_relay_worker_io_test
./build/src/tcpquic_darwin_relay_metrics_test
```

Expected: all executables exit `0`.

- [ ] **Step 4: Run representative Linux regression**

On Linux build environment:

```bash
cmake --build build --target tcpquic_linux_reactor_test -j2
cmake --build build --target tcpquic_server_dial_reactor_test -j2
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
./build/src/tcpquic_linux_reactor_test
./build/src/tcpquic_server_dial_reactor_test
./build/src/tcpquic_client_ingress_reactor_test
```

Expected: all executables exit `0`.

- [ ] **Step 5: Run representative Windows regression**

On Windows build environment:

```powershell
cmake --build build-win-verify --target tcpquic_windows_reactor_test --config Debug
cmake --build build-win-verify --target tcpquic_server_dial_reactor_test --config Debug
cmake --build build-win-verify --target tcpquic_client_ingress_reactor_test --config Debug
```

Expected: all builds succeed and corresponding executables exit `0`.

- [ ] **Step 6: Manual integration smoke**

Run these combinations:

1. macOS client SOCKS5 -> Linux or Windows server.
2. macOS client HTTP CONNECT -> Linux or Windows server.
3. Linux or Windows client -> macOS server with domain target, verifying c-ares async DNS path.
4. macOS client/server speed-test upload and download.

Expected:

- macOS client does not create per-peer listener thread / handshake pool.
- macOS server ordinary OPEN does not create per-OPEN detached dial worker.
- Established tunnels are handed off to `TqDarwinRelayWorker` data plane.

- [ ] **Step 7: Final commit if verification fixes were needed**

```bash
git add <fixed-files>
git commit -m "fix: stabilize Darwin async reactor verification"
```

Skip this commit if no verification fixes were needed.

---

## Self-Review

- Spec coverage: The plan covers the design requirements: Darwin reactor primitive, DNS/dial integration, ingress integration, CMake wiring, docs, and full verification.
- Placeholder scan: No TODO/TBD placeholders remain; optional files are explicitly marked as only included if changed.
- Type consistency: Plan uses existing `ITqSocketReactor`, `TqSocketHandle`, `TqReactorEvents`, `TqClientIngressReactor`, `TqServerDialReactor`, and introduces `TqDarwinReactor` consistently.
- Scope check: Relay data plane is intentionally excluded except for regression coverage; existing `TqDarwinRelayWorker` plan remains separate.
