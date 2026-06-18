# Relay Error Handling Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align Linux and Windows relay error handling so expected pressure becomes backpressure, unrecoverable faults clean up both TCP and QUIC stream consistently, and TCP reset/shutdown semantics are explicit.

**Architecture:** Add an explicit platform TCP reset-close primitive and a small shared relay classification vocabulary. Apply the vocabulary in Linux and Windows relay workers: expected queue/flow pressure pauses receive/read/write paths; fatal stream/TCP state transitions reset TCP and abort/close stream. Preserve graceful FIN paths as TCP send shutdown.

**Tech Stack:** C++17, MsQuic stream callbacks, Linux epoll/readv/writev, Windows IOCP/WSARecv/WSASend, existing unit-test executables in `src/CMakeLists.txt`.

---

## Current Gaps To Fix

- Linux TCP read hard error in `TqLinuxRelayWorker::DrainTcpReadable()` breaks out without recording errno or stopping/closing the relay.
- Windows IOCP / WSA hard errors collapse into `CloseRelay()` without a shared classification or TCP reset semantics.
- Linux QUIC receive event queue full currently causes `StreamShutdown(ABORT)` through `AbortRelayFromCallback()`, but queue full is a recoverable backpressure condition.
- Linux and Windows stream abort/shutdown events are not classified consistently. Windows currently drains on peer abort; Linux partially ignores `PEER_SEND_ABORTED` and hard-stops on `PEER_RECEIVE_ABORTED`.
- QUIC send failures are not separated into recoverable pressure/resource cases versus unrecoverable invalid stream/connection cases.

## File Structure

- Modify `src/platform/platform_socket.h`: declare explicit TCP reset-close helper.
- Modify `src/platform/platform_socket_posix.cpp`: implement reset close with `SO_LINGER {1,0}` then `close()`.
- Modify `src/platform/platform_socket_win.cpp`: implement reset close with `SO_LINGER {1,0}` then `closesocket()`.
- Create `src/tunnel/relay_error.h`: shared lightweight enums for relay pressure/fatal classification and close mode.
- Modify `src/CMakeLists.txt`: enable `TQ_UNIT_TESTING` for Linux relay IO tests when test-only send hooks are added.
- Modify `src/tunnel/linux_relay_worker.h`: add counters and test helpers for TCP read hard errors, receive queue backpressure, and fatal close mode.
- Modify `src/tunnel/linux_relay_worker.cpp`: implement backpressure for event-queue-full receive enqueue failure, fatal reset cleanup, TCP read hard error handling, and stream abort classification.
- Modify `src/tunnel/windows_relay_worker.h`: add counters, test helpers, and close-mode aware methods.
- Modify `src/tunnel/windows_relay_worker.cpp`: split graceful drain from fatal reset cleanup; classify IOCP, WSA, stream abort, and queue pressure paths.
- Modify `src/unittest/platform_socket_test.cpp`: compile and smoke-test `TqResetSocket()`.
- Modify `src/unittest/linux_relay_worker_queue_test.cpp`: verify queue full remains a recorded pressure condition.
- Modify `src/unittest/linux_relay_worker_io_test.cpp`: add Linux relay tests for queue-full backpressure, TCP read hard error, and stream abort reset classification.
- Modify `src/unittest/windows_relay_worker_test.cpp`: add Windows tests for peer abort fatal reset, graceful FIN drain, and pressure not closing relay.
- Modify `docs/relay-lifecycle-review.md`: replace principle-only pending items with links to the implementation behavior once tasks are complete.

---

### Task 1: Add Explicit TCP Reset Close Primitive

**Files:**
- Modify: `src/platform/platform_socket.h`
- Modify: `src/platform/platform_socket_posix.cpp`
- Modify: `src/platform/platform_socket_win.cpp`
- Test: `src/unittest/platform_socket_test.cpp`

- [ ] **Step 1: Add failing test coverage**

Append this block before the final `return 0;` in `src/unittest/platform_socket_test.cpp`:

