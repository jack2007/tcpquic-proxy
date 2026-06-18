#include "tcp_tunnel.h"

#include "client_tunnel_open.h"
#include "compress.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "quic_session.h"
#include "relay.h"
#if defined(__linux__)
#include "server_dial_reactor.h"
#endif
#include "speed_test.h"
#include "tcp_dialer.h"
#include "trace.h"
#include "tunnel_registry.h"
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

std::atomic<TqServerDialReactor*> g_serverDialReactor{nullptr};

TqServerDialReactor* TqGetServerDialReactor() {
#if defined(__linux__)
    return g_serverDialReactor.load(std::memory_order_acquire);
#else
    return nullptr;
#endif
}

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
    return TqCompressAlgo::Zstd;
}

uint8_t TqFlagsFromConfig(const TunnelRequest& req, const TqConfig& cfg) {
    uint8_t flags = req.CompressFlags;
    flags &= static_cast<uint8_t>(~TQ_FLAG_COMPRESS);

    const char* compressMode = cfg.Compress.c_str();
    if (cfg.Compress == "auto") {
        compressMode = TqResolveAutoCompress(cfg);
    }

    if (std::strcmp(compressMode, "zstd") == 0) {
        flags |= TQ_FLAG_COMPRESS;
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

bool TqAppendLiteralTargetAddress(
    const TqOpenRequest& req,
    std::vector<sockaddr_storage>& addrs) {
    sockaddr_storage storage{};
    if (req.AddrType == TQ_ADDR_IPV4 && req.Addr.size() == 4) {
        auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
        addr->sin_family = AF_INET;
        addr->sin_port = htons(req.Port);
        std::memcpy(&addr->sin_addr, req.Addr.data(), 4);
        addrs.push_back(storage);
        return true;
    }

    if (req.AddrType == TQ_ADDR_IPV6 && req.Addr.size() == 16) {
        auto* addr = reinterpret_cast<sockaddr_in6*>(&storage);
        addr->sin6_family = AF_INET6;
        addr->sin6_port = htons(req.Port);
        std::memcpy(&addr->sin6_addr, req.Addr.data(), 16);
        addrs.push_back(storage);
        return true;
    }

    return false;
}

bool TqIsAllowedEphemeralLoopbackTarget(
    const TqOpenRequest& req,
    const std::string& host,
    const TqEphemeralTargetAuthorizer* authorizer) {
    if (authorizer == nullptr) {
        return false;
    }

    if (req.AddrType == TQ_ADDR_IPV4 && host == "127.0.0.1") {
        return authorizer->IsAllowedEphemeralTarget(host, req.Port);
    }

    if (req.AddrType == TQ_ADDR_IPV6 && host == "::1") {
        return authorizer->IsAllowedEphemeralTarget(host, req.Port);
    }

    return false;
}

bool TqResolveAllowedTarget(
    const TqOpenRequest& req,
    const TqAcl& acl,
    const TqEphemeralTargetAuthorizer* authorizer,
    std::vector<sockaddr_storage>& addrs,
    TqOpenError& error) {
    std::string host;
    if (!TqHostFromOpenRequest(req, host)) {
        error = TqOpenError::Internal;
        return false;
    }

    if (TqIsAllowedEphemeralLoopbackTarget(req, host, authorizer)) {
        if (!TqAppendLiteralTargetAddress(req, addrs)) {
            error = TqOpenError::Internal;
            return false;
        }
        return true;
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

struct TqClientTunnelOpenHandle final {
    enum class State {
        Opening,
        OpenSucceeded,
        OpenFailed,
        Accepted,
        Rejected,
        Cancelled,
    };

    std::mutex Lock;
    TqTunnelContext* Context{nullptr};
    TqClientTunnelOpenComplete OnComplete;
    TqTunnelStartResult Result{};
    State OpenState{State::Opening};
    std::atomic<unsigned> RefCount{1};
};

void TqRetainClientTunnelOpenHandle(TqClientTunnelOpenHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->RefCount.fetch_add(1, std::memory_order_relaxed);
}

void TqReleaseClientTunnelOpenHandle(TqClientTunnelOpenHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    if (handle->RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete handle;
    }
}

void TqNotifyClientTunnelOpenComplete(
    TqClientTunnelOpenHandle* handle,
    TqTunnelStartResult result) {
    if (handle == nullptr) {
        return;
    }

    TqClientTunnelOpenComplete onComplete;
    {
        std::lock_guard<std::mutex> guard(handle->Lock);
        if (handle->OpenState != TqClientTunnelOpenHandle::State::Opening) {
            return;
        }
        handle->OpenState = result.Ok
            ? TqClientTunnelOpenHandle::State::OpenSucceeded
            : TqClientTunnelOpenHandle::State::OpenFailed;
        handle->Result = result;
        onComplete = std::move(handle->OnComplete);
    }

    if (onComplete) {
        onComplete(handle, result);
    }
}

struct TqTunnelContext final {
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
        const TqEphemeralTargetAuthorizer* authorizer,
        MsQuicConnection* quicConn = nullptr,
        bool receiveSink = false,
        std::atomic<uint64_t>* receiveSinkBytes = nullptr,
        TqTunnelCompletionFn onComplete = {},
        TqTunnelAclDeniedFn onAclDenied = {}) :
        Role(role),
        Stream(stream),
        TcpFd(tcpFd),
        Config(cfg),
        Acl(acl),
        Authorizer(authorizer),
        QuicConn(quicConn),
        ReceiveSink(receiveSink),
        ReceiveSinkBytes(receiveSinkBytes),
        OnComplete(std::move(onComplete)),
        OnAclDenied(std::move(onAclDenied)) {
        TraceIngressProto = 0;
    }

    ~TqTunnelContext() {
        (void)CancelServerDialAndMaybeDelete();
        UnregisterFromConnection();
        EmitTraceClosed();
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

    void SetAsyncClientOpenHandle(TqClientTunnelOpenHandle* handle, uint8_t flags) {
        std::lock_guard<std::mutex> guard(Lock);
        AsyncClientOpenHandle = handle;
        AsyncClientOpenFlags = flags;
        TqRetainClientTunnelOpenHandle(handle);
    }

    bool SendOpenRequest(const TqOpenRequest& req) {
        std::vector<uint8_t> encoded;
        if (!TqEncodeOpenRequest(req, encoded)) {
            return false;
        }
        if (TqTraceEnabled()) {
            uint32_t connId = 0;
            if (QuicConn != nullptr) {
                connId = Role == TqTunnelRole::ClientOpen
                    ? TqLookupClientTraceConnId(QuicConn)
                    : TqLookupServerConnectionId(QuicConn);
            }
            TqTraceIncOpenTx(connId);
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

        MsQuicStream* stream = nullptr;
        TqSocketHandle tcpFd = TqInvalidSocket;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (Stream == nullptr || ShutdownComplete || StreamShutdownQueued || RelayStarted) {
                return false;
            }
            stream = Stream;
            tcpFd = TcpFd;
        }

        const bool relayStarted = ReceiveSink
            ? TqRelayStartQuicReceiveSink(stream, &RelayHandle, Config.Tuning, ReceiveSinkBytes)
            : TqRelayStart(
                  tcpFd,
                  stream,
                  Compressor.get(),
                  Decompressor.get(),
                  &RelayHandle,
                  Config.Tuning,
                  algo);
        if (!relayStarted) {
            return false;
        }

        {
            std::lock_guard<std::mutex> guard(Lock);
            if (Stream == nullptr || ShutdownComplete || StreamShutdownQueued) {
                TqRelayStop(&RelayHandle);
                return false;
            }
            RelayStarted = true;
        }
        TqTunnelReaper::Instance().Register(this);
        StateChanged.notify_all();
        if (TraceTunnelId != 0) {
            TqTraceRelayStarted(TraceTunnelId);
            TraceRelayStarted = true;
        }
        return true;
    }

    void ArmSelfDeleteOnShutdown() {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            SelfDeleteOnShutdown = true;
            if (!RelayStarted && Stream != nullptr && Stream->Handle != nullptr &&
                !ShutdownComplete && !StreamShutdownQueued) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
        }
        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
    }

    void CloseTcp() {
        (void)CancelServerDialAndMaybeDelete();
        std::lock_guard<std::mutex> guard(Lock);
        CloseTcpLocked();
    }

    bool CancelServerDialAndMaybeDelete() {
#if defined(__linux__)
        TqServerDialReactor* reactor = nullptr;
        uint64_t token = 0;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (!ServerDialPendingWithReactor && !ServerDialCallbackActive) {
                return ShouldDeletePreRelayLocked();
            }
            reactor = ServerDialReactor;
            token = ServerDialToken;
        }
        if (reactor != nullptr && token != 0) {
            reactor->Cancel(token);
        }

        std::lock_guard<std::mutex> guard(Lock);
        if (ServerDialPendingWithReactor && ServerDialReactor == reactor &&
            ServerDialToken == token) {
            ServerDialReactor = nullptr;
            ServerDialToken = 0;
            ServerDialPendingWithReactor = false;
            if (!ServerDialCallbackActive) {
                PendingServerOpen = false;
            }
        }
        return ShouldDeletePreRelayLocked();
#else
        return false;
#endif
    }

    void TraceRelayStopping(const char* reason) {
        if (TraceTunnelId == 0) {
            return;
        }
        const char* backend = "none";
        uint64_t relayId = 0;
        if (RelayHandle.Backend == TqRelayBackendType::LinuxWorker) {
            backend = "linux";
            relayId = RelayHandle.LinuxRelayId;
        } else if (RelayHandle.Backend == TqRelayBackendType::WindowsWorker) {
            backend = "windows";
            relayId = RelayHandle.WindowsRelayId;
        }
        TqTraceRelayStopping(
            TraceTunnelId,
            Role == TqTunnelRole::ClientOpen ? "client" : "server",
            TraceTarget.c_str(),
            backend,
            relayId,
            reason);
    }

    void CloseTcpLocked() {
        if (TqSocketValid(TcpFd)) {
            if (Role == TqTunnelRole::ClientOpen && TraceIngressProto != 0) {
                const TqTraceProxyProto proto =
                    TraceIngressProto == 2 ? TqTraceProxyProto::Http : TqTraceProxyProto::Socks;
                TqTraceProxyClosed(proto, TcpFd);
                TraceIngressProto = 0;
            }
            if (TraceTunnelId != 0 && Role == TqTunnelRole::ServerOpen && TraceTargetTcpOpen) {
                TqTraceTargetTcpClosed(TraceTunnelId);
                TraceTargetTcpOpen = false;
            }
            TqCloseSocket(TcpFd);
            TcpFd = TqInvalidSocket;
        }
    }

    void ReleaseTcpWithoutClose() {
        std::lock_guard<std::mutex> guard(Lock);
        TcpFd = TqInvalidSocket;
    }

    bool ReleaseClientOpenOwnerAndMaybeDelete() {
        std::lock_guard<std::mutex> guard(Lock);
        if (Role == TqTunnelRole::ClientOpen) {
            ClientOpenOwnerActive = false;
        }
        return ShouldDeletePreRelayLocked();
    }

    bool CancelAsyncClientOpen(TqClientTunnelOpenHandle* handle) {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        bool deleteAfterCancel = false;
        TqClientTunnelOpenHandle* releaseHandle = nullptr;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (AsyncClientOpenHandle != handle) {
                return false;
            }
            releaseHandle = AsyncClientOpenHandle;
            AsyncClientOpenHandle = nullptr;
            ClientOpenOwnerActive = false;
            SelfDeleteOnShutdown = true;
            CloseTcpLocked();
            if (Stream != nullptr && Stream->Handle != nullptr &&
                !ShutdownComplete && !StreamShutdownQueued) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
            deleteAfterCancel = ShouldDeletePreRelayLocked();
        }
        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
        TqReleaseClientTunnelOpenHandle(releaseHandle);
        return deleteAfterCancel;
    }

    bool AbandonAsyncClientOpenWithoutClosingTcp(TqClientTunnelOpenHandle* handle) {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        bool deleteAfterAbandon = false;
        TqClientTunnelOpenHandle* releaseHandle = nullptr;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (AsyncClientOpenHandle != handle) {
                return false;
            }
            releaseHandle = AsyncClientOpenHandle;
            AsyncClientOpenHandle = nullptr;
            ClientOpenOwnerActive = false;
            SelfDeleteOnShutdown = true;
            TcpFd = TqInvalidSocket;
            if (Stream != nullptr && Stream->Handle != nullptr &&
                !ShutdownComplete && !StreamShutdownQueued) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
            deleteAfterAbandon = ShouldDeletePreRelayLocked();
        }
        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
        TqReleaseClientTunnelOpenHandle(releaseHandle);
        return deleteAfterAbandon;
    }

    bool AcceptAsyncClientOpen(TqClientTunnelOpenHandle* handle) {
        TqClientTunnelOpenHandle* releaseHandle = nullptr;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (AsyncClientOpenHandle != handle) {
                return false;
            }
            releaseHandle = AsyncClientOpenHandle;
            AsyncClientOpenHandle = nullptr;
            AsyncClientOpenAccepted = true;
            SelfDeleteOnShutdown = true;
            PendingClientRelay = true;
            PendingClientRelayFlags = AsyncClientOpenFlags;
        }
        TqReleaseClientTunnelOpenHandle(releaseHandle);
        if (StartPendingClientRelay()) {
            if (ReleaseClientOpenOwnerAndMaybeDelete()) {
                delete this;
            }
            return true;
        }
        if (IsPendingClientRelayWaitingForOpenSendComplete()) {
            if (ReleaseClientOpenOwnerAndMaybeDelete()) {
                delete this;
            }
            return true;
        }
        if (AbortAcceptedClientOpenBeforeRelay()) {
            delete this;
        }
        return false;
    }

    bool RejectAsyncClientOpen(TqClientTunnelOpenHandle* handle) {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        bool deleteAfterReject = false;
        TqClientTunnelOpenHandle* releaseHandle = nullptr;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (AsyncClientOpenHandle != handle) {
                return false;
            }
            releaseHandle = AsyncClientOpenHandle;
            AsyncClientOpenHandle = nullptr;
            ClientOpenOwnerActive = false;
            SelfDeleteOnShutdown = true;
            CloseTcpLocked();
            if (Stream != nullptr && Stream->Handle != nullptr &&
                !ShutdownComplete && !StreamShutdownQueued) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
            deleteAfterReject = ShouldDeletePreRelayLocked();
        }
        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
        TqReleaseClientTunnelOpenHandle(releaseHandle);
        if (deleteAfterReject) {
            delete this;
        }
        return true;
    }

    bool IsPendingClientRelayWaitingForOpenSendComplete() {
        std::lock_guard<std::mutex> guard(Lock);
        return AsyncClientOpenAccepted && PendingClientRelay && !OpenSendComplete &&
            !RelayStarted && !ShutdownComplete && !StreamShutdownQueued;
    }

    bool AbortAcceptedClientOpenBeforeRelay() {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        bool deleteAfterAbort = false;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (!AsyncClientOpenAccepted || RelayStarted || ShutdownComplete ||
                StreamShutdownQueued) {
                return false;
            }
            PendingClientRelay = false;
            ClientOpenOwnerActive = false;
            SelfDeleteOnShutdown = true;
            CloseTcpLocked();
            if (Stream != nullptr && Stream->Handle != nullptr) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
            deleteAfterAbort = ShouldDeletePreRelayLocked();
        }
        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
        return deleteAfterAbort;
    }

    bool FinishClientOpenAndStartRelay(uint8_t flags) {
        {
            std::lock_guard<std::mutex> guard(Lock);
            PendingClientRelay = true;
            PendingClientRelayFlags = flags;
        }
        if (StartPendingClientRelay()) {
            return true;
        }

        std::unique_lock<std::mutex> guard(Lock);
        if (!StateChanged.wait_for(
                guard,
                TqOpenTimeout,
                [this] { return RelayStarted || ShutdownComplete; })) {
            return false;
        }
        return RelayStarted;
    }

    void AssignTrace(uint64_t tunnelId, std::string target) {
        TraceTunnelId = tunnelId;
        TraceTarget = std::move(target);
    }

    void SetIngressTraceProto(uint8_t proto) {
        TraceIngressProto = proto;
    }

    void TakeOpeningBytes(std::vector<uint8_t>&& openingRx) {
        OpeningRx = std::move(openingRx);
    }

    void ResumeServerOpenFromBufferedData() {
        TryHandleServerOpen();
    }

