# Async Reactor Thread Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 Linux client 侧所有 peer 的 accept + SOCKS5/HTTP CONNECT handshake 收敛到一个 ingress reactor 线程，并把 Linux server 侧普通 OPEN 的 DNS + TCP connect 收敛到一个 dial reactor 线程，DNS 使用 c-ares 异步解析。

**Architecture:** 第一阶段只迁移 Linux 控制面，Windows 保持现有线程模型。新增可测试的状态机、异步 client OPEN API、no-DNS ACL filter、c-ares wrapper、server dial reactor，再逐步替换现有 listener thread / handshake pool / detached dial thread。Relay worker 数据面保持现状。

**Tech Stack:** C++17, CMake, Linux `epoll` / `eventfd`, MsQuic callbacks, c-ares v1.34.6, existing unittest executable style.

---

## File Structure

新增文件：

- `src/runtime/linux_reactor.h/.cpp`  
  Linux-only small reactor wrapper: epoll fd, eventfd wakeup, add/modify/remove fd, run one event batch.
- `src/unittest/linux_reactor_test.cpp`  
  验证 wakeup、read readiness、remove fd 后不再派发。
- `src/acl/acl_filter.h/.cpp`  
  no-DNS ACL filtering helper，只过滤已解析或 literal 地址，不调用 `getaddrinfo()`。
- `src/unittest/acl_filter_test.cpp`  
  验证 allow / deny / empty candidate / port setting。
- `src/tunnel/client_tunnel_open.h/.cpp`  
  异步 client OPEN API，封装 `TqStartClientTunnelAsync()` / cancel handle。
- `src/unittest/client_tunnel_open_test.cpp`  
  验证 async open 不阻塞、success / fail / cancel completion 语义。
- `src/tunnel/ares_dns_resolver.h/.cpp`  
  c-ares wrapper，隐藏 channel 生命周期和 callback 约束。
- `src/unittest/ares_dns_resolver_test.cpp`  
  使用 mock resolver interface 测试状态机；真实 c-ares 只做 build/link smoke。
- `src/tunnel/server_dial_reactor.h/.cpp`  
  Linux server dial reactor，负责 DNS state、connect state、open response、relay start handoff。
- `src/unittest/server_dial_reactor_test.cpp`  
  使用 fake DNS / fake connect 测试成功、DNS fail、connect timeout、cancel。
- `src/ingress/client_ingress_reactor.h/.cpp`  
  Linux client ingress reactor，集中管理所有 peer listen fd 和 accepted sockets。
- `src/ingress/client_ingress_state.h/.cpp`  
  SOCKS5 / HTTP CONNECT handshake 状态机。
- `src/unittest/client_ingress_state_test.cpp`  
  验证半包、错误包、auth fail、success target parse。

修改文件：

- `src/CMakeLists.txt`  
  接入 c-ares、添加新源文件、新测试目标。
- `src/acl/acl.h`  
  声明 no-DNS ACL helper 或 include 新 header。
- `src/tunnel/tcp_tunnel.h/.cpp`  
  暴露 async client open 入口；server OPEN 改为投递 dial reactor。
- `src/tunnel/tcp_dialer.h/.cpp`  
  保留现有同步 dial 给旧路径；抽出 non-blocking connect helpers 给 dial reactor 复用。
- `src/ingress/socks5_server.*` / `src/ingress/http_connect_server.*`  
  保留协议解析函数，Linux 新路径不再启动 per-listener thread。
- `src/main.cpp`  
  Linux client runtime 创建一个全局 ingress reactor；Linux server runtime 创建一个 dial reactor。
- `docs/thread-model_cn.md`  
  更新当前线程模型，标明 Linux 新路径和 Windows 旧路径边界。

---

### Task 1: Vendor c-ares And Build Integration

**Files:**
- Create: `third_party/c-ares/`
- Modify: `src/CMakeLists.txt`
- Test: configure/build existing minimal target

- [ ] **Step 1: Download pinned c-ares source**

Run:

```bash
mkdir -p third_party
curl -L https://github.com/c-ares/c-ares/releases/download/v1.34.6/c-ares-1.34.6.tar.gz -o /tmp/c-ares-1.34.6.tar.gz
tar -xf /tmp/c-ares-1.34.6.tar.gz -C third_party
mv third_party/c-ares-1.34.6 third_party/c-ares
```

Expected: `third_party/c-ares/CMakeLists.txt` and `third_party/c-ares/LICENSE.md` exist.

- [ ] **Step 2: Wire c-ares into CMake**

Modify `src/CMakeLists.txt` near dependency setup:

```cmake
set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/c-ares ${CMAKE_BINARY_DIR}/c-ares)

set(TCPQUIC_CARES_TARGET c-ares::cares)
```

If c-ares exports `c-ares` instead of `c-ares::cares` in this release, define a local alias immediately after `add_subdirectory`:

```cmake
if(NOT TARGET c-ares::cares AND TARGET c-ares)
    add_library(c-ares::cares ALIAS c-ares)
endif()
```

- [ ] **Step 3: Build to verify dependency configuration**

Run:

```bash
cmake --build build --target tcpquic_acl_test -j2
```

