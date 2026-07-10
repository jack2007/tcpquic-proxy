#include "speed_test.h"

#include "config.h"
#include "platform_socket.h"
#include "trace.h"
#include "tuning.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

bool TqSetServerConnectionClientName(MsQuicConnection*, const std::string&) {
    return false;
}

std::string TqLookupServerConnectionPeerId(MsQuicConnection*) {
    return {};
}

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

void TqTraceLogLine(const char*) {
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

void TqTraceRelayStarted(uint64_t tunnelId, const char* backend, uint32_t workerIndex, uint64_t relayId) {
    (void)tunnelId;
    (void)backend;
    (void)workerIndex;
    (void)relayId;
}

void TqTraceRelayStopping(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* reason) {
    (void)tunnelId;
    (void)role;
    (void)target;
    (void)backend;
    (void)workerIndex;
    (void)relayId;
    (void)reason;
}

void TqTraceRelayFatalError(
    const char* backend,
    uint32_t workerIndex,
    const char* reason,
    uint64_t relayId,
    uint64_t socketOrFd,
    uint64_t pendingQuicReceiveBytes,
    uint64_t pendingQuicReceiveQueue,
    uint64_t pendingQuicSends,
    uint64_t inflightQuicSends,
    uint64_t inflightTcpSends) {
    (void)backend;
    (void)workerIndex;
    (void)reason;
    (void)relayId;
    (void)socketOrFd;
    (void)pendingQuicReceiveBytes;
    (void)pendingQuicReceiveQueue;
    (void)pendingQuicSends;
    (void)inflightQuicSends;
    (void)inflightTcpSends;
}

void TqTraceRelayStreamEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)workerIndex;
    (void)relayId;
    (void)streamEvent;
    (void)errorCode;
    (void)status;
    (void)absoluteOffset;
    (void)totalBufferLength;
    (void)bufferCount;
    (void)receiveFlags;
    (void)fin;
    (void)state;
}

void TqTraceRelayStopCondition(
    const char* backend,
    uint32_t workerIndex,
    const char* trigger,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)workerIndex;
    (void)trigger;
    (void)state;
}

void TqTraceRelayReceiveViewEvent(
    const char* backend,
    uint32_t workerIndex,
    const char* stage,
    uintptr_t viewId,
    uint64_t value,
    uint64_t totalLength,
    uint64_t completedLength,
    uint64_t accountedLength,
    uint64_t pendingCompleteBytes,
    size_t sliceIndex,
    size_t sliceCount,
    size_t sliceOffset,
    bool fin,
    bool drained,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)workerIndex;
    (void)stage;
    (void)viewId;
    (void)value;
    (void)totalLength;
    (void)completedLength;
    (void)accountedLength;
    (void)pendingCompleteBytes;
    (void)sliceIndex;
    (void)sliceCount;
    (void)sliceOffset;
    (void)fin;
    (void)drained;
    (void)state;
}

void TqTraceRelayBackpressureEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* action,
    const char* reason,
    uint64_t outstandingQuicSendBytes,
    uint64_t pauseThreshold,
    uint64_t resumeThreshold,
    uint64_t readAheadBytes) {
    (void)backend;
    (void)workerIndex;
    (void)relayId;
    (void)action;
    (void)reason;
    (void)outstandingQuicSendBytes;
    (void)pauseThreshold;
    (void)resumeThreshold;
    (void)readAheadBytes;
}

void TqTraceRelayStreamShutdown(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)state;
}

void TqTraceRelayUnregister(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)state;
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

void TqTraceTargetTcpFailed(uint64_t tunnelId, const char* target, TqOpenError error) {
    (void)tunnelId;
    (void)target;
    (void)error;
}

void TqTraceTargetTcpClosed(uint64_t tunnelId) {
    (void)tunnelId;
}

namespace {

bool ConnectLoopback(uint16_t port, TqSocketHandle& outSocket) {
    outSocket = TqInvalidSocket;
    TqSocketHandle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(sock)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!TqInetPton(AF_INET, "127.0.0.1", &addr.sin_addr)) {
        TqCloseSocket(sock);
        return false;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        TqCloseSocket(sock);
        return false;
    }

    outSocket = sock;
    return true;
}

bool CreateLoopbackListener(TqSocketHandle& outListener, uint16_t& outPort) {
    outListener = TqInvalidSocket;
    outPort = 0;
    TqSocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(listener)) {
        return false;
    }

    (void)TqSetReuseAddr(listener);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (!TqInetPton(AF_INET, "127.0.0.1", &addr.sin_addr)) {
        TqCloseSocket(listener);
        return false;
    }
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0) {
        TqCloseSocket(listener);
        return false;
    }

    socklen_t addrLen = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        TqCloseSocket(listener);
        return false;
    }
    outListener = listener;
    outPort = ntohs(addr.sin_port);
    return outPort != 0;
}

