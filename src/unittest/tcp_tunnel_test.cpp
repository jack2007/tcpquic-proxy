#include "platform_socket.h"
#include "quic_session.h"
#include "speed_test.h"
#include "relay.h"
#include "tcp_dialer.h"
#include "tcp_tunnel.h"
#include "tunnel_registry.h"
#include "trace.h"

struct MsQuicConnection;
struct TqTunnelContext;

#if defined(TCPQUIC_TUNNEL_TESTING)
TqTunnelContext* TqCreateTestRegisteredTunnel(
    MsQuicConnection* connection,
    TqSocketHandle tcpFd);
void TqDestroyTestRegisteredTunnel(TqTunnelContext* context);
TqTunnelContext* TqCreateTestClientOpenOwnedTunnel(unsigned* destroyCount);
void TqTestArmSelfDeleteOnShutdown(TqTunnelContext* context);
void TqTestDispatchShutdownComplete(TqTunnelContext* context);
void TqReleaseTestClientOpenOwner(TqTunnelContext* context);
#endif

uint32_t TqLookupServerConnectionId(MsQuicConnection* connection) {
    (void)connection;
    return 0;
}

uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection) {
    (void)connection;
    return 0;
}

bool TqTraceEnabled() {
    return false;
}

uint64_t TqTraceStreamStarted(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    const char* target,
    uint8_t compressFlags) {
    (void)connection;
    (void)connId;
    (void)role;
    (void)target;
    (void)compressFlags;
    return 0;
}

void TqTraceIncOpenTx(uint32_t connId) {
    (void)connId;
}

void TqTraceIncOpenRx(uint32_t connId) {
    (void)connId;
}

void TqTraceRelayStarted(uint64_t tunnelId) {
    (void)tunnelId;
}

void TqTraceOpenResult(uint64_t tunnelId, bool ok, TqOpenError error, uint32_t connIdField) {
    (void)tunnelId;
    (void)ok;
    (void)error;
    (void)connIdField;
}

void TqTraceStreamClosed(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    bool relayStarted,
    TqOpenError closeReason) {
    (void)tunnelId;
    (void)role;
    (void)target;
    (void)relayStarted;
    (void)closeReason;
}

void TqTraceProxyClosed(TqTraceProxyProto proto, TqSocketHandle fd) {
    (void)proto;
    (void)fd;
}

void TqTraceTargetTcpDialing(uint64_t tunnelId, const char* target) {
    (void)tunnelId;
    (void)target;
}

void TqTraceTargetTcpConnected(uint64_t tunnelId, TqSocketHandle fd) {
    (void)tunnelId;
    (void)fd;
}

void TqTraceTargetTcpFailed(uint64_t tunnelId, TqOpenError error) {
    (void)tunnelId;
    (void)error;
}

void TqTraceTargetTcpClosed(uint64_t tunnelId) {
    (void)tunnelId;
}
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

static unsigned g_abort_a = 0;
static unsigned g_abort_b = 0;
static unsigned g_duplicate_abort_a = 0;
static unsigned g_duplicate_abort_b = 0;
static unsigned g_reentrant_abort = 0;
static uint32_t g_reentrant_aborted = 0;
static MsQuicConnection* g_reentrant_conn = nullptr;
static unsigned g_self_unregister_abort = 0;
static unsigned g_self_register_abort = 0;
static MsQuicConnection* g_self_unregister_conn = nullptr;
static MsQuicConnection* g_self_register_conn = nullptr;
static void* g_self_unregister_ctx = nullptr;
static void* g_self_register_ctx = nullptr;

static void CountAbortA(void*) { ++g_abort_a; }
static void CountAbortB(void*) { ++g_abort_b; }
static void CountDuplicateAbortA(void*) { ++g_duplicate_abort_a; }
static void CountDuplicateAbortB(void*) { ++g_duplicate_abort_b; }
static void CountReentrantAbort(void*) {
    ++g_reentrant_abort;
    g_reentrant_aborted = TqAbortConnectionTunnels(g_reentrant_conn);
}
static void SelfUnregisterAbort(void*) {
    ++g_self_unregister_abort;
    TqUnregisterConnectionTunnel(g_self_unregister_conn, g_self_unregister_ctx);
}
static void SelfRegisterAbort(void*) {
    ++g_self_register_abort;
    if (g_self_register_abort == 1) {
        TqRegisterConnectionTunnel(g_self_register_conn, g_self_register_ctx, SelfRegisterAbort);
    }
}

