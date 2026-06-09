#include "tcp_tunnel.h"

#include "compress.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "quic_session.h"
#include "relay.h"
#include "tcp_dialer.h"
#include "tunnel_reaper.h"

#if !defined(_WIN32)
#include <arpa/inet.h>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace {

constexpr auto TqOpenTimeout = std::chrono::seconds(10);
constexpr int TqTcpDialTimeoutMs = 10 * 1000;

struct TqTunnelSendContext {
    QUIC_BUFFER Buffer;
    uint8_t Data[1];

    static TqTunnelSendContext* New(const uint8_t* data, size_t length) {
        if (length > UINT32_MAX || (length > 0 && data == nullptr)) {
            return nullptr;
        }

        const size_t allocSize =
            sizeof(TqTunnelSendContext) + (length == 0 ? 0 : length - 1);
        auto* context = static_cast<TqTunnelSendContext*>(std::malloc(allocSize));
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

    static void Delete(TqTunnelSendContext* context) {
        std::free(context);
    }
};

enum class TqTunnelRole {
    ClientOpen,
    ServerOpen,
};

TqCompressAlgo TqAlgoFromFlags(uint8_t flags) {
    if ((flags & TQ_FLAG_COMPRESS) == 0) {
        return TqCompressAlgo::None;
    }
    if ((flags & TQ_FLAG_COMPRESS_LZ4) != 0) {
        return TqCompressAlgo::Lz4;
    }
    return TqCompressAlgo::Zstd;
}

uint8_t TqFlagsFromConfig(const TunnelRequest& req, const TqConfig& cfg) {
    uint8_t flags = req.CompressFlags;
    flags &= static_cast<uint8_t>(~(TQ_FLAG_COMPRESS | TQ_FLAG_COMPRESS_LZ4));

    const char* compressMode = cfg.Compress.c_str();
    if (cfg.Compress == "auto") {
        compressMode = TqResolveAutoCompress(cfg);
    }

    if (std::strcmp(compressMode, "zstd") == 0) {
        flags |= TQ_FLAG_COMPRESS;
    } else if (std::strcmp(compressMode, "lz4") == 0) {
        flags |= TQ_FLAG_COMPRESS | TQ_FLAG_COMPRESS_LZ4;
    }

    if (req.AddrType == TQ_ADDR_DOMAIN) {
        flags |= TQ_FLAG_DNS_REMOTE;
    }

    return flags;
}

bool TqBuildOpenRequest(const TunnelRequest& req, const TqConfig& cfg, TqOpenRequest& out) {
    if (req.Port == 0 || req.Host[0] == '\0') {
        return false;
    }

    out = TqOpenRequest{};
    out.Flags = TqFlagsFromConfig(req, cfg);
    out.AddrType = req.AddrType;
    out.Port = req.Port;

    if (req.AddrType == TQ_ADDR_DOMAIN) {
        const size_t hostLen = strnlen(req.Host, sizeof(req.Host));
        if (hostLen == 0 || hostLen >= sizeof(req.Host)) {
            return false;
        }
        out.Addr.assign(req.Host, req.Host + hostLen);
        return true;
    }

    if (req.AddrType == TQ_ADDR_IPV4) {
        in_addr addr{};
        if (!TqInetPton(AF_INET, req.Host, &addr)) {
            return false;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(&addr);
        out.Addr.assign(bytes, bytes + 4);
        return true;
    }

    if (req.AddrType == TQ_ADDR_IPV6) {
        in6_addr addr{};
        if (!TqInetPton(AF_INET6, req.Host, &addr)) {
            return false;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(&addr);
        out.Addr.assign(bytes, bytes + 16);
        return true;
    }

    return false;
}

bool TqHostFromOpenRequest(const TqOpenRequest& req, std::string& host) {
    char text[INET6_ADDRSTRLEN]{};

    if (req.AddrType == TQ_ADDR_DOMAIN) {
        host.assign(req.Addr.begin(), req.Addr.end());
        return !host.empty();
    }

    if (req.AddrType == TQ_ADDR_IPV4 && req.Addr.size() == 4) {
        return TqInetNtop(AF_INET, req.Addr.data(), text, sizeof(text)) != nullptr &&
            (host = text, true);
    }

    if (req.AddrType == TQ_ADDR_IPV6 && req.Addr.size() == 16) {
        return TqInetNtop(AF_INET6, req.Addr.data(), text, sizeof(text)) != nullptr &&
            (host = text, true);
    }

    return false;
}

bool TqDomainResolves(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    const int status = getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    return status == 0;
}

bool TqResolveAllowedTarget(
    const TqOpenRequest& req,
    const TqAcl& acl,
    std::vector<sockaddr_storage>& addrs,
    TqOpenError& error) {
    std::string host;
    if (!TqHostFromOpenRequest(req, host)) {
        error = TqOpenError::Internal;
        return false;
    }

    if (req.AddrType == TQ_ADDR_DOMAIN && !TqDomainResolves(host, req.Port)) {
        error = TqOpenError::DnsFailed;
        return false;
    }

    if (!TqAclResolveAndFilter(acl, host, req.Port, addrs)) {
        error = TqOpenError::AclDenied;
        return false;
    }

    return true;
}

} // namespace

class TqTunnelContext final {
public:
    friend bool TqTunnelRelayStopped(const TqTunnelContext* ctx);
    friend void TqReapTunnelContext(TqTunnelContext* ctx);

public:
    TqTunnelContext(
        TqTunnelRole role,
        MsQuicStream* stream,
        TqSocketHandle tcpFd,
        const TqConfig& cfg,
        const TqAcl* acl,
        MsQuicConnection* quicConn = nullptr,
        TqTunnelCompletionFn onComplete = {},
        TqTunnelAclDeniedFn onAclDenied = {}) :
        Role(role),
        Stream(stream),
        TcpFd(tcpFd),
        Config(cfg),
        Acl(acl),
        QuicConn(quicConn),
        OnComplete(std::move(onComplete)),
        OnAclDenied(std::move(onAclDenied)) {
    }

    ~TqTunnelContext() {
        if (OnComplete) {
            OnComplete();
        }
    }

    TqTunnelContext(const TqTunnelContext&) = delete;
    TqTunnelContext& operator=(const TqTunnelContext&) = delete;

    static QUIC_STATUS QUIC_API Callback(
        _In_ MsQuicStream* stream,
        _In_opt_ void* context,
        _Inout_ QUIC_STREAM_EVENT* event) noexcept {
        auto* tunnel = static_cast<TqTunnelContext*>(context);
        if (tunnel == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        return tunnel->OnStreamEvent(stream, event);
    }

    void SetStream(MsQuicStream* stream) {
        Stream = stream;
    }

    bool SendOpenRequest(const TqOpenRequest& req) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeOpenRequest(req, encoded)) {
            return false;
        }
        return SendBytes(encoded, QUIC_SEND_FLAG_START);
    }

    bool WaitForOpenResponse(TqOpenResponse& response) {
        std::unique_lock<std::mutex> guard(Lock);
        if (!StateChanged.wait_for(guard, TqOpenTimeout, [this] { return OpenDone; })) {
            OpenDone = true;
            OpenOk = false;
            OpenResponse = TqOpenResponse{false, TqOpenError::TcpTimeout, 0};
        }
        response = OpenResponse;
        return OpenOk;
    }

    bool StartRelay(uint8_t flags) {
        const TqCompressAlgo algo = TqAlgoFromFlags(flags);
        if (algo != TqCompressAlgo::None) {
            Compressor = TqCreateCompressor(algo, Config.CompressLevel);
            Decompressor = TqCreateDecompressor(algo);
            if (!Compressor || !Decompressor) {
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> guard(Lock);
            if (Stream == nullptr || ShutdownComplete || StreamShutdownQueued) {
                return false;
            }
            if (!TqRelayStart(
                    TcpFd,
                    Stream,
                    Compressor.get(),
                    Decompressor.get(),
                    &RelayHandle,
                    Config.Tuning,
                    algo)) {
                return false;
            }

            RelayStarted = true;
        }
        TqTunnelReaper::Instance().Register(this);
        return true;
    }

    void ArmSelfDeleteOnShutdown() {
        std::lock_guard<std::mutex> guard(Lock);
        SelfDeleteOnShutdown = true;
        if (Stream != nullptr && !ShutdownComplete && !StreamShutdownQueued) {
            StreamShutdownQueued = true;
            (void)Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
    }

    void CloseTcp() {
        if (TqSocketValid(TcpFd)) {
            TqCloseSocket(TcpFd);
            TcpFd = TqInvalidSocket;
        }
    }

private:
    bool ShouldDeletePreRelayLocked() const {
        return SelfDeleteOnShutdown &&
            !RelayStarted &&
            !PendingServerOpen &&
            (ShutdownComplete || (!StreamShutdownQueued && Stream == nullptr));
    }

    QUIC_STATUS OnStreamEvent(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept {
        bool deleteAfterEvent = false;
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            return OnReceive(stream, event);
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            TqTunnelSendContext::Delete(
                static_cast<TqTunnelSendContext*>(event->SEND_COMPLETE.ClientContext));
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            if (Role == TqTunnelRole::ClientOpen) {
                TryCompleteClientOpen();
            }
            CompleteOpen(false, TqOpenResponse{false, TqOpenError::Internal, 0});
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
            if (Role == TqTunnelRole::ClientOpen) {
                TryCompleteClientOpen();
            }
            CompleteOpen(false, TqOpenResponse{false, TqOpenError::Internal, 0});
            {
                std::lock_guard<std::mutex> guard(Lock);
                ShutdownComplete = true;
                if (Role == TqTunnelRole::ServerOpen && !RelayStarted) {
                    SelfDeleteOnShutdown = true;
                }
                Stream = nullptr;
                StreamShutdownQueued = false;
                deleteAfterEvent = ShouldDeletePreRelayLocked();
            }
            break;
        }
        default:
            break;
        }
        if (deleteAfterEvent) {
            delete this;
        }
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS OnReceive(MsQuicStream*, QUIC_STREAM_EVENT* event) noexcept {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            OpeningRx.insert(OpeningRx.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
        }

        if (Role == TqTunnelRole::ClientOpen) {
            TryCompleteClientOpen();
        } else {
            TryHandleServerOpen();
        }

        return QUIC_STATUS_SUCCESS;
    }

    void TryCompleteClientOpen() {
        if (OpeningRx.size() < TQ_OPEN_RESPONSE_SIZE) {
            return;
        }

        TqOpenResponse response{};
        const bool ok = TqDecodeOpenResponse(
            OpeningRx.data(),
            TQ_OPEN_RESPONSE_SIZE,
            response);
        if (!ok) {
            response = TqOpenResponse{false, TqOpenError::Internal, 0};
        }
        CompleteOpen(ok && response.Ok, response);
    }

    void TryHandleServerOpen() {
        if (ServerOpenDispatched.load(std::memory_order_acquire)) {
            return;
        }

        if (OpeningRx.size() < 10) {
            return;
        }

        const uint16_t addrLen =
            static_cast<uint16_t>((static_cast<uint16_t>(OpeningRx[8]) << 8) | OpeningRx[9]);
        const size_t expectedLen = 11 + addrLen;
        if (OpeningRx.size() < expectedLen) {
            return;
        }

        TqOpenRequest req{};
        if (!TqDecodeOpenRequest(OpeningRx.data(), expectedLen, req)) {
            ServerOpenDispatched.store(true, std::memory_order_release);
            SendOpenFailure(TqOpenError::Internal);
            return;
        }

        std::vector<sockaddr_storage> addrs;
        TqOpenError error = TqOpenError::Internal;
        if (Acl == nullptr || !TqResolveAllowedTarget(req, *Acl, addrs, error)) {
            ServerOpenDispatched.store(true, std::memory_order_release);
            if (error == TqOpenError::AclDenied && OnAclDenied) {
                OnAclDenied();
            }
            SendOpenFailure(error);
            return;
        }

        {
            std::lock_guard<std::mutex> guard(Lock);
            PendingServerOpen = true;
        }
        if (ServerOpenDispatched.exchange(true, std::memory_order_acq_rel)) {
            bool deleteAfterDuplicate = false;
            {
                std::lock_guard<std::mutex> guard(Lock);
                PendingServerOpen = false;
                deleteAfterDuplicate = ShouldDeletePreRelayLocked();
            }
            if (deleteAfterDuplicate) {
                delete this;
            }
            return;
        }

        std::thread([this, req, addrs = std::move(addrs)]() mutable {
            FinishServerOpenAfterDial(req, std::move(addrs));
            ServerOpenFinished();
        }).detach();
    }

    void FinishServerOpenAfterDial(
        TqOpenRequest req,
        std::vector<sockaddr_storage> addrs) {
        const TqDialResult dial = TqDialTcp(addrs, TqTcpDialTimeoutMs);
        if (!TqSocketValid(dial.Fd)) {
            SendOpenFailure(dial.Refused ? TqOpenError::TcpRefused : TqOpenError::TcpTimeout);
            return;
        }
        TcpFd = dial.Fd;

        if (IsShutdownComplete()) {
            CloseTcp();
            return;
        }

        std::vector<uint8_t> encoded;
        const uint32_t connId =
            QuicConn != nullptr ? TqLookupServerConnectionId(QuicConn) : 0;
        if (!TqEncodeOpenResponse(TqOpenResponse{true, TqOpenError::Ok, connId}, encoded) ||
            !SendBytes(encoded, QUIC_SEND_FLAG_NONE) ||
            !StartRelay(req.Flags)) {
            SendOpenFailure(TqOpenError::Internal);
        }
    }

    bool IsShutdownComplete() {
        std::lock_guard<std::mutex> guard(Lock);
        return ShutdownComplete;
    }

    void ServerOpenFinished() {
        bool deleteAfterFinish = false;
        {
            std::lock_guard<std::mutex> guard(Lock);
            PendingServerOpen = false;
            deleteAfterFinish = ShouldDeletePreRelayLocked();
        }
        if (deleteAfterFinish) {
            delete this;
        }
    }

    void SendOpenFailure(TqOpenError error) {
        CloseTcp();
        std::vector<uint8_t> encoded;
        if (!TqEncodeOpenResponse(TqOpenResponse{false, error, 0}, encoded) ||
            !SendBytes(encoded, QUIC_SEND_FLAG_FIN)) {
            ArmSelfDeleteOnShutdown();
            return;
        }
        // Graceful FIN so the client receives OPEN_FAIL before the stream ends.
        ShutdownReceiveAfterOpenFailure();
    }

    void ShutdownReceiveAfterOpenFailure() {
        std::lock_guard<std::mutex> guard(Lock);
        SelfDeleteOnShutdown = true;
        if (Stream != nullptr && !ShutdownComplete && !StreamShutdownQueued) {
            StreamShutdownQueued = true;
            (void)Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE);
        }
    }

    bool SendBytes(const std::vector<uint8_t>& data, QUIC_SEND_FLAGS flags) {
        auto* sendContext = TqTunnelSendContext::New(data.data(), data.size());
        if (sendContext == nullptr) {
            return false;
        }

        QUIC_STATUS status = QUIC_STATUS_INVALID_STATE;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (Stream == nullptr || ShutdownComplete || StreamShutdownQueued) {
                TqTunnelSendContext::Delete(sendContext);
                return false;
            }
            status = Stream->Send(&sendContext->Buffer, 1, flags, sendContext);
        }
        if (QUIC_FAILED(status)) {
            TqTunnelSendContext::Delete(sendContext);
            return false;
        }
        return true;
    }

    void CompleteOpen(bool ok, const TqOpenResponse& response) {
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (OpenDone) {
                return;
            }
            OpenOk = ok;
            OpenResponse = response;
            OpenDone = true;
        }
        StateChanged.notify_all();
    }

