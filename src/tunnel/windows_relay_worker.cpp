#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"

#include <windows.h>

const MsQuicApi* MsQuic = nullptr;

enum class TqWindowsRelayEvent : uint32_t {
    TcpRecv,
    TcpSend,
    QuicReceiveQueued,
    QuicSendComplete,
    CloseRelay,
    StopWorker,
};

struct TqWindowsRelayWorker::CallbackContext {
    TqWindowsRelayWorker* Worker{nullptr};
    std::weak_ptr<RelayContext> Relay;
};

struct TqWindowsRelayWorker::IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsRelayEvent Event{TqWindowsRelayEvent::TcpRecv};
    std::shared_ptr<RelayContext> Relay;
    std::vector<uint8_t> Buffer;
    size_t Offset{0};
};

struct TqWindowsRelayWorker::RelayContext {
    uint64_t Id{0};
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqRelayHandle* PublicHandle{nullptr};
    TqTuningConfig Tuning;
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::atomic<bool> Closing{false};
    std::atomic<bool> TcpRecvClosed{false};
    std::atomic<uint32_t> InFlightQuicSends{0};
    std::shared_ptr<CallbackContext> Callback;
};

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
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.clear();
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
        if (overlapped == nullptr && Stopping_.load()) {
            break;
        }
        if (overlapped == nullptr) {
            continue;
        }
        std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
        if (!ok) {
            CloseRelay(op->Relay);
            continue;
        }
        switch (op->Event) {
        case TqWindowsRelayEvent::TcpRecv:
            HandleTcpRecv(std::move(op), bytes);
            break;
        case TqWindowsRelayEvent::TcpSend:
            HandleTcpSend(std::move(op), bytes);
            break;
        case TqWindowsRelayEvent::QuicReceiveQueued:
            HandleQuicReceiveQueued(std::move(op));
            break;
        case TqWindowsRelayEvent::CloseRelay:
            CloseRelay(op->Relay);
            break;
        default:
            break;
        }
    }
}

bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (!TqSocketValid(tcpFd) || stream == nullptr || handle == nullptr || Iocp_ == nullptr) {
        return false;
    }
    if (::CreateIoCompletionPort(
            reinterpret_cast<HANDLE>(tcpFd),
            static_cast<HANDLE>(Iocp_),
            0,
            0) == nullptr) {
        return false;
    }

    auto relay = std::make_shared<RelayContext>();
    relay->Id = NextRelayId_.fetch_add(1);
    relay->TcpFd = tcpFd;
    relay->Stream = stream;
    relay->Compressor = compressor;
    relay->Decompressor = decompressor;
    relay->PublicHandle = handle;
    relay->Tuning = tuning;
    relay->CompressAlgo = compressAlgo;
    relay->Callback = std::make_shared<CallbackContext>();
    relay->Callback->Worker = this;
    relay->Callback->Relay = relay;
    stream->Callback = StreamCallback;
    stream->Context = relay->Callback.get();

    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_[relay->Id] = relay;
    }

    handle->Backend = TqRelayBackendType::WindowsWorker;
    handle->WindowsWorker = this;
    handle->WindowsRelayId = relay->Id;

    return PostTcpRecv(relay);
}

bool TqWindowsRelayWorker::PostTcpRecv(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load() || relay->TcpRecvClosed.load()) {
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsRelayEvent::TcpRecv;
    op->Relay = relay;
    op->Buffer.resize(relay->Tuning.RelayIoSize);

    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(op->Buffer.data());
    buf.len = static_cast<ULONG>(op->Buffer.size());
    DWORD flags = 0;
    DWORD received = 0;
    IoOperation* raw = op.release();
    const int rc = ::WSARecv(relay->TcpFd, &buf, 1, &received, &flags, &raw->Overlapped, nullptr);
    if (rc == 0 || ::WSAGetLastError() == WSA_IO_PENDING) {
        return true;
    }
    delete raw;
    return false;
}

void TqWindowsRelayWorker::CloseRelay(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.exchange(true)) {
        return;
    }
    TqCloseSocket(relay->TcpFd);
    relay->TcpFd = TqInvalidSocket;
    if (relay->Stream != nullptr && relay->Stream->Context == relay->Callback.get()) {
        relay->Stream->Callback = MsQuicStream::NoOpCallback;
        relay->Stream->Context = nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
    }
    if (relay->PublicHandle != nullptr) {
        relay->PublicHandle->Stop.store(true);
    }
}