namespace {

struct FakeQuicSendRecord {
    std::vector<uint8_t> Bytes;
    QUIC_SEND_FLAGS Flags{QUIC_SEND_FLAG_NONE};
    void* ClientContext{nullptr};
};

struct FakeQuicStreamRecord {
    void* Handler{nullptr};
    void* Context{nullptr};
    std::vector<FakeQuicSendRecord> Sends;
    unsigned CloseCount{0};
    unsigned ShutdownCount{0};
};

std::mutex g_fake_quic_lock;
std::map<HQUIC, FakeQuicStreamRecord> g_fake_quic_streams;

void ResetFakeQuicState() {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    g_fake_quic_streams.clear();
}

void QUIC_API FakeSetCallbackHandler(HQUIC handle, void* handler, void* context) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    auto& record = g_fake_quic_streams[handle];
    record.Handler = handler;
    record.Context = context;
}

QUIC_STATUS QUIC_API FakeStreamSend(
    HQUIC handle,
    const QUIC_BUFFER* const buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* clientSendContext) {
    std::vector<uint8_t> bytes;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        bytes.insert(
            bytes.end(),
            buffers[i].Buffer,
            buffers[i].Buffer + buffers[i].Length);
    }

    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    auto& record = g_fake_quic_streams[handle];
    record.Sends.push_back(FakeQuicSendRecord{std::move(bytes), flags, clientSendContext});
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC handle,
    QUIC_STREAM_SHUTDOWN_FLAGS,
    QUIC_UINT62) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    ++g_fake_quic_streams[handle].ShutdownCount;
    return QUIC_STATUS_SUCCESS;
}

void QUIC_API FakeStreamClose(HQUIC handle) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    ++g_fake_quic_streams[handle].CloseCount;
}

void InstallFakeMsQuicForTcpTunnel(QUIC_API_TABLE& table) {
    std::memset(&table, 0, sizeof(table));
    table.SetCallbackHandler = FakeSetCallbackHandler;
    table.StreamSend = FakeStreamSend;
    table.StreamShutdown = FakeStreamShutdown;
    table.StreamClose = FakeStreamClose;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&table);
    ResetFakeQuicState();
}

bool DispatchFakeStreamEvent(HQUIC handle, QUIC_STREAM_EVENT& event) {
    void* handler = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_fake_quic_lock);
        const auto it = g_fake_quic_streams.find(handle);
        if (it == g_fake_quic_streams.end()) {
            return false;
        }
        handler = it->second.Handler;
        context = it->second.Context;
    }

    if (handler == nullptr) {
        return false;
    }

    auto* callback =
        reinterpret_cast<QUIC_STREAM_CALLBACK_HANDLER>(handler);
    return callback(handle, context, &event) == QUIC_STATUS_SUCCESS;
}

bool WaitForFakeSend(
    HQUIC handle,
    std::vector<uint8_t>& bytes,
    QUIC_SEND_FLAGS& flags,
    uint32_t timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> guard(g_fake_quic_lock);
            const auto it = g_fake_quic_streams.find(handle);
            if (it != g_fake_quic_streams.end() && !it->second.Sends.empty()) {
                bytes = it->second.Sends.front().Bytes;
                flags = it->second.Sends.front().Flags;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool TakeFakeSendRecord(HQUIC handle, FakeQuicSendRecord& record) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    const auto it = g_fake_quic_streams.find(handle);
    if (it == g_fake_quic_streams.end() || it->second.Sends.empty()) {
        return false;
    }
    record = it->second.Sends.front();
    it->second.Sends.erase(it->second.Sends.begin());
    return true;
}

unsigned FakeShutdownCount(HQUIC handle) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    const auto it = g_fake_quic_streams.find(handle);
    return it == g_fake_quic_streams.end() ? 0 : it->second.ShutdownCount;
}

bool DispatchFakeSendComplete(HQUIC handle) {
    FakeQuicSendRecord record;
    if (!TakeFakeSendRecord(handle, record)) {
        return false;
    }
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = record.ClientContext;
    return DispatchFakeStreamEvent(handle, event);
}

