# Linux Relay Tunnel Abort Close Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix Linux relay tunnel close handling so a closed or aborted tunnel is reclaimed promptly and cannot poison later streams on the same QUIC connection.

**Architecture:** Add explicit per-relay abnormal-close handling to `TqLinuxRelayWorker`. Keep graceful half-close behavior when both directions can drain, but reset only the affected tunnel when TCP/QUIC reports a definitive close/error with pending data that can no longer complete.

**Tech Stack:** C++17, MsQuic stream callbacks, Linux epoll/TCP socket options, existing unit tests under `src/unittest`, DGX iperf3/netem diagnostic script.

---

## File Map

- Modify: `src/tunnel/linux_relay_worker.h`
  - Add Linux relay TCP keepalive/user-timeout config fields.
  - Add private helpers for abnormal close, stream binding detach, and pending-state checks.
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - Add TCP keepalive/user-timeout setup.
  - Add helper for single-tunnel abort/reclaim.
  - Change `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` handling so pending relay state is reset instead of left behind.
  - Reset a relay when TCP peer half-closes while TCP read is paused by QUIC send backlog.
  - Treat TCP `ECONNRESET`/timeout as fatal tunnel reset instead of graceful EOF.
  - Use `SO_ERROR` on EPOLLERR.
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
  - Add regression tests for stream shutdown with pending data.
  - Add tests for graceful half-close that must still drain.
  - Add tests for `EPOLLRDHUP` while TCP read is paused by QUIC send backlog.
  - Add tests for TCP write failure reset behavior.
  - Update the existing `SO_LINGER {on,0}` RST test to expect fatal relay reset.
- Modify: `scripts/debug-dgx-iperf3-download-issue.sh`
  - Add an acceptance mode that treats any low-throughput/timeout trigger as failure but cleans up when the full sequence passes.
- Modify: `docs/superpowers/plans/2026-06-23-dgx-10loss-io-throughput-validation-cn.md`
  - Append post-fix validation record after implementation and DGX run.

## Task 1: Add failing unit test for stream shutdown with pending data

**Files:**
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add a regression test block before the existing peer-send-aborted test**

Add this block near the existing shutdown/pending test around the current `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` coverage:

```cpp
    {
        QUIC_API_TABLE table{};
        InstallFakeMsQuicForSend(table);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 8 * 1024 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        int sendBuffer = 4096;
        (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
        const int peerFlags = ::fcntl(fds[1], F_GETFL, 0);
        (void)::fcntl(fds[1], F_SETFL, peerFlags | O_NONBLOCK);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = true;

        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const std::vector<uint8_t> plain(2 * 1024 * 1024, 0x5a);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;
        receiveEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

        const TqLinuxRelayWorkerSnapshot before = worker.Snapshot();
        if (before.PendingBytes == 0 || handle.Stop.load(std::memory_order_acquire)) {
            std::fprintf(stderr,
                "expected pending bytes before shutdown_complete, pending=%llu stop=%d\n",
                static_cast<unsigned long long>(before.PendingBytes),
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent) != QUIC_STATUS_SUCCESS) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

        const TqLinuxRelayWorkerSnapshot after = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            after.PendingBytes != 0 ||
            after.FatalRelayResets == 0) {
            std::fprintf(stderr,
                "expected shutdown_complete with pending data to reset relay, stop=%d pending=%llu resets=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(after.PendingBytes),
                static_cast<unsigned long long>(after.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }
```

- [ ] **Step 2: Run the failing test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected before implementation:

```text
expected shutdown_complete with pending data to reset relay
```

or equivalent failure because `SHUTDOWN_COMPLETE` currently detaches the stream but does not reclaim the relay when pending data remains.

## Task 2: Add single-tunnel abort/reclaim helpers

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add helper declarations in `TqLinuxRelayWorker` private section**

In `src/tunnel/linux_relay_worker.h`, add these private method declarations next to `FailRelayFatal`:

```cpp
    void AbortRelayAndRelease(RelayState* relay, const char* trigger, bool abortStream);
    void DetachRelayStreamBinding(
        RelayState* relay,
        MsQuicStream* stream,
        StreamRelayBinding* binding);
    bool HasPendingAfterStreamShutdown(RelayState* relay) const;
```

