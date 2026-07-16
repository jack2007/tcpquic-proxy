#include "libuv_relay_worker.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace {

void ReleasePendingSend(
    TqUvRelayState& relay,
    std::uint64_t bytes) noexcept {
    const auto released = std::min(relay.PendingQuicSendBytes, bytes);
    relay.PendingQuicSendBytes -= released;
    if (released != 0 && relay.Worker != nullptr) {
        (void)relay.Worker->CompletePendingBytes(
            relay, TqUvPendingDirection::TcpToQuic, released);
    }
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
std::atomic<TqUvStreamSendHookForTest> gStreamSendHookForTest{nullptr};
#endif

enum class SubmitResult : std::uint8_t {
    Submitted,
    ResourceBlocked,
    Fatal,
};

bool IsResourceStatus(QUIC_STATUS status) noexcept {
    return status == QUIC_STATUS_OUT_OF_MEMORY ||
           status == QUIC_STATUS_BUFFER_TOO_SMALL;
}

QUIC_STATUS StreamSend(
    TqUvRelayState& relay,
    const QUIC_BUFFER* buffers,
    std::uint32_t count,
    QUIC_SEND_FLAGS flags,
    void* context) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (const auto hook = gStreamSendHookForTest.load(std::memory_order_acquire)) {
        return hook(relay.Stream, buffers, count, flags, context);
    }
#endif
    if (relay.StreamOwner == nullptr) {
        return QUIC_STATUS_INVALID_STATE;
    }
    auto lease = relay.StreamOwner->TryAcquireSendApi();
    if (!lease || lease.Stream() == nullptr) {
        return QUIC_STATUS_INVALID_STATE;
    }
    return lease.Stream()->Send(buffers, count, flags, context);
}

