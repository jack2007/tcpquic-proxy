#include "relay.h"

#if defined(__linux__)
#include "linux_relay_worker.h"
#endif
#if defined(_WIN32)
#include "windows_relay_worker.h"
#endif
#if defined(__APPLE__)
#include "darwin_relay_worker.h"
#endif

#include "msquic.hpp"
#include "tuning.h"

#include <new>

namespace {
bool TqRelayStartImpl(
    TqSocketHandle tcpFd,
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
    if (handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }

#if defined(_WIN32)
    (void)stream;
    const uint32_t reservedRelays = TqGetActiveRelayCount() + 1;
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, reservedRelays);

    if (streamOwner == nullptr) {
        return false;
    }
    if (!TqWindowsRelayRuntime::Instance().Start(tuning)) {
        return false;
    }

    TqWindowsRelayRegistration registration{};
    registration.TcpFd = tcpFd;
    registration.StreamOwner = std::move(streamOwner);
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.Tuning = tuning;
    registration.CompressAlgo = compressAlgo;

    const auto registered = TqWindowsRelayRuntime::Instance().RegisterRelay(registration);
    if (tcpFdConsumed != nullptr) {
        *tcpFdConsumed = registered.TcpFdConsumed;
    }
    if (!registered.Ok) {
        return false;
    }

    handle->Stop.store(false);
    handle->Control = registered.StopControl;
    handle->Backend = TqRelayBackendType::WindowsWorker;
    handle->WindowsWorker = registered.Worker;
    handle->WindowsRelayId = registered.RelayId;
    handle->WindowsRelayGeneration = registered.RelayGeneration;
    handle->WindowsWorkerIndex = registered.WorkerIndex;
    TqRelayRegisterActive();
    return true;
#elif defined(__linux__)
    const uint32_t activeRelays = TqRelayRegisterActive();
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, activeRelays);
    // Linux production relay requires the manual-cleanup lifetime owner.  The
    // unowned entry point remains available for non-Linux backends, but must
    // never install a handler on a started Linux wrapper.
    if (streamOwner == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }
    if (!TqLinuxRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }

    TqLinuxRelayWorker* worker = TqLinuxRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    TqLinuxRelayRegistration registration{};
    auto control = std::make_shared<TqRelayStopControl>();
    registration.TcpFd = tcpFd;
    registration.Stream = stream;
    registration.StreamOwner = std::move(streamOwner);
    registration.StopControl = control;
    registration.ControlGeneration = control->Generation;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.CompressAlgo = compressAlgo;
    registration.EnableQuicSends = true;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (tcpFdConsumed != nullptr) {
        *tcpFdConsumed = registered.TcpFdConsumed;
    }
    if (!registered.Ok) {
        TqRelayUnregisterActive();
        return false;
    }

    if (control->Stop.load(std::memory_order_acquire)) {
        worker->UnregisterRelay(registered.RelayId);
        TqRelayUnregisterActive();
        return false;
    }

    auto committed = std::shared_ptr<TqRelayLinuxCommittedState>(
        new (std::nothrow) TqRelayLinuxCommittedState{});
    if (committed == nullptr) {
        worker->UnregisterRelay(registered.RelayId);
        TqRelayUnregisterActive();
        return false;
    }
    committed->WorkerIdentity = reinterpret_cast<uintptr_t>(worker);
    committed->RelayId = registered.RelayId;
    committed->WorkerIndex = worker->WorkerIndex();
    committed->Control = control;
    committed->ControlGeneration = control->Generation;

    handle->Stop.store(false);
    handle->Control = control;
    handle->ControlGeneration = control->Generation;
    handle->LinuxWorker = worker;
    handle->LinuxRelayId = registered.RelayId;
    handle->LinuxWorkerIndex = worker->WorkerIndex();
    std::atomic_store(
        &handle->LinuxCommitted,
        std::shared_ptr<const TqRelayLinuxCommittedState>(std::move(committed)));
    handle->Backend = TqRelayBackendType::LinuxWorker;
    return true;