std::thread StartFakeHttpConnectServer(
    TqSocketHandle listener,
    std::string* requestOut,
    std::atomic<bool>* acceptedOut,
    std::string response) {
    return std::thread([listener, requestOut, acceptedOut, response = std::move(response)]() {
        sockaddr_in addr{};
        socklen_t addrLen = sizeof(addr);
        TqSocketHandle accepted = ::accept(
            listener,
            reinterpret_cast<sockaddr*>(&addr),
            &addrLen);
        if (!TqSocketValid(accepted)) {
            return;
        }
        acceptedOut->store(true, std::memory_order_release);
        std::array<uint8_t, 1024> buffer{};
        for (;;) {
            const int rc = TqRecv(accepted, buffer.data(), buffer.size(), TqRecvFlags::None);
            if (rc <= 0) {
                break;
            }
            requestOut->append(
                reinterpret_cast<const char*>(buffer.data()),
                static_cast<size_t>(rc));
            if (requestOut->find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }
        (void)TqSend(accepted, response.data(), response.size(), TqSendFlags::NoSignal);
        TqCloseSocket(accepted);
    });
}

int TestStartAndAuthorization() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 1;
    start.Direction = TqSpeedDirection::Upload;
    start.DurationSec = 2;
    start.Parallel = 2;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 1;
    }
    if (ready.SessionId != 1) {
        return 2;
    }
    if (ready.AddrType != TQ_ADDR_IPV4) {
        return 3;
    }
    if (ready.Addr.size() != 4) {
        return 4;
    }
    if (ready.Addr[0] != 127 || ready.Addr[1] != 0 || ready.Addr[2] != 0 || ready.Addr[3] != 1) {
        return 5;
    }
    if (ready.Port == 0) {
        return 6;
    }
    if (!controller.IsAllowedEphemeralTarget("127.0.0.1", ready.Port)) {
        return 7;
    }

    TqSpeedResult result{};
    if (!controller.FinishSession(1, 11, 22, result)) {
        return 8;
    }
    if (controller.IsAllowedEphemeralTarget("127.0.0.1", ready.Port)) {
        return 9;
    }
    return 0;
}

int TestUploadPath() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 2;
    start.Direction = TqSpeedDirection::Upload;
    start.DurationSec = 2;
    start.Parallel = 1;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 10;
    }

    TqSocketHandle client = TqInvalidSocket;
    if (!ConnectLoopback(ready.Port, client)) {
        controller.StopAll();
        return 11;
    }

    std::vector<uint8_t> payload(1024 * 1024, 0x5a);
    size_t sent = 0;
    while (sent < payload.size()) {
        const int rc = TqSend(client, payload.data() + sent, payload.size() - sent, TqSendFlags::NoSignal);
        if (rc <= 0) {
            TqCloseSocket(client);
            controller.StopAll();
            return 12;
        }
        sent += static_cast<size_t>(rc);
    }
    (void)TqShutdownSend(client);
    TqCloseSocket(client);

    TqSpeedResult result{};
    if (!controller.FinishSession(2, payload.size(), 12345, result)) {
        return 13;
    }
    if (result.ServerBytes != payload.size()) {
        return 14;
    }
    if (result.AcceptedConnections != 1) {
        return 15;
    }
    if (result.ClosedConnections != 1) {
        return 16;
    }
    return 0;
}

int TestDownloadPath() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 3;
    start.Direction = TqSpeedDirection::Download;
    start.DurationSec = 2;
    start.Parallel = 1;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 20;
    }

    TqSocketHandle client = TqInvalidSocket;
    if (!ConnectLoopback(ready.Port, client)) {
        controller.StopAll();
        return 21;
    }

    std::array<uint8_t, 4096> buffer{};
    const int received = TqRecv(client, buffer.data(), buffer.size(), TqRecvFlags::None);
    if (received <= 0) {
        TqCloseSocket(client);
        controller.StopAll();
        return 22;
    }

    TqSpeedResult result{};
    auto finishFuture = std::async(std::launch::async, [&controller, &result, received]() {
        if (!controller.FinishSession(3, static_cast<uint64_t>(received), 6789, result)) {
            return 23;
        }
        return 0;
    });

    uint64_t totalReceived = static_cast<uint64_t>(received);
    for (;;) {
        const int rc = TqRecv(client, buffer.data(), buffer.size(), TqRecvFlags::None);
        if (rc > 0) {
            totalReceived += static_cast<uint64_t>(rc);
            continue;
        }
        if (rc == 0) {
            break;
        }
        TqCloseSocket(client);
        controller.StopAll();
        return 24;
    }
    TqCloseSocket(client);

    if (finishFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        controller.StopAll();
        return 25;
    }
    if (const int finishRc = finishFuture.get(); finishRc != 0) {
        return finishRc;
    }
    if (result.ServerBytes != totalReceived) {
        return 26;
    }
    if (result.AcceptedConnections != 1) {
        return 27;
    }
    if (result.ClosedConnections != 1) {
        return 28;
    }
    return 0;
}