```cpp
    TqSocketHandle resetPair[2]{TqInvalidSocket, TqInvalidSocket};
    assert(TqSocketPair(resetPair));
    assert(TqSocketValid(resetPair[0]));
    assert(TqSocketValid(resetPair[1]));
    TqResetSocket(resetPair[0]);
    resetPair[0] = TqInvalidSocket;
    TqCloseSocket(resetPair[1]);
    resetPair[1] = TqInvalidSocket;
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_platform_socket_test -j
```

Expected: compile failure mentioning `TqResetSocket` is not declared.

- [ ] **Step 3: Declare helper**

Add this declaration to `src/platform/platform_socket.h` after `TqCloseSocket`:

```cpp
void TqResetSocket(TqSocketHandle socket);
```

- [ ] **Step 4: Implement POSIX helper**

Add this function after `TqCloseSocket()` in `src/platform/platform_socket_posix.cpp`:

```cpp
void TqResetSocket(TqSocketHandle socket) {
    if (!TqSocketValid(socket)) {
        return;
    }
    linger resetLinger{};
    resetLinger.l_onoff = 1;
    resetLinger.l_linger = 0;
    (void)::setsockopt(socket, SOL_SOCKET, SO_LINGER, &resetLinger, sizeof(resetLinger));
    (void)::close(socket);
}
```

- [ ] **Step 5: Implement Windows helper**

Add this function after `TqCloseSocket()` in `src/platform/platform_socket_win.cpp`:

```cpp
void TqResetSocket(TqSocketHandle socket) {
    if (!TqSocketValid(socket)) {
        return;
    }
    linger resetLinger{};
    resetLinger.l_onoff = 1;
    resetLinger.l_linger = 0;
    (void)::setsockopt(
        socket,
        SOL_SOCKET,
        SO_LINGER,
        reinterpret_cast<const char*>(&resetLinger),
        sizeof(resetLinger));
    (void)::closesocket(socket);
}
```

- [ ] **Step 6: Run platform socket test**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_platform_socket_test -j
rtk ./build-glibc/src/tcpquic_platform_socket_test
```

Expected: build succeeds and test exits `0`.

- [ ] **Step 7: Commit**

```bash
rtk git add src/platform/platform_socket.h src/platform/platform_socket_posix.cpp src/platform/platform_socket_win.cpp src/unittest/platform_socket_test.cpp
rtk git commit -m "net: add explicit tcp reset close helper"
```

---

### Task 2: Add Shared Relay Error Classification Vocabulary

**Files:**
- Create: `src/tunnel/relay_error.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: compile-only through worker tests

- [ ] **Step 1: Create shared header**

Create `src/tunnel/relay_error.h`:

```cpp
#pragma once

enum class TqRelayCloseMode {
    GracefulDrain,
    AbortReset,
};

enum class TqRelayFailureClass {
    Backpressure,
    Fatal,
};

inline const char* TqRelayCloseModeName(TqRelayCloseMode mode) {
    switch (mode) {
    case TqRelayCloseMode::GracefulDrain:
        return "graceful_drain";
    case TqRelayCloseMode::AbortReset:
        return "abort_reset";
    }
    return "unknown";
}
```

- [ ] **Step 2: Include shared header in relay workers**

Add to both `src/tunnel/linux_relay_worker.h` and `src/tunnel/windows_relay_worker.h` with the other local includes:

```cpp
#include "relay_error.h"
```

- [ ] **Step 3: Build worker tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j
```

Expected: build succeeds.

On Windows-capable build host, also run:

```bash
rtk cmake --build build-win --target tcpquic_windows_relay_worker_test -j
```

Expected: build succeeds. If no Windows build tree exists, record that Windows compile verification was not run.

- [ ] **Step 4: Commit**

```bash
rtk git add src/tunnel/relay_error.h src/tunnel/linux_relay_worker.h src/tunnel/windows_relay_worker.h
rtk git commit -m "relay: add shared error classification vocabulary"
```

---

### Task 3: Fix Linux TCP Read Hard Error Handling

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add failing Linux TCP read hard error test**

Append this test block near the other TCP event tests in `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 240;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 241;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 242;
        }

        ::close(fds[0]);
        if (!worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLIN | EPOLLERR)) {
            worker.Stop();
            ::close(fds[1]);
            return 243;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            snapshot.TcpReadHardErrors != 1 ||
            snapshot.LastTcpReadErrno == 0) {
            std::fprintf(stderr,
                "expected tcp read hard error to stop relay, stop=%d errors=%llu errno=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.TcpReadHardErrors),
                static_cast<unsigned long long>(snapshot.LastTcpReadErrno));
            worker.Stop();
            ::close(fds[1]);
            return 244;
        }

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Add snapshot counters**