bool AllocateViews(
    TqUvRelayState& relay,
    const std::vector<std::uint8_t>& bytes,
    std::vector<TqBufferView>& views) noexcept {
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t chunk = std::min(
                relay.TcpReadChunkSize, bytes.size() - offset);
            TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
            auto owner = TqAllocateRelayBuffer(
                &relay.TcpReadBufferBudget, chunk, &failure);
            if (!owner) {
                return false;
            }
            std::memcpy(owner->Data(), bytes.data() + offset, chunk);
            owner->SetLength(chunk);
            auto* data = owner->Data();
            views.emplace_back(data, chunk, std::move(owner));
            offset += chunk;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool BuildViews(
    TqUvRelayState& relay,
    TqBufferRef input,
    std::size_t length,
    bool endStream,
    std::vector<TqBufferView>& views,
    std::uint64_t& compressedBytes,
    bool& compressionFailed) noexcept {
    try {
        if (relay.Compressor == nullptr ||
            relay.CompressAlgo == TqCompressAlgo::None) {
            if (length != 0) {
                input->SetLength(length);
                auto* data = input->Data();
                views.emplace_back(data, length, std::move(input));
            }
            return true;
        }

        relay.CompressionOutput.clear();
        const std::uint8_t* data = input ? input->Data() : nullptr;
        if (!relay.Compressor->Compress(
                data, length, relay.CompressionOutput, endStream)) {
            compressionFailed = true;
            return false;
        }
        if (!endStream && relay.CompressionOutput.empty() &&
            !relay.Compressor->Flush(relay.CompressionOutput)) {
            compressionFailed = true;
            return false;
        }
        compressedBytes = relay.CompressionOutput.size();
        input.reset();
        return AllocateViews(relay, relay.CompressionOutput, views);
    } catch (...) {
        return false;
    }
}

SubmitResult TrySubmitOperation(
    TqUvRelayState& relay,
    std::unique_ptr<TqUvQuicSendOperation> operation,
    bool backlogAccounted) noexcept {
    const auto operationBytes = operation ? operation->TotalBytes : 0;
    bool inserted = false;
    try {
        auto* delivered = operation.get();
        operation->Reservation =
            relay.StreamOwner->ReserveSendCompletion(delivered);
        if (!operation->Reservation) {
            if (backlogAccounted) {
                ReleasePendingSend(relay, operation->TotalBytes);
            }
            return SubmitResult::Fatal;
        }
        void* completionKey = operation->Reservation.Key();
        operation->Ownership.store(
            TqUvOperationOwnership::Submitted, std::memory_order_release);
        relay.QuicSends.emplace(delivered, std::move(operation));
        inserted = true;

        auto submitted = relay.QuicSends.find(delivered);
        if (!backlogAccounted) {
            if (relay.Worker != nullptr && !relay.Worker->AddPendingBytes(
                    relay,
                    TqUvPendingDirection::TcpToQuic,
                    submitted->second->TotalBytes)) {
                auto failed = std::move(submitted->second);
                relay.QuicSends.erase(submitted);
                (void)failed->Reservation.Cancel();
                return SubmitResult::Fatal;
            }
            relay.PendingQuicSendBytes += submitted->second->TotalBytes;
        }
        if (submitted->second->Fin) {
            relay.QuicFinSubmitted = true;
        }
        const auto status = StreamSend(
            relay,
            submitted->second->QuicBuffers.empty()
                ? nullptr : submitted->second->QuicBuffers.data(),
            static_cast<std::uint32_t>(submitted->second->QuicBuffers.size()),
            submitted->second->Fin
                ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE,
            completionKey);

        // A synchronous completion may have consumed and erased the operation.
        // Its once-only completion fact wins even if the downcall reports error.
        submitted = relay.QuicSends.find(delivered);
        if (submitted == relay.QuicSends.end()) {
            return SubmitResult::Submitted;
        }
        if (submitted->second->CompletionQueued.load(std::memory_order_acquire)) {
            submitted->second->Reservation.Dismiss();
            return SubmitResult::Submitted;
        }
        if (QUIC_SUCCEEDED(status)) {
            submitted->second->Reservation.Dismiss();
            return SubmitResult::Submitted;
        }

        auto retained = std::move(submitted->second);
        relay.QuicSends.erase(submitted);
        (void)retained->Reservation.Cancel();
        retained->Ownership.store(
            TqUvOperationOwnership::Created, std::memory_order_release);
        if (retained->Fin) {
            relay.QuicFinSubmitted = false;
        }
        if (IsResourceStatus(status)) {
            const auto retainedBytes = retained->TotalBytes;
            try {
                relay.PendingQuicSendRetries.push_back(std::move(retained));
                return SubmitResult::ResourceBlocked;
            } catch (...) {
                ReleasePendingSend(relay, retainedBytes);
                return SubmitResult::Fatal;
            }
        }
        ReleasePendingSend(relay, retained->TotalBytes);
        return SubmitResult::Fatal;
    } catch (...) {
        if (backlogAccounted && !inserted) {
            ReleasePendingSend(relay, operationBytes);
        }
        return SubmitResult::Fatal;
    }
}

SubmitResult Submit(
    TqUvRelayState& relay,
    std::vector<TqBufferView> views,
    bool fin) noexcept {
    try {
        if (relay.StreamOwner == nullptr || relay.TerminalStarted) {
            return SubmitResult::Fatal;
        }

        auto operation = std::unique_ptr<TqUvQuicSendOperation>(
            new (std::nothrow) TqUvQuicSendOperation{});
        if (!operation) {
            return SubmitResult::Fatal;
        }
        operation->RelayOwner = relay.shared_from_this();
        operation->RelayId = relay.RelayId;
        operation->RouteGeneration = relay.RouteGeneration;
        operation->ControlGeneration = relay.ControlGeneration;
        operation->Fin = fin;
        operation->Views = std::move(views);
        operation->QuicBuffers.reserve(operation->Views.size());
        for (const auto& view : operation->Views) {
            if (view.Len > std::numeric_limits<std::uint32_t>::max()) {
                return SubmitResult::Fatal;
            }
            operation->QuicBuffers.push_back(QUIC_BUFFER{
                static_cast<std::uint32_t>(view.Len), view.Data});
            operation->TotalBytes += view.Len;
        }

        return TrySubmitOperation(relay, std::move(operation), false);
    } catch (...) {
        // The operation's armed reservation rolls back pre-submit failures.
        // Post-insertion failures are converted to terminal by the caller.
        return SubmitResult::Fatal;
    }
}

} // namespace

