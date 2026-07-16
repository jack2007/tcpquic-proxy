#include "libuv_relay_worker.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

std::uint64_t RemainingBytes(const TqUvPendingQuicReceive& receive) noexcept {
    return receive.TotalBytes >= receive.CompletedBytes
        ? receive.TotalBytes - receive.CompletedBytes
        : 0;
}

bool CompleteReceive(
    const std::shared_ptr<TqUvPendingQuicReceive>& receive,
    std::uint64_t bytes) noexcept {
    if (receive == nullptr || bytes == 0 || receive->Settled.load()) {
        return bytes == 0;
    }
    if (receive->StreamOwner == nullptr) {
        return false;
    }
    auto lease = receive->StreamOwner->TryAcquireReceiveApi();
    if (!lease || lease.Stream() == nullptr) {
        return false;
    }
    const auto remaining = RemainingBytes(*receive);
    const auto completed = std::min(bytes, remaining);
    if (completed == 0) {
        return true;
    }
    lease.Stream()->ReceiveComplete(completed);
    receive->CompletedBytes += completed;
    if (receive->CompletedBytes == receive->TotalBytes) {
        receive->Settled.store(true, std::memory_order_release);
    }
    return completed == bytes;
}

bool CompleteReceiveForRelay(
    TqUvRelayState& relay,
    const std::shared_ptr<TqUvPendingQuicReceive>& receive,
    std::uint64_t bytes) noexcept {
    if (!relay.QuicShutdownObserved.load(std::memory_order_acquire)) {
        return CompleteReceive(receive, bytes);
    }
    // SHUTDOWN_COMPLETE is MsQuic's terminal ownership boundary: outstanding
    // PENDING receive buffers have already been cancelled and the stream API
    // lease is no longer available. Retire only our bookkeeping obligation;
    // calling StreamReceiveComplete after this callback would be invalid.
    if (receive == nullptr || bytes == 0 || receive->Settled.load()) {
        return bytes == 0;
    }
    const auto remaining = RemainingBytes(*receive);
    if (bytes > remaining) {
        return false;
    }
    receive->CompletedBytes += bytes;
    if (receive->CompletedBytes == receive->TotalBytes) {
        receive->Settled.store(true, std::memory_order_release);
    }
    return true;
}

void AdvanceInput(TqUvPendingQuicReceive& receive, std::size_t bytes) noexcept {
    while (bytes != 0 && receive.SliceIndex < receive.Slices.size()) {
        const auto& slice = receive.Slices[receive.SliceIndex];
        const auto available = static_cast<std::size_t>(slice.Length) -
            receive.SliceOffset;
        const auto take = std::min(bytes, available);
        receive.SliceOffset += take;
        bytes -= take;
        if (receive.SliceOffset == slice.Length) {
            ++receive.SliceIndex;
            receive.SliceOffset = 0;
        }
    }
}

bool SetReceiveEnabled(TqUvRelayState& relay, bool enabled) noexcept {
    if (relay.StreamOwner == nullptr) {
        return false;
    }
    auto lease = relay.StreamOwner->TryAcquireReceiveApi();
    return lease && lease.Stream() != nullptr &&
        QUIC_SUCCEEDED(lease.Stream()->ReceiveSetEnabled(enabled));
}

void UpdateBackpressure(TqUvRelayState& relay) noexcept {
    const auto high = relay.PrecommitMaxPendingBytes;
    if (high == 0) {
        return;
    }
    const auto pressure = relay.QuicToTcpPressureBytes.load(
        std::memory_order_acquire);
    if (!relay.QuicReceivePausedByTcpBacklog && pressure >= high) {
        if (SetReceiveEnabled(relay, false)) {
            relay.QuicReceivePausedByTcpBacklog = true;
        }
        return;
    }
    const auto low = high / 2;
    if (relay.QuicReceivePausedByTcpBacklog && pressure <= low &&
        SetReceiveEnabled(relay, true)) {
        relay.QuicReceivePausedByTcpBacklog = false;
    }
}

void RequestTerminal(
    TqUvRelayState& relay,
    TqUvTerminalTrigger trigger) noexcept {
    TqUvRequestTerminal(relay, trigger);
}