#if defined(TCPQUIC_TUNNEL_TESTING) && defined(__linux__)
    void TestMarkLegacyServerOpenPending() {
        std::lock_guard<std::mutex> guard(Lock);
        PendingServerOpen = true;
        ServerDialReactor = nullptr;
        ServerDialToken = 0;
        ServerDialCallbackActive = false;
        ServerDialPendingWithReactor = false;
    }

    bool TestCancelServerDialAndHasPending() {
        (void)CancelServerDialAndMaybeDelete();
        std::lock_guard<std::mutex> guard(Lock);
        return PendingServerOpen;
    }
#endif

    void RegisterWithConnectionIfNeeded() {
        if (QuicConn != nullptr && !RegisteredWithConnection) {
            TqRegisterConnectionTunnel(QuicConn, this, &TqTunnelContext::AbortFromRegistry);
            RegisteredWithConnection = true;
        }
    }

    void UnregisterFromConnection() {
        if (QuicConn != nullptr && RegisteredWithConnection) {
            TqUnregisterConnectionTunnel(QuicConn, this);
            RegisteredWithConnection = false;
        }
    }

    static void AbortFromRegistry(void* context) {
        static_cast<TqTunnelContext*>(context)->AbortForConnectionShutdown();
    }

    void AbortForConnectionShutdown() {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        bool stopRelay = false;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            AbortedByConnection = true;
            SelfDeleteOnShutdown = true;
            stopRelay = RelayStarted;
            if (!RelayStarted && Stream != nullptr && Stream->Handle != nullptr &&
                !ShutdownComplete && !StreamShutdownQueued) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
            CloseTcpLocked();
        }
        (void)CancelServerDialAndMaybeDelete();
        StateChanged.notify_all();

        if (stopRelay) {
            if (shutdownStream) {
                (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
            }
            TraceRelayStopping("connection_shutdown");
            TqRelayStop(&RelayHandle);
            return;
        }

        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
    }