bool DispatchFakeShutdownComplete(HQUIC handle) {
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    return DispatchFakeStreamEvent(handle, event);
}

MsQuicStream* LookupFakeMsQuicStream(HQUIC handle) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    const auto it = g_fake_quic_streams.find(handle);
    if (it == g_fake_quic_streams.end()) {
        return nullptr;
    }
    return static_cast<MsQuicStream*>(it->second.Context);
}

TqTunnelContext* LookupFakeTunnelContext(HQUIC handle) {
    MsQuicStream* stream = LookupFakeMsQuicStream(handle);
    return stream == nullptr ? nullptr : static_cast<TqTunnelContext*>(stream->Context);
}

bool ReapPreparedFakeTunnel(MsQuicStream* stream, TqTunnelContext* context) {
    if (stream == nullptr || context == nullptr) {
        return false;
    }
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqReapTunnelContext(context);
    return true;
}

std::vector<uint8_t> BuildOpenRequestBytes(const char* host, uint16_t port) {
    TqOpenRequest req{};
    req.Flags = 0;
    req.AddrType = TQ_ADDR_IPV4;
    req.Port = port;
    req.Addr.resize(4);
    if (!TqInetPton(AF_INET, host, req.Addr.data())) {
        return {};
    }

    std::vector<uint8_t> encoded;
    if (!TqEncodeOpenRequest(req, encoded)) {
        encoded.clear();
    }
    return encoded;
}

std::vector<uint8_t> BuildSpeedStartBytes(uint32_t sessionId) {
    TqSpeedStart start{};
    start.SessionId = sessionId;
    start.Direction = TqSpeedDirection::Download;
    start.DurationSec = 5;
    start.Parallel = 1;

    std::vector<uint8_t> encoded;
    if (!TqEncodeSpeedStart(start, encoded)) {
        encoded.clear();
    }
    return encoded;
}

class FakeEphemeralTargetAuthorizer final : public TqEphemeralTargetAuthorizer {
public:
    FakeEphemeralTargetAuthorizer(std::string host, uint16_t port) :
        Host_(std::move(host)),
        Port_(port) {}

    bool IsAllowedEphemeralTarget(const std::string& host, uint16_t port) const override {
        return host == Host_ && port == Port_;
    }

private:
    std::string Host_;
    uint16_t Port_{0};
};

class LoopbackListener {
public:
    bool Start() {
        Listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!TqSocketValid(Listener_)) {
            return false;
        }
        if (!TqSetReuseAddr(Listener_)) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        if (!TqInetPton(AF_INET, "127.0.0.1", &addr.sin_addr)) {
            return false;
        }
        if (::bind(Listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        if (::listen(Listener_, 1) != 0) {
            return false;
        }

        sockaddr_in bound{};
        socklen_t boundLen = sizeof(bound);
        if (::getsockname(Listener_, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
            return false;
        }
        Port_ = ntohs(bound.sin_port);
        AcceptThread_ = std::thread([this]() {
            sockaddr_in peer{};
            socklen_t peerLen = sizeof(peer);
            Accepted_ =
                ::accept(Listener_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        });
        return true;
    }

    ~LoopbackListener() {
        if (TqSocketValid(Listener_)) {
            (void)TqShutdownBoth(Listener_);
            TqCloseSocket(Listener_);
        }
        if (AcceptThread_.joinable()) {
            AcceptThread_.join();
        }
        if (TqSocketValid(Accepted_)) {
            TqCloseSocket(Accepted_);
        }
    }

    uint16_t Port() const { return Port_; }

private:
    TqSocketHandle Listener_{TqInvalidSocket};
    TqSocketHandle Accepted_{TqInvalidSocket};
    uint16_t Port_{0};
    std::thread AcceptThread_;
};

int TestServerIncomingOpenDispatchesWithInitialBytes() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 228;

    QUIC_API_TABLE fakeApi{};
    InstallFakeMsQuicForTcpTunnel(fakeApi);

    LoopbackListener listener;
    if (!listener.Start()) return 229;

    TqAcl acl;
    acl.AllowCidrs.push_back("127.0.0.0/8");
    TqConfig cfg{};

    auto rawStream = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7101));
    TqHandleServerIncomingStream(nullptr, rawStream, acl, cfg, nullptr);