#elif defined(__APPLE__)
    if (streamOwner == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }
    if (!TqDarwinRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }

    TqDarwinRelayWorker* worker = TqDarwinRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    auto control = std::make_shared<TqRelayStopControl>();
    const uint64_t generation = control->Generation;

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = tcpFd;
    registration.Stream = stream;
    registration.StreamOwner = std::move(streamOwner);
    registration.Control = control;
    registration.ControlGeneration = generation;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.CompressAlgo = compressAlgo;
    registration.EnableQuicSends = true;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (tcpFdConsumed != nullptr) {
        *tcpFdConsumed = registered.TcpFdConsumed;
    }
    if (!registered.Ok) {
        (void)control->ReleaseActiveAccountingOnce();
        return false;
    }

    if (control->Stop.load(std::memory_order_acquire)) {
        worker->UnregisterRelay(registered.RelayId);
        (void)control->ReleaseActiveAccountingOnce();
        return false;
    }

    handle->Stop.store(false, std::memory_order_release);
    handle->Control = control;
    handle->ControlGeneration = generation;
    handle->Backend = TqRelayBackendType::DarwinWorker;
    handle->DarwinWorker = worker;
    handle->DarwinRelayId = registered.RelayId;

    if (control->Stop.load(std::memory_order_acquire)) {
        TqRelayStop(handle);
        return false;
    }
    return true;
#else
    (void)tcpFd;
    (void)stream;
    (void)compressor;
    (void)decompressor;
    (void)compressAlgo;
    TqRelayUnregisterActive();
    return false;
#endif
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
    return TqRelayStartImpl(
        tcpFd, stream, nullptr, compressor, decompressor, handle, tuning, compressAlgo, nullptr);
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
    return TqRelayStartImpl(
        tcpFd,
        stream,
        std::move(streamOwner),
        compressor,
        decompressor,
        handle,
        tuning,
        compressAlgo,
        tcpFdConsumed);
}

namespace {
bool TqRelayStartQuicReceiveSinkImpl(
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    TqRelayHandle* handle,
    const TqTuningConfig& profileTuning,
    std::atomic<uint64_t>* receiveBytes) {
    if (stream == nullptr || handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }

    const uint32_t activeRelays = TqRelayRegisterActive();
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, activeRelays);

#if defined(__linux__)
    if (streamOwner == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }
    if (!TqLinuxRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }

    TqLinuxRelayWorker* worker = TqLinuxRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    TqLinuxRelayRegistration registration{};
    auto control = std::make_shared<TqRelayStopControl>();
    registration.TcpFd = -1;
    registration.Stream = stream;
    registration.StreamOwner = std::move(streamOwner);
    registration.StopControl = control;
    registration.ControlGeneration = control->Generation;
    registration.EnableQuicSends = false;
    registration.SinkQuicReceives = true;
    registration.SinkQuicReceiveBytes = receiveBytes;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (!registered.Ok) {
        TqRelayUnregisterActive();
        return false;
    }

    if (control->Stop.load(std::memory_order_acquire)) {
        worker->UnregisterRelay(registered.RelayId);
        TqRelayUnregisterActive();
        return false;
    }

    auto committed = std::shared_ptr<TqRelayLinuxCommittedState>(
        new (std::nothrow) TqRelayLinuxCommittedState{});
    if (committed == nullptr) {
        worker->UnregisterRelay(registered.RelayId);
        TqRelayUnregisterActive();
        return false;
    }
    committed->WorkerIdentity = reinterpret_cast<uintptr_t>(worker);
    committed->RelayId = registered.RelayId;
    committed->WorkerIndex = worker->WorkerIndex();
    committed->Control = control;
    committed->ControlGeneration = control->Generation;

    handle->Stop.store(false);
    handle->Control = control;
    handle->ControlGeneration = control->Generation;
    handle->LinuxWorker = worker;
    handle->LinuxRelayId = registered.RelayId;
    handle->LinuxWorkerIndex = worker->WorkerIndex();
    std::atomic_store(
        &handle->LinuxCommitted,
        std::shared_ptr<const TqRelayLinuxCommittedState>(std::move(committed)));
    handle->Backend = TqRelayBackendType::LinuxWorker;
    return true;
#else
    (void)streamOwner;
    (void)profileTuning;
    (void)receiveBytes;
    TqRelayUnregisterActive();
    return false;
#endif
}
} // namespace

bool TqRelayStartQuicReceiveSink(
    MsQuicStream* stream,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes) {
    return TqRelayStartQuicReceiveSinkImpl(
        stream, nullptr, handle, tuning, receiveBytes);
}

bool TqRelayStartQuicReceiveSinkManaged(
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes) {
    return TqRelayStartQuicReceiveSinkImpl(
        stream, std::move(streamOwner), handle, tuning, receiveBytes);
}

void TqRelaySetTraceContext(TqRelayHandle* handle, uint64_t tunnelId, const char* target) {
    if (handle == nullptr || tunnelId == 0) {
        return;
    }
#if defined(_WIN32)
    if (handle->Backend == TqRelayBackendType::WindowsWorker && handle->WindowsRelayId != 0) {
        const auto control = handle->Control;
        const uint64_t controlGeneration =
            control != nullptr ? control->Generation.load(std::memory_order_acquire) : 0;
        TqWindowsRelayRuntime::Instance().SetRelayTraceContext(
            control,
            handle->WindowsWorkerIndex,
            handle->WindowsRelayId,
            handle->WindowsRelayGeneration,
            controlGeneration,
            tunnelId,
            target);
    }
#else
    (void)target;
#endif
}

