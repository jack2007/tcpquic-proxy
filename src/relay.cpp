#include "relay.h"

#if defined(__linux__)
#include "linux_relay_worker.h"
#endif

#include "msquic.hpp"
#include "tcp_write_queue.h"
#include "tuning.h"

#include <cerrno>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

const MsQuicApi* MsQuic;

namespace {

constexpr int TqRelayWarmupMs = 3000;

struct TqRelaySendContext {
    QUIC_BUFFER Buffer;
    size_t Capacity;
    uint8_t Data[1];

    static TqRelaySendContext* New(size_t capacity, size_t maxCapacity) {
        if (capacity > maxCapacity || capacity > UINT32_MAX) {
            return nullptr;
        }

        const size_t allocSize =
            sizeof(TqRelaySendContext) + (capacity == 0 ? 0 : capacity - 1);
        auto* context = static_cast<TqRelaySendContext*>(std::malloc(allocSize));
        if (context == nullptr) {
            return nullptr;
        }

        context->Capacity = capacity;
        context->Buffer.Length = 0;
        context->Buffer.Buffer = context->Data;
        return context;
    }

    static TqRelaySendContext* NewCopy(
        const uint8_t* data,
        size_t length,
        size_t maxCapacity) {
        auto* context = New(length, maxCapacity);
        if (context == nullptr) {
            return nullptr;
        }

        context->Buffer.Length = static_cast<uint32_t>(length);
        if (length > 0 && data != nullptr) {
            std::memcpy(context->Data, data, length);
        }
        return context;
    }

    static void Delete(TqRelaySendContext* context) {
        std::free(context);
    }
};

} // namespace

class TqTunnelRelay final {
public:
    TqTunnelRelay(
        int tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo) :
        TcpFd(tcpFd),
        Stream(stream),
        Compressor(compressor),
        Decompressor(decompressor),
        Handle(handle),
        FastPath(compressor == nullptr && decompressor == nullptr),
        CompressAlgo(compressAlgo),
        RelayIoSize(tuning.RelayIoSize),
        RelayCompressIoSize(tuning.RelayCompressIoSize),
        RelayMaxInFlightSends(tuning.RelayMaxInFlightSends),
        RelayMaxFreeSendContexts(tuning.RelayMaxFreeSendContexts),
        RelayDefaultIdealSend(tuning.RelayDefaultIdealSend),
        RecvBuffer(tuning.RelayIoSize),
        IdealSendBytes(tuning.RelayDefaultIdealSend) {
    }

    ~TqTunnelRelay() {
        Stop();
        for (auto* context : FreeSendContexts) {
            TqRelaySendContext::Delete(context);
        }
        FreeSendContexts.clear();
    }

    TqTunnelRelay(const TqTunnelRelay&) = delete;
    TqTunnelRelay& operator=(const TqTunnelRelay&) = delete;

    bool Start() {
        if (TcpFd < 0 || Stream == nullptr || Handle == nullptr) {
            return false;
        }

        Handle->Stop.store(false);
        {
            std::lock_guard<std::mutex> lock(CallbackLock);
            Stream->Callback = StreamCallback;
            Stream->Context = this;
        }

        try {
            WarmupStart = std::chrono::steady_clock::now();
            WarmupBytesSent = 0;
            WarmupRawBytes = 0;
            WarmupCompressedBytes = 0;
            WarmupClosed = false;
            if (FastPath) {
                FreeSendContexts.reserve(RelayMaxFreeSendContexts);
                for (size_t i = 0; i < RelayMaxFreeSendContexts; ++i) {
                    auto* context = TqRelaySendContext::New(RelayIoSize, RelayIoSize);
                    if (context == nullptr) {
                        for (auto* freeContext : FreeSendContexts) {
                            TqRelaySendContext::Delete(freeContext);
                        }
                        FreeSendContexts.clear();
                        return false;
                    }
                    FreeSendContexts.push_back(context);
                }
            }
            TcpWriter = std::make_unique<TqTcpWriteQueue>(TcpFd, &Handle->Stop, 64, 4u << 20);
            if (!TcpWriter->Start()) {
                DetachStreamCallback();
                return false;
            }
            TcpThread = std::thread(&TqTunnelRelay::TcpToStreamLoop, this);
        } catch (...) {
            if (TcpWriter != nullptr) {
                TcpWriter->Stop();
                TcpWriter.reset();
            }
            DetachStreamCallback();
            return false;
        }

        return true;
    }