- [ ] **Step 2: Implement `DetachRelayStreamBinding`**

In `src/tunnel/linux_relay_worker.cpp`, add this implementation before `AbortRelayAndRelease`:

```cpp
void TqLinuxRelayWorker::DetachRelayStreamBinding(
    RelayState* relay,
    MsQuicStream* stream,
    StreamRelayBinding* binding) {
    if (binding == nullptr) {
        if (relay != nullptr) {
            relay->Stream = nullptr;
            relay->StreamBinding = nullptr;
        }
        return;
    }
    binding->Closing.store(true, std::memory_order_release);
    binding->Relay.store(nullptr, std::memory_order_release);

    MsQuicStream* boundStream = stream;
    if (boundStream == nullptr && relay != nullptr) {
        boundStream = relay->Stream;
    }
    if (boundStream != nullptr && boundStream->Context == binding) {
        boundStream->Callback = MsQuicStream::NoOpCallback;
        boundStream->Context = nullptr;
    }
    if (relay != nullptr) {
        relay->Stream = nullptr;
        relay->StreamBinding = nullptr;
    }
    std::lock_guard<std::mutex> guard(RetiredBindingLock);
    RetiredStreamBindings.emplace_back(binding);
}
```

- [ ] **Step 3: Implement `AbortRelayAndRelease` by extracting the cleanup body from `FailRelayFatal`**

In `src/tunnel/linux_relay_worker.cpp`, add this implementation before `FailRelayFatal`:

```cpp
void TqLinuxRelayWorker::AbortRelayAndRelease(
    RelayState* relay,
    const char* trigger,
    bool abortStream) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    SetRelayStop(relay, trigger);
    relay->Closing = true;
    if (EpollFd >= 0 && relay->TcpFd >= 0) {
        (void)::epoll_ctl(EpollFd, EPOLL_CTL_DEL, relay->TcpFd, nullptr);
    }
    if (relay->TcpFd >= 0) {
        TqResetSocket(relay->TcpFd);
        relay->TcpFd = -1;
    }
    auto* binding = relay->StreamBinding;
    if (abortStream && relay->Stream != nullptr && relay->Stream->Handle != nullptr) {
        (void)relay->Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
    DetachRelayStreamBinding(relay, relay->Stream, binding);
    relay->PendingTcpWrites.clear();
    relay->PendingQuicSendRetries.clear();
    for (auto& pending : relay->PendingQuicReceives) {
        if (pending) {
            FlushDeferredReceiveCompletion(*pending, true);
        }
    }
    relay->PendingQuicReceives.clear();
    relay->PendingQuicReceiveBytes = 0;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        relay->CallbackPendingQuicReceives.clear();
    }
}
```

- [ ] **Step 4: Make `FailRelayFatal` call the helper**

Replace the cleanup body of `FailRelayFatal` after `LogLinuxRelayError(...)` with:

```cpp
    AbortRelayAndRelease(relay, trigger, abortStream);
```

Keep the existing `FatalRelayResets.fetch_add(1)` and `LogLinuxRelayError(...)` before this call.

- [ ] **Step 5: Run the existing relay worker test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected after this refactor:

```text
process exits 0
```

The new Task 1 test may still fail until Task 3 changes the shutdown-complete behavior.

## Task 3: Reset relay on stream shutdown complete with pending state

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add a private helper to detect pending state after stream shutdown**

In `src/tunnel/linux_relay_worker.cpp`, add this member implementation near `MaybeStopFullyClosedRelay`:

```cpp
bool TqLinuxRelayWorker::HasPendingAfterStreamShutdown(RelayState* relay) const {
    if (relay == nullptr) {
        return false;
    }
    bool hasCallbackPending = false;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        hasCallbackPending = !relay->CallbackPendingQuicReceives.empty();
    }
    return relay->OutstandingQuicSends != 0 ||
           relay->OutstandingQuicSendBytes != 0 ||
           !relay->PendingQuicSendRetries.empty() ||
           !relay->PendingTcpWrites.empty() ||
           !relay->PendingQuicReceives.empty() ||
           hasCallbackPending ||
           relay->PendingQuicReceiveBytes != 0 ||
           relay->TcpWriteShutdownQueued ||
           relay->QuicSendFinSubmitted != relay->QuicSendFinCompleted;
}
```