void TqRelayStop(TqRelayHandle* handle) {
    if (handle == nullptr) {
        return;
    }

#if defined(_WIN32)
    if (handle->Backend == TqRelayBackendType::WindowsWorker) {
        const auto control = handle->Control;
        const uint32_t workerIndex = handle->WindowsWorkerIndex;
        const uint64_t relayId = handle->WindowsRelayId;
        const uint64_t relayGeneration = handle->WindowsRelayGeneration;
        const uint64_t controlGeneration =
            control != nullptr ? control->Generation.load(std::memory_order_acquire) : 0;
        handle->Backend = TqRelayBackendType::None;
        handle->WindowsWorker = nullptr;
        handle->WindowsRelayId = 0;
        handle->WindowsRelayGeneration = 0;
        handle->WindowsWorkerIndex = 0;
        handle->Stop.store(true);
        if (control != nullptr) {
            control->Stop.store(true, std::memory_order_release);
        }
        if (relayId != 0) {
            TqWindowsRelayRuntime::Instance().StopRelay(
                control, workerIndex, relayId, relayGeneration, controlGeneration);
        }
        TqRelayUnregisterActive();
        handle->Control.reset();
        return;
    }
#endif

#if defined(__linux__)
    auto linuxCommitted = std::atomic_exchange(
        &handle->LinuxCommitted,
        std::shared_ptr<const TqRelayLinuxCommittedState>{});
    if (linuxCommitted != nullptr || handle->Backend == TqRelayBackendType::LinuxWorker) {
        TqLinuxRelayWorker* const legacyWorker = handle->LinuxWorker;
        const uint64_t relayId = linuxCommitted != nullptr
            ? linuxCommitted->RelayId
            : handle->LinuxRelayId;
        auto control = linuxCommitted != nullptr
            ? linuxCommitted->Control
            : handle->Control;
        const uint64_t generation = linuxCommitted != nullptr
            ? linuxCommitted->ControlGeneration
            : (control != nullptr ? control->Generation : 0);
        handle->Backend = TqRelayBackendType::None;
        handle->LinuxWorker = nullptr;
        handle->LinuxRelayId = 0;
        handle->LinuxWorkerIndex = 0;
        handle->Stop.store(true);
        if (control != nullptr) (void)control->SignalStop(generation);
        if (linuxCommitted != nullptr && relayId != 0 &&
            (control == nullptr ||
             control->WorkerEndpointAlive.load(std::memory_order_acquire))) {
            TqLinuxRelayRuntime::Instance().UnregisterRelay(
                linuxCommitted->WorkerIndex, relayId);
        } else if (legacyWorker != nullptr && relayId != 0) {
            legacyWorker->UnregisterRelay(relayId);
        }
        TqRelayUnregisterActive();
        return;
    }
#endif

#if defined(__APPLE__)
    if (handle->Backend == TqRelayBackendType::DarwinWorker) {
        TqDarwinRelayWorker* worker = handle->DarwinWorker;
        const uint64_t relayId = handle->DarwinRelayId;
        auto control = handle->Control;
        const uint64_t generation = handle->ControlGeneration != 0
            ? handle->ControlGeneration
            : (control != nullptr ? control->Generation : 0);
        handle->Backend = TqRelayBackendType::None;
        handle->DarwinWorker = nullptr;
        handle->DarwinRelayId = 0;
        handle->Control.reset();
        handle->ControlGeneration = 0;
        handle->Stop.store(true, std::memory_order_release);
        if (control != nullptr) {
            (void)control->SignalStop(generation);
        }
        if (worker != nullptr && relayId != 0) {
            worker->UnregisterRelay(relayId);
        }
        if (control != nullptr) {
            (void)control->ReleaseActiveAccountingOnce();
        } else {
            TqRelayUnregisterActive();
        }
        return;
    }
#endif

    handle->Stop.store(true);
    if (handle->Control != nullptr) {
        const uint64_t generation = handle->ControlGeneration != 0
            ? handle->ControlGeneration
            : handle->Control->Generation;
        (void)handle->Control->SignalStop(generation);
    }
}

bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle) {
#if defined(__linux__)
    return TqRelayLinuxCommittedSnapshot(handle) != nullptr;
#else
    (void)handle;
    return false;
#endif
}
