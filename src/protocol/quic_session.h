#pragma once

#define QUIC_API_ENABLE_INSECURE_FEATURES 1
#define QUIC_API_ENABLE_PREVIEW_FEATURES 1

#include "config.h"
#include "msquic.hpp"
#include "quic_address.h"
#include "terminal_convergence.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct TqServerCleanupTracker;

struct TqTerminalConnectionKey {
    uint64_t ConnectionId{0};
    uint64_t Generation{0};
};

class TqTerminalConnectionController {
public:
    virtual ~TqTerminalConnectionController() = default;
    virtual bool RequestShutdown(
        uint32_t slotIndex,
        TqTerminalConnectionKey key,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept = 0;
};

std::shared_ptr<TqTerminalEscalation> TqMakeServerTerminalEscalation(
    TqTerminalConnectionKey key) noexcept;
bool TqLookupClientTerminalConnection(
    MsQuicConnection* connection,
    TqTerminalConnectionKey& key,
    std::shared_ptr<TqTerminalEscalation>& escalation) noexcept;
bool TqLookupServerTerminalConnection(
    MsQuicConnection* connection,
    TqTerminalConnectionKey& key,
    std::shared_ptr<TqTerminalEscalation>& escalation) noexcept;

// Server-side: assign stable conn_id per accepted QUIC connection (OPEN_OK field).
uint32_t TqRegisterServerConnection(
    MsQuicConnection* connection,
    const std::string& encryption = "enabled");
uint32_t TqLookupServerConnectionId(MsQuicConnection* connection);
std::string TqLookupServerConnectionPeerId(MsQuicConnection* connection);
bool TqSetServerConnectionClientName(MsQuicConnection* connection, const std::string& clientName);
void TqUnregisterServerConnection(MsQuicConnection* connection);
uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection);
MsQuicSettings TqMakeMsQuicSettings(const TqConfig& cfg, bool server);

struct TqConnectionSnapshot {
    std::string ConnectionId;
    uint32_t SlotIndex{0};
    uint64_t NumericConnectionId{0};
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

struct TqClientRetryDiagnostics {
    uint64_t ScheduledTotal{0};
    uint64_t ExecutedTotal{0};
    uint64_t StaleDroppedTotal{0};
    uint64_t ScheduleFailedTotal{0};
};

struct TqServerConnectionSnapshot {
    std::string ConnectionId;
    uint64_t NumericConnectionId{0};
    uint64_t Generation{0};
    std::string ClientName;
    std::string RemoteAddress;
    std::string State;
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    uint64_t ActiveTunnels{0};
    std::string LastError;
    std::string Encryption;
};

struct TqClientPickedConnection {
    MsQuicConnection* Connection{nullptr};
    std::shared_ptr<MsQuicConnection> ConnectionOwner;
    std::string ConnectionId;
    uint32_t SlotIndex{0};
    uint64_t NumericConnectionId{0};
    uint64_t Generation{0};
    std::shared_ptr<TqTerminalEscalation> TerminalEscalation;
};

std::vector<TqServerConnectionSnapshot> TqSnapshotServerConnections();
bool TqGetServerConnectionSnapshot(const std::string& connectionId, TqServerConnectionSnapshot& out);
bool TqAbortServerConnectionTunnels(const std::string& connectionId);
void TqServerConnectionStreamStarted(MsQuicConnection* connection);
void TqServerConnectionStreamFinished(MsQuicConnection* connection);
void TqServerConnectionStreamFinishedById(uint32_t connectionId);
void TqFinalStopServerConnectionCleanup() noexcept;

#if defined(TQ_UNIT_TESTING)
uint32_t TqRegisterServerConnectionForTest(
    HQUIC handle,
    MsQuicConnection* connection = nullptr,
    const std::string& encryption = "enabled");
uint32_t TqRegisterServerConnectionOwnerForTest(
    HQUIC handle, std::shared_ptr<MsQuicConnection> connection);
void TqSetBeforeServerTerminalShutdownForTest(std::function<void()> hook);
bool TqDeferServerConnectionOwnerForTest(
    std::shared_ptr<MsQuicConnection> owner,
    std::function<void()> waitForOuterReturn);
std::shared_ptr<TqServerCleanupTracker> TqMakeServerCleanupTrackerForTest();
bool TqDeferServerConnectionOwnerForTrackerForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker,
    std::shared_ptr<MsQuicConnection> owner,
    std::function<void()> waitForOuterReturn);
void TqDrainServerConnectionCleanupTrackerForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker);
uint64_t TqServerConnectionCleanupWatermarkForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker);
void TqDrainServerConnectionCleanupThroughForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker,
    uint64_t watermark);
