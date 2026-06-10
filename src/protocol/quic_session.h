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
#include <vector>

#if defined(_WIN32) && !defined(TCPQUIC_WINDOWS_TLS_QUICTLS)
#include "quic_credentials_win.h"
#endif

// Server-side: assign stable conn_id per accepted QUIC connection (OPEN_OK field).
uint32_t TqRegisterServerConnection(MsQuicConnection* connection);
uint32_t TqLookupServerConnectionId(MsQuicConnection* connection);
void TqUnregisterServerConnection(MsQuicConnection* connection);

class QuicClientSession {
public:
    using StreamHandler = std::function<void(MsQuicConnection*, HQUIC)>;

    bool Start(const TqConfig& cfg);
    void Stop();
    MsQuicConnection* GetConnection();
    MsQuicConnection* PickConnection();
    bool EnsureConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void SetPeerStreamHandler(StreamHandler h);
    uint32_t ConnectionCount() const;

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
        std::shared_ptr<MsQuicApi> Api;
    };

    bool StartSlotLocked(size_t index);
    static void OnSlotConnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection);
    static void OnSlotDisconnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection);
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
