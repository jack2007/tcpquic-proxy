#include "client_tunnel_open.h"
#include "stream_lifetime.h"
#include "tunnel_reaper.h"
#include "msquic.hpp"
#include "relay.h"
#include "speed_test.h"
#include "trace.h"

#include <chrono>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>
#include <thread>

const MsQuicApi* MsQuic = nullptr;

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
    unsigned ShutdownCount{0};
    unsigned CloseCount{0};
};

std::mutex g_fake_quic_lock;
std::map<HQUIC, FakeQuicStreamRecord> g_fake_quic_streams;
uintptr_t g_next_stream_handle = 0x8100;
unsigned g_relay_start_count = 0;
TqSocketHandle g_last_relay_fd = TqInvalidSocket;
bool g_relay_start_should_fail = false;
bool g_stream_send_should_fail = false;
unsigned g_trace_stream_closed_count = 0;
bool g_capture_relay_receive = false;
std::vector<uint8_t> g_relay_receive_bytes;

void ResetFakeState() {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    g_fake_quic_streams.clear();
    g_next_stream_handle = 0x8100;
    g_relay_start_count = 0;
    g_last_relay_fd = TqInvalidSocket;
    g_relay_start_should_fail = false;
    g_stream_send_should_fail = false;
    g_trace_stream_closed_count = 0;
    g_capture_relay_receive = false;
    g_relay_receive_bytes.clear();
}

void QUIC_API FakeSetCallbackHandler(HQUIC handle, void* handler, void* context) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    auto& record = g_fake_quic_streams[handle];
    record.Handler = handler;
    record.Context = context;
}

void QUIC_API FakeConnectionClose(HQUIC) {
}

QUIC_STATUS QUIC_API FakeStreamOpen(
    HQUIC,
    QUIC_STREAM_OPEN_FLAGS,
    QUIC_STREAM_CALLBACK_HANDLER handler,
    void* context,
    HQUIC* stream) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    *stream = reinterpret_cast<HQUIC>(++g_next_stream_handle);
    g_fake_quic_streams[*stream] = FakeQuicStreamRecord{reinterpret_cast<void*>(handler), context};
    return QUIC_STATUS_SUCCESS;
}

void QUIC_API FakeStreamClose(HQUIC handle) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    ++g_fake_quic_streams[handle].CloseCount;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC handle, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    ++g_fake_quic_streams[handle].ShutdownCount;
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API FakeStreamSend(
    HQUIC handle,
    const QUIC_BUFFER* const buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* clientSendContext) {
    if (g_stream_send_should_fail) {
        return QUIC_STATUS_INTERNAL_ERROR;
    }

    std::vector<uint8_t> bytes;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        bytes.insert(bytes.end(), buffers[i].Buffer, buffers[i].Buffer + buffers[i].Length);
    }

    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    g_fake_quic_streams[handle].Sends.push_back(
        FakeQuicSendRecord{std::move(bytes), flags, clientSendContext});
    return QUIC_STATUS_SUCCESS;
}

void InstallFakeMsQuic(QUIC_API_TABLE& table) {
    std::memset(&table, 0, sizeof(table));
    table.SetCallbackHandler = FakeSetCallbackHandler;
    table.ConnectionClose = FakeConnectionClose;
    table.StreamOpen = FakeStreamOpen;
    table.StreamClose = FakeStreamClose;
    table.StreamShutdown = FakeStreamShutdown;
    table.StreamSend = FakeStreamSend;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&table);
    ResetFakeState();
}

HQUIC LatestFakeStreamHandle() {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    return reinterpret_cast<HQUIC>(g_next_stream_handle);
}

unsigned FakeShutdownCount(HQUIC handle) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    const auto it = g_fake_quic_streams.find(handle);
    return it == g_fake_quic_streams.end() ? 0 : it->second.ShutdownCount;
}