private:
    bool ShouldDeletePreRelayLocked() const {
        return SelfDeleteOnShutdown &&
            !RelayStarted &&
            !ClientOpenOwnerActive &&
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
            if (Role == TqTunnelRole::ServerOpen) {
                bool shutdownAfterFailure = false;
                {
                    std::lock_guard<std::mutex> guard(Lock);
                    if (PendingOpenFailureShutdown) {
                        PendingOpenFailureShutdown = false;
                        shutdownAfterFailure = true;
                    }
                }
                if (shutdownAfterFailure) {
                    ShutdownReceiveAfterOpenFailure();
                } else {
                    StartPendingServerRelay();
                }
            } else if (Role == TqTunnelRole::ClientOpen) {
                {
                    std::lock_guard<std::mutex> guard(Lock);
                    OpenSendComplete = true;
                }
                if (!StartPendingClientRelay()) {
                    deleteAfterEvent = AbortAcceptedClientOpenBeforeRelay();
                }
            }
            break;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            (void)CancelServerDialAndMaybeDelete();
            if (Role == TqTunnelRole::ClientOpen) {
                TryCompleteClientOpen();
            }
            CompleteOpen(false, TqOpenResponse{false, TqOpenError::Internal, 0});
            break;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
            (void)CancelServerDialAndMaybeDelete();
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

#if defined(__linux__)
    bool BeginServerDialCallback(
        TqServerDialReactor* reactor,
        const TqServerDialResult& result) {
        std::lock_guard<std::mutex> guard(Lock);
        if (!PendingServerOpen || ServerDialReactor != reactor) {
            if (TqSocketValid(result.Fd)) {
                TqCloseSocket(result.Fd);
            }
            return false;
        }
        ServerDialCallbackActive = true;
        ServerDialToken = 0;
        ServerDialPendingWithReactor = true;
        return true;
    }

    void TryHandleServerOpenWithReactor(
        const TqOpenRequest& req,
        const std::string& host,
        TqServerDialReactor* reactor) {
        {
            std::lock_guard<std::mutex> guard(Lock);
            PendingServerOpen = true;
            ServerDialReactor = reactor;
            ServerDialToken = 0;
            ServerDialCallbackActive = false;
            ServerDialPendingWithReactor = true;
        }
        if (ServerOpenDispatched.exchange(true, std::memory_order_acq_rel)) {
            bool deleteAfterDuplicate = false;
            {
                std::lock_guard<std::mutex> guard(Lock);
                PendingServerOpen = false;
                ServerDialReactor = nullptr;
                ServerDialToken = 0;
                ServerDialPendingWithReactor = false;
                deleteAfterDuplicate = ShouldDeletePreRelayLocked();
            }
            if (deleteAfterDuplicate) {
                delete this;
            }
            return;
        }

        if (TraceTunnelId != 0) {
            TqTraceTargetTcpDialing(TraceTunnelId, TraceTarget.c_str());
        }

        TqServerDialRequest dialRequest;
        dialRequest.Host = host;
        dialRequest.Port = req.Port;
        dialRequest.TraceTunnelId = TraceTunnelId;
        dialRequest.BypassAclForAuthorizedLoopback =
            TqIsAllowedEphemeralLoopbackTarget(req, host, Authorizer);
        dialRequest.Complete =
            [this, reactor, req](const TqServerDialResult& result) {
                if (!BeginServerDialCallback(reactor, result)) {
                    return;
                }
                FinishServerOpenAfterReactorDial(req, result);
                ServerOpenFinished();
            };

        const uint64_t token = reactor->Submit(std::move(dialRequest));
        if (token == 0) {
            bool sendFailure = false;
            bool deleteAfterSubmitFailure = false;
            {
                std::lock_guard<std::mutex> guard(Lock);
                if (PendingServerOpen && ServerDialReactor == reactor &&
                    !ServerDialCallbackActive) {
                    PendingServerOpen = false;
                    ServerDialReactor = nullptr;
                    ServerDialToken = 0;
                    ServerDialPendingWithReactor = false;
                    sendFailure = !AbortedByConnection && !ShutdownComplete &&
                        !StreamShutdownQueued;
                    deleteAfterSubmitFailure = ShouldDeletePreRelayLocked();
                }
            }
            if (sendFailure) {
                SendOpenFailure(TqOpenError::Internal);
            }
            if (deleteAfterSubmitFailure) {
                delete this;
            }
            return;
        }

        bool cancelReturnedToken = false;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (PendingServerOpen && ServerDialReactor == reactor &&
                !ServerDialCallbackActive) {
                ServerDialToken = token;
            } else {
                cancelReturnedToken = true;
            }
        }
        if (cancelReturnedToken) {
            reactor->Cancel(token);
        }
    }
