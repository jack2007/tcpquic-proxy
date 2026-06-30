#include "quic_session.h"
#include "quic_address.h"
#include "relay.h"
#include "trace.h"
#include "tunnel_registry.h"
#include "tuning.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>

extern const MsQuicApi* MsQuic;

namespace {

constexpr char TqAlpn[] = "tcpquic-tunnel/1";
constexpr uint64_t TqIdleTimeoutMs = 60 * 1000;

struct TqCredentialConfig {
    QUIC_CREDENTIAL_CONFIG Config{};
    QUIC_CERTIFICATE_FILE CertFile{};

    TqCredentialConfig(const TqConfig& cfg, bool server, QUIC_CREDENTIAL_FLAGS flags) {
        Config.Flags = flags;
#if defined(__APPLE__) || defined(_WIN32)
        if (!server) {
            // Non-Linux msquic validates via platform trust store unless this flag
            // is set; --ca PEM must use OpenSSL chain verify like Linux.
            Config.Flags |= QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION;
        }
#endif
        if (server) {
            CertFile.PrivateKeyFile = cfg.QuicKey.c_str();
            CertFile.CertificateFile = cfg.QuicCert.c_str();
            Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
            Config.CertificateFile = &CertFile;
        } else {
            Config.Type = QUIC_CREDENTIAL_TYPE_NONE;
            Config.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
                Config.Flags | QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE);
            Config.CaCertificateFile = cfg.QuicCa.c_str();
        }
    }
};

QUIC_CREDENTIAL_FLAGS TqMakeCredentialFlags(bool server) {
    return server ? QUIC_CREDENTIAL_FLAG_NONE : QUIC_CREDENTIAL_FLAG_CLIENT;
}

std::string TqFormatConnectionAddr(MsQuicConnection* connection, uint32_t param) {
    if (connection == nullptr || !connection->IsValid()) {
        return "?";
    }
    QUIC_ADDR addr{};
    uint32_t len = sizeof(addr);
    if (QUIC_FAILED(connection->GetParam(param, &len, &addr))) {
        return "?";
    }
    QUIC_ADDR_STR addrStr{};
    if (!QuicAddrToString(&addr, &addrStr)) {
        return "?";
    }
    return addrStr.Address;
}

std::string TqQuicStatusText(const char* operation, QUIC_STATUS status) {
    char text[96];
    std::snprintf(text, sizeof(text), "%s failed, 0x%x", operation, status);
    return text;
}

}

MsQuicSettings TqMakeMsQuicSettings(const TqConfig& cfg, bool server) {
    MsQuicSettings settings;
    settings.SetCongestionControlAlgorithm(QUIC_CONGESTION_CONTROL_ALGORITHM_BBR);
    settings.SetKeepAlive(cfg.QuicKeepAliveIntervalMs);
    settings.SetIdleTimeoutMs(TqIdleTimeoutMs);
    settings.SetPeerBidiStreamCount(static_cast<uint16_t>(cfg.QuicConnectionStreamCount));
    settings.SetStreamRecvWindowDefault(TqValidationFlowWindowBytes);
    settings.SetConnFlowControlWindow(TqValidationFlowWindowBytes);
    settings.InitialWindowPackets = cfg.Tuning.InitialWindowPackets;
    settings.IsSet.InitialWindowPackets = TRUE;
    settings.SetInitialRttMs(cfg.Tuning.InitialRttMs);
    settings.SetSendBufferingEnabled(false);
    settings.SetPacingEnabled(true);
    settings.SetStreamMultiReceiveEnabled(true);
    settings.SetNetStatsEventEnabled(true);
    if (server) {
        settings.SetServerResumptionLevel(QUIC_SERVER_RESUME_ONLY);
    }
    return settings;
}

#if defined(TQ_UNIT_TESTING)
TqCredentialConfigSnapshot TqBuildCredentialConfigSnapshotForTest(const TqConfig& cfg, bool server) {
    TqCredentialConfig credential(cfg, server, TqMakeCredentialFlags(server));
    TqCredentialConfigSnapshot snapshot{};
    snapshot.Type = credential.Config.Type;
    snapshot.Flags = credential.Config.Flags;
    snapshot.HasCertificateFile = credential.Config.CertificateFile != nullptr;
    if (credential.Config.CaCertificateFile != nullptr) {
        snapshot.CaCertificateFile = credential.Config.CaCertificateFile;
    }
    return snapshot;
}
#endif