unsigned FakeCloseCount(HQUIC handle) {
    std::lock_guard<std::mutex> guard(g_fake_quic_lock);
    const auto it = g_fake_quic_streams.find(handle);
    return it == g_fake_quic_streams.end() ? 0 : it->second.CloseCount;
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
    auto* callback = reinterpret_cast<QUIC_STREAM_CALLBACK_HANDLER>(handler);
    return callback(handle, context, &event) == QUIC_STATUS_SUCCESS;
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

bool DispatchOpenResponse(HQUIC handle, const TqOpenResponse& response) {
    std::vector<uint8_t> encoded;
    if (!TqEncodeOpenResponse(response, encoded)) {
        return false;
    }
    QUIC_BUFFER buffer{
        static_cast<uint32_t>(encoded.size()),
        encoded.data(),
    };
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;
    return DispatchFakeStreamEvent(handle, event);
}

bool DispatchOpenResponseWithPayload(
    HQUIC handle,
    const TqOpenResponse& response,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> encoded;
    if (!TqEncodeOpenResponse(response, encoded)) {
        return false;
    }
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    QUIC_BUFFER buffer{
        static_cast<uint32_t>(encoded.size()),
        encoded.data(),
    };
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;
    return DispatchFakeStreamEvent(handle, event);
}

QUIC_STATUS QUIC_API RelayCaptureCallback(
    MsQuicStream*,
    void*,
    QUIC_STREAM_EVENT* event) {
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const QUIC_BUFFER& buffer = event->RECEIVE.Buffers[i];
            g_relay_receive_bytes.insert(
                g_relay_receive_bytes.end(),
                buffer.Buffer,
                buffer.Buffer + buffer.Length);
        }
    }
    return QUIC_STATUS_SUCCESS;
}

TunnelRequest MakeRequest() {
    TunnelRequest req{};
    req.AddrType = TQ_ADDR_DOMAIN;
    constexpr char host[] = "example.test";
    std::memcpy(req.Host, host, sizeof(host));
    req.Port = 443;
    return req;
}

struct FakeClientOpen {
    QUIC_API_TABLE Api{};
    HQUIC RawConnection{reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7001))};
    std::unique_ptr<MsQuicConnection> Conn;
    TqSocketHandle Fds[2]{TqInvalidSocket, TqInvalidSocket};

    FakeClientOpen() {
        InstallFakeMsQuic(Api);
        Conn.reset(new MsQuicConnection(
            RawConnection,
            CleanUpManual,
            MsQuicConnection::NoOpCallback));
        (void)TqSocketPair(Fds);
    }

    ~FakeClientOpen() {
        if (TqSocketValid(Fds[0])) {
            TqCloseSocket(Fds[0]);
        }
        if (TqSocketValid(Fds[1])) {
            TqCloseSocket(Fds[1]);
        }
        Conn.reset();
    }

    TqSocketHandle ClientFd() const { return Fds[0]; }
    void ReleaseClientFdToTunnel() { Fds[0] = TqInvalidSocket; }
};

} // namespace

uint32_t TqLookupServerConnectionId(MsQuicConnection*) {
    return 0;
}

uint32_t TqLookupClientTraceConnId(MsQuicConnection*) {
    return 0;
}

bool TqTraceEnabled() {
    return false;
}

void TqTraceLogLine(const char*) {
}

uint64_t TqTraceStreamStarted(MsQuicConnection*, uint32_t, const char*, const char*, uint8_t) {
    return 1;
}

void TqTraceIncOpenTx(uint32_t) {
}

void TqTraceIncOpenRx(uint32_t) {
}

void TqTraceRelayStarted(uint64_t, const char*, uint32_t, uint64_t) {
}

void TqTraceRelayStopping(uint64_t, const char*, const char*, const char*, uint32_t, uint64_t, const char*) {
}

void TqTraceRelayFatalError(
    const char*,
    uint32_t,
    const char*,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t) {
}

