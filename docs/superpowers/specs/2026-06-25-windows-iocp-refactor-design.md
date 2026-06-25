# Windows IOCP Refactor Design

## Background
Windows relay connections drop frequently in production due to aggressive error handling, missing completion boundary checks, and a lack of proper backpressure on TCP sends. This leads to legal closing states triggering fatal errors, race conditions in the IOCP completion port, and an unbounded number of `InFlightTcpSends`.

## Goals
Fix the instability while maintaining the "zero-copy" semantics (no unnecessary `memcpy` operations introduced in the data-plane forwarding). Introduce minimal structural changes by correcting the state machine and bounds rather than replacing the IOCP model with an external event queue.

## Proposed Solutions (The 3 Pillars)

### 1. Differentiate Closing from Fatal (Soft Teardown)
- **Problem:** Currently, when `GetQueuedCompletionStatus` fails or returns 0 bytes on send, `RecordTcpHardErrorAndFail` triggers a fatal relay abort, even if the connection was already draining/closing normally.
- **Solution:** Check `relay->CloseAfterDrained` and `relay->Closing` or the presence of a known teardown condition. If it is already in a teardown state, gracefully retire the relay instead of recording a hard error. 
- **Action:** Refine error handling in `Run()` and `HandleTcpSend()` to suppress fatal logs and resets if closing.

### 2. Epoch/Generation Check on Callbacks
- **Problem:** MsQuic can invoke `StreamCallback` concurrently or just as a relay is closing. Current `CallbackBinding` reference counting prevents use-after-free but doesn't prevent an old stream callback from altering states when it shouldn't.
- **Solution:** Protect IOCP operations with a generation/epoch check or ensure that when `relay->Closing` is true, all subsequent operations are strictly discarded. Since `CallbackRefs` is used, we must guarantee that once a relay is closing, any scheduled IOCP task (e.g. `QuicReceiveViewQueued` or `QuicSendComplete`) returns immediately.

### 3. Backpressure on TCP Sends (In-Flight Limits)
- **Problem:** When QUIC receives multiple buffers rapidly, it unconditionally calls `PostQueuedCompletionStatus` with `QuicReceiveViewQueued` for every buffer. The IOCP worker processes these and dispatches multiple concurrent `WSASend` operations, leading to an unbounded `InFlightTcpSends` count.
- **Solution:** Limit the concurrent TCP sends. Use a mechanism similar to the Linux reactor: 
  - `QueueDeferredQuicReceive` should only enqueue to `PendingReceives` and signal IOCP if we are *not* already sending (`InFlightTcpSends < MaxInFlight`).
  - `HandleTcpSend` after finishing a `view` should check `PendingReceives` and dispatch the *next* view if available.
  - This avoids exploding the concurrent WSASend calls on the socket.

## Expected Outcomes
- Stabilized Windows connection lifespan.
- No `FatalRelayResets` during standard connection teardown.
- Capped memory usage and WSASend queue size during burst QUIC traffic.
