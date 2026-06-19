#include "speed_test.h"

#include "config.h"
#include "platform_socket.h"
#include "quic_session.h"
#include "tcp_tunnel.h"
#include "tuning.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/time.h>
#endif

namespace {

using TqClock = std::chrono::steady_clock;
#if defined(_WIN32)
using TqSockLen = int;
#else
using TqSockLen = socklen_t;
#endif

enum class TqControlFrameState {
    NeedMore,
    Ready,
    Invalid,
};

struct TqControlSendContext {
    QUIC_BUFFER Buffer;
    uint8_t Data[1];

    static TqControlSendContext* New(const uint8_t* data, size_t length) {
        if (length > UINT32_MAX || (length > 0 && data == nullptr)) {
            return nullptr;
        }

        const size_t allocSize =
            sizeof(TqControlSendContext) + (length == 0 ? 0 : length - 1);
        auto* context = static_cast<TqControlSendContext*>(std::malloc(allocSize));
        if (context == nullptr) {
            return nullptr;
        }

        context->Buffer.Length = static_cast<uint32_t>(length);
        context->Buffer.Buffer = context->Data;
        if (length > 0) {
            std::memcpy(context->Data, data, length);
        }
        return context;
    }

    static void Delete(TqControlSendContext* context) {
        std::free(context);
    }
};

bool TqIsValidDirection(TqSpeedDirection direction) {
    return direction == TqSpeedDirection::Download || direction == TqSpeedDirection::Upload;
}

void TqCloseSocketIfValid(TqSocketHandle& socket) {
    if (TqSocketValid(socket)) {
        TqCloseSocket(socket);
        socket = TqInvalidSocket;
    }
}

uint32_t TqMakeSessionId() {
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        TqClock::now().time_since_epoch()).count();
    const uint32_t sessionId = static_cast<uint32_t>(nowUs & 0xffffffffu);
    return sessionId == 0 ? 1u : sessionId;
}

double TqElapsedSeconds(uint64_t elapsedUs) {
    return static_cast<double>(elapsedUs) / 1000000.0;
}

double TqGbps(uint64_t bytes, uint64_t elapsedUs) {
    if (elapsedUs == 0) {
        return 0.0;
    }
    return (static_cast<double>(bytes) * 8.0) / static_cast<double>(elapsedUs) / 1000.0;
}

double TqMiBPerSec(uint64_t bytes, uint64_t elapsedUs) {
    if (elapsedUs == 0) {
        return 0.0;
    }
    return static_cast<double>(bytes) * 1000000.0 /
        static_cast<double>(elapsedUs) / (1024.0 * 1024.0);
}

} // namespace

uint64_t TqSpeedByteMismatchLimit(uint64_t highBytes) {
    const uint64_t minLimit = 16ull * 1024ull * 1024ull;
    const uint64_t activeSocketBuffer = static_cast<uint64_t>(
        std::max(0, TqGetActiveTcpSocketBuffer()));
    const uint64_t socketPipelineLimit = (2ull * activeSocketBuffer) + minLimit;
    (void)highBytes;
    return std::max(minLimit, socketPipelineLimit);
}

bool TqSpeedByteCountsCloseEnough(uint64_t localBytes, uint64_t serverBytes, uint64_t& diffOut, uint64_t& limitOut) {
    const uint64_t high = std::max(localBytes, serverBytes);
    const uint64_t low = std::min(localBytes, serverBytes);
    diffOut = high - low;
    limitOut = TqSpeedByteMismatchLimit(high);
    return diffOut <= limitOut;
}

bool TqSpeedConnectWaitShouldStop(uint32_t connected, uint32_t needed, bool deadlineReached) {
    return connected >= needed || deadlineReached;
}

namespace {

const char* TqSpeedDirectionName(TqSpeedDirection direction) {
    switch (direction) {
    case TqSpeedDirection::Download:
        return "download";
    case TqSpeedDirection::Upload:
        return "upload";
    default:
        return "unknown";
    }
}

std::string TqReadyAddressToHost(const TqSpeedReady& ready) {
    char text[INET6_ADDRSTRLEN]{};
    if (ready.AddrType == TQ_ADDR_IPV4 && ready.Addr.size() == 4) {
        const char* converted =
            TqInetNtop(AF_INET, ready.Addr.data(), text, sizeof(text));
        return converted == nullptr ? std::string() : std::string(converted);
    }
    if (ready.AddrType == TQ_ADDR_IPV6 && ready.Addr.size() == 16) {
        const char* converted =
            TqInetNtop(AF_INET6, ready.Addr.data(), text, sizeof(text));
        return converted == nullptr ? std::string() : std::string(converted);
    }
    return {};
}

TqControlFrameState TqTryGetFrameSize(
    const std::vector<uint8_t>& buffer,
    size_t& frameSize) {
    frameSize = 0;
    if (buffer.size() < 4) {
        return TqControlFrameState::NeedMore;
    }
    if (buffer[0] != TQ_MAGIC_0 || buffer[1] != TQ_MAGIC_1 || buffer[2] != TQ_VERSION) {
        return TqControlFrameState::Invalid;
    }

    switch (buffer[3]) {
    case TQ_CMD_SPEED_START:
        frameSize = TQ_SPEED_START_SIZE;
        break;
    case TQ_CMD_SPEED_READY:
        if (buffer.size() < TQ_SPEED_READY_MIN_SIZE) {
            return TqControlFrameState::NeedMore;
        }
        frameSize = TQ_SPEED_READY_MIN_SIZE +
            static_cast<size_t>((static_cast<uint16_t>(buffer[11]) << 8) | buffer[12]);
        break;
    case TQ_CMD_SPEED_FINISH:
        frameSize = TQ_SPEED_FINISH_SIZE;
        break;
    case TQ_CMD_SPEED_RESULT:
        frameSize = TQ_SPEED_RESULT_SIZE;
        break;
    case TQ_CMD_SPEED_ERROR:
        if (buffer.size() < TQ_SPEED_ERROR_MIN_SIZE) {
            return TqControlFrameState::NeedMore;
        }
        frameSize = TQ_SPEED_ERROR_MIN_SIZE +
            static_cast<size_t>((static_cast<uint16_t>(buffer[9]) << 8) | buffer[10]);
        break;
    default:
        return TqControlFrameState::Invalid;
    }

    if (buffer.size() < frameSize) {
        return TqControlFrameState::NeedMore;
    }
    return TqControlFrameState::Ready;
}

bool TqCreateLoopbackListener(uint16_t parallel, TqSocketHandle& outListener, uint16_t& outPort) {
    outListener = TqInvalidSocket;
    outPort = 0;

    TqSocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(listener)) {
        return false;
    }

