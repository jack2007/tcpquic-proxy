# Windows Async Reactor Thread Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Windows client 侧 accept + SOCKS5/HTTP CONNECT handshake 收敛到单一 ingress reactor 线程，并将 Windows server 侧普通 OPEN 的 DNS + TCP connect 收敛到单一 dial reactor 线程。

**Architecture:** 在不改动 `TqWindowsRelayWorker` 数据面的前提下，新增跨平台 socket reactor 接口和 Windows `WSAEventSelect` reactor；把已落地 Linux `TqClientIngressReactor`、`TqAresDnsResolver`、`TqServerDialReactor` 从 Linux-only 依赖改为平台 reactor 依赖，再让 Windows runtime 走同一套异步控制面路径。

**Tech Stack:** C++17, CMake, Winsock2 `WSAEventSelect` / `WSAWaitForMultipleEvents`, existing Linux `epoll` wrapper, c-ares v1.34.6, MsQuic callbacks, existing unittest executable style.

---

## File Structure

新增文件：

- `src/runtime/socket_reactor.h`  
  跨平台控制面 reactor 接口与事件常量。
- `src/runtime/windows_reactor.h/.cpp`  
  Windows `WSAEventSelect` reactor，实现 add/modify/remove/wake/run-once。
- `src/unittest/windows_reactor_test.cpp`  
  Windows reactor 的 wake、read、write/connect、remove、超过 64 event 分片测试。
- `src/runtime/listen_socket.h/.cpp`  
  跨平台 non-blocking listen socket helper，替换 `client_ingress_reactor.cpp` 内 Linux-only listen helper。
- `src/runtime/scoped_socket.h`  
  跨平台 RAII socket wrapper，使用 `TqSocketHandle` 和 `TqCloseSocket()`。

修改文件：

- `src/runtime/linux_reactor.h/.cpp`  
  实现 `ITqSocketReactor`，事件常量迁移到 `socket_reactor.h`。
- `src/tunnel/ares_dns_resolver.cpp`  
  内部 reactor 从 `TqLinuxReactor` 改为平台 reactor。
- `src/tunnel/server_dial_reactor.cpp`  
  移除 Linux-only socket/error 假设，使用 `TqSocketHandle`、平台 reactor、Winsock 错误分类。
- `src/tunnel/tcp_tunnel.cpp`  
  允许 Windows 使用 `TqServerDialReactor`，fallback legacy path 只在未设置 reactor 时使用。
- `src/ingress/client_ingress_reactor.h/.cpp`  
  使用平台 reactor 和跨平台 listen helper，不直接 include `linux_reactor.h`。
- `src/main.cpp`  
  Windows client runtime 改用 async ingress；Windows server runtime 创建 / 设置 dial reactor。
- `src/CMakeLists.txt`  
  添加新源文件和 Windows 测试目标，把部分 Linux-only 测试扩展到 Windows。
- `docs/thread-model_cn.md`  
  更新 Windows 新线程模型。
- `docs/superpowers/specs/2026-06-19-windows-async-reactor-thread-model-design.md`  
  实现过程中若发现约束变化，同步更新设计文档。

---

### Task 1: Add Cross-Platform Reactor Interface

**Files:**
- Create: `src/runtime/socket_reactor.h`
- Modify: `src/runtime/linux_reactor.h`
- Modify: `src/runtime/linux_reactor.cpp`
- Test: `src/unittest/linux_reactor_test.cpp`

- [ ] **Step 1: Create reactor interface header**

Create `src/runtime/socket_reactor.h`:

```cpp
#pragma once

#include "platform_socket.h"

#include <cstdint>
#include <functional>

namespace TqReactorEvents {
constexpr uint32_t Read = 0x1;
constexpr uint32_t Write = 0x2;
constexpr uint32_t Error = 0x4;
} // namespace TqReactorEvents

class ITqSocketReactor {
public:
    using Handler = std::function<void(TqSocketHandle fd, uint32_t events)>;

    virtual ~ITqSocketReactor() = default;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool Add(TqSocketHandle fd, uint32_t events, Handler handler) = 0;
    virtual bool Modify(TqSocketHandle fd, uint32_t events) = 0;
    virtual bool Remove(TqSocketHandle fd) = 0;
    virtual bool Wake() = 0;
    virtual bool RunOnce(int timeoutMs) = 0;
};
```

- [ ] **Step 2: Update Linux reactor declaration**

Modify `src/runtime/linux_reactor.h` so it includes `socket_reactor.h`, removes the local `TqLinuxReactorEvents` namespace, and implements the interface:

```cpp
#pragma once

#include "socket_reactor.h"

#include <unordered_map>

class TqLinuxReactor final : public ITqSocketReactor {
public:
    TqLinuxReactor() = default;
    ~TqLinuxReactor() override;

    TqLinuxReactor(const TqLinuxReactor&) = delete;
    TqLinuxReactor& operator=(const TqLinuxReactor&) = delete;

    bool Start() override;
    void Stop() override;
    bool Add(TqSocketHandle fd, uint32_t events, Handler handler) override;
    bool Modify(TqSocketHandle fd, uint32_t events) override;
    bool Remove(TqSocketHandle fd) override;
    bool Wake() override;
    bool RunOnce(int timeoutMs) override;

private:
    int EpollFd{-1};
    int WakeFd{-1};
    std::unordered_map<TqSocketHandle, Handler> Handlers;
};
```

- [ ] **Step 3: Rename event constants in Linux implementation**

In `src/runtime/linux_reactor.cpp`, replace every `TqLinuxReactorEvents::Read` with `TqReactorEvents::Read`, every `TqLinuxReactorEvents::Write` with `TqReactorEvents::Write`, and every `TqLinuxReactorEvents::Error` with `TqReactorEvents::Error`.

The helper bodies should retain the existing behavior:

```cpp
uint32_t ToEpollEvents(uint32_t events) {
    uint32_t epollEvents = EPOLLRDHUP;
    if ((events & TqReactorEvents::Read) != 0) {
        epollEvents |= EPOLLIN;
    }
    if ((events & TqReactorEvents::Write) != 0) {
        epollEvents |= EPOLLOUT;
    }
    return epollEvents;
}
```

