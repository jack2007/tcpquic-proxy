#pragma once

#define QUIC_API_ENABLE_PREVIEW_FEATURES 1

#include "config.h"
#include "msquic.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
#include "quic_credentials_win.h"
#endif

// Server-side: assign stable conn_id per accepted QUIC connection (OPEN_OK field).
uint32_t TqRegisterServerConnection(MsQuicConnection* connection);
uint32_t TqLookupServerConnectionId(MsQuicConnection* connection);
void TqUnregisterServerConnection(MsQuicConnection* connection);
uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection);
MsQuicSettings TqMakeMsQuicSettings(const TqConfig& cfg, bool server);

class QuicClientSession {
public:
    using StreamHandler = std::function<void(MsQuicConnection*, HQUIC)>;
    using ConnectionStateHandler = std::function<void(uint32_t connectedCount)>;

    ~QuicClientSession();

    bool Start(const TqConfig& cfg);
    void Stop();
    MsQuicConnection* GetConnection();
    MsQuicConnection* PickConnection();
    MsQuicConnection* PickConnectionAt(size_t index);
    MsQuicConnection* PickConnectionFrom(size_t firstIndex);
    bool EnsureConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    bool EnsureAnyConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void SetPeerStreamHandler(StreamHandler h);
    void SetConnectionStateHandler(ConnectionStateHandler h);
    void AbortAllTunnels();
    uint32_t ConnectionCount() const;
    uint32_t ConnectedConnectionCount() const;

private:
    struct ClientSharedState;

    struct ClientConnContext {
        std::shared_ptr<ClientSharedState> State;
        size_t SlotIndex{0};
    };

    struct ConnectionSlot {
        ClientConnContext* Context{nullptr};
        std::unique_ptr<MsQuicConnection> Connection;
        bool Connected{false};
        bool ReconnectNeeded{false};
    };

    struct ConnectionStateNotification {
        ConnectionStateHandler Handler;
        uint32_t ConnectedCount{0};
    };

    struct ClientReconnectGate {
        std::mutex Lock;
        QuicClientSession* Session{nullptr};
    };

    static QUIC_STATUS QUIC_API ConnectionCallback(
        MsQuicConnection* connection,
        void* context,
        QUIC_CONNECTION_EVENT* event) noexcept;

    struct ClientSharedState {
        bool Started{false};
        bool Stopping{false};
        std::vector<ConnectionSlot> Slots;
        std::vector<std::unique_ptr<MsQuicConnection>> OrphanedConnections;
        std::mutex Lock;
        std::condition_variable StateChanged;
        std::condition_variable OrphanDrained;
        StreamHandler PeerStreamHandler;
        ConnectionStateHandler ConnectionStateChanged;
        std::thread ReconnectThread;
        std::chrono::milliseconds ReconnectInterval{std::chrono::milliseconds(3000)};
        std::shared_ptr<ClientReconnectGate> ReconnectGate;
        std::shared_ptr<MsQuicApi> Api;
    };

    void Stop(bool clearHandlers);
    bool StartSlotLocked(size_t index);
    void StartReconnectLoop();
    void StopReconnectLoop(const std::shared_ptr<ClientSharedState>& state);
    static void RunReconnectLoop(
        std::shared_ptr<ClientSharedState> state,
        std::shared_ptr<ClientReconnectGate> gate);
    static uint32_t ConnectedCountLocked(const ClientSharedState& state);
    static void NotifyConnectionStateChanged(ConnectionStateNotification notification);
    static ConnectionStateNotification OnSlotConnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection);
    static ConnectionStateNotification OnSlotDisconnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection);
    static void DropOrphanedConnection(const std::shared_ptr<ClientSharedState>& state, MsQuicConnection* connection);
    static void WaitForOrphanedConnectionsDrain(const std::shared_ptr<ClientSharedState>& state, std::chrono::milliseconds timeout);

    TqConfig Config;
    std::string PeerHost;
    uint16_t PeerPort{0};
    std::atomic<size_t> PickIndex{0};
    std::shared_ptr<ClientSharedState> State{std::make_shared<ClientSharedState>()};
    std::shared_ptr<MsQuicApi> Api;
    std::unique_ptr<MsQuicRegistration> Registration;
    std::unique_ptr<MsQuicConfiguration> Configuration;
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
    std::unique_ptr<TqQuicCredentialHolder> Credentials;
#endif
};

class QuicServerSession {
public:
    using StreamHandler = std::function<void(MsQuicConnection*, HQUIC)>;
    using ConnectionHandler = std::function<void(MsQuicConnection*)>;

    bool Start(const TqConfig& cfg);
    void Stop();
    void Run();
    void SetPeerStreamHandler(StreamHandler h);
    void SetConnectionHandler(ConnectionHandler h);

private:
    static QUIC_STATUS QUIC_API ListenerCallback(
        MsQuicListener* listener,
        void* context,
        QUIC_LISTENER_EVENT* event) noexcept;
    static QUIC_STATUS QUIC_API ConnectionCallback(
        MsQuicConnection* connection,
        void* context,
        QUIC_CONNECTION_EVENT* event) noexcept;

    TqConfig Config;
    bool Started{false};
    bool Stopping{false};
    std::mutex Lock;
    std::condition_variable StateChanged;
    StreamHandler PeerStreamHandler;
    ConnectionHandler PeerConnectionHandler;
    std::shared_ptr<MsQuicApi> Api;
    std::unique_ptr<MsQuicRegistration> Registration;
    std::unique_ptr<MsQuicConfiguration> Configuration;
    std::unique_ptr<MsQuicListener> Listener;
#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
    std::unique_ptr<TqQuicCredentialHolder> Credentials;
#endif
};