bool TqDuplicateServerConnectionCleanupEnqueueForTest(
    std::shared_ptr<MsQuicConnection> owner,
    std::function<void()> waitForOuterReturn);
uint64_t TqServerConnectionCleanupDuplicateCountForTest();
void TqFinalStopServerConnectionCleanupForTest();
void* TqRegisterServerCleanupProducerForTest(
    std::shared_ptr<MsQuicConnection> owner,
    std::function<void()> waitForOuterReturn);
bool TqEnqueueRegisteredServerCleanupProducerForTest(void* producer);
bool TqServerCleanupProducerAdmissionOpenForTest();
bool TqEnterServerCallbackForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker);
void TqLeaveServerCallbackForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker);
void TqCloseServerCallbackAdmissionForTest(
    const std::shared_ptr<TqServerCleanupTracker>& tracker);
void TqFailNextServerCleanupEnqueueForTest();
void TqDrainServerConnectionCleanupForTest();
bool TqSetServerConnectionClientNameForTest(HQUIC handle, const std::string& clientName);
void TqUnregisterServerConnectionForTest(HQUIC handle);
void TqMarkServerConnectionClosingForTest(HQUIC handle);
uint64_t TqServerConnectionShutdownCallsForTest();
uint64_t TqServerTerminalDuplicateForTest();
uint64_t TqServerTerminalClosingSuppressedForTest();
void TqResetServerTerminalEscalationCountersForTest();

struct TqCredentialConfigSnapshot {
    QUIC_CREDENTIAL_TYPE Type{};
    QUIC_CREDENTIAL_FLAGS Flags{};
    bool HasCertificateFile{false};
    std::string CaCertificateFile;
};

TqCredentialConfigSnapshot TqBuildCredentialConfigSnapshotForTest(const TqConfig& cfg, bool server);
const char* TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS status);
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
    TqClientRetryDiagnostics SnapshotRetryDiagnostics() const;
    bool SetDesiredConnectionCount(uint32_t desired, std::string& err);
    bool StopHighestConnection(const std::string& connectionId, std::string& err);
    bool ReconnectConnection(const std::string& connectionId, std::string& err);
    bool AbortConnectionTunnels(const std::string& connectionId, std::string& err);
    uint32_t ConnectionCount() const;
    uint32_t ConnectedConnectionCount() const;
    std::shared_ptr<TqTerminalEscalation> MakeTerminalEscalation(
        uint32_t slotIndex,
        uint64_t numericConnectionId,
        uint64_t generation) noexcept;

#if defined(TQ_UNIT_TESTING)
    void SetRetryDiagnosticsForTest(const TqClientRetryDiagnostics& diagnostics);
    struct ReconnectTestHooks {
        std::function<bool(size_t index)> StartSlotOverride;
        std::function<void(size_t index, const TqClientSlotPath& path)> StartSlotPathObserver;
        std::function<void(size_t index)> BeforePublishSlot;
        std::function<void(size_t index)> AfterStartClaim;
        std::function<QUIC_STATUS(size_t index)> ConnectionStartOverride;
        std::function<void(size_t index, uint64_t generation)> ContextDeleted;
        std::function<bool(
            MsQuicConnection* connection,
            QUIC_STREAM_OPEN_FLAGS openFlags,
            QUIC_SEND_FLAGS sendFlags,
            const std::vector<uint8_t>& payload)> SendClientHelloOverride;
        std::function<void()> BeforeTerminalConnectionShutdown;
        std::function<void()> BeforeRetryStartClaim;
        std::function<void()> BeforeEnsureStartClaim;
        std::function<void()> BeforeShutdownRetryReservation;
        std::function<void()> RetryTraceObserver;
        std::function<void(const char* event)> RetryTraceEventObserver;
    };
    void SetReconnectTestHooks(ReconnectTestHooks hooks);
    void MarkReconnectStartedForTest(size_t slots);
    void MarkReconnectStartedForTest(size_t slots, const TqConfig& cfg);
    void MarkSlotConnectedForTest(size_t index, MsQuicConnection* connection);
    void MarkSlotConnectedForTest(
        size_t index, std::shared_ptr<MsQuicConnection> connection);
    void MarkSlotDisconnectedForTest(size_t index);
    void MarkSlotClosingForTest(size_t index);
    void ClearSlotConnectionForTest(size_t index);
    uint64_t ConnectionShutdownCallsForTest(size_t index) const;
    uint64_t TerminalEscalationGenerationMismatchForTest() const;
    uint64_t TerminalEscalationDuplicateForTest() const;
    uint64_t TerminalEscalationClosingSuppressedForTest() const;
    void SendClientHelloForTest(MsQuicConnection* connection);
    MsQuicConnection* PickConnectionForTest();
    void ScheduleStartRetryForTest(size_t index);
    void SetNextRetryTokenForTest(uint64_t token);
    void SetNextStartClaimForTest(uint64_t claim);
    bool RetryStateLockAvailableForTest();
    bool CompleteSlotShutdownForTest(
        size_t index,
        uint64_t expectedNumericConnectionId,
        uint64_t expectedGeneration,
        bool useCurrentConnectionIdentity);
    static bool ConnectionStartAcceptedForTest(QUIC_STATUS status);
