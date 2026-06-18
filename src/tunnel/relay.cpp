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

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& profileTuning,
    TqCompressAlgo compressAlgo) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }

    const uint32_t activeRelays = TqRelayRegisterActive();
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, activeRelays);

#if defined(_WIN32)
    if (!TqWindowsRelayRuntime::Instance().Start(tuning.LinuxRelayWorkerCount) ||
        !TqWindowsRelayRuntime::Instance().RegisterRelay(
            tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo)) {
        TqRelayUnregisterActive();
        return false;
    }
    handle->Stop.store(false);
    return true;
#elif defined(__linux__)
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
    registration.TcpFd = tcpFd;
    registration.Stream = stream;
    registration.Handle = handle;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.CompressAlgo = compressAlgo;
    registration.EnableQuicSends = true;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (!registered.Ok) {
        TqRelayUnregisterActive();
        return false;
    }

    handle->Stop.store(false);
    handle->Backend = TqRelayBackendType::LinuxWorker;
    handle->LinuxWorker = worker;
    handle->LinuxRelayId = registered.RelayId;
    return true;
#elif defined(__APPLE__)
    if (!TqDarwinRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }

    TqDarwinRelayWorker* worker = TqDarwinRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = tcpFd;
    registration.Stream = stream;
    registration.Handle = handle;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.CompressAlgo = compressAlgo;
    registration.EnableQuicSends = true;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (!registered.Ok) {
        TqRelayUnregisterActive();
        return false;
    }

    handle->Stop.store(false);
    handle->Backend = TqRelayBackendType::DarwinWorker;
    handle->DarwinWorker = worker;
    handle->DarwinRelayId = registered.RelayId;
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

bool TqRelayStartQuicReceiveSink(
    MsQuicStream* stream,
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
    registration.TcpFd = -1;
    registration.Stream = stream;
    registration.Handle = handle;
    registration.EnableQuicSends = false;
    registration.SinkQuicReceives = true;
    registration.SinkQuicReceiveBytes = receiveBytes;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (!registered.Ok) {
        TqRelayUnregisterActive();
        return false;
    }

    handle->Stop.store(false);
    handle->Backend = TqRelayBackendType::LinuxWorker;
    handle->LinuxWorker = worker;
    handle->LinuxRelayId = registered.RelayId;
    return true;
#else
    (void)profileTuning;
    (void)receiveBytes;
    TqRelayUnregisterActive();
    return false;
#endif
}

void TqRelayStop(TqRelayHandle* handle) {
    if (handle == nullptr) {
        return;
    }

#if defined(_WIN32)
    if (handle->Backend == TqRelayBackendType::WindowsWorker) {
        TqWindowsRelayRuntime::Instance().StopRelay(handle);
        handle->Backend = TqRelayBackendType::None;
        handle->WindowsWorker = nullptr;
        handle->WindowsRelayId = 0;
        handle->Stop.store(true);
        TqRelayUnregisterActive();
        return;
    }
#endif

#if defined(__linux__)
    if (handle->Backend == TqRelayBackendType::LinuxWorker) {
        TqLinuxRelayWorker* worker = handle->LinuxWorker;
        const uint64_t relayId = handle->LinuxRelayId;
        handle->Backend = TqRelayBackendType::None;
        handle->LinuxWorker = nullptr;
        handle->LinuxRelayId = 0;
        handle->Stop.store(true);
        if (worker != nullptr && relayId != 0) {
            worker->UnregisterRelay(relayId);
        }
        TqRelayUnregisterActive();
        return;
    }
#endif

#if defined(__APPLE__)
    if (handle->Backend == TqRelayBackendType::DarwinWorker) {
        TqDarwinRelayWorker* worker = handle->DarwinWorker;
        const uint64_t relayId = handle->DarwinRelayId;
        handle->Backend = TqRelayBackendType::None;
        handle->DarwinWorker = nullptr;
        handle->DarwinRelayId = 0;
        handle->Stop.store(true);
        if (worker != nullptr && relayId != 0) {
            worker->UnregisterRelay(relayId);
        }
        TqRelayUnregisterActive();
        return;
    }
#endif

    handle->Stop.store(true);
}

bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle) {
#if defined(__linux__)
    return handle != nullptr && handle->Backend == TqRelayBackendType::LinuxWorker;
#else
    (void)handle;
    return false;
#endif
}