```cpp
uint32_t FromEpollEvents(uint32_t events) {
    uint32_t reactorEvents = 0;
    if ((events & EPOLLIN) != 0) {
        reactorEvents |= TqReactorEvents::Read;
    }
    if ((events & EPOLLOUT) != 0) {
        reactorEvents |= TqReactorEvents::Write;
    }
    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        reactorEvents |= TqReactorEvents::Error;
    }
    return reactorEvents;
}
```

- [ ] **Step 4: Run Linux reactor test**

Run on Linux build environment:

```bash
cmake --build build --target tcpquic_linux_reactor_test -j2
./build/src/tcpquic_linux_reactor_test
```

Expected: build succeeds and test exits `0`.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/socket_reactor.h src/runtime/linux_reactor.h src/runtime/linux_reactor.cpp src/unittest/linux_reactor_test.cpp
git commit -m "refactor: introduce socket reactor interface"
```

---

### Task 2: Implement Windows Reactor

**Files:**
- Create: `src/runtime/windows_reactor.h`
- Create: `src/runtime/windows_reactor.cpp`
- Create: `src/unittest/windows_reactor_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write Windows reactor test**

Create `src/unittest/windows_reactor_test.cpp`:

```cpp
#include "platform_socket.h"
#include "windows_reactor.h"

#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#if defined(_WIN32)
namespace {

bool WaitUntil(TqWindowsReactor& reactor, bool& flag, int timeoutMs) {
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
    TqWindowsReactor reactor;
    assert(reactor.Start());
    assert(reactor.Wake());
    assert(reactor.RunOnce(100));
    reactor.Stop();
}

void TestReadReadiness() {
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    MakeSocketPair(pair);

    TqWindowsReactor reactor;
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

void TestRemoveSuppressesDispatch() {
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    MakeSocketPair(pair);

    TqWindowsReactor reactor;
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

void TestMoreThanMaximumWaitEvents() {
    TqWindowsReactor reactor;
    assert(reactor.Start());

    std::vector<TqSocketHandle> sockets;
    bool observed = false;
    for (int i = 0; i < WSA_MAXIMUM_WAIT_EVENTS + 4; ++i) {
        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        MakeSocketPair(pair);
        sockets.push_back(pair[0]);
        sockets.push_back(pair[1]);
        assert(reactor.Add(pair[0], TqReactorEvents::Read, [&](TqSocketHandle, uint32_t events) {
            if ((events & TqReactorEvents::Read) != 0) {
                observed = true;
            }
        }));
        if (i == WSA_MAXIMUM_WAIT_EVENTS + 3) {
            const char byte = 'z';
            assert(TqSend(pair[1], &byte, 1, TqSendFlags::None) == 1);
        }
    }

    assert(WaitUntil(reactor, observed, 1000));
    reactor.Stop();
    for (TqSocketHandle socket : sockets) {
        TqCloseSocket(socket);
    }
}

} // namespace
#endif

int main() {
#if defined(_WIN32)
    TqSocketStartup startup;
    assert(startup.Ok());
    TestWake();
    TestReadReadiness();
    TestRemoveSuppressesDispatch();
    TestMoreThanMaximumWaitEvents();
#endif
    return 0;
}
```

- [ ] **Step 2: Add CMake target and verify failure**

Modify `src/CMakeLists.txt` inside the `if(WIN32)` test target section:

```cmake
add_executable(tcpquic_windows_reactor_test
    unittest/windows_reactor_test.cpp
    runtime/windows_reactor.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_windows_reactor_test)
target_link_libraries(tcpquic_windows_reactor_test PRIVATE Threads::Threads ws2_32)
set_property(TARGET tcpquic_windows_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_windows_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run on Windows build:

```powershell
cmake --build build-win-verify --target tcpquic_windows_reactor_test --config Debug
```

Expected: FAIL because `windows_reactor.h/.cpp` do not exist yet.

- [ ] **Step 3: Create Windows reactor header**

Create `src/runtime/windows_reactor.h`:

```cpp
#pragma once

#include "socket_reactor.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include <unordered_map>
#include <vector>

class TqWindowsReactor final : public ITqSocketReactor {
public:
    TqWindowsReactor() = default;
    ~TqWindowsReactor() override;

    TqWindowsReactor(const TqWindowsReactor&) = delete;
    TqWindowsReactor& operator=(const TqWindowsReactor&) = delete;

    bool Start() override;
    void Stop() override;
    bool Add(TqSocketHandle fd, uint32_t events, Handler handler) override;
    bool Modify(TqSocketHandle fd, uint32_t events) override;
    bool Remove(TqSocketHandle fd) override;
    bool Wake() override;
    bool RunOnce(int timeoutMs) override;

private:
#if defined(_WIN32)
    struct Entry {
        WSAEVENT Event{WSA_INVALID_EVENT};
        long NetworkEvents{0};
        Handler Callback;
    };

    bool RegisterEvents(TqSocketHandle fd, Entry& entry, uint32_t events);
    bool DispatchSocket(TqSocketHandle fd, Entry& entry);

    WSAEVENT WakeEvent_{WSA_INVALID_EVENT};
    std::unordered_map<TqSocketHandle, Entry> Entries_;
#endif
};
```

- [ ] **Step 4: Implement Windows reactor**

Create `src/runtime/windows_reactor.cpp`:

```cpp
#include "windows_reactor.h"

#if defined(_WIN32)

#include <algorithm>