    (void)TqSetReuseAddr(listener);
    (void)TqSetNoDelay(listener);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (!TqInetPton(AF_INET, "127.0.0.1", &addr.sin_addr)) {
        TqCloseSocket(listener);
        return false;
    }

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    const int backlog = std::max<int>(16, static_cast<int>(parallel));
    if (::listen(listener, backlog) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    TqSockLen addrLen = static_cast<TqSockLen>(sizeof(addr));
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    outListener = listener;
    outPort = ntohs(addr.sin_port);
    return outPort != 0;
}

struct TqSpeedSession {
    explicit TqSpeedSession(const TqSpeedStart& startIn)
        : Start(startIn), StartedAt(TqClock::now()) {}

    struct Connection {
        explicit Connection(TqSocketHandle socketIn) : Socket(socketIn) {}

        std::mutex Mutex;
        TqSocketHandle Socket{TqInvalidSocket};
        std::thread Worker;
    };

    TqSpeedStart Start;
    uint16_t Port{0};
    TqSocketHandle Listener{TqInvalidSocket};
    std::atomic<bool> Stopping{false};
    std::atomic<uint64_t> ServerBytes{0};
    std::atomic<uint32_t> AcceptedConnections{0};
    std::atomic<uint32_t> ClosedConnections{0};
    TqClock::time_point StartedAt;
    std::thread AcceptThread;
    std::mutex Mutex;
    std::vector<std::shared_ptr<Connection>> Connections;
};

void TqCloseConnectionSocket(
    const std::shared_ptr<TqSpeedSession>& session,
    const std::shared_ptr<TqSpeedSession::Connection>& connection) {
    TqSocketHandle socket = TqInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(connection->Mutex);
        socket = connection->Socket;
        connection->Socket = TqInvalidSocket;
    }
    if (TqSocketValid(socket)) {
        TqCloseSocket(socket);
        session->ClosedConnections.fetch_add(1, std::memory_order_relaxed);
    }
}

void TqFillSpeedPayload(uint8_t* data, size_t length, uint64_t& state) {
    for (size_t i = 0; i < length; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        data[i] = static_cast<uint8_t>(state >> 24);
    }
}

bool TqSetSpeedSocketTimeout(TqSocketHandle socket, int timeoutMs) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

} // namespace

void TqTuneSpeedTestLocalSocket(TqSocketHandle socket) {
    if (!TqSocketValid(socket)) {
        return;
    }

    const int bufferBytes = TqGetActiveTcpSocketBuffer();
    const bool noDelayOk = TqSetNoDelay(socket);
    const bool receiveOk = TqSetSocketBuffer(socket, SO_RCVBUF, bufferBytes);
    const bool sendOk = TqSetSocketBuffer(socket, SO_SNDBUF, bufferBytes);
    const int effectiveReceive = TqGetSocketBuffer(socket, SO_RCVBUF);
    const int effectiveSend = TqGetSocketBuffer(socket, SO_SNDBUF);
    std::fprintf(stderr,
        "tcpquic-proxy speed socket tuning: requested=%d rcv=%d snd=%d ok_nodelay=%d ok_rcv=%d ok_snd=%d\n",
        bufferBytes,
        effectiveReceive,
        effectiveSend,
        noDelayOk ? 1 : 0,
        receiveOk ? 1 : 0,
        sendOk ? 1 : 0);
}

namespace {

void TqRunUploadWorker(
    const std::shared_ptr<TqSpeedSession>& session,
    const std::shared_ptr<TqSpeedSession::Connection>& connection) {
    std::array<uint8_t, 64 * 1024> buffer{};
    TqSocketHandle socket = TqInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(connection->Mutex);
        socket = connection->Socket;
    }