In `src/tunnel/linux_relay_worker.h`, add these fields to `TqLinuxRelayWorkerSnapshot` after `TcpWriteHardErrors`:

```cpp
    uint64_t TcpReadHardErrors{0};
    uint64_t LastTcpReadErrno{0};
```

Add `TcpReadHard` to `RelayErrorKind`:

```cpp
        TcpWriteHard,
        TcpReadHard
```

Add private atomics near `TcpWriteHardErrors`:

```cpp
    std::atomic<uint64_t> TcpReadHardErrors{0};
    std::atomic<uint64_t> LastTcpReadErrno{0};
```

- [ ] **Step 3: Record Linux TCP read hard errors**

In `TqLinuxRelayWorker::RecordError()` in `src/tunnel/linux_relay_worker.cpp`, add:

```cpp
    case RelayErrorKind::TcpReadHard:
        TcpReadHardErrors.fetch_add(1);
        break;
```

In `TqLinuxRelayWorker::DrainTcpReadable()`, replace the final hard-error `break;` after the `EAGAIN/EWOULDBLOCK` branch with:

```cpp
        RecordError(RelayErrorKind::TcpReadHard);
        const uint64_t savedErrno = static_cast<uint64_t>(errno);
        LastTcpReadErrno.store(savedErrno);
        SetRelayStop(relay, "tcp_read_hard_error");
        ArmTcpReadable(relay, false);
        break;
```

In `Snapshot()`, populate:

```cpp
    snapshot.TcpReadHardErrors = TcpReadHardErrors.load();
    snapshot.LastTcpReadErrno = LastTcpReadErrno.load();
```

- [ ] **Step 4: Run Linux relay IO test**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_linux_relay_worker_io_test -j
rtk ./build-glibc/src/tcpquic_linux_relay_worker_io_test
```

Expected: build succeeds and test exits `0`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "relay: handle linux tcp read hard errors"
```

---

### Task 4: Make Linux QUIC Receive Queue Full A Backpressure Condition

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

- [ ] **Step 1: Add failing test for queue-full receive backpressure**

Append this block to `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 2;
        config.EventBudget = 1;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 250;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = -1;
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        registration.SinkQuicReceives = true;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            return 251;
        }

        uint8_t first[] = {1};
        uint8_t second[] = {2};
        QUIC_BUFFER buffer{};
        QUIC_STREAM_EVENT receive{};
        receive.Type = QUIC_STREAM_EVENT_RECEIVE;
        receive.RECEIVE.BufferCount = 1;
        receive.RECEIVE.Buffers = &buffer;

        buffer.Buffer = first;
        buffer.Length = sizeof(first);
        if (worker.DispatchStreamEventForTest(fakeStream, &receive) != QUIC_STATUS_PENDING) {
            worker.Stop();
            return 252;
        }

        buffer.Buffer = second;
        buffer.Length = sizeof(second);
        if (worker.DispatchStreamEventForTest(fakeStream, &receive) != QUIC_STATUS_PENDING) {
            worker.Stop();
            return 253;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (handle.Stop.load(std::memory_order_acquire) ||
            snapshot.EventQueueFullErrors == 0 ||
            snapshot.QuicReceiveViewBackpressureQueued == 0) {
            std::fprintf(stderr,
                "expected queue full to be backpressure without stop, stop=%d queue_full=%llu backpressure=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.EventQueueFullErrors),
                static_cast<unsigned long long>(snapshot.QuicReceiveViewBackpressureQueued));
            worker.Stop();
            return 254;
        }

        worker.Stop();
    }
```

- [ ] **Step 2: Add byte-preserving backpressure storage and counter**

In `TqLinuxRelayWorkerSnapshot` add near `QuicReceiveViewEnqueueFailures`:

```cpp
    uint64_t QuicReceiveViewBackpressureQueued{0};
```

In `TqLinuxRelayWorker` private atomics add:

```cpp
    std::atomic<uint64_t> QuicReceiveViewBackpressureQueued{0};
```

In `RelayState`, add a callback-side overflow queue:

```cpp
    std::mutex CallbackPendingQuicReceiveLock;
    std::deque<std::shared_ptr<TqPendingQuicReceive>> CallbackPendingQuicReceives;
```

Populate `QuicReceiveViewBackpressureQueued` in `Snapshot()` and aggregate it in runtime totals if totals include nearby receive counters.

- [ ] **Step 3: Classify event queue full as byte-preserving backpressure**

Change `QueueDeferredQuicReceiveFromOffset()` so `Enqueue()` failure moves the receive view to the per-relay callback backlog, pauses QUIC receive, wakes the owner worker, and returns true instead of false. Keep a local `shared_ptr` before moving the event into the queue attempt:

```cpp
    auto receiveView = event.ReceiveView;
    if (!Enqueue(std::move(event))) {
        QuicReceiveViewBackpressureQueued.fetch_add(1);
        {
            std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
            relay->CallbackPendingQuicReceives.push_back(std::move(receiveView));
        }
        MaybePauseQuicReceive(relay);
        Wake();
        return true;
    }
```

Do not increment `QuicReceiveViewFailures` or `QuicReceiveViewEnqueueFailures` for this path. Those counters stay reserved for unrecoverable receive-view failures.

- [ ] **Step 4: Drain callback-side backlog on the owner worker**

Add a private helper declaration:

```cpp
    void DrainCallbackPendingQuicReceives(RelayState* relay);
```

Implement it in `src/tunnel/linux_relay_worker.cpp`:

```cpp
void TqLinuxRelayWorker::DrainCallbackPendingQuicReceives(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    std::deque<std::shared_ptr<TqPendingQuicReceive>> pending;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        pending.swap(relay->CallbackPendingQuicReceives);
    }
    while (!pending.empty()) {
        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::QuicReceiveView;
        event.RelayId = relay->Id;
        event.ReceiveView = std::move(pending.front());
        pending.pop_front();
        if (event.ReceiveView != nullptr) {
            event.TotalLength = event.ReceiveView->TotalLength;
            event.Fin = event.ReceiveView->Fin;
        }
        ProcessQuicReceiveViewEvent(event);
    }
    MaybeResumeQuicReceive(relay);
}
```

Call `DrainCallbackPendingQuicReceives(relay.get())` for each active relay after normal queue draining in `DrainEvents()`. This preserves MsQuic receive ownership, avoids byte loss, and lets the existing `ProcessQuicReceiveViewEvent()` path write bytes to TCP and call `ReceiveComplete()`.

- [ ] **Step 5: Run Linux queue and IO tests**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j
rtk ./build-glibc/src/tcpquic_linux_relay_worker_queue_test
rtk ./build-glibc/src/tcpquic_linux_relay_worker_io_test
```

Expected: both tests exit `0`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/unittest/linux_relay_worker_queue_test.cpp
rtk git commit -m "relay: treat linux receive queue full as backpressure"
```

---

### Task 5: Add Linux Fatal Relay Cleanup With TCP Reset

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add failing stream abort classification tests**

Add two tests to `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 260;
        }
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 261;
        }
        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 262;
        }

        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &aborted))) {
            worker.Stop();
            ::close(fds[1]);
            return 263;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets == 0) {
            std::fprintf(stderr, "expected peer send abort to fatal reset, stop=%d resets=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 264;
        }
        worker.Stop();
        ::close(fds[1]);
    }
```

Repeat the same block with `aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED` and distinct return codes `265` through `269`.

- [ ] **Step 2: Add fatal reset counter and helper declaration**

In `TqLinuxRelayWorkerSnapshot` add:

```cpp
    uint64_t FatalRelayResets{0};
```

In `TqLinuxRelayWorker` private atomics add:

```cpp
    std::atomic<uint64_t> FatalRelayResets{0};
```

Declare private helper:

```cpp
    void FailRelayFatal(RelayState* relay, const char* trigger, bool abortStream);
```

- [ ] **Step 3: Implement fatal helper**

Add to `src/tunnel/linux_relay_worker.cpp`:

```cpp
void TqLinuxRelayWorker::FailRelayFatal(RelayState* relay, const char* trigger, bool abortStream) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    FatalRelayResets.fetch_add(1);
    relay->Closing = true;
    SetRelayStop(relay, trigger);
    if (EpollFd >= 0 && relay->TcpFd >= 0) {
        (void)::epoll_ctl(EpollFd, EPOLL_CTL_DEL, relay->TcpFd, nullptr);
        TqResetSocket(relay->TcpFd);
        relay->TcpFd = -1;
    }
    if (abortStream && relay->Stream != nullptr && relay->Stream->Handle != nullptr) {
        (void)relay->Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
}
```

Include `platform_socket.h` if not already visible through headers.

- [ ] **Step 4: Use fatal helper in stream abort events**

In `OnStreamEventWithBinding()`:

For `QUIC_STREAM_EVENT_PEER_SEND_ABORTED`, replace the trace-only return with:

```cpp
        FailRelayFatal(relay, "stream_peer_send_aborted", false);
        return QUIC_STATUS_SUCCESS;
```

For `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED`, replace manual closing/enqueue shutdown with:

```cpp
        binding->Closing.store(true, std::memory_order_release);
        binding->Relay.store(nullptr, std::memory_order_release);
        if (stream != nullptr && stream->Context == binding) {
            stream->Callback = MsQuicStream::NoOpCallback;
            stream->Context = nullptr;
        }
        relay->Stream = nullptr;
        FailRelayFatal(relay, "stream_peer_receive_aborted", false);
        return QUIC_STATUS_SUCCESS;
```

- [ ] **Step 5: Populate snapshot and run tests**

Populate `snapshot.FatalRelayResets = FatalRelayResets.load();`.

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_linux_relay_worker_io_test -j
rtk ./build-glibc/src/tcpquic_linux_relay_worker_io_test
```

Expected: test exits `0`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "relay: reset linux tcp on fatal stream abort"
```

---

### Task 6: Split Windows Graceful Drain From Fatal Reset Cleanup

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add Windows snapshot counters**

In `TqWindowsRelayWorkerSnapshot`, add:

```cpp
    uint64_t FatalRelayResets{0};
    uint64_t GracefulRelayDrains{0};
    uint64_t TcpHardErrors{0};
```

In `TqWindowsRelayWorker` private atomics add:

```cpp
    std::atomic<uint64_t> FatalRelayResets_{0};
    std::atomic<uint64_t> GracefulRelayDrains_{0};
    std::atomic<uint64_t> TcpHardErrors_{0};
```

Populate them in `Snapshot()`.

- [ ] **Step 2: Add close-mode methods**

In `src/tunnel/windows_relay_worker.h`, replace:

```cpp
    void CloseRelay(const std::shared_ptr<RelayContext>& relay);
    bool CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay);
```

with:

```cpp
    void CloseRelay(const std::shared_ptr<RelayContext>& relay, TqRelayCloseMode mode);
    bool CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay);
    void FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char* reason);
```

- [ ] **Step 3: Implement fatal reset cleanup**

Change `CloseRelay()` implementation to accept a mode:

```cpp
void TqWindowsRelayWorker::CloseRelay(const std::shared_ptr<RelayContext>& relay, TqRelayCloseMode mode) {
    if (!relay || relay->Closing.exchange(true)) {
        return;
    }
    std::shared_ptr<CallbackBinding> bindingKeepAlive;
    if (mode == TqRelayCloseMode::AbortReset) {
        FatalRelayResets_.fetch_add(1, std::memory_order_relaxed);
        TqResetSocket(relay->TcpFd);
    } else {
        TqCloseSocket(relay->TcpFd);
    }
    relay->TcpFd = TqInvalidSocket;
    if (relay->Callback) {
        bindingKeepAlive = relay->Callback;
        relay->Callback->Closing.store(true, std::memory_order_release);
        relay->Callback->RelayHint.store(nullptr, std::memory_order_release);
    }
    CompleteAllPendingQuicReceives(relay);
    if (relay->Stream != nullptr && relay->Stream->Context == relay->Callback.get()) {
        relay->Stream->Callback = MsQuicStream::NoOpCallback;
        relay->Stream->Context = nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
        if (bindingKeepAlive) {
            RetiredCallbacks_.push_back(std::move(bindingKeepAlive));
        }
    }
    PruneRetiredCallbacks(true);
    if (relay->PublicHandle != nullptr) {
        relay->PublicHandle->Stop.store(true);
    }
}
```

