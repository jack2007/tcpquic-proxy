# Windows IOCP callback queue redesign

## Context

Windows relay currently has three scheduling layers for MsQuic callback work:

1. IOCP completion queue, used for real TCP overlapped completions and wake tokens.
2. `TqWindowsRelayEventQueue`, used by MsQuic callback threads to enqueue `TqWindowsRelayTask`.
3. Per-relay queues such as `relay->PendingReceives`, used to preserve QUIC receive view ownership and ordering.

The `quic -> tcp` receive path already stores payload views in `relay->PendingReceives`, then also enqueues `QuicReceiveView(view*)` into the worker event queue. `FinishReceiveView()` can enqueue another task for the next view. This creates duplicate view-level work items and requires temporary idempotency guards such as `TcpSendPending`.

The target design removes the worker event queue as a callback transport. All MsQuic callback-originated work is posted directly to the worker IOCP queue, and receive draining is scheduled per relay instead of per receive view.

## Goals

- Use IOCP as the only cross-thread worker dispatch queue on Windows.
- Preserve relative ordering between callback events from the same worker by putting them through one queue.
- Replace `QuicReceiveView(view*)` callback scheduling with `RelayReceiveReady(relayId)` markers and worker-internal `RelayReceiveDrain(relayId)` continuations.
- Keep `relay->PendingReceives` as the only queue that owns QUIC receive views.
- Avoid carrying QUIC receive view pointers in wake notifications.
- Keep TCP overlapped completion handling unchanged except where it schedules receive-drain continuation.
- Keep shutdown/abort/send-complete/ideal-send-buffer events ordered with receive-drain events through the same IOCP queue.

## Non-goals

- Do not change Linux or Darwin relay behavior.
- Do not change QUIC receive ownership semantics: callback still returns `QUIC_STATUS_PENDING` when a receive view is queued.
- Do not introduce inline TCP writes from MsQuic callback threads.
- Do not change compression semantics.
- Do not remove defensive receive-view state checks until the new invariants have test coverage.

## Proposed architecture

Add a posted worker operation type for each callback event class:

- `RelayReceiveReady`
- `QuicSendComplete`
- `QuicIdealSendBuffer`
- `QuicPeerSendAborted`
- `QuicPeerReceiveAborted`
- `QuicShutdownComplete`
- `RelayReceiveDrain`, used only for worker-internal continuation after TCP send completion or finish
- existing `CloseRelay` and `StopWorker` remain IOCP operations

The posted operation is represented by an `IoOperation` with `Event` set to the operation type and `Relay` or `RelayId` identifying the relay. Receive-drain operations identify only the relay. They do not carry `TqWindowsPendingQuicReceive`.

`TqWindowsRelayEventQueue` and `TqWindowsRelayTask` stop being part of production callback dispatch. Unit tests can either be migrated to IOCP-posted operations or keep a small test-only helper, but production paths should not enqueue callback work into `EventQueue_`.

## Receive path

Callback path:

```text
MsQuic RECEIVE callback
  -> copy QUIC buffers into TqWindowsPendingQuicReceive::OwnedBuffer
  -> push shared_ptr into relay->PendingReceives
  -> update pending byte/depth metrics and receive backpressure
  -> PostCallbackOperation(RelayReceiveReady, relay)
  -> return QUIC_STATUS_PENDING
```

Worker path:

```text
IOCP RelayReceiveReady(relayId)
  -> DrainRelayReceives(relay)
       -> inspect relay->PendingReceives.front()
       -> if no view: return
       -> if closing/write closed: complete or close as existing logic does
       -> if view->TcpSendPending: return
       -> submit at most one WSASend for the front view
```

TCP send completion path:

```text
IOCP TcpSend completion
  -> clear view->TcpSendPending
  -> advance receive view by completed bytes
  -> flush ReceiveComplete according to batching rules
  -> if view is complete: FinishReceiveView(relay, view)
  -> ScheduleRelayReceiveDrain(relay) when more receive work might exist
```

`FinishReceiveView()` no longer enqueues a task for `nextView`. It removes the completed view from `PendingReceives`, updates metrics, handles FIN, resumes QUIC receive when possible, and schedules a relay receive drain if the queue is still non-empty.

## Scheduling invariant

There are two receive-related IOCP posts:

1. `RelayReceiveReady`: callback-order marker. Every RECEIVE callback that accepts ownership posts one marker. It is not coalesced, because coalescing can move a later receive ahead of an already posted ideal-buffer, abort, shutdown, or send-complete callback event.
2. `RelayReceiveDrain`: worker-internal continuation. This may be coalesced because it is not representing a distinct MsQuic callback event.

Add `RelayContext::ReceiveDrainQueued` for continuation posts only.

```text
ScheduleRelayReceiveDrain(relay):
  if relay is null or closing: return false
  if relay->ReceiveDrainQueued.exchange(true) == true: return true
  PostQueuedCompletionStatus(IOCP, RelayReceiveDrain(relay))
```