    for (;;) {
        const int rc = TqRecv(socket, buffer.data(), buffer.size(), TqRecvFlags::None);
        if (rc > 0) {
            session->ServerBytes.fetch_add(static_cast<uint64_t>(rc), std::memory_order_relaxed);
            continue;
        }
        if (rc == 0) {
            break;
        }
        const int error = TqLastSocketError();
        if (TqSocketInterrupted(error)) {
            continue;
        }
        if (TqSocketWouldBlock(error)) {
            if (session->Stopping.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        break;
    }

    TqCloseConnectionSocket(session, connection);
}

void TqRunDownloadWorker(
    const std::shared_ptr<TqSpeedSession>& session,
    const std::shared_ptr<TqSpeedSession::Connection>& connection) {
    std::array<uint8_t, 64 * 1024> buffer{};
    uint64_t payloadState = 0x4d595df4d0f33173ULL;
    TqFillSpeedPayload(buffer.data(), buffer.size(), payloadState);
    const auto deadline = TqClock::now() + std::chrono::seconds(session->Start.DurationSec);
    TqSocketHandle socket = TqInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(connection->Mutex);
        socket = connection->Socket;
    }

    while (!session->Stopping.load(std::memory_order_relaxed) && TqClock::now() < deadline) {
        const int rc = TqSend(socket, buffer.data(), buffer.size(), TqSendFlags::NoSignal);
        if (rc > 0) {
            session->ServerBytes.fetch_add(static_cast<uint64_t>(rc), std::memory_order_relaxed);
            continue;
        }
        if (rc == 0) {
            break;
        }
        const int error = TqLastSocketError();
        if (TqSocketInterrupted(error)) {
            continue;
        }
        if (TqSocketWouldBlock(error)) {
            if (session->Stopping.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        break;
    }

    if (session->Stopping.load(std::memory_order_relaxed)) {
        TqSocketHandle shutdownSocket = TqInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(connection->Mutex);
            shutdownSocket = connection->Socket;
        }
        if (TqSocketValid(shutdownSocket)) {
            (void)TqShutdownSend(shutdownSocket);
        }
        return;
    }

    TqCloseConnectionSocket(session, connection);
}

void TqRunAcceptLoop(const std::shared_ptr<TqSpeedSession>& session) {
    while (!session->Stopping.load(std::memory_order_relaxed)) {
        sockaddr_in addr{};
        TqSockLen addrLen = static_cast<TqSockLen>(sizeof(addr));
        TqSocketHandle accepted = ::accept(
            session->Listener,
            reinterpret_cast<sockaddr*>(&addr),
            &addrLen);
        if (!TqSocketValid(accepted)) {
            const int error = TqLastSocketError();
            if (!session->Stopping.load(std::memory_order_relaxed) && TqSocketInterrupted(error)) {
                continue;
            }
            break;
        }
        TqTuneSpeedTestLocalSocket(accepted);
        (void)TqSetSpeedSocketTimeout(accepted, 100);

        session->AcceptedConnections.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<TqSpeedSession::Connection> connection =
            std::make_shared<TqSpeedSession::Connection>(accepted);
        {
            std::lock_guard<std::mutex> lock(session->Mutex);
            session->Connections.push_back(connection);
        }
        try {
            if (session->Start.Direction == TqSpeedDirection::Upload) {
                connection->Worker = std::thread(TqRunUploadWorker, session, connection);
            } else {
                connection->Worker = std::thread(TqRunDownloadWorker, session, connection);
            }
        } catch (...) {
            TqCloseConnectionSocket(session, connection);
            break;
        }
    }
}

void TqStopSession(const std::shared_ptr<TqSpeedSession>& session) {
    if (!session) {
        return;
    }

    const bool wasStopping = session->Stopping.exchange(true, std::memory_order_relaxed);
    if (!wasStopping) {
        if (TqSocketValid(session->Listener)) {
            (void)TqShutdownBoth(session->Listener);
            TqCloseSocket(session->Listener);
            session->Listener = TqInvalidSocket;
        }
    }

    if (session->AcceptThread.joinable()) {
        session->AcceptThread.join();
    }

    std::vector<std::shared_ptr<TqSpeedSession::Connection>> connections;
    {
        std::lock_guard<std::mutex> lock(session->Mutex);
        connections = session->Connections;
    }

    if (session->Start.Direction == TqSpeedDirection::Upload) {
        const auto drainDeadline = TqClock::now() + std::chrono::milliseconds(200);
        for (;;) {
            bool allClosed = true;
            for (const auto& connection : connections) {
                TqSocketHandle socket = TqInvalidSocket;
                {
                    std::lock_guard<std::mutex> lock(connection->Mutex);
                    socket = connection->Socket;
                }
                if (TqSocketValid(socket)) {
                    allClosed = false;
                    break;
                }
            }
            if (allClosed || TqClock::now() >= drainDeadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    for (const auto& connection : connections) {
        TqSocketHandle socket = TqInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(connection->Mutex);
            socket = connection->Socket;
        }
        if (TqSocketValid(socket)) {
            (void)TqShutdownBoth(socket);
        }
    }
    for (const auto& connection : connections) {
        if (connection->Worker.joinable()) {
            connection->Worker.join();
        }
    }
    for (const auto& connection : connections) {
        TqCloseConnectionSocket(session, connection);
    }
    {
        std::lock_guard<std::mutex> lock(session->Mutex);
        session->Connections.clear();
    }
}

class TqClientControlStream final {
public:
    bool Open(MsQuicConnection& conn) {
        Stream_.reset(new (std::nothrow) MsQuicStream(
            conn,
            QUIC_STREAM_OPEN_FLAG_NONE,
            CleanUpManual,
            Callback,
            this));
        return Stream_ != nullptr && Stream_->IsValid();
    }

    bool SendStart(const TqSpeedStart& start) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeSpeedStart(start, encoded)) {
            SetFailure("failed to encode speed start");
            return false;
        }
        return SendFrame(encoded, QUIC_SEND_FLAG_START);
    }

    bool SendFinish(const TqSpeedFinish& finish) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeSpeedFinish(finish, encoded)) {
            SetFailure("failed to encode speed finish");
            return false;
        }
        std::fprintf(stderr,
            "tcpquic-proxy: speed client sending FINISH session=%u client_bytes=%llu elapsed_us=%llu\n",
            finish.SessionId,
            static_cast<unsigned long long>(finish.ClientBytes),
            static_cast<unsigned long long>(finish.ClientElapsedUs));
        return SendFrame(encoded, QUIC_SEND_FLAG_FIN);
    }

    bool WaitForReady(std::chrono::milliseconds timeout, TqSpeedReady& ready) {
        std::unique_lock<std::mutex> lock(Mutex_);
        if (!Cv_.wait_for(lock, timeout, [this] {
                return Ready_.has_value() || Error_.has_value() || Failed_ || Closed_;
            })) {
            Failure_ = "timed out waiting for SPEED_READY";
            Failed_ = true;
            return false;
        }
        if (!Ready_.has_value()) {
            return false;
        }
        ready = *Ready_;
        Ready_.reset();
        return true;
    }

    bool WaitForResult(std::chrono::milliseconds timeout, TqSpeedResult& result) {
        std::unique_lock<std::mutex> lock(Mutex_);
        std::fprintf(stderr, "tcpquic-proxy: speed client waiting for SPEED_RESULT\n");
        if (!Cv_.wait_for(lock, timeout, [this] {
                return Result_.has_value() || Error_.has_value() || Failed_ || Closed_;
            })) {
            Failure_ = "timed out waiting for SPEED_RESULT";
            Failed_ = true;
            return false;
        }
        if (!Result_.has_value()) {
            return false;
        }
        result = *Result_;
        Result_.reset();
        std::fprintf(stderr,
            "tcpquic-proxy: speed client received RESULT session=%u server_bytes=%llu elapsed_us=%llu accepted=%u closed=%u\n",
            result.SessionId,
            static_cast<unsigned long long>(result.ServerBytes),
            static_cast<unsigned long long>(result.ServerElapsedUs),
            result.AcceptedConnections,
            result.ClosedConnections);
        return true;
    }