#endif

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

        std::string host;
        if (!TqHostFromOpenRequest(req, host)) {
            ServerOpenDispatched.store(true, std::memory_order_release);
            SendOpenFailure(TqOpenError::Internal);
            return;
        }
        TraceTarget = host + ":" + std::to_string(req.Port);
        if (TraceTunnelId == 0) {
            const uint32_t connId = QuicConn != nullptr ? TqLookupServerConnectionId(QuicConn) : 0;
            TraceTunnelId = TqTraceStreamStarted(QuicConn, connId, "server", TraceTarget.c_str(), req.Flags);
            TqTraceIncOpenRx(connId);
        }

#if defined(__linux__)
        if (TqServerDialReactor* reactor = TqGetServerDialReactor()) {
            TryHandleServerOpenWithReactor(req, host, reactor);
            return;
        }
#endif

        std::vector<sockaddr_storage> addrs;
        TqOpenError error = TqOpenError::Internal;
        if (Acl == nullptr || !TqResolveAllowedTarget(req, *Acl, Authorizer, addrs, error)) {
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

#if defined(__linux__)
    void FinishServerOpenAfterReactorDial(
        TqOpenRequest req,
        const TqServerDialResult& result) {
        if (!result.Done || result.Error != TqOpenError::Ok ||
            !TqSocketValid(result.Fd)) {
            const TqOpenError error = result.Done ? result.Error : TqOpenError::Internal;
            if (TraceTunnelId != 0) {
                TqTraceTargetTcpFailed(TraceTunnelId, error);
            }
            if (error == TqOpenError::AclDenied && OnAclDenied) {
                OnAclDenied();
            }
            SendOpenFailure(error);
            return;
        }

        {
            std::lock_guard<std::mutex> guard(Lock);
            TcpFd = result.Fd;
            if (AbortedByConnection || ShutdownComplete) {
                CloseTcpLocked();
                return;
            }
            if (TraceTunnelId != 0) {
                TraceTargetTcpOpen = true;
                TqTraceTargetTcpConnected(TraceTunnelId, TcpFd);
            }
        }

        if (IsAbortedOrShutdown()) {
            CloseTcp();
            return;
        }

        std::vector<uint8_t> encoded;
        const uint32_t connId =
            QuicConn != nullptr ? TqLookupServerConnectionId(QuicConn) : 0;
        if (IsAbortedOrShutdown()) {
            CloseTcp();
            return;
        }
        const bool encodedOk = TqEncodeOpenResponse(
            TqOpenResponse{true, TqOpenError::Ok, connId},
            encoded);
        const bool sendOk = encodedOk && SendBytes(encoded, QUIC_SEND_FLAG_NONE);
        if (!sendOk) {
            if (!IsAbortedOrShutdown()) {
                SendOpenFailure(TqOpenError::Internal);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (AbortedByConnection || ShutdownComplete || StreamShutdownQueued) {
                return;
            }
            PendingServerRelay = true;
            PendingServerRelayFlags = req.Flags;
        }
    }
#endif

    void FinishServerOpenAfterDial(
        TqOpenRequest req,
        std::vector<sockaddr_storage> addrs) {
        if (TraceTunnelId != 0) {
            std::string host;
            if (TqHostFromOpenRequest(req, host)) {
                TraceTarget = host + ":" + std::to_string(req.Port);
            }
            TqTraceTargetTcpDialing(TraceTunnelId, TraceTarget.c_str());
        }

        const TqDialResult dial = TqDialTcp(addrs, TqTcpDialTimeoutMs);
        if (!TqSocketValid(dial.Fd)) {
            const TqOpenError err = dial.Refused ? TqOpenError::TcpRefused : TqOpenError::TcpTimeout;
            if (TraceTunnelId != 0) {
                TqTraceTargetTcpFailed(TraceTunnelId, err);
            }
            SendOpenFailure(err);
            return;
        }
        {
            std::lock_guard<std::mutex> guard(Lock);
            TcpFd = dial.Fd;
            if (AbortedByConnection || ShutdownComplete) {
                CloseTcpLocked();
                return;
            }
            if (TraceTunnelId != 0) {
                TraceTargetTcpOpen = true;
                TqTraceTargetTcpConnected(TraceTunnelId, TcpFd);
            }
        }

        if (IsAbortedOrShutdown()) {
            CloseTcp();
            return;
        }

        std::vector<uint8_t> encoded;
        const uint32_t connId =
            QuicConn != nullptr ? TqLookupServerConnectionId(QuicConn) : 0;
        if (IsAbortedOrShutdown()) {
            CloseTcp();
            return;
        }
        const bool encodedOk = TqEncodeOpenResponse(TqOpenResponse{true, TqOpenError::Ok, connId}, encoded);
        const bool sendOk = encodedOk && SendBytes(encoded, QUIC_SEND_FLAG_NONE);
        if (!sendOk) {
            if (!IsAbortedOrShutdown()) {
                SendOpenFailure(TqOpenError::Internal);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (AbortedByConnection || ShutdownComplete || StreamShutdownQueued) {
                return;
            }
            PendingServerRelay = true;
            PendingServerRelayFlags = req.Flags;
        }
    }

    bool IsAbortedOrShutdown() {
        std::lock_guard<std::mutex> guard(Lock);
        return AbortedByConnection || ShutdownComplete || StreamShutdownQueued;
    }

    void ServerOpenFinished() {
        bool deleteAfterFinish = false;
        {
            std::lock_guard<std::mutex> guard(Lock);
            PendingServerOpen = false;
            ServerDialToken = 0;
            ServerDialReactor = nullptr;
            ServerDialCallbackActive = false;
            ServerDialPendingWithReactor = false;
            deleteAfterFinish = ShouldDeletePreRelayLocked();
        }
        if (deleteAfterFinish) {
            delete this;
        }
    }

    void SendOpenFailure(TqOpenError error) {
        CloseTcp();
        if (IsAbortedOrShutdown()) {
            ArmSelfDeleteOnShutdown();
            return;
        }
        std::vector<uint8_t> encoded;
        if (!TqEncodeOpenResponse(TqOpenResponse{false, error, 0}, encoded) ||
            !SendBytes(encoded, QUIC_SEND_FLAG_FIN)) {
            ArmSelfDeleteOnShutdown();
            return;
        }
        {
            std::lock_guard<std::mutex> guard(Lock);
            PendingOpenFailureShutdown = true;
        }
    }

    void ShutdownReceiveAfterOpenFailure() {
        MsQuicStream* stream = nullptr;
        bool shutdownStream = false;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            SelfDeleteOnShutdown = true;
            if (Stream != nullptr && Stream->Handle != nullptr &&
                !ShutdownComplete && !StreamShutdownQueued) {
                stream = Stream;
                shutdownStream = true;
                StreamShutdownQueued = true;
            }
        }
        if (shutdownStream) {
            (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE);
        }
    }

    bool SendBytes(const std::vector<uint8_t>& data, QUIC_SEND_FLAGS flags) {
        auto* sendContext = TqTunnelSendContext::New(data.data(), data.size());
        if (sendContext == nullptr) {
            return false;
        }

        MsQuicStream* stream = nullptr;
        auto streamOpLock = StreamOpLock;
        std::lock_guard<std::mutex> streamGuard(*streamOpLock);
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (Stream == nullptr || ShutdownComplete || StreamShutdownQueued) {
                TqTunnelSendContext::Delete(sendContext);
                return false;
            }
            stream = Stream;
        }

        const QUIC_STATUS status =
            stream->Send(&sendContext->Buffer, 1, flags, sendContext);
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
        if (TraceTunnelId != 0) {
            TqTraceOpenResult(TraceTunnelId, ok && response.Ok, response.Error, response.ConnId);
        }
        StateChanged.notify_all();
        CompleteAsyncClientOpenIfNeeded(ok, response);
    }

    void CompleteAsyncClientOpenIfNeeded(bool ok, const TqOpenResponse& response) {
        TqClientTunnelOpenHandle* handle = nullptr;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (Role != TqTunnelRole::ClientOpen || AsyncClientOpenHandle == nullptr) {
                return;
            }
            handle = AsyncClientOpenHandle;
            TqRetainClientTunnelOpenHandle(handle);
        }

        const TqTunnelStartResult result{
            ok && response.Ok,
            response.Error,
            TraceTunnelId,
        };
        TqNotifyClientTunnelOpenComplete(handle, result);
        TqReleaseClientTunnelOpenHandle(handle);
    }

    void EmitTraceClosed() {
        if (TraceClosedEmitted || TraceTunnelId == 0) {
            return;
        }
        TraceClosedEmitted = true;
        const char* role = Role == TqTunnelRole::ClientOpen ? "client" : "server";
        TqOpenError reason = TqOpenError::Ok;
        if (!OpenDone) {
            reason = TqOpenError::Internal;
        } else if (!OpenOk) {
            reason = OpenResponse.Error;
        }
        TqTraceStreamClosed(
            TraceTunnelId,
            role,
            TraceTarget.c_str(),
            TraceRelayStarted,
            reason);
        if (TraceTargetTcpOpen) {
            TqTraceTargetTcpClosed(TraceTunnelId);
            TraceTargetTcpOpen = false;
        }
    }

    void StartPendingServerRelay() {
        uint8_t flags = 0;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (!PendingServerRelay || RelayStarted || ShutdownComplete || StreamShutdownQueued) {
                return;
            }
            flags = PendingServerRelayFlags;
            PendingServerRelay = false;
        }
        const bool relayOk = StartRelay(flags);
        if (!relayOk) {
            SendOpenFailure(TqOpenError::Internal);
        }
    }

    bool StartPendingClientRelay() {
        uint8_t flags = 0;
        {
            std::lock_guard<std::mutex> guard(Lock);
            if (!PendingClientRelay || !OpenSendComplete || RelayStarted ||
                ShutdownComplete || StreamShutdownQueued) {
                return RelayStarted;
            }
            flags = PendingClientRelayFlags;
            PendingClientRelay = false;
        }
        const bool relayOk = StartRelay(flags);
        if (relayOk) {
            StateChanged.notify_all();
        }
        return relayOk;
    }

    TqTunnelRole Role;
    MsQuicStream* Stream;
    TqSocketHandle TcpFd{TqInvalidSocket};
    TqConfig Config;
    const TqAcl* Acl;
    const TqEphemeralTargetAuthorizer* Authorizer;
    MsQuicConnection* QuicConn;
    bool ReceiveSink{false};
    std::atomic<uint64_t>* ReceiveSinkBytes{nullptr};
    TqTunnelCompletionFn OnComplete;
    TqTunnelAclDeniedFn OnAclDenied;
    TqRelayHandle RelayHandle;
    std::unique_ptr<ITqCompressor> Compressor;
    std::unique_ptr<ITqDecompressor> Decompressor;
    // Serializes MsQuic stream Send/Shutdown after state checks; take before Lock.
    std::shared_ptr<std::mutex> StreamOpLock{std::make_shared<std::mutex>()};
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
    bool PendingServerRelay{false};
    uint8_t PendingServerRelayFlags{0};