namespace {

long ToNetworkEvents(uint32_t events) {
    long result = FD_CLOSE;
    if ((events & TqReactorEvents::Read) != 0) {
        result |= FD_READ | FD_ACCEPT;
    }
    if ((events & TqReactorEvents::Write) != 0) {
        result |= FD_WRITE | FD_CONNECT;
    }
    if ((events & TqReactorEvents::Error) != 0) {
        result |= FD_CLOSE;
    }
    return result;
}

uint32_t FromNetworkEvents(const WSANETWORKEVENTS& events) {
    uint32_t result = 0;
    if ((events.lNetworkEvents & (FD_READ | FD_ACCEPT)) != 0) {
        result |= TqReactorEvents::Read;
    }
    if ((events.lNetworkEvents & (FD_WRITE | FD_CONNECT)) != 0) {
        result |= TqReactorEvents::Write;
    }
    if ((events.lNetworkEvents & FD_CLOSE) != 0) {
        result |= TqReactorEvents::Error;
    }

    for (int i = 0; i < FD_MAX_EVENTS; ++i) {
        if (events.iErrorCode[i] != 0) {
            result |= TqReactorEvents::Error;
        }
    }
    return result;
}

bool ValidEvents(uint32_t events) {
    constexpr uint32_t kValid = TqReactorEvents::Read | TqReactorEvents::Write | TqReactorEvents::Error;
    return events != 0 && (events & ~kValid) == 0;
}

} // namespace

TqWindowsReactor::~TqWindowsReactor() {
    Stop();
}

bool TqWindowsReactor::Start() {
    if (WakeEvent_ != WSA_INVALID_EVENT) {
        return true;
    }
    WakeEvent_ = WSACreateEvent();
    return WakeEvent_ != WSA_INVALID_EVENT;
}

void TqWindowsReactor::Stop() {
    for (auto& entry : Entries_) {
        (void)WSAEventSelect(entry.first, nullptr, 0);
        if (entry.second.Event != WSA_INVALID_EVENT) {
            WSACloseEvent(entry.second.Event);
        }
    }
    Entries_.clear();
    if (WakeEvent_ != WSA_INVALID_EVENT) {
        WSACloseEvent(WakeEvent_);
        WakeEvent_ = WSA_INVALID_EVENT;
    }
}

bool TqWindowsReactor::RegisterEvents(TqSocketHandle fd, Entry& entry, uint32_t events) {
    entry.NetworkEvents = ToNetworkEvents(events);
    return WSAEventSelect(fd, entry.Event, entry.NetworkEvents) == 0;
}

bool TqWindowsReactor::Add(TqSocketHandle fd, uint32_t events, Handler handler) {
    if (WakeEvent_ == WSA_INVALID_EVENT || !TqSocketValid(fd) || !ValidEvents(events) || !handler) {
        return false;
    }
    if (Entries_.find(fd) != Entries_.end()) {
        return false;
    }

    Entry entry{};
    entry.Event = WSACreateEvent();
    if (entry.Event == WSA_INVALID_EVENT) {
        return false;
    }
    entry.Callback = std::move(handler);
    if (!RegisterEvents(fd, entry, events)) {
        WSACloseEvent(entry.Event);
        return false;
    }
    Entries_.emplace(fd, std::move(entry));
    return true;
}

bool TqWindowsReactor::Modify(TqSocketHandle fd, uint32_t events) {
    auto it = Entries_.find(fd);
    if (WakeEvent_ == WSA_INVALID_EVENT || it == Entries_.end() || !ValidEvents(events)) {
        return false;
    }
    return RegisterEvents(fd, it->second, events);
}

bool TqWindowsReactor::Remove(TqSocketHandle fd) {
    auto it = Entries_.find(fd);
    if (it == Entries_.end()) {
        return false;
    }
    (void)WSAEventSelect(fd, nullptr, 0);
    if (it->second.Event != WSA_INVALID_EVENT) {
        WSACloseEvent(it->second.Event);
    }
    Entries_.erase(it);
    return true;
}

bool TqWindowsReactor::Wake() {
    return WakeEvent_ != WSA_INVALID_EVENT && WSASetEvent(WakeEvent_) == TRUE;
}

bool TqWindowsReactor::DispatchSocket(TqSocketHandle fd, Entry& entry) {
    WSANETWORKEVENTS events{};
    if (WSAEnumNetworkEvents(fd, entry.Event, &events) != 0) {
        entry.Callback(fd, TqReactorEvents::Error);
        return true;
    }
    const uint32_t reactorEvents = FromNetworkEvents(events);
    if (reactorEvents == 0) {
        return false;
    }
    Handler callback = entry.Callback;
    callback(fd, reactorEvents);
    return true;
}

bool TqWindowsReactor::RunOnce(int timeoutMs) {
    if (WakeEvent_ == WSA_INVALID_EVENT) {
        return false;
    }

    bool didWork = false;
    std::vector<TqSocketHandle> keys;
    keys.reserve(Entries_.size());
    for (const auto& entry : Entries_) {
        keys.push_back(entry.first);
    }

    size_t offset = 0;
    const DWORD timeout = timeoutMs < 0 ? WSA_INFINITE : static_cast<DWORD>(timeoutMs);
    do {
        WSAEVENT events[WSA_MAXIMUM_WAIT_EVENTS]{};
        TqSocketHandle sockets[WSA_MAXIMUM_WAIT_EVENTS]{};
        DWORD count = 0;
        events[count++] = WakeEvent_;

        for (; offset < keys.size() && count < WSA_MAXIMUM_WAIT_EVENTS; ++offset) {
            auto it = Entries_.find(keys[offset]);
            if (it == Entries_.end()) {
                continue;
            }
            sockets[count] = it->first;
            events[count] = it->second.Event;
            ++count;
        }

        const DWORD waitResult = WSAWaitForMultipleEvents(count, events, FALSE, timeout, FALSE);
        if (waitResult == WSA_WAIT_TIMEOUT) {
            return didWork;
        }
        if (waitResult == WSA_WAIT_FAILED) {
            return didWork;
        }
        const DWORD index = waitResult - WSA_WAIT_EVENT_0;
        if (index >= count) {
            return didWork;
        }
        if (index == 0) {
            WSAResetEvent(WakeEvent_);
            didWork = true;
            continue;
        }

        auto it = Entries_.find(sockets[index]);
        if (it != Entries_.end()) {
            didWork = DispatchSocket(it->first, it->second) || didWork;
        }
    } while (offset < keys.size());

    return didWork;
}

#else

TqWindowsReactor::~TqWindowsReactor() = default;
bool TqWindowsReactor::Start() { return false; }
void TqWindowsReactor::Stop() {}
bool TqWindowsReactor::Add(TqSocketHandle, uint32_t, Handler) { return false; }
bool TqWindowsReactor::Modify(TqSocketHandle, uint32_t) { return false; }
bool TqWindowsReactor::Remove(TqSocketHandle) { return false; }
bool TqWindowsReactor::Wake() { return false; }
bool TqWindowsReactor::RunOnce(int) { return false; }