    const std::vector<uint8_t> openBytes =
        BuildOpenRequestBytes("127.0.0.1", listener.Port());
    if (openBytes.empty()) return 230;

    QUIC_BUFFER firstBuffer{
        static_cast<uint32_t>(4),
        const_cast<uint8_t*>(openBytes.data()),
    };
    QUIC_STREAM_EVENT firstEvent{};
    firstEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    firstEvent.RECEIVE.BufferCount = 1;
    firstEvent.RECEIVE.Buffers = &firstBuffer;
    if (!DispatchFakeStreamEvent(rawStream, firstEvent)) return 231;

    QUIC_BUFFER secondBuffer{
        static_cast<uint32_t>(openBytes.size() - 4),
        const_cast<uint8_t*>(openBytes.data() + 4),
    };
    QUIC_STREAM_EVENT secondEvent{};
    secondEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    secondEvent.RECEIVE.BufferCount = 1;
    secondEvent.RECEIVE.Buffers = &secondBuffer;
    if (!DispatchFakeStreamEvent(rawStream, secondEvent)) return 232;

    std::vector<uint8_t> responseBytes;
    QUIC_SEND_FLAGS responseFlags = QUIC_SEND_FLAG_NONE;
    if (!WaitForFakeSend(rawStream, responseBytes, responseFlags, 2000)) return 233;

    TqOpenResponse response{};
    if (!TqDecodeOpenResponse(responseBytes.data(), responseBytes.size(), response)) return 234;
    if (!response.Ok) return 235;
    if (response.Error != TqOpenError::Ok) return 236;
    if (responseFlags != QUIC_SEND_FLAG_NONE) return 237;
    MsQuicStream* wrappedStream = LookupFakeMsQuicStream(rawStream);
    TqTunnelContext* tunnelContext = LookupFakeTunnelContext(rawStream);
    if (wrappedStream == nullptr || tunnelContext == nullptr) return 238;
    if (!DispatchFakeSendComplete(rawStream)) return 239;
    if (!ReapPreparedFakeTunnel(wrappedStream, tunnelContext)) return 240;
    if (!DispatchFakeShutdownComplete(rawStream)) return 241;
    return 0;
}

int TestServerIncomingLoopbackDeniedWithoutAuthorizer() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 238;

    QUIC_API_TABLE fakeApi{};
    InstallFakeMsQuicForTcpTunnel(fakeApi);

    TqAcl acl;
    acl.AllowCidrs.push_back("10.0.0.0/8");
    TqConfig cfg{};

    bool aclDenied = false;
    auto rawStream = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7102));
    TqHandleServerIncomingStream(
        nullptr,
        rawStream,
        acl,
        cfg,
        nullptr,
        {},
        [&aclDenied]() { aclDenied = true; });

    const std::vector<uint8_t> openBytes = BuildOpenRequestBytes("127.0.0.1", 34567);
    if (openBytes.empty()) return 239;

    QUIC_BUFFER buffer{
        static_cast<uint32_t>(openBytes.size()),
        const_cast<uint8_t*>(openBytes.data()),
    };
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;
    if (!DispatchFakeStreamEvent(rawStream, event)) return 240;

    std::vector<uint8_t> responseBytes;
    QUIC_SEND_FLAGS responseFlags = QUIC_SEND_FLAG_NONE;
    if (!WaitForFakeSend(rawStream, responseBytes, responseFlags, 2000)) return 241;

    TqOpenResponse response{};
    if (!TqDecodeOpenResponse(responseBytes.data(), responseBytes.size(), response)) return 242;
    if (response.Ok) return 243;
    if (response.Error != TqOpenError::AclDenied) return 244;
    if (!aclDenied) return 245;
    if (responseFlags != QUIC_SEND_FLAG_FIN) return 246;
    if (!DispatchFakeSendComplete(rawStream)) return 247;
    if (FakeShutdownCount(rawStream) != 1) return 248;
    if (!DispatchFakeShutdownComplete(rawStream)) return 249;
    return 0;
}