void TqUvHandleTcpRead(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay,
    ssize_t received,
    const uv_buf_t& buffer) {
    TqBufferRef owner;
    if (buffer.base != nullptr) {
        const auto found = relay.TcpReadBuffers.find(buffer.base);
        if (found != relay.TcpReadBuffers.end()) {
            owner = std::move(found->second);
            relay.TcpReadBuffers.erase(found);
        }
    }

    if (received == UV_ENOBUFS) {
        const auto failure = relay.TcpReadAcquireFailure;
        relay.TcpReadAcquireFailure = TqBufferAcquireFailure::None;
        if (failure == TqBufferAcquireFailure::PendingBytesLimit) {
            if (relay.TcpReadStarted &&
                worker.CallReadStop(reinterpret_cast<uv_stream_t*>(
                    &relay.TcpHandle)) != 0) {
                TqUvRequestTerminal(
                    relay, TqUvTerminalTrigger::TcpError);
                TqUvProcessTerminalFactsLocal(worker, relay);
                return;
            }
            relay.TcpReadStarted = false;
            relay.TcpReadPausedByQuicBacklog = true;
            return;
        }
        TqUvRequestTerminal(
            relay, TqUvTerminalTrigger::AllocationFailure);
        TqUvProcessTerminalFactsLocal(worker, relay);
        return;
    }

    if (received > 0) {
        if (!owner || static_cast<std::size_t>(received) > owner->Capacity()) {
            TqUvRequestTerminal(relay, TqUvTerminalTrigger::TcpError);
            TqUvProcessTerminalFactsLocal(worker, relay);
            return;
        }
        std::vector<TqBufferView> views;
        std::uint64_t compressedBytes = 0;
        bool compressionFailed = false;
        worker.TcpReadBytes_.fetch_add(
            static_cast<std::uint64_t>(received), std::memory_order_relaxed);
        const bool built = BuildViews(
                relay, std::move(owner), static_cast<std::size_t>(received),
                false, views, compressedBytes, compressionFailed);
        worker.CompressedTcpBytes_.fetch_add(
            compressedBytes, std::memory_order_relaxed);
        if (!built) {
            if (compressionFailed) {
                worker.TcpToQuicCompressFailures_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            TqUvRequestTerminal(relay, TqUvTerminalTrigger::AllocationFailure);
            TqUvProcessTerminalFactsLocal(worker, relay);
            return;
        }
        if (views.empty()) {
            return;
        }
        const auto result = Submit(relay, std::move(views), false);
        if (result == SubmitResult::Fatal) {
            TqUvRequestTerminal(relay, TqUvTerminalTrigger::QuicAbort);
            TqUvProcessTerminalFactsLocal(worker, relay);
            return;
        }
        const bool mustPause = result == SubmitResult::ResourceBlocked ||
            (relay.MaxBufferedQuicSendBytes != 0 &&
             relay.PendingQuicSendBytes >= relay.MaxBufferedQuicSendBytes);
        if (mustPause && relay.TcpReadStarted) {
            if (worker.CallReadStop(
                    reinterpret_cast<uv_stream_t*>(&relay.TcpHandle)) == 0) {
                relay.TcpReadStarted = false;
                relay.TcpReadPausedByQuicBacklog = true;
            } else {
                TqUvRequestTerminal(relay, TqUvTerminalTrigger::TcpError);
                TqUvProcessTerminalFactsLocal(worker, relay);
            }
        }
        return;
    }

    if (received == 0) {
        return;
    }
    if (received != UV_EOF) {
        relay.TcpReadClosed = true;
        TqUvRequestTerminal(relay, TqUvTerminalTrigger::TcpError);
        TqUvProcessTerminalFactsLocal(worker, relay);
        return;
    }

    relay.TcpReadClosed = true;
    if (relay.TcpReadStarted) {
        (void)worker.CallReadStop(
            reinterpret_cast<uv_stream_t*>(&relay.TcpHandle));
        relay.TcpReadStarted = false;
    }
    std::vector<TqBufferView> views;
    std::uint64_t compressedBytes = 0;
    bool compressionFailed = false;
    const bool built = BuildViews(
        relay, {}, 0, true, views, compressedBytes, compressionFailed);
    worker.CompressedTcpBytes_.fetch_add(
        compressedBytes, std::memory_order_relaxed);
    if (!built) {
        if (compressionFailed) {
            worker.TcpToQuicCompressFailures_.fetch_add(
                1, std::memory_order_relaxed);
        }
        TqUvRequestTerminal(relay, TqUvTerminalTrigger::AllocationFailure);
        TqUvProcessTerminalFactsLocal(worker, relay);
        return;
    }
    const auto result = Submit(relay, std::move(views), true);
    if (result == SubmitResult::Fatal) {
        TqUvRequestTerminal(relay, TqUvTerminalTrigger::QuicAbort);
        TqUvProcessTerminalFactsLocal(worker, relay);
    } else if (result == SubmitResult::ResourceBlocked) {
        relay.TcpReadPausedByQuicBacklog = true;
    }
}

void TqUvRetryPendingQuicSends(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay) noexcept {
    while (!relay.TerminalStarted &&
           !relay.PendingQuicSendRetries.empty()) {
        auto operation = std::move(relay.PendingQuicSendRetries.front());
        relay.PendingQuicSendRetries.pop_front();
        const auto result =
            TrySubmitOperation(relay, std::move(operation), true);
        if (result == SubmitResult::ResourceBlocked) {
            return;
        }
        if (result == SubmitResult::Fatal) {
            TqUvRequestTerminal(relay, TqUvTerminalTrigger::QuicAbort);
            TqUvProcessTerminalFactsLocal(worker, relay);
            return;
        }
    }
}

void TqUvHandleSendComplete(
    TqUvRelayWorker& worker,
    TqUvQuicSendOperation& operation,
    bool cancelled) noexcept {
    auto relay = operation.RelayOwner;
    if (!relay) {
        return;
    }
    const auto found = relay->QuicSends.find(&operation);
    if (found == relay->QuicSends.end()) {
        return;
    }
    const bool current = relay->RelayId == operation.RelayId &&
                         relay->RouteGeneration == operation.RouteGeneration &&
                         relay->ControlGeneration == operation.ControlGeneration;
    const auto bytes = operation.TotalBytes;
    const bool fin = operation.Fin;
    operation.Ownership.store(
        cancelled ? TqUvOperationOwnership::Cancelled
                  : TqUvOperationOwnership::Completed,
        std::memory_order_release);
    relay->QuicSends.erase(found);
    ReleasePendingSend(*relay, bytes);
    if (!current) {
        TqUvCheckTerminalConvergence(worker, *relay);
        return;
    }
    if (cancelled) {
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::QuicAbort);
        TqUvProcessTerminalFactsLocal(worker, *relay);
        return;
    }
    if (fin && !cancelled) {
        relay->QuicFinCompleted = true;
        TqUvCheckTerminalConvergence(worker, *relay);
    }
    TqUvRetryPendingQuicSends(worker, *relay);
    const auto pendingBufferBytes =
        relay->TcpReadBufferBudget.PendingBufferBytes.load(
            std::memory_order_relaxed);
    if (!relay->TerminalStarted && relay->TcpReadPausedByQuicBacklog &&
        !relay->TcpReadClosed &&
        relay->PendingQuicSendBytes <= relay->ResumeBufferedQuicSendBytes &&
        pendingBufferBytes <= relay->ResumeBufferedQuicSendBytes) {
        const int status = worker.CallReadStart(
            reinterpret_cast<uv_stream_t*>(&relay->TcpHandle),
            &TqUvOnTcpAlloc,
            &TqUvOnTcpRead);
        if (status == 0) {
            relay->TcpReadStarted = true;
            relay->TcpReadPausedByQuicBacklog = false;
        } else {
            TqUvRequestTerminal(*relay, TqUvTerminalTrigger::TcpError);
            TqUvProcessTerminalFactsLocal(worker, *relay);
        }
    }
    TqUvCheckTerminalConvergence(worker, *relay);
}