#endif
```

- [ ] **Step 5: Run Windows reactor test**

Run:

```powershell
cmake --build build-win-verify --target tcpquic_windows_reactor_test --config Debug
.\build-win-verify\Debug\tcpquic_windows_reactor_test.exe
```

Expected: executable exits `0`.

- [ ] **Step 6: Commit**

```bash
git add src/runtime/windows_reactor.h src/runtime/windows_reactor.cpp src/unittest/windows_reactor_test.cpp src/CMakeLists.txt
git commit -m "feat: add Windows socket reactor"
```

---

### Task 3: Extract Cross-Platform Listen Socket Helpers

**Files:**
- Create: `src/runtime/scoped_socket.h`
- Create: `src/runtime/listen_socket.h`
- Create: `src/runtime/listen_socket.cpp`
- Modify: `src/ingress/client_ingress_reactor.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/client_ingress_reactor_test.cpp`

- [ ] **Step 1: Create scoped socket wrapper**

Create `src/runtime/scoped_socket.h`:

```cpp
#pragma once

#include "platform_socket.h"

class TqScopedSocket {
public:
    explicit TqScopedSocket(TqSocketHandle socket = TqInvalidSocket) : Socket_(socket) {}
    ~TqScopedSocket() { Reset(); }

    TqScopedSocket(const TqScopedSocket&) = delete;
    TqScopedSocket& operator=(const TqScopedSocket&) = delete;

    TqSocketHandle Get() const { return Socket_; }

    TqSocketHandle Release() {
        TqSocketHandle socket = Socket_;
        Socket_ = TqInvalidSocket;
        return socket;
    }

    void Reset(TqSocketHandle socket = TqInvalidSocket) {
        if (TqSocketValid(Socket_)) {
            TqCloseSocket(Socket_);
        }
        Socket_ = socket;
    }

private:
    TqSocketHandle Socket_{TqInvalidSocket};
};
```

- [ ] **Step 2: Create listen socket API**

Create `src/runtime/listen_socket.h`:

```cpp
#pragma once

#include "platform_socket.h"

#include <string>

struct TqListenSocket {
    TqSocketHandle Fd{TqInvalidSocket};
    std::string Address;
};

bool TqCreateNonBlockingListenSocket(const std::string& listen, TqListenSocket& out);
```

Create `src/runtime/listen_socket.cpp` by moving the existing helper logic from `client_ingress_reactor.cpp` and replacing Linux-only pieces with platform calls:

```cpp
#include "listen_socket.h"

#include "scoped_socket.h"

#include <limits>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace {

struct TqParsedHostPort {
    std::string Host;
    uint16_t Port{};
};

