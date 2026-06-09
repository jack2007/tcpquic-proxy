#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"

#include <windows.h>

TqWindowsRelayWorker::TqWindowsRelayWorker() = default;
TqWindowsRelayWorker::~TqWindowsRelayWorker() { Stop(); }

bool TqWindowsRelayWorker::Start() {
    Iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (Iocp_ == nullptr) {
        return false;
    }
    Stopping_.store(false);
    Thread_ = std::thread(&TqWindowsRelayWorker::Run, this);
    return true;
}

void TqWindowsRelayWorker::Stop() {
    if (Iocp_ == nullptr) {
        return;
    }
    Stopping_.store(true);
    PostStop();
    if (Thread_.joinable()) {
        Thread_.join();
    }
    ::CloseHandle(static_cast<HANDLE>(Iocp_));
    Iocp_ = nullptr;
}

void TqWindowsRelayWorker::PostStop() {
    (void)::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, nullptr);
}

void TqWindowsRelayWorker::Run() {
    while (!Stopping_.load()) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(
            static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, INFINITE);
        (void)ok;
        (void)bytes;
        (void)key;
        if (overlapped == nullptr && Stopping_.load()) {
            break;
        }
    }
}

bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle,
    MsQuicStream*,
    ITqCompressor*,
    ITqDecompressor*,
    TqRelayHandle*,
    const TqTuningConfig&,
    TqCompressAlgo) {
    return false;
}

void TqWindowsRelayWorker::StopRelay(uint64_t) {}

QUIC_STATUS QUIC_API TqWindowsRelayWorker::StreamCallback(
    MsQuicStream*,
    void*,
    QUIC_STREAM_EVENT*) noexcept {
    return QUIC_STATUS_SUCCESS;
}

TqWindowsRelayRuntime& TqWindowsRelayRuntime::Instance() {
    static TqWindowsRelayRuntime runtime;
    return runtime;
}

bool TqWindowsRelayRuntime::Start(uint32_t workerCount) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (!Workers_.empty()) {
        return true;
    }
    if (workerCount == 0) {
        workerCount = 1;
    }
    for (uint32_t i = 0; i < workerCount; ++i) {
        auto worker = std::make_unique<TqWindowsRelayWorker>();
        if (!worker->Start()) {
            Workers_.clear();
            return false;
        }
        Workers_.push_back(std::move(worker));
    }
    return true;
}

void TqWindowsRelayRuntime::Stop() {
    std::lock_guard<std::mutex> guard(Lock_);
    Workers_.clear();
}

bool TqWindowsRelayRuntime::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (Workers_.empty()) {
        return false;
    }
    const uint64_t index = NextWorker_.fetch_add(1) % Workers_.size();
    return Workers_[static_cast<size_t>(index)]->RegisterRelay(
        tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo);
}

void TqWindowsRelayRuntime::StopRelay(TqRelayHandle* handle) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::WindowsWorker ||
        handle->WindowsWorker == nullptr) {
        return;
    }
    handle->WindowsWorker->StopRelay(handle->WindowsRelayId);
}

#endif