void TqTraceRelayStreamEvent(
    const char*,
    uint32_t,
    uint64_t,
    const char*,
    uint64_t,
    uint32_t,
    uint64_t,
    uint64_t,
    uint32_t,
    uint32_t,
    bool,
    const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayStopCondition(const char*, uint32_t, const char*, const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayReceiveViewEvent(
    const char*,
    uint32_t,
    const char*,
    uintptr_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    size_t,
    size_t,
    size_t,
    bool,
    bool,
    const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayBackpressureEvent(
    const char*,
    uint32_t,
    uint64_t,
    const char*,
    const char*,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t) {
}

void TqTraceRelayStreamShutdown(const char*, const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayUnregister(const char*, const TqTraceLinuxRelayStreamState&) {
}

void TqTraceOpenResult(uint64_t, bool, TqOpenError, uint32_t) {
}

void TqTraceStreamClosed(uint64_t, const char*, const char*, bool, TqOpenError) {
    ++g_trace_stream_closed_count;
}

void TqTraceProxyClosed(TqTraceProxyProto, TqSocketHandle) {
}

void TqTraceTargetTcpDialing(uint64_t, const char*) {
}

void TqTraceTargetTcpConnected(uint64_t, TqSocketHandle) {
}

void TqTraceTargetTcpFailed(uint64_t, const char*, TqOpenError) {
}

void TqTraceTargetTcpClosed(uint64_t) {
}

bool TqAttachServerSpeedControlStream(
    TqServerSpeedTestController&,
    MsQuicConnection*,
    MsQuicStream*,
    std::vector<uint8_t>,
    std::function<void()>) {
    return false;
}

bool TqAttachServerSpeedControlStreamManaged(
    TqServerSpeedTestController&,
    MsQuicConnection*,
    std::shared_ptr<TqStreamLifetime>,
    std::vector<uint8_t>,
    std::function<void()>) {
    return false;
}

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor*,
    ITqDecompressor*,
    TqRelayHandle* handle,
    const TqTuningConfig&,
    TqCompressAlgo) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }
    if (g_relay_start_should_fail) {
        return false;
    }
    ++g_relay_start_count;
    g_last_relay_fd = tcpFd;
    handle->Backend = TqRelayBackendType::LinuxWorker;
    if (g_capture_relay_receive) {
        stream->Callback = RelayCaptureCallback;
        stream->Context = nullptr;
    }
    return true;
}

namespace {
class RelayCaptureTarget final : public TqStreamLifetime::Target {
public:
    explicit RelayCaptureTarget(std::shared_ptr<TqRelayStopControl> control)
        : Control(std::move(control)) {}
    QUIC_STATUS OnStreamEvent(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        uint64_t) noexcept override {
        if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE && Control != nullptr) {
            Control->Stop.store(true, std::memory_order_release);
        }
        return RelayCaptureCallback(stream, nullptr, event);
    }
    std::shared_ptr<TqRelayStopControl> Control;
};
}

bool TqRelayStartManaged(
    TqSocketHandle tcpFd,
    MsQuicStream*,
    std::shared_ptr<TqStreamLifetime> owner,
    ITqCompressor*,
    ITqDecompressor*,
    TqRelayHandle* handle,
    const TqTuningConfig&,
    TqCompressAlgo,
    bool* tcpFdConsumed) {
    if (handle == nullptr || owner == nullptr ||
        handle->Backend != TqRelayBackendType::None || g_relay_start_should_fail) {
        return false;
    }
    auto target = std::make_shared<RelayCaptureTarget>(handle->Control);
    if (!owner->PublishTarget(owner->RouteGeneration(), std::move(target))) {
        return false;
    }
    ++g_relay_start_count;
    if (tcpFdConsumed != nullptr) {
        *tcpFdConsumed = true;
    }
    g_last_relay_fd = tcpFd;
    handle->Backend = TqRelayBackendType::LinuxWorker;
    return true;
}

bool TqSetServerConnectionClientName(MsQuicConnection*, const std::string&) {
    return false;
}

std::string TqLookupServerConnectionPeerId(MsQuicConnection*) {
    return {};
}

bool TqRelayStartQuicReceiveSink(
    MsQuicStream*,
    TqRelayHandle* handle,
    const TqTuningConfig&,
    std::atomic<uint64_t>*) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }
    if (g_relay_start_should_fail) {
        return false;
    }
    ++g_relay_start_count;
    handle->Backend = TqRelayBackendType::LinuxWorker;
    return true;
}

bool TqRelayStartQuicReceiveSinkManaged(
    MsQuicStream*,
    std::shared_ptr<TqStreamLifetime> owner,
    TqRelayHandle* handle,
    const TqTuningConfig&,
    std::atomic<uint64_t>*) {
    if (handle == nullptr || owner == nullptr ||
        handle->Backend != TqRelayBackendType::None || g_relay_start_should_fail) {
        return false;
    }
    auto target = std::make_shared<RelayCaptureTarget>(handle->Control);
    if (!owner->PublishTarget(owner->RouteGeneration(), std::move(target))) {
        return false;
    }
    ++g_relay_start_count;
    handle->Backend = TqRelayBackendType::LinuxWorker;
    return true;
}

