# macOS kqueue Relay Platform Design

Status: approved design
Date: 2026-06-18
Scope: native macOS/Darwin relay backend for `tcpquic-proxy` using `kqueue`

## Context

`tcpquic-proxy` now has production relay backends for Linux and Windows. Linux uses a readiness-based `epoll` backend with `eventfd`, worker threads, batched TCP I/O, MsQuic pending receive completion, and relay backpressure. Windows uses a completion-based IOCP backend with Winsock overlapped I/O.

On macOS, the project currently builds shared POSIX socket code but does not have a production relay backend. `src/CMakeLists.txt` includes `platform/platform_socket_posix.cpp` for non-Windows platforms, but only includes `tunnel/linux_relay_worker.cpp`, `tunnel/relay_alloc.cpp`, and `tunnel/relay_buffer.cpp` when `CMAKE_SYSTEM_NAME STREQUAL "Linux"`. In `src/tunnel/relay.cpp`, non-Linux and non-Windows platforms fall through to a backend that returns failure from `TqRelayStart`.

The selected direction is a native macOS relay backend implemented with `kqueue`, while preserving the existing Linux and Windows implementations. macOS should reach feature parity with the Linux relay fast path rather than a minimal compile-only or low-performance fallback.

## Goals

- Support production TCP-over-QUIC relay on macOS/Darwin.
- Use `kqueue` for TCP socket readiness and worker wakeups.
- Preserve Linux `epoll` and Windows IOCP behavior.
- Match Linux relay fast-path semantics for:
  - multi-worker runtime;
  - one owner worker per relay;
  - TCP to QUIC batched reads, optional zstd compression, QUIC send accounting, and send backpressure;
  - QUIC to TCP pending receive handling, optional zstd decompression, TCP write queueing, and receive completion;
  - QUIC receive pause/resume under TCP write pressure;
  - relay stop, drain, close, and retired callback lifetime handling;
  - runtime and worker metrics snapshots.
- Keep MsQuic callbacks lightweight. They should enqueue work to the owner worker and must not perform blocking or heavy TCP I/O.
- Verify macOS behavior with Darwin-only unit tests and existing end-to-end proxy tests adapted where necessary.

## Non-Goals

- Replacing `TqLinuxRelayWorker` with a new cross-platform readiness framework in the first macOS implementation.
- Rewriting Windows IOCP to share code with readiness-based backends.
- Introducing libevent, libuv, Boost.Asio, or any other event-loop dependency.
- Supporting non-Darwin BSD platforms in the first implementation.
- Changing the external CLI, config file format, QUIC protocol, SOCKS5 behavior, HTTP CONNECT behavior, ACL behavior, or certificate handling.

## Selected Approach

Add a platform-specific Darwin relay backend beside the existing Linux and Windows backends.

The new backend should use the same public relay boundary as other platforms:

- `TqRelayStart`
- `TqRelayStartQuicReceiveSink`
- `TqRelayStop`
- `TqRelayHandle`
- `TqRelayBackendType`

The Darwin backend should follow the Linux worker state machine closely because `kqueue` and `epoll` are both readiness-based. The first implementation should keep Linux code stable and localize macOS changes to Darwin-specific files plus narrow backend-selection changes. Shared abstractions can be extracted later after macOS behavior is verified.

## Files and Build Integration

Add:

- `src/tunnel/darwin_relay_worker.h`
- `src/tunnel/darwin_relay_worker.cpp`
- `src/tunnel/darwin_relay_event_queue.h`
- `src/unittest/darwin_relay_worker_queue_test.cpp`
- `src/unittest/darwin_relay_worker_io_test.cpp`

Modify:

- `src/tunnel/relay.h`
- `src/tunnel/relay.cpp`
- `src/CMakeLists.txt`
- `src/config/tuning.h` and related tuning parsing only if Darwin-specific overrides are added
- metrics snapshot code only where a platform backend selector needs to expose Darwin counters

`src/CMakeLists.txt` should add Darwin relay sources when `CMAKE_SYSTEM_NAME STREQUAL "Darwin"`:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_PROXY_SOURCES
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
    )