int TestServerIncomingLoopbackAllowedByEphemeralAuthorizer() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 247;

    QUIC_API_TABLE fakeApi{};
    InstallFakeMsQuicForTcpTunnel(fakeApi);

    LoopbackListener listener;
    if (!listener.Start()) return 248;

    TqAcl acl;
    acl.AllowCidrs.push_back("10.0.0.0/8");
    TqConfig cfg{};
    FakeEphemeralTargetAuthorizer authorizer("127.0.0.1", listener.Port());

    auto allowedStream = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7103));
    TqHandleServerIncomingStreamForTest(
        nullptr,
        allowedStream,
        acl,
        cfg,
        &authorizer);

    const std::vector<uint8_t> allowedOpen =
        BuildOpenRequestBytes("127.0.0.1", listener.Port());
    if (allowedOpen.empty()) return 249;

    QUIC_BUFFER allowedBuffer{
        static_cast<uint32_t>(allowedOpen.size()),
        const_cast<uint8_t*>(allowedOpen.data()),
    };
    QUIC_STREAM_EVENT allowedEvent{};
    allowedEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    allowedEvent.RECEIVE.BufferCount = 1;
    allowedEvent.RECEIVE.Buffers = &allowedBuffer;
    if (!DispatchFakeStreamEvent(allowedStream, allowedEvent)) return 250;

    std::vector<uint8_t> allowedResponseBytes;
    QUIC_SEND_FLAGS allowedResponseFlags = QUIC_SEND_FLAG_NONE;
    if (!WaitForFakeSend(allowedStream, allowedResponseBytes, allowedResponseFlags, 2000)) return 251;

    TqOpenResponse allowedResponse{};
    if (!TqDecodeOpenResponse(
            allowedResponseBytes.data(),
            allowedResponseBytes.size(),
            allowedResponse)) {
        return 252;
    }
    if (!allowedResponse.Ok) return 253;
    if (allowedResponse.Error != TqOpenError::Ok) return 254;
    MsQuicStream* allowedWrappedStream = LookupFakeMsQuicStream(allowedStream);
    TqTunnelContext* allowedTunnelContext = LookupFakeTunnelContext(allowedStream);
    if (allowedWrappedStream == nullptr || allowedTunnelContext == nullptr) return 255;
    if (!DispatchFakeSendComplete(allowedStream)) return 256;
    if (!ReapPreparedFakeTunnel(allowedWrappedStream, allowedTunnelContext)) return 257;
    if (!DispatchFakeShutdownComplete(allowedStream)) return 258;

    auto deniedStream = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7104));
    bool aclDenied = false;
    TqHandleServerIncomingStreamForTest(
        nullptr,
        deniedStream,
        acl,
        cfg,
        &authorizer,
        {},
        [&aclDenied]() { aclDenied = true; });

    const std::vector<uint8_t> deniedOpen =
        BuildOpenRequestBytes("127.0.0.1", static_cast<uint16_t>(listener.Port() + 1));
    if (deniedOpen.empty()) return 259;

    QUIC_BUFFER deniedBuffer{
        static_cast<uint32_t>(deniedOpen.size()),
        const_cast<uint8_t*>(deniedOpen.data()),
    };
    QUIC_STREAM_EVENT deniedEvent{};
    deniedEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    deniedEvent.RECEIVE.BufferCount = 1;
    deniedEvent.RECEIVE.Buffers = &deniedBuffer;
    if (!DispatchFakeStreamEvent(deniedStream, deniedEvent)) return 260;

    std::vector<uint8_t> deniedResponseBytes;
    QUIC_SEND_FLAGS deniedResponseFlags = QUIC_SEND_FLAG_NONE;
    if (!WaitForFakeSend(deniedStream, deniedResponseBytes, deniedResponseFlags, 2000)) return 261;

    TqOpenResponse deniedResponse{};
    if (!TqDecodeOpenResponse(
            deniedResponseBytes.data(),
            deniedResponseBytes.size(),
            deniedResponse)) {
        return 262;
    }
    if (deniedResponse.Ok) return 263;
    if (deniedResponse.Error != TqOpenError::AclDenied) return 264;
    if (!aclDenied) return 265;
    if (!DispatchFakeSendComplete(deniedStream)) return 266;
    if (FakeShutdownCount(deniedStream) != 1) return 267;
    if (!DispatchFakeShutdownComplete(deniedStream)) return 268;
    return 0;
}