namespace {

QUIC_EXECUTION_PROFILE TqToMsQuicProfile(TqQuicProfile profile) {
    switch (profile) {
    case TqQuicProfile::LowLatency:
        return QUIC_EXECUTION_PROFILE_LOW_LATENCY;
    default:
        return QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT;
    }
}

std::mutex g_msquicApiLock;
std::weak_ptr<MsQuicApi> g_msquicApi;

void ReleaseSharedMsQuicApi(MsQuicApi* api) {
    {
        std::lock_guard<std::mutex> guard(g_msquicApiLock);
        if (MsQuic == api) {
            MsQuic = nullptr;
        }
    }
    delete api;
}

bool AcquireSharedMsQuicApi(std::shared_ptr<MsQuicApi>& api) {
    {
        std::lock_guard<std::mutex> guard(g_msquicApiLock);
        if (auto existing = g_msquicApi.lock()) {
            api = std::move(existing);
            return true;
        }
    }

    std::shared_ptr<MsQuicApi> created(new (std::nothrow) MsQuicApi(), ReleaseSharedMsQuicApi);
    if (!created || !created->IsValid()) {
        std::fprintf(stderr, "MsQuicOpenVersion failed, 0x%x\n",
            created ? created->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(g_msquicApiLock);
        if (auto existing = g_msquicApi.lock()) {
            api = std::move(existing);
            return true;
        }
        MsQuic = created.get();
        g_msquicApi = created;
        api = std::move(created);
    }
    return true;
}

bool InitApiAndRegistration(
    std::shared_ptr<MsQuicApi>& api,
    std::unique_ptr<MsQuicRegistration>& registration,
    QUIC_EXECUTION_PROFILE profile) {
    if (!AcquireSharedMsQuicApi(api)) {
        return false;
    }

    registration = std::make_unique<MsQuicRegistration>(
        "tcpquic-proxy",
        profile,
        true);
    if (!registration || !registration->IsValid()) {
        std::fprintf(stderr, "RegistrationOpen failed, 0x%x\n",
            registration ? registration->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        return false;
    }
    return true;
}

bool InitConfiguration(
    const TqConfig& cfg,
    const MsQuicRegistration& registration,
    bool server,
    std::unique_ptr<MsQuicConfiguration>& configuration
    ) {
    MsQuicAlpn alpn(TqAlpn);
    MsQuicSettings settings = TqMakeMsQuicSettings(cfg, server);
    const auto flags = TqMakeCredentialFlags(server);

    const QUIC_CREDENTIAL_CONFIG* credentialConfig = nullptr;
    TqCredentialConfig credential(cfg, server, flags);
    credentialConfig = &credential.Config;

    configuration = std::make_unique<MsQuicConfiguration>(
        registration,
        alpn,
        settings,
        MsQuicCredentialConfig(*credentialConfig));
    if (!configuration || !configuration->IsValid()) {
        std::fprintf(stderr, "ConfigurationOpen/LoadCredential failed, 0x%x\n",
            configuration ? configuration->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        return false;
    }
    return true;
}

bool TqSetDisable1RttEncryption(MsQuicConnection* connection, const char* role) {
    const uint8_t value = TRUE;
    const QUIC_STATUS status = connection->SetParam(
        QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION,
        sizeof(value),
        &value);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr,
            "tcpquic-proxy: failed to disable QUIC 1-RTT encryption on %s connection, 0x%x\n",
            role,
            status);
        return false;
    }
    return true;
}

std::mutex g_serverConnIdLock;
struct TqServerConnectionRecord {
    uint32_t Id{0};
    MsQuicConnection* Connection{nullptr};
    std::string RemoteAddress;
    std::string State{"connected"};
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    std::string LastError;
};
std::unordered_map<HQUIC, TqServerConnectionRecord> g_serverConnIds;
std::atomic<uint32_t> g_nextServerConnId{1};

std::mutex g_clientTraceConnLock;
std::unordered_map<HQUIC, uint32_t> g_clientTraceConnIds;

void TqSampleConnectionRtt(MsQuicConnection* connection) {
    if (connection == nullptr || !connection->IsValid()) {
        return;
    }

    QUIC_STATISTICS_V2 stats{};
    if (QUIC_FAILED(connection->GetStatistics(&stats))) {
        return;
    }

    const uint32_t sampleUs = stats.MinRtt > 0 ? stats.MinRtt : stats.Rtt;
    if (sampleUs == 0) {
        return;
    }

    TqRecordMeasuredRtt(std::max(1u, sampleUs / 1000));
}

} // namespace

std::string QuicClientSession::MakeConnectionId(size_t index) {
    return "conn-" + std::to_string(index);
}

bool QuicClientSession::ParseConnectionId(const std::string& connectionId, size_t& index) {
    constexpr const char* kPrefix = "conn-";
    constexpr size_t kPrefixLen = std::char_traits<char>::length(kPrefix);
    if (connectionId.compare(0, kPrefixLen, kPrefix) != 0 || connectionId.size() == kPrefixLen) {
        return false;
    }
    size_t parsed = 0;
    for (size_t i = kPrefixLen; i < connectionId.size(); ++i) {
        const char ch = connectionId[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = parsed * 10 + static_cast<size_t>(ch - '0');
    }
    index = parsed;
    return true;
}

uint32_t TqRegisterServerConnection(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    const uint32_t id = g_nextServerConnId.fetch_add(1);
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    TqServerConnectionRecord record;
    record.Id = id;
    record.Connection = connection;
    record.RemoteAddress = TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS);
    g_serverConnIds[connection->Handle] = std::move(record);
    return id;
}

uint32_t TqLookupServerConnectionId(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    const auto it = g_serverConnIds.find(connection->Handle);
    return it == g_serverConnIds.end() ? 0 : it->second.Id;
}

void TqUnregisterServerConnection(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    g_serverConnIds.erase(connection->Handle);
}

std::vector<TqServerConnectionSnapshot> TqSnapshotServerConnections() {
    std::vector<TqServerConnectionSnapshot> snapshots;
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    snapshots.reserve(g_serverConnIds.size());
    for (const auto& item : g_serverConnIds) {
        TqServerConnectionSnapshot snapshot;
        snapshot.ConnectionId = "srv-" + std::to_string(item.second.Id);
        snapshot.RemoteAddress = item.second.RemoteAddress;
        snapshot.State = item.second.State;
        snapshot.ActiveStreams = item.second.ActiveStreams;
        snapshot.TotalStreams = item.second.TotalStreams;
        snapshot.ActiveTunnels = TqCountConnectionTunnels(item.second.Connection);
        snapshot.LastError = item.second.LastError;
        snapshots.push_back(std::move(snapshot));
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) {
        return a.ConnectionId < b.ConnectionId;
    });
    return snapshots;
}

bool TqGetServerConnectionSnapshot(const std::string& connectionId, TqServerConnectionSnapshot& out) {
    const auto snapshots = TqSnapshotServerConnections();
    auto it = std::find_if(snapshots.begin(), snapshots.end(), [&connectionId](const auto& snapshot) {
        return snapshot.ConnectionId == connectionId;
    });
    if (it == snapshots.end()) {
        return false;
    }
    out = *it;
    return true;
}

bool TqAbortServerConnectionTunnels(const std::string& connectionId) {
    MsQuicConnection* connection = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_serverConnIdLock);
        for (const auto& item : g_serverConnIds) {
            if (connectionId == "srv-" + std::to_string(item.second.Id)) {
                connection = item.second.Connection;
                break;
            }
        }
    }
    if (connection == nullptr) {
        return false;
    }
    (void)TqAbortConnectionTunnels(connection);
    return true;
}

void TqServerConnectionStreamStarted(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    auto it = g_serverConnIds.find(connection->Handle);
    if (it == g_serverConnIds.end()) {
        return;
    }
    ++it->second.ActiveStreams;
    ++it->second.TotalStreams;
}

void TqServerConnectionStreamFinished(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    const uint32_t connectionId = TqLookupServerConnectionId(connection);
    TqServerConnectionStreamFinishedById(connectionId);
}

void TqServerConnectionStreamFinishedById(uint32_t connectionId) {
    if (connectionId == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_serverConnIdLock);
    for (auto& item : g_serverConnIds) {
        if (item.second.Id == connectionId) {
            if (item.second.ActiveStreams != 0) {
                --item.second.ActiveStreams;
            }
            return;
        }
    }
}

uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(g_clientTraceConnLock);
    const auto it = g_clientTraceConnIds.find(connection->Handle);
    return it == g_clientTraceConnIds.end() ? 0 : it->second;
}

static void TqRegisterClientTraceConnection(MsQuicConnection* connection, size_t slotIndex) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_clientTraceConnLock);
    g_clientTraceConnIds[connection->Handle] = static_cast<uint32_t>(slotIndex + 1);
}

static void TqUnregisterClientTraceConnection(MsQuicConnection* connection) {
    if (connection == nullptr || connection->Handle == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_clientTraceConnLock);
    g_clientTraceConnIds.erase(connection->Handle);
}

static bool TqEnvFlagEnabled(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return false;
    }
    const bool enabled = len > 1 && std::strcmp(value, "0") != 0;
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
#endif
}

static bool TqClientDebugEnabled() {
    static const bool enabled = TqEnvFlagEnabled("TQ_QUIC_CLIENT_DEBUG");
    return enabled;
}

static constexpr std::chrono::milliseconds TqClientStartRetryDelay{3000};

static void TqClientDebugLog(const char* message, size_t slotIndex, const void* connection, QUIC_STATUS status = QUIC_STATUS_SUCCESS) {
    if (!TqClientDebugEnabled()) {
        return;
    }
    std::fprintf(stderr,
        "tcpquic-proxy quic-client-debug: %s slot=%zu conn=%p status=0x%x\n",
        message,
        slotIndex + 1,
        connection,
        status);
}

static bool TqConnectionStartAccepted(QUIC_STATUS status) {
    return QUIC_SUCCEEDED(status) || status == QUIC_STATUS_PENDING;
}

QuicClientSession::~QuicClientSession() {
    Stop();
}