    std::string FailureMessage() const {
        std::lock_guard<std::mutex> lock(Mutex_);
        if (Error_.has_value()) {
            std::string message = "server error: ";
            message += Error_->Message;
            return message;
        }
        return Failure_;
    }

private:
    static QUIC_STATUS QUIC_API Callback(
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept {
        auto* self = static_cast<TqClientControlStream*>(context);
        if (self == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        return self->OnStreamEvent(stream, event);
    }

    QUIC_STATUS OnStreamEvent(MsQuicStream*, QUIC_STREAM_EVENT* event) noexcept {
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            std::lock_guard<std::mutex> lock(Mutex_);
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                const auto& buffer = event->RECEIVE.Buffers[i];
                Rx_.insert(Rx_.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
            }
            ParseFramesLocked();
            break;
        }
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            TqControlSendContext::Delete(
                static_cast<TqControlSendContext*>(event->SEND_COMPLETE.ClientContext));
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            SetFailure("peer aborted speed control stream");
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            {
                std::lock_guard<std::mutex> lock(Mutex_);
                Closed_ = true;
            }
            Cv_.notify_all();
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    bool SendFrame(const std::vector<uint8_t>& data, QUIC_SEND_FLAGS flags) {
        if (Stream_ == nullptr || !Stream_->IsValid()) {
            SetFailure("speed control stream is not open");
            return false;
        }
        auto* sendContext = TqControlSendContext::New(data.data(), data.size());
        if (sendContext == nullptr) {
            SetFailure("failed to allocate speed control send buffer");
            return false;
        }
        const QUIC_STATUS status = Stream_->Send(&sendContext->Buffer, 1, flags, sendContext);
        if (QUIC_FAILED(status)) {
            TqControlSendContext::Delete(sendContext);
            SetFailure("failed to send speed control frame");
            return false;
        }
        return true;
    }

    void SetFailure(const char* message) {
        std::lock_guard<std::mutex> lock(Mutex_);
        Failure_ = message;
        Failed_ = true;
        Cv_.notify_all();
    }

    void ParseFramesLocked() {
        while (!Rx_.empty()) {
            size_t frameSize = 0;
            const TqControlFrameState state = TqTryGetFrameSize(Rx_, frameSize);
            if (state == TqControlFrameState::NeedMore) {
                return;
            }
            if (state == TqControlFrameState::Invalid) {
                Failure_ = "invalid speed control frame";
                Failed_ = true;
                Cv_.notify_all();
                return;
            }

            std::vector<uint8_t> frame(Rx_.begin(), Rx_.begin() + static_cast<ptrdiff_t>(frameSize));
            Rx_.erase(Rx_.begin(), Rx_.begin() + static_cast<ptrdiff_t>(frameSize));

            switch (frame[3]) {
            case TQ_CMD_SPEED_READY: {
                TqSpeedReady ready{};
                if (!TqDecodeSpeedReady(frame.data(), frame.size(), ready)) {
                    Failure_ = "failed to decode SPEED_READY";
                    Failed_ = true;
                    Cv_.notify_all();
                    return;
                }
                Ready_ = ready;
                std::fprintf(stderr,
                    "tcpquic-proxy: speed client received READY session=%u port=%u\n",
                    ready.SessionId,
                    static_cast<unsigned>(ready.Port));
                Cv_.notify_all();
                break;
            }
            case TQ_CMD_SPEED_RESULT: {
                TqSpeedResult result{};
                if (!TqDecodeSpeedResult(frame.data(), frame.size(), result)) {
                    Failure_ = "failed to decode SPEED_RESULT";
                    Failed_ = true;
                    Cv_.notify_all();
                    return;
                }
                Result_ = result;
                Cv_.notify_all();
                break;
            }
            case TQ_CMD_SPEED_ERROR: {
                TqSpeedErrorMessage error{};
                if (!TqDecodeSpeedError(frame.data(), frame.size(), error)) {
                    Failure_ = "failed to decode SPEED_ERROR";
                    Failed_ = true;
                    Cv_.notify_all();
                    return;
                }
                Error_ = error;
                Failed_ = true;
                Cv_.notify_all();
                return;
            }
            default:
                Failure_ = "unexpected speed control command";
                Failed_ = true;
                Cv_.notify_all();
                return;
            }
        }
    }

    std::unique_ptr<MsQuicStream> Stream_;
    mutable std::mutex Mutex_;
    std::condition_variable Cv_;
    std::vector<uint8_t> Rx_;
    std::optional<TqSpeedReady> Ready_;
    std::optional<TqSpeedResult> Result_;
    std::optional<TqSpeedErrorMessage> Error_;
    std::string Failure_;
    bool Failed_{false};
    bool Closed_{false};
};

struct TqPumpWorker {
    TqSocketHandle Socket{TqInvalidSocket};
    uint64_t Bytes{0};
    bool Failed{false};
    std::atomic<bool> Done{false};
    std::thread Thread;
};

void TqRunUploadPump(
    TqPumpWorker* worker,
    std::atomic<bool>* stop,
    TqClock::time_point deadline) {
    std::array<uint8_t, 64 * 1024> buffer{};
    uint64_t payloadState = 0x9e3779b97f4a7c15ULL;
    TqFillSpeedPayload(buffer.data(), buffer.size(), payloadState);

    while (!stop->load(std::memory_order_relaxed) && TqClock::now() < deadline) {
        const int rc = TqSend(worker->Socket, buffer.data(), buffer.size(), TqSendFlags::NoSignal);
        if (rc > 0) {
            worker->Bytes += static_cast<uint64_t>(rc);
            continue;
        }
        if (rc == 0) {
            break;
        }
        const int error = TqLastSocketError();
        if (TqSocketInterrupted(error)) {
            continue;
        }
        if (stop->load(std::memory_order_relaxed) || TqClock::now() >= deadline) {
            break;
        }
        worker->Failed = true;
        break;
    }

    (void)TqShutdownSend(worker->Socket);
    worker->Done.store(true, std::memory_order_release);
}

void TqRunDownloadPump(TqPumpWorker* worker, std::atomic<bool>* stop) {
    std::array<uint8_t, 64 * 1024> buffer{};
    for (;;) {
        const int rc = TqRecv(worker->Socket, buffer.data(), buffer.size(), TqRecvFlags::None);
        if (rc > 0) {
            worker->Bytes += static_cast<uint64_t>(rc);
            continue;
        }
        if (rc == 0) {
            break;
        }
        const int error = TqLastSocketError();
        if (TqSocketInterrupted(error)) {
            continue;
        }
        if (!stop->load(std::memory_order_relaxed)) {
            worker->Failed = true;
        }
        break;
    }
    worker->Done.store(true, std::memory_order_release);
}

class TqServerSpeedControlStreamContext final {
public:
    TqServerSpeedControlStreamContext(
        TqServerSpeedTestController& controller,
        MsQuicStream* stream,
        std::function<void()> onComplete) :
        Controller_(controller),
        Stream_(stream),
        OnComplete_(std::move(onComplete)) {}

    ~TqServerSpeedControlStreamContext() {
        CleanupSession();
        if (OnComplete_) {
            OnComplete_();
        }
    }

    bool Prime(std::vector<uint8_t> initialBytes) {
        if (Stream_ == nullptr || !Stream_->IsValid()) {
            return false;
        }
        if (!initialBytes.empty()) {
            BufferedRx_ = std::move(initialBytes);
            ProcessBuffered();
        }
        return true;
    }

    static QUIC_STATUS QUIC_API Callback(
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept {
        auto* self = static_cast<TqServerSpeedControlStreamContext*>(context);
        if (self == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        return self->OnStreamEvent(stream, event);
    }

private:
    QUIC_STATUS OnStreamEvent(MsQuicStream*, QUIC_STREAM_EVENT* event) noexcept {
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            if (FinalFrameQueued_) {
                break;
            }
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                const auto& buffer = event->RECEIVE.Buffers[i];
                BufferedRx_.insert(BufferedRx_.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
            }
            ProcessBuffered();
            break;
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            TqControlSendContext::Delete(
                static_cast<TqControlSendContext*>(event->SEND_COMPLETE.ClientContext));
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            CleanupSession();
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            Stream_ = nullptr;
            delete this;
            break;
        default:
            break;
        }
        return QUIC_STATUS_SUCCESS;
    }

    void ProcessBuffered() {
        while (!FinalFrameQueued_) {
            if (!SessionStarted_) {
                if (BufferedRx_.size() < TQ_SPEED_START_SIZE) {
                    return;
                }
                TqSpeedStart start{};
                if (!TqDecodeSpeedStart(BufferedRx_.data(), TQ_SPEED_START_SIZE, start)) {
                    SendErrorAndFinish(0, TqSpeedError::InvalidRequest, "invalid speed start");
                    return;
                }
                std::fprintf(stderr,
                    "tcpquic-proxy: speed server control decoded START session=%u direction=%s\n",
                    start.SessionId,
                    TqSpeedDirectionName(start.Direction));
                BufferedRx_.erase(
                    BufferedRx_.begin(),
                    BufferedRx_.begin() + static_cast<ptrdiff_t>(TQ_SPEED_START_SIZE));

                TqSpeedReady ready{};
                if (!Controller_.StartSession(start, ready)) {
                    const TqSpeedError error =
                        start.SessionId == 0 ? TqSpeedError::InvalidRequest : TqSpeedError::Internal;
                    SendErrorAndFinish(start.SessionId, error, "failed to start speed session");
                    return;
                }

                SessionStarted_ = true;
                SessionId_ = start.SessionId;
                SessionActive_ = true;
                if (!SendReady(ready)) {
                    AbortAndCleanupSession();
                    return;
                }
                continue;
            }

            if (BufferedRx_.size() < TQ_SPEED_FINISH_SIZE) {
                return;
            }
            TqSpeedFinish finish{};
            if (!TqDecodeSpeedFinish(BufferedRx_.data(), TQ_SPEED_FINISH_SIZE, finish)) {
                SendErrorAndFinish(SessionId_, TqSpeedError::InvalidRequest, "invalid speed finish");
                return;
            }
            std::fprintf(stderr,
                "tcpquic-proxy: speed server control decoded FINISH session=%u client_bytes=%llu elapsed_us=%llu\n",
                finish.SessionId,
                static_cast<unsigned long long>(finish.ClientBytes),
                static_cast<unsigned long long>(finish.ClientElapsedUs));
            BufferedRx_.erase(
                BufferedRx_.begin(),
                BufferedRx_.begin() + static_cast<ptrdiff_t>(TQ_SPEED_FINISH_SIZE));

            if (finish.SessionId != SessionId_) {
                SendErrorAndFinish(SessionId_, TqSpeedError::InvalidRequest, "speed session mismatch");
                return;
            }

            TqSpeedResult result{};
            if (!Controller_.FinishSession(
                    finish.SessionId,
                    finish.ClientBytes,
                    finish.ClientElapsedUs,
                    result)) {
                SessionActive_ = false;
                SendErrorAndFinish(SessionId_, TqSpeedError::Internal, "failed to finish speed session");
                return;
            }

            SessionActive_ = false;
            if (!SendResultAndFinish(result)) {
                AbortAndCleanupSession();
            } else {
                std::fprintf(stderr,
                    "tcpquic-proxy: speed server control queued RESULT session=%u server_bytes=%llu\n",
                    result.SessionId,
                    static_cast<unsigned long long>(result.ServerBytes));
            }
            return;
        }
    }

    bool SendReady(const TqSpeedReady& ready) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeSpeedReady(ready, encoded)) {
            return false;
        }
        return SendFrame(encoded, QUIC_SEND_FLAG_NONE);
    }

    bool SendResultAndFinish(const TqSpeedResult& result) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeSpeedResult(result, encoded)) {
            return false;
        }
        FinalFrameQueued_ = true;
        return SendFrame(encoded, QUIC_SEND_FLAG_FIN);
    }

    void SendErrorAndFinish(uint32_t sessionId, TqSpeedError error, const char* message) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeSpeedError(TqSpeedErrorMessage{sessionId, error, message}, encoded) ||
            !SendFrame(encoded, QUIC_SEND_FLAG_FIN)) {
            AbortAndCleanupSession();
            return;
        }
        FinalFrameQueued_ = true;
    }

    bool SendFrame(const std::vector<uint8_t>& data, QUIC_SEND_FLAGS flags) {
        if (Stream_ == nullptr || !Stream_->IsValid()) {
            return false;
        }
        auto* sendContext = TqControlSendContext::New(data.data(), data.size());
        if (sendContext == nullptr) {
            return false;
        }
        const QUIC_STATUS status = Stream_->Send(&sendContext->Buffer, 1, flags, sendContext);
        if (QUIC_FAILED(status)) {
            TqControlSendContext::Delete(sendContext);
            return false;
        }
        return true;
    }

    void CleanupSession() {
        if (!SessionActive_) {
            return;
        }
        TqSpeedResult ignored{};
        (void)Controller_.FinishSession(SessionId_, 0, 0, ignored);
        SessionActive_ = false;
    }

    void AbortAndCleanupSession() {
        CleanupSession();
        if (Stream_ != nullptr) {
            (void)Stream_->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
    }

    TqServerSpeedTestController& Controller_;
    MsQuicStream* Stream_{nullptr};
    std::function<void()> OnComplete_;
    std::vector<uint8_t> BufferedRx_;
    bool SessionStarted_{false};
    bool SessionActive_{false};
    bool FinalFrameQueued_{false};
    uint32_t SessionId_{0};
};

} // namespace

