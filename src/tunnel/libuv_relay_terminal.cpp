#include "libuv_relay_worker.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>

namespace {

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
std::atomic<TqUvTerminalHookForTest> gTerminalHookForTest{nullptr};
std::atomic<TqUvConvergenceHookForTest> gConvergenceHookForTest{nullptr};
#endif

bool IsAbort(TqUvTerminalTrigger trigger) noexcept {
    return trigger != TqUvTerminalTrigger::TcpEof &&
           trigger != TqUvTerminalTrigger::QuicFin &&
           trigger != TqUvTerminalTrigger::RuntimeStop;
}

TqUvTerminalTrigger SelectTerminalTrigger(std::uint32_t mask) noexcept {
    const TqUvTerminalTrigger priority[] = {
        TqUvTerminalTrigger::RuntimeStop,
        TqUvTerminalTrigger::QuicAbort,
        TqUvTerminalTrigger::TcpError,
        TqUvTerminalTrigger::QueueFailure,
        TqUvTerminalTrigger::AllocationFailure,
        TqUvTerminalTrigger::DecompressionFailure,
        TqUvTerminalTrigger::PressureFailure,
        TqUvTerminalTrigger::ReceiveCompletionFailure,
        TqUvTerminalTrigger::RegistrationFailure,
        TqUvTerminalTrigger::QuicFin,
        TqUvTerminalTrigger::TcpEof,
    };
    for (const auto candidate : priority) {
        const auto bit = std::uint32_t{1} <<
            static_cast<std::uint8_t>(candidate);
        if ((mask & bit) != 0) {
            return candidate;
        }
    }
    return TqUvTerminalTrigger::RegistrationFailure;
}

bool LocalOwnershipDrainedImpl(const TqUvRelayState& relay) noexcept {
    return relay.TcpWrites.empty() && relay.QuicSends.empty() &&
           relay.PendingQuicSendRetries.empty() &&
           relay.PrecommitReceives.empty() &&
           relay.FallbackReceiveHead == nullptr &&
           relay.TcpReadBuffers.empty() &&
           relay.PendingTcpWriteBytes == 0 &&
           relay.PendingQuicSendBytes == 0 &&
           relay.PendingQuicReceiveBytes == 0 &&
           relay.PendingTcpReadBytes == 0 &&
           relay.AccountedQuicToTcpBytes.load(std::memory_order_relaxed) == 0 &&
           relay.AccountedTcpToQuicBytes.load(std::memory_order_relaxed) == 0 &&
           relay.AdmittedQuicReceiveBytes.load(std::memory_order_acquire) == 0 &&
           relay.QuicToTcpPressureBytes.load(std::memory_order_acquire) == 0;
}

void DiscardUnsubmittedSends(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay) noexcept {
    std::uint64_t bytes = 0;
    for (const auto& operation : relay.PendingQuicSendRetries) {
        if (operation != nullptr &&
            operation->TotalBytes <= UINT64_MAX - bytes) {
            bytes += operation->TotalBytes;
        }
    }
    relay.PendingQuicSendRetries.clear();
    const auto released = std::min(relay.PendingQuicSendBytes, bytes);
    relay.PendingQuicSendBytes -= released;
    (void)worker.CompletePendingBytes(
        relay, TqUvPendingDirection::TcpToQuic, released);
}

bool NormalFinReady(const TqUvRelayState& relay) noexcept {
    return relay.TcpReadClosed && relay.QuicFinSubmitted &&
           relay.QuicFinCompleted && relay.QuicFinObserved &&
           relay.TcpWriteClosed && !relay.TcpShutdownPending &&
           TqUvLocalOperationOwnershipDrained(relay);
}

std::shared_ptr<TqTerminalHandoffControl> TerminalHandoff(
    const TqUvRelayState& relay) noexcept {
    return relay.StopControl != nullptr
        ? std::atomic_load(&relay.StopControl->TerminalHandoff)
        : std::shared_ptr<TqTerminalHandoffControl>{};
}

void RecordHandoffResult(
    TqUvRelayState& relay,
    const TqTerminalShutdownResult& result) noexcept {
    relay.TerminalHandoffStatus = result.Status;
    relay.TerminalHandoffSubmitted = result.Submitted;
    relay.TerminalHandoffRetryScheduled = result.RetryScheduled;
    relay.TerminalHandoffRetryPending =
        !result.Submitted && !result.AlreadyTerminal && !result.RetryScheduled;
    if (result.AlreadyTerminal) {
        relay.TerminalHandoffComplete.store(true, std::memory_order_release);
    }
}

void RefreshHandoffResultFromOwner(TqUvRelayState& relay) noexcept {
    if (relay.StreamOwner == nullptr) {
        return;
    }
    if (relay.Binding != nullptr) {
        relay.Binding->TerminalRouteGeneration.store(
            relay.StreamOwner->RouteGeneration(), std::memory_order_release);
    }
    const auto ledger = relay.StreamOwner->TerminalLedger();
    if (ledger == nullptr) {
        return;
    }
    const auto snapshot = ledger->Snapshot(std::chrono::steady_clock::now());
    relay.TerminalHandoffStatus = snapshot.ShutdownStatus;
    const bool submitted =
        snapshot.Phase == TerminalPhase::ShutdownSubmitted ||
        snapshot.Phase == TerminalPhase::TerminalObserved ||
        snapshot.Phase == TerminalPhase::Closed;
    relay.TerminalHandoffSubmitted = submitted;
    if (submitted) {
        relay.TerminalHandoffRetryScheduled = false;
        relay.TerminalHandoffRetryPending = false;
    }
}

void PublishHandoffFacts(TqUvRelayState& relay) noexcept {
    const auto handoff = TerminalHandoff(relay);
    if (handoff == nullptr) {
        return;
    }
    handoff->DataPlaneStopped.store(
        relay.TcpCloseCompleted, std::memory_order_release);
    handoff->TerminalHandoffComplete.store(
        relay.TerminalHandoffComplete.load(std::memory_order_acquire),
        std::memory_order_release);
    handoff->LocalOperationOwnershipTransferredOrDrained.store(
        relay.LocalOwnershipDrained, std::memory_order_release);
    handoff->AbortUpgradePending.store(
        relay.TerminalAbortUpgradePending, std::memory_order_release);
    handoff->AbortUpgradeApplied.store(
        relay.TerminalAbortUpgradeApplied, std::memory_order_release);
    if (TqTerminalReleaseReady(handoff->Snapshot()) &&
        !handoff->HandoffCompletedCounted.exchange(
            true, std::memory_order_acq_rel)) {
        TqRecordTerminalHandoffCompleted();
    }
}

void NotifyTerminalHook(
    TqUvRelayState& relay,
    TqUvTerminalTrigger trigger) noexcept {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (const auto hook = gTerminalHookForTest.load(std::memory_order_acquire)) {
        try { hook(relay, trigger); } catch (...) {}
    }
#else
    (void)relay;
    (void)trigger;
#endif
}

void NotifyConvergenceHook(TqUvRelayState& relay) noexcept {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (const auto hook = gConvergenceHookForTest.load(std::memory_order_acquire)) {
        try { hook(relay); } catch (...) {}
    }
#else
    (void)relay;
#endif
}

} // namespace