bool QuicClientSession::Start(const TqConfig& cfg) {
    StreamHandler handler;
    ConnectionStateHandler stateHandler;
    DelayedTaskScheduler scheduler;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        handler = State->PeerStreamHandler;
        stateHandler = State->ConnectionStateChanged;
        scheduler = State->Scheduler;
    }
    Stop(false);
    State = std::make_shared<ClientSharedState>();
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        State->PeerStreamHandler = handler;
        State->ConnectionStateChanged = stateHandler;
        State->Scheduler = scheduler;
    }

    std::vector<TqClientSlotPath> slotPaths;
    std::string pathErr;
    if (!TqBuildClientSlotPaths(cfg, slotPaths, pathErr)) {
        std::fprintf(stderr, "Invalid QUIC peer/path config: %s\n", pathErr.c_str());
        return false;
    }

    TqConfig sessionConfig = cfg;
    sessionConfig.QuicConnections = static_cast<uint32_t>(slotPaths.size());

    std::shared_ptr<MsQuicApi> api;
    std::unique_ptr<MsQuicRegistration> registration;
    std::unique_ptr<MsQuicConfiguration> configuration;
    if (!InitApiAndRegistration(api, registration, TqToMsQuicProfile(cfg.QuicProfile)) ||
        !InitConfiguration(sessionConfig, *registration, false, configuration)) {
        Stop(false);
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            State->PeerStreamHandler = std::move(handler);
            State->ConnectionStateChanged = std::move(stateHandler);
            State->Scheduler = std::move(scheduler);
        }
        return false;
    }

    std::shared_ptr<MsQuicApi> apiForState;
    size_t slotCount = 0;
    {
        std::lock_guard<std::mutex> configGuard(ConfigLock);
        Config = std::move(sessionConfig);
        SlotPaths = std::move(slotPaths);
        Api = std::move(api);
        Registration = std::move(registration);
        Configuration = std::move(configuration);
        apiForState = Api;
        slotCount = SlotPaths.size();
    }

    {
        std::lock_guard<std::mutex> guard(State->Lock);
        State->Started = true;
        State->Stopping = false;
        State->Api = std::move(apiForState);
        State->SessionGate = std::make_shared<ClientSessionGate>();
        State->Slots.clear();
        State->Slots.resize(slotCount);
        for (size_t i = 0; i < State->Slots.size(); ++i) {
            State->Slots[i].ConnectionId = MakeConnectionId(i);
        }
        PickIndex.store(0);
    }
    {
        std::shared_ptr<ClientSessionGate> gate;
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            gate = State->SessionGate;
        }
        if (gate) {
            std::lock_guard<std::mutex> guard(gate->Lock);
            gate->Session = this;
        }
    }
    StartAllSlots();
    return true;
}

void QuicClientSession::DropOrphanedConnection(const std::shared_ptr<ClientSharedState>& state, MsQuicConnection* connection) {
    if (connection == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(state->Lock);
    for (auto it = state->OrphanedConnections.begin(); it != state->OrphanedConnections.end(); ++it) {
        if (it->get() == connection) {
            state->OrphanedConnections.erase(it);
            if (state->OrphanedConnections.empty()) {
                state->Api.reset();
            }
            state->OrphanDrained.notify_all();
            return;
        }
    }
}

void QuicClientSession::WaitForOrphanedConnectionsDrain(const std::shared_ptr<ClientSharedState>& state, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> guard(state->Lock);
    state->OrphanDrained.wait_for(guard, timeout, [&state] { return state->OrphanedConnections.empty(); });
}

void QuicClientSession::Stop() {
    Stop(true);
}

void QuicClientSession::Stop(bool clearHandlers) {
    std::shared_ptr<MsQuicApi> apiLocal;
    std::unique_ptr<MsQuicRegistration> registrationLocal;
    std::shared_ptr<MsQuicConfiguration> configurationLocal;
    std::shared_ptr<ClientSharedState> state = State;
    std::shared_ptr<ClientSessionGate> gate;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        gate = state->SessionGate;
    }
    if (gate) {
        std::unique_lock<std::mutex> guard(gate->Lock);
        gate->Session = nullptr;
        gate->Drained.wait(guard, [&gate]() {
            return gate->ActiveCalls == 0;
        });
    }

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        state->Started = false;
        state->Stopping = true;
        state->Scheduler = {};
        if (clearHandlers) {
            state->PeerStreamHandler = nullptr;
            state->ConnectionStateChanged = nullptr;
        }
        for (auto& slot : state->Slots) {
            if (slot.Connection) {
                slot.Connection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
                state->OrphanedConnections.push_back(std::move(slot.Connection));
            }
            slot.Context = nullptr;
            slot.Connected = false;
        }
        state->Slots.clear();
    }

    state->StateChanged.notify_all();
    WaitForOrphanedConnectionsDrain(state, std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (state->OrphanedConnections.empty()) {
            state->Api.reset();
        }
    }

    {
        std::lock_guard<std::mutex> guard(ConfigLock);
        apiLocal = std::move(Api);
        registrationLocal = std::move(Registration);
        configurationLocal = std::move(Configuration);
    }
}

MsQuicConnection* QuicClientSession::GetConnection() {
    return PickConnection();
}

MsQuicConnection* QuicClientSession::PickConnection() {
    return PickConnectionWithId().Connection;
}

TqClientPickedConnection QuicClientSession::PickConnectionWithId() {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (State->Slots.empty()) {
        return {};
    }
    const size_t count = State->Slots.size();
    for (size_t attempt = 0; attempt < count; ++attempt) {
        const size_t index = PickIndex.fetch_add(1) % count;
        auto& slot = State->Slots[index];
        if (auto* connection = PickableConnectionLocked(slot)) {
            TqClientPickedConnection picked;
            picked.Connection = connection;
            picked.ConnectionId = slot.ConnectionId.empty() ? MakeConnectionId(index) : slot.ConnectionId;
            return picked;
        }
    }
    return {};
}

MsQuicConnection* QuicClientSession::PickConnectionAt(size_t index) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) {
        return nullptr;
    }
    auto& slot = State->Slots[index];
    return PickableConnectionLocked(slot);
}

MsQuicConnection* QuicClientSession::PickConnectionFrom(size_t firstIndex) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (firstIndex >= State->Slots.size()) {
        return nullptr;
    }
    const size_t count = State->Slots.size() - firstIndex;
    for (size_t attempt = 0; attempt < count; ++attempt) {
        const size_t index = firstIndex + (PickIndex.fetch_add(1) % count);
        auto& slot = State->Slots[index];
        if (auto* connection = PickableConnectionLocked(slot)) {
            return connection;
        }
    }
    return nullptr;
}

uint32_t QuicClientSession::ConnectionCount() const {
    std::lock_guard<std::mutex> guard(State->Lock);
    return static_cast<uint32_t>(State->Slots.size());
}

uint32_t QuicClientSession::ConnectedConnectionCount() const {
    std::lock_guard<std::mutex> guard(State->Lock);
    return ConnectedCountLocked(*State);
}