struct TqServerSpeedTestController::Impl {
    std::mutex Mutex;
    std::unordered_map<uint32_t, std::shared_ptr<TqSpeedSession>> SessionsById;
    std::unordered_map<uint16_t, std::shared_ptr<TqSpeedSession>> SessionsByPort;
};

TqServerSpeedTestController::TqServerSpeedTestController() : Impl_(new Impl()) {}

TqServerSpeedTestController::~TqServerSpeedTestController() {
    StopAll();
    delete Impl_;
    Impl_ = nullptr;
}

bool TqServerSpeedTestController::StartSession(const TqSpeedStart& start, TqSpeedReady& ready) {
    if (Impl_ == nullptr ||
        start.SessionId == 0 ||
        !TqIsValidDirection(start.Direction) ||
        start.DurationSec == 0 ||
        start.Parallel == 0 ||
        start.Flags != 0) {
        return false;
    }

    std::fprintf(stderr,
        "tcpquic-proxy: speed server START session=%u direction=%s duration=%u parallel=%u\n",
        start.SessionId,
        TqSpeedDirectionName(start.Direction),
        static_cast<unsigned>(start.DurationSec),
        static_cast<unsigned>(start.Parallel));

    auto session = std::make_shared<TqSpeedSession>(start);
    if (!TqCreateLoopbackListener(start.Parallel, session->Listener, session->Port)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(Impl_->Mutex);
        if (Impl_->SessionsById.find(start.SessionId) != Impl_->SessionsById.end() ||
            Impl_->SessionsByPort.find(session->Port) != Impl_->SessionsByPort.end()) {
            TqCloseSocket(session->Listener);
            session->Listener = TqInvalidSocket;
            return false;
        }
        Impl_->SessionsById.emplace(start.SessionId, session);
        Impl_->SessionsByPort.emplace(session->Port, session);
    }

    try {
        session->AcceptThread = std::thread(TqRunAcceptLoop, session);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(Impl_->Mutex);
            Impl_->SessionsById.erase(start.SessionId);
            Impl_->SessionsByPort.erase(session->Port);
        }
        TqStopSession(session);
        return false;
    }

    ready = {};
    ready.SessionId = start.SessionId;
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = session->Port;
    ready.Addr = {127, 0, 0, 1};
    std::fprintf(stderr,
        "tcpquic-proxy: speed server READY session=%u port=%u\n",
        start.SessionId,
        static_cast<unsigned>(session->Port));
    return true;
}