void TqRelaySetTraceContext(TqRelayHandle*, uint64_t, const char*) {
}

void TqRelayStop(TqRelayHandle* handle) {
    if (handle != nullptr) {
        handle->Backend = TqRelayBackendType::None;
    }
}

bool TqRelayLinuxFastPathEnabled(const TqRelayHandle*) {
    return false;
}

namespace {

using StartAsyncSignature = TqClientTunnelOpenHandle* (*)(
    MsQuicConnection*,
    const TunnelRequest&,
    TqSocketHandle,
    const TqConfig&,
    TqClientTunnelOpenComplete);

using StartAsyncWithMetadataSignature = TqClientTunnelOpenHandle* (*)(
    MsQuicConnection*,
    const TunnelRequest&,
    TqSocketHandle,
    const TqConfig&,
    TqClientTunnelOpenComplete,
    TqClientTunnelMetadata);

using CancelSignature = void (*)(TqClientTunnelOpenHandle*);
using AcceptSignature = bool (*)(TqClientTunnelOpenHandle*);
using RejectSignature = void (*)(TqClientTunnelOpenHandle*);

static_assert(
    std::is_same_v<
        decltype(static_cast<StartAsyncSignature>(&TqStartClientTunnelAsync)),
        StartAsyncSignature>,
    "TqStartClientTunnelAsync signature must remain stable");

static_assert(
    std::is_same_v<
        decltype(static_cast<StartAsyncWithMetadataSignature>(&TqStartClientTunnelAsync)),
        StartAsyncWithMetadataSignature>,
    "TqStartClientTunnelAsync metadata overload must be available");

static_assert(
    std::is_same_v<decltype(&TqCancelClientTunnelOpen), CancelSignature>,
    "TqCancelClientTunnelOpen signature must remain stable");

static_assert(
    std::is_same_v<decltype(&TqAcceptClientTunnelOpen), AcceptSignature>,
    "TqAcceptClientTunnelOpen signature must remain stable");

static_assert(
    std::is_same_v<decltype(&TqRejectClientTunnelOpen), RejectSignature>,
    "TqRejectClientTunnelOpen signature must remain stable");

int TestInvalidInputsReturnNullAndDoNotComplete() {
    TunnelRequest req{};
    req.AddrType = TQ_ADDR_DOMAIN;
    req.Host[0] = 'e';
    req.Host[1] = '\0';
    req.Port = 443;

    TqConfig cfg{};
    bool completed = false;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        nullptr,
        req,
        TqInvalidSocket,
        cfg,
        [&completed](TqClientTunnelOpenHandle*, TqTunnelStartResult) {
            completed = true;
        });

    if (handle != nullptr) return 1;
    if (completed) return 2;
    return 0;
}

int TestCancelNullIsSafe() {
    TqCancelClientTunnelOpen(nullptr);
    return 0;
}

int TestOpenSuccessWaitsForExplicitAcceptBeforeRelay() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 10;

    FakeClientOpen fake;
    if (!TqSocketValid(fake.ClientFd())) return 11;

    TqConfig cfg{};
    TqClientTunnelOpenHandle* completedHandle = nullptr;
    TqTunnelStartResult completedResult{};
    unsigned completions = 0;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle* h, TqTunnelStartResult result) {
            completedHandle = h;
            completedResult = result;
            ++completions;
        });
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 12;
    HQUIC stream = LatestFakeStreamHandle();
    if (!DispatchOpenResponse(stream, TqOpenResponse{true, TqOpenError::Ok, 99})) return 13;
    if (completions != 1 || completedHandle != handle || !completedResult.Ok) return 14;
    if (g_relay_start_count != 0) return 15;
    if (!DispatchFakeSendComplete(stream)) return 16;
    if (g_relay_start_count != 0) return 17;
    if (!TqAcceptClientTunnelOpen(handle)) return 18;
    if (g_relay_start_count != 1) return 19;
    if (g_last_relay_fd == TqInvalidSocket) return 20;
    if (!DispatchFakeShutdownComplete(stream)) return 201;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return 0;
}