std::vector<TqConnectionSnapshot> QuicClientSession::SnapshotConnections() const {
    std::vector<TqConnectionSnapshot> snapshots;
    std::scoped_lock guard(ConfigLock, State->Lock);
    snapshots.reserve(State->Slots.size());
    for (size_t i = 0; i < State->Slots.size(); ++i) {
        const auto& slot = State->Slots[i];
        TqConnectionSnapshot snapshot;
        snapshot.ConnectionId = slot.ConnectionId.empty() ? MakeConnectionId(i) : slot.ConnectionId;
        snapshot.SlotIndex = static_cast<uint32_t>(i);
        snapshot.Generation = slot.Generation;
        snapshot.Connected = slot.Connected && slot.Connection && slot.Connection->IsValid();
        snapshot.RetryScheduled = slot.RetryScheduled;
        if (snapshot.Connected) {
            snapshot.State = "connected";
        } else if (slot.RetryScheduled) {
            snapshot.State = "retry_scheduled";
        } else if (slot.Connection) {
            snapshot.State = "connecting";
        } else {
            snapshot.State = State->Started && !State->Stopping ? "connecting" : "stopped";
        }
        snapshot.ActiveTunnels = slot.Connection ? TqCountConnectionTunnels(slot.Connection.get()) : 0;
        snapshot.TotalTunnels = 0;
        snapshot.LastError = slot.LastError;
        if (i < SlotPaths.size()) {
            snapshot.PathName = SlotPaths[i].Name;
            snapshot.LocalAddress = SlotPaths[i].LocalAddress;
            snapshot.PeerAddress = SlotPaths[i].PeerText;
        }
        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

bool QuicClientSession::SetDesiredConnectionCount(uint32_t desired, std::string& err) {
    if (desired == 0) {
        err = "desired connection count must be greater than zero";
        return false;
    }

    size_t oldSize = 0;
    {
        std::scoped_lock guard(ConfigLock, State->Lock);
        if (!Config.QuicPaths.empty()) {
            err = "path-mode uses fixed connection slots";
            return false;
        }
        if (!State->Started || State->Stopping) {
            err = "session is not running";
            return false;
        }
        oldSize = State->Slots.size();
        if (desired < oldSize) {
            err = "use StopHighestConnection to shrink";
            return false;
        }

        TqConfig updatedConfig = Config;
        updatedConfig.QuicConnections = desired;
        std::vector<TqClientSlotPath> updatedSlotPaths;
        if (!TqBuildClientSlotPaths(updatedConfig, updatedSlotPaths, err)) {
            return false;
        }

        Config = updatedConfig;
        SlotPaths = std::move(updatedSlotPaths);
        State->Slots.resize(desired);
        for (size_t i = oldSize; i < State->Slots.size(); ++i) {
            State->Slots[i].ConnectionId = MakeConnectionId(i);
        }
    }

    for (size_t i = oldSize; i < desired; ++i) {
        (void)StartSlot(i);
    }
    return true;
}

bool QuicClientSession::StopHighestConnection(const std::string& connectionId, std::string& err) {
    std::unique_ptr<MsQuicConnection> oldConnection;
    {
        std::scoped_lock guard(ConfigLock, State->Lock);
        if (!Config.QuicPaths.empty()) {
            err = "path-mode uses fixed connection slots";
            return false;
        }
        size_t index = 0;
        if (!ParseConnectionId(connectionId, index)) {
            err = "invalid connection id";
            return false;
        }
        if (!State->Started || State->Stopping || State->Slots.empty()) {
            err = "session is not running";
            return false;
        }
        const size_t highest = State->Slots.size() - 1;
        if (index != highest) {
            err = "only highest connection slot can be removed";
            return false;
        }
        auto& slot = State->Slots[index];
        oldConnection = std::move(slot.Connection);
        slot.Context = nullptr;
        slot.Connected = false;
        State->Slots.pop_back();
        Config.QuicConnections = static_cast<uint32_t>(State->Slots.size());
        if (SlotPaths.size() > State->Slots.size()) {
            SlotPaths.resize(State->Slots.size());
        }
    }
    if (oldConnection) {
        oldConnection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        std::lock_guard<std::mutex> guard(State->Lock);
        State->OrphanedConnections.push_back(std::move(oldConnection));
    }
    State->StateChanged.notify_all();
    return true;
}

bool QuicClientSession::ReconnectSlot(size_t index, std::string& err) {
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        if (!State->Started || State->Stopping || index >= State->Slots.size()) {
            err = "connection not found";
            return false;
        }
        auto& slot = State->Slots[index];
        slot.Connected = false;
        slot.RetryScheduled = false;
        ++slot.Generation;
    }
    (void)StartSlot(index);
    State->StateChanged.notify_all();
    return true;
}

bool QuicClientSession::ReconnectConnection(const std::string& connectionId, std::string& err) {
    size_t index = 0;
    if (!ParseConnectionId(connectionId, index)) {
        err = "invalid connection id";
        return false;
    }
    return ReconnectSlot(index, err);
}

bool QuicClientSession::AbortConnectionTunnels(const std::string& connectionId, std::string& err) {
    size_t index = 0;
    if (!ParseConnectionId(connectionId, index)) {
        err = "invalid connection id";
        return false;
    }
    MsQuicConnection* connection = nullptr;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        if (index >= State->Slots.size()) {
            err = "connection not found";
            return false;
        }
        connection = State->Slots[index].Connection.get();
    }
    (void)TqAbortConnectionTunnels(connection);
    return true;
}

MsQuicConnection* QuicClientSession::PickableConnectionLocked(const ConnectionSlot& slot) {
    if (!slot.Connected || slot.RetryScheduled) {
        return nullptr;
    }
#if defined(TQ_UNIT_TESTING)
    if (slot.TestConnectionOverride) {
        return slot.TestConnectionOverride;
    }
#endif
    if (slot.Connection && slot.Connection->IsValid()) {
        return slot.Connection.get();
    }
    return nullptr;
}

uint32_t QuicClientSession::ConnectedCountLocked(const ClientSharedState& state) {
    uint32_t count = 0;
    for (const auto& slot : state.Slots) {
        if (PickableConnectionLocked(slot)) {
            ++count;
        }
    }
    return count;
}

bool QuicClientSession::EnsureConnected(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> guard(State->Lock);
    while (State->Started && !State->Stopping) {
        bool anyConnected = false;
        if (ConnectedCountLocked(*State) > 0) {
            return true;
        }
        for (size_t i = 0; i < State->Slots.size(); ++i) {
            auto& slot = State->Slots[i];
            if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
                anyConnected = true;
                continue;
            }
            if (!slot.Connection) {
                guard.unlock();
                StartSlot(i);
                guard.lock();
                if (ConnectedCountLocked(*State) > 0) {
                    return true;
                }
                break;
            }
        }
        if (anyConnected) {
            return true;
        }
        if (State->Slots.empty() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        State->StateChanged.wait_until(
            guard,
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
    }
    return false;
}

bool QuicClientSession::EnsureAnyConnected(std::chrono::milliseconds timeout) {
    return EnsureConnected(timeout);
}

void QuicClientSession::SetPeerStreamHandler(StreamHandler h) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->PeerStreamHandler = std::move(h);
}

void QuicClientSession::SetConnectionStateHandler(ConnectionStateHandler h) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->ConnectionStateChanged = std::move(h);
}

void QuicClientSession::SetDelayedTaskScheduler(DelayedTaskScheduler scheduler) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->Scheduler = std::move(scheduler);
}

#if defined(TQ_UNIT_TESTING)
void QuicClientSession::SetReconnectTestHooks(ReconnectTestHooks hooks) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->TestHooks = std::move(hooks);
}

void QuicClientSession::MarkReconnectStartedForTest(size_t slots) {
    TqConfig cfg;
    cfg.QuicConnections = static_cast<uint32_t>(slots);
    cfg.QuicPeer = "127.0.0.1:443";
    MarkReconnectStartedForTest(slots, cfg);
}

void QuicClientSession::MarkReconnectStartedForTest(size_t slots, const TqConfig& cfg) {
    std::shared_ptr<ClientSessionGate> gate;
    {
        std::scoped_lock guard(ConfigLock, State->Lock);
        Config = cfg;
        Config.QuicConnections = static_cast<uint32_t>(slots);
        SlotPaths.clear();
        std::string err;
        if (!TqBuildClientSlotPaths(Config, SlotPaths, err)) {
            SlotPaths.resize(slots);
        }
        State->Started = true;
        State->Stopping = false;
        State->Slots.clear();
        State->Slots.resize(slots);
        for (size_t i = 0; i < State->Slots.size(); ++i) {
            State->Slots[i].ConnectionId = MakeConnectionId(i);
        }
        if (!State->SessionGate) {
            State->SessionGate = std::make_shared<ClientSessionGate>();
        }
        gate = State->SessionGate;
    }
    if (gate) {
        std::lock_guard<std::mutex> guard(gate->Lock);
        gate->Session = this;
    }
}

void QuicClientSession::MarkSlotConnectedForTest(size_t index, MsQuicConnection* connection) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) {
        return;
    }
    auto& slot = State->Slots[index];
    slot.Connected = true;
    slot.RetryScheduled = false;
    slot.LastError.clear();
    slot.TestConnectionOverride = connection;
}