#if defined(__linux__)
    TqServerDialReactor* ServerDialReactor{nullptr};
    uint64_t ServerDialToken{0};
    bool ServerDialCallbackActive{false};
    bool ServerDialPendingWithReactor{false};
#endif
    bool PendingClientRelay{false};
    uint8_t PendingClientRelayFlags{0};
    bool OpenSendComplete{false};
    bool PendingOpenFailureShutdown{false};
    bool RegisteredWithConnection{false};
    bool AbortedByConnection{false};
    bool ClientOpenOwnerActive{Role == TqTunnelRole::ClientOpen};
    TqClientTunnelOpenHandle* AsyncClientOpenHandle{nullptr};
    uint8_t AsyncClientOpenFlags{0};
    bool AsyncClientOpenAccepted{false};
    std::atomic<bool> ServerOpenDispatched{false};
    uint64_t TraceTunnelId{0};
    std::string TraceTarget;
    bool TraceRelayStarted{false};
    bool TraceTargetTcpOpen{false};
    bool TraceClosedEmitted{false};
    uint8_t TraceIngressProto{0};
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
    ctx->TraceRelayStopping("reaper");
    TqRelayStop(&ctx->RelayHandle);
    ctx->CloseTcp();
    delete ctx;
}

namespace {

TqTunnelStartResult TqStartClientTunnelInternal(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    bool receiveSink,
    std::atomic<uint64_t>* receiveSinkBytes) {
    if (!receiveSink && !TqSocketValid(clientTcpFd)) {
        return {false, TqOpenError::Internal};
    }

    if (conn == nullptr || !conn->IsValid()) {
        return {false, TqOpenError::Internal};
    }

    TqOpenRequest openReq{};
    if (!TqBuildOpenRequest(req, cfg, openReq)) {
        return {false, TqOpenError::Internal};
    }

    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ClientOpen,
        nullptr,
        clientTcpFd,
        cfg,
        nullptr,
        nullptr,
        conn,
        receiveSink,
        receiveSinkBytes);
    if (context == nullptr) {
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
        delete context;
        return {false, TqOpenError::Internal};
    }