int TestUploadFinishWithOpenClient() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 4;
    start.Direction = TqSpeedDirection::Upload;
    start.DurationSec = 2;
    start.Parallel = 1;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 30;
    }

    TqSocketHandle client = TqInvalidSocket;
    if (!ConnectLoopback(ready.Port, client)) {
        controller.StopAll();
        return 31;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto finishFuture = std::async(std::launch::async, [&controller]() {
        TqSpeedResult result{};
        if (!controller.FinishSession(4, 0, 0, result)) {
            return 34;
        }
        if (result.AcceptedConnections != 1) {
            return 35;
        }
        if (result.ClosedConnections != 1) {
            return 36;
        }
        return 0;
    });

    if (finishFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        TqCloseSocket(client);
        return 32;
    }

    TqCloseSocket(client);
    const int finishRc = finishFuture.get();
    if (finishRc != 0) {
        return finishRc;
    }
    return 0;
}

int TestIngressSpeedTestConnectionRejectsEmptyHttpListen() {
    TqConfig cfg{};
    cfg.HttpListen.clear();

    TqSpeedReady ready{};
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = 12345;
    ready.Addr = {127, 0, 0, 1};
    TqSocketHandle socket = TqInvalidSocket;
    std::vector<uint8_t> leftover;
    std::string err;
    if (TqOpenIngressSpeedTestConnection(cfg, ready, socket, leftover, err)) {
        TqCloseSocket(socket);
        return 40;
    }
    if (err.find("http listen") == std::string::npos) {
        return 41;
    }
    return 0;
}

int TestIngressSpeedTestConnectionSendsConnectRequestAndFailsOnNon200() {
    TqSocketHandle listener = TqInvalidSocket;
    uint16_t listenPort = 0;
    if (!CreateLoopbackListener(listener, listenPort)) {
        return 42;
    }

    std::string request;
    std::atomic<bool> accepted{false};
    std::thread server = StartFakeHttpConnectServer(
        listener,
        &request,
        &accepted,
        "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");

    TqConfig cfg{};
    cfg.HttpListen = "127.0.0.1:" + std::to_string(listenPort);
    cfg.Router.ProxyAuth.push_back(TqProxyAuthUser{"alice", "secret"});
    TqSpeedReady ready{};
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = 23456;
    ready.Addr = {127, 0, 0, 1};

    TqSocketHandle socket = TqInvalidSocket;
    std::vector<uint8_t> leftover;
    std::string err;
    const bool ok = TqOpenIngressSpeedTestConnection(cfg, ready, socket, leftover, err);
    if (TqSocketValid(socket)) {
        TqCloseSocket(socket);
    }
    TqCloseSocket(listener);
    if (server.joinable()) {
        server.join();
    }
    if (ok) {
        return 43;
    }
    if (!accepted.load(std::memory_order_acquire)) {
        return 44;
    }
    if (request.find("CONNECT 127.0.0.1:23456 HTTP/1.1\r\n") == std::string::npos) {
        return 45;
    }
    if (request.find("Host: 127.0.0.1:23456\r\n") == std::string::npos) {
        return 46;
    }
    if (request.find("Proxy-Authorization: Basic YWxpY2U6c2VjcmV0\r\n") == std::string::npos) {
        return 47;
    }
    if (err.find("HTTP CONNECT") == std::string::npos) {
        return 48;
    }
    return 0;
}

int TestIngressSpeedTestConnectionPreservesSuccessLeftover() {
    TqSocketHandle listener = TqInvalidSocket;
    uint16_t listenPort = 0;
    if (!CreateLoopbackListener(listener, listenPort)) {
        return 49;
    }

    std::string request;
    std::atomic<bool> accepted{false};
    std::thread server = StartFakeHttpConnectServer(
        listener,
        &request,
        &accepted,
        "HTTP/1.1 200 Connection Established\r\n\r\npayload");

    TqConfig cfg{};
    cfg.HttpListen = "localhost:" + std::to_string(listenPort);
    TqSpeedReady ready{};
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = 23457;
    ready.Addr = {127, 0, 0, 1};

    TqSocketHandle socket = TqInvalidSocket;
    std::vector<uint8_t> leftover;
    std::string err;
    const bool ok = TqOpenIngressSpeedTestConnection(cfg, ready, socket, leftover, err);
    if (TqSocketValid(socket)) {
        TqCloseSocket(socket);
    }
    TqCloseSocket(listener);
    if (server.joinable()) {
        server.join();
    }
    if (!ok) {
        return 53;
    }
    const std::string payload(leftover.begin(), leftover.end());
    if (payload != "payload") {
        return 54;
    }
    return 0;
}