int TestServerIncomingSpeedStartQueuesStructuredErrorWithoutImmediateAbort() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 268;

    QUIC_API_TABLE fakeApi{};
    InstallFakeMsQuicForTcpTunnel(fakeApi);

    TqAcl acl;
    acl.AllowCidrs.push_back("127.0.0.0/8");
    TqConfig cfg{};

    auto rawStream = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7105));
    TqHandleServerIncomingStream(nullptr, rawStream, acl, cfg, nullptr);

    const std::vector<uint8_t> speedStart = BuildSpeedStartBytes(77);
    if (speedStart.empty()) return 269;

    QUIC_BUFFER buffer{
        static_cast<uint32_t>(speedStart.size()),
        const_cast<uint8_t*>(speedStart.data()),
    };
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;
    if (!DispatchFakeStreamEvent(rawStream, event)) return 270;

    std::vector<uint8_t> responseBytes;
    QUIC_SEND_FLAGS responseFlags = QUIC_SEND_FLAG_NONE;
    if (!WaitForFakeSend(rawStream, responseBytes, responseFlags, 2000)) return 271;

    TqSpeedErrorMessage response{};
    if (!TqDecodeSpeedError(responseBytes.data(), responseBytes.size(), response)) return 272;
    if (response.SessionId != 77) return 273;
    if (response.Error != TqSpeedError::Unsupported) return 274;
    if (responseFlags != QUIC_SEND_FLAG_FIN) return 275;
    if (FakeShutdownCount(rawStream) != 0) return 276;
    if (!DispatchFakeSendComplete(rawStream)) return 277;
    if (FakeShutdownCount(rawStream) != 0) return 278;
    if (!DispatchFakeShutdownComplete(rawStream)) return 279;
    return 0;
}

} // namespace

static int TestTunnelRegistryAbortsOnlyMatchingConnection() {
    auto* conn1 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1001));
    auto* conn2 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1002));
    int ctx1 = 1;
    int ctx2 = 2;
    g_abort_a = 0;
    g_abort_b = 0;

    TqRegisterConnectionTunnel(conn1, &ctx1, CountAbortA);
    TqRegisterConnectionTunnel(conn2, &ctx2, CountAbortB);
    const uint32_t aborted = TqAbortConnectionTunnels(conn1);
    TqUnregisterConnectionTunnel(conn2, &ctx2);

    if (aborted != 1) return 201;
    if (g_abort_a != 1) return 202;
    if (g_abort_b != 0) return 203;
    return 0;
}

static int TestQuicClientSessionReconnectApiSurface() {
    using Handler = QuicClientSession::ConnectionStateHandler;
    static_assert(std::is_same<Handler, std::function<void(uint32_t)>>::value,
        "connection-state callback reports connected slot count");
    static_assert(std::is_same<decltype(std::declval<const QuicClientSession&>().ConnectedConnectionCount()), uint32_t>::value,
        "connected count is publicly readable");
    static_assert(std::is_same<decltype(std::declval<QuicClientSession&>().EnsureAnyConnected()), bool>::value,
        "eager connect API has a default timeout");
    static_assert(std::is_same<decltype(std::declval<QuicClientSession&>().EnsureAnyConnected(std::chrono::milliseconds(1))), bool>::value,
        "eager connect API accepts an explicit timeout");

    (void)static_cast<void (QuicClientSession::*)(Handler)>(&QuicClientSession::SetConnectionStateHandler);
    return 0;
}

static int TestTunnelRegistryRemovesBeforeCallbacks() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x2001));
    int ctx = 1;
    g_reentrant_abort = 0;
    g_reentrant_aborted = 99;
    g_reentrant_conn = conn;

    TqRegisterConnectionTunnel(conn, &ctx, CountReentrantAbort);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    const uint32_t abortedAgain = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 204;
    if (g_reentrant_abort != 1) return 205;
    if (g_reentrant_aborted != 0) return 206;
    if (abortedAgain != 0) return 207;
    return 0;
}