    TqTunnelRole Role;
    MsQuicStream* Stream;
    TqSocketHandle TcpFd{TqInvalidSocket};
    TqConfig Config;
    const TqAcl* Acl;
    MsQuicConnection* QuicConn;
    TqTunnelCompletionFn OnComplete;
    TqTunnelAclDeniedFn OnAclDenied;
    TqRelayHandle RelayHandle;
    std::unique_ptr<ITqCompressor> Compressor;
    std::unique_ptr<ITqDecompressor> Decompressor;
    std::mutex Lock;
    std::condition_variable StateChanged;
    std::vector<uint8_t> OpeningRx;
    TqOpenResponse OpenResponse{};
    bool OpenDone{false};
    bool OpenOk{false};
    bool RelayStarted{false};
    bool SelfDeleteOnShutdown{false};
    bool ShutdownComplete{false};
    bool StreamShutdownQueued{false};
    bool PendingServerOpen{false};
    std::atomic<bool> ServerOpenDispatched{false};
};

bool TqTunnelRelayStopped(const TqTunnelContext* ctx) {
    if (ctx == nullptr) {
        return true;
    }
    return ctx->RelayHandle.Stop.load();
}

void TqReapTunnelContext(TqTunnelContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    TqRelayStop(&ctx->RelayHandle);
    ctx->CloseTcp();
    delete ctx;
}