bool TqUvAllocateTcpReadBuffer(
    TqUvRelayState& relay,
    std::size_t suggested,
    uv_buf_t& output,
    TqBufferAcquireFailure* failure) noexcept {
    output = uv_buf_init(nullptr, 0);
    if (failure != nullptr) {
        *failure = TqBufferAcquireFailure::None;
    }
    if (relay.TcpReadChunkSize == 0 ||
        relay.TcpReadBufferBudget.MaxPendingBufferBytes == 0) {
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::AllocationFailure;
        }
        return false;
    }
    try {
        const auto size = std::min(
            relay.TcpReadChunkSize,
            suggested == 0 ? relay.TcpReadChunkSize : suggested);
        TqBufferAcquireFailure localFailure =
            TqBufferAcquireFailure::None;
        auto owner = TqAllocateRelayBuffer(
            &relay.TcpReadBufferBudget, size, &localFailure);
        if (!owner) {
            if (failure != nullptr) {
                *failure = localFailure;
            }
            return false;
        }
        auto* data = reinterpret_cast<char*>(owner->Data());
        relay.TcpReadBuffers.emplace(data, std::move(owner));
        output = uv_buf_init(data, static_cast<unsigned int>(size));
        return true;
    } catch (...) {
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::AllocationFailure;
        }
        return false;
    }
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
void TqUvSetStreamSendHookForTest(TqUvStreamSendHookForTest hook) noexcept {
    gStreamSendHookForTest.store(hook, std::memory_order_release);
}

uv_buf_t TqUvStageTcpReadBufferForTest(
    TqUvRelayState& relay,
    std::size_t size) {
    auto owner = TqAllocateRelayBuffer(&relay.TcpReadBufferBudget, size);
    if (!owner) {
        return uv_buf_init(nullptr, 0);
    }
    auto* data = reinterpret_cast<char*>(owner->Data());
    relay.TcpReadBuffers.emplace(data, std::move(owner));
    return uv_buf_init(data, static_cast<unsigned int>(size));
}
#endif