static int TestTunnelRegistryDuplicateRegistrationIsSingleEntry() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x3001));
    int ctx = 1;
    g_duplicate_abort_a = 0;
    g_duplicate_abort_b = 0;

    TqRegisterConnectionTunnel(conn, &ctx, CountDuplicateAbortA);
    TqRegisterConnectionTunnel(conn, &ctx, CountDuplicateAbortB);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 208;
    if (g_duplicate_abort_a + g_duplicate_abort_b != 1) return 209;
    return 0;
}

struct TqAbortWaitProbe {
    std::mutex Lock;
    std::condition_variable Wakeup;
    bool AbortEntered{false};
    bool UnregisterStarted{false};
    bool ReleaseAbort{false};
    bool UnregisterReturnedBeforeRelease{false};
    std::atomic<bool> UnregisterReturned{false};
};

static void BlockingAbort(void* context) {
    auto* probe = static_cast<TqAbortWaitProbe*>(context);
    {
        std::lock_guard<std::mutex> guard(probe->Lock);
        probe->AbortEntered = true;
    }
    probe->Wakeup.notify_all();

    std::unique_lock<std::mutex> guard(probe->Lock);
    probe->Wakeup.wait(guard, [probe] { return probe->UnregisterStarted; });
    guard.unlock();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    probe->UnregisterReturnedBeforeRelease =
        probe->UnregisterReturned.load(std::memory_order_acquire);

    guard.lock();
    probe->Wakeup.wait(guard, [probe] { return probe->ReleaseAbort; });
}

static int TestTunnelRegistryUnregisterWaitsForInFlightAbort() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x4001));
    TqAbortWaitProbe probe;

    TqRegisterConnectionTunnel(conn, &probe, BlockingAbort);

    std::thread abortThread([conn] {
        (void)TqAbortConnectionTunnels(conn);
    });

    {
        std::unique_lock<std::mutex> guard(probe.Lock);
        if (!probe.Wakeup.wait_for(
                guard,
                std::chrono::seconds(2),
                [&probe] { return probe.AbortEntered; })) {
            probe.UnregisterStarted = true;
            probe.ReleaseAbort = true;
            probe.Wakeup.notify_all();
            abortThread.join();
            return 210;
        }
    }

    std::thread unregisterThread([conn, &probe] {
        {
            std::lock_guard<std::mutex> guard(probe.Lock);
            probe.UnregisterStarted = true;
        }
        probe.Wakeup.notify_all();
        TqUnregisterConnectionTunnel(conn, &probe);
        probe.UnregisterReturned.store(true, std::memory_order_release);
        probe.Wakeup.notify_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> guard(probe.Lock);
        probe.ReleaseAbort = true;
    }
    probe.Wakeup.notify_all();

    abortThread.join();
    unregisterThread.join();

    if (probe.UnregisterReturnedBeforeRelease) return 211;
    if (!probe.UnregisterReturned.load(std::memory_order_acquire)) return 212;
    return 0;
}

static int TestTunnelRegistryAbortCallbackCanUnregisterItself() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x5001));
    int ctx = 1;
    g_self_unregister_abort = 0;
    g_self_unregister_conn = conn;
    g_self_unregister_ctx = &ctx;

    TqRegisterConnectionTunnel(conn, &ctx, SelfUnregisterAbort);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    const uint32_t abortedAgain = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 213;
    if (g_self_unregister_abort != 1) return 214;
    if (abortedAgain != 0) return 215;
    return 0;
}

static int TestTunnelRegistryAbortCallbackCanRegisterSameContext() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6001));
    int ctx = 1;
    g_self_register_abort = 0;
    g_self_register_conn = conn;
    g_self_register_ctx = &ctx;

    TqRegisterConnectionTunnel(conn, &ctx, SelfRegisterAbort);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    const uint32_t abortedAgain = TqAbortConnectionTunnels(conn);
    const uint32_t abortedThird = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 216;
    if (abortedAgain != 1) return 217;
    if (abortedThird != 0) return 218;
    if (g_self_register_abort != 2) return 219;
    return 0;
}