int TestOpenResponsePayloadIsDeliveredToRelayAfterAccept() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 21;

    FakeClientOpen fake;
    TqConfig cfg{};
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [](TqClientTunnelOpenHandle*, TqTunnelStartResult) {});
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 22;

    HQUIC stream = LatestFakeStreamHandle();
    const std::vector<uint8_t> payload{'h', 'e', 'l', 'l', 'o'};
    if (!DispatchOpenResponseWithPayload(stream, TqOpenResponse{true, TqOpenError::Ok, 99}, payload)) {
        return 23;
    }
    if (!DispatchFakeSendComplete(stream)) return 24;

    g_capture_relay_receive = true;
    if (!TqAcceptClientTunnelOpen(handle)) return 25;
    if (g_relay_receive_bytes != payload) return 26;
    if (!DispatchFakeShutdownComplete(stream)) return 261;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return 0;
}

int TestRejectInsideCompletionDoesNotStartRelay() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 30;

    FakeClientOpen fake;
    TqConfig cfg{};
    unsigned completions = 0;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle* h, TqTunnelStartResult result) {
            if (!result.Ok) return;
            ++completions;
            TqRejectClientTunnelOpen(h);
        });
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 31;
    HQUIC stream = LatestFakeStreamHandle();
    if (!DispatchOpenResponse(stream, TqOpenResponse{true, TqOpenError::Ok, 99})) return 32;
    if (completions != 1) return 33;
    if (g_relay_start_count != 0) return 34;
    if (FakeShutdownCount(stream) == 0) return 35;
    if (!DispatchFakeSendComplete(stream)) return 351;
    if (!DispatchFakeShutdownComplete(stream)) return 352;
    return 0;
}

int TestCancelInsideCompletionDoesNotDoubleDeleteOrStartRelay() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 40;

    FakeClientOpen fake;
    TqConfig cfg{};
    unsigned completions = 0;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle* h, TqTunnelStartResult result) {
            if (!result.Ok) return;
            ++completions;
            TqCancelClientTunnelOpen(h);
        });
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 41;
    HQUIC stream = LatestFakeStreamHandle();
    if (!DispatchOpenResponse(stream, TqOpenResponse{true, TqOpenError::Ok, 99})) return 42;
    if (completions != 1) return 43;
    if (g_relay_start_count != 0) return 44;
    if (FakeShutdownCount(stream) == 0) return 45;
    if (!DispatchFakeSendComplete(stream)) return 451;
    if (!DispatchFakeShutdownComplete(stream)) return 452;
    return 0;
}

int TestFailureResponseCompletesAndCleansUpWithoutRelay() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 50;

    FakeClientOpen fake;
    TqConfig cfg{};
    unsigned completions = 0;
    TqTunnelStartResult completedResult{};
    TqClientTunnelOpenHandle* completedHandle = nullptr;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle* h, TqTunnelStartResult result) {
            completedHandle = h;
            completedResult = result;
            ++completions;
        });
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 51;
    HQUIC stream = LatestFakeStreamHandle();
    if (!DispatchOpenResponse(stream, TqOpenResponse{false, TqOpenError::AclDenied, 0})) return 52;
    if (completions != 1 || completedHandle != handle) return 53;
    if (completedResult.Ok || completedResult.Error != TqOpenError::AclDenied) return 54;
    if (g_relay_start_count != 0) return 55;
    if (FakeShutdownCount(stream) != 0) return 56;
    const char byte = 'x';
    if (TqSend(fake.Fds[1], &byte, 1, TqSendFlags::None) != 1) return 57;
    TqRejectClientTunnelOpen(handle);
    if (FakeShutdownCount(stream) == 0) return 58;
    if (!DispatchFakeSendComplete(stream)) return 581;
    if (!DispatchFakeShutdownComplete(stream)) return 582;
    return 0;
}