#endif

private:
    struct ClientSharedState;
    struct ClientSessionGate;

    struct ClientConnContext {
        std::shared_ptr<ClientSessionGate> Gate;
        std::shared_ptr<ClientSharedState> State;
        size_t SlotIndex{0};
        uint64_t NumericConnectionId{0};
        uint64_t Generation{0};
        ~ClientConnContext();
    };

    struct ConnectionSlot {
        ClientConnContext* Context{nullptr};
        std::shared_ptr<MsQuicConnection> Connection;
        std::string ConnectionId;
        uint64_t NumericConnectionId{0};
        uint64_t Generation{0};
        bool Connected{false};
        bool Closing{false};
        bool TerminalShutdownReserved{false};
        bool RetrySchedulingFailed{false};
        uint64_t ActiveRetryToken{0};
        uint64_t ActiveStartClaim{0};
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
        std::vector<std::shared_ptr<MsQuicConnection>> OrphanedConnections;
        std::mutex Lock;
        std::condition_variable StateChanged;
        std::condition_variable OrphanDrained;
        StreamHandler PeerStreamHandler;
        ConnectionStateHandler ConnectionStateChanged;
        DelayedTaskScheduler Scheduler;
        std::shared_ptr<ClientSessionGate> SessionGate;
        std::shared_ptr<MsQuicApi> Api;
        std::shared_ptr<TqTerminalConnectionController> TerminalController;
        uint64_t TerminalGenerationMismatch{0};
        uint64_t TerminalDuplicate{0};
        uint64_t TerminalClosingSuppressed{0};
        uint64_t NextRetryToken{1};
        uint64_t NextStartClaim{1};
        TqClientRetryDiagnostics RetryDiagnostics;
#if defined(TQ_UNIT_TESTING)
        ReconnectTestHooks TestHooks;
        std::vector<uint64_t> ConnectionShutdownCalls;
#endif
    };

    struct RetryTicket {
        size_t SlotIndex{0};
        uint64_t NumericConnectionId{0};
        uint64_t Generation{0};
        uint64_t Token{0};
    };

    struct RetrySubmission {
        RetryTicket Ticket;
        DelayedTaskScheduler Scheduler;
        std::weak_ptr<ClientSharedState> WeakState;
        std::shared_ptr<ClientSessionGate> Gate;
    };

    struct ShutdownCompleteResult {
        std::shared_ptr<MsQuicConnection> CompletedSlotConnection;
        std::optional<RetrySubmission> Retry;
        ConnectionStateNotification Notification;
        bool WasCurrent{false};
    };

    void Stop(bool clearHandlers);
    bool StartSlot(
        size_t index,
        const RetryTicket* expectedRetry = nullptr,
        bool* retryClaimed = nullptr,
        bool requireIdleClaim = false);
    void StartAllSlots();
    static void InvalidateRetryLocked(ConnectionSlot& slot) noexcept;
    static bool RetryTicketMatchesLocked(
        const ClientSharedState& state,
        const RetryTicket& ticket) noexcept;
    static std::optional<RetrySubmission> ReserveRetryLocked(
        const std::shared_ptr<ClientSharedState>& state,
        size_t index,
        uint64_t expectedNumericConnectionId,
        uint64_t expectedGeneration);
    void ScheduleStartRetry(
        size_t index,
        uint64_t expectedNumericConnectionId,
        uint64_t expectedGeneration);
    void SubmitRetry(RetrySubmission submission);
    static ShutdownCompleteResult CompleteSlotShutdownLocked(
        const std::shared_ptr<ClientSharedState>& state,
        size_t slotIndex,
        ClientConnContext* context,
        MsQuicConnection* connection);
    static QuicClientSession* AcquireLiveSession(
        const std::shared_ptr<ClientSessionGate>& gate);
    static void ReleaseLiveSession(const std::shared_ptr<ClientSessionGate>& gate);
    static MsQuicConnection* PickableConnectionLocked(const ConnectionSlot& slot);
    static uint32_t ConnectedCountLocked(const ClientSharedState& state);
    static void NotifyConnectionStateChanged(ConnectionStateNotification notification);
    void SendClientHello(MsQuicConnection* connection);
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
    std::shared_ptr<TqServerCleanupTracker> CleanupTracker;
};