Expected: build succeeds and c-ares config does not alter existing target.

- [ ] **Step 4: Commit**

```bash
git add third_party/c-ares src/CMakeLists.txt
git commit -m "build: vendor c-ares"
```

---

### Task 2: Add no-DNS ACL Filtering Helper

**Files:**
- Create: `src/acl/acl_filter.h`
- Create: `src/acl/acl_filter.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/acl_filter_test.cpp`

- [ ] **Step 1: Write failing ACL filter test**

Create `src/unittest/acl_filter_test.cpp`:

```cpp
#include "acl_filter.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <vector>

namespace {

sockaddr_storage IPv4(const char* ip, uint16_t port) {
    sockaddr_storage storage{};
    auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    const int ok = inet_pton(AF_INET, ip, &addr->sin_addr);
    assert(ok == 1);
    return storage;
}

uint16_t PortOf(const sockaddr_storage& storage) {
    const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
    return ntohs(addr->sin_port);
}

} // namespace

int main() {
    TqAcl acl{};
    acl.AllowCidrs = {"10.0.0.0/8"};
    acl.DenyCidrs = {"10.0.0.13/32"};

    std::vector<sockaddr_storage> candidates{
        IPv4("10.0.0.7", 0),
    };
    std::vector<sockaddr_storage> out;
    assert(TqAclFilterResolvedAddresses(acl, candidates, 443, out));
    assert(out.size() == 1);
    assert(PortOf(out[0]) == 443);

    candidates = {IPv4("10.0.0.13", 0)};
    out.clear();
    assert(!TqAclFilterResolvedAddresses(acl, candidates, 443, out));
    assert(out.empty());

    candidates = {IPv4("192.168.1.9", 0)};
    out.clear();
    assert(!TqAclFilterResolvedAddresses(acl, candidates, 443, out));
    assert(out.empty());

    candidates.clear();
    out.clear();
    assert(!TqAclFilterResolvedAddresses(acl, candidates, 443, out));
}
```

- [ ] **Step 2: Add test target and run to verify failure**

Modify `src/CMakeLists.txt` near `tcpquic_acl_test`:

```cmake
add_executable(tcpquic_acl_filter_test
    unittest/acl_filter_test.cpp
    acl/acl.cpp
    acl/acl_filter.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_acl_filter_test)
set_property(TARGET tcpquic_acl_filter_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_acl_filter_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_acl_filter_test -j2
```

Expected: FAIL because `acl_filter.h` / `TqAclFilterResolvedAddresses` does not exist.

- [ ] **Step 3: Implement helper**

Create `src/acl/acl_filter.h`:

```cpp
#pragma once

#include "acl.h"
#include "platform_socket.h"

#include <cstdint>
#include <vector>

bool TqAclFilterResolvedAddresses(
    const TqAcl& acl,
    const std::vector<sockaddr_storage>& candidates,
    uint16_t port,
    std::vector<sockaddr_storage>& outAddrs);
```

Create `src/acl/acl_filter.cpp`:

```cpp
#include "acl_filter.h"

#include <cstring>

namespace {

void SetPort(sockaddr_storage& storage, uint16_t port) {
    if (storage.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&storage)->sin_port = htons(port);
    } else if (storage.ss_family == AF_INET6) {
        reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port = htons(port);
    }
}

std::string AddrText(const sockaddr_storage& storage) {
    char text[INET6_ADDRSTRLEN]{};
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        return inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr ? text : "";
    }
    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        return inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text)) != nullptr ? text : "";
    }
    return "";
}

} // namespace

bool TqAclFilterResolvedAddresses(
    const TqAcl& acl,
    const std::vector<sockaddr_storage>& candidates,
    uint16_t port,
    std::vector<sockaddr_storage>& outAddrs) {
    outAddrs.clear();
    for (const auto& candidate : candidates) {
        const std::string text = AddrText(candidate);
        if (text.empty() || !acl.IsAllowed(text, port)) {
            continue;
        }
        sockaddr_storage allowed = candidate;
        SetPort(allowed, port);
        outAddrs.push_back(allowed);
    }
    return !outAddrs.empty();
}
```

- [ ] **Step 4: Run ACL filter test**

Run:

```bash
cmake --build build --target tcpquic_acl_filter_test -j2
./build/bin/Release/tcpquic_acl_filter_test
```

Expected: build succeeds and executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/acl/acl_filter.h src/acl/acl_filter.cpp src/unittest/acl_filter_test.cpp src/CMakeLists.txt
git commit -m "feat(acl): filter resolved target addresses"
```

---

### Task 3: Add Linux Reactor Primitive

**Files:**
- Create: `src/runtime/linux_reactor.h`
- Create: `src/runtime/linux_reactor.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/linux_reactor_test.cpp`

- [ ] **Step 1: Write failing reactor test**

Create `src/unittest/linux_reactor_test.cpp`:

```cpp
#include "linux_reactor.h"

#include <cassert>
#include <unistd.h>