- [ ] **Step 2: Change `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` handling**

In `OnStreamEventWithBinding`, inside the `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` branch, replace the final detach/`MaybeStopFullyClosedRelay` section with:

```cpp
        const bool hasPending = HasPendingAfterStreamShutdown(relay);
        if (hasPending) {
            FatalRelayResets.fetch_add(1);
            LogLinuxRelayError(
                "stream_shutdown_complete_with_pending",
                relay->Id,
                relay->TcpFd,
                LastQuicSendStatus.load(),
                relay->LastTcpWriteErrno,
                PendingTcpWriteBytes(relay->PendingTcpWrites),
                relay->PendingQuicReceiveBytes,
                relay->OutstandingQuicSends,
                relay->OutstandingQuicSendBytes);
            AbortRelayAndRelease(relay, "stream_shutdown_complete_with_pending", false);
        } else {
            DetachRelayStreamBinding(relay, stream, binding);
            MaybeStopFullyClosedRelay(relay, "stream_shutdown_complete");
        }
        return QUIC_STATUS_SUCCESS;
```

- [ ] **Step 3: Run the regression test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected:

```text
process exits 0
```

The Task 1 regression should now pass.

## Task 4: Reset when TCP peer half-closes while QUIC send backlog pauses TCP read

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add the failing unit test**

Add this block in `src/unittest/linux_relay_worker_io_test.cpp` before the existing SO_LINGER reset test:

```cpp
    {
        QUIC_API_TABLE table{};
        InstallFakeMsQuicForSend(table);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 8192;
        config.MaxIov = 4;
        config.MaxBufferedQuicSendBytes = 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = true;

        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const std::vector<uint8_t> payload(8192, 0x61);
        if (::write(fds[1], payload.data(), payload.size()) != static_cast<ssize_t>(payload.size())) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        if (!worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLIN)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        TqLinuxRelayWorkerSnapshot paused = worker.Snapshot();
        if (paused.TcpReadDisabledRelays == 0 || paused.OutstandingQuicSends == 0) {
            std::fprintf(stderr,
                "expected TCP read paused by QUIC backlog, disabled=%llu sends=%llu\n",
                static_cast<unsigned long long>(paused.TcpReadDisabledRelays),
                static_cast<unsigned long long>(paused.OutstandingQuicSends));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        if (::shutdown(fds[1], SHUT_WR) != 0 ||
            !worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLRDHUP)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        CompleteFakeSends(worker, fakeStream);
        (void)worker.DrainForTest(config.EventBudget);

        const TqLinuxRelayWorkerSnapshot reset = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            reset.FatalRelayResets == 0 ||
            reset.PendingBytes != 0) {
            std::fprintf(stderr,
                "expected RDHUP while read paused by QUIC backlog to reset relay, stop=%d resets=%llu pending=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(reset.FatalRelayResets),
                static_cast<unsigned long long>(reset.PendingBytes));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }
```

- [ ] **Step 2: Run the test and verify it fails before implementation**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected before implementation:

```text
expected RDHUP while read paused by QUIC backlog to reset relay
```

- [ ] **Step 3: Reset on RDHUP/HUP while QUIC backlog has paused TCP read**

In `src/tunnel/linux_relay_worker.cpp`, at the start of `ProcessTcpEvents` after finding `relay`, add this block before processing `EPOLLOUT`:

```cpp
    const bool tcpPeerHalfClosed = (events & (EPOLLRDHUP | EPOLLHUP)) != 0;
    if (tcpPeerHalfClosed &&
        relay->TcpReadPausedByQuicBacklog &&
        (relay->OutstandingQuicSendBytes != 0 || !relay->PendingQuicSendRetries.empty())) {
        FailRelayFatal(relay.get(), "tcp_peer_closed_while_quic_send_backlog", true);
        return;
    }
```

Do not apply this rule to plain `EPOLLRDHUP` when `TcpReadPausedByQuicBacklog == false`; ordinary TCP half-close still follows the existing graceful FIN path.

- [ ] **Step 4: Run the Linux relay test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected:

```text
process exits 0
```