    context->SetStream(stream);
    context->RegisterWithConnectionIfNeeded();
    auto releaseClientOpenOwner = [context]() {
        if (context->ReleaseClientOpenOwnerAndMaybeDelete()) {
            delete context;
        }
    };

    const std::string target = std::string(req.Host) + ":" + std::to_string(req.Port);
    const uint32_t connId = TqLookupClientTraceConnId(conn);
    const uint64_t tunnelId = TqTraceStreamStarted(conn, connId, "client", target.c_str(), openReq.Flags);
    context->AssignTrace(tunnelId, target);
    context->SetIngressTraceProto(req.IngressTraceProto);

    if (!context->SendOpenRequest(openReq)) {
        std::fprintf(stderr, "tcpquic-proxy: client tunnel failed to send open request\n");
        context->ReleaseTcpWithoutClose();
        context->ArmSelfDeleteOnShutdown();
        releaseClientOpenOwner();
        return {false, TqOpenError::Internal, tunnelId};
    }

    TqOpenResponse response{};
    if (!context->WaitForOpenResponse(response)) {
        std::fprintf(stderr,
            "tcpquic-proxy: client tunnel open response failed (error=%u)\n",
            static_cast<unsigned>(response.Error));
        context->ReleaseTcpWithoutClose();
        context->ArmSelfDeleteOnShutdown();
        releaseClientOpenOwner();
        return {false, response.Error, tunnelId};
    }
    if (!response.Ok) {
        context->ReleaseTcpWithoutClose();
        context->ArmSelfDeleteOnShutdown();
        releaseClientOpenOwner();
        return {false, response.Error, tunnelId};
    }

    if (!context->FinishClientOpenAndStartRelay(openReq.Flags)) {
        context->ReleaseTcpWithoutClose();
        context->ArmSelfDeleteOnShutdown();
        releaseClientOpenOwner();
        return {false, TqOpenError::Internal, tunnelId};
    }

    releaseClientOpenOwner();
    return {true, TqOpenError::Ok, tunnelId};
}

} // namespace

void TqSetServerDialReactor(TqServerDialReactor* reactor) {
    g_serverDialReactor.store(reactor, std::memory_order_release);
}

TqTunnelStartResult TqStartClientTunnel(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg) {
    return TqStartClientTunnelInternal(conn, req, clientTcpFd, cfg, false, nullptr);
}

TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete) {
    if (!TqSocketValid(clientTcpFd)) {
        return nullptr;
    }

    if (conn == nullptr || !conn->IsValid()) {
        return nullptr;
    }

    TqOpenRequest openReq{};
    if (!TqBuildOpenRequest(req, cfg, openReq)) {
        return nullptr;
    }

    auto* handle = new (std::nothrow) TqClientTunnelOpenHandle();
    if (handle == nullptr) {
        return nullptr;
    }
    handle->OnComplete = std::move(onComplete);

    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ClientOpen,
        nullptr,
        clientTcpFd,
        cfg,
        nullptr,
        nullptr,
        conn,
        false,
        nullptr);
    if (context == nullptr) {
        delete handle;
        return nullptr;
    }

    auto* stream = new (std::nothrow) MsQuicStream(
        *conn,
        QUIC_STREAM_OPEN_FLAG_NONE,
        CleanUpAutoDelete,
        TqTunnelContext::Callback,
        context);
    if (stream == nullptr || !stream->IsValid()) {
        delete stream;
        delete context;
        delete handle;
        return nullptr;
    }

    context->SetStream(stream);
    context->RegisterWithConnectionIfNeeded();

    const std::string target = std::string(req.Host) + ":" + std::to_string(req.Port);
    const uint32_t connId = TqLookupClientTraceConnId(conn);
    const uint64_t tunnelId =
        TqTraceStreamStarted(conn, connId, "client", target.c_str(), openReq.Flags);
    context->AssignTrace(tunnelId, target);
    context->SetIngressTraceProto(req.IngressTraceProto);

    {
        std::lock_guard<std::mutex> guard(handle->Lock);
        handle->Context = context;
    }
    context->SetAsyncClientOpenHandle(handle, openReq.Flags);

    if (!context->SendOpenRequest(openReq)) {
        std::fprintf(stderr, "tcpquic-proxy: async client tunnel failed to send open request\n");
        bool deleteContext = false;
        {
            std::lock_guard<std::mutex> guard(handle->Lock);
            handle->OpenState = TqClientTunnelOpenHandle::State::Cancelled;
            handle->Context = nullptr;
        }
        if (context->AbandonAsyncClientOpenWithoutClosingTcp(handle)) {
            deleteContext = true;
        }
        TqReleaseClientTunnelOpenHandle(handle);
        if (deleteContext) {
            delete context;
        }
        return nullptr;
    }

    return handle;
}

