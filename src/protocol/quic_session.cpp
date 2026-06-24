#include "quic_session.h"
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

struct TqEndpoint {
    std::string Host;
    uint16_t Port{0};
};

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

bool ParsePort(const std::string& value, uint16_t& port) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' ||
        parsed == 0 || parsed > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

bool ParseEndpoint(const std::string& value, TqEndpoint& endpoint) {
    if (value.empty()) {
        return false;
    }

    std::string host;
    std::string portText;
    if (value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos ||
            close + 1 >= value.size() ||
            value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portText = value.substr(close + 2);
    } else {
        const size_t colon = value.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
            return false;
        }
        host = value.substr(0, colon);
        portText = value.substr(colon + 1);
    }

    uint16_t port = 0;
    if (host.empty() || !ParsePort(portText, port)) {
        return false;
    }

    endpoint.Host = host;
    endpoint.Port = port;
    return true;
}

bool ConvertListenAddress(const TqEndpoint& endpoint, QUIC_ADDR& address) {
    if (endpoint.Host == "*") {
        std::memset(&address, 0, sizeof(address));
        QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(&address, endpoint.Port);
        return true;
    }
    return QuicAddrFromString(endpoint.Host.c_str(), endpoint.Port, &address);
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

static constexpr std::chrono::milliseconds TqClientStartRetryDelay{100};

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

    TqEndpoint endpoint;
    if (!ParseEndpoint(cfg.QuicPeer, endpoint)) {
        std::fprintf(stderr, "Invalid --quic-peer endpoint: %s\n", cfg.QuicPeer.c_str());
        return false;
    }

    Config = cfg;
    PeerHost = endpoint.Host;
    PeerPort = endpoint.Port;

    if (!InitApiAndRegistration(Api, Registration, TqToMsQuicProfile(cfg.QuicProfile)) ||
        !InitConfiguration(Config, *Registration, false, Configuration)) {
        Stop(false);
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            State->PeerStreamHandler = std::move(handler);
            State->ConnectionStateChanged = std::move(stateHandler);
            State->Scheduler = std::move(scheduler);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(State->Lock);
        State->Started = true;
        State->Stopping = false;
        State->Api = Api;
        State->SessionGate = std::make_shared<ClientSessionGate>();
        State->Slots.clear();
        State->Slots.resize(Config.QuicConnections);
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
    std::unique_ptr<MsQuicConfiguration> configurationLocal;
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
        std::lock_guard<std::mutex> guard(state->Lock);
        apiLocal = std::move(Api);
        registrationLocal = std::move(Registration);
        configurationLocal = std::move(Configuration);
    }
}

MsQuicConnection* QuicClientSession::GetConnection() {
    return PickConnection();
}

MsQuicConnection* QuicClientSession::PickConnection() {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (State->Slots.empty()) {
        return nullptr;
    }
    const size_t count = State->Slots.size();
    for (size_t attempt = 0; attempt < count; ++attempt) {
        const size_t index = PickIndex.fetch_add(1) % count;
        auto& slot = State->Slots[index];
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return slot.Connection.get();
        }
    }
    return nullptr;
}

MsQuicConnection* QuicClientSession::PickConnectionAt(size_t index) {
    std::lock_guard<std::mutex> guard(State->Lock);
    if (index >= State->Slots.size()) {
        return nullptr;
    }
    auto& slot = State->Slots[index];
    if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
        return slot.Connection.get();
    }
    return nullptr;
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
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return slot.Connection.get();
        }
    }
    return nullptr;
}

uint32_t QuicClientSession::ConnectionCount() const {
    return static_cast<uint32_t>(Config.QuicConnections);
}

uint32_t QuicClientSession::ConnectedConnectionCount() const {
    std::lock_guard<std::mutex> guard(State->Lock);
    return ConnectedCountLocked(*State);
}

std::vector<TqConnectionSnapshot> QuicClientSession::SnapshotConnections() const {
    std::vector<TqConnectionSnapshot> snapshots;
    std::lock_guard<std::mutex> guard(State->Lock);
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
        std::lock_guard<std::mutex> guard(State->Lock);
        if (!State->Started || State->Stopping) {
            err = "session is not running";
            return false;
        }
        oldSize = State->Slots.size();
        if (desired < oldSize) {
            err = "use StopHighestConnection to shrink";
            return false;
        }
        State->Slots.resize(desired);
        for (size_t i = oldSize; i < State->Slots.size(); ++i) {
            State->Slots[i].ConnectionId = MakeConnectionId(i);
        }
        Config.QuicConnections = desired;
    }

    for (size_t i = oldSize; i < desired; ++i) {
        (void)StartSlot(i);
    }
    return true;
}