void QuicClientSession::MarkSlotDisconnectedForTest(size_t index) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) {
        return;
    }
    auto& slot = State->Slots[index];
    slot.Connected = false;
    slot.TestConnectionOverride = nullptr;
}

MsQuicConnection* QuicClientSession::PickConnectionForTest() {
    return PickConnection();
}

void QuicClientSession::ScheduleStartRetryForTest(size_t index) {
    ScheduleStartRetry(index);
}

void QuicClientSession::RestartSlotAfterShutdownCompleteForTest(size_t index, uint64_t generation) {
    RestartSlotAfterShutdownComplete(State, index, generation);
}

bool QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS status) {
    return TqConnectionStartAccepted(status);
}
#endif

QuicClientSession::ClientConnContext::~ClientConnContext() {
#if defined(TQ_UNIT_TESTING)
    std::function<void(size_t, uint64_t)> contextDeleted;
    if (State) {
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            contextDeleted = State->TestHooks.ContextDeleted;
        }
    }
    if (contextDeleted) {
        contextDeleted(SlotIndex, Generation);
    }
#endif
}

void QuicClientSession::AbortAllTunnels() {
    std::vector<MsQuicConnection*> connections;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        connections.reserve(State->Slots.size());
        for (auto& slot : State->Slots) {
            if (slot.Connection) {
                connections.push_back(slot.Connection.get());
            }
        }
    }

    for (MsQuicConnection* connection : connections) {
        (void)TqAbortConnectionTunnels(connection);
    }
}

void QuicClientSession::NotifyConnectionStateChanged(ConnectionStateNotification notification) {
    if (notification.Handler) {
        notification.Handler(notification.ConnectedCount);
    }
}

QuicClientSession* QuicClientSession::AcquireLiveSession(
    const std::shared_ptr<ClientSessionGate>& gate) {
    if (!gate) {
        return nullptr;
    }
    std::lock_guard<std::mutex> guard(gate->Lock);
    if (gate->Session == nullptr) {
        return nullptr;
    }
    ++gate->ActiveCalls;
    return gate->Session;
}

void QuicClientSession::ReleaseLiveSession(const std::shared_ptr<ClientSessionGate>& gate) {
    if (!gate) {
        return;
    }
    std::lock_guard<std::mutex> guard(gate->Lock);
    if (gate->ActiveCalls > 0) {
        --gate->ActiveCalls;
    }
    if (gate->Session == nullptr && gate->ActiveCalls == 0) {
        gate->Drained.notify_all();
    }
}

void QuicClientSession::StartAllSlots() {
    size_t count = 0;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        count = State->Slots.size();
    }
    for (size_t i = 0; i < count; ++i) {
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            if (!State->Started || State->Stopping) {
                return;
            }
        }
        (void)StartSlot(i);
    }
}

bool QuicClientSession::StartSlot(size_t index) {
#if defined(TQ_UNIT_TESTING)
    std::function<bool(size_t)> startSlotOverride;
    std::function<void(size_t, const TqClientSlotPath&)> startSlotPathObserver;
    std::function<void(size_t)> beforePublishSlot;
    std::function<QUIC_STATUS(size_t)> connectionStartOverride;
#endif

    std::unique_ptr<MsQuicConnection> oldConnection;
    std::shared_ptr<ClientSharedState> state = State;
    TqClientSlotPath path;
    uint64_t generation = 0;
    std::shared_ptr<ClientSessionGate> gate;

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            return false;
        }
        gate = state->SessionGate;
    }
    if (!gate) {
        return false;
    }

    struct LiveCallGuard {
        std::shared_ptr<ClientSessionGate> Gate;
        bool Active{false};
        ~LiveCallGuard() {
            if (Active) {
                QuicClientSession::ReleaseLiveSession(Gate);
            }
        }
    };

    LiveCallGuard liveGuard{gate, false};
    QuicClientSession* liveSession = AcquireLiveSession(gate);
    if (liveSession != this) {
        if (liveSession != nullptr) {
            ReleaseLiveSession(gate);
        }
        return false;
    }
    liveGuard.Active = true;

    auto SetSlotLastError = [&state](size_t slotIndex, std::string message) {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (slotIndex < state->Slots.size()) {
            state->Slots[slotIndex].LastError = std::move(message);
        }
    };

    {
        std::scoped_lock guard(ConfigLock, state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            return false;
        }
        if (index >= SlotPaths.size()) {
            state->Slots[index].LastError = "connection slot has no QUIC path";
            return false;
        }
        path = SlotPaths[index];
#if defined(TQ_UNIT_TESTING)
        startSlotOverride = state->TestHooks.StartSlotOverride;
        startSlotPathObserver = state->TestHooks.StartSlotPathObserver;
        beforePublishSlot = state->TestHooks.BeforePublishSlot;
        connectionStartOverride = state->TestHooks.ConnectionStartOverride;
#endif

        auto& slot = state->Slots[index];
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return true;
        }
        oldConnection = std::move(slot.Connection);
        slot.Context = nullptr;
        slot.Connected = false;
        slot.RetryScheduled = false;
        generation = slot.Generation;
        TqClientDebugLog("slot-reset", index, oldConnection.get());
    }

#if defined(TQ_UNIT_TESTING)
    if (startSlotPathObserver) {
        startSlotPathObserver(index, path);
    }
    if (startSlotOverride) {
        return startSlotOverride(index);
    }
#endif

    if (oldConnection) {
        TqClientDebugLog("old-shutdown", index, oldConnection.get());
        oldConnection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        std::lock_guard<std::mutex> guard(state->Lock);
        state->OrphanedConnections.push_back(std::move(oldConnection));
    }

    std::unique_ptr<MsQuicConnection> newConnection;
    MsQuicConnection* connectionToStart = nullptr;
    ClientConnContext* newContext = nullptr;
    TqConfig configSnapshot;
    MsQuicRegistration* registration = nullptr;
    std::shared_ptr<MsQuicConfiguration> configuration;
    bool scheduleRetry = false;
    std::string retryError;

    {
        std::lock_guard<std::mutex> configGuard(ConfigLock);
        if (TqRuntimeTuningEnabled(Config)) {
            TqApplyRuntimeObservations(Config);
            std::unique_ptr<MsQuicConfiguration> refreshedConfiguration;
            if (!Registration || !InitConfiguration(Config, *Registration, false, refreshedConfiguration)) {
                std::fprintf(stderr, "Configuration refresh failed\n");
                retryError = "configuration refresh failed";
                scheduleRetry = true;
            } else {
                Configuration = std::move(refreshedConfiguration);
            }
        }

        configSnapshot = Config;
        registration = Registration.get();
        configuration = Configuration;
    }

    if (scheduleRetry) {
        SetSlotLastError(index, std::move(retryError));
    }

    if (!scheduleRetry && (!registration || !configuration)) {
        SetSlotLastError(index, "session configuration unavailable");
        scheduleRetry = true;
    }

    if (!scheduleRetry) {
        newContext = new (std::nothrow) ClientConnContext{gate, state, index, generation};
    }
    if (!scheduleRetry && newContext == nullptr) {
        SetSlotLastError(index, "connection context allocation failed");
        scheduleRetry = true;
    }

    auto ResetUnpublishedConnectionAndContext = [&newConnection, &newContext]() {
        if (newConnection) {
            newConnection->Context = nullptr;
        }
        delete newContext;
        newContext = nullptr;
        newConnection.reset();
    };

    if (!scheduleRetry) {
        newConnection = std::make_unique<MsQuicConnection>(
                *registration,
                CleanUpManual,
                QuicClientSession::ConnectionCallback,
                newContext);
        if (!newConnection || !newConnection->IsValid()) {
            const QUIC_STATUS initStatus =
                newConnection ? newConnection->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY;
            std::fprintf(stderr, "ConnectionOpen failed, 0x%x\n",
                initStatus);
            SetSlotLastError(index, TqQuicStatusText("ConnectionOpen", initStatus));
            ResetUnpublishedConnectionAndContext();
            scheduleRetry = true;
        }
    }

    if (!scheduleRetry && configSnapshot.QuicDisable1RttEncryption &&
        !TqSetDisable1RttEncryption(newConnection.get(), "client")) {
        SetSlotLastError(index, "failed to disable 1-RTT encryption");
        ResetUnpublishedConnectionAndContext();
        scheduleRetry = true;
    }

    if (!scheduleRetry && !path.LocalAddress.empty()) {
        QuicAddr localAddr;
        if (!TqMakeQuicAddr(TqEndpoint{path.LocalAddress, 0}, localAddr.SockAddr)) {
            SetSlotLastError(index,
                "invalid local address for path " + path.Name + ": " + path.LocalAddress);
            ResetUnpublishedConnectionAndContext();
            scheduleRetry = true;
        } else {
            const QUIC_STATUS bindStatus = newConnection->SetLocalAddr(localAddr);
            if (QUIC_FAILED(bindStatus)) {
                SetSlotLastError(index,
                    TqQuicStatusText(("SetLocalAddr " + path.Name).c_str(), bindStatus));
                ResetUnpublishedConnectionAndContext();
                scheduleRetry = true;
            }
        }
    }

    if (scheduleRetry) {
        ScheduleStartRetry(index);
        return false;
    }