int TestSendOpenFailureReturnsNullWithoutTakingClientFd() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 60;

    FakeClientOpen fake;
    if (!TqSocketValid(fake.ClientFd()) || !TqSocketValid(fake.Fds[1])) return 61;

    g_stream_send_should_fail = true;
    TqConfig cfg{};
    bool completed = false;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle*, TqTunnelStartResult) {
            completed = true;
        });
    if (handle != nullptr) return 62;
    if (completed) return 63;

    const char byte = 'x';
    if (TqSend(fake.ClientFd(), &byte, 1, TqSendFlags::None) != 1) return 64;
    char received = 0;
    if (TqRecv(fake.Fds[1], &received, 1, TqRecvFlags::None) != 1) return 65;
    if (received != byte) return 66;
    return 0;
}

int TestAcceptBeforeSendCompleteThenShutdownCleansUpContext() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 70;

    FakeClientOpen fake;
    TqConfig cfg{};
    unsigned completions = 0;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle* h, TqTunnelStartResult result) {
            if (!result.Ok) return;
            ++completions;
            if (!TqAcceptClientTunnelOpen(h)) {
                completions += 100;
            }
        });
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 71;
    HQUIC stream = LatestFakeStreamHandle();
    if (!DispatchOpenResponse(stream, TqOpenResponse{true, TqOpenError::Ok, 99})) return 72;
    if (completions != 1) return 73;
    if (g_relay_start_count != 0) return 74;
    if (!DispatchFakeShutdownComplete(stream)) return 75;
    if (g_trace_stream_closed_count == 0) return 76;
    if (FakeCloseCount(stream) != 0) return 77;
    if (!DispatchFakeSendComplete(stream)) return 78;
    if (FakeCloseCount(stream) == 0) return 79;
    return 0;
}

int TestAcceptReturnsFalseAndCleansUpWhenRelayStartFails() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 80;

    FakeClientOpen fake;
    TqConfig cfg{};
    TqClientTunnelOpenHandle* completedHandle = nullptr;
    TqClientTunnelOpenHandle* handle = TqStartClientTunnelAsync(
        fake.Conn.get(),
        MakeRequest(),
        fake.ClientFd(),
        cfg,
        [&](TqClientTunnelOpenHandle* h, TqTunnelStartResult result) {
            if (result.Ok) {
                completedHandle = h;
            }
        });
    fake.ReleaseClientFdToTunnel();
    if (handle == nullptr) return 81;
    HQUIC stream = LatestFakeStreamHandle();
    if (!DispatchOpenResponse(stream, TqOpenResponse{true, TqOpenError::Ok, 99})) return 82;
    if (completedHandle != handle) return 83;
    if (!DispatchFakeSendComplete(stream)) return 84;
    g_relay_start_should_fail = true;
    if (TqAcceptClientTunnelOpen(handle)) return 85;
    if (g_relay_start_count != 0) return 86;
    if (FakeShutdownCount(stream) == 0) return 87;
    if (!DispatchFakeShutdownComplete(stream)) return 88;
    if (g_trace_stream_closed_count == 0) return 89;
    return 0;
}

} // namespace

int main() {
    struct ReaperGuard {
        ReaperGuard() { TqTunnelReaper::Instance().Start(); }
        ~ReaperGuard() { TqTunnelReaper::Instance().Stop(); }
    } reaperGuard;
    if (int rc = TestInvalidInputsReturnNullAndDoNotComplete()) return rc;
    if (int rc = TestCancelNullIsSafe()) return rc;
    if (int rc = TestOpenSuccessWaitsForExplicitAcceptBeforeRelay()) return rc;
    if (int rc = TestOpenResponsePayloadIsDeliveredToRelayAfterAccept()) return rc;
    if (int rc = TestRejectInsideCompletionDoesNotStartRelay()) return rc;
    if (int rc = TestCancelInsideCompletionDoesNotDoubleDeleteOrStartRelay()) return rc;
    if (int rc = TestFailureResponseCompletesAndCleansUpWithoutRelay()) return rc;
    if (int rc = TestSendOpenFailureReturnsNullWithoutTakingClientFd()) return rc;
    if (int rc = TestAcceptBeforeSendCompleteThenShutdownCleansUpContext()) return rc;
    if (int rc = TestAcceptReturnsFalseAndCleansUpWhenRelayStartFails()) return rc;
    return 0;
}
