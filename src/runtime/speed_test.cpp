#include "speed_test.h"

#include "platform_socket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using TqClock = std::chrono::steady_clock;
#if defined(_WIN32)
using TqSockLen = int;
#else
using TqSockLen = socklen_t;
#endif

bool TqIsValidDirection(TqSpeedDirection direction) {
    return direction == TqSpeedDirection::Download || direction == TqSpeedDirection::Upload;
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
    }
    session->ClosedConnections.fetch_add(1, std::memory_order_relaxed);
}

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
        break;
    }

    TqCloseConnectionSocket(session, connection);
}

void TqRunDownloadWorker(
    const std::shared_ptr<TqSpeedSession>& session,
    const std::shared_ptr<TqSpeedSession::Connection>& connection) {
    std::array<uint8_t, 64 * 1024> buffer{};
    buffer.fill(0x53);
    TqSocketHandle socket = TqInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(connection->Mutex);
        socket = connection->Socket;
    }

    while (!session->Stopping.load(std::memory_order_relaxed)) {
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
        break;
    }

    TqCloseConnectionSocket(session, connection);
}

void TqRunAcceptLoop(const std::shared_ptr<TqSpeedSession>& session) {
    while (!session->Stopping.load(std::memory_order_relaxed)) {
        sockaddr_in addr{};
        TqSockLen addrLen = static_cast<TqSockLen>(sizeof(addr));
        TqSocketHandle accepted = ::accept(session->Listener, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (!TqSocketValid(accepted)) {
            const int error = TqLastSocketError();
            if (!session->Stopping.load(std::memory_order_relaxed) && TqSocketInterrupted(error)) {
                continue;
            }
            break;
        }

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
    {
        std::lock_guard<std::mutex> lock(session->Mutex);
        session->Connections.clear();
    }
}

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
    return true;
}

bool TqServerSpeedTestController::FinishSession(
    uint32_t sessionId, uint64_t, uint64_t, TqSpeedResult& result) {
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