#if defined(TQ_UNIT_TESTING)
    if (beforePublishSlot) {
        beforePublishSlot(index);
    }
#endif

    bool published = false;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size() ||
            state->Slots[index].Generation != generation) {
            published = false;
        } else {
            auto& slot = state->Slots[index];
            slot.Connection = std::move(newConnection);
            slot.Context = newContext;
            connectionToStart = slot.Connection.get();
            TqClientDebugLog("connection-opened", index, connectionToStart,
                slot.Connection->GetInitStatus());
            published = true;
        }
    }
    if (!published) {
        ResetUnpublishedConnectionAndContext();
        return false;
    }

    QUIC_STATUS startStatus = QUIC_STATUS_SUCCESS;
    TqClientDebugLog("connection-start-before", index, connectionToStart);
#if defined(TQ_UNIT_TESTING)
    if (connectionStartOverride) {
        startStatus = connectionStartOverride(index);
    } else
#endif
    startStatus = connectionToStart->Start(*configuration, path.PeerHost.c_str(), path.PeerPort);
    TqClientDebugLog("connection-start-after", index, connectionToStart, startStatus);

    if (TqTraceEnabled()) {
        TqTraceQuicConnecting("client", static_cast<uint32_t>(index + 1), path.PeerText.c_str());
    }

    std::unique_ptr<MsQuicConnection> failedStartConnection;
    ClientConnContext* failedStartContext = nullptr;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            return QUIC_SUCCEEDED(startStatus);
        }

        auto& slot = state->Slots[index];
        if (slot.Connection.get() != connectionToStart || slot.Context != newContext) {
            return TqConnectionStartAccepted(startStatus);
        }

        if (!TqConnectionStartAccepted(startStatus)) {
            std::fprintf(stderr, "ConnectionStart failed, 0x%x\n", startStatus);
            slot.LastError = TqQuicStatusText("ConnectionStart", startStatus);
            slot.Context = nullptr;
            if (slot.Connection.get() == connectionToStart) {
                slot.Connection->Context = nullptr;
                failedStartConnection = std::move(slot.Connection);
            }
            failedStartContext = newContext;
            newContext = nullptr;
            scheduleRetry = true;
        }
    }
    delete failedStartContext;
    failedStartConnection.reset();
    if (scheduleRetry) {
        ScheduleStartRetry(index);
        return false;
    }

    return true;
}

void QuicClientSession::ScheduleStartRetry(size_t index) {
    DelayedTaskScheduler scheduler;
    std::weak_ptr<ClientSharedState> weakState;
    std::shared_ptr<ClientSessionGate> gate;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        if (!State->Started || State->Stopping || index >= State->Slots.size()) {
            return;
        }
        if (State->Slots[index].RetryScheduled) {
            return;
        }
        scheduler = State->Scheduler;
        weakState = State;
        gate = State->SessionGate;
        if (scheduler && gate) {
            State->Slots[index].RetryScheduled = true;
        }
    }
    if (!scheduler || !gate) {
        return;
    }
    if (!scheduler(TqClientStartRetryDelay, [weakState, gate, index]() {
        auto state = weakState.lock();
        if (!state) {
            return;
        }
        {
            std::lock_guard<std::mutex> guard(state->Lock);
            if (!state->Started || state->Stopping || index >= state->Slots.size() ||
                !state->Slots[index].RetryScheduled) {
                return;
            }
            state->Slots[index].RetryScheduled = false;
        }
        QuicClientSession* session = AcquireLiveSession(gate);
        if (session == nullptr) {
            return;
        }
        (void)session->StartSlot(index);
        ReleaseLiveSession(gate);
    })) {
        std::lock_guard<std::mutex> guard(State->Lock);
        if (index < State->Slots.size()) {
            State->Slots[index].RetryScheduled = false;
        }
    }
}

void QuicClientSession::RestartSlotAfterShutdownComplete(
    const std::shared_ptr<ClientSharedState>& state,
    size_t slotIndex,
    uint64_t generation) {
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || slotIndex >= state->Slots.size() ||
            state->Slots[slotIndex].Generation != generation) {
            return;
        }
    }
    ScheduleStartRetry(slotIndex);
}

QuicClientSession::ConnectionStateNotification QuicClientSession::OnSlotConnected(
    const std::shared_ptr<ClientSharedState>& state,
    size_t index,
    MsQuicConnection* connection) {
    ConnectionStateNotification notification;
    std::lock_guard<std::mutex> guard(state->Lock);
    if (index >= state->Slots.size()) {
        return notification;
    }
    auto& slot = state->Slots[index];
    const bool wasConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (slot.Connection.get() == connection) {
        slot.Connected = true;
        slot.LastError.clear();
    }
    const bool isConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (wasConnected != isConnected) {
        notification.Handler = state->ConnectionStateChanged;
        notification.ConnectedCount = ConnectedCountLocked(*state);
    }
    return notification;
}

QuicClientSession::ConnectionStateNotification QuicClientSession::OnSlotDisconnected(
    const std::shared_ptr<ClientSharedState>& state,
    size_t index,
    MsQuicConnection* connection) {
    ConnectionStateNotification notification;
    std::lock_guard<std::mutex> guard(state->Lock);
    if (index >= state->Slots.size()) {
        return notification;
    }
    auto& slot = state->Slots[index];
    const bool wasConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (slot.Connection.get() == connection) {
        slot.Connected = false;
    }
    const bool isConnected = slot.Connected && slot.Connection && slot.Connection->IsValid();
    if (wasConnected != isConnected) {
        notification.Handler = state->ConnectionStateChanged;
        notification.ConnectedCount = ConnectedCountLocked(*state);
    }
    return notification;
}