bool QuicClientSession::StopHighestConnection(const std::string& connectionId, std::string& err) {
    size_t index = 0;
    if (!ParseConnectionId(connectionId, index)) {
        err = "invalid connection id";
        return false;
    }

    std::unique_ptr<MsQuicConnection> oldConnection;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
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

uint32_t QuicClientSession::ConnectedCountLocked(const ClientSharedState& state) {
    uint32_t count = 0;
    for (const auto& slot : state.Slots) {
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
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
    std::shared_ptr<ClientSessionGate> gate;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
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

void QuicClientSession::ScheduleStartRetryForTest(size_t index) {
    ScheduleStartRetry(index);
}

bool QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS status) {
    return TqConnectionStartAccepted(status);
}
#endif

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
    const size_t count = Config.QuicConnections;
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
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        startSlotOverride = State->TestHooks.StartSlotOverride;
    }
    if (startSlotOverride) {
        return startSlotOverride(index);
    }
#endif

    std::unique_ptr<MsQuicConnection> oldConnection;
    std::shared_ptr<ClientSharedState> state = State;

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            return false;
        }

        auto& slot = state->Slots[index];
        if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
            return true;
        }
        oldConnection = std::move(slot.Connection);
        slot.Context = nullptr;
        slot.Connected = false;
        slot.RetryScheduled = false;
        TqClientDebugLog("slot-reset", index, oldConnection.get());
    }

    if (oldConnection) {
        TqClientDebugLog("old-shutdown", index, oldConnection.get());
        oldConnection->Shutdown(0, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        std::lock_guard<std::mutex> guard(state->Lock);
        state->OrphanedConnections.push_back(std::move(oldConnection));
    }

    std::unique_ptr<MsQuicConnection> newConnection;
    MsQuicConnection* connectionToStart = nullptr;
    ClientConnContext* newContext = nullptr;
    std::shared_ptr<ClientSessionGate> gate;
    bool scheduleRetry = false;

        if (TqRuntimeTuningEnabled(Config)) {
        TqApplyRuntimeObservations(Config);
        if (!InitConfiguration(Config, *Registration, false, Configuration)) {
            std::fprintf(stderr, "Configuration refresh failed\n");
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                if (index < state->Slots.size()) {
                    state->Slots[index].LastError = "configuration refresh failed";
                }
            }
            ScheduleStartRetry(index);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            return false;
        }
        gate = state->SessionGate;
    }

    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            return false;
        }
        generation = state->Slots[index].Generation;
    }

    newContext = new (std::nothrow) ClientConnContext{gate, state, index, generation};
    if (newContext == nullptr) {
        {
            std::lock_guard<std::mutex> guard(state->Lock);
            if (index < state->Slots.size()) {
                state->Slots[index].LastError = "connection context allocation failed";
            }
        }
        scheduleRetry = true;
    }

    if (!scheduleRetry) {
        newConnection = std::make_unique<MsQuicConnection>(
                *Registration,
                CleanUpManual,
                QuicClientSession::ConnectionCallback,
                newContext);
        if (!newConnection || !newConnection->IsValid()) {
            const QUIC_STATUS initStatus =
                newConnection ? newConnection->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY;
            std::fprintf(stderr, "ConnectionOpen failed, 0x%x\n",
                initStatus);
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                if (index < state->Slots.size()) {
                    state->Slots[index].LastError = TqQuicStatusText("ConnectionOpen", initStatus);
                }
            }
            delete newContext;
            newContext = nullptr;
            newConnection.reset();
            scheduleRetry = true;
        }
    }

    if (!scheduleRetry && Config.QuicDisable1RttEncryption &&
        !TqSetDisable1RttEncryption(newConnection.get(), "client")) {
        {
            std::lock_guard<std::mutex> guard(state->Lock);
            if (index < state->Slots.size()) {
                state->Slots[index].LastError = "failed to disable 1-RTT encryption";
            }
        }
        delete newContext;
        newContext = nullptr;
        newConnection.reset();
        scheduleRetry = true;
    }

    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || index >= state->Slots.size()) {
            delete newContext;
            return false;
        }

        auto& slot = state->Slots[index];
        if (!scheduleRetry) {
            slot.Connection = std::move(newConnection);
            slot.Context = newContext;
            connectionToStart = slot.Connection.get();
            TqClientDebugLog("connection-opened", index, connectionToStart,
                slot.Connection->GetInitStatus());
        }
    }
    if (scheduleRetry) {
        ScheduleStartRetry(index);
        return false;
    }

    TqClientDebugLog("connection-start-before", index, connectionToStart);
    const QUIC_STATUS startStatus =
        connectionToStart->Start(*Configuration, PeerHost.c_str(), PeerPort);
    TqClientDebugLog("connection-start-after", index, connectionToStart, startStatus);

    if (TqTraceEnabled()) {
        const std::string peer = PeerHost + ":" + std::to_string(PeerPort);
        TqTraceQuicConnecting("client", static_cast<uint32_t>(index + 1), peer.c_str());
    }

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
            delete newContext;
            slot.Connection.reset();
            scheduleRetry = true;
        }
    }
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
    (void)StartSlot(slotIndex);
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

    TqEndpoint endpoint;
    if (!ParseEndpoint(cfg.QuicListen, endpoint)) {
        std::fprintf(stderr, "Invalid --quic-listen endpoint: %s\n", cfg.QuicListen.c_str());
        return false;
    }

    Config = cfg;
    if (!InitApiAndRegistration(Api, Registration, TqToMsQuicProfile(cfg.QuicProfile)) ||
        !InitConfiguration(Config, *Registration, true, Configuration)) {
        Stop();
        return false;
    }

    QUIC_ADDR address{};
    if (!ConvertListenAddress(endpoint, address)) {
        std::fprintf(stderr, "Invalid --quic-listen address: %s\n", cfg.QuicListen.c_str());
        Stop();
        return false;
    }

    Listener = std::make_unique<MsQuicListener>(
        *Registration,
        CleanUpManual,
        QuicServerSession::ListenerCallback,
        this);
    if (!Listener || !Listener->IsValid()) {
        std::fprintf(stderr, "ListenerOpen failed, 0x%x\n",
            Listener ? Listener->GetInitStatus() : QUIC_STATUS_OUT_OF_MEMORY);
        Stop();
        return false;
    }

    MsQuicAlpn alpn(TqAlpn);
    const QUIC_STATUS status = Listener->Start(alpn, &address);
    if (QUIC_FAILED(status)) {
        std::fprintf(stderr, "ListenerStart failed, 0x%x\n", status);
        Stop();
        return false;
    }

    std::lock_guard<std::mutex> guard(Lock);
    Started = true;
    Stopping = false;
    return true;
}