TqTunnelStartResult TqStartClientTunnel(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg) {
    if (!TqSocketValid(clientTcpFd)) {
        return {false, TqOpenError::Internal};
    }

    if (conn == nullptr || !conn->IsValid()) {
        TqCloseSocket(clientTcpFd);
        return {false, TqOpenError::Internal};
    }

    TqOpenRequest openReq{};
    if (!TqBuildOpenRequest(req, cfg, openReq)) {
        TqCloseSocket(clientTcpFd);
        return {false, TqOpenError::Internal};
    }

    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ClientOpen,
        nullptr,
        clientTcpFd,
        cfg,
        nullptr);
    if (context == nullptr) {
        TqCloseSocket(clientTcpFd);
        return {false, TqOpenError::Internal};
    }

    auto* stream = new (std::nothrow) MsQuicStream(
        *conn,
        QUIC_STREAM_OPEN_FLAG_NONE,
        CleanUpAutoDelete,
        TqTunnelContext::Callback,
        context);
    if (stream == nullptr || !stream->IsValid()) {
        delete stream;
        context->CloseTcp();
        delete context;
        return {false, TqOpenError::Internal};
    }

    context->SetStream(stream);
    if (!context->SendOpenRequest(openReq)) {
        context->CloseTcp();
        context->ArmSelfDeleteOnShutdown();
        return {false, TqOpenError::Internal};
    }

    TqOpenResponse response{};
    if (!context->WaitForOpenResponse(response)) {
        context->CloseTcp();
        context->ArmSelfDeleteOnShutdown();
        return {false, response.Error};
    }
    if (!response.Ok) {
        context->CloseTcp();
        context->ArmSelfDeleteOnShutdown();
        return {false, response.Error};
    }

    if (!context->StartRelay(openReq.Flags)) {
        context->CloseTcp();
        context->ArmSelfDeleteOnShutdown();
        return {false, TqOpenError::Internal};
    }

    return {true, TqOpenError::Ok};
}

void TqHandleServerPeerStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqTunnelCompletionFn onComplete,
    TqTunnelAclDeniedFn onAclDenied) {
    if (rawStream == nullptr) {
        return;
    }

    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ServerOpen,
        nullptr,
        TqInvalidSocket,
        cfg,
        &acl,
        conn,
        onComplete,
        std::move(onAclDenied));
    if (context == nullptr) {
        if (onComplete) {
            onComplete();
        }
        MsQuic->StreamClose(rawStream);
        return;
    }

    auto* stream = new (std::nothrow) MsQuicStream(
        rawStream,
        CleanUpAutoDelete,
        TqTunnelContext::Callback,
        context);
    if (stream == nullptr || !stream->IsValid()) {
        delete stream;
        delete context;
        MsQuic->StreamClose(rawStream);
        return;
    }

    context->SetStream(stream);
}