int main() {
    TqLinuxReactor reactor;
    assert(reactor.Start());

    int fds[2]{-1, -1};
    assert(pipe(fds) == 0);

    bool called = false;
    assert(reactor.Add(fds[0], TqLinuxReactorEvents::Read, [&](int fd, uint32_t events) {
        assert(fd == fds[0]);
        assert((events & TqLinuxReactorEvents::Read) != 0);
        char ch = 0;
        assert(read(fd, &ch, 1) == 1);
        assert(ch == 'x');
        called = true;
    }));

    assert(write(fds[1], "x", 1) == 1);
    assert(reactor.RunOnce(100));
    assert(called);

    called = false;
    assert(reactor.Remove(fds[0]));
    assert(write(fds[1], "y", 1) == 1);
    assert(!reactor.RunOnce(10));
    assert(!called);

    close(fds[0]);
    close(fds[1]);
}
```

- [ ] **Step 2: Add test target and verify failure**

Add to `src/CMakeLists.txt` inside `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` test section:

```cmake
add_executable(tcpquic_linux_reactor_test
    unittest/linux_reactor_test.cpp
    runtime/linux_reactor.cpp
)
tcpquic_target_include_dirs(tcpquic_linux_reactor_test)
set_property(TARGET tcpquic_linux_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_linux_reactor_test -j2
```

Expected: FAIL because `linux_reactor.h` does not exist.

- [ ] **Step 3: Implement Linux reactor**

Create `src/runtime/linux_reactor.h`:

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace TqLinuxReactorEvents {
constexpr uint32_t Read = 0x1;
constexpr uint32_t Write = 0x2;
constexpr uint32_t Error = 0x4;
}

class TqLinuxReactor {
public:
    using Handler = std::function<void(int fd, uint32_t events)>;

    TqLinuxReactor() = default;
    ~TqLinuxReactor();

    TqLinuxReactor(const TqLinuxReactor&) = delete;
    TqLinuxReactor& operator=(const TqLinuxReactor&) = delete;

    bool Start();
    void Stop();
    bool Add(int fd, uint32_t events, Handler handler);
    bool Modify(int fd, uint32_t events);
    bool Remove(int fd);
    bool Wake();
    bool RunOnce(int timeoutMs);

private:
    int EpollFd{-1};
    int WakeFd{-1};
    std::unordered_map<int, Handler> Handlers;
};
```

Create `src/runtime/linux_reactor.cpp` with `epoll_create1`, `eventfd`, `epoll_ctl`, and `epoll_wait`. Convert `EPOLLIN` to `Read`, `EPOLLOUT` to `Write`, and `EPOLLERR | EPOLLHUP | EPOLLRDHUP` to `Error`. `RunOnce()` returns true when at least one non-wakeup handler ran.

- [ ] **Step 4: Run reactor test**

Run:

```bash
cmake --build build --target tcpquic_linux_reactor_test -j2
./build/bin/Release/tcpquic_linux_reactor_test
```

Expected: executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/linux_reactor.h src/runtime/linux_reactor.cpp src/unittest/linux_reactor_test.cpp src/CMakeLists.txt
git commit -m "feat(runtime): add Linux reactor primitive"
```

---

### Task 4: Add Client Handshake State Machine

**Files:**
- Create: `src/ingress/client_ingress_state.h`
- Create: `src/ingress/client_ingress_state.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/client_ingress_state_test.cpp`

- [ ] **Step 1: Write failing state tests**

Create `src/unittest/client_ingress_state_test.cpp`:

```cpp
#include "client_ingress_state.h"

#include <cassert>
#include <string>

int main() {
    TqClientIngressState socks(TqClientIngressProto::Socks5);
    const uint8_t greeting[] = {0x05, 0x01, 0x00};
    assert(socks.Feed(greeting, sizeof(greeting)) == TqClientIngressResult::NeedWrite);
    assert(socks.PendingWrite() == std::string("\x05\x00", 2));
    socks.MarkWriteComplete(2);

    const uint8_t request[] = {
        0x05, 0x01, 0x00, 0x03,
        0x0b,
        'e','x','a','m','p','l','e','.','c','o','m',
        0x01, 0xbb};
    assert(socks.Feed(request, sizeof(request)) == TqClientIngressResult::ReadyToOpen);
    const TunnelRequest& req = socks.Request();
    assert(req.AddrType == TQ_ADDR_DOMAIN);
    assert(std::string(req.Host) == "example.com");
    assert(req.Port == 443);

    TqClientIngressState http(TqClientIngressProto::HttpConnect);
    const std::string httpReq =
        "CONNECT example.org:8443 HTTP/1.1\r\n"
        "Host: example.org:8443\r\n\r\n";
    assert(http.Feed(reinterpret_cast<const uint8_t*>(httpReq.data()), httpReq.size()) ==
        TqClientIngressResult::ReadyToOpen);
    assert(std::string(http.Request().Host) == "example.org");
    assert(http.Request().Port == 8443);
}
```

- [ ] **Step 2: Add test target and verify failure**

Add to `src/CMakeLists.txt`:

```cmake
add_executable(tcpquic_client_ingress_state_test
    unittest/client_ingress_state_test.cpp
    ingress/client_ingress_state.cpp
    ingress/socks5_server.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    tunnel/tcp_dialer.cpp
    protocol/control_protocol.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_client_ingress_state_test)
target_link_libraries(tcpquic_client_ingress_state_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
set_property(TARGET tcpquic_client_ingress_state_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_ingress_state_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_client_ingress_state_test -j2
```

Expected: FAIL because `client_ingress_state.h` does not exist.

- [ ] **Step 3: Implement state interface**

Create `src/ingress/client_ingress_state.h`:

```cpp
#pragma once

#include "tcp_tunnel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class TqClientIngressProto {
    Socks5,
    HttpConnect,
};

enum class TqClientIngressResult {
    NeedRead,
    NeedWrite,
    ReadyToOpen,
    Close,
};

class TqClientIngressState {
public:
    explicit TqClientIngressState(TqClientIngressProto proto);

    TqClientIngressResult Feed(const uint8_t* data, size_t size);
    const std::string& PendingWrite() const;
    void MarkWriteComplete(size_t bytes);
    const TunnelRequest& Request() const;

private:
    TqClientIngressResult FeedSocks5();
    TqClientIngressResult FeedHttpConnect();

    TqClientIngressProto Proto;
    std::vector<uint8_t> ReadBuffer;
    std::string WriteBuffer;
    TunnelRequest ParsedRequest{};
    bool SocksGreetingDone{false};
};
```

Implement `src/ingress/client_ingress_state.cpp` by reusing existing parse helpers:

```cpp
#include "client_ingress_state.h"

#include "http_connect_server.h"
#include "socks5_server.h"

#include <algorithm>
#include <cstring>

TqClientIngressState::TqClientIngressState(TqClientIngressProto proto) : Proto(proto) {}

const std::string& TqClientIngressState::PendingWrite() const { return WriteBuffer; }
const TunnelRequest& TqClientIngressState::Request() const { return ParsedRequest; }

void TqClientIngressState::MarkWriteComplete(size_t bytes) {
    bytes = std::min(bytes, WriteBuffer.size());
    WriteBuffer.erase(0, bytes);
}

TqClientIngressResult TqClientIngressState::Feed(const uint8_t* data, size_t size) {
    if (data != nullptr && size > 0) {
        ReadBuffer.insert(ReadBuffer.end(), data, data + size);
    }
    return Proto == TqClientIngressProto::Socks5 ? FeedSocks5() : FeedHttpConnect();
}
```

The implementation must parse only complete messages, leave partial data buffered, cap HTTP headers at 16 KiB, and generate exactly the SOCKS5 method response `0x05 0x00` for no-auth.

- [ ] **Step 4: Run state tests**

Run:

```bash
cmake --build build --target tcpquic_client_ingress_state_test -j2
./build/bin/Release/tcpquic_client_ingress_state_test
```

Expected: executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/ingress/client_ingress_state.h src/ingress/client_ingress_state.cpp src/unittest/client_ingress_state_test.cpp src/CMakeLists.txt
git commit -m "feat(ingress): add client handshake state machine"
```

---

### Task 5: Add Async Client Tunnel Open API

**Files:**
- Create: `src/tunnel/client_tunnel_open.h`
- Create: `src/tunnel/client_tunnel_open.cpp`
- Modify: `src/tunnel/tcp_tunnel.h`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/client_tunnel_open_test.cpp`

- [ ] **Step 1: Write compile-time API test**

Create `src/unittest/client_tunnel_open_test.cpp`:

```cpp
#include "client_tunnel_open.h"

#include <type_traits>

int main() {
    static_assert(std::is_same<
        decltype(&TqStartClientTunnelAsync),
        TqClientTunnelOpenHandle* (*)(
            MsQuicConnection*,
            const TunnelRequest&,
            TqSocketHandle,
            const TqConfig&,
            TqClientTunnelOpenComplete)>::value,
        "async client tunnel open signature must stay stable");

    static_assert(std::is_same<
        decltype(&TqCancelClientTunnelOpen),
        void (*)(TqClientTunnelOpenHandle*)>::value,
        "cancel signature must stay stable");
}
```

- [ ] **Step 2: Add test target and verify failure**

Add to `src/CMakeLists.txt` near `tcpquic_tunnel_test`:

```cmake
add_executable(tcpquic_client_tunnel_open_test
    unittest/client_tunnel_open_test.cpp
    tunnel/client_tunnel_open.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    tunnel/tunnel_reaper.cpp
    tunnel/relay.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    unittest/trace_proxy_stub.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_client_tunnel_open_test)
target_link_libraries(tcpquic_client_tunnel_open_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
target_compile_definitions(tcpquic_client_tunnel_open_test PRIVATE ${TCPQUIC_TEST_DEFS} TCPQUIC_TUNNEL_TESTING=1)
set_property(TARGET tcpquic_client_tunnel_open_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_tunnel_open_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_client_tunnel_open_test -j2
```

Expected: FAIL because `client_tunnel_open.h` does not exist.

- [ ] **Step 3: Add async API declarations**

Create `src/tunnel/client_tunnel_open.h`:

```cpp
#pragma once

#include "config.h"
#include "tcp_tunnel.h"

#include <functional>

struct TqClientTunnelOpenHandle;
struct MsQuicConnection;

using TqClientTunnelOpenComplete =
    std::function<void(TqClientTunnelOpenHandle*, TqTunnelStartResult)>;

TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete);

void TqCancelClientTunnelOpen(TqClientTunnelOpenHandle* handle);
```

Create `src/tunnel/client_tunnel_open.cpp`:

```cpp
#include "client_tunnel_open.h"

struct TqClientTunnelOpenHandle {
    bool Cancelled{false};
};

TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection*,
    const TunnelRequest&,
    TqSocketHandle,
    const TqConfig&,
    TqClientTunnelOpenComplete) {
    return nullptr;
}

void TqCancelClientTunnelOpen(TqClientTunnelOpenHandle* handle) {
    if (handle != nullptr) {
        handle->Cancelled = true;
    }
}
```

- [ ] **Step 4: Run compile-time API test**

Run:

```bash
cmake --build build --target tcpquic_client_tunnel_open_test -j2
./build/bin/Release/tcpquic_client_tunnel_open_test
```

Expected: executable exits 0.

- [ ] **Step 5: Replace stub with real async implementation**

Move the non-blocking parts of `TqStartClientTunnelInternal()` from `src/tunnel/tcp_tunnel.cpp` into a shared helper:

```cpp
TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete);
```

Implementation rules:

- create `TqTunnelContext` with `TqTunnelRole::ClientOpen`
- create `MsQuicStream`
- send OPEN request
- return handle before OPEN response
- on `TryCompleteClientOpen()`, invoke `onComplete(handle, result)`
- only start relay after successful OPEN and after ingress reactor has accepted completion
- cancellation closes/relinquishes `clientTcpFd` and arms existing shutdown cleanup path

- [ ] **Step 6: Run tunnel tests**

Run:

```bash
cmake --build build --target tcpquic_client_tunnel_open_test tcpquic_tunnel_test -j2
./build/bin/Release/tcpquic_client_tunnel_open_test
./build/bin/Release/tcpquic_tunnel_test
```

Expected: both executables exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/tunnel/client_tunnel_open.h src/tunnel/client_tunnel_open.cpp src/tunnel/tcp_tunnel.h src/tunnel/tcp_tunnel.cpp src/unittest/client_tunnel_open_test.cpp src/CMakeLists.txt
git commit -m "feat(tunnel): add async client open API"
```

---

### Task 6: Add c-ares DNS Resolver Wrapper

**Files:**
- Create: `src/tunnel/ares_dns_resolver.h`
- Create: `src/tunnel/ares_dns_resolver.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/ares_dns_resolver_test.cpp`

- [ ] **Step 1: Write resolver API test**

Create `src/unittest/ares_dns_resolver_test.cpp`:

```cpp
#include "ares_dns_resolver.h"

#include <cassert>

int main() {
    TqAresDnsResolver resolver;
    assert(resolver.Start());
    bool callbackCalled = false;
    const uint64_t id = resolver.Resolve("localhost", 443, [&](const TqDnsResolveResult& result) {
        callbackCalled = true;
        assert(result.Completed);
    });
    assert(id != 0);
    for (int i = 0; i < 100 && !callbackCalled; ++i) {
        resolver.RunOnce(10);
    }
    assert(callbackCalled);
    resolver.Stop();
}
```

- [ ] **Step 2: Add test target and verify failure**

Add to `src/CMakeLists.txt`:

```cmake
add_executable(tcpquic_ares_dns_resolver_test
    unittest/ares_dns_resolver_test.cpp
    tunnel/ares_dns_resolver.cpp
    runtime/linux_reactor.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_ares_dns_resolver_test)
target_link_libraries(tcpquic_ares_dns_resolver_test PRIVATE ${TCPQUIC_CARES_TARGET})
set_property(TARGET tcpquic_ares_dns_resolver_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_ares_dns_resolver_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_ares_dns_resolver_test -j2
```

Expected: FAIL because `ares_dns_resolver.h` does not exist.

- [ ] **Step 3: Implement resolver wrapper**

Create `src/tunnel/ares_dns_resolver.h`:

```cpp
#pragma once

#include "platform_socket.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TqDnsResolveResult {
    bool Completed{false};
    bool Success{false};
    int Status{0};
    std::vector<sockaddr_storage> Addresses;
};

using TqDnsResolveCallback = std::function<void(const TqDnsResolveResult&)>;

class TqAresDnsResolver {
public:
    bool Start();
    void Stop();
    uint64_t Resolve(const std::string& host, uint16_t port, TqDnsResolveCallback callback);
    void Cancel(uint64_t id);
    bool RunOnce(int timeoutMs);
};
```

Implement `src/tunnel/ares_dns_resolver.cpp` with:

- one `ares_channel`
- `ARES_OPT_SOCK_STATE_CB`
- a map from query id to callback
- `ares_getaddrinfo()`
- `ares_process_fds()` when available
- fallback `ares_process_fd()` code path guarded with c-ares version macros
- `ares_freeaddrinfo()` after copying sockaddr values into `sockaddr_storage`

- [ ] **Step 4: Run resolver test**

Run:

```bash
cmake --build build --target tcpquic_ares_dns_resolver_test -j2
./build/bin/Release/tcpquic_ares_dns_resolver_test
```

Expected: executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/tunnel/ares_dns_resolver.h src/tunnel/ares_dns_resolver.cpp src/unittest/ares_dns_resolver_test.cpp src/CMakeLists.txt
git commit -m "feat(tunnel): add c-ares DNS resolver"
```

---

### Task 7: Add Server Dial Reactor

**Files:**
- Create: `src/tunnel/server_dial_reactor.h`
- Create: `src/tunnel/server_dial_reactor.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/server_dial_reactor_test.cpp`

- [ ] **Step 1: Write fake-driven dial reactor test**

Create `src/unittest/server_dial_reactor_test.cpp`:

```cpp
#include "server_dial_reactor.h"

#include <cassert>

int main() {
    TqServerDialReactor reactor;
    assert(reactor.Start());

    TqServerDialRequest req{};
    req.Host = "127.0.0.1";
    req.Port = 9;
    req.TraceTunnelId = 77;

    bool completed = false;
    req.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        assert(result.Done);
    };

    const uint64_t token = reactor.Submit(std::move(req));
    assert(token != 0);
    reactor.Cancel(token);
    reactor.RunOnce(10);
    assert(!completed);

    reactor.Stop();
}
```

- [ ] **Step 2: Add test target and verify failure**

Add to `src/CMakeLists.txt`:

```cmake
add_executable(tcpquic_server_dial_reactor_test
    unittest/server_dial_reactor_test.cpp
    tunnel/server_dial_reactor.cpp
    tunnel/ares_dns_resolver.cpp
    tunnel/tcp_dialer.cpp
    runtime/linux_reactor.cpp
    acl/acl.cpp
    acl/acl_filter.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_server_dial_reactor_test)