bool TqServerSpeedTestController::FinishSession(
    uint32_t sessionId, uint64_t clientBytes, uint64_t, TqSpeedResult& result) {
    if (Impl_ == nullptr || sessionId == 0) {
        return false;
    }

    std::shared_ptr<TqSpeedSession> session;
    {
        std::lock_guard<std::mutex> lock(Impl_->Mutex);
        auto it = Impl_->SessionsById.find(sessionId);
        if (it == Impl_->SessionsById.end()) {
            return false;
        }
        session = it->second;
        Impl_->SessionsById.erase(it);
        Impl_->SessionsByPort.erase(session->Port);
    }

    std::fprintf(stderr,
        "tcpquic-proxy: speed server FINISH begin session=%u bytes=%llu accepted=%u closed=%u\n",
        sessionId,
        static_cast<unsigned long long>(session->ServerBytes.load(std::memory_order_relaxed)),
        session->AcceptedConnections.load(std::memory_order_relaxed),
        session->ClosedConnections.load(std::memory_order_relaxed));
    if (clientBytes > 0) {
        const auto finishDeadline = TqClock::now() + std::chrono::seconds(2);
        while (TqClock::now() < finishDeadline) {
            const uint64_t serverBytes = session->ServerBytes.load(std::memory_order_relaxed);
            const uint32_t closed = session->ClosedConnections.load(std::memory_order_relaxed);
            if (serverBytes >= clientBytes ||
                (closed >= session->AcceptedConnections.load(std::memory_order_relaxed) && serverBytes > 0)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    TqStopSession(session);

    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        TqClock::now() - session->StartedAt).count();
    result = {};
    result.SessionId = sessionId;
    result.ServerBytes = session->ServerBytes.load(std::memory_order_relaxed);
    result.ServerElapsedUs = static_cast<uint64_t>(elapsedUs >= 0 ? elapsedUs : 0);
    result.AcceptedConnections = session->AcceptedConnections.load(std::memory_order_relaxed);
    result.ClosedConnections = session->ClosedConnections.load(std::memory_order_relaxed);
    result.Status = 0;
    std::fprintf(stderr,
        "tcpquic-proxy: speed server FINISH done session=%u bytes=%llu elapsed_us=%llu accepted=%u closed=%u\n",
        sessionId,
        static_cast<unsigned long long>(result.ServerBytes),
        static_cast<unsigned long long>(result.ServerElapsedUs),
        result.AcceptedConnections,
        result.ClosedConnections);
    return true;
}

void TqServerSpeedTestController::StopAll() {
    if (Impl_ == nullptr) {
        return;
    }

    std::vector<std::shared_ptr<TqSpeedSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(Impl_->Mutex);
        sessions.reserve(Impl_->SessionsById.size());
        for (const auto& entry : Impl_->SessionsById) {
            sessions.push_back(entry.second);
        }
        Impl_->SessionsById.clear();
        Impl_->SessionsByPort.clear();
    }

    for (const auto& session : sessions) {
        TqStopSession(session);
    }
}

bool TqServerSpeedTestController::IsAllowedEphemeralTarget(const std::string& host, uint16_t port) const {
    if (Impl_ == nullptr || host != "127.0.0.1" || port == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(Impl_->Mutex);
    return Impl_->SessionsByPort.find(port) != Impl_->SessionsByPort.end();
}

TqConfig TqMakeSpeedClientSessionConfig(const TqConfig& cfg) {
    TqConfig speedCfg = cfg;
    speedCfg.QuicConnections = cfg.QuicConnections + 1;
    return speedCfg;
}

bool TqRunClientSpeedTest(QuicClientSession& quic, const TqConfig& cfg) {
    const uint16_t parallel = static_cast<uint16_t>(std::max<uint32_t>(1, cfg.QuicConnections));
    const uint32_t neededConnections = static_cast<uint32_t>(parallel) + 1u;
    const auto connectDeadline = TqClock::now() + std::chrono::seconds(10);
    while (!TqSpeedConnectWaitShouldStop(
        quic.ConnectedConnectionCount(),
        neededConnections,
        TqClock::now() >= connectDeadline)) {
        (void)quic.EnsureAnyConnected(std::chrono::milliseconds(100));
        if (TqClock::now() < connectDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            break;
        }
    }
    if (quic.ConnectedConnectionCount() < neededConnections) {
        std::fprintf(stderr, "tcpquic-proxy: speed test could not connect to QUIC peer\n");
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    MsQuicConnection* controlConn = quic.PickConnectionAt(0);
    if (controlConn == nullptr) {
        std::fprintf(stderr, "tcpquic-proxy: speed test has no connected QUIC control connection\n");
        return false;
    }

    TqSpeedStart start{};
    start.SessionId = TqMakeSessionId();
    start.Direction = cfg.SpeedTestMode == TqSpeedTestMode::Upload
        ? TqSpeedDirection::Upload
        : TqSpeedDirection::Download;
    start.DurationSec = cfg.SpeedTestDurationSec;
    start.Parallel = parallel;

    TqClientControlStream control;
    if (!control.Open(*controlConn)) {
        std::fprintf(stderr, "tcpquic-proxy: failed to open speed control stream\n");
        return false;
    }
    if (!control.SendStart(start)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", control.FailureMessage().c_str());
        return false;
    }

    TqSpeedReady ready{};
    if (!control.WaitForReady(std::chrono::seconds(10), ready)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", control.FailureMessage().c_str());
        return false;
    }

    TunnelRequest req{};
    req.AddrType = ready.AddrType;
    req.Port = ready.Port;
    req.CompressFlags = 0;
    const std::string readyHost = TqReadyAddressToHost(ready);
    if (readyHost.empty() || readyHost.size() >= sizeof(req.Host)) {
        std::fprintf(stderr, "tcpquic-proxy: invalid speed test target address\n");
        return false;
    }
    std::snprintf(req.Host, sizeof(req.Host), "%s", readyHost.c_str());

    TqConfig tunnelCfg = cfg;
    if (tunnelCfg.Compress == "auto") {
        tunnelCfg.Compress = "off";
    }

    std::vector<TqPumpWorker> workers(parallel);
    std::vector<std::atomic<uint64_t>> sinkBytes(parallel);
    const bool receiveSink = cfg.SpeedTestMode == TqSpeedTestMode::DownloadSink;
    bool ok = false;
    const auto startedAt = TqClock::now();
    const auto deadline = startedAt + std::chrono::seconds(cfg.SpeedTestDurationSec);
    std::atomic<bool> stop{false};
    const uint64_t localElapsedUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - startedAt).count());
    auto countWorkerBytes = [&workers]() {
        uint64_t total = 0;
        for (const auto& worker : workers) {
            total += worker.Bytes;
        }
        return total;
    };
    auto countSinkBytes = [&sinkBytes]() {
        uint64_t total = 0;
        for (const auto& bytes : sinkBytes) {
            total += bytes.load(std::memory_order_relaxed);
        }
        return total;
    };
    auto anyWorkerFailed = [&workers]() {
        for (const auto& worker : workers) {
            if (worker.Failed) {
                return true;
            }
        }
        return false;
    };
    uint64_t finishClientBytes = 0;
    uint64_t measuredLocalBytes = 0;
    TqSpeedFinish finish{};
    TqSpeedResult result{};

    for (uint16_t i = 0; i < parallel; ++i) {
        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!receiveSink && !TqSocketPair(pair)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to create speed test socket pair %u\n", i);
            goto cleanup;
        }
        if (!receiveSink) {
            TqTuneSpeedTestLocalSocket(pair[0]);
            TqTuneSpeedTestLocalSocket(pair[1]);
        }

        if (!quic.EnsureAnyConnected()) {
            std::fprintf(stderr, "tcpquic-proxy: speed test lost all QUIC connections\n");
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            goto cleanup;
        }
        MsQuicConnection* dataConn = quic.PickConnectionFrom(1);
        if (dataConn == nullptr) {
            std::fprintf(stderr, "tcpquic-proxy: failed to pick QUIC connection for speed tunnel\n");
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            goto cleanup;
        }

        const TqTunnelStartResult started = receiveSink
            ? TqStartClientTunnelReceiveSink(dataConn, req, tunnelCfg, &sinkBytes[i])
            : TqStartClientTunnel(dataConn, req, pair[0], tunnelCfg);
        if (!started.Ok) {
            std::fprintf(stderr,
                "tcpquic-proxy: failed to start speed tunnel %u (error=%u)\n",
                i,
                static_cast<unsigned>(started.Error));
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            goto cleanup;
        }

        workers[i].Socket = pair[1];
    }

    if (!receiveSink) {
        for (auto& worker : workers) {
            if (start.Direction == TqSpeedDirection::Upload) {
                worker.Thread = std::thread(TqRunUploadPump, &worker, &stop, deadline);
            } else {
                worker.Thread = std::thread(TqRunDownloadPump, &worker, &stop);
            }
        }
    }

    std::this_thread::sleep_until(deadline);
    if (receiveSink) {
        finishClientBytes = countSinkBytes();
    } else if (start.Direction == TqSpeedDirection::Upload) {
        stop.store(true, std::memory_order_relaxed);
        for (auto& worker : workers) {
            if (!worker.Done.load(std::memory_order_acquire) && TqSocketValid(worker.Socket)) {
                (void)TqShutdownBoth(worker.Socket);
            }
        }
        for (auto& worker : workers) {
            if (worker.Thread.joinable()) {
                worker.Thread.join();
            }
        }
        finishClientBytes = countWorkerBytes();
        if (anyWorkerFailed()) {
            std::fprintf(stderr, "tcpquic-proxy: speed test pump worker failed\n");
            goto cleanup;
        }
    } else {
        finishClientBytes = countWorkerBytes();
    }
    measuredLocalBytes = finishClientBytes;

    finish.SessionId = start.SessionId;
    finish.ClientBytes = finishClientBytes;
    finish.ClientElapsedUs = localElapsedUs;
    if (!control.SendFinish(finish)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", control.FailureMessage().c_str());
        goto cleanup;
    }

    if (!control.WaitForResult(std::chrono::seconds(15), result)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", control.FailureMessage().c_str());
        goto cleanup;
    }

    if (start.Direction == TqSpeedDirection::Download && !receiveSink) {
        const auto drainDeadline = TqClock::now() + std::chrono::seconds(5);
        for (;;) {
            bool allDone = true;
            for (const auto& worker : workers) {
                if (!worker.Done.load(std::memory_order_acquire)) {
                    allDone = false;
                    break;
                }
            }
            if (allDone || TqClock::now() >= drainDeadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto& worker : workers) {
            if (!worker.Done.load(std::memory_order_acquire) && TqSocketValid(worker.Socket)) {
                (void)TqShutdownBoth(worker.Socket);
            }
        }
        for (auto& worker : workers) {
            if (worker.Thread.joinable()) {
                worker.Thread.join();
            }
        }
        if (anyWorkerFailed()) {
            std::fprintf(stderr, "tcpquic-proxy: speed test pump worker failed\n");
            goto cleanup;
        }
    }

    {
        const uint64_t localBytes = measuredLocalBytes;
        const char* modeName = receiveSink
            ? "download-sink"
            : (start.Direction == TqSpeedDirection::Upload ? "upload" : "download");
        const uint64_t throughputBytes =
            start.Direction == TqSpeedDirection::Upload ? result.ServerBytes : localBytes;
        const uint64_t throughputElapsedUs =
            start.Direction == TqSpeedDirection::Upload ? result.ServerElapsedUs : localElapsedUs;
        std::printf(
            "speed-test %s: local_bytes=%llu server_bytes=%llu local_seconds=%.3f "
            "server_seconds=%.3f gbps=%.3f mib_s=%.2f accepted=%u closed=%u\n",
            modeName,
            static_cast<unsigned long long>(localBytes),
            static_cast<unsigned long long>(result.ServerBytes),
            TqElapsedSeconds(localElapsedUs),
            TqElapsedSeconds(result.ServerElapsedUs),
            TqGbps(throughputBytes, throughputElapsedUs),
            TqMiBPerSec(throughputBytes, throughputElapsedUs),
            result.AcceptedConnections,
            result.ClosedConnections);

        ok = result.Status == 0;
        if (result.AcceptedConnections != parallel || result.ClosedConnections != parallel) {
            std::fprintf(stderr,
                "tcpquic-proxy: speed test expected %u accepted/closed connections, got %u/%u\n",
                parallel,
                result.AcceptedConnections,
                result.ClosedConnections);
            ok = false;
        }

        uint64_t diffBytes = 0;
        uint64_t limitBytes = 0;
        if (start.Direction == TqSpeedDirection::Upload &&
            !TqSpeedByteCountsCloseEnough(localBytes, result.ServerBytes, diffBytes, limitBytes)) {
            std::fprintf(stderr,
                "tcpquic-proxy: speed test local/server byte mismatch exceeds limit "
                "(local=%llu server=%llu diff=%llu limit=%llu)\n",
                static_cast<unsigned long long>(localBytes),
                static_cast<unsigned long long>(result.ServerBytes),
                static_cast<unsigned long long>(diffBytes),
                static_cast<unsigned long long>(limitBytes));
            ok = false;
        } else if (start.Direction == TqSpeedDirection::Download) {
            if (localBytes == 0 || result.ServerBytes == 0) {
                std::fprintf(stderr,
                    "tcpquic-proxy: invalid download byte counts "
                    "(local=%llu server=%llu)\n",
                    static_cast<unsigned long long>(localBytes),
                    static_cast<unsigned long long>(result.ServerBytes));
                ok = false;
            } else if (!TqSpeedByteCountsCloseEnough(localBytes, result.ServerBytes, diffBytes, limitBytes)) {
                std::fprintf(stderr,
                    "tcpquic-proxy: download local/server byte mismatch exceeds limit "
                    "(local=%llu server=%llu diff=%llu limit=%llu); throughput uses local bytes\n",
                    static_cast<unsigned long long>(localBytes),
                    static_cast<unsigned long long>(result.ServerBytes),
                    static_cast<unsigned long long>(diffBytes),
                    static_cast<unsigned long long>(limitBytes));
                ok = false;
            }
        }
    }

cleanup:
    stop.store(true, std::memory_order_relaxed);
    for (auto& worker : workers) {
        if (TqSocketValid(worker.Socket)) {
            (void)TqShutdownBoth(worker.Socket);
        }
    }
    for (auto& worker : workers) {
        if (worker.Thread.joinable()) {
            worker.Thread.join();
        }
        TqCloseSocketIfValid(worker.Socket);
    }
    return ok;
}

void TqHandleServerSpeedControlStream(
    TqServerSpeedTestController& controller,
    MsQuicConnection* conn,
    HQUIC rawStream) {
    if (rawStream == nullptr) {
        return;
    }

    auto* stream = new (std::nothrow) MsQuicStream(
        rawStream,
        CleanUpAutoDelete,
        TqServerSpeedControlStreamContext::Callback,
        nullptr);
    if (stream == nullptr || !stream->IsValid()) {
        delete stream;
        MsQuic->StreamClose(rawStream);
        return;
    }

    if (!TqAttachServerSpeedControlStream(controller, conn, stream, {}, {})) {
        (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
}

bool TqAttachServerSpeedControlStream(
    TqServerSpeedTestController& controller,
    MsQuicConnection* conn,
    MsQuicStream* stream,
    std::vector<uint8_t> initialBytes,
    std::function<void()> onComplete) {
    if (stream == nullptr || !stream->IsValid()) {
        return false;
    }

    (void)conn;
    auto* context = new (std::nothrow) TqServerSpeedControlStreamContext(
        controller,
        stream,
        std::move(onComplete));
    if (context == nullptr) {
        return false;
    }

    stream->Callback = TqServerSpeedControlStreamContext::Callback;
    stream->Context = context;
    if (!context->Prime(std::move(initialBytes))) {
        delete context;
        return false;
    }
    return true;
}