void TqCancelClientTunnelOpen(TqClientTunnelOpenHandle* handle) {
    if (handle == nullptr) {
        return;
    }

    TqTunnelContext* context = nullptr;
    bool releaseUser = false;
    {
        std::lock_guard<std::mutex> guard(handle->Lock);
        if (handle->OpenState == TqClientTunnelOpenHandle::State::Accepted ||
            handle->OpenState == TqClientTunnelOpenHandle::State::Rejected ||
            handle->OpenState == TqClientTunnelOpenHandle::State::Cancelled) {
            return;
        }
        handle->OpenState = TqClientTunnelOpenHandle::State::Cancelled;
        context = handle->Context;
        handle->Context = nullptr;
        releaseUser = true;
    }

    if (context != nullptr && context->CancelAsyncClientOpen(handle)) {
        delete context;
    }
    if (releaseUser) {
        TqReleaseClientTunnelOpenHandle(handle);
    }
}

bool TqAcceptClientTunnelOpen(TqClientTunnelOpenHandle* handle) {
    if (handle == nullptr) {
        return false;
    }

    TqTunnelContext* context = nullptr;
    {
        std::lock_guard<std::mutex> guard(handle->Lock);
        if (handle->OpenState != TqClientTunnelOpenHandle::State::OpenSucceeded ||
            handle->Context == nullptr) {
            return false;
        }
        handle->OpenState = TqClientTunnelOpenHandle::State::Accepted;
        context = handle->Context;
        handle->Context = nullptr;
    }

    const bool accepted = context->AcceptAsyncClientOpen(handle);
    TqReleaseClientTunnelOpenHandle(handle);
    return accepted;
}

void TqRejectClientTunnelOpen(TqClientTunnelOpenHandle* handle) {
    if (handle == nullptr) {
        return;
    }

    TqTunnelContext* context = nullptr;
    bool releaseUser = false;
    {
        std::lock_guard<std::mutex> guard(handle->Lock);
        if (handle->OpenState == TqClientTunnelOpenHandle::State::Accepted ||
            handle->OpenState == TqClientTunnelOpenHandle::State::Rejected ||
            handle->OpenState == TqClientTunnelOpenHandle::State::Cancelled) {
            return;
        }
        handle->OpenState = TqClientTunnelOpenHandle::State::Rejected;
        context = handle->Context;
        handle->Context = nullptr;
        releaseUser = true;
    }

    if (context != nullptr) {
        (void)context->RejectAsyncClientOpen(handle);
    }
    if (releaseUser) {
        TqReleaseClientTunnelOpenHandle(handle);
    }
}

TqTunnelStartResult TqStartClientTunnelReceiveSink(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    const TqConfig& cfg,
    std::atomic<uint64_t>* receiveBytes) {
    if (receiveBytes == nullptr) {
        return {false, TqOpenError::Internal};
    }
    return TqStartClientTunnelInternal(conn, req, TqInvalidSocket, cfg, true, receiveBytes);
}

namespace {

bool TqSendDispatcherBytes(
    MsQuicStream* stream,
    const std::vector<uint8_t>& data,
    QUIC_SEND_FLAGS flags) {
    auto* sendContext = TqTunnelSendContext::New(data.data(), data.size());
    if (sendContext == nullptr) {
        return false;
    }
    const QUIC_STATUS status = stream->Send(&sendContext->Buffer, 1, flags, sendContext);
    if (QUIC_FAILED(status)) {
        TqTunnelSendContext::Delete(sendContext);
        return false;
    }
    return true;
}

bool TqSendDispatcherSpeedError(
    MsQuicStream* stream,
    uint32_t sessionId,
    TqSpeedError error,
    const char* message) {
    std::vector<uint8_t> encoded;
    if (!TqEncodeSpeedError(TqSpeedErrorMessage{sessionId, error, message}, encoded)) {
        return false;
    }
    return TqSendDispatcherBytes(stream, encoded, QUIC_SEND_FLAG_FIN);
}

class TqServerIncomingStreamDispatcher final {
public:
    TqServerIncomingStreamDispatcher(
        MsQuicConnection* conn,
        MsQuicStream* stream,
        const TqAcl& acl,
        const TqConfig& cfg,
        TqServerSpeedTestController* speed,
        const TqEphemeralTargetAuthorizer* authorizer,
        TqTunnelCompletionFn onComplete,
        TqTunnelAclDeniedFn onAclDenied) :
        Conn_(conn),
        Stream_(stream),
        Acl_(acl),
        Config_(cfg),
        Speed_(speed),
        Authorizer_(authorizer),
        OnComplete_(std::move(onComplete)),
        OnAclDenied_(std::move(onAclDenied)) {}

    ~TqServerIncomingStreamDispatcher() {
        if (!OwnershipTransferred_ && OnComplete_) {
            OnComplete_();
        }
    }

