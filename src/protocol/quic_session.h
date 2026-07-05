#pragma once

#define QUIC_API_ENABLE_INSECURE_FEATURES 1
#define QUIC_API_ENABLE_PREVIEW_FEATURES 1

#include "config.h"
#include "msquic.hpp"
#include "quic_address.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Server-side: assign stable conn_id per accepted QUIC connection (OPEN_OK field).
uint32_t TqRegisterServerConnection(MsQuicConnection* connection);
uint32_t TqLookupServerConnectionId(MsQuicConnection* connection);
bool TqSetServerConnectionClientName(MsQuicConnection* connection, const std::string& clientName);
void TqUnregisterServerConnection(MsQuicConnection* connection);
uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection);
MsQuicSettings TqMakeMsQuicSettings(const TqConfig& cfg, bool server);

struct TqConnectionSnapshot {
    std::string ConnectionId;
    uint32_t SlotIndex{0};
    uint64_t Generation{0};
    bool Connected{false};
    bool RetryScheduled{false};
    std::string State;
    uint64_t ActiveTunnels{0};
    uint64_t TotalTunnels{0};
    std::string LastError;
    std::string PathName;
    std::string LocalAddress;
    std::string PeerAddress;
};

struct TqServerConnectionSnapshot {
    std::string ConnectionId;
    std::string ClientName;
    std::string RemoteAddress;
    std::string State;
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    uint64_t ActiveTunnels{0};
    std::string LastError;
};

struct TqClientPickedConnection {
    MsQuicConnection* Connection{nullptr};
    std::string ConnectionId;
};

std::vector<TqServerConnectionSnapshot> TqSnapshotServerConnections();
bool TqGetServerConnectionSnapshot(const std::string& connectionId, TqServerConnectionSnapshot& out);
bool TqAbortServerConnectionTunnels(const std::string& connectionId);
void TqServerConnectionStreamStarted(MsQuicConnection* connection);
void TqServerConnectionStreamFinished(MsQuicConnection* connection);
void TqServerConnectionStreamFinishedById(uint32_t connectionId);

#if defined(TQ_UNIT_TESTING)
uint32_t TqRegisterServerConnectionForTest(HQUIC handle, MsQuicConnection* connection = nullptr);
bool TqSetServerConnectionClientNameForTest(HQUIC handle, const std::string& clientName);
void TqUnregisterServerConnectionForTest(HQUIC handle);

struct TqCredentialConfigSnapshot {
    QUIC_CREDENTIAL_TYPE Type{};
    QUIC_CREDENTIAL_FLAGS Flags{};
    bool HasCertificateFile{false};
    std::string CaCertificateFile;
};

TqCredentialConfigSnapshot TqBuildCredentialConfigSnapshotForTest(const TqConfig& cfg, bool server);
#endif

class QuicClientSession {
public:
    using StreamHandler = std::function<void(MsQuicConnection*, HQUIC)>;
    using ConnectionStateHandler = std::function<void(uint32_t connectedCount)>;
    using DelayedTaskScheduler =
        std::function<bool(std::chrono::milliseconds delay, std::function<void()> task)>;

    ~QuicClientSession();

    bool Start(const TqConfig& cfg);
    void Stop();
    MsQuicConnection* GetConnection();
    MsQuicConnection* PickConnection();
    TqClientPickedConnection PickConnectionWithId();
    MsQuicConnection* PickConnectionAt(size_t index);
    MsQuicConnection* PickConnectionFrom(size_t firstIndex);
    bool EnsureConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    bool EnsureAnyConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void SetPeerStreamHandler(StreamHandler h);
    void SetConnectionStateHandler(ConnectionStateHandler h);
    void SetDelayedTaskScheduler(DelayedTaskScheduler scheduler);
    void AbortAllTunnels();
    std::vector<TqConnectionSnapshot> SnapshotConnections() const;
    bool SetDesiredConnectionCount(uint32_t desired, std::string& err);
    bool StopHighestConnection(const std::string& connectionId, std::string& err);
    bool ReconnectConnection(const std::string& connectionId, std::string& err);
    bool AbortConnectionTunnels(const std::string& connectionId, std::string& err);
    uint32_t ConnectionCount() const;
    uint32_t ConnectedConnectionCount() const;

#if defined(TQ_UNIT_TESTING)
    struct ReconnectTestHooks {
        std::function<bool(size_t index)> StartSlotOverride;
        std::function<void(size_t index, const TqClientSlotPath& path)> StartSlotPathObserver;
        std::function<void(size_t index)> BeforePublishSlot;
        std::function<QUIC_STATUS(size_t index)> ConnectionStartOverride;
        std::function<void(size_t index, uint64_t generation)> ContextDeleted;
    };
    void SetReconnectTestHooks(ReconnectTestHooks hooks);
    void MarkReconnectStartedForTest(size_t slots);
    void MarkReconnectStartedForTest(size_t slots, const TqConfig& cfg);
    void MarkSlotConnectedForTest(size_t index, MsQuicConnection* connection);
    void MarkSlotDisconnectedForTest(size_t index);
    MsQuicConnection* PickConnectionForTest();
    void ScheduleStartRetryForTest(size_t index);
    void RestartSlotAfterShutdownCompleteForTest(size_t index, uint64_t generation);
    static bool ConnectionStartAcceptedForTest(QUIC_STATUS status);
#endif

private:
    struct ClientSharedState;
    struct ClientSessionGate;