static int TestConnectionAbortClosesTunnelTcp() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 220;

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(fds)) return 221;

    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x7001));
    TqTunnelContext* ctx = TqCreateTestRegisteredTunnel(conn, fds[0]);
    if (ctx == nullptr) {
        TqCloseSocket(fds[0]);
        TqCloseSocket(fds[1]);
        return 222;
    }

    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    if (aborted != 1) {
        TqDestroyTestRegisteredTunnel(ctx);
        TqCloseSocket(fds[1]);
        return 223;
    }

    char byte = 0;
    const int received = TqRecv(fds[1], &byte, 1, TqRecvFlags::None);
    TqCloseSocket(fds[1]);
    TqDestroyTestRegisteredTunnel(ctx);
    if (received != 0) return 224;
    return 0;
}

static int TestClientOpenOwnerDefersShutdownCompleteDelete() {
    unsigned destroyCount = 0;
    TqTunnelContext* ctx = TqCreateTestClientOpenOwnedTunnel(&destroyCount);
    if (ctx == nullptr) return 225;

    TqTestArmSelfDeleteOnShutdown(ctx);
    TqTestDispatchShutdownComplete(ctx);
    if (destroyCount != 0) return 226;

    TqReleaseTestClientOpenOwner(ctx);
    if (destroyCount != 1) return 227;
    return 0;
}

int main() {
    if (int rc = TestQuicClientSessionReconnectApiSurface()) return rc;
    if (int rc = TestTunnelRegistryAbortsOnlyMatchingConnection()) return rc;
    if (int rc = TestTunnelRegistryRemovesBeforeCallbacks()) return rc;
    if (int rc = TestTunnelRegistryDuplicateRegistrationIsSingleEntry()) return rc;
    if (int rc = TestTunnelRegistryUnregisterWaitsForInFlightAbort()) return rc;
    if (int rc = TestTunnelRegistryAbortCallbackCanUnregisterItself()) return rc;
    if (int rc = TestTunnelRegistryAbortCallbackCanRegisterSameContext()) return rc;
    if (int rc = TestConnectionAbortClosesTunnelTcp()) return rc;
    if (int rc = TestClientOpenOwnerDefersShutdownCompleteDelete()) return rc;
    if (int rc = TestServerIncomingOpenDispatchesWithInitialBytes()) return rc;
    if (int rc = TestServerIncomingLoopbackDeniedWithoutAuthorizer()) return rc;
    if (int rc = TestServerIncomingLoopbackAllowedByEphemeralAuthorizer()) return rc;
    if (int rc = TestServerIncomingSpeedStartQueuesStructuredErrorWithoutImmediateAbort()) return rc;

    TunnelRequest req{};
    req.AddrType = TQ_ADDR_DOMAIN;
    constexpr char host[] = "example.test";
    std::memcpy(req.Host, host, sizeof(host));
    req.Port = 443;
    req.CompressFlags = TQ_FLAG_COMPRESS;

    TqConfig cfg{};
    cfg.Compress = "off";

    TqSocketStartup startup;
    if (!startup.Ok()) return 1;

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(fds)) return 1;
    const TqTunnelStartResult badConn = TqStartClientTunnel(nullptr, req, fds[0], cfg);
    if (badConn.Ok) return 2;
    if (badConn.Error != TqOpenError::Internal) return 3;
    TqCloseSocket(fds[0]);
    TqCloseSocket(fds[1]);

    std::vector<sockaddr_storage> empty;
    if (TqSocketValid(TqDialTcp(empty, 1).Fd)) return 5;

    TqAcl acl;
    bool completed = false;
    bool aclDenied = false;
    TqHandleServerPeerStream(nullptr, nullptr, acl, cfg, [&completed]() { completed = true; }, [&aclDenied]() { aclDenied = true; });
    if (completed) return 6;
    if (aclDenied) return 7;

    {
        TqRelayHandle handle{};
        assert(!TqRelayLinuxFastPathEnabled(&handle));
#if defined(__linux__)
        handle.Backend = TqRelayBackendType::LinuxWorker;
        assert(TqRelayLinuxFastPathEnabled(&handle));
        if (handle.Backend != TqRelayBackendType::LinuxWorker) return 10;
#elif defined(_WIN32)
        handle.Backend = TqRelayBackendType::WindowsWorker;
        assert(!TqRelayLinuxFastPathEnabled(&handle));
        if (handle.Backend != TqRelayBackendType::WindowsWorker) return 10;
#endif
        handle.Backend = TqRelayBackendType::None;
    }

    return 0;
}