## Task 5: Treat TCP reset as fatal tunnel reset

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Update the existing SO_LINGER reset test expectation**

In `src/unittest/linux_relay_worker_io_test.cpp`, find the block that sets:

```cpp
linger resetLinger{};
resetLinger.l_onoff = 1;
resetLinger.l_linger = 0;
```

Replace the assertion that currently expects `ECONNRESET` to close input gracefully with:

```cpp
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.TcpReadHardErrors == 0 ||
            snapshot.FatalRelayResets == 0 ||
            snapshot.TcpReadClosedRelays != 0) {
            std::fprintf(stderr,
                "expected tcp reset during read to fatal-reset relay, read_errors=%llu "
                "resets=%llu read_closed=%llu errno=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpReadHardErrors),
                static_cast<unsigned long long>(snapshot.FatalRelayResets),
                static_cast<unsigned long long>(snapshot.TcpReadClosedRelays),
                static_cast<unsigned long long>(snapshot.LastTcpReadErrno));
            worker.Stop();
            return 1;
        }
```

- [ ] **Step 2: Run the test and verify it fails before implementation**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected before implementation:

```text
expected tcp reset during read to fatal-reset relay
```

- [ ] **Step 3: Change TCP read reset classification**

In `src/tunnel/linux_relay_worker.cpp`, replace:

```cpp
bool IsTcpReadGracefulCloseError(int error) {
    return error == ECONNRESET;
}
```

with:

```cpp
bool IsTcpReadGracefulCloseError(int error) {
    (void)error;
    return false;
}
```

This leaves `readv()` returning `ECONNRESET` to the existing hard-error path:

```cpp
RecordError(RelayErrorKind::TcpReadHard);
LastTcpReadErrno.store(savedErrno);
FailRelayFatal(relay, "tcp_read_hard_error", true);
```

- [ ] **Step 4: Run the Linux relay test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected:

```text
process exits 0
```

## Task 6: Configure TCP keepalive and TCP_USER_TIMEOUT on relay TCP sockets

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add config fields**

In `TqLinuxRelayWorkerConfig`, add:

```cpp
    uint32_t TcpKeepaliveIdleSec{10};
    uint32_t TcpKeepaliveIntervalSec{2};
    uint32_t TcpKeepaliveCount{3};
    uint32_t TcpUserTimeoutMs{10000};
```

- [ ] **Step 2: Add Linux socket option helper**

In `src/tunnel/linux_relay_worker.cpp`, include TCP definitions:

```cpp
#include <netinet/tcp.h>
```

Add this helper in the anonymous namespace:

```cpp
void ConfigureRelayTcpLiveness(
    int fd,
    uint32_t keepIdleSec,
    uint32_t keepIntervalSec,
    uint32_t keepCount,
    uint32_t userTimeoutMs) {
    if (fd < 0) {
        return;
    }
    int enabled = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    if (keepIdleSec != 0) {
        int value = static_cast<int>(keepIdleSec);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
    }
    if (keepIntervalSec != 0) {
        int value = static_cast<int>(keepIntervalSec);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value));
    }
    if (keepCount != 0) {
        int value = static_cast<int>(keepCount);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value));
    }
    if (userTimeoutMs != 0) {
        int value = static_cast<int>(userTimeoutMs);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &value, sizeof(value));
    }
}
```

- [ ] **Step 3: Call the helper during relay registration**

In `RegisterRelayWithId`, after `TqSetNonBlocking(registration.TcpFd)` succeeds, add:

```cpp
        ConfigureRelayTcpLiveness(
            registration.TcpFd,
            Config.TcpKeepaliveIdleSec,
            Config.TcpKeepaliveIntervalSec,
            Config.TcpKeepaliveCount,
            Config.TcpUserTimeoutMs);
```

- [ ] **Step 4: Build the test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected:

```text
process exits 0
```

## Task 7: Improve EPOLLERR handling with SO_ERROR

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add SO_ERROR helper**

Add this helper in the anonymous namespace:

```cpp
int GetSocketError(int fd) {
    if (fd < 0) {
        return EBADF;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return errno != 0 ? errno : EIO;
    }
    return error;
}
```

- [ ] **Step 2: Reset on explicit socket error**