    void Stop() {
        bool expected = false;
        Handle->Stop.compare_exchange_strong(expected, true);

        if (TcpFd >= 0) {
            ::shutdown(TcpFd, SHUT_RDWR);
        }

        SendCv.notify_all();
        DetachStreamCallback();

        if (TcpWriter != nullptr) {
            TcpWriter->Stop();
            TcpWriter.reset();
        }

        if (TcpThread.joinable()) {
            TcpThread.join();
        }

        FlushRuntimeSample();
        FlushCompressionSample();
    }

    void RecordCompressWarmup(size_t rawBytes, size_t compressedBytes) {
        if (FastPath || WarmupClosed || rawBytes == 0 || compressedBytes == 0) {
            return;
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - WarmupStart).count();
        if (elapsedMs > TqRelayWarmupMs) {
            WarmupClosed = true;
            return;
        }

        WarmupRawBytes += rawBytes;
        WarmupCompressedBytes += compressedBytes;
    }

    void FlushCompressionSample() {
        if (FastPath || CompressAlgo == TqCompressAlgo::None) {
            return;
        }

        if (WarmupRawBytes >= 4096 && WarmupCompressedBytes > 0) {
            TqRecordCompressionSample(WarmupRawBytes, WarmupCompressedBytes);
        }
    }

    void RecordWarmupBytes(size_t length) {
        if (!FastPath || WarmupClosed || length == 0) {
            return;
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - WarmupStart).count();
        if (elapsedMs > TqRelayWarmupMs) {
            WarmupClosed = true;
            return;
        }
        WarmupBytesSent += length;
    }

    void FlushRuntimeSample() {
        if (!FastPath) {
            return;
        }

        if (WarmupBytesSent > 0) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - WarmupStart).count();
            const uint32_t sampleMs = static_cast<uint32_t>(
                std::min<int64_t>(std::max<int64_t>(elapsedMs, 1), TqRelayWarmupMs));
            TqRecordRelayThroughput(
                WarmupBytesSent,
                sampleMs,
                IdealSendBytes,
                InFlightSends.load());
        } else {
            TqRecordIdealSendHint(IdealSendBytes);
        }
    }

