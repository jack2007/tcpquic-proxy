# DGX MsQuic BBR queue/loss diagnostic - 2026-06-21

## Instrumentation

Added secnetperf/MsQuic diagnostics:

- Periodic stream stats include app queue fill, queue free bytes, outstanding bytes, enqueue/complete deltas.
- Core stream stats include send request queue delay from enqueue to first STREAM-frame packetization.
- Core stream stats include cumulative new-data bytes and recovery retransmit bytes.
- Connection NetStats include BBR bandwidth estimate, posted/ideal bytes, bytes in flight, loss counters.

Important test-flow fix:

- The server must be started and confirmed `Started!` before applying remote `netem`.
- Starting the server over SSH after applying 180-200ms netem can make the client send before the server is ready, producing false handshake failures.

## Results

All tests used sender-side `netem delay {100,200}ms loss 5% limit 5000000`, BBR, 1 connection, 1 stream, download.

### 512MiB queue

Result directory: `docs/dgx-msquic-secnetperf-bbr-queue512m-lossdiag-serverready-20260621-111116`

| Case | Download | Queue fill | Avg first-send delay | Max first-send delay | Recovery pct | Avg BBR estimate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 8.798 Gbps | 100% | 260 ms | 888 ms | 25.4% | 13.66 Gbps |
| 200ms + 5% | 6.867 Gbps | 100% | 141 ms | 1486 ms | 29.7% | 13.86 Gbps |

### 1GiB queue

Result directory: `docs/dgx-msquic-secnetperf-bbr-queue1g-lossdiag-serverready-20260621-111506`

| Case | Download | Queue fill | Avg first-send delay | Max first-send delay | Recovery pct | Avg BBR estimate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 8.563 Gbps | 100% | 745 ms | 1238 ms | 25.8% | 13.25 Gbps |
| 200ms + 5% | 7.024 Gbps | 100% | 721 ms | 1991 ms | 30.8% | 12.39 Gbps |

## Current conclusion

The send queue is not starving. It remains full at both 512MiB and 1GiB, and increasing it to 1GiB does not materially improve throughput. Instead, it increases enqueue-to-first-send latency, which means bytes are entering the queue but waiting behind already queued/recovery data before packetization.

The direct limiter is after application enqueue: MsQuic packetization/scheduling and recovery retransmission. In loss scenarios, MsQuic marks stream bytes lost, opens the stream recovery window, and `QuicStreamWriteStreamFrames` prioritizes `RecoveryNextOffset` before `NextSendOffset`. With continuous random 5% loss, roughly 25-31% of transmitted stream bytes are recovery bytes in these runs, so a significant part of packetization capacity is spent before new stream offsets can advance.

This does not prove a retransmission bug yet. It does show that simply increasing send queue/window size is not enough: new data is queued but not promptly packetized, and deeper queues mostly add queueing delay.

## MsQuic loss/recovery mechanism notes

- Loss detection uses packet-threshold/FACK and time-threshold/RACK.
- FACK: a packet is lost if `PacketNumber + QUIC_PACKET_REORDER_THRESHOLD < LargestAck`.
- RACK: a packet older than `QUIC_TIME_REORDER_THRESHOLD(max(SmoothedRtt, LatestRttSample))` after a later ACK is treated as lost.
- Once lost, `QuicLossDetectionRetransmitFrames` calls `QuicStreamOnLoss` for STREAM frames.
- `QuicStreamOnLoss` expands `[RecoveryNextOffset, RecoveryEndOffset)` and sets the stream DATA send flag.
- `QuicStreamWriteStreamFrames` sends recovery bytes first while `RecoveryNextOffset < RecoveryEndOffset`, then resumes new bytes from `NextSendOffset`.

## Next useful checks

- Add per-interval recovery/new byte deltas, not just cumulative final percentages.
- Add current recovery window length to stream stats.
- Check whether recovery windows stay open continuously under 5% random loss.
- Compare single-stream behavior with multiple streams on the same connection to see whether recovery-window priority is per-stream head-of-line.