If posting fails, clear `ReceiveDrainQueued`, update the wake/post failure metric, and fail the relay if the failure means queued receive ownership cannot be serviced.

When the IOCP worker handles `RelayReceiveDrain`, it clears `ReceiveDrainQueued` before draining. If worker-side state creates more receive work while draining is in progress, `ScheduleRelayReceiveDrain()` can post another continuation. `DrainRelayReceives()` must still check `TcpSendPending` before submitting a send, because a relay may already have an overlapped TCP send in flight for the current front view. `RelayReceiveReady` operations do not use `ReceiveDrainQueued`; duplicate ready markers are safe because drain is state-based and front-view gated.

## Callback event ordering

All MsQuic callback event handlers post IOCP operations in callback order:

- receive callback posts `RelayReceiveReady` after pushing the view into `PendingReceives`
- send complete posts `QuicSendComplete`
- ideal send buffer posts `QuicIdealSendBuffer`
- peer abort posts the corresponding abort operation
- shutdown complete posts `QuicShutdownComplete`

No callback event uses `EventQueue_`. This keeps callback-originated ordering dependent on a single queue. `RelayReceiveReady` is intentionally per callback event rather than coalesced so it cannot skip over another callback event already posted to IOCP. IOCP does not serialize with real TCP completions, so worker handlers must remain state-driven and idempotent when TCP completions interleave with callback events.

## Error handling

- If a relay cannot be found for a posted callback operation, drop the stale operation after updating stale/drop metrics where appropriate.
- If receive-drain posting fails after a view was queued, close/fail the relay and complete pending receive ownership to avoid leaking MsQuic receive buffers.
- Existing teardown downgrade behavior for TCP completion errors remains unchanged.
- Existing close path must complete all pending receive views and retire callback bindings only after outstanding posted worker operations are safe to discard.

## Metrics and diagnostics

Rename or reinterpret event queue metrics so they describe IOCP-posted callback work instead of `EventQueue_` depth:

- callback IOCP post count
- callback IOCP post failure count
- receive-ready callback post count
- receive-drain continuation scheduled count
- receive-drain continuation coalesced count
- stale posted callback drop count

Keep receive-view trace stages around the drain boundary:

- `queue_receive`
- `post_receive_ready`
- `schedule_receive_drain`
- `drain_receive_front`
- `drain_receive_send_pending`
- `post_tcp_send_receive_view`
- `tcp_send_complete_receive_view`
- `finish_remove`

The old duplicate-task trace `process_send_pending` should become an invariant guard rather than the expected fix signal.

## Testing

Unit tests should cover:

- two receive views queued before the worker drains: only one TCP send is posted for the front view
- completing view A schedules/drains view B without enqueuing a B-specific task
- repeated callback receive events post repeated `RelayReceiveReady` markers, but no marker carries a receive view pointer
- repeated worker-internal `ScheduleRelayReceiveDrain()` calls coalesce while a continuation is queued
- receive-drain operation carries relay identity but no receive view pointer
- callback event ordering through IOCP for receive, ideal-send-buffer, abort, shutdown, and send-complete
- stale posted operations after relay close are ignored without touching freed callback state
- queue/post failure after receive ownership is accepted completes or closes safely
- compressed receive path still advances compressed input ownership only after generated TCP bytes are accepted

Regression verification should include the Windows relay worker tests and a Windows integration run that previously reproduced `CompletedLength > TotalLength` or `AccountedLength > TotalLength`.

## Migration steps

1. Add IOCP posted callback operation helpers and relay-id/payload fields.
2. Implement `ScheduleRelayReceiveDrain()` and `DrainRelayReceives()`.
3. Move RECEIVE callback dispatch from `EnqueueEvent(QuicReceiveView)` to `PostCallbackOperation(RelayReceiveReady)`.
4. Change `FinishReceiveView()` and `HandleTcpSend()` to schedule relay receive drain instead of enqueueing view tasks.
5. Move send-complete, ideal-send-buffer, peer-abort, and shutdown-complete callback events from `EventQueue_` to IOCP posted operations.
6. Remove production use of `DrainEvents()`, `ProcessRelayTask()`, `TqWindowsRelayEventQueue`, and `TqWindowsRelayTask`.
7. Update metrics, snapshots, tests, and docs to describe IOCP-posted callback operations.

## Open implementation notes

- `IoOperation` currently stores `std::shared_ptr<RelayContext> Relay`; posted callback operations can use that to keep relay state alive until the worker processes or drops the operation. This should be balanced with close/retirement rules to avoid keeping stopped relays visible longer than necessary.
- Send-complete operations own `TqWindowsQuicSendOperation*`; the IOCP posted op must preserve exactly one deletion path.
- `TcpSendPending` should remain during the migration as a hard guard against duplicate WSASend submission for a receive view.