private:
    static QUIC_STATUS QUIC_API StreamCallback(
        _In_ MsQuicStream* stream,
        _In_opt_ void* context,
        _Inout_ QUIC_STREAM_EVENT* event) noexcept {
        auto* relay = static_cast<TqTunnelRelay*>(context);
        if (relay == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        return relay->OnStreamEvent(stream, event);
    }

    QUIC_STATUS OnStreamEvent(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept {
        if (Handle->Stop.load()) {
            return QUIC_STATUS_SUCCESS;
        }

        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            return OnStreamReceive(stream, event);

        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            auto* context =
                static_cast<TqRelaySendContext*>(event->SEND_COMPLETE.ClientContext);
            if (FastPath && context != nullptr) {
                if (context->Buffer.Length > 0) {
                    OutstandingBytes.fetch_sub(context->Buffer.Length);
                    InFlightSends.fetch_sub(1);
                    RecordWarmupBytes(context->Buffer.Length);
                }
                ReturnSendContext(context);
                SendCv.notify_all();
            } else {
                TqRelaySendContext::Delete(context);
            }
            break;
        }

        case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
            if (FastPath) {
                const uint64_t ideal = event->IDEAL_SEND_BUFFER_SIZE.ByteCount;
                if (ideal > IdealSendBytes) {
                    IdealSendBytes = ideal;
                }
                if (IdealSendBytes < RelayIoSize) {
                    IdealSendBytes = RelayDefaultIdealSend;
                }
                TqRecordIdealSendHint(IdealSendBytes);
                SendCv.notify_all();
            }
            break;

        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            Handle->Stop.store(true);
            if (TcpFd >= 0) {
                ::shutdown(TcpFd, SHUT_RDWR);
            }
            {
                std::lock_guard<std::mutex> lock(CallbackLock);
                Stream = nullptr;
            }
            SendCv.notify_all();
            break;

        default:
            break;
        }

        return QUIC_STATUS_SUCCESS;
    }

    bool EnqueueTcpData(const uint8_t* data, size_t length) {
        if (length == 0) {
            return true;
        }

        size_t offset = 0;
        while (offset < length) {
            const size_t chunkLength = std::min(RelayIoSize, length - offset);
            if (!TcpWriter->Enqueue(data + offset, chunkLength, false)) {
                return false;
            }
            offset += chunkLength;
        }

        return true;
    }

    QUIC_STATUS OnStreamReceive(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept {
        if (Handle->Stop.load() || TcpWriter == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }

        bool ok = true;

        if (Decompressor != nullptr) {
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount && ok; ++i) {
                const auto& buffer = event->RECEIVE.Buffers[i];
                std::vector<uint8_t> output;
                if (!Decompressor->Decompress(buffer.Buffer, buffer.Length, output)) {
                    ok = false;
                    break;
                }
                if (!output.empty()) {
                    ok = EnqueueTcpData(output.data(), output.size());
                }
            }
        } else {
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount && ok; ++i) {
                const auto& buffer = event->RECEIVE.Buffers[i];
                if (buffer.Length > 0) {
                    ok = EnqueueTcpData(buffer.Buffer, buffer.Length);
                }
            }
        }

        if (ok && (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN)) {
            ok = TcpWriter->Enqueue(nullptr, 0, true);
        }

        if (!ok) {
            Handle->Stop.store(true);
            ::shutdown(TcpFd, SHUT_RDWR);
            stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
            return QUIC_STATUS_SUCCESS;
        }

        return QUIC_STATUS_SUCCESS;
    }

    TqRelaySendContext* AcquireSendContext(size_t length) {
        if (length > RelayIoSize || length > UINT32_MAX) {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(SendLock);
        if (!FreeSendContexts.empty()) {
            auto* context = FreeSendContexts.back();
            FreeSendContexts.pop_back();
            context->Buffer.Length = static_cast<uint32_t>(length);
            context->Buffer.Buffer = context->Data;
            return context;
        }

        auto* context = TqRelaySendContext::New(RelayIoSize, RelayIoSize);
        if (context == nullptr) {
            return nullptr;
        }
        context->Buffer.Length = static_cast<uint32_t>(length);
        return context;
    }

    void ReturnSendContext(TqRelaySendContext* context) {
        if (context == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(SendLock);
        if (FreeSendContexts.size() < RelayMaxFreeSendContexts &&
            context->Capacity == RelayIoSize) {
            context->Buffer.Length = 0;
            FreeSendContexts.push_back(context);
            SendCv.notify_all();
            return;
        }

        TqRelaySendContext::Delete(context);
    }

    bool SendToStream(
        const uint8_t* data,
        size_t length,
        QUIC_SEND_FLAGS flags,
        bool trackOutstanding) {
        auto* context = TqRelaySendContext::NewCopy(data, length, RelayIoSize);
        if (context == nullptr) {
            return false;
        }
        if (!SendContextToStream(context, length, flags, trackOutstanding)) {
            TqRelaySendContext::Delete(context);
            return false;
        }
        return true;
    }

    bool SendContextToStream(
        TqRelaySendContext* context,
        size_t length,
        QUIC_SEND_FLAGS flags,
        bool trackOutstanding) {
        if (context == nullptr || length > UINT32_MAX) {
            return false;
        }
        if (length > context->Capacity) {
            return false;
        }

        context->Buffer.Length = static_cast<uint32_t>(length);
        context->Buffer.Buffer = context->Data;

        if (trackOutstanding) {
            if (length > 0) {
                OutstandingBytes.fetch_add(length);
            }
            InFlightSends.fetch_add(1);
        }

        const QUIC_STATUS status = Stream->Send(&context->Buffer, 1, flags, context);
        if (status == QUIC_STATUS_ABORTED || status == QUIC_STATUS_INVALID_STATE ||
            QUIC_FAILED(status)) {
            if (trackOutstanding) {
                if (length > 0) {
                    OutstandingBytes.fetch_sub(length);
                }
                InFlightSends.fetch_sub(1);
            }
            return false;
        }

        return true;
    }

    void WaitForSendCapacity() {
        std::unique_lock<std::mutex> lock(SendLock);
        SendCv.wait(lock, [this] {
            return Handle->Stop.load() ||
                (OutstandingBytes.load() < IdealSendBytes &&
                 InFlightSends.load() < RelayMaxInFlightSends &&
                 !FreeSendContexts.empty());
        });
    }

    bool HasSendCapacity() const {
        return OutstandingBytes.load() < IdealSendBytes &&
            InFlightSends.load() < RelayMaxInFlightSends &&
            !FreeSendContexts.empty();
    }

    bool PumpNonBlockingTcpToStream() {
        while (!Handle->Stop.load() && HasSendCapacity()) {
            auto* context = AcquireSendContext(RelayIoSize);
            if (context == nullptr) {
                return false;
            }

            const ssize_t received =
                ::recv(TcpFd, context->Data, context->Capacity, MSG_DONTWAIT);
            if (received > 0) {
                if (!SendContextToStream(
                        context,
                        static_cast<size_t>(received),
                        QUIC_SEND_FLAG_NONE,
                        true)) {
                    ReturnSendContext(context);
                    return false;
                }
                continue;
            }

            ReturnSendContext(context);

            if (received == 0) {
                SendToStream(nullptr, 0, QUIC_SEND_FLAG_FIN, false);
                return false;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }

        return true;
    }

    void FillSendPipeline() {
        while (!Handle->Stop.load() && HasSendCapacity()) {
            if (!PumpNonBlockingTcpToStream()) {
                if (!Handle->Stop.load()) {
                    Handle->Stop.store(true);
                }
                break;
            }
        }
    }

    void TcpToStreamLoopFast() {
        while (!Handle->Stop.load()) {
            auto* context = AcquireSendContext(RelayIoSize);
            if (context == nullptr) {
                break;
            }

            const ssize_t received =
                ::recv(TcpFd, context->Data, context->Capacity, 0);
            if (received > 0) {
                if (!SendContextToStream(
                        context,
                        static_cast<size_t>(received),
                        QUIC_SEND_FLAG_NONE,
                        true)) {
                    ReturnSendContext(context);
                    break;
                }
                FillSendPipeline();
                continue;
            }

            ReturnSendContext(context);

            if (received == 0) {
                SendToStream(nullptr, 0, QUIC_SEND_FLAG_FIN, false);
                break;
            }

            if (errno == EINTR) {
                continue;
            }
            break;
        }

        Handle->Stop.store(true);
        ::shutdown(TcpFd, SHUT_RD);
    }

    void TcpToStreamLoop() {
        if (FastPath) {
            TcpToStreamLoopFast();
            return;
        }

        std::vector<uint8_t> input(RelayCompressIoSize);

        while (!Handle->Stop.load()) {
            const ssize_t received = ::recv(TcpFd, input.data(), input.size(), 0);
            if (received > 0) {
                std::vector<uint8_t> output;
                const uint8_t* sendData = input.data();
                size_t sendLength = static_cast<size_t>(received);

                if (Compressor != nullptr) {
                    if (!Compressor->Compress(input.data(), sendLength, output, false)) {
                        break;
                    }
                    if (output.empty() && !Compressor->Flush(output)) {
                        break;
                    }
                    sendData = output.data();
                    sendLength = output.size();
                    if (sendLength == 0) {
                        continue;
                    }
                }

                if (!SendToStream(sendData, sendLength, QUIC_SEND_FLAG_NONE, false)) {
                    break;
                }
                RecordCompressWarmup(static_cast<size_t>(received), sendLength);
                continue;
            }

            if (received == 0) {
                if (Compressor != nullptr) {
                    std::vector<uint8_t> output;
                    if (!Compressor->Compress(nullptr, 0, output, true)) {
                        break;
                    }
                    if (!output.empty()) {
                        if (!SendToStream(output.data(), output.size(), QUIC_SEND_FLAG_NONE, false)) {
                            break;
                        }
                    }
                }
                SendToStream(nullptr, 0, QUIC_SEND_FLAG_FIN, false);
                break;
            }

            if (errno == EINTR) {
                continue;
            }
            break;
        }

        Handle->Stop.store(true);
        ::shutdown(TcpFd, SHUT_RD);
    }

    void DetachStreamCallback() {
        std::lock_guard<std::mutex> lock(CallbackLock);
        MsQuicStream* stream = Stream;
        Stream = nullptr;
        if (stream != nullptr && stream->Context == this) {
            stream->Callback = MsQuicStream::NoOpCallback;
            stream->Context = nullptr;
        }
    }

    int TcpFd;
    MsQuicStream* Stream;
    ITqCompressor* Compressor;
    ITqDecompressor* Decompressor;
    TqRelayHandle* Handle;
    bool FastPath;
    TqCompressAlgo CompressAlgo;
    size_t RelayIoSize;
    size_t RelayCompressIoSize;
    uint32_t RelayMaxInFlightSends;
    size_t RelayMaxFreeSendContexts;
    uint64_t RelayDefaultIdealSend;
    std::vector<uint8_t> RecvBuffer;
    std::vector<TqRelaySendContext*> FreeSendContexts;
    std::unique_ptr<TqTcpWriteQueue> TcpWriter;
    std::thread TcpThread;
    std::mutex CallbackLock;
    std::mutex SendLock;
    std::condition_variable SendCv;
    std::atomic<uint64_t> OutstandingBytes{0};
    std::atomic<uint32_t> InFlightSends{0};
    uint64_t IdealSendBytes;
    std::chrono::steady_clock::time_point WarmupStart{};
    uint64_t WarmupBytesSent{0};
    uint64_t WarmupRawBytes{0};
    uint64_t WarmupCompressedBytes{0};
    bool WarmupClosed{false};
};

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
