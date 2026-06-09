#include "relay.h"

#if defined(__linux__)
#include "linux_relay_worker.h"
#endif

#include "msquic.hpp"
#include "tuning.h"

bool TqRelayStart(
    int tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& profileTuning,
    TqCompressAlgo compressAlgo) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }

#if !defined(__linux__)
    (void)tcpFd;
    (void)stream;
    (void)compressor;
    (void)decompressor;
    (void)profileTuning;
    (void)compressAlgo;
    return false;
#else
    const uint32_t activeRelays = TqRelayRegisterActive();
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, activeRelays);

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
#endif
}

void TqRelayStop(TqRelayHandle* handle) {
    if (handle == nullptr) {
        return;
    }

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
