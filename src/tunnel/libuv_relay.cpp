#include "relay.h"

#include "libuv_relay_worker.h"
#include "stream_lifetime.h"

#include <new>
#include <utility>

#if !defined(TQ_UNIT_TESTING) || !TQ_UNIT_TESTING
const MsQuicApi* MsQuic = nullptr;
#endif

#ifndef TCPQUIC_LIBUV_QUIC_TO_TCP_READY
#define TCPQUIC_LIBUV_QUIC_TO_TCP_READY 0
#endif
#ifndef TCPQUIC_LIBUV_TCP_TO_QUIC_READY
#define TCPQUIC_LIBUV_TCP_TO_QUIC_READY 0
#endif

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
using TqUvStopCommittedHookForTest = bool (*)(
    const std::shared_ptr<const TqUvRelayCommittedState>&);
std::atomic<TqUvStopCommittedHookForTest> gTqUvStopCommittedHookForTest{nullptr};
std::atomic<bool> gTqUvDataPlaneReadyForTest{false};
std::atomic<void (*)()> gTqUvBeforePublicationHookForTest{nullptr};

void TqUvSetStopCommittedHookForTest(TqUvStopCommittedHookForTest hook) {
    gTqUvStopCommittedHookForTest.store(hook, std::memory_order_release);
}

void TqUvSetDataPlaneReadyForTest(bool ready) {
    gTqUvDataPlaneReadyForTest.store(ready, std::memory_order_release);
}

void TqUvSetBeforePublicationHookForTest(void (*hook)()) {
    gTqUvBeforePublicationHookForTest.store(hook, std::memory_order_release);
}
#endif

namespace {

bool TqUvRelayDataPlaneReady() noexcept {
    const bool productionReady = TCPQUIC_LIBUV_QUIC_TO_TCP_READY &&
                                 TCPQUIC_LIBUV_TCP_TO_QUIC_READY;
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    return productionReady ||
           gTqUvDataPlaneReadyForTest.load(std::memory_order_acquire);
#else
    return productionReady;
#endif
}

bool TqUvAcceptCommittedStop(
    const std::shared_ptr<const TqUvRelayCommittedState>& committed) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (const auto hook =
            gTqUvStopCommittedHookForTest.load(std::memory_order_acquire)) {
        return hook(committed);
    }
#endif
    return TqUvRelayRuntime::Instance().StopRelay(committed);
}

bool TqUvRelayStartManagedImpl(
    TqSocketHandle tcpSocket,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& profileTuning,
    TqCompressAlgo compressAlgo,
    bool* tcpFdConsumed) {
    if (tcpFdConsumed != nullptr) {
        *tcpFdConsumed = false;
    }
    if (!TqUvRelayDataPlaneReady()) {
        return false;
    }
    if (tcpSocket == TqInvalidSocket || stream == nullptr ||
        streamOwner == nullptr || handle == nullptr ||
        handle->Backend != TqRelayBackendType::None ||
        handle->Stop.load(std::memory_order_acquire)) {
        return false;
    }

    const std::uint32_t activeRelays = TqRelayRegisterActive();
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, activeRelays);
    if (!TqUvRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }
    auto* worker = TqUvRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    auto control = handle->Control;
    if (control == nullptr) {
        try {
            control = std::make_shared<TqRelayStopControl>();
        } catch (const std::bad_alloc&) {
            TqRelayUnregisterActive();
            return false;
        }
    }
    TqUvRelayRegistration registration{};
    registration.TcpSocket = tcpSocket;
    registration.Stream = stream;
    registration.StreamOwner = std::move(streamOwner);
    registration.StopControl = control;
    registration.ControlGeneration = control->Generation;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.CompressAlgo = compressAlgo;
    registration.PrecommitMaxPendingBytes = tuning.RelayPerTunnelPendingBytes;
    registration.TcpReadChunkSize = tuning.RelayReadChunkSize;
    registration.MaxPendingBufferBytes = tuning.MaxPendingBufferBytesPerRelay;
    registration.MaxBufferedQuicSendBytes =
        tuning.MaxPendingBufferBytesPerRelay;
    registration.ResumeBufferedQuicSendBytes =
        tuning.MaxPendingBufferBytesPerRelay / 2;
    registration.WorkerEventBudget = tuning.RelayWorkerEventBudget;
    registration.WorkerByteBudgetPerTick =
        tuning.RelayWorkerByteBudgetPerTick;
    registration.ReleaseAccountingOnFailedRegistration = true;
    const auto registered = worker->RegisterRelayWithId(std::move(registration));
    if (tcpFdConsumed != nullptr) {
        *tcpFdConsumed = registered.TcpFdConsumed;
    }
    if (!registered.Ok || registered.Committed == nullptr) {
        if (registered.TerminalCleanupPending) {
            control->ReleaseAccountingAtBackendTerminal.store(
                true, std::memory_order_release);
        } else {
            (void)control->ReleaseActiveAccountingOnce();
        }
        return false;
    }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (const auto hook =
            gTqUvBeforePublicationHookForTest.load(std::memory_order_acquire)) {
        hook();
    }