At the start of `ProcessTcpEvents`, after finding `relay`, add:

```cpp
    if ((events & EPOLLERR) != 0) {
        const int socketError = GetSocketError(relay->TcpFd);
        if (socketError != 0) {
            relay->LastTcpWriteErrno = static_cast<uint64_t>(socketError);
            LastTcpWriteErrno.store(static_cast<uint64_t>(socketError));
            RecordError(RelayErrorKind::TcpWriteHard);
            FatalRelayResets.fetch_add(1);
            LogLinuxRelayError(
                "tcp_socket_error",
                relay->Id,
                relay->TcpFd,
                LastQuicSendStatus.load(),
                static_cast<uint64_t>(socketError),
                PendingTcpWriteBytes(relay->PendingTcpWrites),
                relay->PendingQuicReceiveBytes,
                relay->OutstandingQuicSends,
                relay->OutstandingQuicSendBytes);
            AbortRelayAndRelease(relay.get(), "tcp_socket_error", true);
            return;
        }
    }
```

Do not reset solely on `EPOLLHUP` or `EPOLLRDHUP` without a socket error; those can be valid half-close signals. `EPOLLERR` with `SO_ERROR != 0` is an explicit TCP error and must reset only the current relay.

- [ ] **Step 3: Run Linux relay tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected:

```text
process exits 0
```

## Task 8: Add acceptance mode to the DGX diagnostic script

**Files:**
- Modify: `scripts/debug-dgx-iperf3-download-issue.sh`

- [ ] **Step 1: Add an acceptance flag near existing environment defaults**

Add:

```bash
ACCEPTANCE_MODE="${ACCEPTANCE_MODE:-0}"
```

- [ ] **Step 2: Make trigger handling fail and clean in acceptance mode**

In both `run_case()` and `run_case_existing_proxy()`, replace each trigger block that currently does:

```bash
echo "trigger=iperf_rc_${rc}" >"$case_dir/trigger.txt"
capture_site "$case_dir"
PRESERVE_SITE=1
return 100
```

with:

```bash
if [[ "$ACCEPTANCE_MODE" == "1" ]]; then
    echo "trigger=iperf_rc_${rc}" >"$case_dir/trigger.txt"
    capture_site "$case_dir"
    log "acceptance mode: trigger=iperf_rc_${rc}; cleaning up and failing validation"
    PRESERVE_SITE=0
    cleanup_all
    return 100
fi

echo "trigger=iperf_rc_${rc}" >"$case_dir/trigger.txt"
capture_site "$case_dir"
PRESERVE_SITE=1
return 100
```

Also replace each low-throughput trigger block that currently does:

```bash
echo "trigger=low_throughput_${recv_mbps}_mbps" >"$case_dir/trigger.txt"
capture_site "$case_dir"
PRESERVE_SITE=1
return 100
```

with:

```bash
if [[ "$ACCEPTANCE_MODE" == "1" ]]; then
    echo "trigger=low_throughput_${recv_mbps}_mbps" >"$case_dir/trigger.txt"
    capture_site "$case_dir"
    log "acceptance mode: trigger=low_throughput_${recv_mbps}_mbps; cleaning up and failing validation"
    PRESERVE_SITE=0
    cleanup_all
    return 100
fi

echo "trigger=low_throughput_${recv_mbps}_mbps" >"$case_dir/trigger.txt"
capture_site "$case_dir"
PRESERVE_SITE=1
return 100
```

- [ ] **Step 3: Verify script syntax**

Run:

```bash
rtk bash -n scripts/debug-dgx-iperf3-download-issue.sh
```

Expected:

```text
no output, exit 0
```

## Task 9: Build tcpquic-proxy and run local verification

**Files:**
- Build outputs only

- [ ] **Step 1: Build the updated binary**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy tcpquic_linux_relay_worker_io_test -j$(nproc)
```

Expected:

```text
tcpquic-proxy and tcpquic_linux_relay_worker_io_test build successfully
```

- [ ] **Step 2: Run unit test**

Run:

```bash
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected:

```text
process exits 0
```

## Task 10: DGX reproduction acceptance test

**Files:**
- Uses: `scripts/debug-dgx-iperf3-download-issue.sh`
- Uses: `docs/dgx-iperf3-10loss-debug-*`