Add:

```cpp
void TqWindowsRelayWorker::FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char*) {
    CloseRelay(relay, TqRelayCloseMode::AbortReset);
}
```

Update existing intentional shutdown calls to `CloseRelay(relay, TqRelayCloseMode::GracefulDrain)` unless the call site is a hard error.

- [ ] **Step 4: Classify IOCP/WSA hard errors as fatal**

In `Run()`, change the `!ok` branch:

```cpp
        if (!ok) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            TcpHardErrors_.fetch_add(1, std::memory_order_relaxed);
            FailRelayFatal(op->Relay, "iocp_completion_error");
            continue;
        }
```

For immediate WSARecv/WSASend failures that are not `WSA_IO_PENDING`, increment `TcpHardErrors_` and call `FailRelayFatal()` instead of plain `CloseRelay()`.

- [ ] **Step 5: Classify stream abort events as fatal**

In `TqWindowsRelayWorker::StreamCallback()` replace:

```cpp
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        relay->CloseAfterDrained.store(true);
        (void)worker->CloseRelayIfDrained(relay);
    }
```

with:

```cpp
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        worker->FailRelayFatal(relay, event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
            ? "stream_peer_send_aborted"
            : "stream_peer_receive_aborted");
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        if (event->SHUTDOWN_COMPLETE.ConnectionShutdown) {
            worker->FailRelayFatal(relay, "stream_connection_shutdown");
            return QUIC_STATUS_SUCCESS;
        }
        relay->CloseAfterDrained.store(true);
        (void)worker->CloseRelayIfDrained(relay);
    }
```

- [ ] **Step 6: Keep graceful FIN as drain**

Keep `QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN` as success/no-op. Keep receive FIN handling in `FinishReceiveView()` as `TqShutdownSend()` after pending data is written.

Increment `GracefulRelayDrains_` inside `CloseRelayIfDrained()` when it actually performs `TqShutdownSend()` and sets `PublicHandle->Stop`.

- [ ] **Step 7: Add Windows tests**

Add tests to `src/unittest/windows_relay_worker_test.cpp`:

```cpp
    {
        TqWindowsRelayWorker receiveWorker;
        assert(receiveWorker.Start());
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            return 150;
        }
        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
        (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &aborted);
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets == 0) {
            receiveWorker.Stop();
            return 151;
        }
        receiveWorker.Stop();
    }
```

Add a parallel test for `QUIC_STREAM_EVENT_PEER_SEND_ABORTED`.

- [ ] **Step 8: Build Windows relay test**

On Windows-capable build host:

```bash
rtk cmake --build build-win --target tcpquic_windows_relay_worker_test -j
rtk ./build-win/src/tcpquic_windows_relay_worker_test.exe
```

Expected: build succeeds and test exits `0`. If only Linux is available, document that Windows execution was not run and at least verify the Linux build is unaffected:

```bash
rtk cmake --build build-glibc --target tcpquic_linux_relay_worker_io_test -j
```

- [ ] **Step 9: Commit**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "relay: align windows fatal and graceful close semantics"
```

---

### Task 7: Classify QUIC Send Failures

**Files:**
- Modify: `src/CMakeLists.txt`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Enable Linux test-only send hook compilation**

In `src/CMakeLists.txt`, update `tcpquic_linux_relay_worker_io_test` compile definitions:

```cmake
target_compile_definitions(tcpquic_linux_relay_worker_io_test PRIVATE ${TCPQUIC_TEST_DEFS} TQ_UNIT_TESTING=1)
```

- [ ] **Step 2: Add send failure counters and Linux test hook**

Add to Linux and Windows snapshots:

```cpp
    uint64_t QuicSendBackpressureEvents{0};
    uint64_t QuicSendFatalErrors{0};
```

Add matching atomics in both workers.

In `src/tunnel/linux_relay_worker.h`, under `#if defined(TQ_UNIT_TESTING)`, declare:

```cpp
using TqLinuxRelayStreamSendForTest = QUIC_STATUS (*)(
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* context);

void TqLinuxRelaySetStreamSendForTest(TqLinuxRelayStreamSendForTest sendFn);
```

In `src/tunnel/linux_relay_worker.cpp`, add:

```cpp
#if defined(TQ_UNIT_TESTING)
namespace {
TqLinuxRelayStreamSendForTest g_linuxRelayStreamSendForTest = nullptr;
}

void TqLinuxRelaySetStreamSendForTest(TqLinuxRelayStreamSendForTest sendFn) {
    g_linuxRelayStreamSendForTest = sendFn;
}
#endif

QUIC_STATUS TqLinuxRelayStreamSend(
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* context) {
#if defined(TQ_UNIT_TESTING)
    if (g_linuxRelayStreamSendForTest != nullptr) {
        return g_linuxRelayStreamSendForTest(stream, buffers, bufferCount, flags, context);
    }
#endif
    return stream->Send(buffers, bufferCount, flags, context);
}
```

Replace direct `stream->Send(...)` calls inside `SubmitTcpBatchToQuic()` with `TqLinuxRelayStreamSend(stream, ...)`.

- [ ] **Step 3: Linux classify `SubmitTcpBatchToQuic()` failures**

In `src/tunnel/linux_relay_worker.cpp`:

- stream null / handle null -> increment `QuicSendFatalErrors`, return false, and let caller fatal close.
- operation allocation failure -> increment `QuicSendBackpressureEvents`, pause TCP read with `ArmTcpReadable(relay, false)`, return true so relay remains alive.
- buffer too large -> if caused by local batching, split before send; if still too large after split, increment `QuicSendBackpressureEvents` and pause read, not fatal.
- `stream->Send()` failed with invalid state / aborted stream status -> increment `QuicSendFatalErrors`, return false.
- transient resource status -> increment `QuicSendBackpressureEvents`, pause TCP read, return true.

Add a helper to avoid status-specific logic scattered inline:

```cpp
bool TqLinuxRelayWorker::IsQuicSendBackpressureStatus(QUIC_STATUS status) const {
    return status == QUIC_STATUS_OUT_OF_MEMORY || status == QUIC_STATUS_BUFFER_TOO_SMALL;
}
```

Use constants present in `third_party/msquic/src/inc/msquic.h`; for this repository, `QUIC_STATUS_OUT_OF_MEMORY` is the required transient resource status. Do not invent non-existent constants.

- [ ] **Step 4: Windows classify `stream->Send()` failures**

In every Windows `relay->Stream->Send()` failure path:

- If the stream/handle is invalid or relay is closing, call `FailRelayFatal()`.
- If the status is a transient resource/backpressure status, keep relay alive, pause the corresponding producer path, and increment `QuicSendBackpressureEvents_`.
- If status is fatal, increment `QuicSendFatalErrors_` and call `FailRelayFatal()`.

- [ ] **Step 5: Add tests for Linux send backpressure and fatal send**

Add a fake send status near the top of `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
namespace {
QUIC_STATUS g_LinuxFakeStreamSendStatus = QUIC_STATUS_SUCCESS;

QUIC_STATUS LinuxFakeStreamSend(
    MsQuicStream*,
    const QUIC_BUFFER*,
    uint32_t,
    QUIC_SEND_FLAGS,
    void*) {
    return g_LinuxFakeStreamSendStatus;
}
} // namespace
```

In the test body, call:

```cpp
TqLinuxRelaySetStreamSendForTest(LinuxFakeStreamSend);
g_LinuxFakeStreamSendStatus = QUIC_STATUS_OUT_OF_MEMORY;
```

Drive TCP readable data into a registered relay with `EnableQuicSends=true`. Expected transient assertions:

```cpp
if (snapshot.QuicSendBackpressureEvents == 0 || handle.Stop.load(std::memory_order_acquire)) {
    TqLinuxRelaySetStreamSendForTest(nullptr);
    return 280;
}
```

Then set a fatal status that this repository already treats as invalid state, for example:

```cpp
g_LinuxFakeStreamSendStatus = QUIC_STATUS_INVALID_STATE;
```

Expected fatal assertions:

```cpp
if (snapshot.QuicSendFatalErrors == 0 || !handle.Stop.load(std::memory_order_acquire)) {
    TqLinuxRelaySetStreamSendForTest(nullptr);
    return 281;
}
```

- [ ] **Step 6: Add Windows fake send failure tests**

Extend `FakeStreamSend()` in `src/unittest/windows_relay_worker_test.cpp` with a global status:

```cpp
QUIC_STATUS g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
```

Return it from `FakeStreamSend()`. Add tests for transient status and fatal status:

```cpp
g_FakeStreamSendStatus = QUIC_STATUS_OUT_OF_MEMORY;
```

Expected transient: relay remains active, backpressure counter increments, no fatal reset.

Expected fatal: relay stop true, fatal counter increments, TCP reset close path used.

- [ ] **Step 7: Run relay tests**

Run Linux:

```bash
rtk cmake --build build-glibc --target tcpquic_linux_relay_worker_io_test -j
rtk ./build-glibc/src/tcpquic_linux_relay_worker_io_test
```

Run Windows where available:

```bash
rtk cmake --build build-win --target tcpquic_windows_relay_worker_test -j
rtk ./build-win/src/tcpquic_windows_relay_worker_test.exe
```

- [ ] **Step 8: Commit**

```bash
rtk git add src/CMakeLists.txt src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "relay: classify quic send pressure and fatal errors"
```

---

### Task 8: Update Lifecycle Documentation

**Files:**
- Modify: `docs/relay-lifecycle-review.md`

- [ ] **Step 1: Replace principle-only pending wording with implemented behavior**

Update section `## 9. TODO` by moving completed items into a new `## 9. Implemented Error Semantics` section:

```markdown
## 9. Implemented Error Semantics

- TCP hard errors are fatal and reset/close both sides of the relay.
- Backpressure/resource pressure pauses producers or receive paths and keeps the relay alive.
- Stream peer aborts are fatal and reset the paired TCP connection.
- Graceful FIN drains pending bytes then maps to TCP send shutdown.
```

Keep any not-yet-implemented items under `## 10. Remaining Follow-ups`.

- [ ] **Step 2: Verify docs**

Run:

```bash
rtk rg -n "Implemented Error Semantics|Remaining Follow-ups|TCP hard errors|Backpressure" docs/relay-lifecycle-review.md
```

Expected: all headings and behavior bullets are present.

- [ ] **Step 3: Commit**

```bash
rtk git add docs/relay-lifecycle-review.md
rtk git commit -m "docs: update relay lifecycle error semantics"
```

---

## Verification Matrix

Run after all tasks:

```bash
rtk cmake --build build-glibc --target \
  tcpquic_platform_socket_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_tunnel_test \
  -j
rtk ./build-glibc/src/tcpquic_platform_socket_test
rtk ./build-glibc/src/tcpquic_linux_relay_worker_queue_test
rtk ./build-glibc/src/tcpquic_linux_relay_worker_io_test
rtk ./build-glibc/src/tcpquic_tunnel_test
```

On Windows-capable build host:

```bash
rtk cmake --build build-win --target tcpquic_platform_socket_test tcpquic_windows_relay_worker_test -j
rtk ./build-win/src/tcpquic_platform_socket_test.exe
rtk ./build-win/src/tcpquic_windows_relay_worker_test.exe
```

Expected:

- Linux tests exit `0`.
- Windows tests exit `0` on Windows build host.
- No expected pressure test increments fatal counters.
- Fatal stream/TCP tests set relay stop and fatal/reset counters.
- Graceful FIN tests do not increment fatal counters.

## Self-Review

- Spec coverage: all current `docs/relay-lifecycle-review.md` pending items are covered by Tasks 1 through 8.
- Sanity scan: no unresolved marker, empty pending item, or unspecified file path remains.
- Type consistency: shared names are `TqRelayCloseMode`, `TqRelayFailureClass`, `TqResetSocket`, `FatalRelayResets`, `QuicSendBackpressureEvents`, and `QuicSendFatalErrors`.
- Risk note: Task 4 intentionally preserves receive bytes in a per-relay callback backlog before returning `QUIC_STATUS_PENDING`; implementation must keep ownership valid until `ReceiveComplete()` is called by the owner worker.