#endif
    if (control->Stop.load(std::memory_order_acquire) ||
        handle->Stop.load(std::memory_order_acquire)) {
        control->ReleaseAccountingAtBackendTerminal.store(
            true, std::memory_order_release);
        (void)control->SignalStop(control->Generation);
        (void)TqUvAcceptCommittedStop(registered.Committed);
        return false;
    }

    handle->Control = control;
    handle->ControlGeneration = control->Generation;
    handle->Backend = TqRelayBackendType::LibuvWorker;
    std::atomic_store(&handle->LibuvCommitted, registered.Committed);
    if (handle->Stop.load(std::memory_order_acquire)) {
        TqRelayStop(handle);
        return false;
    }
    return true;
}

} // namespace

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    return TqUvRelayStartManagedImpl(
        tcpFd, stream, nullptr, compressor, decompressor, handle, tuning,
        compressAlgo, nullptr);
}

bool TqRelayStartManaged(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo,
    bool* tcpFdConsumed) {
    return TqUvRelayStartManagedImpl(
        tcpFd, stream, std::move(streamOwner), compressor, decompressor,
        handle, tuning, compressAlgo, tcpFdConsumed);
}

bool TqRelayStartQuicReceiveSink(
    MsQuicStream*,
    TqRelayHandle*,
    const TqTuningConfig&,
    std::atomic<std::uint64_t>*) {
    return false;
}

bool TqRelayStartQuicReceiveSinkManaged(
    MsQuicStream*,
    std::shared_ptr<TqStreamLifetime>,
    TqRelayHandle*,
    const TqTuningConfig&,
    std::atomic<std::uint64_t>*) {
    return false;
}

void TqRelayStop(TqRelayHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->Stop.store(true, std::memory_order_release);
    auto committed = std::atomic_load(&handle->LibuvCommitted);
    if (committed == nullptr) {
        return;
    }
    if (committed->Control == nullptr ||
        !committed->Control->SignalStop(committed->ControlGeneration) ||
        !TqUvAcceptCommittedStop(committed)) {
        return;
    }
    const auto handoff = std::atomic_load(
        &committed->Control->TerminalHandoff);
    const auto facts = handoff != nullptr
        ? handoff->Snapshot()
        : TqTerminalHandoffFacts{};
    if (!TqRelayBackendReleaseReady(
            TqRelayBackendType::LibuvWorker,
            committed->Control->Stop.load(std::memory_order_acquire),
            handoff != nullptr ? &facts : nullptr)) {
        return;
    }
    auto expected = committed;
    if (!std::atomic_compare_exchange_strong(
            &handle->LibuvCommitted,
            &expected,
            std::shared_ptr<const TqUvRelayCommittedState>{})) {
        return;
    }
    handle->Backend = TqRelayBackendType::None;
    (void)committed->Control->ReleaseActiveAccountingOnce();
}

void TqRelaySetTraceContext(TqRelayHandle*, std::uint64_t, const char*) {}

bool TqRelayLinuxFastPathEnabled(const TqRelayHandle*) {
    return false;
}