void TqWindowsRelayWorker::HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        return;
    }
    if (bytes == 0) {
        if (!relay->TcpRecvClosed.exchange(true)) {
            std::vector<uint8_t> finalOutput;
            if (relay->Compressor != nullptr) {
                if (!relay->Compressor->Compress(nullptr, 0, finalOutput, true)) {
                    CloseRelay(relay);
                    return;
                }
            }
            if (!finalOutput.empty()) {
                op->Buffer.swap(finalOutput);
                QUIC_BUFFER buffer{};
                buffer.Buffer = op->Buffer.data();
                buffer.Length = static_cast<uint32_t>(op->Buffer.size());
                relay->InFlightQuicSends.fetch_add(1);
                IoOperation* raw = op.release();
                const QUIC_STATUS status = relay->Stream->Send(&buffer, 1, QUIC_SEND_FLAG_FIN, raw);
                if (QUIC_FAILED(status)) {
                    relay->InFlightQuicSends.fetch_sub(1);
                    delete raw;
                    CloseRelay(relay);
                    return;
                }
            } else {
                relay->Stream->Send(nullptr, 0, QUIC_SEND_FLAG_FIN, nullptr);
            }
            TqCloseSocket(relay->TcpFd);
            relay->TcpFd = TqInvalidSocket;
        }
        return;
    }
    op->Buffer.resize(bytes);
    if (relay->Compressor != nullptr) {
        std::vector<uint8_t> compressed;
        if (!relay->Compressor->Compress(
                op->Buffer.data(),
                static_cast<uint32_t>(op->Buffer.size()),
                compressed,
                false)) {
            CloseRelay(relay);
            return;
        }
        if (compressed.empty() && !relay->Compressor->Flush(compressed)) {
            CloseRelay(relay);
            return;
        }
        op->Buffer.swap(compressed);
    }
    if (op->Buffer.empty()) {
        (void)PostTcpRecv(relay);
        return;
    }

    QUIC_BUFFER buffer{};
    buffer.Buffer = op->Buffer.data();
    buffer.Length = static_cast<uint32_t>(op->Buffer.size());
    relay->InFlightQuicSends.fetch_add(1);
    IoOperation* raw = op.release();
    const QUIC_STATUS status = relay->Stream->Send(&buffer, 1, QUIC_SEND_FLAG_NONE, raw);
    if (QUIC_FAILED(status)) {
        relay->InFlightQuicSends.fetch_sub(1);
        delete raw;
        CloseRelay(relay);
    }
}

void TqWindowsRelayWorker::HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Buffer.empty()) {
        return;
    }
    if (relay->Decompressor != nullptr) {
        std::vector<uint8_t> output;
        if (!relay->Decompressor->Decompress(
                op->Buffer.data(),
                static_cast<uint32_t>(op->Buffer.size()),
                output)) {
            CloseRelay(relay);
            return;
        }
        op->Buffer.swap(output);
    }
    if (op->Buffer.empty()) {
        return;
    }
    op->Event = TqWindowsRelayEvent::TcpSend;
    op->Offset = 0;
    if (!PostTcpSend(std::move(op))) {
        CloseRelay(relay);
    }
}

bool TqWindowsRelayWorker::PostTcpSend(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Offset >= op->Buffer.size()) {
        return false;
    }
    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(op->Buffer.data() + op->Offset);
    buf.len = static_cast<ULONG>(op->Buffer.size() - op->Offset);
    DWORD sent = 0;
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &buf, 1, &sent, 0, &raw->Overlapped, nullptr);
    if (rc != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
        delete raw;
        return false;
    }
    return true;
}

void TqWindowsRelayWorker::HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        return;
    }
    op->Offset += bytes;
    if (op->Offset < op->Buffer.size()) {
        if (!PostTcpSend(std::move(op))) {
            CloseRelay(relay);
        }
    }
}

void TqWindowsRelayWorker::StopRelay(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsRelayEvent::CloseRelay;
    op->Relay = relay;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        CloseRelay(relay);
    }
}

QUIC_STATUS QUIC_API TqWindowsRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* callback = static_cast<TqWindowsRelayWorker::CallbackContext*>(context);
    (void)stream;
    if (callback == nullptr || callback->Worker == nullptr || event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    auto relay = callback->Relay.lock();
    if (!relay) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        std::unique_ptr<IoOperation> completed(
            static_cast<IoOperation*>(event->SEND_COMPLETE.ClientContext));
        if (completed && completed->Relay) {
            completed->Relay->InFlightQuicSends.fetch_sub(1);
            if (!completed->Relay->Closing.load() && !completed->Relay->TcpRecvClosed.load()) {
                (void)completed->Relay->Callback->Worker->PostTcpRecv(completed->Relay);
            }
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        auto op = std::make_unique<IoOperation>();
        op->Event = TqWindowsRelayEvent::QuicReceiveQueued;
        op->Relay = relay;
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER& buffer = event->RECEIVE.Buffers[i];
            op->Buffer.insert(op->Buffer.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
        }
        IoOperation* raw = op.release();
        if (!::PostQueuedCompletionStatus(
                static_cast<HANDLE>(callback->Worker->Iocp_), 0, 0, &raw->Overlapped)) {
            delete raw;
            callback->Worker->CloseRelay(relay);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN) {
        if (TqSocketValid(relay->TcpFd)) {
            (void)TqShutdownSend(relay->TcpFd);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        callback->Worker->CloseRelay(relay);
    }
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