- [ ] **Step 1: Copy rebuilt binary to the remote DGX host**

Run:

```bash
rtk ssh jack@169.254.59.196 'mkdir -p /home/jack/tcpquic-dgx-bin'
rtk scp build/bin/Release/tcpquic-proxy jack@169.254.59.196:/home/jack/tcpquic-dgx-bin/tcpquic-proxy
```

Expected:

```text
remote /home/jack/tcpquic-dgx-bin/tcpquic-proxy is updated
```

- [ ] **Step 2: Reproduce using the exact previous failure method**

Run:

```bash
rtk env ACCEPTANCE_MODE=1 SEQUENTIAL=1 DELAYS='10 20 30 40 50 60 70 80 90 150 300 500' ATTEMPTS=1 scripts/debug-dgx-iperf3-download-issue.sh
```

Expected:

```text
script exits 0
no trigger=iperf_rc_124
no trigger=low_throughput
no iperf3 TEST_END -> DISPLAY_RESULTS wait measured in hundreds of seconds
no remote tcpquic-proxy diag_stats line with old tunnel pending_bytes around hundreds of MB after a case ends
```

- [ ] **Step 3: If the script fails, preserve the failing result directory path**

Run:

```bash
rtk ls -td docs/dgx-iperf3-10loss-debug-* | head -3
```

Expected on failure:

```text
newest result directory contains iperf verbose logs, proxy stderr logs, trace logs, ss snapshots, qdisc snapshots
```

Do not mark the plan complete if this acceptance run fails.

## Task 11: Record post-fix validation

**Files:**
- Modify: `docs/superpowers/plans/2026-06-23-dgx-10loss-io-throughput-validation-cn.md`

- [ ] **Step 1: Append post-fix validation section**

Add a section:

````markdown
## 七、2026-06-24 tunnel 异常关闭修复后验证记录

### 1. 修复摘要

本轮修复 Linux relay tunnel 关闭/异常路径：当 QUIC stream shutdown complete 或 TCP/QUIC 明确错误发生时，如果 relay 仍保留 pending/outstanding 数据，则 reset 当前 tunnel 并释放 pending buffer，不关闭整个 QUIC connection。

### 2. 验收命令

```bash
rtk env ACCEPTANCE_MODE=1 SEQUENTIAL=1 DELAYS='10 20 30 40 50 60 70 80 90 150 300 500' ATTEMPTS=1 scripts/debug-dgx-iperf3-download-issue.sh
```

### 3. 验收结果

记录本次结果目录、是否出现 `iperf_rc_124`、是否出现低吞吐 trigger、是否仍存在 `TEST_END -> DISPLAY_RESULTS` 长时间等待、远端 proxy 是否仍出现旧 tunnel 百 MB 级 pending buffer 残留。
````

- [ ] **Step 2: Fill actual values from the DGX run**

Use:

```bash
rtk ls -td docs/dgx-iperf3-10loss-debug-* | head -1
rtk rg -n "trigger=|State set to 4-TEST_END|State set to 14-DISPLAY_RESULTS|pending_bytes=" docs/dgx-iperf3-10loss-debug-*/ -g '*.txt' -g '*.log'
```

Expected:

```text
the document contains concrete result directory and pass/fail evidence
```

## Self-Review

- Spec coverage: The plan covers stream shutdown with pending data, single-tunnel reset, TCP peer half-close while QUIC backlog pauses TCP read, TCP reset classification, TCP liveness socket options, EPOLLERR/SO_ERROR handling, local unit tests, DGX reproduction acceptance, and final documentation.
- Placeholder scan: No placeholder text remains. The DGX script task now uses the existing `cleanup_all` and `PRESERVE_SITE` mechanisms explicitly.
- Type consistency: All C++ symbols referenced are either existing (`TqLinuxRelayWorker`, `RelayState`, `FailRelayFatal`, `PendingTcpWriteBytes`, `TqLinuxRelayWorkerSnapshot`) or introduced in this plan (`AbortRelayAndRelease`, `DetachRelayStreamBinding`, `HasPendingAfterStreamShutdown`, `ConfigureRelayTcpLiveness`, `GetSocketError`, TCP config fields).