int TestIngressSpeedTestConnectionRejectsNonLoopbackListenHost() {
    TqConfig cfg{};
    cfg.HttpListen = "192.0.2.1:8080";
    TqSpeedReady ready{};
    ready.AddrType = TQ_ADDR_IPV4;
    ready.Port = 23458;
    ready.Addr = {127, 0, 0, 1};

    TqSocketHandle socket = TqInvalidSocket;
    std::vector<uint8_t> leftover;
    std::string err;
    if (TqOpenIngressSpeedTestConnection(cfg, ready, socket, leftover, err)) {
        TqCloseSocket(socket);
        return 55;
    }
    if (err.find("loopback") == std::string::npos) {
        return 56;
    }
    return 0;
}

int TestSpeedLocalSocketUsesActiveThroughputBuffer() {
    const int requestedBuffer = 4 * 1024 * 1024;
    TqSetActiveTcpSocketBuffer(requestedBuffer);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        return 50;
    }

    TqTuneSpeedTestLocalSocket(pair[0]);

    const int sendBuffer = TqGetSocketBuffer(pair[0], SO_SNDBUF);
    const int receiveBuffer = TqGetSocketBuffer(pair[0], SO_RCVBUF);
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);

    if (sendBuffer < requestedBuffer) {
        return 51;
    }
    if (receiveBuffer < requestedBuffer) {
        return 52;
    }
    return 0;
}

int TestSpeedByteMismatchLimitAllowsConfiguredSocketBuffer() {
    const uint64_t requestedBuffer = 64ull * 1024ull * 1024ull;
    const uint64_t expectedLimit = (2ull * requestedBuffer) + (16ull * 1024ull * 1024ull);
    TqSetActiveTcpSocketBuffer(static_cast<int>(requestedBuffer));

    const uint64_t localBytes = 60ull * 1024ull * 1024ull * 1024ull;
    uint64_t diff = 0;
    uint64_t limit = 0;
    if (!TqSpeedByteCountsCloseEnough(localBytes, localBytes - expectedLimit + 1, diff, limit)) {
        return 60;
    }
    if (limit != expectedLimit) {
        return 61;
    }
    if (TqSpeedByteMismatchLimit(4ull * 1024ull * 1024ull * 1024ull) != expectedLimit) {
        return 63;
    }
    if (TqSpeedByteCountsCloseEnough(localBytes, localBytes - expectedLimit - 1, diff, limit)) {
        return 62;
    }
    return 0;
}

int TestSpeedConnectWaitIgnoresTransientPollMissBeforeDeadline() {
    if (TqSpeedConnectWaitShouldStop(0, 2, false)) {
        return 70;
    }
    if (!TqSpeedConnectWaitShouldStop(1, 2, true)) {
        return 71;
    }
    if (!TqSpeedConnectWaitShouldStop(2, 2, false)) {
        return 72;
    }
    return 0;
}

} // namespace

int main() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 100;
    }
    if (const int rc = TestStartAndAuthorization(); rc != 0) {
        return rc;
    }
    if (const int rc = TestUploadPath(); rc != 0) {
        return rc;
    }
    if (const int rc = TestDownloadPath(); rc != 0) {
        return rc;
    }
    if (const int rc = TestUploadFinishWithOpenClient(); rc != 0) {
        return rc;
    }
    if (const int rc = TestIngressSpeedTestConnectionRejectsEmptyHttpListen(); rc != 0) {
        return rc;
    }
    if (const int rc = TestIngressSpeedTestConnectionSendsConnectRequestAndFailsOnNon200(); rc != 0) {
        return rc;
    }
    if (const int rc = TestIngressSpeedTestConnectionPreservesSuccessLeftover(); rc != 0) {
        return rc;
    }
    if (const int rc = TestIngressSpeedTestConnectionRejectsNonLoopbackListenHost(); rc != 0) {
        return rc;
    }
    if (const int rc = TestSpeedLocalSocketUsesActiveThroughputBuffer(); rc != 0) {
        return rc;
    }
    if (const int rc = TestSpeedByteMismatchLimitAllowsConfiguredSocketBuffer(); rc != 0) {
        return rc;
    }
    if (const int rc = TestSpeedConnectWaitIgnoresTransientPollMissBeforeDeadline(); rc != 0) {
        return rc;
    }
    return 0;
}