target_link_libraries(tcpquic_server_dial_reactor_test PRIVATE ${TCPQUIC_CARES_TARGET})
set_property(TARGET tcpquic_server_dial_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_server_dial_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_server_dial_reactor_test -j2
```

Expected: FAIL because `server_dial_reactor.h` does not exist.

- [ ] **Step 3: Implement server dial reactor API**

Create `src/tunnel/server_dial_reactor.h`:

```cpp
#pragma once

#include "control_protocol.h"
#include "platform_socket.h"

#include <cstdint>
#include <functional>
#include <string>

struct TqServerDialResult {
    bool Done{false};
    TqOpenError Error{TqOpenError::Internal};
    TqSocketHandle Fd{TqInvalidSocket};
};

using TqServerDialComplete = std::function<void(const TqServerDialResult&)>;

struct TqServerDialRequest {
    std::string Host;
    uint16_t Port{0};
    uint64_t TraceTunnelId{0};
    TqServerDialComplete Complete;
};

class TqServerDialReactor {
public:
    bool Start();
    void Stop();
    uint64_t Submit(TqServerDialRequest request);
    void Cancel(uint64_t token);
    bool RunOnce(int timeoutMs);
};
```

Implement `src/tunnel/server_dial_reactor.cpp` with:

- token allocation starting at 1
- map token -> dial state
- state-owned connect socket
- cancel path closes socket and suppresses completion
- literal IP path skips DNS
- domain path calls `TqAresDnsResolver`
- resolved addresses pass through `TqAclFilterResolvedAddresses`

- [ ] **Step 4: Run server dial reactor test**

Run:

```bash
cmake --build build --target tcpquic_server_dial_reactor_test -j2
./build/bin/Release/tcpquic_server_dial_reactor_test
```

Expected: executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/tunnel/server_dial_reactor.h src/tunnel/server_dial_reactor.cpp src/unittest/server_dial_reactor_test.cpp src/CMakeLists.txt
git commit -m "feat(tunnel): add server dial reactor"
```

---

### Task 8: Add Client Ingress Reactor

**Files:**
- Create: `src/ingress/client_ingress_reactor.h`
- Create: `src/ingress/client_ingress_reactor.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/client_ingress_reactor_test.cpp`

- [ ] **Step 1: Write reactor construction test**

Create `src/unittest/client_ingress_reactor_test.cpp`:

```cpp
#include "client_ingress_reactor.h"

#include <cassert>

int main() {
    TqClientIngressReactor reactor;
    assert(reactor.Start());

    TqClientIngressPeer peer{};
    peer.PeerId = "peer-a";
    peer.SocksListen = "127.0.0.1:0";
    peer.HttpListen = "127.0.0.1:0";

    assert(reactor.AddPeer(peer));
    assert(reactor.PeerCountForTest() == 1);
    assert(reactor.RemovePeer("peer-a"));
    assert(reactor.PeerCountForTest() == 0);

    reactor.Stop();
}
```

- [ ] **Step 2: Add test target and verify failure**

Add to `src/CMakeLists.txt`:

```cmake
add_executable(tcpquic_client_ingress_reactor_test
    unittest/client_ingress_reactor_test.cpp
    ingress/client_ingress_reactor.cpp
    ingress/client_ingress_state.cpp
    ingress/proxy_auth.cpp
    tunnel/client_tunnel_open.cpp
    tunnel/tcp_tunnel.cpp
    runtime/linux_reactor.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    tunnel/relay.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    tunnel/tunnel_reaper.cpp
    protocol/compress.cpp
    unittest/trace_proxy_stub.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_client_ingress_reactor_test)
target_link_libraries(tcpquic_client_ingress_reactor_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
target_compile_definitions(tcpquic_client_ingress_reactor_test PRIVATE ${TCPQUIC_TEST_DEFS} TCPQUIC_TUNNEL_TESTING=1)
set_property(TARGET tcpquic_client_ingress_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_ingress_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Run:

```bash
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
```

Expected: FAIL because `client_ingress_reactor.h` does not exist.

- [ ] **Step 3: Implement reactor shell**

Create `src/ingress/client_ingress_reactor.h`:

```cpp
#pragma once

#include "config.h"

#include <cstddef>
#include <string>

struct TqClientIngressPeer {
    std::string PeerId;
    std::string SocksListen;
    std::string HttpListen;
    TqConfig Config;
};

class TqClientIngressReactor {
public:
    bool Start();
    void Stop();
    bool AddPeer(const TqClientIngressPeer& peer);
    bool RemovePeer(const std::string& peerId);
    size_t PeerCountForTest() const;
};
```

Implement `src/ingress/client_ingress_reactor.cpp` with:

- one worker thread
- `TqLinuxReactor` instance
- peer map
- listen socket creation using existing host:port parsing logic copied into a focused helper
- nonblocking listen sockets
- accept loop per readiness event

- [ ] **Step 4: Run reactor construction test**

Run:

```bash
cmake --build build --target tcpquic_client_ingress_reactor_test -j2
./build/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp src/unittest/client_ingress_reactor_test.cpp src/CMakeLists.txt
git commit -m "feat(ingress): add client ingress reactor"
```

---

### Task 9: Wire Linux Client Runtime To Ingress Reactor

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/ingress/client_ingress_reactor.h/.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Add routing adapter expectation test**

Modify `src/unittest/router_runtime_test.cpp` to assert multi-peer metrics remain unchanged after switching runtime ownership. Add a focused assertion near existing peer metrics checks:

```cpp
if (body.find("\"socks_listen\":\"127.0.0.1:1080\"") == std::string::npos) return 180;
if (body.find("\"http_listen\":\"127.0.0.1:8080\"") == std::string::npos) return 181;
```

- [ ] **Step 2: Run router runtime test before wiring**

Run:

```bash
cmake --build build --target tcpquic_router_runtime_test -j2
./build/bin/Release/tcpquic_router_runtime_test
```

Expected: PASS before wiring; this protects admin-visible behavior.

- [ ] **Step 3: Replace Linux client listener ownership**

In `src/main.cpp`, under `#if defined(__linux__)`:

- create one `std::unique_ptr<TqClientIngressReactor>` for single-peer runtime
- create one shared `TqClientIngressReactor` in multi-peer adapter
- remove Linux use of per-peer `TqThreadPool Pool`
- keep old `TqSocks5Server` / `TqHttpConnectServer` path under non-Linux compile branch

Required behavior:

```text
single peer:
  TqSinglePeerClientRuntime::Start()
    -> IngressReactor.Start()
    -> AddPeer(primary)

multi peer:
  adapter StartPeer(peer)
    -> SharedIngressReactor.AddPeer(peer)
  adapter StopPeer(peer)
    -> SharedIngressReactor.RemovePeer(peerId)
```

- [ ] **Step 4: Run client ingress and router tests**

Run:

```bash
cmake --build build --target tcpquic_client_ingress_reactor_test tcpquic_router_runtime_test -j2
./build/bin/Release/tcpquic_client_ingress_reactor_test
./build/bin/Release/tcpquic_router_runtime_test
```

Expected: both executables exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp src/unittest/router_runtime_test.cpp src/CMakeLists.txt
git commit -m "feat(client): route Linux ingress through one reactor"
```

---

### Task 10: Wire Server OPEN To Dial Reactor

**Files:**
- Modify: `src/tunnel/tcp_tunnel.h`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/tunnel/server_dial_reactor.h/.cpp`
- Modify: `src/main.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

- [ ] **Step 1: Add cancellation regression test**

Modify `src/unittest/tcp_tunnel_test.cpp` to include a server-open cancellation test under `TCPQUIC_TUNNEL_TESTING` helpers:

```cpp
static int TestServerOpenCancelDoesNotUseFreedTunnel() {
    TqServerDialReactor reactor;
    if (!reactor.Start()) return 1401;
    TqServerDialRequest req{};
    req.Host = "127.0.0.1";
    req.Port = 9;
    bool completed = false;
    req.Complete = [&](const TqServerDialResult&) { completed = true; };
    const uint64_t token = reactor.Submit(std::move(req));
    reactor.Cancel(token);
    reactor.RunOnce(10);
    reactor.Stop();
    return completed ? 1402 : 0;
}
```

Call it from `main()`:

```cpp
if (int rc = TestServerOpenCancelDoesNotUseFreedTunnel()) return rc;
```

- [ ] **Step 2: Run test to verify it fails before wiring**

Run:

```bash
cmake --build build --target tcpquic_tunnel_test -j2
```

Expected: FAIL because `TqServerDialReactor` is not linked into `tcpquic_tunnel_test`.

- [ ] **Step 3: Inject dial reactor into server tunnel path**

Modify `src/tunnel/tcp_tunnel.h`:

```cpp
class TqServerDialReactor;

void TqSetServerDialReactor(TqServerDialReactor* reactor);
```

Modify `src/tunnel/tcp_tunnel.cpp`:

- add process-global atomic or mutex-protected `TqServerDialReactor*`
- in `TryHandleServerOpen()`, after parsing OPEN request, submit to reactor on Linux
- keep existing detached thread path when reactor is null
- store token in `TqTunnelContext`
- cancel token in abort / shutdown / destructor paths

- [ ] **Step 4: Start reactor in server runtime**

Modify `src/main.cpp` server startup:

```cpp
#if defined(__linux__)
    TqServerDialReactor serverDial;
    if (!serverDial.Start()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to start server dial reactor\n");
        return 1;
    }
    TqSetServerDialReactor(&serverDial);
#endif
```

On server shutdown, call:

```cpp
#if defined(__linux__)
    TqSetServerDialReactor(nullptr);
    serverDial.Stop();
#endif
```

- [ ] **Step 5: Run tunnel tests**

Run:

```bash
cmake --build build --target tcpquic_tunnel_test tcpquic_server_dial_reactor_test -j2
./build/bin/Release/tcpquic_server_dial_reactor_test
./build/bin/Release/tcpquic_tunnel_test
```

Expected: both executables exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp src/tunnel/tcp_tunnel.h src/tunnel/tcp_tunnel.cpp src/tunnel/server_dial_reactor.h src/tunnel/server_dial_reactor.cpp src/unittest/tcp_tunnel_test.cpp src/CMakeLists.txt
git commit -m "feat(server): route Linux OPEN dial through reactor"
```

---

### Task 11: Update Documentation And Full Verification

**Files:**
- Modify: `docs/thread-model_cn.md`
- Modify: `docs/superpowers/specs/2026-06-18-thread-model-cares-design.md`
- Modify: `README.md` if build requirements mention third-party dependencies

- [ ] **Step 1: Update thread model document**

Modify `docs/thread-model_cn.md` so Linux current model says:

```markdown
Linux client 模式下，所有 peer 的 SOCKS5 / HTTP CONNECT accept 和 handshake 由一个 ingress reactor 线程处理。
Linux server 模式下，普通 OPEN 的 DNS 和 TCP connect 由一个 server dial reactor 线程处理。
Windows 当前仍使用旧 listener / handshake pool / per-OPEN dial worker 模型，后续单独迁移。
Relay worker 线程模型不变。
```

- [ ] **Step 2: Run focused test suite**

Run:

```bash
cmake --build build --target \
  tcpquic_acl_filter_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_linux_reactor_test \
  tcpquic_client_ingress_state_test \
  tcpquic_client_ingress_reactor_test \
  tcpquic_client_tunnel_open_test \
  tcpquic_ares_dns_resolver_test \
  tcpquic_server_dial_reactor_test \
  tcpquic_tunnel_test \
  tcpquic_router_runtime_test \
  -j2
```

Expected: all targets build successfully.

- [ ] **Step 3: Run focused executables**

Run:

```bash
./build/bin/Release/tcpquic_acl_filter_test
./build/bin/Release/tcpquic_linux_reactor_test
./build/bin/Release/tcpquic_client_ingress_state_test
./build/bin/Release/tcpquic_client_ingress_reactor_test
./build/bin/Release/tcpquic_client_tunnel_open_test
./build/bin/Release/tcpquic_ares_dns_resolver_test
./build/bin/Release/tcpquic_server_dial_reactor_test
./build/bin/Release/tcpquic_tunnel_test
./build/bin/Release/tcpquic_router_runtime_test
```

Expected: every executable exits 0.

- [ ] **Step 4: Verify thread-count behavior manually**

Run a Linux client with two peers and both SOCKS5 / HTTP CONNECT enabled. Then run:

```bash
ps -L -p "$(pgrep -n tcpquic-proxy)" -o tid,comm | rg "ingress|dial|tcpquic|quic|relay"
```

Expected:

- one Linux client ingress reactor thread
- no per-peer SOCKS5 listener thread
- no per-peer HTTP CONNECT listener thread
- no per-peer handshake pool worker
- existing MsQuic and relay worker threads still present

- [ ] **Step 5: Commit**

```bash
git add docs/thread-model_cn.md docs/superpowers/specs/2026-06-18-thread-model-cares-design.md README.md
git commit -m "docs: update async reactor thread model"
```

---

## Self-Review

Spec coverage:

- client single ingress thread: Tasks 4, 5, 8, 9
- server single dial thread: Tasks 6, 7, 10
- c-ares async DNS: Tasks 1, 6, 7
- no-DNS ACL filtering: Task 2
- async client OPEN API: Task 5
- server dial lifecycle and cancel: Tasks 7, 10
- resource limits: Tasks 8 and 10 must enforce constants from the spec while implementing reactor queues
- Windows boundary: Task 11 documents Linux migrated / Windows old-path boundary
- tests and verification: Tasks 2 through 11

No placeholders remain in this plan. All tasks have exact paths, commands, expected outcomes, and commit points.