endif()
```

macOS should continue to use `platform/platform_socket_posix.cpp` for the socket abstraction.

Darwin-only tests should be guarded with `CMAKE_SYSTEM_NAME STREQUAL "Darwin"` so Linux and Windows builds do not include `<sys/event.h>` or other Darwin-only APIs.

## Public Relay Backend Selection

`TqRelayBackendType` should gain a Darwin backend:

```cpp
enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
    DarwinWorker,
};
```

`TqRelayHandle` should gain Darwin ownership fields:

```cpp
struct TqDarwinRelayWorker;

struct TqRelayHandle {
    // existing fields stay unchanged
    TqDarwinRelayWorker* DarwinWorker = nullptr;
    uint64_t DarwinRelayId = 0;
};
```

`src/tunnel/relay.cpp` should select backends at compile time:

- `_WIN32`: `TqWindowsRelayRuntime`
- `__linux__`: `TqLinuxRelayRuntime`
- `__APPLE__`: `TqDarwinRelayRuntime`
- other platforms: current unsupported fallback

`TqRelayStop` should dispatch to the Darwin worker when `handle->Backend == TqRelayBackendType::DarwinWorker`.

`TqRelayStartQuicReceiveSink` can remain Linux-only unless a Darwin test needs the same sink path. If Darwin tests need it, implement a Darwin-equivalent test-only registration path with the same callback and pending receive semantics as the production worker.

## Darwin Runtime and Worker Model

Add two top-level backend types:

- `TqDarwinRelayRuntime`
- `TqDarwinRelayWorker`

`TqDarwinRelayRuntime` owns a fixed set of workers. It provides:

- `Instance()`
- `Start(const TqRelayRuntimeConfig&)`
- `Stop()`
- `RegisterRelay(const TqRelayRegistration&)`
- `StopRelay(TqRelayHandle*)`
- `Snapshot()`

The runtime should assign relays round-robin, matching the existing Linux and Windows model.

Each `TqDarwinRelayWorker` owns:

- one `kqueue` fd;
- one worker thread;
- one internal event queue for MsQuic callback events and control events;
- a relay table keyed by relay id;
- a retired relay/callback list for safe deferred destruction;
- worker-local metrics counters;
- runtime tuning values.

All TCP socket I/O for a relay happens on its owner worker. MsQuic stream callbacks may run on MsQuic threads, but they only capture the minimum receive/send-complete metadata, enqueue a worker event, trigger the worker wakeup, and return.

## kqueue Event Model

### Worker Startup

`TqDarwinRelayWorker::Start` should:

1. Create a kqueue with `kqueue()`.
2. Register an `EVFILT_USER` event with a stable worker-local identifier.
3. Set `Running` to true.
4. Start `std::thread(&TqDarwinRelayWorker::Run, this)`.

### Worker Wakeup

Use `EVFILT_USER` with `NOTE_TRIGGER` as the Darwin replacement for Linux `eventfd`.

When a MsQuic callback or another thread enqueues an internal event, it should call `Wake()`:

1. Push `TqDarwinRelayEvent` into the bounded queue.
2. Trigger the worker's `EVFILT_USER` event with `kevent` and `NOTE_TRIGGER`.

The worker handles the user event by calling `DrainEvents(Config.EventBudget)`.

A pipe or socketpair wakeup is not preferred because `EVFILT_USER` avoids extra file descriptors and avoids drain-read bookkeeping.

### TCP Socket Registration

When a relay is registered:

1. Set the TCP socket nonblocking with `TqSetNonBlocking`.
2. Set `SO_NOSIGPIPE` on the socket when available.
3. Create a relay state object with compressor/decompressor instances and per-direction queues.
4. Register `EVFILT_READ` for the socket as enabled.
5. Register `EVFILT_WRITE` as disabled or add it lazily on first pending write.
6. Set the MsQuic stream callback to the Darwin worker callback binding.
7. Fill `TqRelayHandle` with `Backend = DarwinWorker`, `DarwinWorker`, and `DarwinRelayId`.

`kevent.udata` should identify the relay. The preferred representation is a relay id rather than a raw pointer, because the relay table and retired list can then reject stale events after close. If a pointer is used for performance, the worker must delete filters before retirement and preserve object lifetime until all queued events are drained.

### Readiness Mapping

Darwin readiness maps to Linux readiness as follows:

| Linux epoll | Darwin kqueue |
|---|---|
| `EPOLLIN` | `EVFILT_READ` |
| `EPOLLOUT` | `EVFILT_WRITE` |
| `EPOLLRDHUP` / `EPOLLHUP` | `EV_EOF` on read/write filters |
| `EPOLLERR` | `EV_ERROR` |
| `eventfd` readable | `EVFILT_USER` triggered |

`EV_EOF` should not by itself discard data or immediately close the relay. The worker should still run the normal read drain path and treat `readv == 0` as confirmed TCP EOF. `EV_ERROR` should mark the relay as failed and enter close/drain handling.

### Interest Updates

Keep the Linux-style armed flags:

- `TcpReadArmed`
- `TcpWriteArmed`

`UpdateTcpInterest` should enable or disable filters using `EV_ADD | EV_ENABLE` and `EV_DISABLE`. Repeated `EV_DELETE` / `EV_ADD` should be avoided except during final relay close.

`EVFILT_WRITE` should only be enabled when the relay has pending TCP writes or deferred QUIC receives that may become writable. Once the write queue is empty and no deferred receive can be advanced, `EVFILT_WRITE` should be disabled to avoid a busy loop.

## Internal Event Queue

Add `TqDarwinRelayEventQueue` with the same semantics as the Linux queue:

- bounded capacity;
- power-of-two ring storage;
- multi-producer enqueue from MsQuic callback/control threads;
- single worker drain path;
- approximate size and capacity metrics.

Event types should include:

- `TcpWritable`
- `QuicReceive`
- `QuicReceiveView`
- `QuicSendComplete`
- `QuicIdealSendBuffer`
- `Shutdown`
- `StopRelay`
- `TestMarker` for tests

The queue should carry pending QUIC receive metadata equivalent to the Linux backend so the Darwin worker can preserve `QUIC_STATUS_PENDING` receive ownership and batch completion.

The first macOS implementation may keep `darwin_relay_event_queue.h` separate from `linux_relay_event_queue.h`. After Darwin behavior is stable, the two queues can be folded into a platform-neutral `relay_event_queue.h` in a separate refactor.

## TCP to QUIC Path

The Darwin TCP-to-QUIC path should mirror Linux:

1. `kevent` returns `EVFILT_READ` or an EOF-related read event.
2. `ProcessTcpEvents` calls `DrainTcpReadable`.
3. `DrainTcpReadable` loops with `readv` under the configured byte/event budget.
4. Read bytes are stored in relay buffer owners from `relay_buffer.cpp` / `relay_alloc.cpp`.
5. `BuildTcpToQuicViews` constructs MsQuic buffers directly or through zstd compression.
6. `SubmitTcpBatchToQuic` calls `Stream->Send`.
7. In-flight QUIC send counts and bytes are incremented.
8. MsQuic `QUIC_STREAM_EVENT_SEND_COMPLETE` is converted into a Darwin worker event.
9. The worker handles send completion, releases buffer owners, decrements accounting, retries pending QUIC sends, and resumes TCP read if backpressure has cleared.

If `Stream->Send` reports transient backpressure such as out-of-memory or buffer-too-small, the Darwin worker should queue the send operation and retry from the worker thread, matching Linux behavior.

When QUIC send backlog exceeds configured thresholds, disable `EVFILT_READ` by clearing `TcpReadArmed` and calling `UpdateTcpInterest`. When backlog falls below resume thresholds, re-enable `EVFILT_READ`.

## QUIC to TCP Path

The Darwin QUIC-to-TCP path should preserve MsQuic pending receive semantics:

1. MsQuic invokes `QUIC_STREAM_EVENT_RECEIVE` on a callback thread.
2. The callback captures receive buffers into a pending receive or receive view object owned by the relay backend.
3. The callback queues `QuicReceive` or `QuicReceiveView` to the owner worker.
4. The callback returns `QUIC_STATUS_PENDING`.
5. The worker processes the queued receive event.
6. If compression is disabled, the worker creates TCP write views over the received data.
7. If zstd compression is enabled, the worker decompresses on the worker thread, then creates TCP write views.
8. `FlushTcpWrites` writes with `sendmsg` or `writev` and advances completed offsets.
9. When all bytes for a pending receive are written or intentionally discarded due to close, the worker completes the MsQuic pending receive.
10. If the socket is not writable, the worker retains the pending receive, enables `EVFILT_WRITE`, and retries when kqueue reports write readiness.

QUIC receive pause/resume must be implemented in the first complete macOS backend. When pending QUIC receive bytes exceed the configured limit, the worker should call `SetQuicReceiveEnabled(false)`. After TCP writes reduce pending bytes below the resume threshold, it should call `SetQuicReceiveEnabled(true)`.

This prevents unbounded memory growth when the QUIC side is faster than the TCP peer or when the TCP peer stops reading.

## macOS Socket Differences

### SIGPIPE

Linux uses `MSG_NOSIGNAL` for `sendmsg`; macOS does not support that flag. The Darwin backend should prevent SIGPIPE by setting `SO_NOSIGPIPE` on accepted or connected relay sockets before worker I/O begins.

A small helper should be added in the Darwin worker or POSIX socket layer:

- `TqSetSocketNoSigPipeIfSupported(TqSocketHandle fd)`
- `TqSendMsgNoSignal(TqSocketHandle fd, const msghdr* msg)`

On macOS, `TqSendMsgNoSignal` calls `sendmsg(fd, msg, 0)` after `SO_NOSIGPIPE` has been set. On Linux, existing `MSG_NOSIGNAL` behavior should remain unchanged.

### EOF and Error Events

`EV_EOF` can be delivered together with remaining readable bytes. The Darwin worker should treat it as a hint and still drain reads before deciding that the TCP side has closed.

For `EV_ERROR`, the worker should record the error, stop further reads, abort or drain the QUIC side according to the same rules as Linux, and retire the relay after all pending ownership is resolved.

### Write Readiness

`EVFILT_WRITE` can remain continuously ready for idle sockets. The worker should enable it only while there is useful work to advance and disable it immediately after queues become empty.

## Close, Drain, and Lifetime

`TqRelayStop` for Darwin should enqueue a stop event to the owner worker and wake it.

The worker stop path should:

1. Mark the relay as closing.
2. Disable and delete kqueue filters for the TCP socket.
3. Stop accepting new callback work for the relay.
4. Flush or complete pending QUIC receives according to the existing relay close semantics.
5. Send QUIC FIN or abort as required by the direction that closed.
6. Shutdown TCP send when QUIC receive FIN is fully flushed.
7. Close the TCP socket when both directions are drained or a fatal error requires abort.
8. Move callback binding and relay state to a retired list until queued events and callback references are safe to release.

Late kqueue events and late MsQuic callback events must be safe. They should check relay id/state, observe the closing flag, release any temporary ownership, and return without touching freed memory.

Worker shutdown should:

1. Set a stopping flag.
2. Trigger `EVFILT_USER`.
3. Drain shutdown events.
4. Close or retire remaining relays.
5. Join the thread.
6. Close the kqueue fd.
7. Release retired relay/callback state.

## Metrics and Tuning

Darwin should expose the same categories of runtime metrics as Linux where possible:

- worker count;
- registered relay count;
- retired relay count;
- event queue depth/capacity;
- TCP read/write events;
- QUIC receive queued/completed counts;
- QUIC send complete counts;
- TCP read backpressure events;
- QUIC receive pause/resume counts;
- kqueue wake count;
- kqueue error count.

For tuning, the first implementation should reuse existing Linux relay fast-path values as Darwin defaults where the semantics match. If user-facing config names are Linux-specific, Darwin can map to those defaults internally first. Darwin-specific config names should only be added when a setting has macOS-specific semantics.

This keeps the first macOS implementation compatible with existing tuning behavior and avoids premature config surface expansion.

## Testing Strategy

### Unit Tests

Add Darwin-only queue tests in `src/unittest/darwin_relay_worker_queue_test.cpp`:

- push/pop preserves order;
- full queue rejects additional events;
- queue size approximation is within expected bounds;
- wake event is triggered when events are enqueued;
- shutdown event can be drained after normal events.

Add Darwin-only I/O tests in `src/unittest/darwin_relay_worker_io_test.cpp`:

- `socketpair` or loopback TCP read readiness produces `EVFILT_READ`;
- pending writes enable `EVFILT_WRITE` and completed writes disable it;
- EOF with remaining data drains the data before close;
- stop removes kqueue filters and closes relay state safely;
- repeated late events after stop are ignored safely.

Add or extend backend-selection tests on Darwin:

- `TqRelayStart` selects `TqRelayBackendType::DarwinWorker`;
- `TqRelayStop` dispatches to the Darwin worker;
- unsupported test-only sink behavior is explicit if `TqRelayStartQuicReceiveSink` remains Linux-only.

### Integration Tests

Run existing local proxy tests on macOS after adapting shell portability where needed:

- `scripts/test-tcpquic-proxy.sh` with compression disabled;
- `scripts/test-tcpquic-proxy.sh` with zstd compression;
- SOCKS5 path;
- HTTP CONNECT path;
- ACL/DNS/refused negative cases;
- multiple QUIC connections;
- built-in download speed test;
- built-in upload speed test.

Where scripts currently assume Linux tools, use Darwin equivalents such as `sysctl -n hw.ncpu` instead of `nproc`. Linux-only DGX/netem scripts are outside the macOS adaptation scope.

### Performance and Stability Checks

The macOS backend should be validated under sustained and concurrent relay traffic:

- pending QUIC receive bytes should not grow without bound;
- event queue depth should recover after bursts;
- TCP read should pause under QUIC send pressure and resume after send completions;
- QUIC receive should pause under TCP write pressure and resume after writes complete;
- worker threads should stop cleanly;
- file descriptors should not leak across repeated integration test runs.

## Implementation Order

1. Add Darwin backend type, handle fields, CMake Darwin source selection, and empty runtime skeleton.
2. Implement kqueue worker startup, `EVFILT_USER` wakeup, event queue, and queue tests.
3. Implement TCP socket registration, read/write interest updates, and basic kqueue I/O tests.
4. Port Linux TCP-to-QUIC send path semantics, including relay buffers and QUIC send completion handling.
5. Port Linux QUIC-to-TCP pending receive semantics, including receive views and zstd decompression.
6. Implement TCP read backpressure and QUIC receive pause/resume.
7. Implement stop, drain, close, and retired callback lifetime handling.
8. Add Darwin metrics snapshots and backend-selection tests.
9. Run Darwin unit tests and macOS local integration tests.
10. After macOS behavior is verified, consider a separate refactor to share event queue or readiness-worker helpers between Linux and Darwin.

## Risks and Mitigations

### Risk: kqueue EOF semantics differ from epoll

Mitigation: Treat `EV_EOF` as a hint, always drain readable data, and only treat `readv == 0` as confirmed TCP EOF.

### Risk: SIGPIPE terminates the process during TCP writes

Mitigation: Set `SO_NOSIGPIPE` on Darwin relay sockets before worker sends, and avoid using Linux-only `MSG_NOSIGNAL` in Darwin code.

### Risk: repeated `EVFILT_WRITE` readiness causes CPU spin

Mitigation: Enable write readiness only while pending TCP writes or deferred QUIC receives exist, and disable it immediately after queues drain.

### Risk: duplicated Linux/Darwin relay code increases maintenance cost

Mitigation: Keep the first Darwin implementation platform-local to avoid destabilizing Linux. Extract shared readiness helpers later after both implementations have passing tests and comparable behavior.

### Risk: late callbacks or stale kqueue events access freed relay state

Mitigation: Use relay ids, closing flags, deleted filters, callback bindings, and a retired list so all late events can be ignored safely before memory is released.

## Acceptance Criteria

- macOS builds include `darwin_relay_worker.cpp`, `relay_alloc.cpp`, and `relay_buffer.cpp`.
- `TqRelayStart` on macOS registers relays with `TqDarwinRelayRuntime` and returns success for valid sockets and streams.
- Darwin worker tests pass on macOS.
- Local macOS proxy integration tests pass for SOCKS5, HTTP CONNECT, compression off, and zstd.
- Built-in upload and download speed tests run on macOS without relay failure.
- Backpressure tests or metrics confirm that TCP read and QUIC receive pause/resume work under pressure.
- Linux and Windows builds keep their existing backend selection and behavior.

## Self-Review

- Placeholder scan: no unresolved placeholders are present. References to future refactoring are explicitly scoped as post-verification cleanup, not required for the first macOS backend.
- Consistency check: the design consistently selects a Darwin-specific `kqueue` backend and does not require changes to Linux `epoll` or Windows IOCP behavior.
- Scope check: the work is focused on macOS relay backend parity with Linux fast-path semantics. Non-Darwin BSD support and cross-platform readiness refactoring are intentionally out of scope.
- Ambiguity check: backend selection, event mapping, backpressure, close/lifetime handling, build integration, and acceptance criteria are stated explicitly.
