# tcpquic-proxy Ideal Send Buffer Backpressure Design

## Goal

This worktree validates whether tcpquic-proxy throughput under high RTT and packet loss improves when proxy relay backpressure follows MsQuic's stream-level `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` hint instead of tcpquic-proxy's current `relay-inflight-bytes` and BDP-derived `initial-quic-read-ahead` logic.

The validation target is the 2*DGX 1x1 download case:

- 1 QUIC connection
- 1 HTTP CONNECT stream
- sender-side netem `delay 100ms loss 5% limit 5000000`
- sender-side netem `delay 200ms loss 5% limit 5000000`

## Background

The secnetperf parameter matrix showed that the main throughput lever is effective application-level outstanding send bytes:

- `postbuf=128MiB, corecap=128MiB`: `3.580 Gbps / 1.790 Gbps`
- `postbuf=512MiB, corecap=128MiB`: `8.651 Gbps / 6.538 Gbps`
- `postbuf=128MiB, corecap=512MiB`: improved because secnetperf responds to `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`

tcpquic-proxy was lower in comparable tests:

- 100ms + 5% loss: about `1.4-1.8 Gbps`
- 200ms + 5% loss: about `0.6 Gbps`

The existing 100ms trace showed repeated relay-side pauses:

```text
event=relay_backpressure action=pause reason=quic_send_backlog
pause_threshold=67108864
read_ahead=67108864
```

It also showed `posted` often below MsQuic `ideal`, which means the proxy relay was not consistently feeding enough data into MsQuic to match the amount MsQuic wanted available.

## Required Behavior

This worktree will implement a validation mode with these fixed QUIC/window parameters:

- stream receive window (`srw`): `2GiB`
- connection flow control window (`fcw`): `2GiB`
- MsQuic core max ideal send buffer cap (`corecap`): `1GiB`

It will replace the current relay send backlog pause threshold source:

```text
old:
  relay-inflight-bytes / initial-quic-read-ahead / 2*BDP from NETWORK_STATISTICS

new:
  latest QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount for the stream
```

The relay should pause TCP reads when the relay's outstanding QUIC send bytes reach the latest stream ideal send buffer size, and resume as soon as outstanding bytes are strictly lower than that value:

```text
pause:
  OutstandingQuicSendBytes >= QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount

resume:
  OutstandingQuicSendBytes < QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount
```

The resume condition intentionally does not wait for the queue to drain to half of the ideal-send value. The validation goal is to keep the relay send queue as close to MsQuic's current ideal value as possible, so any completion that brings outstanding bytes below the ideal should allow TCP reads to refill the queue.

## Parameter Mapping

### `srw` and `fcw`

These are MsQuic flow-control credit settings:

- `StreamRecvWindowDefault`
- `ConnFlowControlWindow`

They are configured in `TqMakeMsQuicSettings()` in `src/protocol/quic_session.cpp`.

Current types are `uint32_t`, so `2GiB` must be represented as `0x80000000u`. The value is a cap, not an immediate memory allocation.

### `corecap`

MsQuic's current code caps connection ideal send buffer growth with:

```c
QUIC_MAX_IDEAL_SEND_BUFFER_SIZE
```

in `third_party/msquic/src/core/quicdef.h`.

For this validation branch, set it to:

```c
#define QUIC_MAX_IDEAL_SEND_BUFFER_SIZE 0x40000000 // 1GiB
```

This cap limits how large `Connection->SendBuffer.IdealBytes` can grow. It does not itself allocate 1GiB and does not directly send data.

### `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`

The callback payload has one direct field:

```c
Event->IDEAL_SEND_BUFFER_SIZE.ByteCount
```

This is the value the relay should use as the per-stream send backlog target.

## Architecture

### Current Flow

For Linux relay TCP-to-QUIC traffic:

```text
TCP readable
  -> TqLinuxRelayWorker::DrainTcpReadable()
  -> BuildTcpToQuicViews()
  -> SubmitTcpBatchToQuic()
  -> MsQuic->StreamSend()
  -> OutstandingQuicSendBytes += operation->TotalBytes
  -> ShouldPauseTcpReadForQuicBacklog()
```

The current pause threshold comes from:

```cpp
CurrentMaxBufferedQuicSendBytes()
  -> TqRelayCurrentQuicReadAheadBytes()
```

`TqRelayCurrentQuicReadAheadBytes()` is updated from `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` using `2 * BDP`.

### New Flow

The relay stream callback will handle `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`:

```text
QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount
  -> enqueue worker event with relay id and byte count
  -> worker updates RelayState.IdealSendBufferBytes
  -> ShouldPauseTcpReadForQuicBacklog() uses RelayState.IdealSendBufferBytes
```

The callback must not directly mutate relay state without following the existing worker ownership model. Linux relay currently queues `SEND_COMPLETE` from callback into the worker event queue, so the ideal-send event should follow the same pattern.

### Fallback

Before the first ideal-send event arrives, use a conservative fallback:

```text
fallback = 64MiB
```

This avoids unlimited TCP reads during connection startup. The fallback must be visible in tuning logs and trace logs so tests can distinguish "no ideal event yet" from "ideal event received".

## Linux Implementation Scope

Primary implementation target is Linux because the DGX tests use `linux-epoll`.

Files to modify:

- `src/protocol/quic_session.cpp`
  - force srw/fcw to 2GiB in the validation mode or profile
  - stop using `NETWORK_STATISTICS` to update relay read-ahead for this validation path
- `src/config/tuning.h`
  - add explicit constants for 2GiB windows and the initial fallback, if needed
- `src/config/tuning.cpp`
  - remove or bypass BDP-derived read-ahead for this validation branch
  - make logs show that ideal-send-buffer mode is active
- `src/tunnel/linux_relay_event_queue.h`
  - add an event carrying ideal-send bytes
- `src/tunnel/linux_relay_worker.h`
  - add `IdealSendBufferBytes` to `RelayState`
  - add worker-side handler for ideal-send update events
- `src/tunnel/linux_relay_worker.cpp`
  - handle `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`
  - update pause/resume logic to use relay ideal-send bytes
  - trace ideal-send updates and pause threshold source
- `src/runtime/trace.cpp`
  - add trace lines for stream ideal-send updates
- `src/runtime/trace.h`
  - add trace helper declaration
- `third_party/msquic/src/core/quicdef.h`
  - set `QUIC_MAX_IDEAL_SEND_BUFFER_SIZE` to `1GiB`

Darwin and Windows relay code may keep the old behavior for this validation branch unless build tests require interface adjustments.

## Observability

Every test run must log these values:

- `ideal_send_event_bytes`
- `relay_ideal_send_bytes`
- `outstanding_quic_send_bytes`
- `pause_threshold`
- `resume_threshold`
- `posted` from MsQuic network stats
- `ideal` from MsQuic network stats
- `bytes_in_flight`
- `bbr_bw_mbps`
- `loss_rate`

The trace should make it possible to answer:

1. Did MsQuic emit stream ideal-send events?
2. Did tcpquic-proxy receive and store those event values?
3. Did relay pause thresholds track those values?
4. Was TCP reading paused while `posted < ideal`?
5. Did larger ideal-send values translate into larger `posted` and `OutstandingQuicSendBytes`?

## Test Matrix

Initial functional tests:

- unit test for ideal-send callback event enqueue
- unit test for relay pause threshold selection
- unit test for resume threshold strictly below ideal-send bytes
- build `tcpquic-proxy`

DGX validation:

| Case | netem | Expected observation |
| --- | --- | --- |
| baseline branch | 100ms + 5% loss | reproduce ~1.4-1.8Gbps |
| ideal-send branch | 100ms + 5% loss | higher `posted`, fewer premature relay pauses |
| baseline branch | 200ms + 5% loss | reproduce ~0.6Gbps |
| ideal-send branch | 200ms + 5% loss | higher `posted`, throughput closer to secnetperf low baseline |

The first success criterion is not a specific Gbps number. It is that the proxy can show larger and more stable QUIC posted/outstanding bytes driven by `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`. Throughput improvement is then measured against the previous tcpquic-proxy results.

## Risks

- The stream callback may deliver ideal-send events before relay binding is fully active; the handler must tolerate missing relay state.
- The ideal-send value may grow slowly because MsQuic only raises `IdealBytes` after `BytesInFlightMax` grows. Startup fallback must not be too small.
- If proxy downstream TCP write becomes the bottleneck, increasing QUIC outstanding will not improve end-to-end throughput.
- Setting flow-control windows to 2GiB does not allocate 2GiB immediately, but it allows larger buffering under slow consumers.

## Non-Goals

- Do not redesign congestion control.
- Do not change QUIC loss detection.
- Do not change HTTP CONNECT protocol framing.
- Do not optimize multi-connection or multi-stream cases until 1x1 behavior is understood.