bool TqParsePortAllowZero(const std::string& text, uint16_t& port) {
    if (text.empty()) {
        return false;
    }
    unsigned long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = (value * 10) + static_cast<unsigned long>(ch - '0');
        if (value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool TqParseHostPortAllowZero(const std::string& target, TqParsedHostPort& out) {
    if (target.empty() || target.find("://") != std::string::npos) {
        return false;
    }
    std::string host;
    std::string portText;
    if (target.front() == '[') {
        const size_t close = target.find(']');
        if (close == std::string::npos || close + 2 > target.size() || target[close + 1] != ':') {
            return false;
        }
        host = target.substr(1, close - 1);
        portText = target.substr(close + 2);
    } else {
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos || colon + 1 >= target.size()) {
            return false;
        }
        host = target.substr(0, colon);
        portText = target.substr(colon + 1);
        if (host.find(':') != std::string::npos) {
            return false;
        }
    }
    uint16_t port = 0;
    if (!TqParsePortAllowZero(portText, port)) {
        return false;
    }
    out.Host = std::move(host);
    out.Port = port;
    return true;
}

std::string TqBoundAddressString(TqSocketHandle fd, const std::string& requestedHost) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return {};
    }

    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (TqInetNtop(AF_INET, &addr->sin_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(addr->sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }
    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (TqInetNtop(AF_INET6, &addr->sin6_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(addr->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    if (!requestedHost.empty()) {
        return requestedHost + ":0";
    }
    return {};
}

} // namespace

bool TqCreateNonBlockingListenSocket(const std::string& listen, TqListenSocket& out) {
    TqParsedHostPort hostPort{};
    if (!TqParseHostPortAllowZero(listen, hostPort)) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(hostPort.Port);
    const int status = getaddrinfo(
        hostPort.Host.empty() ? nullptr : hostPort.Host.c_str(),
        port.c_str(),
        &hints,
        &result);
    if (status != 0) {
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        TqScopedSocket fd(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!TqSocketValid(fd.Get())) {
            continue;
        }
        if (!TqSetNonBlocking(fd.Get())) {
            continue;
        }
        (void)TqSetReuseAddr(fd.Get());
        if (bind(fd.Get(), ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            listen(fd.Get(), SOMAXCONN) == 0) {
            std::string address = TqBoundAddressString(fd.Get(), hostPort.Host);
            if (address.empty()) {
                continue;
            }
            out.Fd = fd.Release();
            out.Address = std::move(address);
            freeaddrinfo(result);
            return true;
        }
    }

    freeaddrinfo(result);
    return false;
}
```

- [ ] **Step 3: Remove duplicated helpers from client ingress implementation**

Modify `src/ingress/client_ingress_reactor.cpp`:

- Add includes:

```cpp
#include "listen_socket.h"
#include "scoped_socket.h"
```

- Delete the local `TqParsedHostPort`, `TqListenSocket`, `TqScopedFd`, `TqSetNonBlockingCloseOnExec`, `TqSetReuseAddress`, `TqBoundAddressString`, and `TqCreateNonBlockingListenSocket` definitions.
- Replace local `TqScopedFd` use with `TqScopedSocket` if any remains.

- [ ] **Step 4: Update CMake target sources**

For every target that builds `ingress/client_ingress_reactor.cpp`, add `runtime/listen_socket.cpp`.

At minimum update `tcpquic_client_ingress_reactor_test`:

```cmake
add_executable(tcpquic_client_ingress_reactor_test
    unittest/client_ingress_reactor_test.cpp
    unittest/trace_proxy_stub.cpp
    ingress/client_ingress_reactor.cpp
    ingress/client_ingress_state.cpp
    ingress/socks5_server.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    protocol/control_protocol.cpp
    runtime/listen_socket.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
    runtime/linux_reactor.cpp
)
```

- [ ] **Step 5: Run ingress reactor test on Linux**

Run:

```bash
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
./build/src/tcpquic_client_ingress_reactor_test
```

Expected: executable exits `0`.

- [ ] **Step 6: Commit**

```bash
git add src/runtime/scoped_socket.h src/runtime/listen_socket.h src/runtime/listen_socket.cpp src/ingress/client_ingress_reactor.cpp src/CMakeLists.txt
git commit -m "refactor: share nonblocking listen socket helper"
```

---

### Task 4: Make Client Ingress Reactor Platform-Neutral

**Files:**
- Modify: `src/ingress/client_ingress_reactor.h`
- Modify: `src/ingress/client_ingress_reactor.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/client_ingress_reactor_test.cpp`

- [ ] **Step 1: Replace Linux reactor include with platform reactor selection**

Modify `src/ingress/client_ingress_reactor.h`:

```cpp
#include "socket_reactor.h"
#if defined(_WIN32)
#include "windows_reactor.h"
#else
#include "linux_reactor.h"
#endif
```

Add a private alias before the class or inside private section:

```cpp
#if defined(_WIN32)
using TqPlatformReactor = TqWindowsReactor;
#else
using TqPlatformReactor = TqLinuxReactor;
#endif
```

Replace:

```cpp
TqLinuxReactor Reactor;
```

with:

```cpp
TqPlatformReactor Reactor;
```

- [ ] **Step 2: Replace event constants in implementation**

In `src/ingress/client_ingress_reactor.cpp`, replace all `TqLinuxReactorEvents::Read`, `Write`, `Error` with `TqReactorEvents::Read`, `Write`, `Error`.

- [ ] **Step 3: Replace raw int fd maps with TqSocketHandle where needed**

In `src/ingress/client_ingress_reactor.h`, change fields that store socket handles:

```cpp
TqSocketHandle SocksFd{TqInvalidSocket};
TqSocketHandle HttpFd{TqInvalidSocket};
std::unordered_map<TqSocketHandle, ListenEntry> Listens;
std::unordered_map<TqSocketHandle, ClientEntry> Clients;
```

Change member function signatures:

```cpp
void AcceptLoop(TqSocketHandle listenFd);
void HandleClientEvents(TqSocketHandle clientFd, uint32_t events);
void HandleClientRead(TqSocketHandle clientFd);
void HandleClientWrite(TqSocketHandle clientFd);
void HandleIngressResult(TqSocketHandle clientFd, TqClientIngressResult result);
void StartClientOpen(TqSocketHandle clientFd);
void CompleteClientOpen(TqSocketHandle clientFd, TqClientTunnelOpenHandle* handle, TqTunnelStartResult result);
void CloseClientLocked(TqSocketHandle clientFd, bool closeFd);
void CloseClientOwnedByTunnelLocked(TqSocketHandle clientFd);
```

Update corresponding definitions in `.cpp`.

- [ ] **Step 4: Add Windows source to test target**

In `src/CMakeLists.txt`, change `tcpquic_client_ingress_reactor_test` platform reactor source:

```cmake
if(WIN32)
    target_sources(tcpquic_client_ingress_reactor_test PRIVATE runtime/windows_reactor.cpp)
    target_link_libraries(tcpquic_client_ingress_reactor_test PRIVATE ws2_32)
else()
    target_sources(tcpquic_client_ingress_reactor_test PRIVATE runtime/linux_reactor.cpp)
endif()
```

- [ ] **Step 5: Run Linux and Windows ingress tests**

Linux:

```bash
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
./build/src/tcpquic_client_ingress_reactor_test
```

Windows:

```powershell
cmake --build build-win-verify --target tcpquic_client_ingress_reactor_test --config Debug
.\build-win-verify\Debug\tcpquic_client_ingress_reactor_test.exe
```

Expected: both executables exit `0`.

- [ ] **Step 6: Commit**

```bash
git add src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp src/CMakeLists.txt
git commit -m "feat: enable platform-neutral client ingress reactor"
```

---

### Task 5: Move Windows Client Runtime To Async Ingress

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `tcpquic_client_ingress_reactor_test`, Windows app build

- [ ] **Step 1: Include async ingress headers for Windows too**

Modify `src/main.cpp` top-level includes. Replace:

```cpp
#if defined(__linux__)
#include "client_ingress_reactor.h"
#include "client_tunnel_open.h"
#include "server_dial_reactor.h"
#endif
```

with:

```cpp
#include "client_ingress_reactor.h"
#include "client_tunnel_open.h"
#include "server_dial_reactor.h"
```

- [ ] **Step 2: Remove non-Linux proxy auth helper guard**

`TqMakeProxyAuthTable()` is only needed by legacy listener path. Keep it available during fallback but do not require it for Windows async ingress. If no legacy path remains after this task, delete the helper.

- [ ] **Step 3: Unify multi-peer runtime fields**

In `TqMultiPeerRuntimeAdapter::PeerRuntime`, replace the platform split:

```cpp
#if defined(__linux__)
TqClientIngressReactor* Ingress{nullptr};
TqClientIngressTunnelStartFn StartTunnel;
#else
std::unique_ptr<TqThreadPool> Pool;
std::unique_ptr<TqSocks5Server> Socks;
std::unique_ptr<TqHttpConnectServer> Http;
TunnelStartFn StartTunnel;
#endif
```

with:

```cpp
TqClientIngressReactor* Ingress{nullptr};
TqClientIngressTunnelStartFn StartTunnel;
```

- [ ] **Step 4: Unify StartTunnel async lambda**

Use the existing Linux lambda for all platforms:

```cpp
runtime->StartTunnel = [weakRuntime, peerCfg](
                           const TunnelRequest& req,
                           TqSocketHandle fd,
                           TqClientTunnelOpenComplete onComplete) {
    auto runtime = weakRuntime.lock();
    if (!runtime || !runtime->Quic) {
        return static_cast<TqClientTunnelOpenHandle*>(nullptr);
    }
    MsQuicConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> guard(runtime->TunnelStartMutex);
        if (!runtime->Quic || !runtime->Quic->EnsureAnyConnected()) {
            std::fprintf(stderr,
                "tcpquic-proxy: peer %s has no connected QUIC connection for tunnel\n",
                peerCfg.QuicPeer.c_str());
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        conn = runtime->Quic->PickConnection();
        if (conn == nullptr) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
    }
    return TqStartClientTunnelAsync(conn, req, fd, peerCfg, std::move(onComplete));
};
```

- [ ] **Step 5: Unify listener open/close logic**

In `PeerRuntime::OpenListenersLocked()`, delete the non-Linux `TqSocks5Server` / `TqHttpConnectServer` branch and use the existing ingress `AddPeer()` path for all platforms.

In `CloseListenersLocked()`, use only:

```cpp
if (ListenersOpen && Ingress != nullptr) {
    (void)Ingress->RemovePeer(PeerId);
}
ListenersOpen = false;
```

In `StopAll()`, remove pool stop logic.

- [ ] **Step 6: Unify global ingress owner**

In `TqMultiPeerRuntimeAdapter`, remove `#if defined(__linux__)` around:

```cpp
std::mutex IngressLock;
std::unique_ptr<TqClientIngressReactor> Ingress;
bool EnsureIngressStarted(std::string& err);
```

- [ ] **Step 7: Unify single-peer client runtime**

In `TqSinglePeerClientRuntime`, remove non-Linux pool/listener branches and use the Linux async ingress flow for all platforms:

- constructor no longer initializes `Pool`.
- destructor stops `Ingress`.
- `Start()` starts `Ingress`.
- `SetStartTunnel()` takes `TqClientIngressTunnelStartFn`.
- `OpenListenersLocked()` uses `Ingress.AddPeer()`.
- `CloseListenersLocked()` uses `Ingress.RemovePeer()`.

- [ ] **Step 8: Update app target sources**

Ensure main executable links these sources on Windows:

```cmake
ingress/client_ingress_reactor.cpp
ingress/client_ingress_state.cpp
runtime/listen_socket.cpp
runtime/windows_reactor.cpp
tunnel/client_tunnel_open.cpp
```

Keep `socks5_server.cpp` and `http_connect_server.cpp` if parser helpers are still used by `client_ingress_state` or response mapping.

- [ ] **Step 9: Build Windows app and ingress test**

Run:

```powershell
cmake --build build-win-verify --target tcpquic_client_ingress_reactor_test --config Debug
cmake --build build-win-verify --target tcpquic --config Debug
```

Expected: both builds succeed.

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp src/CMakeLists.txt
git commit -m "feat: use async ingress reactor on Windows"
```

---

### Task 6: Make c-ares Resolver Platform-Neutral

**Files:**
- Modify: `src/tunnel/ares_dns_resolver.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/ares_dns_resolver_test.cpp`

- [ ] **Step 1: Replace Linux reactor include**

Modify `src/tunnel/ares_dns_resolver.cpp`:

```cpp
#if defined(_WIN32)
#include "windows_reactor.h"
#else
#include "linux_reactor.h"
#endif
```

Add alias:

```cpp
#if defined(_WIN32)
using TqPlatformReactor = TqWindowsReactor;
#else
using TqPlatformReactor = TqLinuxReactor;
#endif
```

Replace:

```cpp
TqLinuxReactor Reactor;
```

with:

```cpp
TqPlatformReactor Reactor;
```

- [ ] **Step 2: Replace event constants**

Replace all `TqLinuxReactorEvents::Read`, `Write`, `Error` with `TqReactorEvents::Read`, `Write`, `Error`.

- [ ] **Step 3: Use TqSocketHandle for c-ares sockets**

Change `RegisteredSockets` type:

```cpp
std::unordered_set<TqSocketHandle> RegisteredSockets;
```

In `UpdateSocket()`:

```cpp
const TqSocketHandle fd = static_cast<TqSocketHandle>(socketFd);
```

In `ProcessFd()`:

```cpp
void ProcessFd(TqSocketHandle fd, uint32_t events) {
    ares_fd_events_t fdEvent{};
    fdEvent.fd = static_cast<ares_socket_t>(fd);
    ...
}
```

- [ ] **Step 4: Update Windows CMake target for resolver test**

Move `tcpquic_ares_dns_resolver_test` out of Linux-only block or create a Windows equivalent. Its source list should choose reactor implementation by platform:

```cmake
add_executable(tcpquic_ares_dns_resolver_test
    unittest/ares_dns_resolver_test.cpp
    tunnel/ares_dns_resolver.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(WIN32)
    target_sources(tcpquic_ares_dns_resolver_test PRIVATE runtime/windows_reactor.cpp)
    target_link_libraries(tcpquic_ares_dns_resolver_test PRIVATE ws2_32)
else()
    target_sources(tcpquic_ares_dns_resolver_test PRIVATE runtime/linux_reactor.cpp)
endif()
tcpquic_target_include_dirs(tcpquic_ares_dns_resolver_test)
target_link_libraries(tcpquic_ares_dns_resolver_test PRIVATE Threads::Threads ${TCPQUIC_CARES_TARGET})
target_compile_definitions(tcpquic_ares_dns_resolver_test PRIVATE TQ_UNIT_TESTING=1)
```

- [ ] **Step 5: Run resolver tests on both platforms**

Linux:

```bash
cmake --build build --target tcpquic_ares_dns_resolver_test -j2
./build/src/tcpquic_ares_dns_resolver_test
```

Windows:

```powershell
cmake --build build-win-verify --target tcpquic_ares_dns_resolver_test --config Debug
.\build-win-verify\Debug\tcpquic_ares_dns_resolver_test.exe
```

Expected: both executables exit `0`.

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/ares_dns_resolver.cpp src/CMakeLists.txt
git commit -m "feat: enable c-ares resolver on Windows reactor"
```

---

### Task 7: Make Server Dial Reactor Platform-Neutral

**Files:**
- Modify: `src/tunnel/server_dial_reactor.cpp`
- Modify: `src/tunnel/server_dial_reactor.h`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/server_dial_reactor_test.cpp`

- [ ] **Step 1: Replace Linux reactor include**

Modify `src/tunnel/server_dial_reactor.cpp`:

```cpp
#if defined(_WIN32)
#include "windows_reactor.h"
#else
#include "linux_reactor.h"
#endif
```

Add alias:

```cpp
#if defined(_WIN32)
using TqPlatformReactor = TqWindowsReactor;
#else
using TqPlatformReactor = TqLinuxReactor;
#endif
```

Replace:

```cpp
TqLinuxReactor Reactor;
```

with:

```cpp
TqPlatformReactor Reactor;
```

- [ ] **Step 2: Remove Unix socket include assumptions**

Guard Unix-only includes:

```cpp
#if !defined(_WIN32)
#include <sys/socket.h>
#endif
```

Use `platform_socket.h` APIs for close, nonblocking, and socket validity.

- [ ] **Step 3: Add platform error helpers**

Replace `IsRefusedError()` and `IsTimeoutLikeError()` with platform-aware versions:

```cpp
bool IsRefusedError(int error) {
#if defined(_WIN32)
    return error == WSAECONNREFUSED;
#else
    return error == ECONNREFUSED;
#endif
}

bool IsTimeoutLikeError(int error) {
#if defined(_WIN32)
    return error == WSAETIMEDOUT || error == WSAEHOSTUNREACH ||
        error == WSAENETUNREACH || error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == ETIMEDOUT || error == EHOSTUNREACH || error == ENETUNREACH ||
        error == EAGAIN || error == EWOULDBLOCK;
#endif
}
```

- [ ] **Step 4: Normalize nonblocking connect pending states**

Where connect result is evaluated, treat these as pending:

```cpp
bool IsConnectPendingError(int error) {
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
#else
    return error == EINPROGRESS || error == EALREADY || error == EWOULDBLOCK;
#endif
}
```

Use this helper when `Connect()` returns non-zero.

- [ ] **Step 5: Use platform reactor event constants**

Replace all `TqLinuxReactorEvents::Write`, `Read`, `Error` with `TqReactorEvents::Write`, `Read`, `Error`.

- [ ] **Step 6: Update CMake server dial tests**

Move `tcpquic_server_dial_reactor_test` and `tcpquic_server_dial_reactor_worker_test` out of Linux-only block or duplicate platform-aware targets.

For Windows, include:

```cmake
runtime/windows_reactor.cpp
```

For Linux, include:

```cmake
runtime/linux_reactor.cpp
```

Both platforms link `${TCPQUIC_CARES_TARGET}` and `${TCPQUIC_PLATFORM_SOURCES}`.

- [ ] **Step 7: Run server dial tests**

Linux:

```bash
cmake --build build --target tcpquic_server_dial_reactor_test tcpquic_server_dial_reactor_worker_test -j2
./build/src/tcpquic_server_dial_reactor_test
./build/src/tcpquic_server_dial_reactor_worker_test
```

Windows:

```powershell
cmake --build build-win-verify --target tcpquic_server_dial_reactor_test --config Debug
.\build-win-verify\Debug\tcpquic_server_dial_reactor_test.exe
```

Expected: all available executables exit `0`.

- [ ] **Step 8: Commit**

```bash
git add src/tunnel/server_dial_reactor.cpp src/tunnel/server_dial_reactor.h src/CMakeLists.txt
git commit -m "feat: enable server dial reactor on Windows"
```

---

### Task 8: Route Windows Server OPEN Through Dial Reactor

**Files:**
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/tunnel/tcp_tunnel.h`
- Modify: `src/main.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/speed_test_test.cpp`

- [ ] **Step 1: Include server dial reactor for Windows**

Modify `src/tunnel/tcp_tunnel.cpp`, replacing:

```cpp
#if defined(__linux__)
#include "server_dial_reactor.h"
#endif
```

with:

```cpp
#include "server_dial_reactor.h"
```

- [ ] **Step 2: Allow global reactor on Windows**

Replace `TqGetServerDialReactor()` implementation:

```cpp
TqServerDialReactor* TqGetServerDialReactor() {
    return g_serverDialReactor.load(std::memory_order_acquire);
}
```

Keep fallback behavior in `TryHandleServerOpen()` when this returns `nullptr`.

- [ ] **Step 3: Ensure context members are not Linux-only**

In `TqTunnelContext`, any fields needed by `ServerOpenFinished()` for dial reactor lifecycle must be declared for Windows too:

```cpp
uint64_t ServerDialToken{0};
TqServerDialReactor* ServerDialReactor{nullptr};
bool ServerDialCallbackActive{false};
bool ServerDialPendingWithReactor{false};
```

Do not regress prior Windows build decision: only broaden declarations if all uses are now platform-neutral and server dial reactor is compiled on Windows.

- [ ] **Step 4: Initialize Windows server dial reactor in runtime**

In `src/main.cpp`, server runtime startup should create a `TqServerDialReactor` for Windows and Linux:

```cpp
auto dialReactor = std::make_unique<TqServerDialReactor>(cfg.Acl);
if (!dialReactor->Start()) {
    std::fprintf(stderr, "tcpquic-proxy: failed to start server dial reactor\n");
    return 1;
}
TqSetServerDialReactor(dialReactor.get());
```

On shutdown:

```cpp
TqSetServerDialReactor(nullptr);
dialReactor->Stop();
dialReactor.reset();
```

Place this next to the existing Linux server reactor setup if one exists; avoid constructing two reactors.

- [ ] **Step 5: Update Windows app/test target sources**

Any Windows target that includes `tcp_tunnel.cpp` now needs:

```cmake
tunnel/server_dial_reactor.cpp
tunnel/ares_dns_resolver.cpp
acl/acl_filter.cpp
runtime/windows_reactor.cpp
```

and link `${TCPQUIC_CARES_TARGET}`.

- [ ] **Step 6: Run tunnel and speed tests**

Linux:

```bash
cmake --build build --target tcpquic_tunnel_test tcpquic_speed_test_test -j2
./build/src/tcpquic_tunnel_test
./build/src/tcpquic_speed_test_test
```

Windows:

```powershell
cmake --build build-win-verify --target tcpquic_tunnel_test --config Debug
cmake --build build-win-verify --target tcpquic_speed_test_test --config Debug
.\build-win-verify\Debug\tcpquic_tunnel_test.exe
.\build-win-verify\Debug\tcpquic_speed_test_test.exe
```

Expected: all available executables exit `0`.

- [ ] **Step 7: Commit**

```bash
git add src/tunnel/tcp_tunnel.cpp src/tunnel/tcp_tunnel.h src/main.cpp src/CMakeLists.txt
git commit -m "feat: route Windows server open through dial reactor"
```

---

### Task 9: Update Thread Model Documentation

**Files:**
- Modify: `docs/thread-model_cn.md`
- Modify: `docs/superpowers/specs/2026-06-19-windows-async-reactor-thread-model-design.md`

- [x] **Step 1: Update thread model overview**

In `docs/thread-model_cn.md`, update Windows sections to state:

```markdown
Windows client 控制面现在与 Linux 一致：所有 peer 的 SOCKS5 / HTTP CONNECT listen、accept、握手状态机由单一 `TqClientIngressReactor` 线程处理。该 reactor 底层使用 `TqWindowsReactor`，通过 Winsock event 驱动控制面 socket。

Windows server 普通 OPEN 的 DNS 解析与 TCP connect 由单一 `TqServerDialReactor` 线程处理。DNS 使用 vendored c-ares，由 `TqWindowsReactor` 驱动 c-ares sockets 和 timeout。每条 OPEN 不再创建 detached dial thread。

Windows relay 数据面仍由 `TqWindowsRelayWorker` worker 组处理，使用 IOCP 和 overlapped I/O；控制面 reactor 不接管 relay 数据面。
```

- [x] **Step 2: Update design implementation status**

In `docs/superpowers/specs/2026-06-19-windows-async-reactor-thread-model-design.md`, change status from `待实现` to `已实现` only after Task 8 verification has passed. Add a short implementation status section listing completed phases.

- [x] **Step 3: Commit docs**

```bash
git add docs/thread-model_cn.md docs/superpowers/specs/2026-06-19-windows-async-reactor-thread-model-design.md
git commit -m "docs: update Windows async thread model"
```

---

### Task 10: Full Verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Build core Linux targets**

Run:

```bash
cmake --build build --target tcpquic_linux_reactor_test tcpquic_client_ingress_reactor_test tcpquic_ares_dns_resolver_test tcpquic_server_dial_reactor_test tcpquic_server_dial_reactor_worker_test tcpquic_tunnel_test tcpquic_speed_test_test -j2
```

Expected: build succeeds.

- [ ] **Step 2: Run Linux tests**

Run:

```bash
./build/src/tcpquic_linux_reactor_test
./build/src/tcpquic_client_ingress_reactor_test
./build/src/tcpquic_ares_dns_resolver_test
./build/src/tcpquic_server_dial_reactor_test
./build/src/tcpquic_server_dial_reactor_worker_test
./build/src/tcpquic_tunnel_test
./build/src/tcpquic_speed_test_test
```

Expected: every executable exits `0`.

- [ ] **Step 3: Build core Windows targets**

Run:

```powershell
cmake --build build-win-verify --target tcpquic_windows_reactor_test --config Debug
cmake --build build-win-verify --target tcpquic_client_ingress_reactor_test --config Debug
cmake --build build-win-verify --target tcpquic_ares_dns_resolver_test --config Debug
cmake --build build-win-verify --target tcpquic_server_dial_reactor_test --config Debug
cmake --build build-win-verify --target tcpquic_tunnel_test --config Debug
cmake --build build-win-verify --target tcpquic_speed_test_test --config Debug
cmake --build build-win-verify --target tcpquic_windows_relay_worker_test --config Debug
cmake --build build-win-verify --target tcpquic --config Debug
```

Expected: every build succeeds.

- [ ] **Step 4: Run Windows tests**

Run:

```powershell
.\build-win-verify\Debug\tcpquic_windows_reactor_test.exe
.\build-win-verify\Debug\tcpquic_client_ingress_reactor_test.exe
.\build-win-verify\Debug\tcpquic_ares_dns_resolver_test.exe
.\build-win-verify\Debug\tcpquic_server_dial_reactor_test.exe
.\build-win-verify\Debug\tcpquic_tunnel_test.exe
.\build-win-verify\Debug\tcpquic_speed_test_test.exe
.\build-win-verify\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: every executable exits `0`.

- [ ] **Step 5: Confirm no legacy Windows control threads remain in runtime path**

Search source references manually or via code search and confirm:

- Windows `TqMultiPeerRuntimeAdapter::PeerRuntime` no longer owns `TqThreadPool`, `TqSocks5Server`, or `TqHttpConnectServer` for normal runtime.
- Windows `TqSinglePeerClientRuntime` no longer owns handshake `TqThreadPool` for normal runtime.
- Windows server startup sets `TqServerDialReactor` before accepting streams.
- `TqWindowsRelayWorker` remains unchanged as relay data plane.

- [ ] **Step 6: Final commit if verification fixes were needed**

If Task 10 required fixes, commit them:

```bash
git add <fixed-files>
git commit -m "fix: stabilize Windows async reactor integration"
```

---

## Self-Review

- Spec coverage: Tasks 1-2 cover reactor abstraction and Windows reactor; Tasks 3-5 cover Windows client ingress; Tasks 6-8 cover c-ares and server dial; Task 9 covers docs; Task 10 covers verification.
- No placeholder steps remain: every task lists exact files, code shapes, commands, and expected results.
- Platform boundary: relay worker remains untouched except verification; Linux behavior is preserved through interface adaptation.
- Risk controls: Windows `WSA_MAXIMUM_WAIT_EVENTS`, c-ares callback threading, nonblocking connect errors, and early data boundary each have explicit implementation or test coverage.