bool TqUvLocalOperationOwnershipDrained(
    const TqUvRelayState& relay) noexcept {
    return LocalOwnershipDrainedImpl(relay);
}

void TqUvRequestTerminal(
    TqUvRelayState& relay,
    TqUvTerminalTrigger trigger) noexcept {
    const auto bit = std::uint32_t{1} << static_cast<std::uint8_t>(trigger);
    relay.TerminalTriggerMask.fetch_or(bit, std::memory_order_release);
    if (relay.Worker != nullptr) {
        relay.Worker->WakeForDurableFacts();
    }
}

void TqUvBeginTerminalLocal(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay,
    TqUvTerminalTrigger trigger) noexcept {
    if (!relay.ActivationMutex.Lock()) {
        return;
    }
    const auto selectedMask = relay.TerminalTriggerMask.exchange(
        0, std::memory_order_acq_rel) |
        (std::uint32_t{1} << static_cast<std::uint8_t>(trigger));
    trigger = SelectTerminalTrigger(selectedMask);
    if (relay.TerminalStarted) {
        const bool upgradeToAbort = !relay.TerminalAborted && IsAbort(trigger);
        if (upgradeToAbort) {
            relay.TerminalAborted = true;
            if (relay.TerminalHandoffStarted &&
                !relay.TerminalHandoffComplete.load(std::memory_order_acquire)) {
                if (relay.TerminalHandoffSubmitted) {
                    relay.TerminalAbortUpgradePending = true;
                } else {
                    relay.TerminalHandoffRetryPending = true;
                }
            }
        }
        (void)relay.ActivationMutex.Unlock();
        if (upgradeToAbort) {
            DiscardUnsubmittedSends(worker, relay);
            relay.TcpReadBuffers.clear();
            relay.PendingTcpReadBytes = 0;
            TqUvSettleQuicReceivesAtTerminal(relay);
            TqUvCheckTerminalConvergence(worker, relay);
        }
        return;
    }
    relay.TerminalStarted = true;
    relay.TerminalAborted = IsAbort(trigger);
    relay.TerminalBeginCount.fetch_add(1, std::memory_order_release);
    relay.Activation = TqUvActivation::Terminal;
    if (relay.Binding != nullptr) {
        relay.Binding->Closing.store(true, std::memory_order_release);
        relay.Binding->Activation.store(
            TqUvActivation::Terminal, std::memory_order_release);
    }
    worker.SettlePrecommit(
        relay.shared_from_this(), false);
    (void)relay.ActivationMutex.Unlock();

    if (relay.TcpReadStarted) {
        try {
            (void)worker.CallReadStop(
                reinterpret_cast<uv_stream_t*>(&relay.TcpHandle));
        } catch (...) {
        }
        relay.TcpReadStarted = false;
    }
    if (relay.TerminalAborted) {
        DiscardUnsubmittedSends(worker, relay);
        relay.TcpReadBuffers.clear();
        relay.PendingTcpReadBytes = 0;
        TqUvSettleQuicReceivesAtTerminal(relay);
    }
    if (!relay.TerminalHandoffStarted) {
        relay.TerminalHandoffStarted = true;
        const auto handoff = TerminalHandoff(relay);
        if (handoff != nullptr &&
            !handoff->HandoffStartedCounted.exchange(
                true, std::memory_order_acq_rel)) {
            TqRecordTerminalHandoffStarted();
        }
        if (relay.StreamOwner != nullptr && relay.Binding != nullptr) {
            const auto result = relay.StreamOwner->BeginTerminalShutdown(
                0,
                relay.Binding,
                handoff != nullptr ? handoff->Escalation
                                   : std::shared_ptr<TqTerminalEscalation>{},
                relay.TerminalAborted
                    ? TqTerminalShutdownIntent::AbortBothImmediate
                    : TqTerminalShutdownIntent::GracefulComplete);
            relay.Binding->TerminalRouteGeneration.store(
                relay.StreamOwner->RouteGeneration(),
                std::memory_order_release);
            RecordHandoffResult(relay, result);
        } else {
            relay.TerminalHandoffComplete.store(true, std::memory_order_release);
        }
    }
    if (relay.StopControl != nullptr) {
        (void)relay.StopControl->SignalStop(relay.ControlGeneration);
    }
    try {
        worker.CloseRelay(relay.shared_from_this());
    } catch (...) {
        // Durable TerminalStarted is retried by the worker safety scan.
    }
    NotifyTerminalHook(relay, trigger);
    TqUvCheckTerminalConvergence(worker, relay);
}