void QuicServerSession::Stop() {
    std::shared_ptr<MsQuicApi> apiLocal;
    std::unique_ptr<MsQuicRegistration> registrationLocal;
    std::unique_ptr<MsQuicConfiguration> configurationLocal;
    std::unique_ptr<MsQuicListener> listenerLocal;

    {
        std::lock_guard<std::mutex> guard(Lock);
        Started = false;
        Stopping = true;
        if (Listener && Listener->Handle) {
            MsQuic->ListenerStop(Listener->Handle);
        }
        apiLocal = std::move(Api);
        registrationLocal = std::move(Registration);
        configurationLocal = std::move(Configuration);
        listenerLocal = std::move(Listener);
    }

    StateChanged.notify_all();
}

void QuicServerSession::Run() {
    std::unique_lock<std::mutex> guard(Lock);
    StateChanged.wait(guard, [this] { return !Started || Stopping; });
    guard.unlock();
    Stop();
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

    TqTraceQuicIncoming();

    auto* connection = new (std::nothrow) MsQuicConnection(
        event->NEW_CONNECTION.Connection,
        CleanUpAutoDelete,
        QuicServerSession::ConnectionCallback,
        session);
    if (connection == nullptr) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }

    if (session->Config.QuicDisable1RttEncryption &&
        !TqSetDisable1RttEncryption(connection, "server")) {
        connection->Handle = nullptr;
        delete connection;
        return QUIC_STATUS_INVALID_STATE;
    }

    if (QUIC_FAILED(connection->SetConfiguration(*session->Configuration))) {
        connection->Handle = nullptr;
        delete connection;
        return QUIC_STATUS_INVALID_STATE;
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