    static QUIC_STATUS QUIC_API Callback(
        _In_ MsQuicStream* stream,
        _In_opt_ void* context,
        _Inout_ QUIC_STREAM_EVENT* event) noexcept {
        auto* dispatcher = static_cast<TqServerIncomingStreamDispatcher*>(context);
        if (dispatcher == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        return dispatcher->OnStreamEvent(stream, event);
    }

private:
    QUIC_STATUS OnStreamEvent(MsQuicStream*, QUIC_STREAM_EVENT* event) noexcept {
        switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            if (CloseAfterStructuredError_) {
                break;
            }
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                const auto& buffer = event->RECEIVE.Buffers[i];
                BufferedRx_.insert(
                    BufferedRx_.end(),
                    buffer.Buffer,
                    buffer.Buffer + buffer.Length);
            }
            TryDispatch();
            break;
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            TqTunnelSendContext::Delete(
                static_cast<TqTunnelSendContext*>(event->SEND_COMPLETE.ClientContext));
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

    void TryDispatch() {
        if (BufferedRx_.size() < 4 || Stream_ == nullptr) {
            return;
        }

        if (BufferedRx_[0] != TQ_MAGIC_0 ||
            BufferedRx_[1] != TQ_MAGIC_1 ||
            BufferedRx_[2] != TQ_VERSION) {
            AbortOrClose();
            return;
        }

        switch (BufferedRx_[3]) {
        case TQ_CMD_OPEN:
            HandOffToTunnelContext();
            return;
        case TQ_CMD_SPEED_START:
            HandOffToSpeedControlStream();
            return;
        default:
            if (!QueueStructuredError(
                Stream_,
                0,
                TqSpeedError::Unsupported,
                "unsupported control stream")) {
                AbortOrClose();
            }
            return;
        }
    }

    void HandOffToTunnelContext() {
        auto* context = new (std::nothrow) TqTunnelContext(
            TqTunnelRole::ServerOpen,
            Stream_,
            TqInvalidSocket,
            Config_,
            &Acl_,
            Authorizer_,
            Conn_,
            false,
            nullptr,
            std::move(OnComplete_),
            std::move(OnAclDenied_));
        if (context == nullptr) {
            AbortOrClose();
            return;
        }

        context->SetStream(Stream_);
        context->TakeOpeningBytes(std::move(BufferedRx_));
        Stream_->Callback = TqTunnelContext::Callback;
        Stream_->Context = context;
        context->RegisterWithConnectionIfNeeded();

        OwnershipTransferred_ = true;
        Stream_ = nullptr;
        context->ResumeServerOpenFromBufferedData();
        delete this;
    }

    void HandOffToSpeedControlStream() {
        if (BufferedRx_.size() < TQ_SPEED_START_SIZE) {
            return;
        }

        TqSpeedStart start{};
        if (!TqDecodeSpeedStart(BufferedRx_.data(), TQ_SPEED_START_SIZE, start)) {
            if (!QueueStructuredError(Stream_, 0, TqSpeedError::InvalidRequest, "invalid speed start")) {
                AbortOrClose();
            }
            return;
        }

        if (Speed_ == nullptr) {
            if (!QueueStructuredError(
                    Stream_,
                    start.SessionId,
                    TqSpeedError::Unsupported,
                    "speed control unavailable")) {
                AbortOrClose();
            }
            return;
        }

        if (!TqAttachServerSpeedControlStream(
                *Speed_,
                Conn_,
                Stream_,
                std::move(BufferedRx_),
                std::move(OnComplete_))) {
            AbortOrClose();
            return;
        }

        OwnershipTransferred_ = true;
        Stream_ = nullptr;
        delete this;
    }

    bool QueueStructuredError(
        MsQuicStream* stream,
        uint32_t sessionId,
        TqSpeedError error,
        const char* message) {
        if (!TqSendDispatcherSpeedError(stream, sessionId, error, message)) {
            return false;
        }
        CloseAfterStructuredError_ = true;
        return true;
    }

    void AbortOrClose() {
        if (Stream_ != nullptr) {
            (void)Stream_->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
        }
    }

    MsQuicConnection* Conn_{nullptr};
    MsQuicStream* Stream_{nullptr};
    const TqAcl& Acl_;
    TqConfig Config_;
    TqServerSpeedTestController* Speed_{nullptr};
    const TqEphemeralTargetAuthorizer* Authorizer_{nullptr};
    TqTunnelCompletionFn OnComplete_;
    TqTunnelAclDeniedFn OnAclDenied_;
    std::vector<uint8_t> BufferedRx_;
    bool OwnershipTransferred_{false};
    bool CloseAfterStructuredError_{false};
};

void TqHandleServerIncomingStreamInternal(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqServerSpeedTestController* speed,
    const TqEphemeralTargetAuthorizer* authorizer,
    TqTunnelCompletionFn onComplete,
    TqTunnelAclDeniedFn onAclDenied) {
    if (rawStream == nullptr) {
        return;
    }

    auto* stream = new (std::nothrow) MsQuicStream(
        rawStream,
        CleanUpAutoDelete,
        MsQuicStream::NoOpCallback,
        nullptr);
    if (stream == nullptr || !stream->IsValid()) {
        delete stream;
        if (onComplete) {
            onComplete();
        }
        MsQuic->StreamClose(rawStream);
        return;
    }

    auto* dispatcher = new (std::nothrow) TqServerIncomingStreamDispatcher(
        conn,
        stream,
        acl,
        cfg,
        speed,
        authorizer,
        std::move(onComplete),
        std::move(onAclDenied));
    if (dispatcher == nullptr) {
        delete stream;
        if (onComplete) {
            onComplete();
        }
        MsQuic->StreamClose(rawStream);
        return;
    }

    stream->Callback = TqServerIncomingStreamDispatcher::Callback;
    stream->Context = dispatcher;
}

} // namespace

void TqHandleServerIncomingStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqServerSpeedTestController* speed,
    TqTunnelCompletionFn onComplete,
    TqTunnelAclDeniedFn onAclDenied) {
    TqHandleServerIncomingStreamInternal(
        conn,
        rawStream,
        acl,
        cfg,
        speed,
        speed,
        std::move(onComplete),
        std::move(onAclDenied));
}

#if defined(TCPQUIC_TUNNEL_TESTING)
void TqHandleServerIncomingStreamForTest(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    const TqEphemeralTargetAuthorizer* authorizer,
    TqTunnelCompletionFn onComplete,
    TqTunnelAclDeniedFn onAclDenied) {
    TqHandleServerIncomingStreamInternal(
        conn,
        rawStream,
        acl,
        cfg,
        nullptr,
        authorizer,
        std::move(onComplete),
        std::move(onAclDenied));
}
#endif

void TqHandleServerPeerStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqTunnelCompletionFn onComplete,
    TqTunnelAclDeniedFn onAclDenied) {
    TqHandleServerIncomingStream(
        conn,
        rawStream,
        acl,
        cfg,
        nullptr,
        std::move(onComplete),
        std::move(onAclDenied));
}

#if defined(TCPQUIC_TUNNEL_TESTING)
TqTunnelContext* TqCreateTestRegisteredTunnel(
    MsQuicConnection* connection,
    TqSocketHandle tcpFd) {
    TqConfig cfg;
    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ClientOpen,
        nullptr,
        tcpFd,
        cfg,
        nullptr,
        nullptr,
        connection);
    if (context != nullptr) {
        context->RegisterWithConnectionIfNeeded();
    }
    return context;
}

void TqDestroyTestRegisteredTunnel(TqTunnelContext* context) {
    delete context;
}

TqTunnelContext* TqCreateTestClientOpenOwnedTunnel(unsigned* destroyCount) {
    TqConfig cfg;
    auto onComplete = [destroyCount]() {
        if (destroyCount != nullptr) {
            ++(*destroyCount);
        }
    };
    return new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ClientOpen,
        nullptr,
        TqInvalidSocket,
        cfg,
        nullptr,
        nullptr,
        nullptr,
        false,
        nullptr,
        std::move(onComplete));
}

#if defined(__linux__)
TqTunnelContext* TqCreateTestServerOpenLegacyPendingTunnel(unsigned* destroyCount) {
    TqConfig cfg;
    auto onComplete = [destroyCount]() {
        if (destroyCount != nullptr) {
            ++(*destroyCount);
        }
    };
    auto* context = new (std::nothrow) TqTunnelContext(
        TqTunnelRole::ServerOpen,
        nullptr,
        TqInvalidSocket,
        cfg,
        nullptr,
        nullptr,
        nullptr,
        false,
        nullptr,
        std::move(onComplete));
    if (context != nullptr) {
        context->TestMarkLegacyServerOpenPending();
    }
    return context;
}

bool TqTestCancelServerDialAndHasPending(TqTunnelContext* context) {
    if (context == nullptr) {
        return false;
    }
    return context->TestCancelServerDialAndHasPending();
}
#endif

void TqTestArmSelfDeleteOnShutdown(TqTunnelContext* context) {
    if (context != nullptr) {
        context->ArmSelfDeleteOnShutdown();
    }
}

void TqTestDispatchShutdownComplete(TqTunnelContext* context) {
    if (context == nullptr) {
        return;
    }
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)TqTunnelContext::Callback(nullptr, context, &event);
}

void TqReleaseTestClientOpenOwner(TqTunnelContext* context) {
    if (context == nullptr) {
        return;
    }
    if (context->ReleaseClientOpenOwnerAndMaybeDelete()) {
        delete context;
    }
}
#endif