QUIC_STATUS QUIC_API QuicClientSession::ConnectionCallback(
    MsQuicConnection* connection,
    void* context,
    QUIC_CONNECTION_EVENT* event) noexcept {
    auto* slotContext = static_cast<ClientConnContext*>(context);
    if (slotContext == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    std::shared_ptr<QuicClientSession::ClientSharedState> state = slotContext->State;
    const size_t slotIndex = slotContext->SlotIndex;
    const uint64_t generation = slotContext->Generation;

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_NETWORK_STATISTICS:
        if (TqTraceEnabled() || TqDiagStatsEnabled()) {
            TqTraceQuicNetworkStats(
                connection,
                TqTraceNetworkStats{
                    event->NETWORK_STATISTICS.BytesInFlight,
                    event->NETWORK_STATISTICS.PostedBytes,
                    event->NETWORK_STATISTICS.IdealBytes,
                    event->NETWORK_STATISTICS.SmoothedRTT,
                    event->NETWORK_STATISTICS.CongestionWindow,
                    event->NETWORK_STATISTICS.Bandwidth,
                    event->NETWORK_STATISTICS.BytesInFlightMax,
                    event->NETWORK_STATISTICS.BbrState,
                    event->NETWORK_STATISTICS.BbrRecoveryState,
                    event->NETWORK_STATISTICS.BbrRecoveryWindow,
                    event->NETWORK_STATISTICS.BbrPacingGain,
                    event->NETWORK_STATISTICS.BbrCwndGain,
                    event->NETWORK_STATISTICS.BbrMinRtt,
                    event->NETWORK_STATISTICS.BbrSendQuantum,
                    event->NETWORK_STATISTICS.BbrAppLimited != FALSE,
                    event->NETWORK_STATISTICS.SendFlushCount,
                    event->NETWORK_STATISTICS.SendFlushPacingDelayedCount,
                    event->NETWORK_STATISTICS.SendFlushCcBlockedCount,
                    event->NETWORK_STATISTICS.SendFlushSchedulingCount,
                    event->NETWORK_STATISTICS.SendFlushAmplificationBlockedCount,
                    event->NETWORK_STATISTICS.SendFlushNoWorkCount,
                    event->NETWORK_STATISTICS.SendFlushLastAllowance,
                    event->NETWORK_STATISTICS.SendFlushLastPathAllowance,
                    event->NETWORK_STATISTICS.SendFlushLastResult,
                    event->NETWORK_STATISTICS.SendFlushLastDatagrams,
                    event->NETWORK_STATISTICS.OutFlowBlockedReasons,
                    event->NETWORK_STATISTICS.LossDetectionEventCount,
                    event->NETWORK_STATISTICS.LossDetectionFackPacketCount,
                    event->NETWORK_STATISTICS.LossDetectionRackPacketCount,
                    event->NETWORK_STATISTICS.LostRetransmittableBytes,
                    event->NETWORK_STATISTICS.LastLostRetransmittableBytes});
        }
        break;
    case QUIC_CONNECTION_EVENT_CONNECTED:
        TqClientDebugLog("event-connected", slotIndex, connection);
        TqSampleConnectionRtt(connection);
        {
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC client connected peer=%s slot=%zu",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                slotIndex + 1);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotConnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        if (TqTraceEnabled() || TqDiagStatsEnabled()) {
            TqRegisterClientTraceConnection(connection, slotIndex);
            TqTraceQuicConnected(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client",
                static_cast<uint32_t>(slotIndex + 1));
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        if (state) {
            QuicClientSession::StreamHandler handler;
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                handler = state->PeerStreamHandler;
            }
            if (handler) {
                handler(connection, event->PEER_STREAM_STARTED.Stream);
            } else {
                MsQuic->StreamClose(event->PEER_STREAM_STARTED.Stream);
            }
        } else {
            MsQuic->StreamClose(event->PEER_STREAM_STARTED.Stream);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        TqClientDebugLog("event-shutdown-transport", slotIndex, connection,
            event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
        (void)TqAbortConnectionTunnels(connection);
        TqSampleConnectionRtt(connection);
        if (TqTraceEnabled()) {
            TqTraceQuicShutdownTransport(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client",
                static_cast<uint32_t>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
                event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        TqClientDebugLog("event-shutdown-peer", slotIndex, connection);
        (void)TqAbortConnectionTunnels(connection);
        TqSampleConnectionRtt(connection);
        if (TqTraceEnabled()) {
            TqTraceQuicShutdownPeer(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client",
                event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    {
        std::shared_ptr<QuicClientSession::ClientSessionGate> gate = slotContext->Gate;
        std::unique_ptr<MsQuicConnection> completedSlotConnection;
        TqClientDebugLog("event-shutdown-complete", slotIndex, connection);
        (void)TqAbortConnectionTunnels(connection);
        {
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC client disconnected peer=%s slot=%zu",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                slotIndex + 1);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
        }
        if (TqTraceEnabled()) {
            TqUnregisterClientTraceConnection(connection);
            TqTraceQuicDisconnected(
                connection,
                static_cast<uint32_t>(slotIndex + 1),
                "client");
        }
        if (state) {
            auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                if (slotIndex < state->Slots.size() &&
                    state->Slots[slotIndex].Context == slotContext) {
                    auto& slot = state->Slots[slotIndex];
                    slot.Context = nullptr;
                    slot.RetryScheduled = false;
                    if (slot.Connection.get() == connection) {
                        completedSlotConnection = std::move(slot.Connection);
                    }
                }
            }
            state->StateChanged.notify_all();
            QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
        }
        connection->Close();
        completedSlotConnection.reset();
        if (state) {
            QuicClientSession::DropOrphanedConnection(state, connection);
        }
        delete slotContext;
        if (gate && state) {
            QuicClientSession* session = QuicClientSession::AcquireLiveSession(gate);
            if (session != nullptr) {
                session->RestartSlotAfterShutdownComplete(state, slotIndex, generation);
                QuicClientSession::ReleaseLiveSession(gate);
            }
        }
        break;
    }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

bool QuicServerSession::Start(const TqConfig& cfg) {
    Stop();

    std::vector<TqResolvedListen> resolvedListens;
    std::string resolveErr;
    if (!TqResolveServerListenList(cfg.QuicListen, resolvedListens, resolveErr)) {
        std::fprintf(stderr, "Invalid --quic-listen endpoint: %s (%s)\n",
            cfg.QuicListen.c_str(), resolveErr.c_str());
        return false;
    }

    Config = cfg;
    if (!InitApiAndRegistration(Api, Registration, TqToMsQuicProfile(cfg.QuicProfile)) ||
        !InitConfiguration(Config, *Registration, true, Configuration)) {
        Stop();
        return false;
    }

    const std::vector<TqResolvedListen> bindListens = TqBuildServerListenerBindList(resolvedListens);
    {
        std::lock_guard<std::mutex> guard(Lock);
        AllowedListens = resolvedListens;
    }

    MsQuicAlpn alpn(TqAlpn);
    for (const TqResolvedListen& listen : bindListens) {
        auto listener = std::make_unique<MsQuicListener>(
            *Registration,
            CleanUpManual,
            QuicServerSession::ListenerCallback,
            this);
        if (!listener || !listener->IsValid()) {
            std::fprintf(stderr, "ListenerOpen failed for %s, 0x%x\n",
                listen.Text.c_str(),
                listener ? listener->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
            Stop();
            return false;
        }

        const QUIC_STATUS status = listener->Start(alpn, &listen.Address);
        if (QUIC_FAILED(status)) {
            std::fprintf(stderr, "ListenerStart failed for %s, 0x%x\n",
                listen.Text.c_str(), status);
            Stop();
            return false;
        }

        std::lock_guard<std::mutex> guard(Lock);
        Listeners.push_back(std::move(listener));
    }

    std::lock_guard<std::mutex> guard(Lock);
    Started = true;
    Stopping = false;
    ResolvedListens.clear();
    ResolvedListens.reserve(resolvedListens.size());
    for (const TqResolvedListen& listen : resolvedListens) {
        ResolvedListens.push_back(listen.Text);
    }
    return true;
}

void QuicServerSession::Stop() {
    std::shared_ptr<MsQuicApi> apiLocal;
    std::unique_ptr<MsQuicRegistration> registrationLocal;
    std::unique_ptr<MsQuicConfiguration> configurationLocal;
    std::vector<std::unique_ptr<MsQuicListener>> listenersLocal;

    {
        std::lock_guard<std::mutex> guard(Lock);
        Started = false;
        Stopping = true;
        for (const auto& listener : Listeners) {
            if (listener && listener->Handle) {
                MsQuic->ListenerStop(listener->Handle);
            }
        }
        apiLocal = std::move(Api);
        registrationLocal = std::move(Registration);
        configurationLocal = std::move(Configuration);
        listenersLocal = std::move(Listeners);
        AllowedListens.clear();
        ResolvedListens.clear();
    }

    StateChanged.notify_all();
}

void QuicServerSession::Run() {
    std::unique_lock<std::mutex> guard(Lock);
    StateChanged.wait(guard, [this] { return !Started || Stopping; });
    guard.unlock();
    Stop();
}

std::vector<std::string> QuicServerSession::ResolvedListenAddresses() {
    std::lock_guard<std::mutex> guard(Lock);
    return ResolvedListens;
}

void QuicServerSession::SetPeerStreamHandler(StreamHandler h) {
    std::lock_guard<std::mutex> guard(Lock);
    PeerStreamHandler = std::move(h);
}

void QuicServerSession::SetConnectionHandler(ConnectionHandler h) {
    std::lock_guard<std::mutex> guard(Lock);
    PeerConnectionHandler = std::move(h);
}

QUIC_STATUS QUIC_API QuicServerSession::ListenerCallback(
    MsQuicListener*,
    void* context,
    QUIC_LISTENER_EVENT* event) noexcept {
    auto* session = static_cast<QuicServerSession*>(context);
    if (session == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        return QUIC_STATUS_SUCCESS;
    }

    bool disable1RttEncryption = false;
    {
        std::lock_guard<std::mutex> guard(session->Lock);
        const QUIC_ADDR* localAddress =
            event->NEW_CONNECTION.Info ? event->NEW_CONNECTION.Info->LocalAddress : nullptr;
        if (session->Stopping ||
            !session->Configuration ||
            !TqServerListenAllowsLocalAddress(session->AllowedListens, localAddress)) {
            return QUIC_STATUS_INVALID_STATE;
        }
        disable1RttEncryption = session->Config.QuicDisable1RttEncryption;
    }

    TqTraceQuicIncoming();

    auto* connection = new (std::nothrow) MsQuicConnection(
        event->NEW_CONNECTION.Connection,
        CleanUpAutoDelete,
        QuicServerSession::ConnectionCallback,
        session);
    if (connection == nullptr) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }

    if (disable1RttEncryption &&
        !TqSetDisable1RttEncryption(connection, "server")) {
        connection->Handle = nullptr;
        delete connection;
        return QUIC_STATUS_INVALID_STATE;
    }

    {
        std::lock_guard<std::mutex> guard(session->Lock);
        if (session->Stopping || !session->Configuration ||
            QUIC_FAILED(connection->SetConfiguration(*session->Configuration))) {
            connection->Handle = nullptr;
            delete connection;
            return QUIC_STATUS_INVALID_STATE;
        }
    }

    QuicServerSession::ConnectionHandler handler;
    {
        std::lock_guard<std::mutex> guard(session->Lock);
        handler = session->PeerConnectionHandler;
    }
    if (handler) {
        handler(connection);
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicServerSession::ConnectionCallback(
    MsQuicConnection* connection,
    void* context,
    QUIC_CONNECTION_EVENT* event) noexcept {
    auto* session = static_cast<QuicServerSession*>(context);
    if (session == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_NETWORK_STATISTICS:
        if (TqTraceEnabled() || TqDiagStatsEnabled()) {
            TqTraceQuicNetworkStats(
                connection,
                TqTraceNetworkStats{
                    event->NETWORK_STATISTICS.BytesInFlight,
                    event->NETWORK_STATISTICS.PostedBytes,
                    event->NETWORK_STATISTICS.IdealBytes,
                    event->NETWORK_STATISTICS.SmoothedRTT,
                    event->NETWORK_STATISTICS.CongestionWindow,
                    event->NETWORK_STATISTICS.Bandwidth,
                    event->NETWORK_STATISTICS.BytesInFlightMax,
                    event->NETWORK_STATISTICS.BbrState,
                    event->NETWORK_STATISTICS.BbrRecoveryState,
                    event->NETWORK_STATISTICS.BbrRecoveryWindow,
                    event->NETWORK_STATISTICS.BbrPacingGain,
                    event->NETWORK_STATISTICS.BbrCwndGain,
                    event->NETWORK_STATISTICS.BbrMinRtt,
                    event->NETWORK_STATISTICS.BbrSendQuantum,
                    event->NETWORK_STATISTICS.BbrAppLimited != FALSE,
                    event->NETWORK_STATISTICS.SendFlushCount,
                    event->NETWORK_STATISTICS.SendFlushPacingDelayedCount,
                    event->NETWORK_STATISTICS.SendFlushCcBlockedCount,
                    event->NETWORK_STATISTICS.SendFlushSchedulingCount,
                    event->NETWORK_STATISTICS.SendFlushAmplificationBlockedCount,
                    event->NETWORK_STATISTICS.SendFlushNoWorkCount,
                    event->NETWORK_STATISTICS.SendFlushLastAllowance,
                    event->NETWORK_STATISTICS.SendFlushLastPathAllowance,
                    event->NETWORK_STATISTICS.SendFlushLastResult,
                    event->NETWORK_STATISTICS.SendFlushLastDatagrams,
                    event->NETWORK_STATISTICS.OutFlowBlockedReasons,
                    event->NETWORK_STATISTICS.LossDetectionEventCount,
                    event->NETWORK_STATISTICS.LossDetectionFackPacketCount,
                    event->NETWORK_STATISTICS.LossDetectionRackPacketCount,
                    event->NETWORK_STATISTICS.LostRetransmittableBytes,
                    event->NETWORK_STATISTICS.LastLostRetransmittableBytes});
        }
        break;
    case QUIC_CONNECTION_EVENT_CONNECTED:
        TqSampleConnectionRtt(connection);
        connection->SendResumptionTicket(QUIC_SEND_RESUMPTION_FLAG_FINAL);
        {
            const uint32_t connId = TqRegisterServerConnection(connection);
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC server connection accepted from=%s conn=%u",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                connId);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
            TqTraceQuicConnected(connection, connId, "server", 0);
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            QuicServerSession::StreamHandler handler;
            {
                std::lock_guard<std::mutex> guard(session->Lock);
                handler = session->PeerStreamHandler;
            }
            if (handler) {
                handler(connection, event->PEER_STREAM_STARTED.Stream);
            } else {
                MsQuic->StreamClose(event->PEER_STREAM_STARTED.Stream);
            }
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        (void)TqAbortConnectionTunnels(connection);
        if (TqTraceEnabled()) {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            TqTraceQuicShutdownTransport(
                connection,
                connId,
                "server",
                static_cast<uint32_t>(event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status),
                event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        (void)TqAbortConnectionTunnels(connection);
        if (TqTraceEnabled()) {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            TqTraceQuicShutdownPeer(
                connection,
                connId,
                "server",
                event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        (void)TqAbortConnectionTunnels(connection);
        {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            char line[256];
            std::snprintf(
                line,
                sizeof(line),
                "tcpquic-proxy: QUIC server connection disconnected from=%s conn=%u",
                TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
                connId);
            std::fprintf(stderr, "%s\n", line);
            TqTraceLogLine(line);
        }
        if (TqTraceEnabled()) {
            const uint32_t connId = TqLookupServerConnectionId(connection);
            TqTraceQuicDisconnected(connection, connId, "server");
        }
        TqUnregisterServerConnection(connection);
        break;
    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}