void ReleaseAdmitted(TqUvRelayState& relay, std::uint64_t bytes) noexcept {
    auto current = relay.AdmittedQuicReceiveBytes.load(std::memory_order_acquire);
    for (;;) {
        const auto next = current >= bytes ? current - bytes : 0;
        if (relay.AdmittedQuicReceiveBytes.compare_exchange_weak(
                current, next, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            const auto released = current - next;
            if (released != 0 && relay.Worker != nullptr) {
                (void)relay.Worker->CompletePendingBytes(
                    relay, TqUvPendingDirection::QuicToTcp, released);
            }
            return;
        }
    }
}

bool ReservePressure(TqUvRelayState& relay, std::uint64_t bytes) noexcept {
    const auto limit = relay.PrecommitMaxPendingBytes;
    auto current = relay.QuicToTcpPressureBytes.load(std::memory_order_acquire);
    for (;;) {
        if (limit == 0 || bytes > limit || current > limit ||
            bytes > limit - current) {
            return false;
        }
        if (relay.QuicToTcpPressureBytes.compare_exchange_weak(
                current, current + bytes, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void ReleasePressure(TqUvRelayState& relay, std::uint64_t bytes) noexcept {
    auto current = relay.QuicToTcpPressureBytes.load(std::memory_order_acquire);
    for (;;) {
        const auto next = current >= bytes ? current - bytes : 0;
        if (relay.QuicToTcpPressureBytes.compare_exchange_weak(
                current, next, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

std::shared_ptr<TqUvPendingQuicReceive> FrontReceive(
    TqUvRelayState& relay) noexcept {
    if (!relay.PrecommitReceives.empty()) {
        return relay.PrecommitReceives.front();
    }
    return relay.FallbackReceiveHead;
}

void PopReceive(
    TqUvRelayState& relay,
    const std::shared_ptr<TqUvPendingQuicReceive>& pending) noexcept {
    if (!relay.PrecommitReceives.empty() &&
        relay.PrecommitReceives.front() == pending) {
        relay.PrecommitReceives.pop_front();
        if (pending->Fin) {
            relay.QuicFinObserved = true;
            if (relay.Worker != nullptr) {
                TqUvCheckTerminalConvergence(*relay.Worker, relay);
            }
        }
        return;
    }
    if (relay.FallbackReceiveHead != pending) {
        return;
    }
    relay.FallbackReceiveHead = pending->FallbackNext;
    pending->FallbackNext.reset();
    if (relay.FallbackReceiveHead == nullptr) {
        relay.FallbackReceiveTail.reset();
    }
    if (pending->Fin) {
        relay.QuicFinObserved = true;
        if (relay.Worker != nullptr) {
            TqUvCheckTerminalConvergence(*relay.Worker, relay);
        }
    }
}

void AppendFallbackReceive(
    TqUvRelayState& relay,
    const std::shared_ptr<TqUvPendingQuicReceive>& pending) noexcept {
    pending->FallbackNext.reset();
    if (relay.FallbackReceiveTail != nullptr) {
        relay.FallbackReceiveTail->FallbackNext = pending;
    } else {
        relay.FallbackReceiveHead = pending;
    }
    relay.FallbackReceiveTail = pending;
}

bool QueueReceiveOnLoop(
    TqUvRelayWorker& worker,
    const std::shared_ptr<TqUvRelayState>& relay,
    const std::shared_ptr<TqUvPendingQuicReceive>& pending) noexcept {
    bool useFallback = relay->ActiveReceiveFallbackMode;
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (relay->FailActiveReceiveQueueAdmissionsForTest != 0) {
        --relay->FailActiveReceiveQueueAdmissionsForTest;
        useFallback = true;
        relay->ActiveReceiveFallbackMode = true;
    }
#endif
    if (!useFallback) {
        try {
            relay->PrecommitReceives.push_back(pending);
        } catch (...) {
            // Stay on the allocation-free lane permanently so later commands
            // cannot overtake this receive through the primary deque.
            relay->ActiveReceiveFallbackMode = true;
            AppendFallbackReceive(*relay, pending);
        }
    } else {
        AppendFallbackReceive(*relay, pending);
    }
    relay->PendingQuicReceiveBytes += pending->TotalBytes;
    TqUvProcessQuicToTcp(worker, *relay);
    return true;
}

void ScheduleCompletionRetry(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay,
    const std::shared_ptr<TqUvPendingQuicReceive>& pending) noexcept {
    constexpr std::uint32_t TqUvMaxImmediateCompletionRetries = 4;
    if (++pending->CompletionRetryCount >=
        TqUvMaxImmediateCompletionRetries) {
        RequestTerminal(relay, TqUvTerminalTrigger::ReceiveCompletionFailure);
        return;
    }
    auto owner = relay.Binding != nullptr
        ? relay.Binding->Relay.lock()
        : std::shared_ptr<TqUvRelayState>{};
    if (owner == nullptr || relay.Worker == nullptr) {
#if !defined(TQ_UNIT_TESTING) || !TQ_UNIT_TESTING
        RequestTerminal(relay, TqUvTerminalTrigger::ReceiveCompletionFailure);
#endif
        return;
    }
    try {
        if (!worker.Post([owner](TqUvRelayWorker& current) {
                TqUvProcessQuicToTcp(current, *owner);
            })) {
            RequestTerminal(relay, TqUvTerminalTrigger::QueueFailure);
        }
    } catch (...) {
        RequestTerminal(relay, TqUvTerminalTrigger::QueueFailure);
    }
}

bool ScheduleProcessingContinuation(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay) noexcept {
    if (relay.QuicToTcpContinuationPending) {
        return true;
    }
    try {
        auto owner = relay.shared_from_this();
        relay.QuicToTcpContinuationPending = true;
        if (worker.PostDeferred([owner](TqUvRelayWorker& local) {
                owner->QuicToTcpContinuationPending = false;
                TqUvProcessQuicToTcp(local, *owner);
            })) {
            return true;
        }
    } catch (...) {
    }
    relay.QuicToTcpContinuationPending = false;
    RequestTerminal(relay, TqUvTerminalTrigger::QueueFailure);
    TqUvProcessTerminalFactsLocal(worker, relay);
    return false;
}

} // namespace

QUIC_STATUS TqUvAcceptQuicReceive(
    const std::shared_ptr<TqUvRelayState>& relay,
    MsQuicStream* stream,
    const QUIC_STREAM_EVENT& event) noexcept {
    if (relay == nullptr || stream == nullptr ||
        event.Type != QUIC_STREAM_EVENT_RECEIVE) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event.RECEIVE.TotalBufferLength == 0) {
        if ((event.RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) == 0 ||
            !relay->ActivationMutex.Lock()) {
            return QUIC_STATUS_SUCCESS;
        }
        const bool active = relay->Activation == TqUvActivation::Active &&
            (relay->Binding == nullptr ||
             relay->Binding->Activation.load(std::memory_order_acquire) ==
                 TqUvActivation::Active);
        if (active) {
            relay->QuicFinObserved.store(true, std::memory_order_release);
        }
        (void)relay->ActivationMutex.Unlock();
        if (active && relay->Worker != nullptr) {
            relay->Worker->WakeForDurableFacts();
        }
        return QUIC_STATUS_SUCCESS;
    }
    std::shared_ptr<TqUvPendingQuicReceive> pending;
    try {
        pending = std::make_shared<TqUvPendingQuicReceive>();
        pending->Stream = stream;
        pending->StreamOwner = relay->StreamOwner;
        pending->RelayId = relay->RelayId;
        pending->RouteGeneration = relay->RouteGeneration;
        pending->ControlGeneration = relay->ControlGeneration;
        pending->Fin = (event.RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        pending->Slices.reserve(event.RECEIVE.BufferCount);
        for (std::uint32_t index = 0; index < event.RECEIVE.BufferCount; ++index) {
            const auto& buffer = event.RECEIVE.Buffers[index];
            if (buffer.Length >
                std::numeric_limits<std::uint64_t>::max() - pending->TotalBytes) {
                return QUIC_STATUS_OUT_OF_MEMORY;
            }
            pending->Slices.push_back({buffer.Buffer, buffer.Length});
            pending->TotalBytes += buffer.Length;
        }
    } catch (...) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    if (pending->TotalBytes == 0 || !relay->ActivationMutex.Lock()) {
        return pending->TotalBytes == 0
            ? QUIC_STATUS_SUCCESS
            : QUIC_STATUS_OUT_OF_MEMORY;
    }
    if (relay->Binding == nullptr ||
        relay->Binding->Activation.load(std::memory_order_acquire) !=
            TqUvActivation::Active) {
        (void)relay->ActivationMutex.Unlock();
        return QUIC_STATUS_SUCCESS;
    }
    if (!ReservePressure(*relay, pending->TotalBytes)) {
        (void)relay->ActivationMutex.Unlock();
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    relay->AdmittedQuicReceiveBytes.fetch_add(
        pending->TotalBytes, std::memory_order_acq_rel);
    if (relay->Worker != nullptr && !relay->Worker->AddPendingBytes(
            *relay,
            TqUvPendingDirection::QuicToTcp,
            pending->TotalBytes)) {
        relay->AdmittedQuicReceiveBytes.fetch_sub(
            pending->TotalBytes, std::memory_order_acq_rel);
        ReleasePressure(*relay, pending->TotalBytes);
        (void)relay->ActivationMutex.Unlock();
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    (void)relay->ActivationMutex.Unlock();

    bool admitted = false;
    if (relay->Worker != nullptr) {
        try {
            admitted = relay->Worker->Post([relay, pending](TqUvRelayWorker& worker) {
                (void)QueueReceiveOnLoop(worker, relay, pending);
            });
        } catch (...) {
            admitted = false;
        }
    }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    else if (relay->DirectQuicReceiveForTest) {
        AppendFallbackReceive(*relay, pending);
        relay->PendingQuicReceiveBytes += pending->TotalBytes;
        admitted = true;
    }
#endif
    if (!admitted) {
        ReleaseAdmitted(*relay, pending->TotalBytes);
        ReleasePressure(*relay, pending->TotalBytes);
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    return QUIC_STATUS_PENDING;
}

void TqUvProcessQuicToTcp(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay) {
    try {
    if (relay.QuicToTcpContinuationPending) {
        return;
    }
    std::uint64_t decompressionCalls = 0;
    std::uint64_t decompressionInputBytes = 0;
    const auto callBudget = std::max<std::uint64_t>(
        relay.QuicToTcpCallBudget, 1);
    const auto byteBudget = relay.QuicToTcpByteBudgetPerTick == 0
        ? std::max<std::uint64_t>(relay.PrecommitMaxPendingBytes, 1)
        : relay.QuicToTcpByteBudgetPerTick;
    const auto budgetExhausted = [&]() noexcept {
        return decompressionCalls >= callBudget ||
            decompressionInputBytes >= byteBudget;
    };
    const auto recordDecompressionFailure = [&]() noexcept {
        worker.ZstdDecompressFailures_.fetch_add(1, std::memory_order_relaxed);
        worker.QuicReceiveDecompressFailures_.fetch_add(
            1, std::memory_order_relaxed);
    };
    for (;;) {
    std::shared_ptr<TqUvPendingQuicReceive> pending;
    for (;;) {
        pending = FrontReceive(relay);
        if (pending == nullptr) {
            return;
        }
        UpdateBackpressure(relay);
        if (pending->Settled.load() &&
            (relay.CompressAlgo == TqCompressAlgo::None ||
             pending->DecompressionDrained)) {
            PopReceive(relay, pending);
            continue;
        }
        const bool terminal = relay.TerminalStarted ||
            relay.QuicShutdownObserved.load(std::memory_order_acquire) ||
            relay.Binding == nullptr ||
            relay.Binding->Activation.load(std::memory_order_acquire) !=
                TqUvActivation::Active;
        if (!terminal) {
            break;
        }
        if (pending->TcpWriteSubmitted) {
            return;
        }
        const auto bytes = pending->CompletionPendingBytes != 0
            ? pending->CompletionPendingBytes
            : RemainingBytes(*pending);
        pending->CompletionPendingBytes = bytes;
        if (CompleteReceiveForRelay(relay, pending, bytes)) {
            pending->CompletionPendingBytes = 0;
            relay.PendingQuicReceiveBytes =
                relay.PendingQuicReceiveBytes >= bytes
                    ? relay.PendingQuicReceiveBytes - bytes
                    : 0;
            ReleaseAdmitted(relay, bytes);
            ReleasePressure(relay, bytes);
            if (pending->DeferredOutputBytes != 0) {
                ReleasePressure(relay, pending->DeferredOutputBytes);
                pending->DeferredOutputOwners.clear();
                pending->DeferredOutputBytes = 0;
            }
            if (pending->Settled.load(std::memory_order_acquire)) {
                PopReceive(relay, pending);
            }
            UpdateBackpressure(relay);
            continue;
        }
        ScheduleCompletionRetry(worker, relay, pending);
        return;
    }
    if (pending->TcpWriteSubmitted) {
        return;
    }

    auto operation = std::make_unique<TqUvTcpWriteOperation>();
    operation->RelayOwner = relay.Binding != nullptr
        ? relay.Binding->Relay.lock()
        : std::shared_ptr<TqUvRelayState>{};
    operation->ReceiveOwner = pending;
    operation->RelayId = relay.RelayId;
    operation->RouteGeneration = relay.RouteGeneration;
    operation->ControlGeneration = relay.ControlGeneration;

    auto submitWrite = [&](std::unique_ptr<TqUvTcpWriteOperation> write) {
        if (write == nullptr || write->Buffers.empty() ||
            write->Buffers.size() > std::numeric_limits<unsigned>::max()) {
            return false;
        }
        write->Request.data = &relay;
        write->Ownership.store(
            TqUvOperationOwnership::Submitted, std::memory_order_release);
        auto* request = &write->Request;
        const auto bytes = write->TotalBytes;
        const auto inserted = relay.TcpWrites.emplace(request, std::move(write));
        if (!inserted.second) {
            return false;
        }
        if (bytes > std::numeric_limits<std::uint64_t>::max() -
                relay.PendingTcpWriteBytes) {
            relay.TcpWrites.erase(inserted.first);
            RequestTerminal(relay, TqUvTerminalTrigger::PressureFailure);
            return false;
        }
        relay.PendingTcpWriteBytes += bytes;
        const int status = worker.CallWrite(
            request,
            reinterpret_cast<uv_stream_t*>(&relay.TcpHandle),
            inserted.first->second->Buffers.data(),
            static_cast<unsigned>(inserted.first->second->Buffers.size()),
            &TqUvOnTcpWriteComplete);
        if (status == 0) {
            if (inserted.first->second->ReceiveOwner != nullptr) {
                inserted.first->second->ReceiveOwner->TcpWriteSubmitted = true;
            }
            return true;
        }
        relay.PendingTcpWriteBytes -= bytes;
        auto failed = std::move(inserted.first->second);
        relay.TcpWrites.erase(inserted.first);
        ReleasePressure(relay, failed->PressureBytes);
        if (failed->ReceiveCompleteBytes != 0) {
            if (CompleteReceive(
                    failed->ReceiveOwner, failed->ReceiveCompleteBytes)) {
                relay.PendingQuicReceiveBytes =
                    relay.PendingQuicReceiveBytes >= failed->ReceiveCompleteBytes
                        ? relay.PendingQuicReceiveBytes - failed->ReceiveCompleteBytes
                        : 0;
                ReleaseAdmitted(relay, failed->ReceiveCompleteBytes);
                ReleasePressure(relay, failed->ReceiveCompleteBytes);
            } else if (failed->ReceiveOwner != nullptr) {
                failed->ReceiveOwner->CompletionPendingBytes =
                    failed->ReceiveCompleteBytes;
            }
        }
        RequestTerminal(relay, TqUvTerminalTrigger::TcpError);
        UpdateBackpressure(relay);
        return false;
    };

    if (pending->CompletionPendingBytes != 0) {
        const auto bytes = pending->CompletionPendingBytes;
        if (!CompleteReceive(pending, bytes)) {
            ScheduleCompletionRetry(worker, relay, pending);
            return;
        }
        pending->CompletionRetryCount = 0;
        pending->CompletionPendingBytes = 0;
        relay.PendingQuicReceiveBytes =
            relay.PendingQuicReceiveBytes >= bytes
                ? relay.PendingQuicReceiveBytes - bytes
                : 0;
        ReleaseAdmitted(relay, bytes);
        ReleasePressure(relay, bytes);
        if (pending->Settled.load() &&
            (relay.CompressAlgo == TqCompressAlgo::None ||
             pending->DecompressionDrained)) {
            PopReceive(relay, pending);
        }
        UpdateBackpressure(relay);
        if (pending->DeferredOutputBytes == 0 &&
            (relay.CompressAlgo == TqCompressAlgo::None ||
             pending->DecompressionDrained)) {
            return;
        }
        if (pending->DeferredOutputBytes != 0) {
            for (auto& owner : pending->DeferredOutputOwners) {
                operation->Buffers.push_back(uv_buf_init(
                    reinterpret_cast<char*>(owner->Data()),
                    static_cast<unsigned>(owner->Length())));
                operation->OutputOwners.push_back(std::move(owner));
            }
            pending->DeferredOutputOwners.clear();
            operation->TotalBytes = pending->DeferredOutputBytes;
            operation->PressureBytes = pending->DeferredOutputBytes;
            pending->DeferredOutputBytes = 0;
            (void)submitWrite(std::move(operation));
            return;
        }
    }

    if (relay.CompressAlgo == TqCompressAlgo::None) {
        for (std::size_t index = pending->SliceIndex;
             index < pending->Slices.size(); ++index) {
            const auto& slice = pending->Slices[index];
            const auto offset = index == pending->SliceIndex
                ? pending->SliceOffset
                : 0;
            if (offset >= slice.Length) {
                continue;
            }
            const auto length = static_cast<std::size_t>(slice.Length) - offset;
            operation->Buffers.push_back(uv_buf_init(
                reinterpret_cast<char*>(const_cast<std::uint8_t*>(slice.Data + offset)),
                static_cast<unsigned>(length)));
            operation->TotalBytes += length;
        }
        operation->ReceiveCompleteBytes = RemainingBytes(*pending);
        (void)submitWrite(std::move(operation));
        return;
    }

    if (relay.Decompressor == nullptr) {
        recordDecompressionFailure();
        RequestTerminal(relay, TqUvTerminalTrigger::DecompressionFailure);
        return;
    }
    // Match the native drain contract: consuming input is bounded progress,
    // even when no output is produced. The admitted receive bounds such work;
    // the first produced output is submitted to uv_write and yields the loop.
    // Only a true 0/0 result without NeedsMoreInput is treated as no progress.
    while (true) {
        const bool hasInput = pending->SliceIndex < pending->Slices.size();
        if (!hasInput && pending->DecompressionDrained) {
            if (pending->Settled.load()) {
                PopReceive(relay, pending);
            }
            break;
        }
        if (decompressionCalls >= callBudget ||
            (hasInput && decompressionInputBytes >= byteBudget)) {
            (void)ScheduleProcessingContinuation(worker, relay);
            return;
        }
        const std::uint8_t* input = nullptr;
        std::size_t inputLength = 0;
        if (hasInput) {
            const auto& slice = pending->Slices[pending->SliceIndex];
            input = slice.Data + pending->SliceOffset;
            const auto available = static_cast<std::size_t>(slice.Length) -
                pending->SliceOffset;
            const auto remainingByteBudget = byteBudget -
                decompressionInputBytes;
            inputLength = static_cast<std::size_t>(std::min<std::uint64_t>(
                available, remainingByteBudget));
        }
        std::vector<std::uint8_t> output(
            std::max<std::size_t>(inputLength, 64 * 1024));
        TqDecompressResult result{};
        const bool decompressed = relay.Decompressor->DecompressInto(
            input, inputLength, output.data(), output.size(), &result);
        worker.ZstdDecompressCalls_.fetch_add(1, std::memory_order_relaxed);
        if (!decompressed ||
            result.InputConsumed > inputLength ||
            result.OutputProduced > output.size()) {
            recordDecompressionFailure();
            RequestTerminal(relay, TqUvTerminalTrigger::DecompressionFailure);
            return;
        }
        worker.ZstdDecompressInputBytes_.fetch_add(
            result.InputConsumed, std::memory_order_relaxed);
        worker.ZstdDecompressOutputBytes_.fetch_add(
            result.OutputProduced, std::memory_order_relaxed);
        if (result.NeedsMoreInput) {
            worker.ZstdDecompressNeedInput_.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (result.NeedsMoreOutput) {
            worker.ZstdDecompressNeedOutput_.fetch_add(
                1, std::memory_order_relaxed);
        }
        ++decompressionCalls;
        decompressionInputBytes = result.InputConsumed >
                std::numeric_limits<std::uint64_t>::max() -
                    decompressionInputBytes
            ? std::numeric_limits<std::uint64_t>::max()
            : decompressionInputBytes + result.InputConsumed;
        if (result.OutputProduced != 0) {
            if (relay.BufferBudget.MaxPendingBufferBytes == 0) {
                relay.BufferBudget.MaxPendingBufferBytes =
                    relay.PrecommitMaxPendingBytes;
            }
            TqBufferAcquireFailure failure{};
            auto owner = TqAllocateRelayBuffer(
                &relay.BufferBudget, result.OutputProduced, &failure);
            if (!owner) {
                RequestTerminal(relay, TqUvTerminalTrigger::AllocationFailure);
                return;
            }
            std::memcpy(owner->Data(), output.data(), result.OutputProduced);
            owner->SetLength(result.OutputProduced);
            worker.DecompressedTcpBytes_.fetch_add(
                result.OutputProduced, std::memory_order_relaxed);
            if (result.OutputProduced >
                std::numeric_limits<std::uint64_t>::max() -
                    pending->DeferredOutputBytes) {
                RequestTerminal(relay, TqUvTerminalTrigger::PressureFailure);
                return;
            }
            pending->DeferredOutputOwners.push_back(std::move(owner));
            if (!ReservePressure(relay, result.OutputProduced)) {
                pending->DeferredOutputOwners.pop_back();
                RequestTerminal(relay, TqUvTerminalTrigger::PressureFailure);
                return;
            }
            pending->DeferredOutputBytes += result.OutputProduced;
            UpdateBackpressure(relay);
        }
        // Cursor and output ownership are published together before the
        // completion lease. A retry therefore never re-enters decompression.
        AdvanceInput(*pending, result.InputConsumed);
        if (result.InputConsumed != 0) {
            pending->CompletionPendingBytes = result.InputConsumed;
            if (!CompleteReceive(pending, result.InputConsumed)) {
                ScheduleCompletionRetry(worker, relay, pending);
                return;
            }
            pending->CompletionRetryCount = 0;
            pending->CompletionPendingBytes = 0;
            relay.PendingQuicReceiveBytes =
                relay.PendingQuicReceiveBytes >= result.InputConsumed
                    ? relay.PendingQuicReceiveBytes - result.InputConsumed
                    : 0;
            ReleaseAdmitted(relay, result.InputConsumed);
            ReleasePressure(relay, result.InputConsumed);
        }
        if (!hasInput && result.InputConsumed == 0 &&
            result.OutputProduced == 0) {
            if (!result.NeedsMoreInput) {
                recordDecompressionFailure();
                RequestTerminal(relay, TqUvTerminalTrigger::DecompressionFailure);
                return;
            }
            pending->DecompressionDrained = true;
        } else if (result.InputConsumed == 0 &&
                   result.OutputProduced == 0) {
            recordDecompressionFailure();
            RequestTerminal(relay, TqUvTerminalTrigger::DecompressionFailure);
            return;
        }
        if (pending->Settled.load() && pending->DecompressionDrained) {
            PopReceive(relay, pending);
        }
        UpdateBackpressure(relay);
        if (pending->DeferredOutputBytes == 0) {
            if (pending->DecompressionDrained) {
                break;
            }
            if (budgetExhausted()) {
                (void)ScheduleProcessingContinuation(worker, relay);
                return;
            }
            continue;
        }
        for (auto& owner : pending->DeferredOutputOwners) {
            operation->Buffers.push_back(uv_buf_init(
                reinterpret_cast<char*>(owner->Data()),
                static_cast<unsigned>(owner->Length())));
            operation->OutputOwners.push_back(std::move(owner));
        }
        pending->DeferredOutputOwners.clear();
        operation->TotalBytes = pending->DeferredOutputBytes;
        operation->PressureBytes = pending->DeferredOutputBytes;
        pending->DeferredOutputBytes = 0;
        (void)submitWrite(std::move(operation));
        return;
    }
    if (pending->DecompressionDrained) {
        if (FrontReceive(relay) != nullptr && budgetExhausted()) {
            (void)ScheduleProcessingContinuation(worker, relay);
            return;
        }
        continue;
    }
    recordDecompressionFailure();
    RequestTerminal(relay, TqUvTerminalTrigger::DecompressionFailure);
    return;
    }
    } catch (...) {
        RequestTerminal(relay, TqUvTerminalTrigger::AllocationFailure);
    }
}

void TqUvOnTcpWriteComplete(uv_write_t* request, int status) noexcept {
    if (request == nullptr || request->data == nullptr) {
        return;
    }
    auto* relay = static_cast<TqUvRelayState*>(request->data);
    const auto found = relay->TcpWrites.find(request);
    if (found == relay->TcpWrites.end()) {
        return;
    }
    auto operation = std::move(found->second);
    relay->TcpWrites.erase(found);
    auto expected = TqUvOperationOwnership::Submitted;
    if (!operation->Ownership.compare_exchange_strong(
            expected,
            TqUvOperationOwnership::CompletionClaimed,
            std::memory_order_acq_rel)) {
        return;
    }
    relay->PendingTcpWriteBytes =
        relay->PendingTcpWriteBytes >= operation->TotalBytes
            ? relay->PendingTcpWriteBytes - operation->TotalBytes
            : 0;
    ReleasePressure(*relay, operation->PressureBytes);
    if (operation->ReceiveOwner != nullptr) {
        operation->ReceiveOwner->TcpWriteSubmitted = false;
    }
    if (operation->ReceiveCompleteBytes != 0) {
        if (CompleteReceiveForRelay(
                *relay, operation->ReceiveOwner,
                operation->ReceiveCompleteBytes)) {
            relay->PendingQuicReceiveBytes =
                relay->PendingQuicReceiveBytes >= operation->ReceiveCompleteBytes
                    ? relay->PendingQuicReceiveBytes - operation->ReceiveCompleteBytes
                    : 0;
            ReleaseAdmitted(*relay, operation->ReceiveCompleteBytes);
            ReleasePressure(*relay, operation->ReceiveCompleteBytes);
        } else if (operation->ReceiveOwner != nullptr) {
            operation->ReceiveOwner->CompletionPendingBytes =
                operation->ReceiveCompleteBytes;
        }
    }
    if (operation->ReceiveOwner != nullptr &&
        operation->ReceiveOwner->Settled.load() &&
        (relay->CompressAlgo == TqCompressAlgo::None ||
         operation->ReceiveOwner->DecompressionDrained)) {
        PopReceive(*relay, operation->ReceiveOwner);
    }
    operation->Ownership.store(
        TqUvOperationOwnership::Completed, std::memory_order_release);
    if (status != 0) {
        RequestTerminal(*relay, TqUvTerminalTrigger::TcpError);
    } else {
        if (relay->Worker != nullptr) {
            relay->Worker->TcpWriteBytes_.fetch_add(
                operation->TotalBytes, std::memory_order_relaxed);
            TqUvCheckTerminalConvergence(*relay->Worker, *relay);
        }
    }
    UpdateBackpressure(*relay);
    if (status == 0 && relay->Worker != nullptr) {
        TqUvProcessQuicToTcp(*relay->Worker, *relay);
    }
}

void TqUvSettleQuicReceivesAtTerminal(TqUvRelayState& relay) noexcept {
    for (;;) {
        auto pending = FrontReceive(relay);
        if (pending == nullptr || pending->TcpWriteSubmitted) {
            return;
        }
        const auto bytes = RemainingBytes(*pending);
        if (bytes != 0 && !CompleteReceiveForRelay(relay, pending, bytes)) {
            pending->CompletionPendingBytes = bytes;
            return;
        }
        pending->CompletionPendingBytes = 0;
        relay.PendingQuicReceiveBytes =
            relay.PendingQuicReceiveBytes >= bytes
                ? relay.PendingQuicReceiveBytes - bytes : 0;
        ReleaseAdmitted(relay, bytes);
        ReleasePressure(relay, bytes);
        if (pending->DeferredOutputBytes != 0) {
            ReleasePressure(relay, pending->DeferredOutputBytes);
            pending->DeferredOutputOwners.clear();
            pending->DeferredOutputBytes = 0;
        }
        pending->Settled.store(true, std::memory_order_release);
        PopReceive(relay, pending);
    }
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
TqUvQuicToTcpSnapshotForTest TqUvQuicToTcpSnapshot(
    const TqUvRelayState& relay) noexcept {
    return {
        relay.PendingQuicReceiveBytes,
        relay.PendingTcpWriteBytes,
        [&relay]() noexcept {
            std::uint64_t count = relay.PrecommitReceives.size();
            for (auto current = relay.FallbackReceiveHead;
                 current != nullptr; current = current->FallbackNext) {
                ++count;
            }
            return count;
        }(),
        static_cast<std::uint64_t>(relay.TcpWrites.size()),
        relay.QuicToTcpPressureBytes.load(std::memory_order_acquire),
        relay.QuicReceivePausedByTcpBacklog,
    };
}
#endif