    struct ClientConnContext {
        std::shared_ptr<ClientSessionGate> Gate;
        std::shared_ptr<ClientSharedState> State;
        size_t SlotIndex{0};
        uint64_t Generation{0};
        ~ClientConnContext();
    };

    struct ConnectionSlot {
        ClientConnContext* Context{nullptr};
        std::unique_ptr<MsQuicConnection> Connection;
        std::string ConnectionId;
        uint64_t Generation{0};
        bool Connected{false};
        bool RetryScheduled{false};
        std::string LastError;
#if defined(TQ_UNIT_TESTING)
        MsQuicConnection* TestConnectionOverride{nullptr};
#endif
    };

    struct ConnectionStateNotification {
        ConnectionStateHandler Handler;
        uint32_t ConnectedCount{0};
    };

    struct ClientSessionGate {
        std::mutex Lock;
        std::condition_variable Drained;
        QuicClientSession* Session{nullptr};
        size_t ActiveCalls{0};
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
        DelayedTaskScheduler Scheduler;
        std::shared_ptr<ClientSessionGate> SessionGate;
        std::shared_ptr<MsQuicApi> Api;
#if defined(TQ_UNIT_TESTING)
        ReconnectTestHooks TestHooks;
#endif
    };

    void Stop(bool clearHandlers);
    bool StartSlot(size_t index);
    void StartAllSlots();
    void ScheduleStartRetry(size_t index);
    void RestartSlotAfterShutdownComplete(
        const std::shared_ptr<ClientSharedState>& state,
        size_t slotIndex,
        uint64_t generation);
    static QuicClientSession* AcquireLiveSession(
        const std::shared_ptr<ClientSessionGate>& gate);
    static void ReleaseLiveSession(const std::shared_ptr<ClientSessionGate>& gate);
    static MsQuicConnection* PickableConnectionLocked(const ConnectionSlot& slot);
    static uint32_t ConnectedCountLocked(const ClientSharedState& state);
    static void NotifyConnectionStateChanged(ConnectionStateNotification notification);
    static ConnectionStateNotification OnSlotConnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection);
    static ConnectionStateNotification OnSlotDisconnected(const std::shared_ptr<ClientSharedState>& state, size_t index, MsQuicConnection* connection);
    static void DropOrphanedConnection(const std::shared_ptr<ClientSharedState>& state, MsQuicConnection* connection);
    static void WaitForOrphanedConnectionsDrain(const std::shared_ptr<ClientSharedState>& state, std::chrono::milliseconds timeout);
    static std::string MakeConnectionId(size_t index);
    static bool ParseConnectionId(const std::string& connectionId, size_t& index);
    bool ReconnectSlot(size_t index, std::string& err);

    TqConfig Config;
    std::vector<TqClientSlotPath> SlotPaths;
    mutable std::mutex ConfigLock;
    std::atomic<size_t> PickIndex{0};
    std::shared_ptr<ClientSharedState> State{std::make_shared<ClientSharedState>()};
    std::shared_ptr<MsQuicApi> Api;
    std::unique_ptr<MsQuicRegistration> Registration;
    std::shared_ptr<MsQuicConfiguration> Configuration;
};

class QuicServerSession {
public:
    using StreamHandler = std::function<void(MsQuicConnection*, HQUIC)>;
    using ConnectionHandler = std::function<void(MsQuicConnection*)>;

    bool Start(const TqConfig& cfg);
    void Stop();
    void Run();
    std::vector<std::string> ResolvedListenAddresses();
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
    std::vector<std::unique_ptr<MsQuicListener>> Listeners;
    std::vector<TqResolvedListen> AllowedListens;
    std::vector<std::string> ResolvedListens;
};