void TqUvProcessTerminalFactsLocal(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay) noexcept {
    assert(!worker.Running_.load(std::memory_order_acquire) ||
           worker.IsLoopThread());
    const auto mask = relay.TerminalTriggerMask.exchange(
        0, std::memory_order_acq_rel);
    if (mask == 0) {
        TqUvCheckTerminalConvergence(worker, relay);
        return;
    }
    TqUvBeginTerminalLocal(worker, relay, SelectTerminalTrigger(mask));
}

void TqUvCheckTerminalConvergence(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay) noexcept {
    NotifyConvergenceHook(relay);
    if (!relay.TerminalStarted && relay.QuicFinObserved &&
        !relay.TcpWriteClosed && !relay.TcpShutdownPending &&
        relay.TcpWrites.empty() && relay.PendingTcpWriteBytes == 0) {
        relay.TcpShutdown.data = &relay;
        relay.TcpShutdownPending = true;
        int status = UV_EIO;
        try {
            status = worker.CallShutdown(
                &relay.TcpShutdown,
                reinterpret_cast<uv_stream_t*>(&relay.TcpHandle),
                &TqUvOnTcpShutdownComplete);
        } catch (...) {
        }
        if (status != 0) {
            relay.TcpShutdownPending = false;
            TqUvRequestTerminal(relay, TqUvTerminalTrigger::TcpError);
            TqUvProcessTerminalFactsLocal(worker, relay);
            return;
        }
    }
    if (!relay.TerminalStarted && NormalFinReady(relay)) {
        TqUvRequestTerminal(relay, TqUvTerminalTrigger::TcpEof);
        TqUvProcessTerminalFactsLocal(worker, relay);
        return;
    }
    if (!relay.TerminalStarted) {
        return;
    }
    RefreshHandoffResultFromOwner(relay);
    if (relay.TerminalAbortUpgradePending &&
        !relay.TerminalAbortUpgradeApplied &&
        relay.StreamOwner != nullptr) {
        const auto handoff = TerminalHandoff(relay);
        const auto upgrade = relay.StreamOwner->UpgradeTerminalShutdownToAbort(
            TqRelayStreamErrorCancelOnLoss,
            handoff != nullptr ? handoff->Escalation
                               : std::shared_ptr<TqTerminalEscalation>{});
        relay.TerminalHandoffStatus = upgrade.Status;
        if (upgrade.Submitted || upgrade.AlreadyTerminal) {
            relay.TerminalAbortUpgradeApplied = upgrade.Submitted;
        }
    }
    if (relay.TerminalHandoffRetryPending && relay.StreamOwner != nullptr &&
        relay.Binding != nullptr) {
        relay.TerminalHandoffEscalated = true;
        const auto handoff = TerminalHandoff(relay);
        const auto retry = relay.StreamOwner->BeginTerminalShutdown(
            TqRelayStreamErrorCancelOnLoss,
            relay.Binding,
            handoff != nullptr ? handoff->Escalation
                               : std::shared_ptr<TqTerminalEscalation>{},
            TqTerminalShutdownIntent::AbortBothImmediate);
        relay.Binding->TerminalRouteGeneration.store(
            relay.StreamOwner->RouteGeneration(), std::memory_order_release);
        RecordHandoffResult(relay, retry);
        if (relay.TerminalHandoffRetryPending && handoff != nullptr &&
            !handoff->HandoffFailedCounted.exchange(
                true, std::memory_order_acq_rel)) {
            TqRecordTerminalHandoffFailed();
        }
    }
    if (relay.TcpHandleInitialized && !relay.TcpClosePending &&
        !relay.TcpCloseCompleted) {
        try {
            worker.CloseRelay(relay.shared_from_this());
        } catch (...) {
            return;
        }
    }
    if (relay.TerminalAborted) {
        TqUvSettleQuicReceivesAtTerminal(relay);
    }
    relay.LocalOwnershipDrained = TqUvLocalOperationOwnershipDrained(relay);
    if (relay.QuicShutdownObserved.load(std::memory_order_acquire)) {
        relay.TerminalHandoffComplete.store(true, std::memory_order_release);
        relay.TerminalAbortUpgradePending = false;
    }
    PublishHandoffFacts(relay);
    if (!relay.TerminalReleased && relay.TcpCloseCompleted &&
        relay.TerminalHandoffComplete.load(std::memory_order_acquire) &&
        relay.LocalOwnershipDrained) {
        relay.TerminalReleased = true;
        relay.TerminalReleaseCount.fetch_add(1, std::memory_order_release);
        if (relay.StopControl != nullptr &&
            relay.StopControl->ReleaseAccountingAtBackendTerminal.load(
                std::memory_order_acquire)) {
            (void)relay.StopControl->ReleaseActiveAccountingOnce();
        }
        worker.EraseRelay(relay.RelayId);
    }
}

void TqUvOnTcpShutdownComplete(uv_shutdown_t* request, int status) {
    auto* relay = request != nullptr
        ? static_cast<TqUvRelayState*>(request->data) : nullptr;
    if (relay == nullptr || relay->Worker == nullptr) {
        return;
    }
    relay->TcpShutdownPending = false;
    if (status == 0) {
        relay->TcpWriteClosed = true;
        TqUvCheckTerminalConvergence(*relay->Worker, *relay);
    } else {
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::TcpError);
        TqUvProcessTerminalFactsLocal(*relay->Worker, *relay);
    }
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
void TqUvSetTerminalHookForTest(TqUvTerminalHookForTest hook) noexcept {
    gTerminalHookForTest.store(hook, std::memory_order_release);
}

void TqUvSetConvergenceHookForTest(TqUvConvergenceHookForTest hook) noexcept {
    gConvergenceHookForTest.store(hook, std::memory_order_release);
}
#endif
